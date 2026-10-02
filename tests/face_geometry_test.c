#include "gamecube/face_geometry.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static GcFaceGeometry synthetic(void) {
    GcFaceGeometry g = {0};
    g.frame_rate = 60;
    for (unsigned i = 0; i < 31; ++i)
        g.options_mask[i] = 1;
    const uint8_t corners[4] = {0, 7, 22, 15};
    const uint8_t order[4] = {22, 0, 15, 7};
    const uint8_t start[4] = {2, 0, 3, 1};
    memcpy(g.options_corners, corners, sizeof(corners));
    memcpy(g.options_corner_order, order, sizeof(order));
    memcpy(g.options_corner_start, start, sizeof(start));
    g.options_edges[0] = 80;
    g.options_edges[1] = -70;
    g.options_edges[2] = -80;
    g.options_edges[3] = 70;
    g.options_wave_height = 10;
    g.options_wave_duration = 16;
    g.memory_scale = 0.7f;
    g.memory_inner_scale = 0.5f;
    g.memory_turns[0] = -8000;
    g.memory_turns[1] = 32768;
    g.memory_turns[2] = 40768;
    g.options_exit_step = g.memory_exit_step = 5;
    g.options_exit_fade = 12;
    g.memory_exit_fade = 13;
    g.gameplay_radius = 75;
    g.gameplay_scale = 0.65f;
    g.gameplay_exit_step = 5;
    g.gameplay_exit_fade = 10;
    g.calendar_scale = 0.7f;
    g.calendar_hand_scale = 0.55f;
    g.calendar_exit_step = 6;
    g.calendar_exit_fade = 14;
    g.card_min_scale = 1.3f;
    g.card_max_scale = 2;
    g.card_hover_offset[0] = 10;
    g.card_hover_offset[1] = 5;
    g.card_particle_radius = 48;
    g.card_particle_start = 0.25f;
    g.card_particle_growth = 0.75f;
    g.card_particle_scale[0] = g.card_particle_scale[1] = g.card_particle_scale[2] =
        0.3f;
    g.card_arrow_ticks = 15;
    g.card_arrow_amplitude = 8;
    g.grid_lighting.reference_distance = 500;
    g.grid_lighting.reference_brightness = 0.375f;
    g.grid_lighting.position[0] = 296;
    g.grid_lighting.position[1] = 224;
    g.grid_lighting.position[2] = 474;
    g.grid_lighting.cutoff_degrees = 36;
    return g;
}

static GcMenuAnimationPose identity_pose(void) {
    GcMenuAnimationPose p = {0};
    p.cube_matrix[0] = p.cube_matrix[5] = p.cube_matrix[10] = 1;
    return p;
}

static void test_memory_arrival_and_turn(void) {
    GcFaceGeometry g = synthetic();
    GcFaceGeometryState state;
    uint8_t delays[36] = {0};
    delays[0] = 31;
    assert(gc_face_geometry_init(&g, delays, &state));
    for (unsigned tick = 0; tick <= 31; ++tick) {
        assert(gc_face_geometry_update(&g, &state, true, GC_FACE_MEMORY_CARD, false));
        assert(state.memory[0].alpha == 0);
    }
    for (unsigned tick = 32; tick <= 51; ++tick)
        assert(gc_face_geometry_update(&g, &state, true, GC_FACE_MEMORY_CARD, false));
    assert(state.memory[0].alpha == 255);
    assert(state.memory[0].position[0] == -50 && state.memory[0].position[1] == 50);
    assert(state.memory[35].position[0] == 50 && state.memory[35].position[1] == -50);
    for (unsigned tick = 52; tick <= 300; ++tick)
        assert(gc_face_geometry_update(&g, &state, true, GC_FACE_MEMORY_CARD, false));
    assert(state.memory_turn == -32768);
    for (unsigned tick = 301; tick <= 490; ++tick)
        assert(gc_face_geometry_update(&g, &state, true, GC_FACE_MEMORY_CARD, false));
    assert(state.memory_tick == 210 && state.memory_turn == 0);
    assert(state.memory[7].alpha == 255 && state.memory[22].alpha == 255);
    GcMenuAnimationPose pose = identity_pose();
    GcFaceGeometryPoint point;
    assert(gc_face_geometry_get(&g, &state, &pose, GC_FACE_MEMORY_CARD, 7, &point));
    assert(point.model == GC_FACE_MODEL_MENU_CUBE && point.register_mask == 1);
    assert(fabsf(point.scale[0] - 0.35f) < 0.0001f);
    assert(!gc_face_geometry_get(&g, &state, &pose, GC_FACE_MEMORY_CARD, 36, &point));
    for (unsigned tick = 0; tick < 26; ++tick)
        assert(gc_face_geometry_update(&g, &state, false, GC_FACE_MEMORY_CARD, false));
    for (unsigned i = 0; i < 36; ++i)
        assert(state.memory[i].alpha == 0);
    delays[5] = 32;
    assert(!gc_face_geometry_init(&g, delays, &state));
}

