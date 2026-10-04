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
    atomic_init(&audio->menu_volume_adjustment, 0);
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
    audio->semitone_ratios[60] = 1;
    audio->fractional_semitone_ratios[0] = 1;
    audio->instruments[0] = (GcAudioInstrument){
        .region_count = 1,
        .volume = 1,
        .pitch = 1,
        .regions = {{.key = 127, .velocity = 127, .volume = 1, .pitch = 1}},
    };
    gc_audio_sequence_init(audio);
    assert(audio->rejected_commands == 0);
    return audio;
}

static void test_signed_register_target(unsigned revision) {
    const uint16_t encoded[] = {0x7fff, 0x8000, 0xffff};
    const float expected[] = {0x1.fffcp-1f, -1, -0x1p-15f};
    for (unsigned index = 0; index < 3; ++index) {
        /* Load a word register, use it as a parameter target, then wait. */
        uint8_t sequence[] = {0xac,
                              4,
                              (uint8_t)(encoded[index] >> 8),
                              (uint8_t)encoded[index],
                              0x90,
                              1,
                              4,
                              0x80,
                              4};
        GcAudio *audio = start_sequence(revision, sequence, sizeof(sequence));
        assert(audio->tracks[0].parameters[1] == expected[index]);
        gc_audio_sequence_tick(audio);
        assert(audio->tracks[0].parameters[1] == expected[index]);
        free(audio);
    }
}

static void test_signed_register_multiply(unsigned revision) {
    const uint16_t operands[][2] = {
        {0x7fff, 0xffff}, {0xffff, 0x7fff}, {0x8000, 0xffff}, {0xffff, 0x8000},
        {0x8000, 0x8000}, {0xffff, 0xffff}, {0x7fff, 0x7fff}, {0, 0x8000},
    };
    const uint32_t products[] = {
        0xffff8001, 0xffff8001, 0x00008000, 0x00008000,
        0x40000000, 0x00000001, 0x3fff0001, 0,
    };
    for (unsigned index = 0; index < sizeof(products) / sizeof(products[0]); ++index) {
        uint8_t sequence[] = {
            0xac,
            14,
            (uint8_t)(operands[index][0] >> 8),
            (uint8_t)operands[index][0],
            0xac,
            15,
            (uint8_t)(operands[index][1] >> 8),
            (uint8_t)operands[index][1],
            0xa2,
            14,
            15,
            0x80,
            4,
        };
        GcAudio *audio = start_sequence(revision, sequence, sizeof(sequence));
        assert(audio->tracks[0].registers[4] == products[index] >> 16);
        assert(audio->tracks[0].registers[5] == (uint16_t)products[index]);
        free(audio);
    }
}

static void test_parent_child_pan_and_echo(unsigned revision) {
    /* The native defaults give oscillator and track pan equal weight, then
     * blend child and parent parameters using register twelve. This also
     * enables the track's echo send instead of silently discarding it.
     */
    uint8_t sequence[48] = {
        0x9c, 2, 0x60, 0, 0x9c, 3, 0x60, 0, 0xc1, 0, 0, 0, 32, 0x80, 255,
    };
    const uint8_t child[] = {
        0x9c, 2, 0x20, 0, 0x9c, 3, 0x20, 0, 60, 1, 127, 0x80, 255,
    };
    for (unsigned index = 0; index < sizeof(child); ++index)
        sequence[32 + index] = child[index];
    GcAudio *audio = start_sequence(revision, sequence, sizeof(sequence));
    assert(audio->notes_started == 1 && audio->voices[0].active);
    assert(audio->voices[0].track != 0);
    const GcAudioTrack *track = &audio->tracks[audio->voices[0].track];
    assert(track->registers[8] == 0);
    assert(track->registers[9] == 1 && track->registers[10] == 1);
    assert(track->registers[11] == 0x7fff && track->registers[12] == 0x4000);
    assert(audio->voices[0].pan == 0x1.00008p-1f);
    assert(audio->voices[0].reverb == 0x1.0001p-2f);
    free(audio);
}

static void test_regional_register_defaults(unsigned revision) {
    uint8_t defaults[] = {0x80, 255};
    GcAudio *audio = start_sequence(revision, defaults, sizeof(defaults));
    assert(audio->tracks[0].registers[13] == (revision ? 0x40 : 0));
    free(audio);

    uint8_t inheritance[24] = {
        0xac, 13, 0, 0x7b, 0xc1, 0, 0, 0, 16, 0x80, 255,
    };
    inheritance[16] = 0x80;
    inheritance[17] = 255;
    audio = start_sequence(revision, inheritance, sizeof(inheritance));
    unsigned child = audio->tracks[0].children[0];
    assert(child < GC_AUDIO_TRACKS);
    assert(audio->tracks[child].registers[13] == (revision ? 0x7b : 0));
    free(audio);

    inheritance[5] = 0x80;
    audio = start_sequence(revision, inheritance, sizeof(inheritance));
    child = audio->tracks[0].children[0];
    assert(child < GC_AUDIO_TRACKS);
    assert(audio->tracks[child].registers[13] == (revision ? 0x40 : 0));
    free(audio);
}

