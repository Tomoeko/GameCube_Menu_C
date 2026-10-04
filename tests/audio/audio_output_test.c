#include "audio/audio_internal.h"

#include <assert.h>
#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static GcAudio *test_audio(unsigned rate, int16_t samples[257]) {
    GcAudio *audio = calloc(1, sizeof(*audio));
    assert(audio);
    atomic_init(&audio->mono, false);
    atomic_init(&audio->menu_volume_adjustment, 0);
    atomic_init(&audio->event_read, 0);
    atomic_init(&audio->event_write, 0);
    atomic_init(&audio->dropped_events, 0);
    atomic_init(&audio->active_voices, 0);
    atomic_init(&audio->sequence_stopped, true);
    audio->sample_rate = rate;
    audio->output_resampler = cc_audio_resampler_create(64057, 2, rate);
    audio->output_state = cc_audio_resample_state_create(
        cc_audio_resampler_taps(audio->output_resampler));
    audio->music_output_state = cc_audio_resample_state_create(
        cc_audio_resampler_taps(audio->output_resampler));
    assert(audio->output_resampler && audio->output_state && audio->music_output_state);
    audio->master_gain = 32767;
    audio->output_gain = 4096;
    audio->waves[0] =
        (GcAudioWave){.samples = samples, .count = 257, .loop = true, .loop_end = 257};
    audio->tracks[0].parent = GC_AUDIO_TRACKS;
    audio->tracks[0].parameters[0] = 1;
    audio->tracks[0].routes[0] = 0x10;
    audio->voices[0] = (GcAudioVoice){.active = true,
                                      .detached = true,
                                      .released = true,
                                      .step = 1,
                                      .base_gain = 0.75f,
                                      .track_gain = 1,
                                      .envelope_volume = 1,
                                      .envelope_pitch = 1,
                                      .buses = {1},
                                      .routes = {0x10},
                                      .pan_weights = {0, 1, 0}};
    for (unsigned phase = 0; phase < 64; ++phase)
        audio->resampling_coefficients[phase][3] = 32767;
    return audio;
}

typedef struct {
    GcAudio *audio;
    uint64_t native_frames;
    bool mono;
} HostReference;

static void reference_begin(HostReference *reference, GcAudio *audio) {
    *reference = (HostReference){.audio = audio};
}

static bool reference_native_frame(void *context, float output[2]) {
    HostReference *reference = context;
    gc_audio_dsp_render_frame(reference->audio, reference->mono, output);
    ++reference->native_frames;
    return true;
}

static void reference_frame(HostReference *reference, float output[2]) {
    assert(cc_audio_resampler_frame(reference->audio->output_resampler,
                                    reference->audio->output_state,
                                    reference_native_frame, reference, output));
}

static void destroy_test_audio(GcAudio *audio) {
    cc_audio_resample_state_destroy(audio->output_state);
    cc_audio_resample_state_destroy(audio->music_output_state);
    cc_audio_resampler_destroy(audio->output_resampler);
    free(audio);
}

static void test_output_clock(unsigned rate, unsigned pattern) {
    int16_t samples[257];
    for (unsigned index = 0; index < 257; ++index) {
        samples[index] = pattern == 0   ? (int16_t)((int)(index * 7919 % 60001) - 30000)
                         : pattern == 1 ? 12000
                                        : (index == 0 ? 24000 : 0);
    }
    GcAudio *audio = test_audio(rate, samples);
    assert(gc_audio_sample_rate(audio) == rate);
    GcAudio *expected_audio = test_audio(rate, samples);
    HostReference reference;
    reference_begin(&reference, expected_audio);
    size_t frame_count = (size_t)rate * 2;
    float actual[1024];
    float minimum = INFINITY;
    float maximum = -INFINITY;
    size_t completed = 0;
    while (completed < frame_count) {
        size_t count = 1 + completed % 512;
        if (count > frame_count - completed)
            count = frame_count - completed;
        gc_audio_render(audio, actual, count);
        for (size_t frame = 0; frame < count; ++frame) {
            float expected[2];
            reference_frame(&reference, expected);
            assert(!memcmp(actual + frame * 2, expected, sizeof(expected)));
            minimum = fminf(minimum, actual[frame * 2]);
            maximum = fmaxf(maximum, actual[frame * 2]);
        }
        completed += count;
    }
    assert(reference.native_frames == 64058);
    assert(maximum > 0 && maximum > minimum);
    assert(audio->update_samples == 58);
    assert(audio->voices[0].position == expected_audio->voices[0].position);
    destroy_test_audio(expected_audio);
    destroy_test_audio(audio);
}

