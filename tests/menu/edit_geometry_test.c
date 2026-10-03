#include "gamecube/edit_geometry.h"
#include "console_common/support/endian.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static void put_u16(uint8_t *bytes, unsigned value) {
    bytes[0] = (uint8_t)(value >> 8);
    bytes[1] = (uint8_t)value;
}

static void put_u32(uint8_t *bytes, uint32_t value) {
    bytes[0] = (uint8_t)(value >> 24);
    bytes[1] = (uint8_t)(value >> 16);
    bytes[2] = (uint8_t)(value >> 8);
    bytes[3] = (uint8_t)value;
}

static void put_float(uint8_t *bytes, float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    put_u32(bytes, bits);
}

static void build_motion(uint8_t bytes[188]) {
    memset(bytes, 0, 188);
    memcpy(bytes, "CAFD", 4);
    put_u32(bytes + 4, 1);
    put_u32(bytes + 8, 16);
    put_u32(bytes + 12, 160);
    for (unsigned index = 0; index < 4; ++index) {
        put_float(bytes + 16 + index * 36, (float)index * 10);
        put_float(bytes + 20 + index * 36, (float)index * 2);
        put_float(bytes + 24 + index * 36, (float)index * -3);
    }
    put_u16(bytes + 160, 2);
    put_u16(bytes + 162, 2);
    put_u32(bytes + 164, 8);
    bytes[168] = bytes[176] = 255;
    bytes[169] = 255;
    bytes[177] = 0;
    put_u32(bytes + 172, 16);
    put_u32(bytes + 180, 8);
    put_u16(bytes + 184, 0);
    put_u16(bytes + 186, 3);
}

static void test_motion_and_bounds(void) {
    uint8_t bytes[188];
    GcEditMotion motion = {0};
    GcEditMotion saved;
    float point[3];

    build_motion(bytes);
    assert(gc_edit_motion_decode(bytes, sizeof(bytes), &motion));
    assert(motion.point_count == 4 && motion.group_count == 1);
    assert(gc_edit_motion_point(&motion, 0, 0, 0, point));
    assert(point[0] == 0 && point[1] == 0 && point[2] == 0);
    assert(gc_edit_motion_point(&motion, 0, 0, 0.3f, point));
    assert(fabsf(point[0] - 15) < 0.0001f);
    assert(fabsf(point[1] - 3) < 0.0001f);
    assert(fabsf(point[2] + 4.5f) < 0.0001f);
    assert(gc_edit_motion_point(&motion, 0, 1, 0.3f, point));
    assert(point[0] == 0); /* The second block has the native stagger delay. */
    assert(gc_edit_motion_point(&motion, 0, 1, 1, point));
    assert(point[0] == 30 && point[1] == 6 && point[2] == -9);
    point[0] = 987;
    assert(!gc_edit_motion_point(&motion, 1, 0, 1, point));
    assert(!gc_edit_motion_point(&motion, 0, 2, 1, point));
    assert(!gc_edit_motion_point(&motion, 0, 0, NAN, point));
    assert(point[0] == 987);
    put_float(bytes + 40, 12);  /* First outgoing x tangent. */
    put_float(bytes + 136, -4); /* Last incoming x tangent. */
    assert(gc_edit_motion_point(&motion, 0, 0, 0.3f, point));
    assert(fabsf(point[0] - 17) < 0.0001f);
    saved = motion;
    put_u32(bytes + 4, 0);
    assert(!gc_edit_motion_decode(bytes, sizeof(bytes), &motion));
    assert(memcmp(&motion, &saved, sizeof(motion)) == 0);
    build_motion(bytes);
    put_u16(bytes + 186, 4);
    assert(!gc_edit_motion_decode(bytes, sizeof(bytes), &motion));
    build_motion(bytes);
    put_u32(bytes + 172, UINT32_MAX);
    assert(!gc_edit_motion_decode(bytes, sizeof(bytes), &motion));
    build_motion(bytes);
    put_u32(bytes + 172, 4);
    assert(!gc_edit_motion_decode(bytes, sizeof(bytes), &motion));
    build_motion(bytes);
    put_float(bytes + 16, NAN);
    assert(!gc_edit_motion_decode(bytes, sizeof(bytes), &motion));
    build_motion(bytes);
    assert(!gc_edit_motion_decode(bytes, 187, &motion));
}

static void test_palette_blend(void) {
    GcEditGeometry geometry = {0};
    uint8_t colors[2][4];
    uint8_t saved[2][4];
    for (unsigned panel = 0; panel < 2; ++panel)
        for (unsigned reg = 0; reg < 2; ++reg)
            for (unsigned channel = 0; channel < 4; ++channel) {
                geometry.palettes[panel][0][reg][channel] = 20;
                geometry.palettes[panel][1][reg][channel] = 150;
                geometry.palettes[panel][2][reg][channel] = 10;
            }
    assert(gc_edit_geometry_colors(&geometry, false, 0.25f, 0.5f, colors));
    assert(colors[0][0] == 32 && colors[1][3] == 32);
    assert(gc_edit_geometry_colors(&geometry, true, 1, 0, colors));
    assert(colors[0][0] == 150);
    assert(gc_edit_geometry_colors(&geometry, true, 0, 1, colors));
    assert(colors[0][0] == 20);
    memcpy(saved, colors, sizeof(saved));
    assert(!gc_edit_geometry_colors(&geometry, false, NAN, 0, colors));
    assert(!gc_edit_geometry_colors(&geometry, false, 1.01f, 0, colors));
    assert(!gc_edit_geometry_colors(&geometry, false, 0, -1, colors));
    assert(memcmp(colors, saved, sizeof(saved)) == 0);
}