static void test_regional_parent_blend(unsigned revision) {
    uint8_t sequence[48] = {
        0x9c, 2, 0x2e, 0xe0, 0xac, 12, 0, 3, 0xc1, 0, 0, 0, 32, 0x80, 255,
    };
    const uint8_t child[] = {0x9c, 2, 0, 1, 60, 1, 127, 0x80, 255};
    for (unsigned index = 0; index < sizeof(child); ++index)
        sequence[32 + index] = child[index];
    GcAudio *audio = start_sequence(revision, sequence, sizeof(sequence));
    assert(audio->notes_started == 1);
    assert(audio->voices[0].reverb == (revision ? 0x1.0c9e18p-15f : 0x1.0c9e1ap-15f));
    free(audio);
}

static void test_cube_volume_single_rounding(unsigned revision) {
    uint8_t sequence[] = {0x80, 255};
    GcAudio *audio = start_sequence(revision, sequence, sizeof(sequence));
    audio->tracks[0].id = 0x21002;
    gc_audio_sequence_cube(audio, 0, 0x1.288484p-7f);
    assert(audio->tracks[0].ports[1] == 29649);
    gc_audio_sequence_cube(audio, 1, 0x1.288484p-7f);
    assert(audio->tracks[0].ports[1] == 0);
    free(audio);
}

static void test_three_level_pitch(unsigned revision) {
    uint8_t idle[] = {0x80, 255};
    GcAudio *audio = start_sequence(revision, idle, sizeof(idle));
    /* Authored lookup values expose the rounding difference between a
     * child's inherited parent ratio and a flattened left-associative product.
     */
    audio->semitone_ratios[66] = 0.9f;
    audio->semitone_ratios[72] = 0.1f;
    audio->semitone_ratios[84] = 0.1f;
    uint8_t sequence[80] = {
        0x9c, 1, 0x10, 0, 0xc1, 0, 0, 0, 32, 0x80, 255,
    };
    const uint8_t parent[] = {
        0x9c, 1, 0x20, 0, 0xc1, 0, 0, 0, 64, 0x80, 255,
    };
    const uint8_t child[] = {0x9c, 1, 0x40, 0, 60, 1, 127, 0x80, 255};
    for (unsigned index = 0; index < sizeof(parent); ++index)
        sequence[32 + index] = parent[index];
    for (unsigned index = 0; index < sizeof(child); ++index)
        sequence[64 + index] = child[index];
    audio->sequence = sequence;
    audio->sequence_size = sizeof(sequence);
    gc_audio_sequence_init(audio);
    assert(audio->notes_started == 1);
    assert(audio->voices[0].step == (float)audio->voices[0].base_step * 0x1.26e978p-7f);
    gc_audio_sequence_tick(audio);
    assert(audio->voices[0].step == (float)audio->voices[0].base_step * 0x1.26e978p-7f);
    free(audio);
}

static void test_null_release_guard(unsigned revision) {
    uint8_t idle[] = {0x80, 255};
    GcAudio *audio = start_sequence(revision, idle, sizeof(idle));
    audio->instruments[0].envelopes[0] = (GcAudioEnvelope){
        .enabled = true,
        .target = 0,
        .rate = 1,
        .scale = 1,
        .attack_identity = GC_AUDIO_TABLE_BANK | 64,
        .attack_count = 2,
        .attack = {{0, 20, 16384}, {14, 0, 0}},
    };
    uint8_t sequence[] = {60, 1, 127, 0x80, 1, 0x81, 0x80, 255};
    audio->sequence = sequence;
    audio->sequence_size = sizeof(sequence);
    gc_audio_sequence_init(audio);
    GcAudioVoice *voice = &audio->voices[0];
    for (unsigned update = 0; update < 5; ++update)
        gc_audio_sequence_envelopes(audio, voice);
    float current = voice->envelope_volume;
    gc_audio_sequence_tick(audio);
    assert(voice->released);
    assert(voice->envelopes[0].released == (revision != 0));
    gc_audio_sequence_envelopes(audio, voice);
    if (revision) {
        assert(voice->envelopes[0].quick_release);
        assert(voice->envelope_volume > 0 && voice->envelope_volume < current);
    } else {
        /* USA normal note-off leaves this null-release attack running. */
        assert(voice->envelope_volume == 0x1.333332p-3f);
        assert(voice->envelopes[0].remaining == 14);
        assert(!voice->envelopes[0].release_pending);
    }
    free(audio);
}

