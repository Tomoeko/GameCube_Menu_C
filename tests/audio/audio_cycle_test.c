#include "audio/audio_internal.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static GcAudio *start_sequence(unsigned revision, uint8_t *sequence, size_t size) {
    static int16_t samples[32];
    GcAudio *audio = calloc(1, sizeof(*audio));
    assert(audio);
    atomic_init(&audio->mono, false);
    atomic_init(&audio->event_read, 0);
    atomic_init(&audio->event_write, 0);
    audio->sequence_revision = revision;
    audio->sequence = sequence;
    audio->sequence_size = size;
    audio->wave_count = 1;
    audio->waves[0] = (GcAudioWave){.samples = samples,
                                    .count = 32,
                                    .loop = true,
                                    .loop_end = 32,
                                    .rate = 32000,
                                    .key = 60};
    audio->instruments[0] = (GcAudioInstrument){
        .region_count = 1,
        .volume = 1,
        .pitch = 1,
        .regions = {{.key = 127, .velocity = 127, .volume = 1, .pitch = 1}},
    };
    gc_audio_sequence_init(audio);
    return audio;
}

static GcAudio *start_cycle_note(unsigned revision, unsigned mode) {
    static uint8_t usa[3][7] = {
        {0xd6, 0, 60, 1, 127, 0x80, 255},
        {0xd6, 1, 60, 1, 127, 0x80, 255},
        {0xd6, 2, 60, 1, 127, 0x80, 255},
    };
    static uint8_t eur[3][9] = {
        {0xd6, 0, 0xf0, 0x11, 60, 1, 127, 0x80, 255},
        {0xd6, 1, 0xf0, 0x00, 60, 1, 127, 0x80, 255},
        {0xd6, 2, 0xf0, 0x11, 60, 1, 127, 0x80, 255},
    };
    assert(mode < 3);
    GcAudio *audio = revision ? start_sequence(revision, eur[mode], sizeof(eur[mode]))
                              : start_sequence(revision, usa[mode], sizeof(usa[mode]));
    assert(audio->notes_started == 1 && audio->rejected_commands == 0);
    return audio;
}

static void test_cycle_release_continuity(const GcAudioEnvelope *envelope) {
    for (unsigned phase = 1; phase < 160; ++phase) {
        GcAudioEnvelopeState uninterrupted;
        gc_audio_envelope_start(&uninterrupted, envelope);
        for (unsigned tick = 0; tick < phase; ++tick)
            (void)gc_audio_envelope_step(&uninterrupted);
        GcAudioEnvelopeState released = uninterrupted;
        gc_audio_envelope_release(&released);
        assert(released.released && released.pc == uninterrupted.pc);
        assert(released.remaining == uninterrupted.remaining);
        assert(released.target == uninterrupted.target);
        assert(released.current == uninterrupted.current);
        gc_audio_envelope_release(&released);
        for (unsigned tick = 0; tick < 160; ++tick) {
            float expected = gc_audio_envelope_step(&uninterrupted);
            assert(gc_audio_envelope_step(&released) == expected);
            assert(released.pc == uninterrupted.pc);
            assert(released.remaining == uninterrupted.remaining);
            assert(released.current == uninterrupted.current);
            assert(!released.ended);
        }
    }
}

static void test_cycle_dispatch(unsigned revision, unsigned mode) {
    GcAudio *audio = start_cycle_note(revision, mode);
    unsigned slot = revision && mode == 1 ? 0 : 1;
    const GcAudioEnvelope *envelope = &audio->tracks[0].envelopes[slot];
    const GcAudioVoice *voice = &audio->voices[0];
    assert(envelope->enabled && envelope->release_continues);
    assert(!audio->tracks[0].envelopes[1 - slot].enabled);
    assert(envelope->target == (mode ? 0u : 1u));
    assert(envelope->rate == (revision && !mode ? 0.8f : 1));
    assert(envelope->scale == (revision ? 0 : 1));
    assert(envelope->offset == 1);
    assert(voice->active && voice->envelopes[slot].envelope.release_continues);
    assert(voice->envelopes[slot].pc == 2);
    if (revision) {
        assert(voice->envelope_volume == 1 && voice->envelope_pitch == 1);
    } else if (mode) {
        assert(voice->envelope_volume == 1 + 32767.0f / 32768);
        assert(voice->envelope_pitch == 1);
    } else {
        assert(voice->envelope_pitch == 1 && voice->envelope_volume == 1);
    }
    test_cycle_release_continuity(envelope);
    free(audio);
}