static void test_chunks_and_reset(void) {
    int16_t samples[257];
    for (unsigned index = 0; index < 257; ++index)
        samples[index] = (int16_t)((int)(index * 7919 % 60001) - 30000);
    GcAudio *single = test_audio(42422, samples);
    GcAudio *chunked = test_audio(42422, samples);
    const size_t frames = 84844;
    float *expected = malloc(frames * 2 * sizeof(*expected));
    float *actual = malloc(frames * 2 * sizeof(*actual));
    assert(expected && actual);
    gc_audio_render(single, expected, frames);
    for (size_t frame = 0; frame < frames;) {
        size_t count = 1 + frame % 997;
        if (count > frames - frame)
            count = frames - frame;
        gc_audio_render(chunked, actual + frame * 2, count);
        frame += count;
    }
    assert(!memcmp(expected, actual, frames * 2 * sizeof(*actual)));
    assert(single->voices[0].position == chunked->voices[0].position);
    gc_audio_render(chunked, NULL, 1);
    gc_audio_render(chunked, actual, 0);
    double position = chunked->voices[0].position;
    actual[0] = 1;
    gc_audio_render(chunked, actual, SIZE_MAX);
    assert(actual[0] == 1 && chunked->voices[0].position == position);
    gc_audio_render(chunked, actual, 2);
    assert(chunked->voices[0].position > position);
    gc_audio_reset(chunked);
    assert(chunked->update_samples == 0);
    gc_audio_render(NULL, actual, 1);
    assert(actual[0] == 0 && actual[1] == 0);
    free(actual);
    free(expected);
    destroy_test_audio(chunked);
    destroy_test_audio(single);
}

static void test_mono_transition(void) {
    int16_t samples[257];
    for (size_t index = 0; index < 257; ++index)
        samples[index] = 12000;
    GcAudio *audio = test_audio(48000, samples);
    GcAudio *native = test_audio(48000, samples);
    GcAudio *fixtures[] = {audio, native};
    for (size_t index = 0; index < 2; ++index) {
        GcAudioVoice *voice = &fixtures[index]->voices[0];
        voice->buses[1] = 2;
        voice->routes[0] = 0x14;
        voice->routes[1] = 0x24;
        voice->pan_weights[1] = 0;
        voice->pan_weights[2] = 1;
        assert(gc_audio_route_scale(fixtures[index], voice, 0, false) == 1);
        assert(gc_audio_route_scale(fixtures[index], voice, 1, false) == 0);
        assert(gc_audio_route_scale(fixtures[index], voice, 0, true) ==
               gc_audio_route_scale(fixtures[index], voice, 1, true));
    }
    HostReference reference;
    reference_begin(&reference, native);
    float actual[1024], expected[2];
    for (size_t phase = 0; phase < 3; ++phase) {
        if (phase == 1) {
            gc_audio_set_mono(audio, true);
            reference.mono = true;
        }
        gc_audio_render(audio, actual, 512);
        for (size_t frame = 0; frame < 512; ++frame) {
            reference_frame(&reference, expected);
            assert(!memcmp(actual + frame * 2, expected, sizeof(expected)));
            if (phase == 2)
                assert(fabsf(actual[frame * 2] - actual[frame * 2 + 1]) <
                       4.0f / 32768 + 8 * FLT_EPSILON);
        }
        /* A route update changes native gains; reconstruction preserves its
         * causal history rather than replacing already queued stereo samples.
         */
        if (phase == 1)
            assert(actual[0] > actual[1] + 0.01f);
    }
    destroy_test_audio(native);
    destroy_test_audio(audio);
}