static void test_live_release_pointer_guard(bool present_at_callback) {
    uint8_t idle[] = {0x80, 255};
    GcAudio *audio = start_sequence(0, idle, sizeof(idle));
    uint8_t sequence[] = {60, 1, 127, 0x80, 1, 0x81, 0x80, 255};
    audio->sequence = sequence;
    audio->sequence_size = sizeof(sequence);
    audio->tracks[0].pc = 0;
    audio->tracks[0].wait = 0;
    GcAudioEnvelope *descriptor = &audio->tracks[0].envelopes[0];
    *descriptor = (GcAudioEnvelope){
        .enabled = true,
        .target = 0,
        .rate = 1,
        .scale = 1,
        .attack_identity = GC_AUDIO_TABLE_SEQUENCE | 64,
        .release_identity = present_at_callback ? 0 : GC_AUDIO_TABLE_SEQUENCE | 80,
        .attack_count = 2,
        .release_count = present_at_callback ? 0 : 2,
        .attack = {{0, 20, 16384}, {14, 0, 0}},
        .release = {{0, 0, 8192}, {14, 0, 0}},
    };
    gc_audio_sequence_tick(audio);
    GcAudioVoice *voice = &audio->voices[0];
    assert(audio->notes_started == 1 && voice->envelope_tracks[0] == 0);
    /* The descriptor changes before note-off while the physical snapshot
     * still has the opposite pointer. The callback must read the live one.
     */
    descriptor->release_identity =
        present_at_callback ? GC_AUDIO_TABLE_SEQUENCE | 80 : 0;
    descriptor->release_count = present_at_callback ? 2 : 0;
    gc_audio_sequence_tick(audio);
    assert(voice->released);
    assert(voice->envelopes[0].released == present_at_callback);
    assert(voice->envelopes[0].release_pending == present_at_callback);
    gc_audio_sequence_envelopes(audio, voice);
    assert(voice->envelope_volume == (present_at_callback ? 0.25f : 0x1.999992p-6f));
    free(audio);
}

static void test_gate_release_update_order(unsigned revision) {
    uint8_t idle[] = {0x80, 255};
    GcAudio *audio = start_sequence(revision, idle, sizeof(idle));
    audio->master_gain = 32767;
    audio->instruments[0].envelopes[0] = (GcAudioEnvelope){
        .enabled = true,
        .rate = 1,
        .scale = 1,
        .attack_identity = GC_AUDIO_TABLE_BANK | 64,
        .release_identity = GC_AUDIO_TABLE_BANK | 80,
        .attack_count = 2,
        .release_count = 2,
        .attack = {{0, 0, 24576}, {14, 0, 0}},
        .release = {{0, 0, 8192}, {14, 0, 0}},
    };
    uint8_t sequence[12] = {0xdd, 0, revision ? 1 : 0x10, 0};
    const uint8_t note[] = {60, 0x08, 127, 25, 1, 0x80, 255};
    unsigned offset = revision ? 4 : 3;
    for (unsigned index = 0; index < sizeof(note); ++index)
        sequence[offset + index] = note[index];
    audio->sequence = sequence;
    audio->sequence_size = sizeof(sequence);
    gc_audio_sequence_init(audio);
    GcAudioVoice *voice = &audio->voices[0];
    assert(audio->notes_started == 1 && voice->duration == 1);
    assert(voice->envelope_volume == 0.75f && !voice->released);
    float output[2];
    for (unsigned frame = 0; frame < (unsigned)GC_AUDIO_DSP_QUANTUM; ++frame)
        gc_audio_dsp_render_frame(audio, false, output);
    assert(!voice->released);
    gc_audio_dsp_render_frame(audio, false, output);
    /* The expiring block keeps its computed attack value in the DSP gain.
     * The release table starts on the following physical update.
     */
    assert(voice->released && voice->envelope_volume == 0.75f);
    assert(voice->dsp_gains[0].increment == 0);
    for (unsigned frame = 1; frame <= (unsigned)GC_AUDIO_DSP_QUANTUM; ++frame)
        gc_audio_dsp_render_frame(audio, false, output);
    assert(voice->envelope_volume == 0.25f);
    assert(voice->dsp_gains[0].increment < 0);
    free(audio);
}

static GcAudio *release_fixture_audio(unsigned revision) {
    uint8_t idle[] = {0x80, 255};
    GcAudio *audio = start_sequence(revision, idle, sizeof(idle));
    audio->sequence = NULL;
    audio->sequence_size = 0;
    audio->semitone_ratios[72] = 1.25f;
    audio->instruments[0].envelopes[0] = (GcAudioEnvelope){
        .enabled = true,
        .rate = 1,
        .scale = 1,
        .attack_identity = GC_AUDIO_TABLE_BANK | 64,
        .release_identity = GC_AUDIO_TABLE_BANK | 80,
        .attack_count = 2,
        .release_count = 2,
        .attack = {{0, 0, 24576}, {14, 0, 0}},
        .release = {{0, 200, 0}, {15, 0, 0}},
    };
    return audio;
}