static void test_options_native_loop(void) {
    GcFaceGeometry g = synthetic();
    GcFaceGeometryState state;
    assert(gc_face_geometry_init(&g, NULL, &state));
    for (unsigned tick = 0; tick < 20; ++tick)
        assert(gc_face_geometry_update(&g, &state, true, GC_FACE_OPTIONS, false));
    for (unsigned i = 0; i < 4; ++i)
        assert(state.options[g.options_corners[i]].alpha == 120);
    for (unsigned tick = 20; tick <= 265; ++tick)
        assert(gc_face_geometry_update(&g, &state, true, GC_FACE_OPTIONS, false));
    for (unsigned i = 30; i < 61; ++i) {
        assert(state.options[i].alpha == 255);
        assert(state.options[i].position[0] == 0);
    }
    for (unsigned tick = 266; tick <= 599; ++tick)
        assert(gc_face_geometry_update(&g, &state, true, GC_FACE_OPTIONS, false));
    assert(state.options_tick == 70);
    for (unsigned i = 30; i < 61; ++i)
        assert(state.options[i].alpha == 0);
    GcMenuAnimationPose pose = identity_pose();
    GcFaceGeometryPoint point;
    assert(gc_face_geometry_get(&g, &state, &pose, GC_FACE_OPTIONS, 1, &point));
    assert(fabsf(point.scale[0] - 0.67f) < 0.0001f);
    assert(fabsf(point.matrix[3]) < 0.0001f && fabsf(point.matrix[11] - 80) < 0.0001f);
    for (unsigned tick = 0; tick < 30; ++tick)
        assert(gc_face_geometry_update(&g, &state, true, GC_FACE_OPTIONS, true));
    for (unsigned i = 0; i < 61; ++i) {
        assert(!state.options[i].alpha);
        assert(state.options[i].position[0] == 0 && state.options[i].position[1] == 0);
    }
}

