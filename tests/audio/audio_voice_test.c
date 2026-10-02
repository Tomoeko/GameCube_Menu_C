#include "audio/audio_internal.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static GcAudio *empty_audio(void) {
    GcAudio *audio = calloc(1, sizeof(*audio));
    assert(audio);
    atomic_init(&audio->mono, false);
    atomic_init(&audio->event_read, 0);
    atomic_init(&audio->event_write, 0);
    atomic_init(&audio->sequence_stopped, false);
    audio->tracks[0].parent = GC_AUDIO_TRACKS;
    audio->tracks[0].parameters[0] = 1;
    audio->tracks[0].routes[0] = 0x10;
    audio->master_gain = 65535;
    audio->output_gain = 4096;
    return audio;
}

static GcAudio *note_audio(unsigned revision, bool attack) {
    /* An authored single-note sequence drives the real sequence dispatcher. */
    static uint8_t sequence[] = {60, 1, 127, 0x80, 255};
    static int16_t samples[32] = {1000};
    GcAudio *audio = empty_audio();
    audio->sequence_revision = revision;
    audio->tempo = revision ? 526 : 525;
    audio->timebase = 48;
    audio->master_gain = attack ? 32767 : 16384;
    audio->sequence = sequence;
    audio->sequence_size = sizeof(sequence);
    audio->wave_count = 1;
    audio->waves[0] = (GcAudioWave){.samples = samples,
                                    .count = 32,
                                    .key = 60,
                                    .rate = 32000,
                                    .loop = true,
                                    .loop_end = 32};
    audio->semitone_ratios[60] = 1;
    audio->fractional_semitone_ratios[0] = 1;
    GcAudioInstrument *instrument = &audio->instruments[0];
    instrument->region_count = 1;
    instrument->regions[0] =
        (GcAudioRegion){.key = 127, .velocity = 127, .volume = 1, .pitch = 1};
    instrument->volume = attack ? 1 : 1459.0f / 16384;
    instrument->pitch = 1;
    if (attack) {
        instrument->envelopes[0] = (GcAudioEnvelope){
            .enabled = true,
            .rate = 1,
            .scale = 1,
            .attack_count = 2,
            .attack = {{0, 16384, 32767}, {14, 0, 0}},
        };
    }
    GcAudioTrack *track = &audio->tracks[0];
    track->active = true;
    if (revision) {
        track->envelope_modes[0] = 15;
        track->envelope_modes[1] = 15;
    }
    for (unsigned child = 0; child < 16; ++child)
        track->children[child] = GC_AUDIO_TRACKS;
    track->registers[9] = 1;
    track->routes[0] = revision ? 0x100 : 0x10;
    track->timebase = 48;
    return audio;
}

static void test_initial_gain_block(unsigned revision, bool attack) {
    GcAudio *audio = note_audio(revision, attack);
    float output[2];
    for (unsigned frame = 0; frame < 80; ++frame) {
        gc_audio_dsp_render_frame(audio, false, output);
        assert(audio->notes_started == 0);
    }
    gc_audio_dsp_render_frame(audio, false, output);
    assert(audio->notes_started == 1 && audio->sequence_ticks == 1);
    GcAudioVoice *voice = &audio->voices[0];
    GcAudioDspGain *gain = &voice->dsp_gains[0];
    assert(gain->initialized && gain->frame == 1);
    assert(gain->current == (attack ? 0 : 1459));
    assert(gain->product_multiplier == (attack ? 0 : 1459));
    int32_t increment = revision ? 4096 : 2048;
    assert(gain->increment == (attack ? increment : 0));
    if (attack) {
        float target =
            gc_audio_bus_gain(audio, gc_audio_route_gain(audio, voice, 0, false));
        assert(target * 65536 == (revision ? 2 : 1));
    }
    /* Initial setup stores oscillator value zero. The same update advances
     * its target to one in USA and two in EUR, where table tick durations
     * use the DAC clock. USA ramps forty pairs, EUR thirty-two then holds.
     */
    for (unsigned frame = 0; frame < 80; ++frame) {
        if (frame)
            gc_audio_dsp_render_frame(audio, false, output);
        unsigned pairs = (frame + 1) / 2;
        if (revision && pairs > 32)
            pairs = 32;
        int64_t expected = attack ? (int64_t)pairs * increment : INT64_C(1459) * 65536;
        assert(audio->sequence_ticks == 1 && gain->frame == frame + 1);
        assert(gain->accumulator == expected);
    }
    assert(gain->current == (attack ? (revision ? 2 : 1) : 1459) &&
           gain->increment == 0);
    free(audio);
}

