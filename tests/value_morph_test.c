#include "gamecube/value_morph.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    uint32_t value;
    unsigned calls;
    bool fail;
} RandomFixture;

static bool random_sample(void *context, uint32_t *value) {
    RandomFixture *fixture = context;
    ++fixture->calls;
    if (fixture->fail)
        return false;
    *value = fixture->value;
    return true;
}

static GcValueMorphStyle synthetic_style(void) {
    return (GcValueMorphStyle){.spread = .4f,
                               .tens_multiplier = 2,
                               .duration = 10,
                               .sound_offsets = 768,
                               .disc_direction = {-1, 0, 0}};
}

static void test_disc_order_and_ready_gate(void) {
    GcValueMorphStyle style = synthetic_style();
    GcValueMorphState state = {0};
    RandomFixture random = {599, 0, false};
    gc_menu menu;
    gc_menu_init(&menu, GC_REGION_USA);
    menu.page = GC_PAGE_DISC;
    menu.disc_status = GC_DISC_ABSENT;
    assert(gc_value_morph_advance(&state, &style, &menu, false, 200, random_sample,
                                  &random));
    assert(!state.initialized && !random.calls);
    assert(
        gc_value_morph_advance(&state, &style, &menu, true, 2, random_sample, &random));
    assert(state.drawn.kind == GC_VALUE_MORPH_DISC);
    assert(state.drawn.next[0] == 1 && state.drawn.ticks[0] == 10);
    menu.disc_status = GC_DISC_READY;
    assert(
        gc_value_morph_advance(&state, &style, &menu, true, 1, random_sample, &random));
    assert(random.calls == 768);
    assert(state.drawn.next[0] == 1 && state.drawn.ticks[0] == 10);
    assert(state.current.previous[0] == 1 && state.current.next[0] == 0);
    assert(state.current.ticks[0] == 1 && state.current.direction[0] == -1);
    assert(gc_value_morph_advance(&state, &style, &menu, true, 1, NULL, NULL));
    assert(state.drawn.ticks[0] == 1);
    menu.disc_status = GC_DISC_LID_OPEN;
    assert(
        gc_value_morph_advance(&state, &style, &menu, true, 1, random_sample, &random));
    assert(random.calls == 1536);
    assert(state.drawn.next[0] == 0 && state.current.next[0] == 2);
    assert(gc_value_morph_advance(&state, &style, &menu, true, 50, NULL, NULL));
    menu.disc_status = GC_DISC_UNREADABLE;
    assert(
        gc_value_morph_advance(&state, &style, &menu, true, 1, random_sample, &random));
    assert(random.calls == 1536); /* Both native states select the unknown word. */
    menu.disc_status = GC_DISC_ABSENT;
    assert(
        gc_value_morph_advance(&state, &style, &menu, true, 1, random_sample, &random));
    assert(random.calls == 2304 && state.current.next[0] == 1);
    GcValueMorphState saved = state;
    menu.disc_status = (gc_disc_status)99;
    assert(!gc_value_morph_advance(&state, &style, &menu, true, 1, NULL, NULL));
    assert(memcmp(&state, &saved, sizeof(state)) == 0);
}

static void test_sound_order_and_interruption(void) {
    GcValueMorphStyle style = synthetic_style();
    GcValueMorphState state = {0};
    RandomFixture random = {599, 0, false};
    gc_menu menu;
    gc_menu_init(&menu, GC_REGION_USA);
    menu.page = GC_PAGE_OPTIONS;
    menu.editing = true;
    menu.settings.sound = GC_SOUND_STEREO;
    assert(gc_value_morph_advance(&state, &style, &menu, false, 20, random_sample,
                                  &random));
    assert(!state.initialized && random.calls == 0);
    assert(
        gc_value_morph_advance(&state, &style, &menu, true, 1, random_sample, &random));
    assert(state.drawn.kind == GC_VALUE_MORPH_NONE);
    assert(
        gc_value_morph_advance(&state, &style, &menu, true, 1, random_sample, &random));
    assert(state.drawn.kind == GC_VALUE_MORPH_SOUND);
    assert(state.drawn.ticks[0] == 10 && random.calls == 0);
    menu.settings.sound = GC_SOUND_MONO;
    assert(
        gc_value_morph_advance(&state, &style, &menu, true, 1, random_sample, &random));
    assert(random.calls == 768);
    assert(state.drawn.next[0] == GC_SOUND_STEREO && state.drawn.ticks[0] == 10);
    assert(state.current.previous[0] == GC_SOUND_STEREO);
    assert(state.current.next[0] == GC_SOUND_MONO && state.current.ticks[0] == 1);
    assert(state.current.direction[0] == 1 && state.current.offsets[767] == 31);
    assert(
        gc_value_morph_advance(&state, &style, &menu, true, 1, random_sample, &random));
    assert(state.drawn.ticks[0] == 1 && state.current.ticks[0] == 2);
    menu.settings.sound = GC_SOUND_STEREO;
    assert(
        gc_value_morph_advance(&state, &style, &menu, true, 1, random_sample, &random));
    assert(random.calls == 1536);
    assert(state.drawn.next[0] == GC_SOUND_MONO && state.drawn.ticks[0] == 2);
    assert(state.current.previous[0] == GC_SOUND_MONO);
    assert(state.current.next[0] == GC_SOUND_STEREO &&
           state.current.direction[0] == -1);
    GcValueMorphState saved = state;
    assert(
        gc_value_morph_advance(&state, &style, &menu, true, 0, random_sample, &random));
    assert(memcmp(&state, &saved, sizeof(state)) == 0);
    assert(gc_value_morph_advance(&state, &style, &menu, true, UINT64_MAX,
                                  random_sample, &random));
    assert(state.drawn.ticks[0] == 10 && state.current.ticks[0] == 10);
    assert(random.calls == 1536);
}