static void test_options_corner_phase_boundary(const GcFaceGeometry *geometry) {
    GcFaceGeometryState state;
    assert(gc_face_geometry_init(geometry, NULL, &state));
    assert(
        gc_face_geometry_advance(geometry, &state, 70, true, GC_FACE_OPTIONS, false));
    const float expected[4][2] = {
        {-80, 69.6726f}, {79.6396f, 70}, {-79.6396f, -70}, {80, -69.6726f}};
    float terminal[4][3];
    GcFaceGeometryPoint terminal_points[4];
    GcMenuAnimationPose pose = identity_pose();
    for (unsigned corner = 0; corner < 4; ++corner) {
        unsigned index = geometry->options_corner_order[corner];
        const GcFaceGeometryCell *cell = &state.options[index];
        assert(cell->alpha == 255);
        assert(fabsf(cell->position[0] - expected[corner][0]) < 0.001f);
        assert(fabsf(cell->position[1] - expected[corner][1]) < 0.001f);
        memcpy(terminal[corner], cell->position, sizeof(terminal[corner]));
        assert(gc_face_geometry_get(geometry, &state, &pose, GC_FACE_OPTIONS, index,
                                    &terminal_points[corner]));
        for (unsigned previous = 0; previous < corner; ++previous) {
            float distance_squared = 0;
            for (unsigned axis = 0; axis < 3; ++axis) {
                unsigned translation = axis * 4 + 3;
                float difference = terminal_points[corner].matrix[translation] -
                                   terminal_points[previous].matrix[translation];
                distance_squared += difference * difference;
            }
            assert(distance_squared > 10000);
        }
    }
    /* Native 23840 / PAL 24938 copy retained base coordinates throughout
     * the rotation phase. Include its first tick to catch the center snap. */
    for (unsigned tick = 70; tick < 225; ++tick) {
        assert(gc_face_geometry_update(geometry, &state, true, GC_FACE_OPTIONS, false));
        for (unsigned corner = 0; corner < 4; ++corner) {
            const GcFaceGeometryCell *cell =
                &state.options[geometry->options_corner_order[corner]];
            assert(cell->alpha == 255);
            assert(memcmp(terminal[corner], cell->position, sizeof(terminal[corner])) ==
                   0);
            GcFaceGeometryPoint point;
            assert(gc_face_geometry_get(geometry, &state, &pose, GC_FACE_OPTIONS,
                                        geometry->options_corner_order[corner],
                                        &point));
            assert(point.alpha == 255 && point.model == GC_FACE_MODEL_MENU_CUBE);
            assert(point.register_mask == 1);
            assert(!memcmp(point.registers[0], geometry->options_color,
                           sizeof(geometry->options_color)));
            for (unsigned axis = 0; axis < 3; ++axis) {
                unsigned translation = axis * 4 + 3;
                assert(point.matrix[translation] ==
                       terminal_points[corner].matrix[translation]);
            }
        }
    }
    assert(
        gc_face_geometry_advance(geometry, &state, 40, true, GC_FACE_OPTIONS, false));
    for (unsigned corner = 0; corner < 4; ++corner) {
        const GcFaceGeometryCell *cell =
            &state.options[geometry->options_corner_order[corner]];
        assert(cell->alpha == 255);
        assert(cell->position[0] == 0);
        assert(cell->position[1] == terminal[corner][1]);
    }
    for (unsigned cell = 0; cell < 61; ++cell)
        assert(state.options[cell].alpha == (cell < 30 ? 255 : 0));
    /* USA 23b2c / PAL 24c24 invert all 61 alphas at 265, after the
     * outline has contracted. It is not an early corner-only fade. */
    assert(gc_face_geometry_update(geometry, &state, true, GC_FACE_OPTIONS, false));
    assert(state.options_tick == 266);
    for (unsigned cell = 0; cell < 61; ++cell)
        assert(state.options[cell].alpha == (cell < 30 ? 0 : 255));
    assert(
        gc_face_geometry_advance(geometry, &state, 293, true, GC_FACE_OPTIONS, false));
    assert(state.options_tick == 559);
    for (unsigned cell = 0; cell < 61; ++cell)
        assert(state.options[cell].alpha == (cell < 30 ? 0 : 255));
    assert(gc_face_geometry_update(geometry, &state, true, GC_FACE_OPTIONS, false));
    for (unsigned cell = 0; cell < 61; ++cell)
        assert(state.options[cell].alpha == (cell < 30 ? 255 : 0));
    assert(
        gc_face_geometry_advance(geometry, &state, 40, true, GC_FACE_OPTIONS, false));
    assert(state.options_tick == 70);
    assert(gc_face_geometry_update(geometry, &state, true, GC_FACE_OPTIONS, false));
    for (unsigned corner = 0; corner < 4; ++corner) {
        const GcFaceGeometryCell *cell =
            &state.options[geometry->options_corner_order[corner]];
        assert(cell->alpha == 255);
        assert(memcmp(terminal[corner], cell->position, sizeof(terminal[corner])) == 0);
    }
}