static void test_capture_wrapping(void) {
    int16_t samples[257];
    for (unsigned index = 0; index < 257; ++index)
        samples[index] = (int16_t)((int)(index * 7919 % 60001) - 30000);
    GcAudio *audio = test_audio(48000, samples);
    assert(!gc_audio_capture_begin(NULL, 17));
    assert(!gc_audio_capture_begin(audio, 0));
    assert(!gc_audio_capture_begin(audio, SIZE_MAX));
    assert(!gc_audio_capture_begin(audio, UINT_MAX));
    audio->device = audio;
    assert(!gc_audio_capture_begin(audio, 17));
    audio->device = NULL;
    float rendered[34];
    float captured[34];
    uint64_t first_sample = UINT64_MAX;
    assert(gc_audio_capture_read(audio, captured, 17, &first_sample) == 0);
    assert(first_sample == UINT64_MAX && !gc_audio_capture_failed(audio));
    gc_audio_render(audio, rendered, 17);
    assert(gc_audio_capture_begin(audio, 17));
    assert(!gc_audio_capture_begin(audio, 17));
    uint64_t completed = 0;
    for (unsigned iteration = 0; iteration < 1000; ++iteration) {
        size_t frames = 1 + iteration % 17;
        gc_audio_render(audio, rendered, frames);
        size_t received = 0;
        while (received < frames) {
            size_t count = 1 + (iteration + received) % 7;
            size_t remaining = frames - received;
            if (count > remaining)
                count = remaining;
            assert(gc_audio_capture_read(audio, captured, count, &first_sample) ==
                   count);
            assert(first_sample == completed + received);
            assert(
                !memcmp(rendered + received * 2, captured, count * 2 * sizeof(float)));
            received += count;
        }
        completed += frames;
        assert(gc_audio_capture_read(audio, captured, 17, &first_sample) == 0);
        assert(first_sample == completed && !gc_audio_capture_failed(audio));
    }
    assert(gc_audio_capture_read(audio, NULL, 1, NULL) == 0);
    assert(gc_audio_capture_read(audio, captured, 0, NULL) == 0);
    assert(gc_audio_capture_read(audio, captured, SIZE_MAX, NULL) == 0);
    gc_audio_capture_end(audio);
    gc_audio_capture_end(audio);
    assert(!gc_audio_capture_failed(audio));
    destroy_test_audio(audio);
}

static void test_capture_overflow_and_reset(void) {
    int16_t samples[257];
    for (unsigned index = 0; index < 257; ++index)
        samples[index] = (int16_t)((int)(index * 7919 % 60001) - 30000);
    GcAudio *audio = test_audio(48000, samples);
    GcAudio *reference = test_audio(48000, samples);
    assert(gc_audio_capture_begin(audio, 3));
    float rendered[12];
    float expected[12];
    float captured[12];
    gc_audio_render(audio, rendered, 2);
    gc_audio_render(reference, expected, 2);
    assert(!memcmp(rendered, expected, 4 * sizeof(float)));
    gc_audio_render(audio, rendered + 4, 2);
    gc_audio_render(reference, expected + 4, 2);
    assert(!memcmp(rendered + 4, expected + 4, 4 * sizeof(float)));
    assert(gc_audio_capture_failed(audio));
    uint64_t first_sample = UINT64_MAX;
    assert(gc_audio_capture_read(audio, captured, 6, &first_sample) == 2);
    assert(first_sample == 0 && !memcmp(rendered, captured, 4 * sizeof(float)));
    gc_audio_render(audio, rendered, 6);
    gc_audio_render(reference, expected, 6);
    assert(!memcmp(rendered, expected, sizeof(rendered)));
    assert(gc_audio_capture_read(audio, captured, 6, NULL) == 0);
    assert(gc_audio_capture_failed(audio));
    gc_audio_capture_end(audio);
    assert(gc_audio_capture_begin(audio, 6));
    gc_audio_render(audio, rendered, 3);
    gc_audio_reset(audio);
    gc_audio_render(audio, rendered + 6, 3);
    assert(gc_audio_capture_read(audio, captured, 4, &first_sample) == 4);
    assert(first_sample == 0 && !memcmp(rendered, captured, 8 * sizeof(float)));
    assert(gc_audio_capture_read(audio, captured, 4, &first_sample) == 2);
    assert(first_sample == 4 && !memcmp(rendered + 8, captured, 4 * sizeof(float)));
    assert(!gc_audio_capture_failed(audio));
    gc_audio_capture_end(audio);
    destroy_test_audio(reference);
    destroy_test_audio(audio);
}