static void test_number_counters_and_wrap(void) {
    GcValueMorphStyle style = synthetic_style();
    GcValueMorphState state = {0};
    RandomFixture random = {0};
    gc_menu menu;
    gc_menu_init(&menu, GC_REGION_EUROPE);
    menu.page = GC_PAGE_CALENDAR;
    menu.editing = true;
    menu.editor_index = 5;
    menu.clock = (gc_date_time){2024, 2, 29, 12, 34, 8};
    assert(
        gc_value_morph_advance(&state, &style, &menu, true, 2, random_sample, &random));
    menu.clock.second = 9;
    assert(
        gc_value_morph_advance(&state, &style, &menu, true, 1, random_sample, &random));
    assert(random.calls == 112);
    assert(state.drawn.ticks[0] == 10 && state.drawn.ticks[1] == 0);
    assert(state.drawn.previous[1] == 8 && state.drawn.next[1] == 9);
    assert(state.current.ticks[1] == 1 && state.current.offsets[111] == 8);
    assert(state.drawn.direction[1] == -1);
    menu.clock.second = 10;
    assert(
        gc_value_morph_advance(&state, &style, &menu, true, 1, random_sample, &random));
    assert(random.calls == 224);
    assert(state.drawn.ticks[0] == 0 && state.drawn.ticks[1] == 0);
    assert(state.drawn.previous[0] == 0 && state.drawn.next[0] == 1);
    assert(state.drawn.previous[1] == 9 && state.drawn.next[1] == 0);
    assert(gc_value_morph_advance(&state, &style, &menu, true, 20, NULL, NULL));
    menu.clock.second = 59;
    assert(gc_value_morph_advance(&state, &style, &menu, true, 20, NULL, NULL));
    menu.clock.second = 0;
    assert(gc_value_morph_advance(&state, &style, &menu, true, 1, NULL, NULL));
    assert(state.drawn.direction[1] == -1);
    menu.clock.second = 59;
    assert(gc_value_morph_advance(&state, &style, &menu, true, 1, NULL, NULL));
    assert(state.drawn.direction[1] == 1);
    menu.editor_index = 4;
    assert(
        gc_value_morph_advance(&state, &style, &menu, true, 1, random_sample, &random));
    assert(state.drawn.kind == GC_VALUE_MORPH_NONE);
    assert(random.calls == 224); /* Cursor movement does not consume new offsets. */
    menu.page = GC_PAGE_OPTIONS;
    menu.editor_index = 1;
    menu.settings.screen_position = -1;
    assert(gc_value_morph_advance(&state, &style, &menu, true, 2, NULL, NULL));
    menu.settings.screen_position = 1;
    assert(
        gc_value_morph_advance(&state, &style, &menu, true, 1, random_sample, &random));
    assert(state.drawn.ticks[0] == 10 && state.drawn.ticks[1] == 10);
    assert(random.calls == 336); /* Native consumes offsets even if |value| is equal. */
    assert(state.drawn.direction[0] == -1);
}