static void test_distinct_release_table(void) {
    GcAudioEnvelope envelope = {.enabled = true,
                                .rate = 1,
                                .scale = 1,
                                .attack_count = 2,
                                .attack = {{0, 20, 32767}, {14, 0, 0}},
                                .release_count = 2,
                                .release = {{0, 10, 0}, {15, 1, 0}}};
    GcAudioEnvelopeState state;
    gc_audio_envelope_start(&state, &envelope);
    for (unsigned tick = 0; tick < 7; ++tick)
        (void)gc_audio_envelope_step(&state);
    float current = state.current;
    gc_audio_envelope_release(&state);
    assert(state.pc == 0 && state.remaining == 0 && state.target == current);
    assert(gc_audio_envelope_step(&state) == current);
    for (unsigned tick = 0; tick < 10; ++tick)
        (void)gc_audio_envelope_step(&state);
    assert(state.ended);
}

static GcAudio *start_table_program(unsigned revision, uint8_t *sequence, size_t size) {
    static uint8_t wait[] = {0x80, 255};
    GcAudio *audio = start_sequence(revision, wait, sizeof(wait));
    audio->sequence = sequence;
    audio->sequence_size = size;
    audio->tracks[0].pc = 48;
    audio->tracks[0].wait = 0;
    gc_audio_sequence_tick(audio);
    assert(audio->notes_started == 1 && audio->rejected_commands == 0);
    return audio;
}

static void test_cycle_release_replacement(unsigned revision) {
    const uint8_t release[] = {0, 0, 0, 4, 0, 0, 0, 15, 0, 1, 0, 0};
    const uint8_t usa[] = {0xd6, 1, 0xd7, 1, 0, 0, 0, 60, 1, 127, 0x80, 255};
    const uint8_t eur[] = {0xd6, 1, 0xd7, 1, 0, 0, 0, 0xf0, 0, 60, 1, 127, 0x80, 255};
    uint8_t sequence[96] = {0};
    memcpy(sequence, release, sizeof(release));
    if (revision)
        memcpy(sequence + 48, eur, sizeof(eur));
    else
        memcpy(sequence + 48, usa, sizeof(usa));
    GcAudio *audio = start_table_program(revision, sequence, sizeof(sequence));
    const GcAudioEnvelope *envelope = &audio->tracks[0].envelopes[0];
    assert(!envelope->release_continues);
    assert(envelope->release_count == 2 && envelope->release[0].ticks == 4);
    if (revision) {
        GcAudioVoice *voice = &audio->voices[0];
        for (unsigned tick = 0; tick < 5; ++tick)
            gc_audio_voice_envelopes(voice);
        gc_audio_voice_release(voice);
        assert(voice->envelopes[0].pc == 0);
        for (unsigned tick = 0; tick < 3; ++tick) {
            gc_audio_voice_envelopes(voice);
            assert(voice->active);
        }
        gc_audio_voice_envelopes(voice);
        assert(!voice->active);
    } else {
        assert(audio->tracks[0].envelopes[1].release_continues);
    }
    free(audio);
}

static void test_sequence_table_identity(unsigned revision, unsigned attack_offset,
                                         unsigned release_offset) {
    const uint8_t cycle[] = {0, 0,    0, 8, 0x7f, 0xff, 0, 0, 0,
                             8, 0x80, 1, 0, 13,   0,    0, 0, 0};
    const uint8_t program[] = {0xd6,
                               1,
                               0xd7,
                               0,
                               0,
                               0,
                               (uint8_t)attack_offset,
                               0xd7,
                               1,
                               0,
                               0,
                               (uint8_t)release_offset,
                               60,
                               1,
                               127,
                               0x80,
                               255};
    uint8_t sequence[96] = {0};
    memcpy(sequence, cycle, sizeof(cycle));
    memcpy(sequence + 24, cycle, sizeof(cycle));
    memcpy(sequence + 48, program, sizeof(program));
    GcAudio *audio = start_table_program(revision, sequence, sizeof(sequence));
    const GcAudioEnvelope *envelope = &audio->tracks[0].envelopes[0];
    assert(envelope->attack_count == 3 && envelope->release_count == 3);
    assert(envelope->release_continues == (attack_offset == release_offset));
    if (attack_offset == release_offset) {
        test_cycle_release_continuity(envelope);
    } else {
        GcAudioEnvelopeState state;
        gc_audio_envelope_start(&state, envelope);
        for (unsigned tick = 0; tick < 5; ++tick)
            (void)gc_audio_envelope_step(&state);
        float current = state.current;
        gc_audio_envelope_release(&state);
        assert(state.pc == 0 && state.remaining == 0 && state.target == current);
        assert(gc_audio_envelope_step(&state) == current);
    }
    GcAudioEnvelopeStep ignored[GC_AUDIO_ENVELOPE_STEPS];
    unsigned count;
    assert(
        gc_audio_envelope_decode_steps(ignored, &count, sequence, sizeof(sequence), 0));
    assert(count == 0);
    free(audio);
}

