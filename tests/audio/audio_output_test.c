#include "audio/audio_internal.h"

#include <assert.h>
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
    uint64_t phase;
    uint64_t native_frames;
    float previous[2];
    float next[2];
} HostReference;

static void reference_begin(HostReference *reference, GcAudio *audio) {
    *reference = (HostReference){.audio = audio, .native_frames = 2};
    gc_audio_dsp_render_frame(audio, false, reference->previous);
    gc_audio_dsp_render_frame(audio, false, reference->next);
}

static void reference_frame(HostReference *reference, float output[2]) {
    uint64_t denominator = (uint64_t)reference->audio->sample_rate * 2;
    float fraction = (float)((double)reference->phase / (double)denominator);
    for (unsigned channel = 0; channel < 2; ++channel) {
        float difference = reference->next[channel] - reference->previous[channel];
        output[channel] = fmaf(difference, fraction, reference->previous[channel]);
    }
    reference->phase += 64057;
    while (reference->phase >= denominator) {
        memcpy(reference->previous, reference->next, sizeof(reference->previous));
        gc_audio_dsp_render_frame(reference->audio, false, reference->next);
        reference->phase -= denominator;
        ++reference->native_frames;
    }
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
    assert(reference.native_frames == 64059 && reference.phase == 0);
    assert(maximum > 0 && maximum > minimum);
    assert(audio->output_phase == 0 && audio->update_samples == 59);
    assert(audio->voices[0].position == expected_audio->voices[0].position);
    assert(!memcmp(audio->output_previous, reference.previous,
                   sizeof(reference.previous)));
    assert(!memcmp(audio->output_next, reference.next, sizeof(reference.next)));
    free(expected_audio);
    free(audio);
}

static void test_fraction_rounding(void) {
    /* Division is the independent reference. Reciprocal multiplication may
     * replace it only when the final binary32 rounding is identical.
     */
    const unsigned rates[] = {8000, 11025, 42422, 44100, 48000, 192000};
    for (unsigned index = 0; index < sizeof(rates) / sizeof(rates[0]); ++index) {
        unsigned denominator = rates[index] * 2;
        double reciprocal = 1.0 / denominator;
        for (unsigned phase = 0; phase < denominator; ++phase)
            assert((float)((double)phase * reciprocal) ==
                   (float)((double)phase / denominator));
    }
    for (unsigned rate = 8000; rate <= 192000; ++rate) {
        unsigned denominator = rate * 2;
        double reciprocal = 1.0 / denominator;
        for (unsigned index = 0; index < 16; ++index) {
            unsigned phase = (unsigned)(((uint64_t)denominator * index) / 16);
            assert((float)((double)phase * reciprocal) ==
                   (float)((double)phase / denominator));
            if (phase)
                assert((float)((double)(phase - 1) * reciprocal) ==
                       (float)((double)(phase - 1) / denominator));
        }
    }
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
    assert(single->output_phase == chunked->output_phase);
    gc_audio_render(chunked, NULL, 1);
    gc_audio_render(chunked, actual, 0);
    assert(chunked->output_phase == 0 && chunked->output_ready);
    actual[0] = 1;
    gc_audio_render(chunked, actual, SIZE_MAX);
    assert(actual[0] == 1 && chunked->output_phase == 0);
    gc_audio_render(chunked, actual, 1);
    assert(chunked->output_phase != 0);
    gc_audio_reset(chunked);
    assert(chunked->output_phase == 0 && !chunked->output_ready);
    assert(chunked->output_previous[0] == 0 && chunked->output_next[0] == 0);
    gc_audio_render(NULL, actual, 1);
    assert(actual[0] == 0 && actual[1] == 0);
    free(actual);
    free(expected);
    free(chunked);
    free(single);
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
    free(audio);
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
    free(reference);
    free(audio);
}

int main(void) {
    assert(gc_audio_sample_rate(NULL) == 0);
    test_fraction_rounding();
    const unsigned rates[] = {8000,  11025, 22050, 32000, 42422,
                              44100, 48000, 96000, 192000};
    for (unsigned index = 0; index < sizeof(rates) / sizeof(rates[0]); ++index)
        test_output_clock(rates[index], 0);
    test_output_clock(48000, 1);
    test_output_clock(48000, 2);
    test_chunks_and_reset();
    test_capture_wrapping();
    test_capture_overflow_and_reset();
    puts("Host audio output tests passed.");
    return 0;
}
