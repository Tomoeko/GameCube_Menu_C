#include "audio/audio_internal.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static GcAudioEnvelope release_envelope(unsigned revision) {
    return (GcAudioEnvelope){
        .enabled = true,
        .null_release_quick = revision != 0,
        .scaled_duration = revision != 0,
        .rate = 0.25f,
        .scale = 0.75f,
        .offset = -0.125f,
        .attack_identity = GC_AUDIO_TABLE_SEQUENCE | 32,
        .release_identity = GC_AUDIO_TABLE_SEQUENCE | 48,
        .attack_count = 2,
        .release_count = 2,
        .attack = {{0, 0, 24576}, {14, 0, 0}},
        .release = {{0, 40, 0}, {15, 0, 0}},
    };
}

static float forced_length(unsigned revision) {
    float factor = (float)GC_AUDIO_DSP_RATE / 80.0f;
    factor /= 600.0f;
    return revision ? 5.0f * factor : 5.0f;
}

static void test_forced_release_table(unsigned revision) {
    GcAudioEnvelope envelope = release_envelope(revision);
    GcAudioEnvelopeState state;
    gc_audio_envelope_start(&state, &envelope);
    state.current = 0x1.766e4p-1f;
    state.target = 0.9f;
    state.step = 0.01f;
    state.remaining = 7;
    state.pc = 1;
    float length = forced_length(revision);
    float increment = -state.current / length;
    float expected = fmaf(-increment, length, 0);
    assert(gc_audio_envelope_quick_release(&state));
    assert(!gc_audio_envelope_release(&state));
    assert(gc_audio_envelope_step(&state) ==
           fmaf(expected, envelope.scale, envelope.offset));
    assert(state.current == expected && state.remaining == length);
    assert(state.step == increment && state.forced_release && state.quick_release);
    assert(state.pc == 1 && !state.quick_pending && !state.ended);
    assert(!gc_audio_envelope_quick_release(&state));
    assert(!gc_audio_envelope_release(&state));
    unsigned updates = 1;
    while (!state.ended && updates < 10) {
        gc_audio_envelope_step(&state);
        ++updates;
    }
    assert(state.ended && updates == (revision ? 5u : 6u));
    assert(!gc_audio_envelope_release(&state));
}

static void test_forced_shared_identity(unsigned revision) {
    GcAudioEnvelope envelope = release_envelope(revision);
    envelope.release_identity = envelope.attack_identity;
    envelope.release_continues = true;
    GcAudioEnvelopeState state;
    gc_audio_envelope_start(&state, &envelope);
    state.current = 0.8f;
    state.target = 0.6f;
    state.step = (state.target - state.current) / 8;
    state.remaining = state.length = 8;
    state.pc = 1;
    float expected;
    if (revision) {
        float length = forced_length(revision);
        expected = fmaf(-(-state.current / length), length, 0);
    } else {
        expected = fmaf(-state.step, 7, state.target);
    }
    assert(gc_audio_envelope_quick_release(&state));
    assert(gc_audio_envelope_step(&state) ==
           fmaf(expected, envelope.scale, envelope.offset));
    assert(state.remaining == (revision ? forced_length(revision) : 7));
    assert(state.target == (revision ? 0 : 0.6f));
}

static void test_normal_release_restart(unsigned revision) {
    GcAudioEnvelope envelope = release_envelope(revision);
    GcAudioEnvelopeState state;
    gc_audio_envelope_start(&state, &envelope);
    gc_audio_envelope_step(&state);
    assert(gc_audio_envelope_release(&state));
    gc_audio_envelope_step(&state);
    float length = state.length;
    for (unsigned update = 0; update < 5; ++update)
        gc_audio_envelope_step(&state);
    float current = state.current;
    assert(state.released && state.remaining < length);
    assert(gc_audio_envelope_release(&state));
    gc_audio_envelope_step(&state);
    assert(state.remaining == length);
    assert(state.step == -current / length);
}

static void test_custom_release_restart_and_force(void) {
    GcAudioEnvelope envelope = release_envelope(1);
    envelope.release_identity = 0;
    envelope.release_count = 0;
    GcAudioEnvelopeState state;
    gc_audio_envelope_start(&state, &envelope);
    gc_audio_envelope_step(&state);
    assert(gc_audio_envelope_release(&state));
    gc_audio_envelope_step(&state);
    gc_audio_envelope_step(&state);
    assert(state.quick_release && !state.forced_release && !state.ended);
    float custom_length = state.length;
    assert(state.remaining < custom_length);
    /* Native EUR keeps its custom duration after a release pointer appears.
     * A repeated normal callback rebuilds that duration; E9 selects five.
     */
    state.envelope.release_identity = GC_AUDIO_TABLE_SEQUENCE | 48;
    state.envelope.release_count = 2;
    assert(gc_audio_envelope_release(&state));
    gc_audio_envelope_step(&state);
    assert(state.length == custom_length);
    assert(state.remaining == custom_length - envelope.rate);
    assert(gc_audio_envelope_quick_release(&state));
    gc_audio_envelope_step(&state);
    assert(state.forced_release && state.length == forced_length(1));
    assert(!gc_audio_envelope_release(&state));
}

