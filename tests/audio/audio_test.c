#include "gamecube/audio.h"
#include "gamecube/ipl.h"
#include "audio/audio_internal.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void test_residuals_and_history(void) {
    const uint8_t residuals[9] = {0x00, 0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef};
    int16_t samples[16];
    int16_t history[2] = {0};
    assert(gc_audio_afc_decode(residuals, sizeof(residuals), samples, 16, history));
    for (unsigned i = 0; i < 16; ++i)
        assert(samples[i] == (int16_t)(i < 8 ? (int)i : (int)i - 16));
    assert(history[0] == -1 && history[1] == -2);
    const uint8_t hold[9] = {0x01};
    history[0] = 1234;
    history[1] = -200;
    assert(gc_audio_afc_decode(hold, sizeof(hold), samples, 16, history));
    for (unsigned i = 0; i < 16; ++i)
        assert(samples[i] == 1234);
}

static void test_clipping_and_bounds(void) {
    uint8_t frame[9];
    memset(frame, 0x77, sizeof(frame));
    frame[0] = 0xf0;
    int16_t samples[16];
    int16_t history[2] = {0};
    assert(gc_audio_afc_decode(frame, sizeof(frame), samples, 16, history));
    for (unsigned i = 0; i < 16; ++i)
        assert(samples[i] == INT16_MAX);
    memset(frame + 1, 0x88, 8);
    assert(gc_audio_afc_decode(frame, sizeof(frame), samples, 16, history));
    for (unsigned i = 0; i < 16; ++i)
        assert(samples[i] == INT16_MIN);
    assert(!gc_audio_afc_decode(frame, 8, samples, 16, history));
    assert(!gc_audio_afc_decode(frame, 9, samples, 15, history));
    assert(!gc_audio_afc_decode(NULL, 9, samples, 16, history));
    assert(!gc_audio_afc_decode(frame, 9, NULL, 16, history));
    assert(!gc_audio_afc_decode(frame, 9, samples, 16, NULL));
    float silence[8];
    gc_audio_render(NULL, silence, 4);
    for (unsigned i = 0; i < 8; ++i)
        assert(silence[i] == 0);
}

static void test_native_envelopes(void) {
    GcAudioEnvelope envelope = {.enabled = true,
                                .rate = 1,
                                .scale = 1,
                                .attack_count = 2,
                                .release_count = 2,
                                .attack = {{0, 4, 32767}, {14, 0, 0}},
                                .release = {{1, 4, 0}, {15, 0, 0}}};
    GcAudioEnvelopeState state;
    gc_audio_envelope_start(&state, &envelope);
    for (unsigned tick = 0; tick <= 4; ++tick) {
        float expected = (float)tick * (32767.0f / 32768.0f) / 4;
        assert(fabsf(gc_audio_envelope_step(&state) - expected) < 0.00001f);
    }
    float held = gc_audio_envelope_step(&state);
    assert(state.held && fabsf(held - 32767.0f / 32768.0f) < 0.00001f);
    gc_audio_envelope_release(&state);
    for (unsigned tick = 0; tick < 4; ++tick) {
        float expected = held * (float)(4 - tick) / 4;
        assert(fabsf(gc_audio_envelope_step(&state) - expected * expected) < 0.00001f);
    }
    assert(gc_audio_envelope_step(&state) == 0 && state.ended);
    envelope.attack_count = 4;
    envelope.attack[0] = (GcAudioEnvelopeStep){0, 0, 32767};
    envelope.attack[1] = (GcAudioEnvelopeStep){0, 2, -32768};
    envelope.attack[2] = (GcAudioEnvelopeStep){0, 2, 32767};
    envelope.attack[3] = (GcAudioEnvelopeStep){13, 0, 1};
    envelope.scale = 0.25f;
    envelope.offset = 1;
    gc_audio_envelope_start(&state, &envelope);
    const float cycle[5] = {1.24999237f, 0.999996185f, 0.75f, 0.999996185f,
                            1.24999237f};
    for (unsigned tick = 0; tick < 21; ++tick)
        assert(fabsf(gc_audio_envelope_step(&state) - cycle[tick % 4]) < 0.00001f);
    envelope.attack_count = 1;
    envelope.attack[0] = (GcAudioEnvelopeStep){13, 0, 0};
    gc_audio_envelope_start(&state, &envelope);
    assert(isfinite(gc_audio_envelope_step(&state)) && state.ended);
    /* Native FNMSUBS/FMADD single-rounding cases. Rounding the product
     * first would discard these exact 2^-26 residuals on another host.
     */
    envelope.rate = 0;
    envelope.scale = 1;
    envelope.offset = 0;
    state = (GcAudioEnvelopeState){.envelope = envelope,
                                   .remaining = 0x1.fffp-1f,
                                   .target = 1,
                                   .step = 0x1.0008p0f};
    assert(gc_audio_envelope_step(&state) == 0x1p-26f);
    envelope.scale = 0x1.0008p0f;
    envelope.offset = -1;
    state = (GcAudioEnvelopeState){
        .envelope = envelope, .remaining = 1, .target = 0x1.fffp-1f};
    assert(gc_audio_envelope_step(&state) == -0x1p-26f);
}