static void test_attached_release_controls(unsigned revision) {
    GcAudio *audio = release_fixture_audio(revision);
    uint8_t sequence[64] = {0xc1, 0, 0, 0, 32, 0x80, 255};
    const uint8_t child[] = {
        60, 1, 127, 0x80, 1, 0x81, 0x9c, 0, 0x40, 0, 0x9c, 1, 0x20, 0, 0x80, 255,
    };
    for (unsigned index = 0; index < sizeof(child); ++index)
        sequence[32 + index] = child[index];
    audio->sequence = sequence;
    audio->sequence_size = sizeof(sequence);
    gc_audio_sequence_init(audio);
    gc_audio_sequence_tick(audio);
    const GcAudioVoice *voice = &audio->voices[0];
    assert(voice->released && !voice->detached);
    assert(voice->track_gain == 0.5f);
    assert(voice->step == (float)voice->base_step * 1.25f);
    free(audio);
}

static void test_detached_release_controls(unsigned revision) {
    GcAudio *audio = release_fixture_audio(revision);
    uint8_t sequence[88] = {
        0xc1, 0, 0, 0, 32, 0x80, 1, 0xc1, 0, 0, 0, 64, 0x80, 255,
    };
    const uint8_t original[] = {0x9c, 3, 0x20, 0, 60, 1, 127, 0x80, 255};
    const uint8_t replacement[] = {0x9c, 3, 0x60, 0, 0x80, 255};
    for (unsigned index = 0; index < sizeof(original); ++index)
        sequence[32 + index] = original[index];
    for (unsigned index = 0; index < sizeof(replacement); ++index)
        sequence[64 + index] = replacement[index];
    audio->sequence = sequence;
    audio->sequence_size = sizeof(sequence);
    gc_audio_sequence_init(audio);
    GcAudioVoice *voice = &audio->voices[0];
    float pan = voice->pan;
    unsigned track = voice->track;
    gc_audio_sequence_tick(audio);
    assert(voice->released && voice->detached);
    unsigned replacement_track = audio->tracks[0].children[0];
    assert(replacement_track < GC_AUDIO_TRACKS);
    assert(audio->tracks[replacement_track].active);
    assert((replacement_track == track) == (revision == 0));
    assert(voice->owner_track == 0);
    for (unsigned slot = 0; slot < GC_AUDIO_NOTE_SLOTS; ++slot)
        assert(audio->tracks[replacement_track].note_voices[slot] == GC_AUDIO_VOICES);
    gc_audio_sequence_envelopes(audio, voice);
    assert(voice->pan == pan);
    free(audio);
}

static void test_timed_note_handle_cleanup(unsigned revision) {
    GcAudio *audio = release_fixture_audio(revision);
    uint8_t sequence[] = {60, 0x08, 127, 100, 2, 0xff};
    audio->sequence = sequence;
    audio->sequence_size = sizeof(sequence);
    gc_audio_sequence_init(audio);
    GcAudioVoice *voice = &audio->voices[0];
    GcAudioTrack *track = &audio->tracks[0];
    assert(track->note_voices[0] == 0 && track->clear_note_on_wait);
    gc_audio_voice_release(audio, voice);
    gc_audio_sequence_envelopes(audio, voice);
    gc_audio_sequence_envelopes(audio, voice);
    float remaining = voice->envelopes[0].remaining;
    gc_audio_sequence_tick(audio);
    assert(track->note_voices[0] == 0 && track->wait == 1);
    gc_audio_sequence_tick(audio);
    assert(!track->active && track->note_voices[0] == GC_AUDIO_VOICES);
    assert(!voice->envelopes[0].release_pending);
    assert(voice->envelopes[0].remaining == remaining);
    assert(voice->detached && voice->owner_track == GC_AUDIO_TRACKS);
    free(audio);
}

static void test_indexed_note_replacement_ownership(unsigned revision) {
    GcAudio *audio = release_fixture_audio(revision);
    uint8_t sequence[] = {
        60, 1, 127, 0x80, 1, 60, 1, 127, 0x80, 1, 0x81, 0x80, 1, 0xe8, 0x80, 1, 0xff,
    };
    audio->sequence = sequence;
    audio->sequence_size = sizeof(sequence);
    gc_audio_sequence_init(audio);
    GcAudioVoice *first = &audio->voices[0];
    gc_audio_sequence_tick(audio);
    GcAudioVoice *second = &audio->voices[1];
    assert(audio->tracks[0].note_voices[1] == 1);
    assert(first->on_release_list == (revision != 0));
    assert(first->released == (revision != 0) && !second->on_release_list);
    assert(first->duration == UINT32_MAX);
    gc_audio_sequence_envelopes(audio, first);
    gc_audio_sequence_envelopes(audio, first);
    float remaining = first->envelopes[0].remaining;
    gc_audio_sequence_tick(audio);
    assert(audio->tracks[0].note_voices[1] == GC_AUDIO_VOICES);
    assert(second->envelopes[0].release_pending);
    assert(!first->envelopes[0].release_pending);
    assert(first->envelopes[0].remaining == remaining);
    gc_audio_sequence_envelopes(audio, second);
    gc_audio_sequence_tick(audio);
    /* USA's overwritten voice remains active until the owner-wide E8.
     * EUR E8 also forces both voices already in the release list.
     */
    assert(first->on_release_list);
    assert(first->envelopes[0].release_pending == (revision == 0));
    assert(first->envelopes[0].quick_pending == (revision != 0));
    gc_audio_sequence_envelopes(audio, first);
    gc_audio_sequence_envelopes(audio, second);
    gc_audio_sequence_tick(audio);
    assert(!first->envelopes[0].release_pending);
    assert(!second->envelopes[0].release_pending);
    assert(first->detached && second->detached);
    free(audio);
}