static void test_failure_preserves_state(void) {
    GcValueMorphStyle style = synthetic_style();
    GcValueMorphState state = {0};
    gc_menu menu;
    gc_menu_init(&menu, GC_REGION_USA);
    menu.page = GC_PAGE_OPTIONS;
    menu.editing = true;
    assert(gc_value_morph_advance(&state, &style, &menu, true, 2, NULL, NULL));
    GcValueMorphState saved = state;
    menu.settings.sound = GC_SOUND_MONO;
    RandomFixture random = {0, 0, true};
    assert(!gc_value_morph_advance(&state, &style, &menu, true, 1, random_sample,
                                   &random));
    assert(memcmp(&state, &saved, sizeof(state)) == 0);
    style.duration = 0;
    assert(!gc_value_morph_advance(&state, &style, &menu, true, 1, NULL, NULL));
    assert(memcmp(&state, &saved, sizeof(state)) == 0);
}

static unsigned read_u16(const uint8_t *bytes) {
    return (unsigned)bytes[0] * 256 + bytes[1];
}

static unsigned map_source(const GcIplResource *map, size_t field, unsigned index) {
    unsigned count = read_u16(map->bytes + field);
    unsigned relative = read_u16(map->bytes + field + 2);
    assert(index < count);
    return read_u16(map->bytes + field + relative + (size_t)index * 2);
}

static const GcEditPoint *find_point(const GcEditPoint *points, size_t count,
                                     unsigned source, uint8_t alpha) {
    for (size_t index = 0; index < count; ++index)
        if (points[index].source_index == source && points[index].alpha == alpha)
            return &points[index];
    return NULL;
}

static void test_native_sound_points(const GcEditGeometry *geometry,
                                     const GcValueMorphStyle *style,
                                     gc_language language) {
    GcEditPoint source[GC_EDIT_POINT_LIMIT], output[GC_EDIT_POINT_LIMIT];
    GcEditState editor = {0};
    GcValueMorphState state = {0};
    RandomFixture random = {599, 0, false};
    gc_menu menu;
    gc_menu_init(&menu, geometry->europe ? GC_REGION_EUROPE : GC_REGION_USA);
    menu.page = GC_PAGE_OPTIONS;
    menu.editing = true;
    menu.settings.language = language;
    menu.settings.sound = GC_SOUND_STEREO;
    assert(gc_edit_state_advance(&editor, geometry, &menu, true, 200));
    assert(gc_value_morph_advance(&state, style, &menu, true, 200, NULL, NULL));
    menu.settings.sound = GC_SOUND_MONO;
    assert(
        gc_value_morph_advance(&state, style, &menu, true, 1, random_sample, &random));
    assert(random.calls == style->sound_offsets);
    assert(
        gc_value_morph_advance(&state, style, &menu, true, 1, random_sample, &random));
    size_t source_count = gc_edit_geometry_source_points(geometry, &menu, &editor,
                                                         source, GC_EDIT_POINT_LIMIT);
    size_t count = gc_value_morph_points(geometry, &menu, &editor, style, &state,
                                         output, GC_EDIT_POINT_LIMIT);
    assert(source_count && count && count < GC_EDIT_POINT_LIMIT);
    assert(gc_value_morph_points(geometry, &menu, &editor, style, &state, NULL, 0) ==
           count);
    const GcIplResource *map = &geometry->maps[language];
    unsigned stereo = map_source(map, geometry->europe ? 0xdc : 0xd0, 0);
    unsigned mono = map_source(map, geometry->europe ? 0xb0 : 0xac, 0);
    const GcEditPoint *outgoing = find_point(output, count, stereo, 229);
    const GcEditPoint *incoming = find_point(output, count, mono, 25);
    assert(outgoing && incoming);
    assert(fabsf(outgoing->position[0] - source[stereo].position[0] + 3.1f) < .001f);
    assert(fabsf(incoming->position[0] - source[mono].position[0] - 27.9f) < .001f);
    assert(memcmp(outgoing->colors, geometry->palettes[0][0],
                  sizeof(outgoing->colors)) == 0);
    GcEditPoint sentinel = {.position = {987, 654, 321}};
    output[0] = sentinel;
    assert(!gc_value_morph_points(geometry, &menu, &editor, style, &state, output,
                                  count - 1));
    assert(memcmp(&output[0], &sentinel, sizeof(sentinel)) == 0);
}

