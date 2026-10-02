#include "gamecube/card_usage.h"
#include "gamecube/layout.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_native_availability(void) {
    gc_card cards[2] = {{.status = GC_CARD_READY,
                         .capacity_blocks = 59,
                         .file_count = 1,
                         .files = {{.blocks = 1}}},
                        {.status = GC_CARD_ABSENT}};
    GcCardUsage state = {0};
    assert(gc_card_usage_advance(&state, cards, 0));
    assert(state.alpha[0] == 0 && state.level[0] == 0);
    assert(gc_card_usage_advance(&state, cards, 1));
    assert(state.alpha[0] == 60 && state.alpha[1] == 60);
    assert(state.level[0] == 5 && state.level[1] == 0);
    assert(gc_card_usage_fill(&state, 0) == 0);
    assert(gc_card_usage_alpha(&state, 0, 128) == 30);
    assert(gc_card_usage_advance(&state, cards, 48));
    assert(state.alpha[0] == 252 && state.alpha[1] == 60);
    assert(gc_card_usage_fill(&state, 0) == 4);
    assert(gc_card_usage_advance(&state, cards, 1));
    assert(state.alpha[0] == 255 && gc_card_usage_fill(&state, 0) == 5);
    assert(gc_card_usage_alpha(&state, 0, 128) == 128);
    cards[0].files[0].blocks = 59;
    assert(gc_card_usage_advance(&state, cards, 0));
    assert(state.level[0] == 5);
    assert(gc_card_usage_advance(&state, cards, 1));
    assert(state.level[0] == 255 && gc_card_usage_fill(&state, 0) == 255);
    cards[0] = (gc_card){.status = GC_CARD_ABSENT};
    assert(gc_card_usage_advance(&state, cards, 1));
    assert(state.alpha[0] == 251 && state.level[0] == 255);
    assert(gc_card_usage_fill(&state, 0) == 249);
    assert(gc_card_usage_advance(&state, cards, 48));
    assert(state.alpha[0] == 60 && state.level[0] == 255);
    assert(gc_card_usage_fill(&state, 0) == 0);
    assert(gc_card_usage_alpha(&state, 0, 255) == 60);
    cards[0] = (gc_card){.status = GC_CARD_READY,
                         .capacity_blocks = 59,
                         .file_count = 1,
                         .files = {{.blocks = 58}}};
    assert(gc_card_usage_advance(&state, cards, 1));
    assert(state.alpha[0] == 64 && state.level[0] == 251);
    assert(gc_card_usage_fill(&state, 0) == 5);
    assert(gc_card_usage_advance(&state, cards, UINT64_MAX));
    assert(state.alpha[0] == 255 && state.alpha[1] == 60);
    assert(gc_card_usage_fill(&state, 0) == 251);
}

static void test_native_height_trace(void) {
    gc_card cards[2] = {{.status = GC_CARD_READY,
                         .capacity_blocks = 59,
                         .file_count = 1,
                         .files = {{.blocks = 59}}},
                        {.status = GC_CARD_READY, .capacity_blocks = 251}};
    GcCardUsage state = {0};
    /* Four native updates after a card becomes ready. Heights retain the
     * original 12.4 precision before conversion to renderer coordinates. */
    static const uint8_t native_alpha[4] = {60, 64, 68, 72};
    static const uint8_t native_fill[4] = {0, 5, 10, 15};
    static const unsigned native_height[4] = {0, 28, 57, 86};
    for (unsigned tick = 0; tick < 4; ++tick) {
        assert(gc_card_usage_advance(&state, cards, 1));
        assert(state.alpha[0] == native_alpha[tick]);
        unsigned fill = gc_card_usage_fill(&state, 0);
        assert(fill == native_fill[tick]);
        assert(fill * 1472 / 255 == native_height[tick]);
        assert(state.level[1] == 0 && gc_card_usage_fill(&state, 1) == 0);
    }
    GcCardUsage batched = {0};
    assert(gc_card_usage_advance(&batched, cards, 4));
    assert(memcmp(&state, &batched, sizeof(state)) == 0);
    assert(gc_card_usage_advance(&batched, cards, UINT64_MAX));
    assert(batched.alpha[0] == 255 && gc_card_usage_fill(&batched, 0) == 255);
    cards[0].status = GC_CARD_DAMAGED;
    assert(gc_card_usage_advance(&batched, cards, 49));
    assert(batched.alpha[0] == 60 && batched.level[0] == 255);
    assert(gc_card_usage_fill(&batched, 0) == 0);
}

