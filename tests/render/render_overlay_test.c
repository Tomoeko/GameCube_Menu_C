#include "gamecube/audio.h"
#include "gamecube/frame_history.h"
#include "gamecube/render.h"
#include "render/software/software.h"
#include "render/render_state.h"

#include <assert.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static unsigned pixel(CcPlatform *platform, unsigned x, unsigned y) {
    uint8_t rgba[4];
    assert(gc_software_read_pixel(platform, x, y, rgba));
    assert(rgba[0] == rgba[1] && rgba[1] == rgba[2] && rgba[3] == 255);
    return rgba[0];
}

static void test_bar_pixels(GcScene *scene) {
    gc_scene_draw_wait(scene);
    uint64_t black = gc_software_frame_hash(scene->platform);
    gc_scene_volume_indicator(scene, 100, 0);
    gc_scene_draw_wait(scene);
    assert(gc_software_frame_hash(scene->platform) == black);

    gc_scene_volume_indicator(scene, 100, 1);
    gc_scene_draw_wait(scene);
    assert(pixel(scene->platform, 488, 21) == 255);
    unsigned track = pixel(scene->platform, 552, 21);
    assert(track >= 55 && track <= 57);
    assert(pixel(scene->platform, 494, 21) == 255);
    assert(pixel(scene->platform, 497, 21) == track);
    assert(pixel(scene->platform, 480, 20) == 0);
    assert(pixel(scene->platform, 482, 20) == 255);
    assert(pixel(scene->platform, 607, 20) == 0);
    assert(pixel(scene->platform, 488, 19) == 0);
    assert(pixel(scene->platform, 488, 24) == 0);
    assert(pixel(scene->platform, 608, 22) == 0);

    gc_scene_volume_indicator(scene, 0, 1);
    gc_scene_draw_wait(scene);
    assert(pixel(scene->platform, 488, 21) == track);
    gc_scene_volume_indicator(scene, 10, 1);
    gc_scene_draw_wait(scene);
    assert(pixel(scene->platform, 480, 21) == 255);
    assert(pixel(scene->platform, 482, 21) == track);

    gc_scene_volume_indicator(scene, UINT_MAX, 1);
    assert(scene->volume_indicator_percent == GC_AUDIO_MENU_VOLUME_MAX);
    gc_scene_draw_wait(scene);
    assert(pixel(scene->platform, 552, 21) == 255);
    assert(pixel(scene->platform, 606, 21) == 255);
    uint64_t filled = gc_software_frame_hash(scene->platform);
    gc_scene_draw_wait(scene);
    assert(gc_software_frame_hash(scene->platform) == filled);

    gc_scene_volume_indicator(scene, GC_AUDIO_MENU_VOLUME_MAX, 0.5f);
    gc_scene_draw_wait(scene);
    unsigned faded = pixel(scene->platform, 552, 21);
    assert(faded > track && faded < 255);
    gc_scene_volume_indicator(scene, 100, NAN);
    gc_scene_draw_wait(scene);
    assert(gc_software_frame_hash(scene->platform) == black);
    gc_scene_volume_indicator(scene, 100, -1);
    assert(scene->volume_indicator_alpha == 0);
    gc_scene_volume_indicator(scene, 100, 2);
    assert(scene->volume_indicator_alpha == 1);
}

static void test_redraw_preserves_clock(GcScene *scene) {
    gc_menu menu;
    gc_menu_init(&menu, GC_REGION_USA);
    menu.page = GC_PAGE_GAME_STARTED;
    menu.page_elapsed = 999;
    /* Game-started draws a black scene without decoded local resources. */
    scene->font_texture = 1;
    scene->animation_started = true;
    scene->animation_page = GC_PAGE_CUBE;
    scene->animation_elapsed = 3;
    scene->animation_fraction = 0.75;
    scene->ui_ticks = 123;
    scene->card_ticks = 456;
    scene->menu_animation.oscillator_phase = 789;
    gc_scene_volume_indicator(scene, 100, 1);
    gc_scene_redraw(scene, &menu);
    assert(scene->animation_page == GC_PAGE_CUBE);
    assert(scene->animation_elapsed == 3 && scene->animation_fraction == 0.75);
    assert(scene->ui_ticks == 123 && scene->card_ticks == 456);
    assert(scene->menu_animation.oscillator_phase == 789);
    assert(pixel(scene->platform, 488, 21) == 255);

    scene->inspection_fade_alpha = 255;
    gc_scene_draw_wait(scene);
    assert(pixel(scene->platform, 488, 21) == 255);
    assert(scene->ui_ticks == 123 && scene->card_ticks == 456);
}