static void test_retirement_order(unsigned revision) {
    GcAudio *audio = note_audio(revision, false);
    audio->update_samples = 80;
    audio->voices[0] = (GcAudioVoice){.active = true,
                                      .track = 2,
                                      .slot = 7,
                                      .duration = UINT32_MAX,
                                      .end_pending = true,
                                      .envelope_pitch = 1};
    audio->voices[1] = (GcAudioVoice){.active = true,
                                      .track = 1,
                                      .slot = 7,
                                      .duration = UINT32_MAX,
                                      .envelope_pitch = 1};
    float output[2];
    gc_audio_dsp_render_frame(audio, false, output);
    assert(audio->notes_started == 1);
    assert(!audio->voices[0].active && audio->voices[1].active);
    assert(audio->voices[2].active && audio->voices[2].track == 0 &&
           audio->voices[2].slot == 1);
    free(audio);
}

static void test_regional_note_setup(unsigned revision) {
    static uint8_t sequence[] = {60, 1, 64, 0x80, 255};
    GcAudio *audio = note_audio(revision, false);
    audio->sequence = sequence;
    GcAudioInstrument *instrument = &audio->instruments[0];
    instrument->volume = 0.5f;
    instrument->regions[0].volume = 0.25f;
    instrument->pitch = 1.2f;
    instrument->regions[0].pitch = 0.8f;
    float output[2];
    for (unsigned frame = 0; frame <= 80; ++frame)
        gc_audio_dsp_render_frame(audio, false, output);
    assert(audio->notes_started == 1);
    const GcAudioVoice *voice = &audio->voices[0];
    /* These independently evaluated register results distinguish USA's
     * linear velocity and double pitch chain from EUR's squared velocity
     * and separately rounded single precision pitch products.
     */
    float gain = revision ? 0x1.040c20p-5f : 0x1.020408p-4f;
    float pitch = revision ? 0x1.eb152ap-1f : 0x1.eb1528p-1f;
    assert(voice->active && voice->base_gain == gain && voice->track_gain == 1);
    assert(voice->base_step == pitch && voice->step == pitch);
    assert(voice->dsp_gains[0].current == (revision ? 520 : 1032));
    free(audio);
}

static void test_mono_snapshot(unsigned revision) {
    GcAudio *audio = note_audio(revision, false);
    GcAudioTrack *track = &audio->tracks[0];
    track->registers[9] = 0;
    track->registers[10] = 1;
    track->parameters[3] = 0;
    track->routes[0] = revision ? 0x150 : 0x14;
    gc_audio_route_table_init(audio);
    atomic_store_explicit(&audio->mono, true, memory_order_relaxed);
    float output[2];
    for (unsigned frame = 0; frame <= 80; ++frame)
        gc_audio_dsp_render_frame(audio, false, output);
    assert(audio->notes_started == 1);
    const GcAudioDspGain *gain = &audio->voices[0].dsp_gains[0];
    /* A host callback retains its stereo snapshot when the UI changes mono.
     * New-note setup must use that same snapshot for its initial multiplier.
     */
    assert(atomic_load_explicit(&audio->mono, memory_order_relaxed));
    assert(gain->current == 1459 && gain->increment == 0);
    free(audio);
}

static void test_initial_oscillator_composition(unsigned revision) {
    static uint8_t sequence[] = {0xf0, 0x00, 60, 1, 127, 0x80, 255};
    GcAudio *audio = note_audio(revision, false);
    if (revision) {
        audio->sequence = sequence;
        audio->sequence_size = sizeof(sequence);
    }
    audio->master_gain = 32767;
    GcAudioEnvelope instrument_envelope = {
        .enabled = true,
        .rate = 1,
        .scale = 1,
        .attack_count = 2,
        .attack = {{0, 0, 16384}, {14, 0, 0}},
    };
    GcAudioEnvelope track_envelope = instrument_envelope;
    track_envelope.attack[0].value = 8192;
    audio->instruments[0].volume = 1;
    audio->instruments[0].envelopes[0] = instrument_envelope;
    audio->tracks[0].envelopes[0] = track_envelope;
    float output[2];
    for (unsigned frame = 0; frame <= 80; ++frame)
        gc_audio_dsp_render_frame(audio, false, output);
    assert(audio->notes_started == 1);
    const GcAudioVoice *voice = &audio->voices[0];
    /* Both versions install the instrument before a track override. Initial
     * setup multiplies both values; the first callback evaluates the final
     * slot mapping after resetting its target accumulators.
     */
    if (revision)
        assert(audio->tracks[0].envelope_modes[0] == 0);
    assert(voice->envelope_tracks[0] == 0);
    assert(voice->envelope_volume == 0.25f);
    assert(voice->dsp_gains[0].current == 4095);
    assert(voice->dsp_gains[0].product_multiplier == 4095);
    assert(gc_audio_bus_gain(audio, gc_audio_route_gain(audio, voice, 0, false)) *
               65536 ==
           8191);
    free(audio);
}

static int16_t coefficient_product(int16_t value) {
    int64_t product = (int64_t)value * 32767;
    int64_t middle = product / 32768;
    int64_t remainder = product % 32768;
    if (remainder < 0)
        remainder = -remainder;
    if (remainder > 16384 || (remainder == 16384 && middle % 2))
        middle += product < 0 ? -1 : 1;
    return (int16_t)middle;
}