static float mean_field_x(const GcEditPoint *points, size_t count, GcEditField field) {
    float total = 0;
    unsigned matches = 0;
    for (size_t index = 0; index < count; ++index)
        if (points[index].field == field) {
            total += points[index].position[0];
            ++matches;
        }
    assert(matches);
    return total / (float)matches;
}

static void test_selected_movement(const GcEditGeometry *geometry,
                                   gc_language language) {
    gc_menu menu;
    GcEditState state;
    GcEditPoint baseline[GC_EDIT_POINT_LIMIT], moving[GC_EDIT_POINT_LIMIT];
    gc_menu_init(&menu, geometry->europe ? GC_REGION_EUROPE : GC_REGION_USA);
    menu.settings.language = language;
    menu.clock = (gc_date_time){2024, 2, 29, 12, 34, 56};
    for (unsigned page = 0; page < 2; ++page) {
        menu.page = page ? GC_PAGE_CALENDAR : GC_PAGE_OPTIONS;
        menu.editor_index = 0;
        menu.editing = false;
        gc_edit_state_init(&state);
        unsigned gate = (unsigned)floorf((float)geometry->entrance_ticks *
                                         geometry->motion_profile[5]) +
                        1;
        assert(gc_edit_state_advance(&state, geometry, &menu, false, 200));
        assert(state.motion_counter == 0 && state.sampled_motion == 0);
        assert(gc_edit_state_advance(&state, geometry, &menu, true, gate));
        assert(state.motion_counter == 0 && state.sampled_motion == 0);
        assert(gc_edit_state_advance(&state, geometry, &menu, true, 1));
        assert(state.motion_counter == 1 && state.sampled_motion == 0);
        assert(gc_edit_state_advance(&state, geometry, &menu, true,
                                     geometry->motion_ticks));
        assert(state.motion_counter == 1 && state.sampled_motion == 0);
        assert(gc_edit_state_advance(&state, geometry, &menu, true, 30));
        assert(state.sampled_motion == 30);
        size_t count =
            gc_edit_geometry_points(geometry, &menu, 1, baseline, GC_EDIT_POINT_LIMIT);
        assert(count &&
               gc_edit_geometry_points_with_state(geometry, &menu, &state, moving,
                                                  GC_EDIT_POINT_LIMIT) == count);
        bool animated = false, stationary = false;
        for (size_t index = 0; index < count; ++index) {
            assert(moving[index].source_index == baseline[index].source_index);
            if (moving[index].selected) {
                assert(moving[index].angles[1] == 5461);
                assert(moving[index].angles[2] == 546);
                animated |= memcmp(moving[index].position, baseline[index].position,
                                   sizeof(moving[index].position)) != 0;
            } else {
                assert(moving[index].angles[1] == 0 && moving[index].angles[2] == 0);
                assert(memcmp(moving[index].position, baseline[index].position,
                              sizeof(moving[index].position)) == 0);
                stationary = true;
            }
        }
        assert(animated && stationary);
        GcEditState saved = state;
        assert(gc_edit_state_advance(&state, geometry, &menu, true, 0));
        assert(memcmp(&saved, &state, sizeof(state)) == 0);
        menu.editing = true;
        assert(gc_edit_state_advance(&state, geometry, &menu, true, 11));
        assert(gc_edit_geometry_points_with_state(geometry, &menu, &state, moving,
                                                  GC_EDIT_POINT_LIMIT) >= count);
        bool selected = false;
        for (size_t index = 0; index < count; ++index)
            if (moving[index].selected) {
                assert(moving[index].angles[1] == 0 && moving[index].angles[2] == 0);
                selected = true;
            }
        assert(selected);
        state.motion_counter = geometry->motion_ticks;
        saved = state;
        assert(!gc_edit_state_advance(&state, geometry, &menu, true, 1));
        assert(memcmp(&saved, &state, sizeof(state)) == 0);
    }
}