static void test_native_resampling(void) {
    GcAudio *audio = calloc(1, sizeof(*audio));
    assert(audio);
    int16_t samples[8] = {32767, -32768, 1000, -1000, 20, 40, 60, 80};
    GcAudioWave wave = {.samples = samples, .count = 8};
    audio->resampling_coefficients[0][0] = 32767;
    audio->resampling_coefficients[32][1] = 32767;
    assert(gc_audio_resample(audio, &wave, 0) == 0);
    assert(gc_audio_resample(audio, &wave, 4) == 32766);
    assert(gc_audio_resample(audio, &wave, 4.5) == -32767);
    assert(gc_audio_resample(audio, &wave, 4.499) == 0);
    assert(gc_audio_resample_pitch(audio, &wave, 4.5, 3.999755859375) == -32767);
    assert(gc_audio_resample_pitch(audio, &wave, 4.5, 4) == 32767);
    assert(gc_audio_resample_pitch(audio, &wave, 5.75, 4) == -32768);
    assert(gc_audio_resample_pitch(audio, &wave, 3.99, 4) == 0);
    audio->resampling_coefficients[0][0] = 16384;
    samples[0] = -1;
    assert(gc_audio_resample(audio, &wave, 4) == 0);
    samples[0] = -3;
    assert(gc_audio_resample(audio, &wave, 4) == -2);
    samples[0] = 3;
    assert(gc_audio_resample(audio, &wave, 4) == 2);
    for (unsigned tap = 0; tap < 4; ++tap) {
        audio->resampling_coefficients[0][tap] = 32767;
        samples[tap] = 32767;
    }
    assert(gc_audio_resample(audio, &wave, 4) == INT16_MAX);
    for (unsigned tap = 0; tap < 4; ++tap)
        samples[tap] = -32768;
    assert(gc_audio_resample(audio, &wave, 4) == INT16_MIN);
    memset(audio->resampling_coefficients[0], 0,
           sizeof(audio->resampling_coefficients[0]));
    audio->resampling_coefficients[0][3] = 32767;
    wave.loop = true;
    wave.loop_start = 4;
    wave.loop_end = 8;
    assert(gc_audio_resample(audio, &wave, 8) == 80);
    assert(gc_audio_resample(audio, &wave, 9) == 20);
    assert(gc_audio_resample(audio, &wave, 13) == 20);
    assert(gc_audio_resample_pitch(audio, &wave, 12.5, 4) == 20);
    assert(gc_audio_resample(audio, &wave, NAN) == 0);
    free(audio);
}

static void test_native_mixer_routes_and_delays(void) {
    GcAudio *audio = calloc(1, sizeof(*audio));
    assert(audio);
    GcAudioVoice voice = {.track = 0, .pan = 0, .reverb = 0.5f};
    audio->tracks[0].routes[0] = 0x14;
    audio->tracks[0].routes[1] = 0x24;
    audio->tracks[0].routes[2] = 0x36;
    voice.buses[0] = 1;
    voice.buses[1] = 2;
    voice.buses[2] = 3;
    assert(gc_audio_route_scale(audio, &voice, 0, false) == 1);
    assert(gc_audio_route_scale(audio, &voice, 1, false) == 0);
    assert(fabsf(gc_audio_route_scale(audio, &voice, 2, false) - 0.7071067811865475f) <
           0.00001f);
    assert(fabsf(gc_audio_route_scale(audio, &voice, 0, true) -
                 gc_audio_route_scale(audio, &voice, 1, true)) < 0.00001f);
    audio->sequence_revision = 1;
    voice.pan = 0.25f;
    audio->tracks[0].routes[0] = 0x150;
    audio->tracks[0].routes[1] = 0x210;
    audio->tracks[0].routes[2] = 0x352;
    assert(fabsf(gc_audio_route_scale(audio, &voice, 0, false) -
                 sinf(0.75f * 1.5707963267948966f)) < 0.00001f);
    assert(fabsf(gc_audio_route_scale(audio, &voice, 1, false) -
                 sinf(0.25f * 1.5707963267948966f)) < 0.00001f);
    assert(fabsf(gc_audio_route_scale(audio, &voice, 2, false) -
                 sinf(0.75f * 1.5707963267948966f) * 0.7071067811865475f) < 0.00001f);
    audio->sequence_revision = 0;
    GcAudioEffect *feedback = &audio->effects[0];
    feedback->mode = 1;
    feedback->length = 4;
    feedback->return_bus[0] = 1;
    feedback->return_gain[0] = INT16_MAX;
    feedback->filter[7] = 16384;
    GcAudioEffect *echo = &audio->effects[1];
    echo->mode = 2;
    echo->length = 3;
    echo->return_bus[0] = 2;
    echo->return_gain[0] = INT16_MAX;
    for (unsigned frame = 0; frame <= 10; ++frame) {
        int16_t buses[12] = {0};
        gc_audio_effects_begin(audio, buses);
        if (!frame)
            buses[3] = buses[4] = 16000;
        gc_audio_effects_end(audio, buses);
        int16_t left = frame == 5 ? 8000 : frame == 10 ? 4000 : 0;
        assert(buses[1] == left);
        assert(buses[2] == (frame == 3 ? 15999 : -1));
    }
    memset(audio, 0, sizeof(*audio));
    audio->sequence_revision = 1;
    audio->chorus_read = 150u * 65536u;
    audio->chorus_direction = -1;
    unsigned heard = 0;
    for (unsigned frame = 0; frame < 400; ++frame) {
        int16_t buses[12] = {0};
        gc_audio_effects_begin(audio, buses);
        if (!frame)
            buses[10] = 16000;
        gc_audio_effects_end(audio, buses);
        assert(buses[1] == buses[2] + 1);
        if (buses[1] != 0) {
            assert(frame >= 10 && frame <= 12);
            assert(buses[1] == 8000);
            ++heard;
        }
    }
    assert(heard == 1);
    free(audio);
}

