#include "gamecube/card_lighting.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static GcCardLightingStyle selection_style(void) {
    return (GcCardLightingStyle){true, 10, 30, 1, 128, 320, 28, 0.525f};
}

static void test_slot_change_trace(void) {
    GcCardLightingStyle style = selection_style();
    GcCardLighting state = {0};
    gc_menu menu;
    gc_menu_init(&menu, GC_REGION_EUROPE);
    menu.page = GC_PAGE_CARDS;
    menu.cards[0].status = menu.cards[1].status = GC_CARD_READY;
    assert(gc_card_lighting_advance(&style, &state, &menu, 5));
    assert(state.focus[0] == 5 && state.focus[1] == 0 && state.center == 0);
    assert(gc_card_lighting_header_alpha(&style, &state, 0, 0, 255) == 190);
    assert(gc_card_lighting_header_alpha(&style, &state, 1, 0, 255) == 127);
    assert(gc_card_lighting_advance(&style, &state, &menu, 5));
    assert(state.focus[0] == 10);
    menu.card_slot = 1;
    assert(gc_card_lighting_advance(&style, &state, &menu, 5));
    assert(state.focus[0] == 5 && state.focus[1] == 5);
    assert(gc_card_lighting_header_alpha(&style, &state, 0, 1, 255) == 190);
    assert(gc_card_lighting_header_alpha(&style, &state, 1, 1, 255) == 190);
    assert(gc_card_lighting_advance(&style, &state, &menu, 5));
    assert(state.focus[0] == 0 && state.focus[1] == 10);
    menu.cards[1].status = GC_CARD_ABSENT;
    assert(gc_card_lighting_advance(&style, &state, &menu, 10));
    assert(state.focus[0] == 0 && state.focus[1] == 0 && state.center == 10);
    GcCardLighting saved = state;
    assert(gc_card_lighting_advance(&style, &state, &menu, 0));
    assert(memcmp(&saved, &state, sizeof(state)) == 0);
    menu.page = GC_PAGE_FACE;
    assert(gc_card_lighting_advance(&style, &state, &menu, 100));
    assert(!state.active && state.center == 10);
    menu.page = GC_PAGE_CARDS;
    menu.cards[1].status = GC_CARD_READY;
    assert(gc_card_lighting_advance(&style, &state, &menu, 1));
    assert(state.center == 0 && state.focus[1] == 1 && state.blink_phase == 1);
    style.focus_ticks = 0;
    saved = state;
    assert(!gc_card_lighting_advance(&style, &state, &menu, 1));
    assert(memcmp(&saved, &state, sizeof(state)) == 0);
}

static void test_blink_and_bounded_advance(void) {
    GcCardLightingStyle style = selection_style();
    style.header_flags = 2;
    GcCardLighting slow = {0}, fast = {0};
    gc_menu menu;
    gc_menu_init(&menu, GC_REGION_EUROPE);
    menu.page = GC_PAGE_CARDS;
    for (unsigned tick = 0; tick < 6001; ++tick)
        assert(gc_card_lighting_advance(&style, &slow, &menu, 1));
    assert(gc_card_lighting_advance(&style, &fast, &menu, 6001));
    assert(memcmp(&slow, &fast, sizeof(slow)) == 0);
    fast.blink_phase = 30;
    assert(gc_card_lighting_header_alpha(&style, &fast, 0, 0, 255) == 255);
    assert(gc_card_lighting_advance(&style, &fast, &menu, 1));
    assert(gc_card_lighting_header_alpha(&style, &fast, 0, 0, 255) == 255);
    assert(gc_card_lighting_advance(&style, &fast, &menu, 1));
    assert(gc_card_lighting_header_alpha(&style, &fast, 0, 0, 255) == 250);
    fast.blink_phase = 61;
    assert(gc_card_lighting_header_alpha(&style, &fast, 0, 0, 255) == 127);
    assert(gc_card_lighting_advance(&style, &fast, &menu, 1));
    assert(gc_card_lighting_header_alpha(&style, &fast, 0, 0, 255) == 127);
    assert(gc_card_lighting_header_alpha(&style, &fast, 1, 0, 255) == 255);
    assert(gc_card_lighting_advance(&style, &fast, &menu, UINT64_MAX));
    assert(fast.center == 10 && fast.blink_phase < 62);
}