static void test_disc_continuous_movement(const GcEditGeometry *geometry,
                                          gc_language language) {
    gc_menu menu;
    GcEditState state = {0};
    GcEditPoint baseline[GC_EDIT_POINT_LIMIT], moving[GC_EDIT_POINT_LIMIT];
    gc_menu_init(&menu, geometry->europe ? GC_REGION_EUROPE : GC_REGION_USA);
    menu.settings.language = language;
    menu.page = GC_PAGE_DISC;
    menu.disc_status = GC_DISC_LID_OPEN;
    unsigned gate = (unsigned)floorf((float)geometry->entrance_ticks *
                                     geometry->motion_profile[5]) +
                    1;
    assert(gc_edit_state_advance(&state, geometry, &menu, false, 200));
    assert(state.sampled_tick == 0 && state.motion_counter == 0);
    assert(!gc_edit_disc_source_points_with_state(geometry, language, &state, moving,
                                                  GC_EDIT_POINT_LIMIT));
    assert(gc_edit_state_advance(&state, geometry, &menu, true, gate));
    assert(state.sampled_motion == 0 && state.motion_counter == 0);
    assert(gc_edit_state_advance(&state, geometry, &menu, true, 1));
    assert(state.sampled_motion == 0 && state.motion_counter == 1);
    assert(gc_edit_state_advance(&state, geometry, &menu, true, 30));
    assert(state.sampled_motion == 30);
    /* Finish the entrance while retaining the same phase for this comparison. */
    assert(
        gc_edit_state_advance(&state, geometry, &menu, true, geometry->motion_ticks));
    size_t count = gc_edit_disc_source_points(geometry, language, 1, baseline,
                                              GC_EDIT_POINT_LIMIT);
    assert(count &&
           gc_edit_disc_source_points_with_state(geometry, language, &state, moving,
                                                 GC_EDIT_POINT_LIMIT) == count);
    assert(moving[0].angles[0] == 0 && moving[0].angles[1] == 5461 &&
           moving[0].angles[2] == 546);
    bool depth_changed = false;
    for (size_t index = 0; index < count; ++index) {
        assert(moving[index].source_index == baseline[index].source_index);
        assert(memcmp(moving[index].angles, moving[0].angles,
                      sizeof(moving[index].angles)) == 0);
        depth_changed |=
            fabsf(moving[index].position[2] - baseline[index].position[2]) > 1;
    }
    assert(depth_changed);
    double original_distance = 0, moving_distance = 0;
    for (unsigned axis = 0; axis < 3; ++axis) {
        double first = baseline[0].position[axis] - baseline[count - 1].position[axis];
        double second = moving[0].position[axis] - moving[count - 1].position[axis];
        original_distance += first * first;
        moving_distance += second * second;
    }
    assert(fabs(original_distance - moving_distance) < .02);
    GcEditState saved = state;
    assert(gc_edit_state_advance(&state, geometry, &menu, true, 0));
    assert(memcmp(&state, &saved, sizeof(state)) == 0);
    assert(
        gc_edit_state_advance(&state, geometry, &menu, true, geometry->motion_ticks));
    assert(memcmp(&state, &saved, sizeof(state)) == 0);
    GcEditState reduced = state;
    uint64_t huge = UINT64_MAX;
    assert(gc_edit_state_advance(&state, geometry, &menu, true, huge));
    assert(
        gc_edit_state_advance(&reduced, geometry, &menu, true,
                              geometry->motion_ticks + huge % geometry->motion_ticks));
    assert(memcmp(&state, &reduced, sizeof(state)) == 0);
    /* Changing the word keeps the ongoing whole-group phase and geometry. */
    menu.disc_status = GC_DISC_READY;
    assert(gc_edit_disc_status_points_with_state(geometry, language, menu.disc_status,
                                                 &state, moving,
                                                 GC_EDIT_POINT_LIMIT) > 40);
    assert(state.sampled_motion == reduced.sampled_motion);
    saved = state;
    assert(gc_edit_state_advance(&state, geometry, &menu, false, 600));
    assert(state.sampled_motion == saved.sampled_motion &&
           state.motion_counter == saved.motion_counter);
    state.ready = true;
    state.sampled_motion = geometry->motion_ticks;
    GcEditPoint sentinel = {.position = {987, 654, 321}};
    moving[0] = sentinel;
    assert(!gc_edit_disc_source_points_with_state(geometry, language, &state, moving,
                                                  GC_EDIT_POINT_LIMIT));
    assert(memcmp(&moving[0], &sentinel, sizeof(sentinel)) == 0);
}

static size_t points_of_field(const GcEditPoint *points, size_t count,
                              GcEditField field) {
    size_t found = 0;
    for (size_t i = 0; i < count; ++i)
        if (points[i].field == field && points[i].alpha)
            ++found;
    return found;
}