static void test_native_master_and_output(void) {
    GcAudio *audio = calloc(1, sizeof(*audio));
    assert(audio);
    audio->master_gain = 13107;
    audio->output_gain = 16384;
    assert(gc_audio_bus_gain(audio, 1) == 13107.0f / 65536);
    assert(gc_audio_bus_gain(audio, 0.5f) == 6553.0f / 65536);
    assert(gc_audio_bus_gain(audio, -0.1f) == 0);
    assert(gc_audio_bus_gain(audio, 1.2f) == gc_audio_bus_gain(audio, 1));
    assert(gc_audio_bus_gain(audio, NAN) == 0);
    assert(gc_audio_dsp_output(audio, 4096) == 16384);
    assert(gc_audio_dsp_output(audio, -4096) == -16384);
    assert(gc_audio_dsp_output(audio, 13107) == INT16_MAX);
    assert(gc_audio_dsp_output(audio, -13107) == INT16_MIN);
    audio->output_gain = 4097;
    assert(gc_audio_dsp_output(audio, 1) == 1);
    assert(gc_audio_dsp_output(audio, -1) == -2);
    free(audio);
}

static void test_native_dsp_gain_ramps(void) {
    assert(gc_audio_dsp_shift(-1, 16) == -1);
    assert(gc_audio_dsp_round(16384, 15) == 0);
    assert(gc_audio_dsp_round(49152, 15) == 2);
    assert(gc_audio_dsp_round(-16384, 15) == 0);
    assert(gc_audio_dsp_round(-49152, 15) == -2);
    assert(gc_audio_dsp_wrap(32768) == INT16_MIN);
    assert(gc_audio_dsp_wrap(-32769) == INT16_MAX);
    for (unsigned revised = 0; revised < 2; ++revised) {
        GcAudioDspGain gain = {0};
        gc_audio_dsp_gain_prepare(&gain, 0, revised != 0);
        gc_audio_dsp_gain_prepare(&gain, 4096, revised != 0);
        assert(gain.increment == (revised ? 8388608 : 6710272));
        for (unsigned sample = 0; sample < 80; ++sample) {
            int16_t mixed = gc_audio_dsp_gain_mix(&gain, INT16_MIN, 0, revised != 0);
            /* Trace of 0x011e: MULCAC pipelines the next multiplication
             * before ADDAX advances by 6553/64 (USA) or 128 (EUR).
             */
            unsigned pair = sample ? (sample - 1) / 2 : 0;
            int64_t multiplier = revised ? (int64_t)(pair < 32 ? pair : 32) * 128
                                         : (int64_t)pair * 6553 / 64;
            assert(mixed == -(int16_t)((multiplier + 1) / 2));
        }
        assert(gain.current == (revised ? 4096 : 4095));
        assert(gain.increment == 0 && gain.frame == 80);
        gc_audio_dsp_gain_prepare(&gain, 0, revised != 0);
        assert(gain.increment == (revised ? -8388608 : -6709248));
        for (unsigned sample = 0; sample < 80; ++sample)
            (void)gc_audio_dsp_gain_mix(&gain, INT16_MIN, 0, revised != 0);
        assert(gain.current == 0);
    }
    GcAudioDspGain gain = {0};
    gc_audio_dsp_gain_prepare(&gain, 32767, false);
    assert(gc_audio_dsp_gain_mix(&gain, INT16_MAX, 30000, false) == INT16_MAX);
    assert(gc_audio_dsp_gain_mix(&gain, INT16_MIN, -30000, false) == INT16_MIN);
    gain = (GcAudioDspGain){0};
    gc_audio_dsp_gain_prepare(&gain, 1, false);
    assert(gc_audio_dsp_gain_mix(&gain, -1, 0, false) == -1);
    gc_audio_dsp_gain_prepare(&gain, 2, false);
    assert(gain.increment == 2048);
}

