#include "gamecube/startup.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static void test_affine_transform(void) {
    const float matrix[12] = {0, -1, 0, 3, 1, 0, 0, 4, 0, 0, 1, 5};
    float point[3] = {2, 1, 6};
    gc_startup_transform(matrix, point, point);
    assert(point[0] == 2 && point[1] == 6 && point[2] == 11);
}

static void test_synthetic_sequence(void) {
    GcStartup startup = {0};
    startup.frame_rate = 60;
    startup.roll_ticks = startup.corner_ticks = 2;
    startup.drop_ticks = startup.bounce_ticks = startup.rise_ticks = 2;
    startup.reveal_ticks = startup.logotype_ticks = 2;
    startup.drop_height = 10;
    startup.bounce_height = 20;
    startup.rise_velocity = 10;
    startup.cube_edge = 54;
    startup.scene_base_angles[0] = 8192;
    startup.scene_base_angles[1] = -6420;
    startup.scene_base_angles[2] = -5461;
    startup.scene_origin_offset = 15;
    startup.bounce_quarter_turns = 2;
    startup.wave_decay[0] = startup.wave_decay[1] = 0.5f;
    startup.wave_translation[0] = startup.wave_translation[1] = 0.01f;
    startup.step_count = 3;
    startup.steps[0] = (GcStartupStep){0, 1, 255};
    startup.steps[1] = (GcStartupStep){3, 1, 255};
    startup.steps[2] = (GcStartupStep){7, 1, 255};
    GcStartupPose pose;
    assert(gc_startup_sample(&startup, 0, &pose));
    assert(pose.phase == GC_STARTUP_DROP && !pose.complete);
    assert(pose.cover_alpha == 0 && pose.moving_cube_alpha == 255);
    /* The cube's native pivot rests half a cell above the first trail cell.
     * Drop height is a later world-space translation, not a rotated pivot.
     */
    assert(fabsf(pose.cube_matrix[3] - 54) < 0.001f);
    assert(fabsf(pose.cube_matrix[7] - 108) < 0.001f);
    assert(fabsf(pose.cube_matrix[11] + 54) < 0.001f);
    assert(pose.moving_cube_world_y == 10);
    bool saw_trail = false;
    bool saw_animation = false;
    unsigned previous = 0;
    for (unsigned tick = 0; tick < 100; ++tick) {
        assert(gc_startup_sample(&startup, tick, &pose));
        assert((unsigned)pose.phase >= previous);
        previous = (unsigned)pose.phase;
        for (unsigned element = 0; element < 12; ++element) {
            assert(isfinite(pose.scene_matrix[element]));
            assert(isfinite(pose.cube_matrix[element]));
        }
        for (size_t trail = 0; trail < pose.trail_count; ++trail) {
            assert(pose.trails[trail].face < 3);
            assert(pose.trails[trail].positions[0][1] == 81);
        }
        saw_trail |= pose.trail_count > 0;
        saw_animation |= pose.logotype_frame > 0;
    }
    assert(saw_trail && saw_animation && pose.complete);
    assert(pose.cover_alpha == 255 && pose.moving_cube_alpha == 0);
    assert(fabsf(pose.cube_matrix[3] - 40.5f) < 0.001f);
    assert(fabsf(pose.cube_matrix[7] - 40.5f) < 0.001f);
    assert(fabsf(pose.cube_matrix[11] - 40.5f) < 0.001f);
    assert(pose.moving_cube_world_y == 0);
    startup.roll_ticks = 0;
    assert(!gc_startup_sample(&startup, 0, &pose));
}

