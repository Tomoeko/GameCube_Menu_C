#include "audio/audio_internal.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static GcAudio *start_sequence(unsigned revision, uint8_t *sequence, size_t size) {
    GcAudio *audio = calloc(1, sizeof(*audio));
    assert(audio);
    atomic_init(&audio->mono, false);
    atomic_init(&audio->event_read, 0);
    atomic_init(&audio->event_write, 0);
    atomic_init(&audio->dropped_events, 0);
    audio->sequence_revision = revision;
    audio->sequence = sequence;
    audio->sequence_size = size;
    gc_audio_sequence_init(audio);
    return audio;
}

static void assert_complete_fifo(const GcAudio *audio) {
    bool present[GC_AUDIO_CHILD_TRACKS] = {false};
    assert(audio->free_track_count == GC_AUDIO_CHILD_TRACKS);
    for (unsigned offset = 0; offset < GC_AUDIO_CHILD_TRACKS; ++offset) {
        unsigned cursor = (audio->free_track_read + offset) % GC_AUDIO_CHILD_TRACKS;
        unsigned index = audio->free_tracks[cursor];
        assert(index > 0 && index < GC_AUDIO_TRACKS);
        assert(!present[index - 1] && audio->track_available[index - 1]);
        assert(!audio->tracks[index].active);
        present[index - 1] = true;
    }
}

static void stop_sequence(GcAudio *audio) {
    assert(gc_audio_stop_sequence(audio));
    gc_audio_sequence_tick(audio);
    for (unsigned index = 0; index < GC_AUDIO_TRACKS; ++index)
        assert(!audio->tracks[index].active);
    if (audio->sequence_revision)
        assert_complete_fifo(audio);
}

static void test_replacement_wrap(unsigned revision) {
    uint8_t sequence[18] = {0xc1, 0, 0, 0, 16, 0x80, 255};
    sequence[16] = 0x80;
    sequence[17] = 255;
    GcAudio *audio = start_sequence(revision, sequence, sizeof(sequence));
    assert(audio->tracks[0].children[0] == 1);
    for (unsigned replacement = 1; replacement <= 768; ++replacement) {
        audio->tracks[0].pc = 0;
        audio->tracks[0].wait = 0;
        gc_audio_sequence_tick(audio);
        unsigned expected = revision ? replacement % GC_AUDIO_CHILD_TRACKS + 1 : 1;
        assert(audio->tracks[0].children[0] == expected);
        assert(audio->tracks[expected].active);
        if (revision)
            assert(audio->free_track_count == GC_AUDIO_CHILD_TRACKS - 1);
    }
    assert(audio->rejected_commands == 0);
    stop_sequence(audio);
    unsigned read = audio->free_track_read;
    unsigned write = audio->free_track_write;
    stop_sequence(audio);
    assert(audio->free_track_read == read && audio->free_track_write == write);
    free(audio);
}

static void test_subtree_enqueue_order(unsigned revision) {
    uint8_t sequence[50] = {
        0xc1, 0, 0, 0, 32, 0x80, 1, 0xc1, 0, 0, 0, 48, 0x80, 255,
    };
    const uint8_t child[] = {0xc1, 0, 0, 0, 48, 0x80, 255};
    memcpy(sequence + 32, child, sizeof(child));
    sequence[48] = 0x80;
    sequence[49] = 255;
    GcAudio *audio = start_sequence(revision, sequence, sizeof(sequence));
    assert(audio->tracks[0].children[0] == 1);
    assert(audio->tracks[1].children[0] == 2);
    gc_audio_sequence_tick(audio);
    assert(audio->tracks[0].children[0] == (revision ? 3u : 1u));
    assert(!audio->tracks[2].active);
    if (revision) {
        /* Native teardown returns the parent before walking child slots. */
        assert(audio->free_tracks[0] == 1 && audio->free_tracks[1] == 2);
        assert(audio->free_track_read == 3 && audio->free_track_write == 2);
    }
    stop_sequence(audio);
    free(audio);
}

static size_t append_children(uint8_t *sequence, size_t start, unsigned count,
                              unsigned target) {
    for (unsigned slot = 0; slot < count; ++slot) {
        sequence[start++] = 0xc1;
        sequence[start++] = (uint8_t)slot;
        sequence[start++] = (uint8_t)(target >> 16);
        sequence[start++] = (uint8_t)(target >> 8);
        sequence[start++] = (uint8_t)target;
    }
    sequence[start++] = 0x80;
    sequence[start++] = 255;
    return start;
}

static void test_external_root_and_exhaustion(unsigned revision) {
    uint8_t sequence[258] = {0};
    assert(append_children(sequence, 0, 16, 128) < 128);
    assert(append_children(sequence, 128, 16, 256) < 256);
    append_children(sequence, 256, 0, 0);
    GcAudio *audio = start_sequence(revision, sequence, sizeof(sequence));
    /* Sixteen parents and 240 grandchildren fill all 256 physical children.
     * The external IPL root consumes no child slot; further opens fail.
     */
    unsigned active = 0;
    for (unsigned index = 1; index < GC_AUDIO_TRACKS; ++index)
        active += audio->tracks[index].active ? 1u : 0u;
    assert(active == 256 && audio->tracks[0].active);
    assert(audio->rejected_commands == 16);
    if (revision)
        assert(audio->free_track_count == 0);
    unsigned read = audio->free_track_read;
    unsigned write = audio->free_track_write;
    audio->tracks[16].pc = 128;
    audio->tracks[16].wait = 0;
    gc_audio_sequence_tick(audio);
    assert(audio->rejected_commands == 32);
    assert(audio->free_track_read == read && audio->free_track_write == write);

    uint8_t replacement[18] = {0xc1, 0, 0, 0, 16, 0x80, 255};
    replacement[16] = 0x80;
    replacement[17] = 255;
    audio->sequence = replacement;
    audio->sequence_size = sizeof(replacement);
    audio->tracks[0].pc = 0;
    audio->tracks[0].wait = 0;
    gc_audio_sequence_tick(audio);
    /* With an empty FIFO, replacement uses its newly freed parent first;
     * the former grandchildren stay available for subsequent opens.
     */
    assert(audio->tracks[0].children[0] == 1 && audio->tracks[1].active);
    assert(audio->rejected_commands == 32);
    for (unsigned index = 17; index <= 32; ++index)
        assert(!audio->tracks[index].active);
    if (revision)
        assert(audio->free_track_count == 16);
    stop_sequence(audio);
    free(audio);
}

int main(void) {
    for (unsigned revision = 0; revision < 2; ++revision) {
        test_replacement_wrap(revision);
        test_subtree_enqueue_order(revision);
        test_external_root_and_exhaustion(revision);
    }
    puts("Regional audio track allocation tests passed.");
    return 0;
}