static void test_sequence_table_bounds(void) {
    const uint8_t terminated[] = {0, 15, 0, 1, 0, 0};
    const uint8_t unterminated[] = {0, 0, 0, 8, 0x7f, 0xff};
    GcAudioEnvelopeStep steps[GC_AUDIO_ENVELOPE_STEPS];
    unsigned count;
    assert(gc_audio_envelope_decode_sequence_steps(steps, &count, terminated,
                                                   sizeof(terminated), 0));
    assert(count == 1 && steps[0].curve == 15);
    assert(!gc_audio_envelope_decode_sequence_steps(steps, &count, terminated,
                                                    sizeof(terminated) - 1, 0));
    assert(count == 0);
    assert(!gc_audio_envelope_decode_sequence_steps(steps, &count, terminated,
                                                    sizeof(terminated), SIZE_MAX));
    assert(count == 0);
    assert(!gc_audio_envelope_decode_sequence_steps(steps, &count, unterminated,
                                                    sizeof(unterminated), 0));
    assert(count == 1);
}

static void test_null_tables(unsigned revision) {
    GcAudioEnvelope envelope = {.enabled = true,
                                .null_release_quick = revision != 0,
                                .scaled_duration = revision != 0,
                                .release_continues = true,
                                .rate = 0x1.d4cp-1f,
                                .scale = 0x1.77p-4f};
    GcAudioEnvelopeState state;
    gc_audio_envelope_start(&state, &envelope);
    for (unsigned tick = 0; tick < 20; ++tick)
        assert(gc_audio_envelope_step(&state) == 1);
    gc_audio_envelope_release(&state);
    for (unsigned tick = 0; tick < 11; ++tick) {
        float value = gc_audio_envelope_step(&state);
        assert(!state.ended);
        if (revision)
            assert(value > 0 && value < envelope.scale);
        else
            assert(value == 1);
    }
    float final = gc_audio_envelope_step(&state);
    assert(state.ended == (revision != 0));
    assert(final == (revision ? 0 : 1));
    if (revision)
        assert(state.length == 0x1.55a328p+3f);
}

static void test_menu_oscillator_release(void) {
    uint8_t sequence[] = {
        0xf0, 0x00, 0xd6, 1,    0xf0, 0x11, 0x9c, 9, 0x0b, 0xb8, 0x9c, 10, 0x75, 0x30,
        60,   1,    127,  0x80, 1,    0x81, 0x80, 1, 0xc8, 0,    0,    0,  14,
    };
    GcAudio *audio = start_sequence(1, sequence, sizeof(sequence));
    GcAudioVoice *voice = &audio->voices[0];
    assert(voice->active && voice->envelope_volume == 1);
    assert(voice->envelopes[0].envelope.release_continues);
    assert(voice->envelopes[1].envelope.enabled);
    assert(voice->envelopes[1].envelope.attack_count == 0);
    assert(voice->envelopes[1].envelope.release_count == 0);
    assert(voice->envelope_tracks[0] == 0 && voice->envelope_tracks[1] == 1);
    gc_audio_sequence_tick(audio);
    assert(voice->released);
    for (unsigned tick = 0; tick < 11; ++tick) {
        gc_audio_sequence_envelopes(audio, voice);
        assert(voice->active);
    }
    gc_audio_sequence_envelopes(audio, voice);
    assert(!voice->active && !voice->envelopes[0].ended);

    /* A continuing loop must keep allocating and retiring notes instead of
     * exhausting the physical channels with released sample loops.
     */
    for (unsigned tick = 0; tick < 512; ++tick) {
        gc_audio_sequence_tick(audio);
        unsigned active = 0;
        for (unsigned slot = 0; slot < GC_AUDIO_VOICES; ++slot) {
            if (audio->voices[slot].active) {
                gc_audio_sequence_envelopes(audio, &audio->voices[slot]);
                active += audio->voices[slot].active;
            }
        }
        assert(active <= 7);
    }
    assert(audio->notes_started > 200 && audio->rejected_commands == 0);
    free(audio);
}

static void test_cycle_without_installation(void) {
    uint8_t sequence[] = {0xd6, 1, 60, 1, 127, 0x80, 255};
    GcAudio *audio = start_sequence(1, sequence, sizeof(sequence));
    assert(audio->tracks[0].envelope_modes[0] == 15);
    assert(audio->tracks[0].envelope_modes[1] == 15);
    assert(!audio->voices[0].envelopes[0].envelope.enabled);
    assert(!audio->voices[0].envelopes[1].envelope.enabled);
    free(audio);
}