static void test_batch_advance(void) {
    GcFaceGeometry g = synthetic();
    for (unsigned selection = 0; selection < 8; ++selection) {
        gc_face face = (gc_face)(selection % 4);
        bool editing = selection >= 4;
        GcFaceGeometryState slow, fast;
        assert(gc_face_geometry_init(&g, NULL, &slow));
        fast = slow;
        for (unsigned i = 0; i < 17777; ++i)
            assert(gc_face_geometry_update(&g, &slow, true, face, editing));
        assert(gc_face_geometry_advance(&g, &fast, 17777, true, face, editing));
        assert(!memcmp(&slow, &fast, sizeof(slow)));
    }
}

static void test_native_clock_pieces(void) {
    GcFaceGeometry g = synthetic();
    GcFaceGeometryState state;
    assert(gc_face_geometry_init(&g, NULL, &state));
    gc_date_time clock = {2001, 1, 1, 0, 0, 0};
    assert(gc_face_geometry_set_clock(&state, &clock));
    assert(gc_face_geometry_advance(&g, &state, 50, true, GC_FACE_CALENDAR, false));
    assert(state.calendar[1].position[0] == 0 && state.calendar[1].position[1] == 100);
    assert(state.calendar[8].position[0] == 0 && state.calendar[8].position[1] == 80);
    assert(state.calendar[11].position[0] == 0 && state.calendar[11].position[1] == 60);
    /* The native -3-unit seconds bias indexes the -16-unit sine bin. */
    assert(state.calendar[12].position[0] < -0.15f &&
           state.calendar[12].position[0] > -0.16f);
    assert(state.calendar_tick == 50 && state.calendar[12].alpha == 255);
    clock.hour = 3;
    clock.minute = 15;
    clock.second = 30;
    assert(gc_face_geometry_set_clock(&state, &clock));
    assert(gc_face_geometry_update(&g, &state, true, GC_FACE_CALENDAR, false));
    assert(state.calendar[8].position[0] < -79 && state.calendar[8].position[1] < 0);
    assert(state.calendar[11].position[0] < -59 && state.calendar[11].position[1] < 0);
    assert(state.calendar[12].position[1] < -99);
    clock.second = 60;
    assert(!gc_face_geometry_set_clock(&state, &clock));
}