static void test_menu_volume_output(void) {
    int16_t samples[257];
    for (size_t index = 0; index < 257; ++index)
        samples[index] = 12000;
    GcAudio *audio = test_audio(48000, samples);
    audio->voices[0].menu_music = true;
    audio->voices[1] = audio->voices[0];
    audio->voices[1].menu_music = false;
    audio->voices[1].buses[0] = 2;
    audio->voices[1].routes[0] = 0x20;
    GcAudio *effects = test_audio(48000, samples);
    effects->voices[0].buses[0] = 2;
    effects->voices[0].routes[0] = 0x20;
    GcAudio *music = test_audio(48000, samples);
    float rendered[2048], captured[2048], expected_effects[2048];
    float original_music[2048];
    assert(gc_audio_menu_volume(audio) == 100);
    assert(!gc_audio_set_menu_volume(NULL, 50));
    assert(gc_audio_menu_volume(NULL) == 0);
    assert(!gc_audio_set_menu_volume(audio, GC_AUDIO_MENU_VOLUME_MAX + 1));
    assert(gc_audio_menu_volume(audio) == 100);
    assert(gc_audio_capture_begin(audio, 1024));
    const unsigned volumes[] = {100, 50, 0, 100, 200, GC_AUDIO_MENU_VOLUME_MAX, 100};
    for (unsigned phase = 0; phase < sizeof(volumes) / sizeof(*volumes); ++phase) {
        assert(gc_audio_set_menu_volume(audio, volumes[phase]));
        assert(gc_audio_menu_volume(audio) == volumes[phase]);
        gc_audio_render(audio, rendered, 1024);
        gc_audio_render(effects, expected_effects, 1024);
        gc_audio_render(music, original_music, 1024);
        uint64_t first;
        assert(gc_audio_capture_read(audio, captured, 1024, &first) == 1024);
        assert(first == (uint64_t)phase * 1024);
        assert(!memcmp(rendered, captured, sizeof(rendered)));
        for (unsigned frame = 768; frame < 1024; ++frame) {
            float effect = rendered[frame * 2 + 1];
            assert(effect == expected_effects[frame * 2 + 1]);
            float expected = original_music[frame * 2] * (float)volumes[phase] / 100;
            assert(fabsf(rendered[frame * 2] - expected) < 2.0f / 32768);
        }
    }
    assert(gc_audio_set_menu_volume(audio, 35));
    gc_audio_reset(audio);
    assert(gc_audio_menu_volume(audio) == 35);
    assert(gc_audio_set_menu_volume(audio, GC_AUDIO_MENU_VOLUME_MAX));
    gc_audio_reset(audio);
    assert(gc_audio_menu_volume(audio) == GC_AUDIO_MENU_VOLUME_MAX);
    gc_audio_capture_end(audio);
    destroy_test_audio(music);
    destroy_test_audio(effects);
    destroy_test_audio(audio);
}

static void configure_music_effects(GcAudio *audio, int16_t effects[257],
                                    unsigned revision, bool music) {
    audio->sequence_revision = revision;
    gc_audio_route_table_init(audio);
    audio->voices[0].menu_music = music;
    audio->voices[0].base_gain = 0.25f;
    audio->voices[0].pan = 0.2f;
    audio->voices[0].step = 3307.0 / 4096;
    const unsigned buses[6] = {1, 3, 8, 9, 10, 11};
    for (unsigned route = 0; route < 6; ++route) {
        audio->voices[0].buses[route] = buses[route];
        audio->voices[0].routes[route] = 0;
    }
    audio->voices[0].routes[0] = revision ? 0x50 : 0x4;
    assert(gc_audio_route_gain(audio, &audio->voices[0], 0, false) !=
           gc_audio_route_gain(audio, &audio->voices[0], 0, true));
    audio->waves[1] =
        (GcAudioWave){.samples = effects, .count = 257, .loop = true, .loop_end = 257};
    audio->voices[1] = audio->voices[0];
    audio->voices[1].wave = 1;
    audio->voices[1].menu_music = false;
    audio->voices[1].base_gain = 0.125f;
    audio->voices[1].pan = 0.8f;
    audio->voices[1].routes[0] = revision ? 0x10 : 0x4;
    audio->voices[1].buses[0] = 2;
    for (unsigned route = 2; route < 6; ++route)
        audio->voices[1].buses[route] = 0;
    audio->effects[0] = (GcAudioEffect){.mode = 1,
                                        .length = 17,
                                        .return_bus = {1, 2},
                                        .return_gain = {8192, -4096},
                                        .filter = {0, 0, 0, 0, 0, 0, 0, 16384}};
    memcpy(audio->music_output.effects, audio->effects, sizeof(audio->effects));
    audio->chorus_read = audio->music_output.chorus_read = revision ? 150u * 65536u : 0;
    audio->chorus_direction = audio->music_output.chorus_direction = revision ? -1 : 0;
}