static void test_replacement_without_free_voice(unsigned revision) {
    GcAudio *audio = release_fixture_audio(revision);
    uint8_t sequence[] = {60, 1, 127, 0x80, 1, 60, 1, 127, 0x80, 255};
    audio->sequence = sequence;
    audio->sequence_size = sizeof(sequence);
    gc_audio_sequence_init(audio);
    GcAudioVoice *first = &audio->voices[0];
    /* Exhaust the portable voice pool to exercise its bounded failure path.
     * The replaced logical handle must not retain the old playing voice.
     */
    for (unsigned index = 1; index < GC_AUDIO_VOICES; ++index) {
        audio->voices[index] = (GcAudioVoice){
            .active = true,
            .track = 0,
            .owner_track = 0,
            .slot = 7,
            .duration = UINT32_MAX,
        };
    }
    gc_audio_sequence_tick(audio);
    assert(audio->notes_started == 1 && audio->rejected_commands == 1);
    assert(audio->tracks[0].note_voices[1] == GC_AUDIO_VOICES);
    assert(first->active && first->released == (revision != 0));
    assert(first->on_release_list == (revision != 0));
    free(audio);
}

static void test_repeated_null_release_callback(void) {
    GcAudio *audio = release_fixture_audio(0);
    uint8_t sequence[] = {60, 1, 127, 0x80, 1, 0x81, 0x80, 255};
    audio->sequence = sequence;
    audio->sequence_size = sizeof(sequence);
    audio->tracks[0].pc = 0;
    audio->tracks[0].wait = 0;
    GcAudioEnvelope *descriptor = &audio->tracks[0].envelopes[0];
    *descriptor = audio->instruments[0].envelopes[0];
    descriptor->release_identity = 0;
    descriptor->release_count = 0;
    gc_audio_sequence_tick(audio);
    GcAudioVoice *voice = &audio->voices[0];
    assert(voice->envelope_tracks[0] == 0);
    gc_audio_voice_release(audio, voice);
    assert(voice->released && !voice->on_release_list);
    assert(!voice->envelopes[0].released);
    descriptor->release_identity = GC_AUDIO_TABLE_BANK | 80;
    descriptor->release_count = 2;
    gc_audio_sequence_tick(audio);
    assert(voice->on_release_list && voice->envelopes[0].release_pending);
    assert(audio->tracks[0].note_voices[1] == GC_AUDIO_VOICES);
    free(audio);
}

static void test_zero_duration_parser_wait(unsigned revision) {
    uint8_t sequence[] = {60, 0x10, 127, 100, 0, 0, 0xa4, 0, 42, 0x80, 255};
    GcAudio *audio = start_sequence(revision, sequence, sizeof(sequence));
    GcAudioTrack *track = &audio->tracks[0];
    assert(audio->notes_started == 1 && track->pc == 6);
    assert(track->note_voices[0] == 0 && track->registers[0] == 0);
    assert(track->wait == (revision ? UINT32_MAX : 0));
    gc_audio_sequence_tick(audio);
    if (revision) {
        assert(track->wait == UINT32_MAX && track->pc == 6);
        audio->voices[0].active = false;
        gc_audio_sequence_tick(audio);
    }
    assert(track->registers[0] == 42 && track->wait == 255);
    /* Neither region takes the positive-wait completion cleanup path. */
    assert(track->note_voices[0] == 0);
    assert(audio->rejected_commands == 0);
    free(audio);
}