static void test_rom(const char *path) {
    GcFaceGeometry g;
    GcFaceGeometryState state;
    assert(gc_face_geometry_load(path, &g));
    assert(g.options_color[0] == -30 && g.options_color[1] == 20 &&
           g.options_color[2] == -50 && g.options_color[3] == 255);
    assert(g.memory_color[0] == -45 && g.memory_color[1] == -40 &&
           g.memory_color[2] == 50 && g.memory_color[3] == 255);
    assert(g.memory_scale == 0.7f && g.memory_inner_scale == 0.5f);
    assert(g.options_wave_height == 10 && g.options_wave_duration == 16);
    assert(g.memory_exit_step == 5 && g.memory_exit_fade == 13);
    assert(g.options_exit_step == 5 && g.options_exit_fade == 12);
    assert(g.gameplay_radius == 75 && g.gameplay_scale == 0.65f);
    assert(g.calendar_scale == 0.7f && g.calendar_hand_scale == 0.55f);
    assert(g.calendar_colors[0][0] == 40 && g.calendar_colors[0][1] == 20 &&
           g.calendar_colors[0][2] == -50 && g.calendar_colors[0][3] == 255);
    assert(g.calendar_colors[1][0] == 80 && g.calendar_colors[1][1] == 0 &&
           g.calendar_colors[1][2] == -60 && g.calendar_colors[1][3] == 255);
    assert(g.gameplay_exit_step == 5 && g.gameplay_exit_fade == 10);
    assert(g.card_min_scale == 1.3f && g.card_max_scale == 2);
    assert(g.card_hover_offset[0] == 10 && g.card_hover_offset[1] == 5);
    assert(g.card_particle_radius == 48 && g.card_particle_start == 0.25f &&
           g.card_particle_growth == 0.75f);
    for (unsigned i = 0; i < 3; ++i)
        assert(g.card_particle_scale[i] == 0.3f);
    assert(g.card_colors[0][0][0] == 100 && g.card_colors[0][0][1] == 150 &&
           g.card_colors[0][0][2] == 255 && g.card_colors[0][0][3] == 255);
    assert(g.grid_lighting.reference_distance == 500 &&
           g.grid_lighting.reference_brightness == 0.375f &&
           g.grid_lighting.position[0] == 296 && g.grid_lighting.position[1] == 224 &&
           g.grid_lighting.position[2] == 474 && g.grid_lighting.cutoff_degrees == 36);
    assert(g.card_arrow_ticks == 15 && g.card_arrow_amplitude == 8);
    test_options_corner_phase_boundary(&g);
    assert(gc_face_geometry_init(&g, NULL, &state));
    GcMenuAnimationPose pose = identity_pose();
    for (unsigned tick = 0; tick < 3000; ++tick) {
        assert(gc_face_geometry_update(&g, &state, true, GC_FACE_OPTIONS, false));
        for (unsigned i = 0; i < 61; ++i) {
            GcFaceGeometryPoint p;
            assert(gc_face_geometry_get(&g, &state, &pose, GC_FACE_OPTIONS, i, &p));
            for (unsigned j = 0; j < 12; ++j)
                assert(isfinite(p.matrix[j]));
        }
    }
    printf("Native face geometry recovered (%u Hz).\n", g.frame_rate);
}

static void test_card_pose_and_particles(void) {
    GcFaceGeometry g = synthetic();
    GcStartup startup = {0};
    startup.menu_profile[3] = 350;
    startup.menu_profile[4] = startup.menu_profile[5] = 1000;
    startup.menu_profile[10] = -15000;
    startup.menu_profile[11] = -16384;
    GcFaceGeometryPoint point;
    assert(gc_card_geometry_sample(&g, &startup, 84, 106, false, false, 8192, 0, 0, 255,
                                   &point));
    assert(point.model == GC_FACE_MODEL_CARD_BASE && point.alpha == 255);
    assert(point.scale[0] == 1.3f && point.matrix[3] == -208 &&
           point.matrix[7] == 118 && point.matrix[11] == 0);
    assert(gc_card_geometry_sample(&g, &startup, 84, 106, true, true, 8192, 6, 0, 255,
                                   &point));
    assert(point.model == GC_FACE_MODEL_CARD_COVER && point.scale[0] == 2);
    assert(point.matrix[3] == -218 && point.matrix[7] == 118 && point.matrix[11] == 2);
    GcFaceGeometryPoint selected = point;
    assert(gc_card_geometry_erase(&g, &startup, 84, 106, 1, 8192, 0, 255, &point));
    assert(!memcmp(&selected, &point, sizeof(point)));
    assert(gc_card_geometry_erase(&g, &startup, 84, 106, 0.5f, 8192, 0, 255, &point));
    assert(point.scale[0] == 1 && point.matrix[3] == -213 && point.matrix[11] == 1);
    assert(gc_card_geometry_erase(&g, &startup, 84, 106, 0, 8192, 0, 255, &point));
    assert(point.scale[0] == 0 && point.matrix[3] == -208 && point.matrix[7] == 118 &&
           point.matrix[11] == 0 && point.matrix[0] == 1 && point.matrix[5] == 1 &&
           point.matrix[10] == 1);
    assert(!gc_card_geometry_erase(&g, &startup, 84, 106, NAN, 8192, 0, 255, &point));
    assert(!gc_card_geometry_erase(&g, &startup, 84, 106, 1.1f, 8192, 0, 255, &point));
    assert(!gc_card_geometry_sample(&g, &startup, NAN, 106, false, false, 8192, 0, 0,
                                    255, &point));
    assert(!gc_card_geometry_sample(&g, &startup, 84, 106, false, false, 1024, 0, 0,
                                    255, &point));
    float parent[12] = {1, 0, 0, -208, 0, 1, 0, 118, 0, 0, 1, 0};
    assert(gc_card_geometry_particle(&g, parent, 8192, 2, 2, 0, 255, &point));
    assert(point.alpha == 0 && point.matrix[7] == 130);
    assert(gc_card_geometry_particle(&g, parent, 8192, 3, 2, 0, 255, &point));
    assert(point.alpha == 229 && fabsf(point.matrix[7] - 133.6f) < 0.0001f);
    assert(point.scale[0] == 0.3f);
    assert(gc_card_geometry_particle(&g, parent, 8192, 12, 2, 16384, 255, &point));
    assert(point.alpha == 0 && point.matrix[3] == -160);
    assert(!gc_card_geometry_particle(&g, parent, 8192, 3, 4, 0, 255, &point));
}