static void test_native_dsp_effect_trace(void) {
    GcAudio *audio = calloc(1, sizeof(*audio));
    assert(audio);
    GcAudioEffect *effect = &audio->effects[0];
    effect->mode = 1;
    effect->length = 16;
    effect->return_bus[0] = 1;
    effect->return_gain[0] = 0x2fff;
    /* Native USA descriptor at ROM 0x5ec48. Expected middle words below
     * trace M2 eight-tap MADD/MOVPZ (0x01bc), MULMVZ/ADDR (0x0342),
     * SET40 voice additions, and the 0x0402 feedback write. The short
     * ring exposes two feedback generations without a private ROM fixture.
     */
    const int16_t coefficients[8] = {0, 0, 0, 16, 512, 2048, 4369, 13055};
    memcpy(effect->filter, coefficients, sizeof(coefficients));
    const int16_t returned[40] = {
        0,   0,   0,  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,    0,   0,   4895, -206, 151,
        -97, -66, -2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1950, 571, 339, 45,   -31,  -13};
    const int16_t stored[40] = {
        32767, -12345, 3, 0,     0,    0,   0,    0,    0,   0,   0,   0,  0, 0,
        0,     0,      0, 13055, -549, 403, -259, -177, -6,  0,   0,   0,  0, 0,
        0,     0,      0, 0,     0,    0,   5201, 1522, 903, 120, -82, -36};
    for (unsigned frame = 0; frame < 40; ++frame) {
        int16_t buses[12] = {0};
        gc_audio_effects_begin(audio, buses);
        assert(buses[1] == returned[frame]);
        assert(buses[2] == -1);
        int16_t input = frame == 0 ? 32767 : frame == 1 ? -12345 : frame == 2 ? 3 : 0;
        buses[3] = gc_audio_dsp_saturate((int64_t)buses[3] + input);
        unsigned position = effect->position;
        gc_audio_effects_end(audio, buses);
        assert(effect->delay[position] == stored[frame]);
    }
    memset(audio, 0, sizeof(*audio));
    for (unsigned frame = 0; frame < 161; ++frame) {
        int16_t buses[12] = {0};
        gc_audio_effects_begin(audio, buses);
        assert(buses[1] == (frame == 80 ? 12345 : 0));
        assert(buses[2] == (frame == 80 ? -12346 : -1));
        if (frame == 0)
            buses[8] = 12345;
        gc_audio_effects_end(audio, buses);
    }
    memset(audio, 0, sizeof(*audio));
    effect = &audio->effects[0];
    effect->mode = 1;
    effect->length = 16;
    effect->filter[0] = effect->filter[1] = INT16_MAX;
    effect->history[0] = effect->history[1] = INT16_MAX;
    effect->return_bus[0] = 1;
    effect->return_gain[0] = INT16_MAX;
    int16_t buses[12] = {0};
    gc_audio_effects_begin(audio, buses);
    assert(buses[3] == -4 && buses[1] == -4);
    /* The return already occupies the bus before the saturating voice
     * add; processing effects after all voices would give a different word.
     */
    GcAudioDspGain gain = {0};
    gc_audio_dsp_gain_prepare(&gain, INT16_MAX, false);
    assert(gc_audio_dsp_gain_mix(&gain, -32768, -30000, false) == INT16_MIN);
    memset(effect->filter, 0, sizeof(effect->filter));
    memset(effect->history, 0, sizeof(effect->history));
    effect->filter[7] = effect->history[7] = INT16_MAX;
    memset(buses, 0, sizeof(buses));
    gc_audio_effects_begin(audio, buses);
    assert(buses[1] == 32765);
    gain = (GcAudioDspGain){0};
    gc_audio_dsp_gain_prepare(&gain, INT16_MAX, false);
    buses[1] = gc_audio_dsp_gain_mix(&gain, INT16_MAX, buses[1], false);
    buses[1] = gc_audio_dsp_gain_mix(&gain, INT16_MIN, buses[1], false);
    assert(buses[1] == 16383);
    free(audio);
}

static void test_native_track_ownership(void) {
    uint8_t sequence[0x50] = {0};
    const uint8_t root[] = {0xc1, 0,    0, 0,    0x20, 0xc1, 1, 0, 0,
                            0x30, 0x80, 1, 0xc8, 0,    0,    0, 10};
    const uint8_t parent[] = {0xc1, 0, 0, 0, 0x40, 0x80, 1, 0xc8, 0, 0, 0, 0x35};
    const uint8_t reused[] = {0x80, 10, 0xff};
    memcpy(sequence, root, sizeof(root));
    sequence[0x20] = 0xff;
    memcpy(sequence + 0x30, parent, sizeof(parent));
    memcpy(sequence + 0x40, reused, sizeof(reused));
    GcAudio *audio = calloc(1, sizeof(*audio));
    assert(audio);
    audio->sequence = sequence;
    audio->sequence_size = sizeof(sequence);
    gc_audio_sequence_init(audio);
    unsigned parent_index = audio->tracks[0].children[1];
    unsigned reused_index = audio->tracks[parent_index].children[0];
    assert(parent_index < GC_AUDIO_TRACKS && reused_index < GC_AUDIO_TRACKS);
    assert(audio->tracks[0].children[0] == GC_AUDIO_TRACKS);
    assert(audio->tracks[reused_index].parent == parent_index);
    assert(audio->tracks[reused_index].wait == 10);
    for (unsigned tick = 0; tick < 9; ++tick) {
        gc_audio_sequence_tick(audio);
        assert(audio->tracks[reused_index].wait == 9 - tick);
        assert(audio->tracks[reused_index].active);
    }
    gc_audio_sequence_tick(audio);
    assert(!audio->tracks[reused_index].active);
    assert(audio->tracks[parent_index].children[0] == GC_AUDIO_TRACKS);
    free(audio);
}

