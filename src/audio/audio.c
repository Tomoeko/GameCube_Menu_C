#include "audio_internal.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

GcAudio *gc_audio_create(const char *ipl_path, unsigned sample_rate) {
    if (!ipl_path || sample_rate < 8000 || sample_rate > 192000)
        return NULL;
    GcAudio *audio = calloc(1, sizeof(*audio));
    if (!audio)
        return NULL;
    if (!gc_audio_resources_load(audio, ipl_path)) {
        gc_audio_destroy(audio);
        return NULL;
    }
    audio->sample_rate = sample_rate;
    audio->output_resampler = cc_audio_resampler_create(64057, 2, sample_rate);
    audio->output_state = cc_audio_resample_state_create(
        cc_audio_resampler_taps(audio->output_resampler));
    if (!audio->output_resampler || !audio->output_state) {
        gc_audio_destroy(audio);
        return NULL;
    }
    atomic_init(&audio->event_read, 0);
    atomic_init(&audio->event_write, 0);
    atomic_init(&audio->mono, false);
    atomic_init(&audio->dropped_events, 0);
    gc_audio_reset(audio);
    return audio;
}

void gc_audio_destroy(GcAudio *audio) {
    if (!audio)
        return;
    gc_audio_device_stop(audio);
    gc_audio_capture_end(audio);
    gc_audio_resources_release(audio);
    cc_audio_resample_state_destroy(audio->output_state);
    cc_audio_resampler_destroy(audio->output_resampler);
    free(audio);
}

void gc_audio_reset(GcAudio *audio) {
    if (!audio)
        return;
    gc_audio_device_stop(audio);
    memset(audio->tracks, 0, sizeof(audio->tracks));
    memset(audio->voices, 0, sizeof(audio->voices));
    memset(audio->pending_events, 0, sizeof(audio->pending_events));
    cc_audio_resample_state_reset(audio->output_state);
    memset(audio->surround_delay, 0, sizeof(audio->surround_delay));
    memset(audio->chorus, 0, sizeof(audio->chorus));
    for (unsigned index = 0; index < 4; ++index) {
        GcAudioEffect *effect = &audio->effects[index];
        memset(effect->history, 0, sizeof(effect->history));
        memset(effect->delay, 0, sizeof(effect->delay));
        effect->position = 0;
    }
    audio->tick_fraction = 0;
    audio->update_samples = 0;
    audio->chorus_read = audio->sequence_revision ? 150u * 65536u : 0;
    audio->chorus_direction = audio->sequence_revision ? -1 : 0;
    audio->chorus_write = audio->chorus_frame = audio->dsp_frame = 0;
    audio->native_counter = 0;
    audio->sequence_ticks = audio->notes_started = 0;
    audio->rejected_commands = 0;
    atomic_store_explicit(&audio->event_read, 0, memory_order_relaxed);
    atomic_store_explicit(&audio->event_write, 0, memory_order_relaxed);
    atomic_store_explicit(&audio->dropped_events, 0, memory_order_relaxed);
    gc_audio_sequence_init(audio);
}
static bool queue_event(GcAudio *audio, unsigned event, float value) {
    if (!audio || event > UINT16_MAX)
        return false;
    unsigned write = atomic_load_explicit(&audio->event_write, memory_order_relaxed);
    unsigned read = atomic_load_explicit(&audio->event_read, memory_order_acquire);
    unsigned next = (write + 1) % GC_AUDIO_EVENT_QUEUE;
    if (next == read) {
        atomic_fetch_add_explicit(&audio->dropped_events, 1, memory_order_relaxed);
        return false;
    }
    audio->pending_events[write] = (GcAudioEvent){(uint16_t)event, value};
    atomic_store_explicit(&audio->event_write, next, memory_order_release);
    return true;
}

bool gc_audio_event(GcAudio *audio, unsigned native_event) {
    return native_event < 0x100 && queue_event(audio, native_event, 0);
}

bool gc_audio_startup_sound(GcAudio *audio, unsigned selector) {
    return selector < 3 && queue_event(audio, selector, 0);
}

bool gc_audio_cube_motion(GcAudio *audio, unsigned direction, float fraction) {
    return direction < 2 && isfinite(fraction) && fraction >= 0 && fraction <= 1 &&
           queue_event(audio, 0x100 + direction, fraction);
}

bool gc_audio_menu_begin(GcAudio *audio) {
    return queue_event(audio, 0x102, 0);
}

bool gc_audio_menu_end(GcAudio *audio) {
    return queue_event(audio, 0x103, 0);
}

bool gc_audio_stop_sequence(GcAudio *audio) {
    return queue_event(audio, 0x104, 0);
}

unsigned gc_audio_sample_rate(const GcAudio *audio) {
    return audio ? audio->sample_rate : 0;
}

unsigned gc_audio_active_voices(const GcAudio *audio) {
    return audio ? atomic_load_explicit(&audio->active_voices, memory_order_acquire)
                 : 0;
}

bool gc_audio_sequence_stopped(const GcAudio *audio) {
    return audio &&
           atomic_load_explicit(&audio->sequence_stopped, memory_order_acquire);
}

void gc_audio_set_mono(GcAudio *audio, bool mono) {
    if (audio)
        atomic_store_explicit(&audio->mono, mono, memory_order_relaxed);
}

typedef struct {
    GcAudio *audio;
    bool mono;
} NativeOutput;

static bool native_output_frame(void *context, float stereo[2]) {
    NativeOutput *output = context;
    gc_audio_dsp_render_frame(output->audio, output->mono, stereo);
    return true;
}

void gc_audio_render(GcAudio *audio, float *stereo, size_t frames) {
    if (!stereo || frames > SIZE_MAX / (2 * sizeof(*stereo)))
        return;
    memset(stereo, 0, frames * 2 * sizeof(*stereo));
    if (!audio || !frames)
        return;
    NativeOutput output = {audio,
                           atomic_load_explicit(&audio->mono, memory_order_relaxed)};
    /* Preserve the recovered 64057/2 DAC clock and fixed-point DSP buses.
     * Only host reconstruction changes. Its causal delay (~3 ms at 48 kHz)
     * is identical in device output and the captured recording stream.
     */
    cc_audio_resampler_render(audio->output_resampler, audio->output_state,
                              native_output_frame, &output, stereo, frames);
    unsigned active = 0;
    for (unsigned voice = 0; voice < GC_AUDIO_VOICES; ++voice)
        active += audio->voices[voice].active ? 1u : 0u;
    atomic_store_explicit(&audio->active_voices, active, memory_order_release);
    gc_audio_capture_output(audio, stereo, frames);
}

void gc_audio_info(const GcAudio *audio, GcAudioInfo *info) {
    if (!audio || !info)
        return;
    *info = (GcAudioInfo){.wave_count = audio->wave_count,
                          .instrument_count = audio->instrument_count,
                          .sequence_ticks = audio->sequence_ticks,
                          .notes_started = audio->notes_started,
                          .rejected_commands = audio->rejected_commands,
                          .dropped_events = atomic_load_explicit(&audio->dropped_events,
                                                                 memory_order_relaxed),
                          .active_voices = gc_audio_active_voices(audio),
                          .sequence_stopped = gc_audio_sequence_stopped(audio)};
}