static void test_native_random(void) {
    GcFaceRandom random;
    const uint32_t samples[5] = {1, 2, 3, 4, 256};
    assert(gc_face_random_init(samples, &random));
    assert(random.values[0] == UINT32_C(0x41c67ea6));
    assert(random.values[1] == 1 && random.values[2] == 3 && random.values[3] == 4);
    uint32_t value;
    assert(gc_face_random_next(&random, 0, &value));
    assert(value == UINT32_C(0xc553f47b));
    random.values[1] = 1;
    assert(gc_face_random_next(&random, 256, &value));
    assert(random.values[1] == UINT32_C(0x8810));
    assert(!gc_face_random_init(NULL, &random));
    assert(!gc_face_random_next(&random, 0, NULL));
}

static void test_native_grid_light(void) {
    GcFaceGeometry g = synthetic();
    float center[4], left[4], right[4], corner[4], faded[4];
    assert(gc_menu_grid_color(&g, 296, 224, 255, center));
    assert(center[0] == 1 && center[1] == 1 && center[2] == 1);
    assert(fabsf(center[3] - 0.3875969f) < 0.00001f);
    assert(gc_menu_grid_color(&g, 196, 224, 255, left));
    assert(gc_menu_grid_color(&g, 396, 224, 255, right));
    assert(left[3] == right[3] && left[3] > 0 && left[3] < center[3]);
    assert(gc_menu_grid_color(&g, 0, 0, 255, corner));
    assert(corner[3] == 0);
    /* Native geometry emits the complete rectangle, while the spotlight
     * suppresses its corners. Its 36 degree cone reaches about344 pixels
     * from the light center at the grid's z0 plane. */
    const float corners[4][2] = {{0, 0}, {608, 0}, {0, 448}, {608, 448}};
    for (unsigned index = 0; index < 4; ++index) {
        assert(
            gc_menu_grid_color(&g, corners[index][0], corners[index][1], 255, corner));
        assert(corner[3] == 0);
    }
    assert(gc_menu_grid_color(&g, 296 + 340, 224, 255, corner));
    assert(corner[3] > 0 && corner[3] < 0.005f);
    assert(gc_menu_grid_color(&g, 296 + 350, 224, 255, corner));
    assert(corner[3] == 0);
    assert(gc_menu_grid_color(&g, 296, 224, 0, corner));
    assert(corner[3] == 0);
    assert(gc_menu_grid_color(&g, 296, 224, 128, faded));
    assert(fabsf(faded[3] - center[3] * 128 / 255) < 0.00001f);
    assert(!gc_menu_grid_color(&g, NAN, 224, 255, faded));
    g.grid_lighting.reference_brightness = 0;
    assert(!gc_menu_grid_color(&g, 296, 224, 255, faded));
}