static void test_supplied_rom(const char *path) {
    GcStartup startup = {0};
    assert(gc_startup_load(path, &startup));
    assert(startup.step_count == 33 && startup.cube_edge == 54);
    assert(startup.trail_texture.width == 64 && startup.trail_texture.height == 64);
    assert(startup.trail_color[0] == 100 && startup.trail_color[1] == 80 &&
           startup.trail_color[2] == 190);
    bool pal = startup.frame_rate == 50;
    const unsigned expected_ntsc[GC_STARTUP_PHASE_COUNT] = {0, 23, 212, 238, 270, 311};
    const unsigned expected_pal[GC_STARTUP_PHASE_COUNT] = {0, 19, 178, 200, 227, 269};
    assert(!memcmp(startup.phase_start_ticks, pal ? expected_pal : expected_ntsc,
                   sizeof(expected_ntsc)));
    GcStartupPose pose;
    assert(gc_startup_sample(&startup, startup.sequence_ticks, &pose));
    assert(pose.complete && pose.trail_count == 16);
    assert(pose.logotype_frame == 60);
    assert(startup.spin_ticks == 82);
    assert(startup.menu_ticks == (pal ? 495 : 537));
    assert(startup.kinetic_increment == (pal ? 36 : 30));
    assert(startup.scene_origin_offset == 15);
    assert(startup.scene_base_angles[0] == 8192 &&
           startup.scene_base_angles[1] == -6420 &&
           startup.scene_base_angles[2] == -5461);
    assert(startup.transition_base_angles[0] == 8191 &&
           startup.transition_base_angles[1] == -6371 &&
           startup.transition_base_angles[2] == -5461);
    assert(startup.kinetic_phase_rates[0] == 90 &&
           startup.kinetic_phase_rates[1] == 60 &&
           startup.kinetic_phase_rates[2] == 70);
    assert(gc_startup_sample(&startup, 0, &pose));
    /* The native baseline Euler transforms the (15,15,15) origin into
     * approximately (0,0,26); this is not a world-axis translation.
     */
    assert(fabsf(pose.scene_matrix[3] + 0.026541679f) < 0.0001f);
    assert(fabsf(pose.scene_matrix[7] - 0.015360053f) < 0.0001f);
    assert(fabsf(pose.scene_matrix[11] - 25.980740232f) < 0.0001f);
    assert(gc_startup_sample(&startup, startup.sequence_ticks - 2, &pose));
    assert(!pose.drawing_ready);
    assert(gc_startup_sample(&startup, startup.sequence_ticks - 1, &pose));
    assert(pose.drawing_ready && !pose.complete);
    assert(gc_startup_sample_menu(&startup, startup.sequence_ticks - 1, &pose));
    assert(pose.scene_phase == GC_STARTUP_SCENE_SPINNING && !pose.complete);
    assert(!pose.perspective && pose.boot_mark_alpha == 255);
    unsigned first_kinetic = pal ? 36 : 30;
    assert(pose.cover_cube_alpha == first_kinetic * 255 / 3000);
    assert(gc_startup_sample_menu(&startup, startup.sequence_ticks, &pose));
    assert(pose.cover_cube_alpha == first_kinetic * 3 * 255 / 3000);
    assert(gc_startup_sample_menu(
        &startup, startup.sequence_ticks + startup.spin_ticks - 2, &pose));
    assert(pose.cover_cube_alpha == 255);
    assert(gc_startup_sample_menu(
        &startup, startup.sequence_ticks + startup.spin_ticks - 1, &pose));
    assert(pose.scene_phase == GC_STARTUP_SCENE_TRANSITION && pose.perspective);
    assert(pose.glass_cube_alpha == 17 && !pose.boot_mark_alpha);
    for (unsigned tick = 0; tick <= startup.menu_ticks + 100; ++tick) {
        assert(gc_startup_sample_menu(&startup, tick, &pose));
        for (unsigned element = 0; element < 12; ++element) {
            assert(isfinite(pose.scene_matrix[element]));
            assert(isfinite(pose.glass_matrix[element]));
        }
        for (unsigned axis = 0; axis < 3; ++axis) {
            assert(isfinite(pose.model_scale[axis]));
            assert(isfinite(pose.glass_scale[axis]));
        }
    }
    assert(pose.scene_phase == GC_STARTUP_SCENE_MENU && pose.complete);
    assert(pose.glass_cube_alpha == 255 && pose.menu_labels_alpha == 255);
    for (unsigned axis = 0; axis < 3; ++axis)
        assert(fabsf(pose.glass_scale[axis] - 1.1f) < 0.001f);
    GcStartupPose repeated;
    unsigned settled = startup.menu_ticks + 300;
    assert(gc_startup_sample_menu(&startup, settled, &pose));
    assert(gc_startup_sample_menu(&startup, settled + (unsigned)GC_STARTUP_MENU_PERIOD,
                                  &repeated));
    assert(
        !memcmp(pose.glass_matrix, repeated.glass_matrix, sizeof(pose.glass_matrix)));
    unsigned transition_start = startup.menu_ticks - 145;
    assert(gc_startup_sample_menu(&startup, transition_start + 44, &pose));
    assert(pose.menu_labels_alpha == 0);
    assert(gc_startup_sample_menu(&startup, transition_start + 94, &pose));
    assert(pose.menu_labels_alpha > 60 && pose.menu_labels_alpha < 70);
    gc_startup_destroy(&startup);
    assert(!startup.trail_texture.rgba);
}

int main(int argc, char **argv) {
    test_affine_transform();
    test_synthetic_sequence();
    if (argc == 2)
        test_supplied_rom(argv[1]);
    puts("Startup tests passed");
    return 0;
}