static void test_native_dsp_sequence_boundaries(void) {
    const uint8_t sequence[] = {0x80, 1, 0xc8, 0, 0, 0, 0};
    GcAudio *audio = calloc(1, sizeof(*audio));
    assert(audio);
    audio->sample_rate = 32029;
    audio->sequence = (uint8_t *)sequence;
    audio->sequence_size = sizeof(sequence);
    gc_audio_sequence_init(audio);
    float stereo[2];
    /* At 120*48/25200, the first sequence tick is the fifth DSP update.
     * The output adapter primes two samples, hence native sample398 below.
     */
    for (unsigned sample = 0; sample < 398; ++sample) {
        gc_audio_render(audio, stereo, 1);
        assert(audio->sequence_ticks == 0);
    }
    gc_audio_render(audio, stereo, 1);
    assert(audio->sequence_ticks == 1);
    free(audio);
}

static void test_native_parameter_interpolation(void) {
    const uint8_t sequence[] = {0x9e, 1, 0x40, 0, 4,    0x80, 1,
                                0x9c, 1, 0x20, 0, 0x80, 5,    0xff};
    GcAudio *audio = calloc(1, sizeof(*audio));
    assert(audio);
    audio->sequence = (uint8_t *)sequence;
    audio->sequence_size = sizeof(sequence);
    gc_audio_sequence_init(audio);
    assert(audio->tracks[0].parameters[1] == 0.125f);
    assert(audio->tracks[0].parameter_ticks[1] == 3);
    gc_audio_sequence_tick(audio);
    assert(fabsf(audio->tracks[0].parameters[1] - 1.0f / 6) < 0.000001f);
    assert(audio->tracks[0].parameter_ticks[1] == 2);
    gc_audio_sequence_tick(audio);
    assert(fabsf(audio->tracks[0].parameters[1] - 5.0f / 24) < 0.000001f);
    gc_audio_sequence_tick(audio);
    assert(audio->tracks[0].parameters[1] == 0.25f);
    assert(audio->tracks[0].parameter_ticks[1] == 0);
    free(audio);
}

static void test_native_track_register_inheritance(void) {
    uint8_t sequence[0x30] = {0};
    const uint8_t root[] = {0xac, 6,    0,    7,    0xa4, 7, 24,   0xc1, 0, 0,
                            0,    0x20, 0xc1, 0x81, 0,    0, 0x25, 0x80, 50};
    memcpy(sequence, root, sizeof(root));
    sequence[0x20] = sequence[0x25] = 0x80;
    sequence[0x21] = sequence[0x26] = 50;
    GcAudio *audio = calloc(1, sizeof(*audio));
    assert(audio);
    audio->sequence = sequence;
    audio->sequence_size = sizeof(sequence);
    gc_audio_sequence_init(audio);
    unsigned inherited = audio->tracks[0].children[0];
    unsigned independent = audio->tracks[0].children[1];
    assert(inherited < GC_AUDIO_TRACKS && independent < GC_AUDIO_TRACKS);
    assert(audio->tracks[inherited].registers[6] == 7);
    assert(audio->tracks[inherited].registers[7] == 24);
    assert(audio->tracks[independent].registers[6] == 0);
    assert(audio->tracks[independent].registers[7] == 12);
    free(audio);
}

static void test_native_cube_modulation(void) {
    GcAudio *audio = calloc(1, sizeof(*audio));
    assert(audio);
    audio->tracks[1].active = audio->tracks[2].active = true;
    audio->tracks[1].id = 0x21002;
    audio->tracks[2].id = 0x21003;
    gc_audio_sequence_cube(audio, 0, 0.25f);
    assert(audio->tracks[1].ports[1] == 16383);
    assert(audio->tracks[2].ports[3] == 1966);
    assert(audio->tracks[2].ports[2] == 1228);
    assert(!(audio->tracks[2].port_imported & 1));
    gc_audio_sequence_cube(audio, 1, 0.25f);
    assert(audio->tracks[1].ports[1] == 0);
    assert(audio->tracks[2].ports[3] == 3993);
    assert(audio->tracks[2].ports[2] == 4792);
    gc_audio_sequence_cube(audio, 1, 1);
    assert(audio->tracks[2].ports[3] == 7065);
    assert(audio->tracks[2].ports[2] == 8478);
    assert(audio->tracks[2].ports[0] == 10);
    assert(audio->tracks[2].port_imported & 1);
    audio->tracks[2].port_imported = 0;
    gc_audio_sequence_cube(audio, 0, 0.49f);
    assert(audio->tracks[2].ports[0] == 14);
    assert(audio->tracks[2].port_imported & 1);
    free(audio);
}