static void test_descriptor_storage_reuse(unsigned revision) {
    uint8_t sequence[80] = {
        0xc1, 0, 0, 0, 32, 0x80, 1, 0xc1, 0, 0, 0, 64, 0x80, 1, 0xc7, 0, 0, 7,
    };
    const uint8_t original[] = {
        0xd8, 0, 8, 0, 9, 0, 10, 0x40, 0, 0, 11, 0x80, 255,
    };
    memcpy(sequence + 32, original, sizeof(original));
    sequence[64] = 0x80;
    sequence[65] = 255;
    GcAudio *audio = start_sequence(revision, sequence, sizeof(sequence));
    unsigned original_index = audio->tracks[0].children[0];
    GcAudioTrack *original_track = &audio->tracks[original_index];
    GcAudioEnvelope first = original_track->envelopes[0];
    GcAudioLocalEnvelopeTables tables = original_track->local_envelope_tables;
    assert(first.attack_identity == (GC_AUDIO_TABLE_LOCAL | 1));
    assert(first.release_identity == (GC_AUDIO_TABLE_LOCAL | 2));
    assert(tables.attack[0].ticks == 8 && tables.release[0].ticks == 11);
    original_track->envelopes[1] = first;
    original_track->envelopes[1].target = 1;
    original_track->envelopes[1].rate = 0.75f;
    original_track->envelopes[1].scale = 0.625f;
    original_track->envelopes[1].offset = 0.125f;
    GcAudioEnvelope second = original_track->envelopes[1];
    /* USA reuses this child directly; EUR returns to its storage only after
     * cycling through every child slot in the FIFO pool.
     */
    unsigned replacements = revision ? GC_AUDIO_TRACKS - 1 : 1;
    for (unsigned replacement = 0; replacement < replacements; ++replacement)
        gc_audio_sequence_tick(audio);
    assert(audio->tracks[0].children[0] == original_index);
    GcAudioTrack *reused = &audio->tracks[original_index];
    assert(!reused->envelopes[0].enabled && !reused->envelopes[1].enabled);
    assert(reused->envelope_modes[0] == 15 && reused->envelope_modes[1] == 15);
    assert(memcmp(&reused->local_envelope_tables, &tables, sizeof(tables)) == 0);
    if (revision) {
        const GcAudioEnvelope *reset = &reused->envelopes[0];
        assert(reset->target == 0 && reset->rate == 1 && reset->scale == 1);
        assert(reset->offset == 0 && reset->attack_identity == 0);
        assert(reset->attack_count == 0);
        assert(reset->release_identity == (GC_AUDIO_TABLE_TEMPLATE | 1));
        assert(reset->release_count == 2 && reset->release[0].ticks == 10);
    } else {
        assert(reused->envelopes[0].attack_identity == first.attack_identity);
        assert(reused->envelopes[0].release_identity == first.release_identity);
        assert(reused->envelopes[0].attack_count == first.attack_count);
        assert(reused->envelopes[0].release_count == first.release_count);
    }
    const GcAudioEnvelope *retained = &reused->envelopes[1];
    assert(retained->target == second.target && retained->rate == second.rate);
    assert(retained->scale == second.scale && retained->offset == second.offset);
    assert(retained->attack_identity == second.attack_identity);
    assert(retained->release_identity == second.release_identity);

    GcAudioVoice voice = {.active = true, .track = original_index};
    memset(voice.envelope_tracks, 2, sizeof(voice.envelope_tracks));
    voice.envelope_tracks[0] = 1;
    gc_audio_envelope_start(&voice.envelopes[0], &second);
    gc_audio_sequence_envelopes(audio, &voice);
    assert(voice.envelopes[0].envelope.attack[0].ticks == 8);
    /* Descriptor one aliases the inline tables formerly installed by zero.
     * D8 changes that backing without changing one's descriptor pointers.
     */
    sequence[34] = 15;
    audio->tracks[0].pc = 64;
    audio->tracks[0].wait = 0;
    reused->pc = 32;
    reused->wait = 0;
    gc_audio_sequence_tick(audio);
    gc_audio_sequence_envelopes(audio, &voice);
    assert(voice.envelopes[0].envelope.attack[0].ticks == 15);
    assert(voice.envelopes[0].envelope.target == 1);
    assert(audio->rejected_commands == 0);
    free(audio);
}

static void test_birth_weights_and_routes(unsigned revision) {
    uint8_t sequence[36] = {
        0x9c, 3, 0x20, 0,    60, 1, 127, 0x80, 1, 0xac,
        9,    0, 0,    0xac, 10, 0, 1,   0xdd, 0, revision ? 2 : 0x20,
        0};
    const uint8_t note[] = {60, 2, 127, 0x80, 255};
    unsigned offset = revision ? 21 : 20;
    for (unsigned index = 0; index < sizeof(note); ++index)
        sequence[offset + index] = note[index];
    GcAudio *audio = start_sequence(revision, sequence, sizeof(sequence));
    GcAudioVoice *original = &audio->voices[0];
    assert(original->pan == 0.375f);
    uint16_t route = original->routes[0];
    gc_audio_sequence_tick(audio);
    assert(audio->notes_started == 2);
    gc_audio_sequence_envelopes(audio, original);
    assert(original->pan == 0.375f && original->routes[0] == route);
    assert(original->buses[0] == 1);
    assert(audio->voices[1].pan == 0.25f && audio->voices[1].buses[0] == 2);
    free(audio);
}