static void test_additive_spotlight(void) {
    GcCardGridLighting lighting = {0};
    lighting.lights[1] = (GcMenuGridLighting){500, 0.525f, {124, 166, 320}, 28};
    lighting.alpha[1] = 255;
    float center[4], far[4], faded[4];
    assert(gc_card_lighting_grid_color(&lighting, 124, 166, 255, center));
    float expected = 1 / (1 + (1 - 0.525f) * 320 / (0.525f * 500));
    assert(fabsf(center[3] - expected) < 0.000001f);
    assert(gc_card_lighting_grid_color(&lighting, 424, 166, 255, far));
    assert(far[3] == 0);
    assert(gc_card_lighting_grid_color(&lighting, 124, 166, 127, faded));
    assert(fabsf(faded[3] - center[3] * 127 / 255) < 0.000001f);
    lighting.lights[2] = lighting.lights[1];
    lighting.alpha[2] = 255;
    assert(gc_card_lighting_grid_color(&lighting, 124, 166, 127, faded));
    assert(fabsf(faded[3] - 127.0f / 255) < 0.000001f);
}

static void test_fixed_center(const GcCardLightingStyle *style,
                              const GcFaceGeometry *geometry,
                              const GcLayoutTable *table) {
    GcCardLighting state = {0};
    gc_menu menu;
    gc_menu_init(&menu, GC_REGION_USA);
    menu.page = GC_PAGE_CARDS;
    GcCardGridLighting original;
    assert(gc_card_lighting_grid(style, &state, geometry, table, &original));
    assert(original.alpha[0] == 255 && original.alpha[1] == 0 &&
           original.alpha[2] == 0);
    assert(original.lights[0].reference_distance == 500);
    assert(original.lights[0].reference_brightness == 0.375f);
    assert(original.lights[0].position[0] == 296 &&
           original.lights[0].position[1] == 224 &&
           original.lights[0].position[2] == 474);
    assert(original.lights[0].cutoff_degrees == 36);
    float left[4], right[4];
    assert(gc_card_lighting_grid_color(&original, 132, 166, 255, left));
    assert(gc_card_lighting_grid_color(&original, 460, 166, 255, right));
    assert(left[3] > 0.2f && left[3] == right[3]);
    for (unsigned mask = 0; mask < 4; ++mask) {
        menu.cards[0].status = mask & 1 ? GC_CARD_READY : GC_CARD_ABSENT;
        menu.cards[1].status = mask & 2 ? GC_CARD_READY : GC_CARD_ABSENT;
        for (unsigned slot = 0; slot < 2; ++slot) {
            menu.card_slot = slot;
            assert(gc_card_lighting_advance(style, &state, &menu, 10));
            GcCardGridLighting actual;
            assert(gc_card_lighting_grid(style, &state, geometry, table, &actual));
            assert(!memcmp(original.lights, actual.lights, sizeof(actual.lights)));
            assert(!memcmp(original.alpha, actual.alpha, sizeof(actual.alpha)));
            assert(gc_card_lighting_header_alpha(style, &state, 0, slot, 255) == 255);
            assert(gc_card_lighting_header_alpha(style, &state, 1, slot, 255) == 255);
            assert(gc_card_lighting_header_alpha(style, &state, 1, slot, 73) == 73);
        }
    }
}