static uint32_t resource_word(const uint8_t *bytes) {
    return (uint32_t)bytes[0] << 24 | (uint32_t)bytes[1] << 16 |
           (uint32_t)bytes[2] << 8 | bytes[3];
}

static void test_resource_cleanup_and_bounds(const char *path) {
    FILE *file = fopen(path, "rb");
    assert(file);
    uint8_t *rom = malloc(GC_IPL_ROM_SIZE);
    GcAudio *audio = calloc(1, sizeof(*audio));
    assert(rom && audio);
    assert(fread(rom, 1, GC_IPL_ROM_SIZE, file) == GC_IPL_ROM_SIZE);
    assert(fclose(file) == 0);
    assert(!gc_audio_resources_decode(NULL, rom, GC_IPL_ROM_SIZE));
    assert(!gc_audio_resources_decode(audio, NULL, GC_IPL_ROM_SIZE));
    assert(!gc_audio_resources_decode(audio, rom, GC_IPL_ROM_SIZE - 1));
    assert(!audio->sequence && !audio->wave_count);
    assert(gc_audio_resources_decode(audio, rom, GC_IPL_ROM_SIZE));
    bool revised_effects = audio->sequence_revision != 0;
    gc_audio_resources_release(audio);
    assert(!audio->sequence && !audio->wave_count);
    gc_audio_resources_release(audio);
    memset(audio, 0, sizeof(*audio));

    if (revised_effects) {
        /* A malicious delay count used to wrap during the 80-sample block
         * conversion: 0x10000001 * 80 becomes 80 in a 32-bit unsigned value.
         */
        const size_t delay_offset = 0xf7dc0 + 12;
        uint8_t saved[4];
        memcpy(saved, rom + delay_offset, sizeof(saved));
        const uint8_t overflowing_delay[4] = {0x10, 0, 0, 1};
        memcpy(rom + delay_offset, overflowing_delay, sizeof(overflowing_delay));
        assert(!gc_audio_resources_decode(audio, rom, GC_IPL_ROM_SIZE));
        assert(audio->wave_count && audio->waves[0].samples);
        gc_audio_resources_release(audio);
        memcpy(rom + delay_offset, saved, sizeof(saved));
        memset(audio, 0, sizeof(*audio));
    }

    size_t waves = 0;
    while (waves + 4 <= GC_IPL_ROM_SIZE && memcmp(rom + waves, "WSYS", 4))
        ++waves;
    assert(waves + 24 <= GC_IPL_ROM_SIZE);
    const uint8_t *system = rom + waves;
    size_t winf = resource_word(system + 16);
    size_t archive = resource_word(system + winf + 8);
    size_t count_offset = resource_word(system + archive + 0x44) ? 0x44 : 0x70;
    size_t descriptor = resource_word(system + archive + count_offset + 4);
    uint8_t saved_loop[12];
    memcpy(saved_loop, rom + waves + descriptor + 16, sizeof(saved_loop));
    const uint8_t invalid_loop[12] = {0, 0, 0, 1};
    memcpy(rom + waves + descriptor + 16, invalid_loop, sizeof(invalid_loop));
    assert(!gc_audio_resources_decode(audio, rom, GC_IPL_ROM_SIZE));
    assert(audio->wave_count && audio->waves[0].samples);
    gc_audio_resources_release(audio);
    assert(!audio->sequence && !audio->wave_count && !audio->waves[0].samples);
    gc_audio_resources_release(audio);
    memcpy(rom + waves + descriptor + 16, saved_loop, sizeof(saved_loop));
    memset(audio, 0, sizeof(*audio));
    assert(gc_audio_resources_decode(audio, rom, GC_IPL_ROM_SIZE));
    gc_audio_resources_release(audio);
    free(audio);
    free(rom);
}