static void test_disc_launch(const GcEditGeometry *geometry, gc_language language) {
    gc_menu menu;
    GcEditState state = {0};
    GcEditPoint baseline[GC_EDIT_POINT_LIMIT], launched[GC_EDIT_POINT_LIMIT];
    gc_menu_init(&menu, geometry->europe ? GC_REGION_EUROPE : GC_REGION_USA);
    menu.page = GC_PAGE_DISC;
    menu.settings.language = language;
    menu.disc_status = GC_DISC_READY;
    assert(gc_edit_state_advance(&state, geometry, &menu, true, 200));
    unsigned previous_motion = state.sampled_motion;
    menu.launch_requested = true;
    GcEditState paused = state;
    assert(gc_edit_state_advance(&state, geometry, &menu, true, 0));
    assert(memcmp(&state, &paused, sizeof(state)) == 0);
    assert(gc_edit_state_advance(&state, geometry, &menu, true, 1));
    assert(state.disc_launching && state.entrance_counter == 1 && !state.sampled_tick);
    assert(state.disc_launch_motion == previous_motion &&
           state.disc_launch_motion_active);
    paused = state;
    assert(gc_edit_state_advance(&state, geometry, &menu, false, 200));
    assert(state.entrance_counter == paused.entrance_counter &&
           state.sampled_motion == paused.sampled_motion);
    state.ready = true;
    state.disc_launch_motion_active = false; /* Isolate the launch transform. */
    size_t count = gc_edit_disc_source_points(geometry, language, 1, baseline,
                                              GC_EDIT_POINT_LIMIT);
    assert(count == geometry->disc_launch.point_count);
    assert(gc_edit_disc_source_points_with_state(geometry, language, &state, launched,
                                                 GC_EDIT_POINT_LIMIT) == count);
    for (size_t index = 0; index < count; ++index)
        for (unsigned axis = 0; axis < 3; ++axis)
            assert(fabsf(launched[index].position[axis] -
                         baseline[index].position[axis]) < .001f);
    state.sampled_tick = 3;
    float progress = 3.0f / geometry->entrance_ticks;
    assert(gc_edit_disc_source_points_with_state(geometry, language, &state, launched,
                                                 GC_EDIT_POINT_LIMIT) == count);
    float local = (10 * progress - .1f) / .9f;
    float factor = 1 - .05f * (1 - local * local * (3 - 2 * local));
    assert(fabsf(launched[0].scale - .8f * factor * factor) < .00001f);
    double original_distance = 0, compressed_distance = 0;
    for (unsigned axis = 0; axis < 3; ++axis) {
        double original =
            baseline[0].position[axis] - baseline[count - 1].position[axis];
        double compressed =
            launched[0].position[axis] - launched[count - 1].position[axis];
        original_distance += original * original;
        compressed_distance += compressed * compressed;
    }
    assert(fabs(compressed_distance / original_distance - factor * factor) < .00001);
    /* Native stagger timing is indexed by the complete source group, so
     * separate words retain the same underlying dispersal trajectory. */
    state.sampled_tick = 40;
    progress = 40.0f / geometry->entrance_ticks;
    assert(gc_edit_disc_source_points_with_state(geometry, language, &state, launched,
                                                 GC_EDIT_POINT_LIMIT) == count);
    uint8_t expected_alpha = (uint8_t)(255 * (1 - 1.4f * (progress - .1f)));
    unsigned displaced = 0;
    for (size_t index = 0; index < count; ++index) {
        assert(launched[index].colors[0][3] == expected_alpha);
        assert(launched[index].colors[1][3] == 255);
        assert(launched[index].scale == .8f);
        assert(launched[index].angles[0] == 0 && launched[index].angles[2] == 0);
        displaced +=
            fabsf(launched[index].position[2] - baseline[index].position[2]) > 5;
    }
    assert(displaced > count / 2);
    GcEditPoint word[GC_EDIT_POINT_LIMIT];
    size_t visible = gc_edit_disc_status_points_with_state(
        geometry, language, GC_DISC_READY, &state, word, GC_EDIT_POINT_LIMIT);
    assert(visible > 40 && visible < count);
    for (size_t index = 0; index < visible; ++index)
        assert(memcmp(&word[index], &launched[word[index].source_index],
                      sizeof(word[index])) == 0);
    GcEditGeometry malformed = *geometry;
    malformed.disc_launch.point_count = 0;
    GcEditPoint sentinel = {.position = {987, 654, 321}};
    word[0] = sentinel;
    assert(!gc_edit_disc_source_points_with_state(&malformed, language, &state, word,
                                                  GC_EDIT_POINT_LIMIT));
    assert(memcmp(word, &sentinel, sizeof(sentinel)) == 0);
    state = paused;
    GcEditState replay = state;
    assert(gc_edit_state_advance(&state, geometry, &menu, true, 40));
    for (unsigned tick = 0; tick < 40; ++tick)
        assert(gc_edit_state_advance(&replay, geometry, &menu, true, 1));
    assert(memcmp(&state, &replay, sizeof(state)) == 0);
    assert(gc_edit_state_advance(&state, geometry, &menu, true, 100));
    assert(state.sampled_tick == geometry->entrance_ticks && state.disc_launching);
    assert(gc_edit_disc_source_points_with_state(geometry, language, &state, launched,
                                                 GC_EDIT_POINT_LIMIT) == count);
    for (size_t index = 0; index < count; ++index)
        assert(launched[index].colors[0][3] == 0);
}