static void test_redraw_after_rewind(GcScene *scene) {
    GcFrameHistory *history = gc_frame_history_create(3);
    gc_menu menu;
    GcBootControl boot = {0};
    assert(history);
    gc_menu_init(&menu, GC_REGION_USA);
    menu.page = GC_PAGE_GAME_STARTED;
    scene->inspection_fade_alpha = 0;
    scene->animation_page = GC_PAGE_CUBE;
    for (unsigned frame = 0; frame < 3; ++frame) {
        menu.page_elapsed = (double)frame / 60;
        scene->animation_elapsed = (double)frame / 60;
        scene->animation_fraction = 0.25;
        scene->ui_ticks = 10 + frame;
        scene->card_ticks = 20 + frame;
        scene->menu_animation.oscillator_phase = (uint16_t)(30 + frame);
        scene->edit_state.entrance_counter = (uint16_t)(40 + frame);
        scene->card_lighting.focus[0] = (uint16_t)(50 + frame);
        assert(gc_frame_history_save(history, frame, &menu, &boot, scene, NULL));
    }
    gc_scene_volume_indicator(scene, 200, 0.5f);
    uint64_t counter = 2;
    assert(gc_frame_history_seek(history, -2, &counter, &menu, &boot, scene, NULL));
    assert(counter == 0 && menu.page_elapsed == 0);
    assert(scene->volume_indicator_percent == 200);
    assert(scene->volume_indicator_alpha == 0.5f);
    gc_scene_redraw(scene, &menu);
    uint64_t frozen = gc_software_frame_hash(scene->platform);
    for (unsigned repeat = 0; repeat < 3; ++repeat) {
        gc_scene_redraw(scene, &menu);
        assert(gc_software_frame_hash(scene->platform) == frozen);
        assert(scene->animation_page == GC_PAGE_CUBE);
        assert(scene->animation_elapsed == 0 && scene->animation_fraction == 0.25);
        assert(scene->ui_ticks == 10 && scene->card_ticks == 20);
        assert(scene->menu_animation.oscillator_phase == 30);
        assert(scene->edit_state.entrance_counter == 40);
        assert(scene->card_lighting.focus[0] == 50);
        assert(scene->perspective);
    }
    assert(pixel(scene->platform, 488, 21) > pixel(scene->platform, 560, 21));
    gc_frame_history_destroy(history);
}

static void assert_native_state(const GcScene *before, const GcScene *scene) {
#define CHECK(type, field)                                                             \
    assert(memcmp(&before->field, &scene->field, sizeof(type)) == 0);
    GC_SCENE_VISUAL_FIELDS(CHECK)
#undef CHECK
#define CHECK_ARRAY(field)                                                             \
    assert(memcmp(before->field, scene->field, sizeof(before->field)) == 0);
    GC_SCENE_ARRAY_FIELDS(CHECK_ARRAY)
#undef CHECK_ARRAY
}

static void test_frozen_pose(GcScene *scene, gc_menu *menu, GcScene *before) {
    gc_scene_volume_indicator(scene, 100, 0);
    gc_scene_draw(scene, menu);
    *before = *scene;
    uint64_t frozen = gc_software_frame_hash(scene->platform);
    for (unsigned repeat = 0; repeat < 2; ++repeat) {
        gc_scene_redraw(scene, menu);
        assert_native_state(before, scene);
        assert(gc_software_frame_hash(scene->platform) == frozen);
    }
    gc_scene_volume_indicator(scene, 100, 1);
    gc_scene_redraw(scene, menu);
    assert_native_state(before, scene);
    assert(pixel(scene->platform, 488, 21) == 255);
    gc_scene_volume_indicator(scene, 100, 0);
}

static void test_resource_redraw(CcPlatform *platform, const char *resource_path,
                                 bool japanese) {
    GcScene *scene = calloc(1, sizeof(*scene));
    GcScene *before = malloc(sizeof(*before));
    gc_menu *menu = malloc(sizeof(*menu));
    GcFrameHistory *history = gc_frame_history_create(2);
    GcBootControl boot = {0};
    assert(scene && before && menu && history);
    assert(gc_scene_init(scene, platform, resource_path));
    gc_region region = scene->startup.frame_rate == 50 ? GC_REGION_EUROPE
                       : japanese                      ? GC_REGION_JAPAN
                                                       : GC_REGION_USA;
    gc_menu_init(menu, region);
    menu->startup_elapsed = 3;
    test_frozen_pose(scene, menu, before);

    gc_menu_skip_startup(menu);
    menu->page_elapsed = 0.5;
    test_frozen_pose(scene, menu, before);
    /* Restore a real cube pose, including model projection, before a HUD refresh. */
    gc_scene_redraw(scene, menu);
    uint64_t frozen = gc_software_frame_hash(platform);
    *before = *scene;
    assert(gc_frame_history_save(history, 0, menu, &boot, scene, NULL));
    menu->page_elapsed = 0.75;
    gc_scene_draw(scene, menu);
    assert(gc_frame_history_save(history, 1, menu, &boot, scene, NULL));
    uint64_t counter = 1;
    assert(gc_frame_history_seek(history, -1, &counter, menu, &boot, scene, NULL));
    gc_scene_redraw(scene, menu);
    assert_native_state(before, scene);
    assert(gc_software_frame_hash(platform) == frozen);

    menu->page = GC_PAGE_FACE;
    menu->face = GC_FACE_OPTIONS;
    menu->page_elapsed = 2;
    test_frozen_pose(scene, menu, before);
    gc_menu_press(menu, GC_BUTTON_CONFIRM);
    assert(menu->page == GC_PAGE_OPTIONS);
    menu->page_elapsed = 2;
    test_frozen_pose(scene, menu, before);
    gc_frame_history_destroy(history);
    gc_scene_destroy(scene);
    free(menu);
    free(before);
    free(scene);
}

int main(int argc, char **argv) {
    CcPlatform *platform = cc_platform_create("Volume overlay", 640, 480);
    GcScene *scene = calloc(1, sizeof(*scene));
    assert(platform && scene);
    scene->platform = platform;
    gc_scene_volume_indicator(NULL, 100, 1);
    test_bar_pixels(scene);
    test_redraw_preserves_clock(scene);
    test_redraw_after_rewind(scene);
    if (argc > 1)
        test_resource_redraw(platform, argv[1],
                             argc > 2 && strcmp(argv[2], "JAP") == 0);
    cc_platform_destroy(platform);
    free(scene);
    return 0;
}