static void test_private_ipl(const char *path) {
    for (unsigned selector = 0; selector < 3; ++selector) {
        GcAudio *audio = gc_audio_create(path, 48000);
        assert(audio);
        assert(audio->master_gain == 13107 && audio->output_gain == 16384);
        assert(gc_audio_pitch_ratio(audio, 0) == 1);
        assert(gc_audio_pitch_ratio(audio, 12) == 2);
        assert(gc_audio_pitch_ratio(audio, -12) == 0.5f);
        float fractional =
            audio->semitone_ratios[59] * audio->fractional_semitone_ratios[32];
        assert(gc_audio_pitch_ratio(audio, -0.5f) == fractional);
        assert(gc_audio_pitch_ratio(audio, -0.499f) == fractional);
        assert(gc_audio_pitch_ratio(audio, -0.484375f) != fractional);
        assert(gc_audio_startup_sound(audio, selector));
        float samples[960];
        double energy = 0;
        double left_energy = 0;
        double right_energy = 0;
        double stereo_product = 0;
        for (unsigned block = 0; block < 1000; ++block) {
            gc_audio_render(audio, samples, 480);
            for (unsigned i = 0; i < 960; ++i) {
                assert(isfinite(samples[i]) && fabsf(samples[i]) <= 1);
                energy += (double)samples[i] * samples[i];
            }
            if (selector == 0 && block < 500) {
                for (unsigned i = 0; i < 480; ++i) {
                    float left = samples[i * 2];
                    float right = samples[i * 2 + 1];
                    left_energy += (double)left * left;
                    right_energy += (double)right * right;
                    stereo_product += (double)left * right;
                }
            }
        }
        GcAudioInfo info;
        gc_audio_info(audio, &info);
        printf("startup=%u waves=%u instruments=%u notes=%llu rejected=%u energy=%g\n",
               selector, info.wave_count, info.instrument_count,
               (unsigned long long)info.notes_started, info.rejected_commands, energy);
        assert(info.wave_count == 12 && info.instrument_count == 9);
        assert(info.notes_started >= 2 && energy > 1);
        assert(!info.rejected_commands);
        /* The recorded startup and its recovered left/right native waves are
         * stereo. Averaging a parent's panning destroys this separation.
         */
        if (selector == 0) {
            double correlation = stereo_product / sqrt(left_energy * right_energy);
            assert(correlation > 0.3 && correlation < 0.8);
            uint64_t ticks = audio->sequence_revision ? 2999 : 2859;
            assert(info.sequence_ticks >= ticks - 1 &&
                   info.sequence_ticks <= ticks + 1);
        }
        gc_audio_set_mono(audio, true);
        gc_audio_render(audio, samples, 480);
        for (unsigned i = 0; i < 480; ++i)
            /* The native surround copy retains NOT(0) == -1 on the
             * right bus even with mono route gains. Q12 output gain four
             * preserves this four-unit DC difference before host resampling.
             */
            assert(fabsf(samples[i * 2] - samples[i * 2 + 1]) <= 4.0f / 32768);
        gc_audio_destroy(audio);
    }
    for (unsigned event = 3; event < 23; ++event) {
        GcAudio *audio = gc_audio_create(path, 48000);
        assert(audio && gc_audio_event(audio, event));
        float samples[960];
        for (unsigned block = 0; block < 400; ++block) {
            gc_audio_render(audio, samples, 480);
            if (block == 3 && event <= 6) {
                /* The original navigation children at sequence 0x3f0/0x419/
                 * 0xaed/0xb2b use native wave9, key48, 32000 Hz. Their
                 * first chord notes after inherited transpose are respectively
                 * 52/55/48, 48/45/41, 60/58, 49/47. This distinguishes advance,
                 * retreat, full-page entry and full-page exit samples/pitches.
                 */
                const int semitones[4][3] = {
                    {4, 7, 0}, {0, -3, -7}, {12, 10, 0}, {1, -1, 0}};
                unsigned expected = event <= 4 ? 3 : 2;
                unsigned found = 0;
                assert(audio->notes_started == expected);
                for (unsigned index = 0; index < GC_AUDIO_VOICES; ++index) {
                    const GcAudioVoice *voice = &audio->voices[index];
                    if (!voice->active)
                        continue;
                    assert(found < expected && audio->waves[voice->wave].id == 9);
                    float pitch =
                        (float)(32000.0 / GC_AUDIO_DSP_RATE) *
                        gc_audio_pitch_ratio(audio, (float)semitones[event - 3][found]);
                    assert(fabs(voice->base_step - pitch) < 0.000001);
                    ++found;
                }
                assert(found == expected);
            }
        }
        GcAudioInfo info;
        gc_audio_info(audio, &info);
        assert(info.notes_started && !info.rejected_commands);
        gc_audio_destroy(audio);
    }
    GcAudio *audio = gc_audio_create(path, 48000);
    assert(audio);
    float samples[960];
    for (unsigned phase = 0; phase < 42; ++phase) {
        assert(gc_audio_event(audio, 23));
        assert(gc_audio_cube_motion(audio, phase & 1, (float)(phase % 21) / 20));
        gc_audio_render(audio, samples, 480);
        for (unsigned i = 0; i < 960; ++i)
            assert(isfinite(samples[i]) && fabsf(samples[i]) <= 1);
    }
    GcAudioInfo info;
    gc_audio_info(audio, &info);
    assert(info.notes_started && !info.rejected_commands);
    assert(!gc_audio_cube_motion(audio, 2, 0.5f));
    assert(!gc_audio_cube_motion(audio, 0, NAN));
    assert(!gc_audio_startup_sound(audio, 3));
    for (unsigned i = 0; i < 100; ++i)
        gc_audio_event(audio, 3);
    gc_audio_info(audio, &info);
    assert(info.dropped_events == 37);
    gc_audio_destroy(audio);
    audio = gc_audio_create(path, 48000);
    assert(audio && gc_audio_menu_begin(audio));
    /* iplrom.com opens the background children, then waits 1200 sequence
     * ticks before its first two notes. Reusing an ended child through a
     * stale parent slot formerly advanced these waits twice per tick.
     */
    for (unsigned tick = 0; tick < 1200; ++tick) {
        gc_audio_sequence_tick(audio);
        assert(audio->notes_started == 0);
    }
    gc_audio_sequence_tick(audio);
    assert(audio->notes_started == 2 && !audio->rejected_commands);
    gc_audio_destroy(audio);
    audio = gc_audio_create(path, 48000);
    assert(audio && gc_audio_menu_begin(audio));
    double energy = 0;
    for (unsigned block = 0; block < 1000; ++block) {
        gc_audio_render(audio, samples, 480);
        for (unsigned i = 0; i < 960; ++i) {
            assert(isfinite(samples[i]) && fabsf(samples[i]) <= 1);
            energy += (double)samples[i] * samples[i];
        }
    }
    gc_audio_info(audio, &info);
    printf("menu notes=%llu rejected=%u energy=%g\n",
           (unsigned long long)info.notes_started, info.rejected_commands, energy);
    assert(info.notes_started >= 5 && !info.rejected_commands && energy > 1);
    assert(gc_audio_menu_end(audio));
    for (unsigned block = 0; block < 300; ++block)
        gc_audio_render(audio, samples, 480);
    gc_audio_info(audio, &info);
    assert(!info.rejected_commands);
    gc_audio_destroy(audio);
    audio = gc_audio_create(path, 48000);
    assert(audio && gc_audio_startup_sound(audio, 0));
    for (unsigned block = 0; block < 200; ++block)
        gc_audio_render(audio, samples, 480);
    assert(gc_audio_active_voices(audio) == 2);
    assert(!gc_audio_sequence_stopped(audio));
    assert(gc_audio_stop_sequence(audio));
    gc_audio_render(audio, samples, 480);
    assert(gc_audio_sequence_stopped(audio));
    assert(gc_audio_active_voices(audio) == 2);
    uint64_t stopped_ticks = audio->sequence_ticks;
    uint64_t stopped_notes = audio->notes_started;
    for (unsigned block = 0; block < 200; ++block)
        gc_audio_render(audio, samples, 480);
    assert(gc_audio_active_voices(audio) == 0);
    assert(audio->sequence_ticks == stopped_ticks);
    assert(audio->notes_started == stopped_notes);
    gc_audio_destroy(audio);
}

