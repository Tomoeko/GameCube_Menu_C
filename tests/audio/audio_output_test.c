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
    atomic_init(&audio->event_read, 0);
    atomic_init(&audio->event_write, 0);
    atomic_init(&audio->dropped_events, 0);
    atomic_init(&audio->active_voices, 0);
    atomic_init(&audio->sequence_stopped, true);
    audio->sample_rate = rate;
    audio->output_resampler = cc_audio_resampler_create(64057, 2, rate);
    audio->output_state = cc_audio_resample_state_create(
        cc_audio_resampler_taps(audio->output_resampler));
    assert(audio->output_resampler && audio->output_state);
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
    puts("Host audio output tests passed.");
    return 0;
}