static bool reference_music_frame(void *context, float stereo[2]) {
    HostReference *reference = context;
    float full_mix[2];
    gc_audio_dsp_render_frame(reference->audio, reference->mono, full_mix);
    memcpy(stereo, reference->audio->music_output.stereo, sizeof(float[2]));
    ++reference->native_frames;
    return true;
}

static float normalized_limit(float sample) {
    return fmaxf(-1, fminf(1, sample));
}

static void test_warm_music_stem(unsigned revision, unsigned rate, bool music) {
    int16_t samples[257], effect_samples[257];
    for (unsigned index = 0; index < 257; ++index) {
        samples[index] = (int16_t)((int)(index * 7919 % 4001) - 2000);
        effect_samples[index] = (int16_t)((int)(index * 3137 % 1001) - 500);
    }
    GcAudio *audio = test_audio(rate, samples);
    GcAudio *original = test_audio(rate, samples);
    GcAudio *stem = test_audio(rate, samples);
    configure_music_effects(audio, effect_samples, revision, music);
    configure_music_effects(original, effect_samples, revision, music);
    configure_music_effects(stem, effect_samples, revision, music);
    HostReference reference;
    reference_begin(&reference, stem);
    assert(gc_audio_capture_begin(audio, 512));
    const unsigned volumes[] = {
        100, 0, GC_AUDIO_MENU_VOLUME_MAX, 50, GC_AUDIO_MENU_VOLUME_MAX, 100};
    uint64_t host_frames = 0;
    bool heard_stem = false;
    for (unsigned phase = 0; phase < sizeof(volumes) / sizeof(*volumes); ++phase) {
        assert(gc_audio_set_menu_volume(audio, volumes[phase]));
        bool mono = phase >= 4;
        gc_audio_set_mono(audio, mono);
        gc_audio_set_mono(original, mono);
        reference.mono = mono;
        /* Stop dry music during the last phase; its effect history must
         * continue through both host reconstruction streams without resets.
         */
        if (phase == 5) {
            audio->voices[0].active = original->voices[0].active =
                stem->voices[0].active = false;
        }
        for (unsigned completed = 0; completed < 2048;) {
            unsigned count = 1 + completed % 512;
            if (count > 2048 - completed)
                count = 2048 - completed;
            float rendered[1024], baseline[1024], captured[1024];
            gc_audio_render(audio, rendered, count);
            gc_audio_render(original, baseline, count);
            uint64_t first;
            assert(gc_audio_capture_read(audio, captured, count, &first) == count);
            assert(first == host_frames);
            assert(!memcmp(rendered, captured, count * sizeof(float[2])));
            for (unsigned frame = 0; frame < count; ++frame) {
                float contribution[2];
                assert(cc_audio_resampler_frame(
                    stem->output_resampler, stem->output_state, reference_music_frame,
                    &reference, contribution));
                for (unsigned channel = 0; channel < 2; ++channel) {
                    heard_stem = heard_stem || contribution[channel] != 0;
                    if (!music)
                        assert(contribution[channel] == 0);
                    float expected = baseline[frame * 2 + channel];
                    if (volumes[phase] != 100)
                        expected +=
                            ((float)volumes[phase] / 100 - 1) * contribution[channel];
                    assert(rendered[frame * 2 + channel] == normalized_limit(expected));
                }
            }
            /* Host controls never change original voice, native mixer or
             * shared effect histories, including mute-to-boost transitions.
             */
            assert(!memcmp(audio->voices, original->voices, sizeof(audio->voices)));
            assert(!memcmp(audio->effects, original->effects, sizeof(audio->effects)));
            assert(!memcmp(audio->chorus, original->chorus, sizeof(audio->chorus)));
            host_frames += count;
            completed += count;
        }
    }
    assert(heard_stem == music);
    assert(!gc_audio_capture_failed(audio));
    gc_audio_capture_end(audio);
    destroy_test_audio(stem);
    destroy_test_audio(original);
    destroy_test_audio(audio);
}

