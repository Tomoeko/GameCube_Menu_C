#include "audio_internal.h"

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
    atomic_init(&audio->event_read, 0);
    atomic_init(&audio->event_write, 0);
    atomic_init(&audio->mono, false);
    atomic_init(&audio->dropped_events, 0);
    gc_audio_sequence_init(audio);
    return audio;
}

void gc_audio_destroy(GcAudio *audio) {
    if (!audio)
        return;
    gc_audio_device_stop(audio);
    gc_audio_resources_release(audio);
    free(audio);
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

void gc_audio_render(GcAudio *audio, float *stereo, size_t frames) {
    if (!stereo || frames > SIZE_MAX / (2 * sizeof(*stereo)))
        return;
    memset(stereo, 0, frames * 2 * sizeof(*stereo));
    if (!audio || !frames)
        return;
    bool mono = atomic_load_explicit(&audio->mono, memory_order_relaxed);
    if (!audio->output_ready) {
        gc_audio_dsp_render_frame(audio, mono, audio->output_previous);
        gc_audio_dsp_render_frame(audio, mono, audio->output_next);
        audio->output_ready = true;
    }
    for (size_t frame = 0; frame < frames; ++frame) {
        /* The firmware mixes at its DAC clock. This final linear conversion
         * adapts that stream to the host's requested rate, outside the DSP.
         */
        float fraction = (float)audio->output_fraction;
        for (unsigned channel = 0; channel < 2; ++channel)
            stereo[frame * 2 + channel] =
                audio->output_previous[channel] +
                (audio->output_next[channel] - audio->output_previous[channel]) *
                    fraction;
        audio->output_fraction += GC_AUDIO_DSP_RATE / audio->sample_rate;
        while (audio->output_fraction >= 1) {
            memcpy(audio->output_previous, audio->output_next,
                   sizeof(audio->output_previous));
            gc_audio_dsp_render_frame(audio, mono, audio->output_next);
            audio->output_fraction -= 1;
        }
    }
    unsigned active = 0;
    for (unsigned voice = 0; voice < GC_AUDIO_VOICES; ++voice)
        active += audio->voices[voice].active ? 1u : 0u;
    atomic_store_explicit(&audio->active_voices, active, memory_order_release);
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

#ifndef __APPLE__
bool gc_audio_device_start(GcAudio *audio) {
    (void)audio;
    return false;
}

void gc_audio_device_stop(GcAudio *audio) {
    (void)audio;
}
#endif
