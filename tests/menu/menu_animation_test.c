#include "gamecube/menu_animation.h"
#include "gamecube/angle.h"
#include "gamecube/ipl_model.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static GcStartup synthetic_config(void) {
    GcStartup startup = {0};
    startup.menu_rotation_step = 750;
    startup.menu_focus_enter_step = 1840;
    startup.menu_focus_exit_step = 1400;
    startup.menu_focus_distance = -70;
    startup.transition_ticks[0] = 15;
    startup.transition_ticks[1] = 80;
    startup.transition_ticks[2] = 50;
    return startup;
}

static void test_native_plane_alignment(void) {
    GcStartup startup = synthetic_config();
    for (unsigned face = 0; face < 4; ++face) {
        GcMenuAnimation animation;
        GcMenuAnimationPose pose;
        assert(gc_menu_animation_init(&startup, &animation));
        for (unsigned tick = 0; tick < 22; ++tick)
            assert(gc_menu_animation_update(&startup, &animation, true, (gc_face)face,
                                            false));
        assert(gc_menu_animation_sample(&startup, &animation, true, (gc_face)face,
                                        false, &pose));
        assert(pose.rotation_complete && pose.focus_complete);
        assert(pose.pane_alpha[face] == 255 && pose.frame_alpha[face] == 255);
        assert(!pose.pane_alpha[GC_MENU_ANIMATION_HOME_PANE]);
        const float local[3] = {144, 144, 0};
        float point[3];
        gc_startup_transform(pose.pane_matrices[face], local, point);
        assert(fabsf(point[0]) < 0.001f && fabsf(point[1]) < 0.001f);
        assert(fabsf(point[2] - 144) < 0.001f);
        for (unsigned tick = 0; tick < 30; ++tick)
            assert(gc_menu_animation_update(&startup, &animation, false, (gc_face)face,
                                            false));
        assert(gc_menu_animation_sample(&startup, &animation, false, (gc_face)face,
                                        false, &pose));
        assert(pose.rotation_complete && pose.pane_alpha[4] == 255);
    }
}

static void test_focus_and_frame_alpha(void) {
    GcStartup startup = synthetic_config();
    GcMenuAnimation animation;
    GcMenuAnimationPose pose;
    assert(gc_menu_animation_init(&startup, &animation));
    float previous_depth = 0;
    for (unsigned tick = 0; tick < 17; ++tick) {
        assert(gc_menu_animation_update(&startup, &animation, true, GC_FACE_MEMORY_CARD,
                                        true));
        assert(gc_menu_animation_sample(&startup, &animation, true, GC_FACE_MEMORY_CARD,
                                        true, &pose));
        assert(pose.cube_matrix[11] <= previous_depth);
        assert(previous_depth - pose.cube_matrix[11] < 15);
        previous_depth = pose.cube_matrix[11];
    }
    assert(animation.focus_angle == 32767);
    assert(gc_menu_animation_sample(&startup, &animation, true, GC_FACE_MEMORY_CARD,
                                    true, &pose));
    assert(pose.focus_complete);
    assert(fabsf(pose.cube_matrix[11] + 140) < 0.001f);
    assert(pose.glass_alpha == 0 && pose.pane_alpha[GC_FACE_MEMORY_CARD] == 0);
    unsigned alpha = pose.pane_alpha[GC_FACE_MEMORY_CARD];
    assert(pose.frame_alpha[GC_FACE_MEMORY_CARD] == alpha * alpha / 255);
    for (unsigned tick = 0; tick < 23; ++tick) {
        assert(gc_menu_animation_update(&startup, &animation, true, GC_FACE_MEMORY_CARD,
                                        false));
        assert(gc_menu_animation_sample(&startup, &animation, true, GC_FACE_MEMORY_CARD,
                                        false, &pose));
        assert(pose.cube_matrix[11] >= previous_depth);
        assert(pose.cube_matrix[11] - previous_depth < 11);
        previous_depth = pose.cube_matrix[11];
    }
    assert(animation.focus_angle == 0);
    assert(pose.cube_matrix[11] == 0);
    assert(gc_menu_animation_sample(&startup, &animation, true, GC_FACE_MEMORY_CARD,
                                    false, &pose));
    assert(pose.glass_alpha == 255 && pose.pane_alpha[GC_FACE_MEMORY_CARD] == 255);
    startup.menu_glass_min_alpha = 64;
    for (unsigned tick = 0; tick < 17; ++tick)
        assert(gc_menu_animation_update(&startup, &animation, true, GC_FACE_MEMORY_CARD,
                                        true));
    assert(gc_menu_animation_sample(&startup, &animation, true, GC_FACE_MEMORY_CARD,
                                    true, &pose));
    assert(pose.glass_alpha == 64 && pose.pane_alpha[GC_FACE_MEMORY_CARD] == 64);
    animation.oscillator_phase = 65534;
    assert(gc_menu_animation_update(&startup, &animation, false, GC_FACE_MEMORY_CARD,
                                    false));
    assert(animation.oscillator_phase == 5);
    assert(!gc_menu_animation_update(&startup, &animation, true, (gc_face)100, false));
    startup.menu_rotation_step = 0;
    assert(!gc_menu_animation_init(&startup, &animation));
}

static void test_supplied_rom(const char *path) {
    GcStartup startup = {0};
    assert(gc_startup_load(path, &startup));
    assert(startup.menu_rotation_step == 750);
    assert(startup.menu_focus_enter_step == 1840);
    assert(startup.menu_focus_exit_step == 1400);
    assert(startup.menu_focus_distance == -70 && startup.menu_focus_twist == 0);
    GcMenuAnimation animation;
    GcMenuAnimationPose pose;
    assert(gc_menu_animation_init(&startup, &animation));
    assert(gc_menu_animation_update(&startup, &animation, false, GC_FACE_GAME_PLAY,
                                    false));
    assert(gc_menu_animation_sample(&startup, &animation, false, GC_FACE_GAME_PLAY,
                                    false, &pose));
    GcStartupPose handoff;
    assert(gc_startup_sample_menu(&startup, startup.menu_ticks, &handoff));
    for (unsigned element = 0; element < 12; ++element)
        assert(fabsf(pose.cube_matrix[element] - handoff.glass_matrix[element]) <
               0.0001f);
    for (unsigned tick = 0; tick < 10000; ++tick) {
        gc_face face = (gc_face)((tick / 200) % 4);
        bool selected = tick % 200 >= 50;
        bool editing = tick % 200 >= 100;
        assert(gc_menu_animation_update(&startup, &animation, selected, face, editing));
        assert(gc_menu_animation_sample(&startup, &animation, selected, face, editing,
                                        &pose));
        for (unsigned element = 0; element < 12; ++element)
            assert(isfinite(pose.cube_matrix[element]));
    }
    GcIplModel glyph = {0};
    assert(gc_ipl_model_load(path, "cube1", &glyph));
    assert(glyph.joint_count == 1 && glyph.triangle_count > 0);
    gc_ipl_model_destroy(&glyph);
    gc_startup_destroy(&startup);
}

int main(int argc, char **argv) {
    assert(gc_angle_sine(0) == gc_angle_sine(15));
    assert(gc_angle_cosine(65536) == gc_angle_cosine(0));
    assert(gc_angle_sine(-1) == gc_angle_sine(65535));
    assert(gc_angle_sine(16384) == 1 && gc_angle_cosine(0) == 1);
    test_native_plane_alignment();
    test_focus_and_frame_alpha();
    if (argc == 2)
        test_supplied_rom(argv[1]);
    puts("menu animation tests passed");
    return 0;
}
