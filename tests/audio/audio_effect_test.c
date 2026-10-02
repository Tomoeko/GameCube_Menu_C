#include "audio/audio_internal.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    unsigned frame;
    int16_t sample;
    int16_t returned;
    int16_t output;
} ReturnPulse;

typedef struct {
    uint32_t initial_read;
    int direction;
    unsigned positive_frame;
    unsigned negative_frame;
    uint32_t block_reads[3];
} ChorusScenario;

static void test_mode_two_history_tail_and_clipping(void) {
    GcAudio *audio = calloc(1, sizeof(*audio));
    assert(audio);
    audio->output_gain = 16384;
    GcAudioEffect *effect = &audio->effects[0];
    effect->mode = 2;
    effect->length = 80;
    effect->return_bus[0] = 1;
    effect->return_gain[0] = 16384;
    const ReturnPulse pulses[] = {{0, 16000, 8000, 32000},
                                  {79, -16000, -8000, -32000},
                                  {80, 32000, 16000, INT16_MAX},
                                  {159, INT16_MIN, -16384, INT16_MIN},
                                  {160, INT16_MAX, 16384, INT16_MAX}};

    /* Mode 2 returns the eight history words before new DMA input. Its
     * 80-sample ring therefore returns each pulse after 88 samples.
     * These synthetic checks establish C behavior, not hardware fidelity.
     */
    for (unsigned frame = 0; frame < 400; ++frame) {
        int16_t buses[12] = {0};
        gc_audio_effects_begin(audio, buses);
        int16_t expected_return = 0;
        int16_t expected_output = 0;
        for (size_t index = 0; index < sizeof(pulses) / sizeof(pulses[0]); ++index) {
            const ReturnPulse *pulse = &pulses[index];
            if (frame == pulse->frame)
                buses[3] = pulse->sample;
            if (frame == pulse->frame + 88) {
                expected_return = pulse->returned;
                expected_output = pulse->output;
            }
        }
        gc_audio_effects_end(audio, buses);
        if (buses[1] != expected_return)
            fprintf(stderr, "Mode 2 frame %u: return %d, expected %d\n", frame,
                    buses[1], expected_return);
        assert(buses[1] == expected_return);
        assert(buses[2] == -1);
        assert(gc_audio_dsp_output(audio, buses[1]) == expected_output);
    }
    assert(effect->position == 0);
    assert(audio->dsp_frame == 0);
    free(audio);
}

static void test_chorus_scenario(const ChorusScenario *scenario) {
    GcAudio *audio = calloc(1, sizeof(*audio));
    assert(audio);
    audio->sequence_revision = 1;
    audio->chorus_read = scenario->initial_read;
    audio->chorus_direction = scenario->direction;
    unsigned heard = 0;

    for (unsigned frame = 0; frame < 400; ++frame) {
        int16_t buses[12] = {0};
        gc_audio_effects_begin(audio, buses);
        if (frame == 0)
            buses[10] = 16000;
        else if (frame == 79)
            buses[10] = -16000;
        gc_audio_effects_end(audio, buses);

        int16_t expected = frame == scenario->positive_frame   ? 8000
                           : frame == scenario->negative_frame ? -8000
                                                               : 0;
        assert(buses[1] == expected);
        assert(buses[2] == expected - 1);
        if (expected)
            ++heard;
        assert(audio->chorus_read < 160u * 65536u);
        if ((frame + 1) % 80 == 0) {
            unsigned block = (frame + 1) / 80;
            assert(audio->chorus_frame == 0);
            assert(audio->chorus_write == (block % 2 ? 80u : 0u));
            if (block <= 3)
                assert(audio->chorus_read == scenario->block_reads[block - 1]);
        }
    }
    assert(heard == 2);
    assert(audio->dsp_frame == 0);
    free(audio);
}

static void test_chorus_fractional_block_boundaries(void) {
    /* Recovered block completion preserves the fractional step in SET16.
     * Fast motion retains its integer advance; slow motion subtracts its
     * fraction below one. The first three endpoints cross the ring boundary.
     */
    const ChorusScenario slow = {.initial_read = 150u * 65536u,
                                 .direction = -1,
                                 .positive_frame = 11,
                                 .negative_frame = 91,
                                 .block_reads = {68u * 65536u + 25088u,
                                                 146u * 65536u + 50176u,
                                                 65u * 65536u + 9728u}};
    const ChorusScenario fast = {.initial_read = 100u * 65536u,
                                 .direction = 1,
                                 .positive_frame = 60,
                                 .negative_frame = 138,
                                 .block_reads = {20u * 65536u + 40448u,
                                                 101u * 65536u + 15360u,
                                                 21u * 65536u + 55808u}};
    test_chorus_scenario(&slow);
    test_chorus_scenario(&fast);
}

int main(void) {
    test_mode_two_history_tail_and_clipping();
    test_chorus_fractional_block_boundaries();
    puts("Audio effect regressions passed.");
    return 0;
}