static void test_extra_oscillator_slot(unsigned slot) {
    uint8_t sequence[] = {0xd6, 1, 0xf0, (uint8_t)slot, 60, 1, 127, 0x80, 255};
    GcAudio *audio = start_sequence(1, sequence, sizeof(sequence));
    assert(slot >= 2 && slot < GC_AUDIO_OSCILLATORS);
    assert(audio->voices[0].envelopes[slot].envelope.release_continues);
    assert(audio->voices[0].envelope_tracks[slot] == 0);
    assert(!audio->voices[0].envelopes[0].envelope.enabled);
    free(audio);
}

static void test_invalid_oscillator_installation(unsigned operand) {
    uint8_t sequence[] = {0xf0, (uint8_t)operand, 60, 1, 127, 0x80, 255};
    GcAudio *audio = start_sequence(1, sequence, sizeof(sequence));
    assert(!audio->tracks[0].active);
    assert(audio->notes_started == 0 && audio->rejected_commands == 1);
    free(audio);
}

static void test_inherited_oscillator(unsigned mode) {
    uint8_t wait[] = {0x80, 255};
    uint8_t sequence[] = {0xd6, 1, 0xf0, (uint8_t)mode, 60, 1, 127, 0x80, 255};
    GcAudio *audio = start_sequence(1, wait, sizeof(wait));
    for (unsigned i = 0; i < 2; ++i) {
        audio->instruments[0].envelopes[i] = (GcAudioEnvelope){
            .enabled = true,
            .rate = 1,
            .scale = 1,
            .attack_identity = GC_AUDIO_TABLE_BANK | 32,
            .release_identity = GC_AUDIO_TABLE_BANK | 64,
            .attack_count = 2,
            .attack = {{0, 0, 16384}, {14, 0, 0}},
            .release_count = 2,
            .release = {{0, 4, 0}, {15, 1, 0}},
        };
    }
    audio->sequence = sequence;
    audio->sequence_size = sizeof(sequence);
    audio->tracks[0].pc = 0;
    audio->tracks[0].wait = 0;
    gc_audio_sequence_tick(audio);
    assert(audio->notes_started == 1 && audio->rejected_commands == 0);
    const GcAudioEnvelope *track = &audio->tracks[0].envelopes[0];
    const GcAudioVoice *voice = &audio->voices[0];
    unsigned slot = mode & 3;
    assert(voice->envelope_tracks[slot] == 0);
    if (slot < 2) {
        assert(track->attack_identity == (GC_AUDIO_TABLE_BANK | 32));
        assert(track->attack_count == 2);
        assert(!track->release_continues);
        if (mode < 8) {
            assert(track->release_identity == (GC_AUDIO_TABLE_TEMPLATE | 3));
            assert(track->release_count == 6);
        } else {
            assert(track->release_identity == (GC_AUDIO_TABLE_BANK | 64));
            assert(track->release_count == 2);
        }
        assert(voice->envelope_volume == 0.125f);
    } else {
        assert(track->release_continues);
        assert(track->attack_identity == (GC_AUDIO_TABLE_TEMPLATE | 3));
        assert(voice->envelope_volume == 0.25f);
    }
    free(audio);
}

static void test_program_resets_oscillator_installation(void) {
    uint8_t sequence[] = {0xf0, 0x00, 0xac, 6, 0, 0, 0xd6, 1, 60, 1, 127, 0x80, 255};
    GcAudio *audio = start_sequence(1, sequence, sizeof(sequence));
    assert(audio->tracks[0].envelope_modes[0] == 15);
    assert(!audio->voices[0].envelopes[0].envelope.enabled);
    free(audio);
}

int main(void) {
    test_cycle_dispatch(0, 0);
    test_cycle_dispatch(0, 1);
    test_cycle_dispatch(1, 0);
    test_cycle_dispatch(1, 1);
    test_cycle_dispatch(1, 2);
    test_distinct_release_table();
    test_sequence_table_bounds();
    test_null_tables(0);
    test_null_tables(1);
    test_menu_oscillator_release();
    test_cycle_without_installation();
    test_extra_oscillator_slot(2);
    test_extra_oscillator_slot(3);
    test_invalid_oscillator_installation(0x20);
    test_invalid_oscillator_installation(0x0c);
    test_invalid_oscillator_installation(0x0d);
    test_invalid_oscillator_installation(0x0e);
    for (unsigned mode = 4; mode < 12; ++mode)
        test_inherited_oscillator(mode);
    test_program_resets_oscillator_installation();
    for (unsigned revision = 0; revision < 2; ++revision) {
        test_cycle_release_replacement(revision);
        test_sequence_table_identity(revision, 0, 0);
        test_sequence_table_identity(revision, 24, 24);
        test_sequence_table_identity(revision, 0, 24);
    }
    puts("audio cycle tests passed");
    return 0;
}