static void test_audio_restart(const char *path) {
    GcAudio *fresh = gc_audio_create(path, 48000);
    GcAudio *reused = gc_audio_create(path, 48000);
    assert(fresh && reused);
    int16_t *retained_samples = reused->waves[0].samples;
    uint8_t *retained_sequence = reused->sequence;
    float expected[960];
    float actual[960];
    assert(gc_audio_menu_begin(reused));
    for (unsigned block = 0; block < 500; ++block)
        gc_audio_render(reused, actual, 480);
    assert(gc_audio_stop_sequence(reused));
    gc_audio_render(reused, actual, 480);
    assert(gc_audio_sequence_stopped(reused));
    assert(gc_audio_event(reused, 13)); /* A queued old cue must also disappear. */
    gc_audio_set_mono(reused, true);
    gc_audio_set_mono(fresh, true);
    gc_audio_reset(reused);
    assert(reused->waves[0].samples == retained_samples &&
           reused->sequence == retained_sequence && !reused->device);
    assert(reused->sequence_ticks == fresh->sequence_ticks &&
           reused->notes_started == fresh->notes_started);
    assert(gc_audio_startup_sound(fresh, 0) && gc_audio_startup_sound(reused, 0));
    for (unsigned block = 0; block < 500; ++block) {
        gc_audio_render(fresh, expected, 480);
        gc_audio_render(reused, actual, 480);
        assert(!memcmp(expected, actual, sizeof(expected)));
    }
    gc_audio_destroy(fresh);
    gc_audio_destroy(reused);
}

int main(int argc, char **argv) {
    test_residuals_and_history();
    test_clipping_and_bounds();
    test_native_envelopes();
    test_native_track_ownership();
    test_native_dsp_sequence_boundaries();
    test_native_parameter_interpolation();
    test_native_track_register_inheritance();
    test_native_cube_modulation();
    test_native_resampling();
    test_native_mixer_routes_and_delays();
    test_native_master_and_output();
    test_native_dsp_gain_ramps();
    test_native_dsp_effect_trace();
    assert(!gc_audio_create(NULL, 48000));
    assert(!gc_audio_create("Files/missing-ipl.bin", 1));
    if (argc == 2) {
        test_resource_cleanup_and_bounds(argv[1]);
        test_private_ipl(argv[1]);
        test_audio_restart(argv[1]);
    }
    puts("audio tests passed");
    return 0;
}