static void test_options_native_parts(const GcEditGeometry *geometry,
                                      gc_language language) {
    gc_menu menu;
    GcEditState state;
    GcEditPoint points[GC_EDIT_POINT_LIMIT];
    gc_menu_init(&menu, geometry->europe ? GC_REGION_EUROPE : GC_REGION_USA);
    menu.settings.language = language;
    menu.page = GC_PAGE_OPTIONS;
    gc_edit_state_init(&state);
    assert(geometry->option_heading_color == 0x00ff78ff);
    assert(geometry->arrow_ticks == 25);
    assert(geometry->screen_bar_ticks[0] == 30 && geometry->screen_bar_ticks[1] == 10);
    assert(gc_edit_state_advance(&state, geometry, &menu, true, 100));
    size_t count = gc_edit_geometry_points_with_state(geometry, &menu, &state, points,
                                                      GC_EDIT_POINT_LIMIT);
    assert(count && points_of_field(points, count, GC_EDIT_ARROW) == 0);
    if (geometry->europe)
        assert(points_of_field(points, count, GC_EDIT_LANGUAGE) == 7);
    menu.editing = true;
    menu.editor_index = 0;
    menu.settings.sound = GC_SOUND_STEREO;
    assert(gc_edit_state_advance(&state, geometry, &menu, true, 11));
    count = gc_edit_geometry_points_with_state(geometry, &menu, &state, points,
                                               GC_EDIT_POINT_LIMIT);
    assert(points_of_field(points, count, GC_EDIT_ARROW) == 9);
    assert(points_of_field(points, count, GC_EDIT_SCREEN_POSITION) > 0);
    unsigned phase = state.sampled_arrow;
    GcEditState saved = state;
    assert(gc_edit_state_advance(&state, geometry, &menu, true, 0));
    assert(memcmp(&state, &saved, sizeof(state)) == 0);
    assert(gc_edit_state_advance(&state, geometry, &menu, true, 25));
    assert(state.sampled_arrow == phase);
    menu.settings.sound = GC_SOUND_MONO;
    assert(gc_edit_state_advance(&state, geometry, &menu, true, 11));
    assert(state.arrows[0] == 0 && state.arrows[1] == geometry->selection_ticks);
    menu.editor_index = 1;
    assert(gc_edit_state_advance(&state, geometry, &menu, true, 1));
    assert(state.screen_bar_state == 1 && state.screen_bar_counter == 0);
    assert(gc_edit_state_advance(&state, geometry, &menu, true, 30));
    count = gc_edit_geometry_points_with_state(geometry, &menu, &state, points,
                                               GC_EDIT_POINT_LIMIT);
    assert(points_of_field(points, count, GC_EDIT_SCREEN_BAR) == 74);
    assert(points_of_field(points, count, GC_EDIT_ARROW) == 18);
    menu.settings.screen_position = GC_SCREEN_POSITION_MIN;
    assert(gc_edit_state_advance(&state, geometry, &menu, true, 11));
    assert(state.arrows[2] == 0 && state.arrows[3] == geometry->selection_ticks);
    menu.editing = false;
    assert(gc_edit_state_advance(&state, geometry, &menu, true, 1));
    assert(state.screen_bar_state == 2 && state.screen_bar_counter == 0);
    assert(gc_edit_state_advance(&state, geometry, &menu, true, 9));
    assert(state.screen_bar_counter == 9);
    assert(gc_edit_state_advance(&state, geometry, &menu, true, 1));
    assert(state.screen_bar_state == 0);
    if (geometry->europe) {
        menu.editor_index = 2;
        menu.editing = true;
        menu.settings.language = GC_LANGUAGE_ENGLISH;
        assert(gc_edit_state_advance(&state, geometry, &menu, true, 11));
        count = gc_edit_geometry_points_with_state(geometry, &menu, &state, points,
                                                   GC_EDIT_POINT_LIMIT);
        assert(points_of_field(points, count, GC_EDIT_LANGUAGE) == 7);
        assert(points_of_field(points, count, GC_EDIT_ARROW) == 9);
        float before = mean_field_x(points, count, GC_EDIT_LANGUAGE);
        menu.settings.language = GC_LANGUAGE_DUTCH;
        assert(gc_edit_state_advance(&state, geometry, &menu, true, 1));
        assert(state.language_movement == 0);
        assert(gc_edit_state_advance(&state, geometry, &menu, true, 10));
        assert(state.language_movement == geometry->selection_ticks);
        count = gc_edit_geometry_points_with_state(geometry, &menu, &state, points,
                                                   GC_EDIT_POINT_LIMIT);
        assert(mean_field_x(points, count, GC_EDIT_LANGUAGE) > before + 300);
        assert(state.arrows[4] == geometry->selection_ticks && state.arrows[5] == 0);
    }
    state.screen_bar_state = 3;
    saved = state;
    assert(!gc_edit_state_advance(&state, geometry, &menu, true, 1));
    assert(memcmp(&state, &saved, sizeof(state)) == 0);
}

static void test_pal_resource_languages(const GcText *text,
                                        const GcEditGeometry *geometry) {
    if (!text->europe)
        return;
    /* Read the original constructor's li/stw pairs, rather than repeating
     * the runtime's resource IDs. Each destination is a SRAM locale slot. */
    static const struct {
        size_t load;
        size_t store;
        unsigned register_index;
    } selectors[6] = {{0xbfd8, 0xc048, 8}, {0xbfe4, 0xc050, 6}, {0xbfe0, 0xc04c, 7},
                      {0xbff0, 0xc058, 4}, {0xbff8, 0xc05c, 0}, {0xbfec, 0xc054, 5}};
    GcIplResourceTable table;
    assert(gc_ipl_resource_table_decode(text->rom, text->rom_size, 0x82040, &table));
    for (unsigned language = GC_LANGUAGE_ENGLISH; language <= GC_LANGUAGE_DUTCH;
         ++language) {
        assert(selectors[language].load <= text->rom_size - 4);
        assert(selectors[language].store <= text->rom_size - 4);
        uint32_t load = cc_read_be32(text->rom + selectors[language].load);
        uint32_t store = cc_read_be32(text->rom + selectors[language].store);
        assert(load >> 26 == 14 && (load >> 16 & 31) == 0);
        assert((load >> 21 & 31) == selectors[language].register_index);
        assert(store >> 26 == 36 && (store >> 16 & 31) == 3);
        assert((store >> 21 & 31) == selectors[language].register_index);
        assert((store & 0xffff) == 0x150 + language * 4);
        GcIplResource original = {0};
        assert(gc_ipl_resource_unpack(&table, load & 0xffff, &original));
        assert(geometry->maps[language].byte_count == original.byte_count);
        assert(memcmp(geometry->maps[language].bytes, original.bytes,
                      original.byte_count) == 0);
        gc_ipl_resource_destroy(&original);
    }
}