static void test_editor_gate(void) {
    GcFaceGeometry g = synthetic();
    const gc_face faces[4] = {GC_FACE_GAME_PLAY, GC_FACE_CALENDAR, GC_FACE_OPTIONS,
                              GC_FACE_MEMORY_CARD};
    for (unsigned i = 0; i < 4; ++i) {
        GcFaceGeometryState state;
        assert(gc_face_geometry_init(&g, NULL, &state));
        assert(gc_face_geometry_advance(&g, &state, 300, true, faces[i], false));
        assert(!gc_face_geometry_editor_ready(&state, faces[i]));
        assert(gc_face_geometry_update(&g, &state, true, faces[i], true));
        assert(!gc_face_geometry_editor_ready(&state, faces[i]));
        assert(gc_face_geometry_advance(&g, &state, 30, true, faces[i], true));
        assert(gc_face_geometry_editor_ready(&state, faces[i]));
    }
    GcFaceGeometryState memory = {0};
    assert(gc_face_geometry_editor_ready(&memory, GC_FACE_MEMORY_CARD));
    memory.memory[35].position[2] = 1;
    assert(!gc_face_geometry_editor_ready(&memory, GC_FACE_MEMORY_CARD));
    memory.memory[35].position[2] = 0;
    memory.memory[35].alpha = 1;
    assert(!gc_face_geometry_editor_ready(&memory, GC_FACE_MEMORY_CARD));
    assert(!gc_face_geometry_editor_ready(NULL, GC_FACE_OPTIONS));
}

static void test_card_arrows(void) {
    GcFaceGeometry g = synthetic();
    float offset;
    assert(gc_card_arrow_offset(&g, 0, &offset) && offset == 0);
    assert(gc_card_arrow_offset(&g, 7, &offset) && offset == 3.5625f);
    assert(gc_card_arrow_offset(&g, 15, &offset) && offset == 8);
    assert(gc_card_arrow_offset(&g, 16, &offset) && offset == 8);
    assert(gc_card_arrow_offset(&g, 31, &offset) && offset == 0);
    assert(gc_card_arrow_offset(&g, 32, &offset) && offset == 0);
    assert(gc_card_arrow_offset(&g, UINT64_MAX, &offset) && offset == 0);
    g.card_arrow_ticks = 0;
    assert(!gc_card_arrow_offset(&g, 0, &offset));
}

static void test_settled_periods(void) {
    GcFaceGeometry g = synthetic();
    const uint64_t periods[4] = {6549504, 1, 281, 530};
    for (unsigned i = 0; i < 4; ++i) {
        gc_face face = (gc_face)i;
        GcFaceGeometryState state, previous;
        assert(gc_face_geometry_period(face) == periods[i]);
        assert(gc_face_geometry_init(&g, NULL, &state));
        assert(gc_face_geometry_advance(&g, &state, 1200, true, face, false));
        previous = state;
        assert(gc_face_geometry_advance(&g, &state, periods[i], true, face, false));
        assert(memcmp(&previous, &state, sizeof(state)) == 0);
    }
    assert(gc_face_geometry_period((gc_face)100) == 0);
    assert(GC_FACE_GAMEPLAY_PERIOD % 624 == 0 && GC_FACE_GAMEPLAY_PERIOD % 656 == 0 &&
           GC_FACE_GAMEPLAY_PERIOD % 4096 == 0);
}

int main(int argc, char **argv) {
    test_memory_arrival_and_turn();
    test_options_native_loop();
    GcFaceGeometry geometry = synthetic();
    test_options_corner_phase_boundary(&geometry);
    test_batch_advance();
    test_native_clock_pieces();
    test_card_pose_and_particles();
    test_native_random();
    test_native_grid_light();
    test_editor_gate();
    test_card_arrows();
    test_settled_periods();
    for (int i = 1; i < argc; ++i)
        test_rom(argv[i]);
    puts("Face geometry tests passed.");
    return 0;
}