static void test_native_number_points(const GcEditGeometry *geometry,
                                      const GcValueMorphStyle *style,
                                      gc_language language) {
    GcEditPoint source[GC_EDIT_POINT_LIMIT], output[GC_EDIT_POINT_LIMIT];
    GcEditState editor = {0};
    GcValueMorphState state = {0};
    RandomFixture random = {0};
    gc_menu menu;
    gc_menu_init(&menu, geometry->europe ? GC_REGION_EUROPE : GC_REGION_USA);
    menu.page = GC_PAGE_CALENDAR;
    menu.editing = true;
    menu.editor_index = 5;
    menu.settings.language = language;
    menu.clock = (gc_date_time){2024, 2, 29, 12, 34, 9};
    assert(gc_edit_state_advance(&editor, geometry, &menu, true, 200));
    assert(gc_value_morph_advance(&state, style, &menu, true, 200, NULL, NULL));
    menu.clock.second = 10;
    assert(
        gc_value_morph_advance(&state, style, &menu, true, 2, random_sample, &random));
    assert(random.calls == 112);
    size_t source_count = gc_edit_geometry_source_points(geometry, &menu, &editor,
                                                         source, GC_EDIT_POINT_LIMIT);
    size_t count = gc_value_morph_points(geometry, &menu, &editor, style, &state,
                                         output, GC_EDIT_POINT_LIMIT);
    assert(source_count && count);
    const GcIplResource *map = &geometry->maps[language];
    unsigned tens = map_source(map, 0x50, 0);
    unsigned units = map_source(map, 0x54, 2);
    const GcEditPoint *first = find_point(output, count, tens, 229);
    const GcEditPoint *second = find_point(output, count, units, 229);
    assert(first && second);
    assert(fabsf(first->position[1] - source[tens].position[1] - 1.6f) < .001f);
    assert(fabsf(second->position[1] - source[units].position[1] - .8f) < .001f);
    assert(memcmp(first->colors, geometry->palettes[1][0], sizeof(first->colors)) == 0);
    /* A native pattern's padding must not duplicate a drawn source block. */
    for (size_t index = 0; index < count; ++index)
        if (output[index].field == GC_EDIT_SECOND)
            for (size_t other = index + 1; other < count; ++other)
                assert(output[index].source_index != output[other].source_index ||
                       output[index].alpha != output[other].alpha);
}

static void test_native_disc_points(const GcEditGeometry *geometry,
                                    const GcValueMorphStyle *style,
                                    gc_language language) {
    GcEditPoint source[GC_EDIT_POINT_LIMIT], output[GC_EDIT_POINT_LIMIT];
    GcEditState editor = {0};
    GcValueMorphState state = {0};
    RandomFixture random = {599, 0, false};
    gc_menu menu;
    gc_menu_init(&menu, geometry->europe ? GC_REGION_EUROPE : GC_REGION_USA);
    menu.page = GC_PAGE_DISC;
    menu.settings.language = language;
    menu.disc_status = GC_DISC_ABSENT;
    assert(gc_edit_state_advance(&editor, geometry, &menu, true, 200));
    assert(gc_value_morph_advance(&state, style, &menu, true, 200, NULL, NULL));
    menu.disc_status = GC_DISC_READY;
    assert(
        gc_value_morph_advance(&state, style, &menu, true, 2, random_sample, &random));
    assert(random.calls == style->sound_offsets);
    size_t source_count = gc_edit_disc_source_points_with_state(
        geometry, language, &editor, source, GC_EDIT_POINT_LIMIT);
    size_t count = gc_value_morph_points(geometry, &menu, &editor, style, &state,
                                         output, GC_EDIT_POINT_LIMIT);
    assert(source_count && count && count < GC_EDIT_POINT_LIMIT);
    assert(gc_value_morph_points(geometry, &menu, &editor, style, &state, NULL, 0) ==
           count);
    const GcIplResource *map = &geometry->maps[language];
    unsigned previous = map_source(map, 0x6c, 0);
    unsigned next = map_source(map, 0x70, 0);
    const GcEditPoint *outgoing = find_point(output, count, previous, 229);
    const GcEditPoint *incoming = find_point(output, count, next, 25);
    assert(outgoing && incoming);
    assert(fabsf(outgoing->position[0] - source[previous].position[0] - 3.1f) < .001f);
    assert(fabsf(incoming->position[0] - source[next].position[0] + 27.9f) < .001f);
    assert(outgoing->field == GC_EDIT_DISC && !outgoing->selected);
    assert(memcmp(outgoing->angles, source[previous].angles,
                  sizeof(outgoing->angles)) == 0);
    assert(memcmp(incoming->angles, source[next].angles, sizeof(incoming->angles)) ==
           0);
    assert(outgoing->angles[1] != 0);
    assert(memcmp(outgoing->colors, geometry->disc_colors, sizeof(outgoing->colors)) ==
           0);
    GcEditPoint sentinel = {.position = {987, 654, 321}};
    output[0] = sentinel;
    assert(!gc_value_morph_points(geometry, &menu, &editor, style, &state, output,
                                  count - 1));
    assert(memcmp(&output[0], &sentinel, sizeof(sentinel)) == 0);
    assert(gc_value_morph_advance(&state, style, &menu, true, 30, NULL, NULL));
    count = gc_value_morph_points(geometry, &menu, &editor, style, &state, output,
                                  GC_EDIT_POINT_LIMIT);
    assert(count == read_u16(map->bytes + 0x70));
    for (size_t index = 0; index < count; ++index)
        assert(output[index].alpha == 255);
    menu.launch_requested = true;
    assert(gc_edit_state_advance(&editor, geometry, &menu, true, 41));
    assert(editor.disc_launching && editor.sampled_tick == 40);
    count = gc_value_morph_points(geometry, &menu, &editor, style, &state, output,
                                  GC_EDIT_POINT_LIMIT);
    assert(count == read_u16(map->bytes + 0x70));
    uint8_t launch_alpha =
        (uint8_t)(255 * (1 - 1.4f * (40.0f / geometry->entrance_ticks - .1f)));
    for (size_t index = 0; index < count; ++index) {
        assert(output[index].alpha == 255); /* The original word copies stay visible. */
        assert(output[index].colors[0][3] == launch_alpha);
    }
}