static void test_music_boost_limit(void) {
    int16_t samples[257];
    for (unsigned index = 0; index < 257; ++index)
        samples[index] = index % 64 < 32 ? INT16_MAX : INT16_MIN;
    GcAudio *audio = test_audio(48000, samples);
    audio->voices[0].menu_music = true;
    assert(gc_audio_set_menu_volume(audio, GC_AUDIO_MENU_VOLUME_MAX));
    assert(gc_audio_capture_begin(audio, 2048));
    float rendered[4096], captured[4096];
    gc_audio_render(audio, rendered, 2048);
    bool positive_limit = false;
    bool negative_limit = false;
    for (unsigned sample = 0; sample < 4096; ++sample) {
        assert(isfinite(rendered[sample]));
        assert(rendered[sample] >= -1 && rendered[sample] <= 1);
        positive_limit = positive_limit || rendered[sample] == 1;
        negative_limit = negative_limit || rendered[sample] == -1;
    }
    assert(positive_limit && negative_limit);
    uint64_t first;
    assert(gc_audio_capture_read(audio, captured, 2048, &first) == 2048);
    assert(first == 0 && !memcmp(rendered, captured, sizeof(rendered)));
    gc_audio_capture_end(audio);
    destroy_test_audio(audio);
}

static void test_reconstruction_peak_limit(void) {
    int16_t samples[257];
    for (unsigned index = 0; index < 257; ++index)
        samples[index] = index % 64 < 32 ? INT16_MAX : INT16_MIN;
    GcAudio *audio = test_audio(48000, samples);
    GcAudio *native = test_audio(48000, samples);
    audio->output_gain = native->output_gain = 16384;
    HostReference reference;
    reference_begin(&reference, native);
    float rendered[4096], captured[4096];
    assert(gc_audio_capture_begin(audio, 2048));
    gc_audio_render(audio, rendered, 2048);
    bool positive_limit = false;
    bool negative_limit = false;
    float largest_peak = 0;
    for (unsigned frame = 0; frame < 2048; ++frame) {
        float expected[2];
        reference_frame(&reference, expected);
        for (unsigned channel = 0; channel < 2; ++channel) {
            float raw = expected[channel];
            float value = rendered[frame * 2 + channel];
            largest_peak = fmaxf(largest_peak, fabsf(raw));
            positive_limit |= raw > 1;
            negative_limit |= raw < -1;
            assert(value == fmaxf(-1, fminf(1, raw)));
        }
    }
    assert(positive_limit && negative_limit && largest_peak > 1.2f);
    assert(gc_audio_capture_read(audio, captured, 2048, NULL) == 2048);
    assert(!memcmp(rendered, captured, sizeof(rendered)));
    gc_audio_capture_end(audio);
    destroy_test_audio(native);
    destroy_test_audio(audio);
}

int main(void) {
    assert(gc_audio_sample_rate(NULL) == 0);
    const unsigned rates[] = {8000,  11025, 22050, 32000, 42422,
                              44100, 48000, 96000, 192000};
    for (unsigned index = 0; index < sizeof(rates) / sizeof(rates[0]); ++index)
        test_output_clock(rates[index], 0);
    test_output_clock(48000, 1);
    test_output_clock(48000, 2);
    test_chunks_and_reset();
    test_mono_transition();
    test_capture_wrapping();
    test_capture_overflow_and_reset();
    test_menu_volume_output();
    for (unsigned revision = 0; revision < 2; ++revision) {
        test_warm_music_stem(revision, 8000, true);
        test_warm_music_stem(revision, 192000, true);
        test_warm_music_stem(revision, 48000, false);
    }
    test_music_boost_limit();
    test_reconstruction_peak_limit();
    puts("Host audio output tests passed.");
    return 0;
}