static void test_zero_sum_birth_weights(unsigned revision) {
    uint8_t sequence[] = {0xac, 9,    0, 0,  0xac, 10,  0,    0,  0x9c,
                          2,    0x60, 0, 60, 1,    127, 0x80, 255};
    GcAudio *audio = start_sequence(revision, sequence, sizeof(sequence));
    assert(audio->notes_started == 1);
    assert(audio->voices[0].reverb == 0.25f);
    free(audio);
}

static void test_live_oscillator_target(unsigned revision) {
    uint8_t usa[] = {0xd6, 1, 60, 1, 127, 0x80, 1, 0xd6, 0, 0x80, 255};
    uint8_t eur[] = {0xd6, 2, 0xf0, 0x11, 0x9c, 9, 0x40, 0, 60,   1,  127,
                     0x80, 1, 0xd6, 0,    0x9c, 9, 0x40, 0, 0x80, 255};
    GcAudio *audio = revision ? start_sequence(revision, eur, sizeof(eur))
                              : start_sequence(revision, usa, sizeof(usa));
    GcAudioVoice *voice = &audio->voices[0];
    gc_audio_sequence_tick(audio);
    gc_audio_sequence_envelopes(audio, voice);
    assert(audio->rejected_commands == 0);
    assert(voice->envelopes[1].envelope.target == 1);
    assert(voice->envelope_volume == 1);
    assert(voice->envelope_pitch == (revision ? 0x1.7852bap+0f : 0x1.f3314cp+0f));
    free(audio);
}

static void test_live_release_table_identity(unsigned revision, bool becomes_shared) {
    uint8_t sequence[80] = {
        0xd7, 0, 0, 0, 40, 0xd7, 1, 0, 0, becomes_shared ? 56 : 40,
    };
    const uint8_t note[] = {
        60, 1, 127, 0x80, 1, 0x81, 0xd7, 1, 0, 0, becomes_shared ? 40 : 56, 0x80, 255,
    };
    unsigned offset = 10;
    if (revision) {
        sequence[offset++] = 0xf0;
        sequence[offset++] = 0;
    }
    memcpy(sequence + offset, note, sizeof(note));
    /* The attack ramps and cycles; the distinct release immediately holds
     * quarter scale. Both share one real mutable descriptor in the track.
     */
    sequence[43] = 10;
    sequence[44] = 0x40;
    sequence[47] = 13;
    sequence[60] = 0x20;
    sequence[63] = 14;
    GcAudio *audio = start_sequence(revision, sequence, sizeof(sequence));
    GcAudioVoice *voice = &audio->voices[0];
    gc_audio_sequence_tick(audio);
    assert(audio->rejected_commands == 0);
    assert(voice->released && voice->envelopes[0].release_pending);
    gc_audio_sequence_envelopes(audio, voice);
    assert(!voice->envelopes[0].release_pending);
    assert(voice->envelopes[0].envelope.release_continues == becomes_shared);
    if (becomes_shared) {
        assert(voice->envelopes[0].pc == 1);
        assert(voice->envelopes[0].current > 0);
        assert(!voice->envelopes[0].held);
    } else {
        assert(voice->envelopes[0].pc == 2);
        assert(voice->envelopes[0].current == 0.25f);
        assert(voice->envelopes[0].held);
        assert(voice->envelope_volume == 0.25f);
    }
    free(audio);
}

static void test_ramp_final_step(unsigned revision) {
    /* Native FADDS leaves two low mantissa bits after this seventeen-step
     * ramp. Replacing its last accumulated value with the target loses them.
     */
    uint8_t sequence[] = {0x9c, 0, 0, 0, 0x9f, 0, 0x2e, 0xe0, 0, 17, 0x80, 40};
    GcAudio *audio = start_sequence(revision, sequence, sizeof(sequence));
    for (unsigned tick = 1; tick < 17; ++tick)
        gc_audio_sequence_tick(audio);
    assert(audio->tracks[0].parameter_ticks[0] == 0);
    assert(audio->tracks[0].parameters[0] == 0x1.770004p-2f);
    assert(audio->tracks[0].parameter_targets[0] == 0x1.77p-2f);
    gc_audio_sequence_tick(audio);
    assert(audio->tracks[0].parameters[0] == 0x1.770004p-2f);
    free(audio);
}

static void test_regional_gate_rounding(unsigned revision) {
    /* Independently recovered FMULS/FDIVS/FCTIWZ results. Double arithmetic
     * produces sixty-three USA updates and 5,618 EUR updates instead.
     */
    unsigned duration = revision ? 1604 : 15;
    unsigned gate = revision ? 84 : 96;
    uint8_t sequence[] = {
        60,   0x10, 127, (uint8_t)gate, (uint8_t)(duration >> 8), (uint8_t)duration,
        0x80, 255};
    GcAudio *audio = start_sequence(revision, sequence, sizeof(sequence));
    assert(audio->notes_started == 1 && audio->voices[0].active);
    assert(audio->voices[0].duration == (revision ? 5619u : 62u));
    assert(audio->tracks[0].wait == duration);
    free(audio);
}