static void test_original_rom(const char *path) {
    GcText text = {0};
    GcEditGeometry geometry = {0};
    gc_menu menu;
    GcEditPoint points[GC_EDIT_POINT_LIMIT];
    size_t count;

    assert(gc_text_load(path, &text));
    assert(gc_edit_geometry_decode(&text, &geometry));
    test_pal_resource_languages(&text, &geometry);
    assert(!gc_edit_geometry_decode(&text, &geometry));
    assert(geometry.motion.group_count == 5);
    assert(geometry.entrance_ticks == (text.europe ? 58 : 70));
    assert(geometry.selection_ticks == 10);
    assert(geometry.language_fade_ticks == 20);
    assert(geometry.motion_ticks == 120 && geometry.angle_units == 65535);
    assert(geometry.motion_profile[0] == 2 && geometry.motion_profile[1] == 5);
    assert(geometry.motion_profile[2] == 30 && geometry.motion_profile[3] == 3);
    assert(geometry.disc_launch.compression_end == .1f);
    assert(geometry.disc_launch.compression_time == 10);
    assert(geometry.disc_launch.compression_amount == .05f);
    assert(geometry.disc_launch.fade_speed == 1.4f);
    assert(geometry.disc_launch.delay_span == .4f);
    assert(geometry.disc_launch.translation[0] == -585 &&
           geometry.disc_launch.translation[1] == 200 &&
           geometry.disc_launch.translation[2] == -500);
    assert(fabsf(geometry.motion_profile[4] - 0.45f) < 0.00001f);
    assert(fabsf(geometry.motion_profile[5] - 0.9f) < 0.00001f);
    assert(fabsf(geometry.transforms[0][0] - 10) < 0.0001f);
    assert(fabsf(geometry.transforms[0][1] + (text.europe ? 84 : 11)) < 0.0001f);
    assert(fabsf(geometry.transforms[0][2] - (text.europe ? 0.55f : 0.82f)) < 0.0001f);
    assert(fabsf(geometry.transforms[1][1] - (text.europe ? -35 : 13)) < 0.0001f);
    assert(fabsf(geometry.transforms[1][2] - (text.europe ? 0.64f : 0.82f)) < 0.0001f);
    assert(fabsf(geometry.transforms[2][1] + 46) < 0.0001f);
    assert(fabsf(geometry.transforms[3][1] + 20) < 0.0001f);
    assert(fabsf(geometry.transforms[2][2] - (text.europe ? 0.74f : 0.82f)) < 0.0001f);
    assert(geometry.palettes[0][0][0][0] == 180);
    assert(geometry.palettes[0][0][0][1] == 255);
    assert(geometry.palettes[1][0][0][0] == 240);
    static const uint8_t disc_colors[2][2][4] = {
        {{255, 130, 150, 255}, {120, 10, 60, 255}},
        {{130, 50, 160, 255}, {50, 20, 70, 255}}};
    assert(memcmp(geometry.disc_colors, disc_colors[text.europe],
                  sizeof(geometry.disc_colors)) == 0);
    assert(geometry.transforms[4][0] == -8 && geometry.transforms[4][1] == -8);
    assert(fabsf(geometry.transforms[4][2] - 0.8f) < 0.0001f);
    for (unsigned language = 0; language < 7; ++language) {
        bool available = text.europe ? language != GC_LANGUAGE_JAPANESE
                                     : language == GC_LANGUAGE_ENGLISH ||
                                           language == GC_LANGUAGE_JAPANESE;
        if (!available)
            continue;
        test_selected_movement(&geometry, (gc_language)language);
        test_disc_continuous_movement(&geometry, (gc_language)language);
        test_disc_launch(&geometry, (gc_language)language);
        test_options_native_parts(&geometry, (gc_language)language);
        size_t no_disc = gc_edit_disc_points(&geometry, (gc_language)language, false, 1,
                                             points, GC_EDIT_POINT_LIMIT);
        assert(no_disc > 40 && no_disc < 712);
        for (size_t index = 0; index < no_disc; ++index) {
            assert(points[index].field == GC_EDIT_DISC);
            assert(fabsf(points[index].scale - 0.8f) < 0.0001f);
            assert(fabsf(points[index].position[2] - 100) < 0.0001f);
            assert(memcmp(points[index].colors, geometry.disc_colors,
                          sizeof(points[index].colors)) == 0);
        }
        GcEditPoint disc_saved = points[0];
        assert(gc_edit_disc_points(&geometry, (gc_language)language, false, 1, points,
                                   no_disc - 1) == 0);
        assert(memcmp(points, &disc_saved, sizeof(disc_saved)) == 0);
        size_t press_start = gc_edit_disc_points(&geometry, (gc_language)language, true,
                                                 1, points, GC_EDIT_POINT_LIMIT);
        assert(press_start > 40 && press_start < 712 && press_start != no_disc);
        assert(gc_edit_disc_points(&geometry, (gc_language)language, true, 1, NULL,
                                   0) == press_start);
        assert(gc_edit_disc_points(&geometry, (gc_language)language, true, NAN, points,
                                   GC_EDIT_POINT_LIMIT) == 0);
        assert(gc_edit_disc_status_points(&geometry, (gc_language)language,
                                          GC_DISC_ABSENT, 1, NULL, 0) == no_disc);
        assert(gc_edit_disc_status_points(&geometry, (gc_language)language,
                                          GC_DISC_READY, 1, NULL, 0) == press_start);
        size_t unreadable = gc_edit_disc_status_points(&geometry, (gc_language)language,
                                                       GC_DISC_UNREADABLE, 1, points,
                                                       GC_EDIT_POINT_LIMIT);
        assert(unreadable > 40 && unreadable < 712);
        assert(unreadable != no_disc && unreadable != press_start);
        assert(gc_edit_disc_status_points(&geometry, (gc_language)language,
                                          GC_DISC_LID_OPEN, 1, NULL, 0) == unreadable);
        gc_menu_init(&menu, text.europe ? GC_REGION_EUROPE : GC_REGION_USA);
        menu.settings.language = (gc_language)language;
        menu.clock = (gc_date_time){2024, 2, 29, 12, 34, 56};
        menu.page = GC_PAGE_CALENDAR;
        GcEditState state;
        gc_edit_state_init(&state);
        assert(gc_edit_state_advance(&state, &geometry, &menu, false, 100));
        assert(state.active && !state.ready && state.entrance_counter == 0);
        assert(gc_edit_geometry_points_with_state(&geometry, &menu, &state, points,
                                                  GC_EDIT_POINT_LIMIT) == 0);
        assert(gc_edit_state_advance(&state, &geometry, &menu, true, 1));
        assert(state.entrance_counter == 1 && state.sampled_tick == 0);
        assert(gc_edit_state_progress(&state, &geometry) == 0);
        count = gc_edit_geometry_points_with_state(&geometry, &menu, &state, points,
                                                   GC_EDIT_POINT_LIMIT);
        assert(count > 180);
        unsigned first_selected = UINT32_MAX;
        for (size_t index = 0; index < count; ++index) {
            if (points[index].selected) {
                first_selected = points[index].source_index;
                assert(state.selection[first_selected] == 0);
                assert(state.next_selection[first_selected] == 1);
            }
            assert(memcmp(points[index].colors, geometry.palettes[1][2],
                          sizeof(points[index].colors)) == 0);
        }
        assert(first_selected != UINT32_MAX);
        assert(gc_edit_state_advance(&state, &geometry, &menu, true, 5));
        assert(state.sampled_tick == 5 && state.selection[first_selected] == 5);
        assert(state.next_selection[first_selected] == 6);
        menu.editor_index = 3;
        assert(gc_edit_state_advance(&state, &geometry, &menu, true, 1));
        assert(state.selection[first_selected] == 6);
        assert(state.next_selection[first_selected] == 5);
        assert(gc_edit_state_advance(&state, &geometry, &menu, true, 5));
        assert(state.selection[first_selected] == 1);
        assert(state.next_selection[first_selected] == 0);
        assert(gc_edit_state_advance(&state, &geometry, &menu, true, UINT64_MAX));
        assert(state.sampled_tick == geometry.entrance_ticks);
        assert(gc_edit_state_progress(&state, &geometry) == 1);
        assert(state.selection[first_selected] == 0);
        GcEditState state_saved = state;
        GcEditGeometry invalid_geometry = geometry;
        invalid_geometry.selection_ticks = 0;
        assert(!gc_edit_state_advance(&state, &invalid_geometry, &menu, true, 1));
        assert(memcmp(&state_saved, &state, sizeof(state)) == 0);
        state.entrance_counter = (uint16_t)(geometry.entrance_ticks + 1);
        state_saved = state;
        assert(!gc_edit_state_advance(&state, &geometry, &menu, true, 1));
        assert(memcmp(&state_saved, &state, sizeof(state)) == 0);
        gc_edit_state_init(&state);
        assert(gc_edit_state_advance(&state, &geometry, &menu, true, 10));
        menu.page = GC_PAGE_OPTIONS;
        menu.editor_index = 0;
        assert(gc_edit_state_advance(&state, &geometry, &menu, true, 1));
        assert(state.page == GC_PAGE_OPTIONS && state.sampled_tick == 0);
        if (text.europe) {
            assert(state.language_labels[language] == 1);
            assert(state.language_captions[language] == 1);
            assert(gc_edit_state_advance(&state, &geometry, &menu, false, 19));
            assert(!state.ready && state.sampled_tick == 0);
            for (unsigned row = 0; row < 6; ++row) {
                assert(gc_edit_language_alpha(&state, &geometry, (gc_language)row,
                                              false) == (row == language ? 255 : 127));
                assert(gc_edit_language_alpha(&state, &geometry, (gc_language)row,
                                              true) == (row == language ? 255 : 0));
            }
            unsigned next_language = (language + 1) % 6;
            menu.settings.language = (gc_language)next_language;
            assert(gc_edit_state_advance(&state, &geometry, &menu, false, 10));
            assert(gc_edit_language_alpha(&state, &geometry, (gc_language)language,
                                          false) == 127);
            assert(gc_edit_language_alpha(&state, &geometry, (gc_language)next_language,
                                          false) == 255);
            assert(gc_edit_language_alpha(&state, &geometry, (gc_language)language,
                                          true) == 127);
            assert(gc_edit_language_alpha(&state, &geometry, (gc_language)next_language,
                                          true) == 127);
            GcEditState captions_saved = state;
            assert(gc_edit_state_advance(&state, &geometry, &menu, false, 0));
            assert(memcmp(&state, &captions_saved, sizeof(state)) == 0);
            assert(gc_edit_state_advance(&state, &geometry, &menu, false, 10));
            assert(gc_edit_language_alpha(&state, &geometry, (gc_language)language,
                                          true) == 0);
            assert(gc_edit_language_alpha(&state, &geometry, (gc_language)next_language,
                                          true) == 255);
            state.language_labels[0] = 21;
            captions_saved = state;
            assert(!gc_edit_state_advance(&state, &geometry, &menu, false, 1));
            assert(memcmp(&state, &captions_saved, sizeof(state)) == 0);
            state.language_labels[0] = 10;
            menu.settings.language = (gc_language)language;
        }
        menu.page = GC_PAGE_CUBE;
        assert(gc_edit_state_advance(&state, &geometry, &menu, true, 1));
        assert(!state.active && state.sampled_tick == 0);
        menu.page = GC_PAGE_CALENDAR;
        menu.editor_index = 0;
        count =
            gc_edit_geometry_points(&geometry, &menu, 1, points, GC_EDIT_POINT_LIMIT);
        assert(count > 180 && count < 555);
        assert(gc_edit_geometry_points(&geometry, &menu, 1, NULL, 0) == count);
        for (size_t index = 0; index < count; ++index) {
            assert(fabsf(points[index].position[2] - 100) < 0.0001f);
            assert(points[index].scale > 0 && points[index].scale <= 1);
            assert(points[index].colors[0][3] == 255);
        }
        float day = mean_field_x(points, count, GC_EDIT_DAY);
        float month = mean_field_x(points, count, GC_EDIT_MONTH);
        float year = mean_field_x(points, count, GC_EDIT_YEAR);
        if (language == GC_LANGUAGE_JAPANESE)
            assert(year < month && month < day);
        else if (text.europe)
            assert(day < month && month < year);
        else
            assert(month < day && day < year);
        menu.clock = (gc_date_time){2026, 10, 1, 12, 34, 56};
        assert(gc_date_time_weekday(&menu.clock) == 4);
        count =
            gc_edit_geometry_points(&geometry, &menu, 1, points, GC_EDIT_POINT_LIMIT);
        day = mean_field_x(points, count, GC_EDIT_DAY);
        month = mean_field_x(points, count, GC_EDIT_MONTH);
        year = mean_field_x(points, count, GC_EDIT_YEAR);
        if (language == GC_LANGUAGE_JAPANESE)
            assert(year < month && month < day);
        else if (text.europe)
            assert(day < month && month < year);
        else
            assert(month < day && day < year);
        menu.clock.day = 2;
        assert(gc_date_time_weekday(&menu.clock) == 5);
        GcEditPoint saved = points[0];
        assert(gc_edit_geometry_points(&geometry, &menu, 1, points, count - 1) == 0);
        assert(memcmp(&points[0], &saved, sizeof(saved)) == 0);
        menu.editing = true;
        menu.editor_index = GC_CALENDAR_MINUTE;
        count =
            gc_edit_geometry_points(&geometry, &menu, 1, points, GC_EDIT_POINT_LIMIT);
        unsigned selected = 0;
        for (size_t index = 0; index < count; ++index)
            if (points[index].selected) {
                assert(points[index].field == GC_EDIT_MINUTE);
                ++selected;
            }
        assert(selected);
        menu.page = GC_PAGE_OPTIONS;
        menu.editor_index = 0;
        menu.settings.sound = GC_SOUND_MONO;
        menu.settings.screen_position = -32;
        size_t mono =
            gc_edit_geometry_points(&geometry, &menu, 1, points, GC_EDIT_POINT_LIMIT);
        assert(mono > 40 && mono < 478);
        selected = 0;
        for (size_t index = 0; index < mono; ++index)
            if (points[index].selected) {
                assert(points[index].field == GC_EDIT_SOUND);
                ++selected;
            }
        assert(selected);
        menu.settings.sound = GC_SOUND_STEREO;
        size_t stereo =
            gc_edit_geometry_points(&geometry, &menu, 1, points, GC_EDIT_POINT_LIMIT);
        assert(stereo != mono);
        menu.editor_index = 1;
        count =
            gc_edit_geometry_points(&geometry, &menu, 1, points, GC_EDIT_POINT_LIMIT);
        selected = 0;
        for (size_t index = 0; index < count; ++index)
            if (points[index].selected) {
                assert(points[index].field == GC_EDIT_SCREEN_POSITION);
                ++selected;
            }
        assert(selected);
    }
    gc_edit_geometry_destroy(&geometry);
    assert(!geometry.motion_owner.bytes && !geometry.motion.bytes);
    gc_text_destroy(&text);
}

int main(int argc, char **argv) {
    test_motion_and_bounds();
    test_palette_blend();
    for (int index = 1; index < argc; ++index)
        test_original_rom(argv[index]);
    puts("edit geometry tests passed");
    return 0;
}