static int16_t source_output(const int16_t *samples, size_t endpoint,
                             uint64_t position_q12, unsigned pitch_q12) {
    uint64_t cursor = position_q12 / 4096;
    int16_t source = cursor >= 4 && cursor - 4 < endpoint ? samples[cursor - 4] : 0;
    int16_t resampled = pitch_q12 >= 16384 ? source : coefficient_product(source);
    int64_t product = (int64_t)resampled * 32767;
    return (int16_t)(product >= 0 ? product / 65536 : -((-product + 65535) / 65536));
}

static void test_source_end(size_t count, bool afc_source, uint64_t start_q12,
                            unsigned pitch_q12, unsigned active_blocks) {
    GcAudio *audio = empty_audio();
    int16_t *samples = calloc(count ? count : 1, sizeof(*samples));
    assert(samples);
    for (size_t index = 0; index < count; ++index)
        samples[index] = (int16_t)(1000 + index % 400);
    for (unsigned phase = 0; phase < 64; ++phase)
        audio->resampling_coefficients[phase][0] = 32767;
    audio->waves[0] =
        (GcAudioWave){.samples = samples, .count = count, .afc_source = afc_source};
    GcAudioVoice *voice = &audio->voices[0];
    *voice = (GcAudioVoice){.active = true,
                            .released = true,
                            .position = (double)start_q12 / 4096,
                            .step = (double)pitch_q12 / 4096,
                            .base_gain = 0.5f,
                            .track_gain = 1,
                            .envelope_volume = 1,
                            .envelope_pitch = 1,
                            .duration = UINT32_MAX};
    voice->buses[0] = 1;
    gc_audio_dsp_voice_begin(audio, voice, false);
    size_t endpoint = afc_source ? count / 16 * 16 : count;
    uint64_t position_q12 = start_q12;
    for (unsigned frame = 0; frame < (active_blocks + 1) * 80; ++frame) {
        float output[2];
        gc_audio_dsp_render_frame(audio, false, output);
        bool active = frame < active_blocks * 80;
        int16_t expected =
            active ? source_output(samples, endpoint, position_q12, pitch_q12) : 0;
        assert(voice->active == active);
        assert(output[0] == (float)expected / 32768);
        assert(output[1] == -1.0f / 32768);
        if (active)
            position_q12 += pitch_q12;
        assert(voice->position == (double)position_q12 / 4096);
    }
    free(samples);
    free(audio);
}

static void test_source_boundaries(void) {
    test_source_end(8, false, 0, 4096, 1);
    const size_t partial_counts[] = {8, 17, 31, 32, 33};
    for (unsigned index = 0; index < sizeof(partial_counts) / sizeof(*partial_counts);
         ++index)
        test_source_end(partial_counts[index], true, 0, 4096, 1);
    /* Exact consumption retains the FIR history until one following block
     * performs the exhausted-buffer fetch; it then retires the voice.
     */
    test_source_end(80, false, 0, 4096, 2);
    test_source_end(80, true, 0, 4096, 2);
    test_source_end(160, false, 0, 4096, 3);
    test_source_end(160, true, 0, 4096, 3);
    test_source_end(81, true, 0, 4096, 2);
    test_source_end(80, true, 0, 16384, 1);
    test_source_end(52049, true, UINT64_C(52044) * 4096 + 1776, 911, 1);
    test_source_end(52049, true, UINT64_C(52036) * 4096 + 1024, 1216, 1);
    test_source_end(32, true, UINT64_C(32) * 4096, 0, 1);
    test_source_end(0, false, 0, 0, 1);
}

static void test_block_pitch(void) {
    GcAudio *audio = empty_audio();
    int16_t samples[240] = {0};
    audio->waves[0] = (GcAudioWave){.samples = samples, .count = 240};
    GcAudioVoice *voice = &audio->voices[0];
    *voice = (GcAudioVoice){.active = true,
                            .released = true,
                            .step = 1,
                            .envelope_pitch = 1,
                            .duration = UINT32_MAX};
    for (unsigned frame = 0; frame < 160; ++frame) {
        if (frame == 10)
            voice->step = 0.5;
        float output[2];
        gc_audio_dsp_render_frame(audio, false, output);
        double expected = frame < 80 ? frame + 1.0 : 80 + (frame - 79) * 0.5;
        assert(voice->position == expected);
    }
    free(audio);
}

int main(void) {
    for (unsigned revision = 0; revision < 2; ++revision) {
        test_initial_gain_block(revision, true);
        test_initial_gain_block(revision, false);
        test_retirement_order(revision);
        test_regional_note_setup(revision);
        test_mono_snapshot(revision);
        test_initial_oscillator_composition(revision);
    }
    test_source_boundaries();
    test_block_pitch();
    puts("audio voice tests passed");
    return 0;
}