static void test_rom(const char *path) {
    GcText text = {0};
    GcLayouts layouts;
    GcFaceGeometry geometry;
    GcCardLightingStyle style;
    assert(gc_text_load(path, &text));
    assert(gc_layouts_index(&text, &layouts));
    assert(gc_face_geometry_load(path, &geometry));
    assert(gc_card_lighting_style_decode(&text, &style));
    GcCardLightingStyle saved = style;
    text.rom_size = 128;
    assert(!gc_card_lighting_style_decode(&text, &style));
    assert(memcmp(&saved, &style, sizeof(style)) == 0);
    text.rom_size = GC_IPL_ROM_SIZE;
    assert(style.selection_lights == text.europe);
    const GcLayoutTable *table =
        gc_layout_table(&layouts, GC_LANGUAGE_ENGLISH, GC_LAYOUT_CARD);
    if (!text.europe) {
        test_fixed_center(&style, &geometry, table);
        gc_text_destroy(&text);
        return;
    }
    assert(style.focus_ticks == 10 && style.blink_ticks == 30);
    assert(style.header_flags == 1 && style.header_strength == 128);
    assert(style.slot_height == 320 && style.cutoff_degrees == 28);
    assert(style.reference_brightness == 0.525f);
    GcCardLighting state = {.focus = {10, 0}};
    GcCardGridLighting lighting;
    assert(gc_card_lighting_grid(&style, &state, &geometry, table, &lighting));
    assert(lighting.alpha[0] == 0);
    GcLayoutPane first, horizontal, vertical;
    assert(gc_layout_find_pane(table, "ia05", 0, &first));
    assert(gc_layout_find_pane(table, "ia06", 0, &horizontal));
    assert(gc_layout_find_pane(table, "ia09", 0, &vertical));
    assert(lighting.lights[1].position[0] ==
           (first.box.center_x + horizontal.box.center_x) * 0.5f);
    assert(lighting.lights[1].position[1] ==
           (first.box.center_y + vertical.box.center_y) * 0.5f);
    assert(lighting.alpha[1] == 255 && lighting.alpha[2] == 0);
    assert(gc_card_lighting_header_alpha(&style, &state, 0, 0, 255) == 255);
    assert(gc_card_lighting_header_alpha(&style, &state, 1, 0, 255) == 127);
    float selected[4], inactive[4];
    assert(gc_card_lighting_grid_color(&lighting, lighting.lights[1].position[0],
                                       lighting.lights[1].position[1], 255, selected));
    assert(gc_card_lighting_grid_color(&lighting, lighting.lights[2].position[0],
                                       lighting.lights[2].position[1], 255, inactive));
    assert(selected[3] > 0.6f && inactive[3] == 0);
    state.focus[0] = 0;
    state.focus[1] = 10;
    assert(gc_card_lighting_grid(&style, &state, &geometry, table, &lighting));
    assert(lighting.alpha[0] == 0 && lighting.alpha[1] == 0 &&
           lighting.alpha[2] == 255);
    assert(gc_card_lighting_header_alpha(&style, &state, 0, 1, 255) == 127);
    assert(gc_card_lighting_header_alpha(&style, &state, 1, 1, 255) == 255);
    state = (GcCardLighting){.center = style.focus_ticks};
    assert(gc_card_lighting_grid(&style, &state, &geometry, table, &lighting));
    assert(lighting.alpha[0] == 255 && lighting.alpha[1] == 0 &&
           lighting.alpha[2] == 0);
    assert(gc_card_lighting_grid_color(&lighting, 132, 166, 255, selected));
    assert(gc_card_lighting_grid_color(&lighting, 460, 166, 255, inactive));
    assert(selected[3] > 0.2f && selected[3] == inactive[3]);
    gc_text_destroy(&text);
}

int main(int argc, char **argv) {
    test_slot_change_trace();
    test_blink_and_bounded_advance();
    test_additive_spotlight();
    for (int index = 1; index < argc; ++index)
        test_rom(argv[index]);
    puts("Regional card lighting and header fade traces passed.");
    return 0;
}