static void test_native_resources(const char *path) {
    GcText text = {0};
    GcEditGeometry geometry = {0};
    GcValueMorphStyle style = {0};
    assert(gc_text_load(path, &text));
    assert(gc_edit_geometry_decode(&text, &geometry));
    assert(gc_value_morph_style_decode(&text, &style));
    assert(style.duration == 10 && style.spread == .4f && style.tens_multiplier == 2);
    assert(style.sound_offsets == (text.europe ? 1430 : 768));
    assert(style.disc_direction[0] == -1 && style.disc_direction[1] == 0 &&
           style.disc_direction[2] == 0);
    for (unsigned language = 0; language < 7; ++language)
        if (geometry.maps[language].bytes) {
            test_native_sound_points(&geometry, &style, (gc_language)language);
            test_native_number_points(&geometry, &style, (gc_language)language);
            test_native_disc_points(&geometry, &style, (gc_language)language);
        }
    if (text.europe) {
        gc_menu menu;
        gc_menu_init(&menu, GC_REGION_EUROPE);
        menu.page = GC_PAGE_OPTIONS;
        menu.editing = true;
        menu.editor_index = 2;
        menu.settings_before_edit.language = GC_LANGUAGE_ENGLISH;
        menu.settings.language = GC_LANGUAGE_GERMAN;
        assert(gc_edit_geometry_map_language(&menu) == GC_LANGUAGE_ENGLISH);
        GcEditState editor = {0};
        GcEditPoint points[GC_EDIT_POINT_LIMIT];
        assert(gc_edit_state_advance(&editor, &geometry, &menu, true, 200));
        size_t count = gc_edit_geometry_points_with_state(&geometry, &menu, &editor,
                                                          points, GC_EDIT_POINT_LIMIT);
        size_t old_sound = 0;
        for (size_t index = 0; index < count; ++index)
            old_sound += points[index].field == GC_EDIT_SOUND;
        assert(old_sound == 124);
        menu.settings_before_edit.language = GC_LANGUAGE_GERMAN;
        assert(gc_edit_geometry_map_language(&menu) == GC_LANGUAGE_GERMAN);
        count = gc_edit_geometry_points_with_state(&geometry, &menu, &editor, points,
                                                   GC_EDIT_POINT_LIMIT);
        size_t committed_sound = 0;
        for (size_t index = 0; index < count; ++index)
            committed_sound += points[index].field == GC_EDIT_SOUND;
        assert(committed_sound == 150);
    }
    GcValueMorphStyle saved = style;
    size_t saved_size = text.rom_size;
    text.rom_size = 32;
    assert(!gc_value_morph_style_decode(&text, &style));
    assert(memcmp(&style, &saved, sizeof(style)) == 0);
    text.rom_size = saved_size;
    gc_edit_geometry_destroy(&geometry);
    gc_text_destroy(&text);
}

int main(int argc, char **argv) {
    test_sound_order_and_interruption();
    test_disc_order_and_ready_gate();
    test_number_counters_and_wrap();
    test_failure_preserves_state();
    if (argc > 1)
        test_native_resources(argv[1]);
    puts("Value morph tests passed.");
    return 0;
}