static GcAudio *start_sequence(unsigned revision, uint8_t *sequence, size_t size) {
    static int16_t samples[32];
    GcAudio *audio = calloc(1, sizeof(*audio));
    assert(audio);
    atomic_init(&audio->mono, false);
    atomic_init(&audio->event_read, 0);
    atomic_init(&audio->event_write, 0);
    atomic_init(&audio->dropped_events, 0);
    audio->sequence_revision = revision;
    audio->sequence = sequence;
    audio->sequence_size = size;
    audio->wave_count = 1;
    audio->waves[0] = (GcAudioWave){.samples = samples,
                                    .count = 32,
                                    .rate = 32000,
                                    .key = 60,
                                    .loop = true,
                                    .loop_end = 32};
    audio->semitone_ratios[60] = 1;
    audio->fractional_semitone_ratios[0] = 1;
    audio->instruments[0] = (GcAudioInstrument){
        .region_count = 1,
        .volume = 1,
        .pitch = 1,
        .regions = {{.key = 127, .velocity = 127, .volume = 1, .pitch = 1}},
    };
    audio->instruments[0].envelopes[0] = release_envelope(revision);
    audio->instruments[0].envelopes[0].rate = 1;
    audio->instruments[0].envelopes[0].scale = 1;
    audio->instruments[0].envelopes[0].offset = 0;
    gc_audio_sequence_init(audio);
    return audio;
}

static void test_release_list_selection(unsigned revision) {
    uint8_t sequence[] = {60, 1,    127,  60, 2,    127,  0x80, 1,    0x81, 0x80,
                          1,  0xe9, 0x80, 1,  0xe8, 0x80, 1,    0x82, 0x80, 255};
    GcAudio *audio = start_sequence(revision, sequence, sizeof(sequence));
    GcAudioVoice *first = &audio->voices[0];
    GcAudioVoice *second = &audio->voices[1];
    gc_audio_sequence_tick(audio);
    assert(first->on_release_list && !second->on_release_list);
    gc_audio_sequence_envelopes(audio, first);
    gc_audio_sequence_tick(audio);
    assert(first->envelopes[0].quick_pending);
    assert(!second->envelopes[0].quick_pending && !second->released);
    gc_audio_sequence_envelopes(audio, first);
    gc_audio_sequence_tick(audio);
    assert(second->on_release_list);
    if (revision)
        assert(second->envelopes[0].quick_pending);
    else
        assert(second->envelopes[0].release_pending);
    assert(first->envelopes[0].forced_release);
    gc_audio_sequence_envelopes(audio, second);
    gc_audio_sequence_envelopes(audio, second);
    float remaining = second->envelopes[0].remaining;
    gc_audio_sequence_tick(audio);
    assert(second->envelopes[0].release_pending == (revision == 0));
    gc_audio_sequence_envelopes(audio, second);
    if (revision)
        assert(second->envelopes[0].remaining < remaining);
    else
        assert(second->envelopes[0].remaining > remaining);
    assert(audio->tracks[0].note_voices[1] == GC_AUDIO_VOICES);
    assert(audio->tracks[0].note_voices[2] == GC_AUDIO_VOICES);
    assert(audio->rejected_commands == 0);
    free(audio);
}

static void test_migrated_release_owner(unsigned revision) {
    uint8_t sequence[39] = {
        0xc1, 0, 0, 0, 32, 0x80, 1, 0xda, 0, 0xe9, 0x80, 255,
    };
    const uint8_t child[] = {60, 1, 127, 0x80, 255};
    for (unsigned index = 0; index < sizeof(child); ++index)
        sequence[32 + index] = child[index];
    GcAudio *audio = start_sequence(revision, sequence, sizeof(sequence));
    GcAudioVoice *voice = &audio->voices[0];
    unsigned source = voice->track;
    assert(source != 0 && voice->owner_track == source);
    gc_audio_sequence_tick(audio);
    assert(voice->track == source && voice->owner_track == 0);
    assert(voice->detached && voice->on_release_list);
    assert(voice->envelopes[0].quick_pending);
    gc_audio_sequence_envelopes(audio, voice);
    assert(voice->envelopes[0].forced_release);
    free(audio);
}

int main(void) {
    test_custom_release_restart_and_force();
    for (unsigned revision = 0; revision < 2; ++revision) {
        test_forced_release_table(revision);
        test_forced_shared_identity(revision);
        test_normal_release_restart(revision);
        test_release_list_selection(revision);
        test_migrated_release_owner(revision);
    }
    puts("Regional audio release tests passed.");
    return 0;
}