static void test_gate_zero_and_bounds(unsigned revision) {
    uint8_t zero[] = {60, 0x10, 127, 100, 0, 0, 0x80, 255};
    GcAudio *audio = start_sequence(revision, zero, sizeof(zero));
    assert(audio->notes_started == 1);
    assert(audio->voices[0].duration == (revision ? UINT32_MAX : 0));
    float output[2];
    for (unsigned frame = 0; frame <= (unsigned)GC_AUDIO_DSP_QUANTUM; ++frame)
        gc_audio_dsp_render_frame(audio, false, output);
    assert(audio->voices[0].released == (revision == 0));
    free(audio);

    uint8_t maximum[] = {60, 0x18, 127, 127, 0xff, 0xff, 0xfe};
    audio = start_sequence(revision, maximum, sizeof(maximum));
    assert(audio->notes_started == 1 && audio->voices[0].active);
    assert(audio->voices[0].duration > 0xffffff);
    assert(audio->voices[0].duration < UINT32_MAX);
    free(audio);

    /* A valid encoded duration can exceed the portable counter range at a
     * very slow tempo. It must not cause an out-of-range float conversion.
     */
    uint8_t overflow[] = {0xfe, 0, 1, 0xfd, 0, 1, 60, 0x18, 127, 127, 0xff, 0xff, 0xfe};
    audio = start_sequence(revision, overflow, sizeof(overflow));
    assert(audio->notes_started == 1 && audio->voices[0].duration == UINT32_MAX);
    free(audio);
}

static void test_menu_music_identity(unsigned revision) {
    /* A music child inherits the menu identity without replacing its own ID.
     * A release tail retains the tag after its track is stopped/recycled. */
    uint8_t sequence[48] = {
        0xd0, 0, 2, 0x10, 1, 0xc1, 0, 0, 0, 32, 0x80, 255,
    };
    const uint8_t child[] = {60, 1, 127, 0x80, 255};
    memcpy(sequence + 32, child, sizeof(child));
    GcAudio *audio = start_sequence(revision, sequence, sizeof(sequence));
    GcAudioVoice *voice = &audio->voices[0];
    assert(voice->active && voice->menu_music);
    voice->base_gain = voice->track_gain = voice->envelope_volume = 1;
    voice->buses[0] = 1;
    voice->routes[0] = revision ? 0x100 : 0x10;
    assert(gc_audio_route_gain(audio, voice, 0, false) == 1);
    assert(gc_audio_set_menu_volume(audio, 50));
    assert(gc_audio_route_gain(audio, voice, 0, false) == 1);
    voice->menu_music = false;
    assert(gc_audio_route_gain(audio, voice, 0, false) == 1);
    voice->menu_music = true;
    assert(gc_audio_stop_sequence(audio));
    gc_audio_sequence_tick(audio);
    assert(voice->menu_music && voice->detached);
    assert(gc_audio_route_gain(audio, voice, 0, false) == 1);
    assert(gc_audio_menu_volume(audio) == 50);
    free(audio);

    sequence[4] = 2; /* The startup track and its descendants are not music. */
    audio = start_sequence(revision, sequence, sizeof(sequence));
    assert(audio->voices[0].active && !audio->voices[0].menu_music);
    free(audio);
}

int main(void) {
    for (unsigned revision = 0; revision < 2; ++revision) {
        test_signed_register_target(revision);
        test_signed_register_multiply(revision);
        test_parent_child_pan_and_echo(revision);
        test_regional_register_defaults(revision);
        test_regional_parent_blend(revision);
        test_cube_volume_single_rounding(revision);
        test_three_level_pitch(revision);
        test_null_release_guard(revision);
        test_gate_release_update_order(revision);
        test_attached_release_controls(revision);
        test_detached_release_controls(revision);
        test_timed_note_handle_cleanup(revision);
        test_indexed_note_replacement_ownership(revision);
        test_replacement_without_free_voice(revision);
        test_zero_duration_parser_wait(revision);
        test_descriptor_storage_reuse(revision);
        test_birth_weights_and_routes(revision);
        test_zero_sum_birth_weights(revision);
        test_live_oscillator_target(revision);
        test_live_release_table_identity(revision, false);
        test_live_release_table_identity(revision, true);
        test_ramp_final_step(revision);
        test_regional_gate_rounding(revision);
        test_gate_zero_and_bounds(revision);
        test_menu_music_identity(revision);
    }
    test_live_release_pointer_guard(false);
    test_live_release_pointer_guard(true);
    test_repeated_null_release_callback();
    puts("audio control arithmetic tests passed");
    return 0;
}