static void test_rejected_metadata(void) {
    gc_card cards[2] = {{.status = GC_CARD_READY, .capacity_blocks = 59},
                        {.status = GC_CARD_READY, .capacity_blocks = 251}};
    GcCardUsage state = {.alpha = {80, 160}, .level = {70, 140}};
    GcCardUsage saved = state;
    cards[1].capacity_blocks = 0;
    assert(!gc_card_usage_advance(&state, cards, 1));
    assert(memcmp(&state, &saved, sizeof(state)) == 0);
    cards[1].capacity_blocks = 251;
    cards[1].file_count = 1;
    cards[1].files[0].blocks = 252;
    assert(!gc_card_usage_advance(&state, cards, 1));
    assert(memcmp(&state, &saved, sizeof(state)) == 0);
    cards[1].file_count = GC_CARD_FILE_LIMIT + 1;
    assert(!gc_card_usage_advance(&state, cards, 1));
    assert(memcmp(&state, &saved, sizeof(state)) == 0);
    assert(gc_card_usage_alpha(NULL, 0, 255) == 0);
    assert(gc_card_usage_fill(NULL, 0) == 0);
    assert(gc_card_usage_alpha(&state, 2, 255) == 0);
    assert(gc_card_usage_fill(&state, 2) == 0);
}

static uint32_t read_word(const GcText *text, size_t code_offset) {
    size_t offset = code_offset + 0x820;
    assert(offset <= text->rom_size && text->rom_size - offset >= 4);
    const uint8_t *bytes = text->rom + offset;
    return (uint32_t)bytes[0] << 24 | (uint32_t)bytes[1] << 16 |
           (uint32_t)bytes[2] << 8 | bytes[3];
}

static void test_original_constants(const char *path) {
    GcText text = {0};
    GcLayouts layouts = {0};
    GcLayoutFrame frame;
    assert(gc_text_load(path, &text));
    size_t update = text.europe ? 0x1247c : 0x11994;
    /* Native DIVW/free-complement and signed +/-4, upper/lower clamp words. */
    assert(read_word(&text, update + 0x1c) == UINT32_C(0x2c000004));
    assert(read_word(&text, update + 0x30) == UINT32_C(0x1c8400ff));
    assert(read_word(&text, update + 0x34) == UINT32_C(0x7c0403d6));
    assert(read_word(&text, update + 0x38) == UINT32_C(0x200000ff));
    assert(read_word(&text, update + 0x44) == UINT32_C(0x38e0fffc));
    assert(read_word(&text, update + 0x5c) == UINT32_C(0x2c0000ff));
    assert(read_word(&text, update + 0x70) == UINT32_C(0x2c00003c));
    assert(gc_layouts_index(&text, &layouts));
    const GcLayoutTable *table =
        gc_layout_table(&layouts, GC_LANGUAGE_ENGLISH, GC_LAYOUT_CARD_FACE);
    assert(table && gc_layout_find_frame(table, "mca2", 0, &frame));
    assert(frame.box.center_x == 91 && frame.box.center_y == 139);
    assert(frame.parameters[2] == 1120 && frame.parameters[3] == 1472);
    assert(frame.colors[0] == UINT32_C(0xfbffff77));
    assert(gc_layout_find_frame(table, "mcb2", 0, &frame));
    assert(frame.box.center_x == 197 && frame.box.center_y == 139);
    assert(frame.parameters[2] == 1120 && frame.parameters[3] == 1472);
    assert(frame.colors[0] == UINT32_C(0xffffff77));
    gc_text_destroy(&text);
}

int main(int argc, char **argv) {
    test_native_availability();
    test_native_height_trace();
    test_rejected_metadata();
    for (int index = 1; index < argc; ++index)
        test_original_constants(argv[index]);
    puts("Native card usage, availability, fixed-point heights and retained levels "
         "passed.");
    return 0;
}
