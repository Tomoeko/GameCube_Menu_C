#include "gamecube/render.h"
#include "gamecube/frame_history.h"
#include "render/software/software.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void draw_at(GcScene *scene, gc_menu *menu, double seconds) {
    menu->page_elapsed = seconds;
    gc_scene_draw(scene, menu);
}

static void startup_projection(GcScene *scene, gc_menu *menu) {
    const unsigned samples[] = {1, 2, 15, 30, 31, 45};
    GcBootConfig config;
    GcBootControl control;
    GcBootEvents events;
    GcBootInput input = {.drive_state = GC_BOOT_DRIVE_ABSENT};
    assert(gc_boot_config_init(&config, &scene->startup));
    assert(gc_boot_control_init(&config, &control, GC_BOOT_NORMAL));
    gc_menu_init(menu,
                 scene->startup.frame_rate == 50 ? GC_REGION_EUROPE : GC_REGION_USA);
    while (control.phase != GC_BOOT_TRANSITION) {
        assert(control.video_tick < scene->startup.menu_ticks);
        assert(gc_boot_control_step(&config, &control, &input, &events));
    }
    for (unsigned index = 0; index < sizeof(samples) / sizeof(samples[0]); index++) {
        unsigned sample = samples[index];
        while (control.transition_tick < sample)
            assert(gc_boot_control_step(&config, &control, &input, &events));
        /* Stay within the sampled video tick when converting to seconds. */
        menu->startup_elapsed =
            (control.video_tick + 0.125) / scene->startup.frame_rate;
        gc_scene_set_boot(scene, &config, &control);
        gc_scene_draw(scene, menu);
        float expected = sample == 1 ? -55 : -55 + 45 * fminf(1, (float)sample / 30);
        assert(scene->perspective && fabsf(scene->camera_y - expected) < 0.0001f);
        uint64_t live_pixels = gc_software_frame_hash(scene->platform);
        gc_scene_draw(scene, menu);
        assert(live_pixels == gc_software_frame_hash(scene->platform));
        gc_scene_set_boot(scene, NULL, NULL);
        gc_scene_draw(scene, menu);
        assert(scene->perspective && fabsf(scene->camera_y - expected) < 0.0001f);
        assert(live_pixels == gc_software_frame_hash(scene->platform));
    }
}

typedef struct {
    unsigned calls;
    GcFaceRandom *random;
} LaunchRandom;

static bool launch_random(void *context, uint32_t *value) {
    LaunchRandom *fixture = context;
    ++fixture->calls;
    return gc_face_random_next(fixture->random, 1000, value);
}

static void disc_launch(GcScene *scene, gc_menu *menu) {
    double rate = scene->startup.frame_rate;
    gc_menu_init(menu, rate == 50 ? GC_REGION_EUROPE : GC_REGION_USA);
    gc_menu_skip_startup(menu);
    menu->face = GC_FACE_GAME_PLAY;
    menu->page = GC_PAGE_DISC;
    gc_menu_set_disc(menu, GC_DISC_READY, NULL, NULL);
    draw_at(scene, menu, 3);
    assert(scene->edit_state.ready && !scene->edit_state.disc_launching);
    GcFrameHistory *history = gc_frame_history_create(4);
    GcFrameRuntime runtime = {0};
    GcBootControl boot = {0};
    const uint32_t samples[5] = {1, 3, 5, 7, 11};
    assert(history && gc_face_random_init(samples, &runtime.random));
    LaunchRandom random = {.random = &runtime.random};
    gc_scene_set_card_random(scene, launch_random, &random);
    assert(gc_frame_history_save(history, 0, menu, &boot, scene, &runtime));
    gc_menu_press(menu, GC_BUTTON_START);
    assert(menu->page == GC_PAGE_GAME_STARTED && menu->launch_requested);
    /* The local launch adapter retains the preceding Disc presentation. */
    menu->page = GC_PAGE_DISC;
    draw_at(scene, menu, 3);
    assert(!random.calls && !scene->edit_state.disc_launching);
    draw_at(scene, menu, 3 + 1 / rate);
    assert(random.calls == 1 && scene->edit_state.disc_launching);
    assert(!scene->edit_state.sampled_tick && scene->edit_state.entrance_counter == 1);
    uint64_t pixels = gc_software_frame_hash(scene->platform);
    uint64_t first_pixels = pixels;
    GcFaceRandom first_random = runtime.random;
    assert(gc_frame_history_save(history, 1, menu, &boot, scene, &runtime));
    draw_at(scene, menu, 3 + 1 / rate);
    assert(pixels == gc_software_frame_hash(scene->platform) && random.calls == 1);
    draw_at(scene, menu, 3 + 4 / rate);
    assert(scene->edit_state.sampled_tick == 3);
    assert(pixels != gc_software_frame_hash(scene->platform));
    assert(gc_frame_history_save(history, 4, menu, &boot, scene, &runtime));
    uint64_t counter = 4;
    assert(gc_frame_history_seek(history, -1, &counter, menu, &boot, scene, &runtime));
    assert(counter == 1 && scene->edit_state.disc_launching);
    assert(memcmp(&runtime.random, &first_random, sizeof(first_random)) == 0);
    draw_at(scene, menu, 3 + 1 / rate);
    assert(first_pixels == gc_software_frame_hash(scene->platform) &&
           random.calls == 1);
    assert(gc_frame_history_seek(history, -1, &counter, menu, &boot, scene, &runtime));
    assert(!counter && !menu->launch_requested && !scene->edit_state.disc_launching);
    gc_menu_press(menu, GC_BUTTON_START);
    menu->page = GC_PAGE_DISC;
    draw_at(scene, menu, 3 + 1 / rate);
    assert(random.calls == 2);
    assert(memcmp(&runtime.random, &first_random, sizeof(first_random)) == 0);
    assert(first_pixels == gc_software_frame_hash(scene->platform));
    draw_at(scene, menu, 3 + 4 / rate);
    pixels = gc_software_frame_hash(scene->platform);
    draw_at(scene, menu, 3 + 30 / rate);
    assert(scene->edit_state.sampled_tick == 29 && random.calls == 2);
    assert(pixels != gc_software_frame_hash(scene->platform));
    pixels = gc_software_frame_hash(scene->platform);
    draw_at(scene, menu, 3 + 30 / rate);
    assert(pixels == gc_software_frame_hash(scene->platform) && random.calls == 2);
    draw_at(scene, menu, 3 + 87 / rate);
    assert(scene->edit_state.sampled_tick == scene->edit_geometry.entrance_ticks);
    assert(random.calls == 2);
    gc_scene_set_card_random(scene, NULL, NULL);
    gc_frame_history_destroy(history);
}

static uint8_t calendar_alpha(const GcScene *scene, GcTransitionChannel channel) {
    return gc_page_transition_alpha(&scene->page_style, &scene->page_transitions,
                                    GC_TRANSITION_CALENDAR, channel);
}

static void page_lifecycle(GcScene *scene, gc_menu *menu) {
    const double rate = scene->startup.frame_rate;
    gc_menu_skip_startup(menu);
    draw_at(scene, menu, 0);
    assert(scene->menu_pose.glass_alpha == 255);
    assert(gc_scene_can_press(scene, menu, GC_BUTTON_RIGHT));

    gc_menu_press(menu, GC_BUTTON_RIGHT);
    assert(menu->page == GC_PAGE_FACE && menu->face == GC_FACE_CALENDAR);
    assert(!gc_scene_can_press(scene, menu, GC_BUTTON_CONFIRM));
    assert(!gc_scene_can_press(scene, menu, GC_BUTTON_CANCEL));
    draw_at(scene, menu, 2);
    assert(gc_scene_can_press(scene, menu, GC_BUTTON_CONFIRM));
    gc_menu_press(menu, GC_BUTTON_CONFIRM);
    assert(menu->page == GC_PAGE_CALENDAR);
    assert(!gc_scene_can_press(scene, menu, GC_BUTTON_CONFIRM));
    draw_at(scene, menu, 0);
    assert(calendar_alpha(scene, GC_TRANSITION_VALUES) == 0);

    /* Independent native trace: values begin immediately, the grid after
     * 20 ticks, and text after 70. Each channel fades over 20 ticks. */
    draw_at(scene, menu, 10 / rate);
    assert(calendar_alpha(scene, GC_TRANSITION_VALUES) == 127);
    assert(calendar_alpha(scene, GC_TRANSITION_GRID) == 0);
    assert(calendar_alpha(scene, GC_TRANSITION_TEXT) == 0);
    draw_at(scene, menu, 30 / rate);
    assert(calendar_alpha(scene, GC_TRANSITION_VALUES) == 255);
    assert(calendar_alpha(scene, GC_TRANSITION_GRID) == 127);
    draw_at(scene, menu, 80 / rate);
    assert(calendar_alpha(scene, GC_TRANSITION_TEXT) == 127);
    draw_at(scene, menu, 2);
    assert(calendar_alpha(scene, GC_TRANSITION_TEXT) == 255);
    assert(scene->menu_pose.glass_alpha == 0);
    assert(gc_scene_can_press(scene, menu, GC_BUTTON_CONFIRM));
    assert(gc_scene_can_press(scene, menu, GC_BUTTON_CANCEL));
    uint16_t entrance = scene->edit_state.entrance_counter;
    scene->edit_state.entrance_counter =
        (uint16_t)(scene->edit_geometry.entrance_ticks - 1);
    assert(!gc_scene_can_press(scene, menu, GC_BUTTON_UP));
    scene->edit_state.entrance_counter = entrance;

    uint64_t ui_ticks = scene->ui_ticks;
    uint16_t phase = scene->menu_animation.oscillator_phase;
    draw_at(scene, menu, 2);
    assert(scene->ui_ticks == ui_ticks);
    assert(scene->menu_animation.oscillator_phase == phase);

    gc_menu_press(menu, GC_BUTTON_CANCEL);
    assert(menu->page == GC_PAGE_FACE);
    assert(!gc_scene_can_press(scene, menu, GC_BUTTON_CONFIRM));
    draw_at(scene, menu, 0);
    assert(gc_page_transition_visible(&scene->page_style, &scene->page_transitions,
                                      GC_TRANSITION_CALENDAR));
    draw_at(scene, menu, 10 / rate);
    assert(calendar_alpha(scene, GC_TRANSITION_TEXT) == 127);
    draw_at(scene, menu, 30 / rate);
    assert(!gc_page_transition_visible(&scene->page_style, &scene->page_transitions,
                                       GC_TRANSITION_CALENDAR));
    assert(scene->menu_pose.glass_alpha == 255);
}

static void calendar_captions(GcScene *scene, gc_menu *menu) {
    menu->settings.language = GC_LANGUAGE_ENGLISH;
    menu->clock = (gc_date_time){2026, 10, 1, 12, 34, 56};
    assert(gc_date_time_weekday(&menu->clock) == 4);
    gc_menu_press(menu, GC_BUTTON_CONFIRM);
    assert(menu->page == GC_PAGE_CALENDAR);
    draw_at(scene, menu, 3);
    const GcLayoutTable *table =
        gc_layout_table(&scene->layouts, GC_LANGUAGE_ENGLISH, GC_LAYOUT_CALENDAR);
    /* The native page submits the captions, never bar1/bar2. Their GLH frame
     * centers coincide with Date/Time; a frame draw strikes through both. */
    for (unsigned index = 0; index < 2; ++index) {
        GcLayoutText caption;
        GcLayoutFrame bar;
        assert(gc_layout_find_text(table, index ? "txt2" : "txt1", 0, &caption));
        assert(gc_layout_find_frame(table, index ? "bar2" : "bar1", 0, &bar));
        assert(caption.box.center_y == bar.box.center_y);
        unsigned center_y = (unsigned)lroundf(240 + (caption.box.center_y - 224) *
                                                        scene->pixel_scale_y);
        unsigned left = (unsigned)lroundf(caption.box.center_x + 28 - 85);
        for (unsigned y = center_y - 1; y <= center_y + 1; ++y)
            for (unsigned x = left; x < left + 15; ++x) {
                uint8_t rgba[4];
                assert(gc_software_read_pixel(scene->platform, x, y, rgba));
                assert(rgba[2] < 120);
            }
    }
    uint64_t pixels = gc_software_frame_hash(scene->platform);
    draw_at(scene, menu, 3);
    assert(pixels == gc_software_frame_hash(scene->platform));
    menu->clock.day = 2;
    assert(gc_date_time_weekday(&menu->clock) == 5);
}

static void disc_status_fades(GcScene *scene, gc_menu *menu) {
    const double rate = scene->startup.frame_rate;
    GcDisc disc = {0};
    assert(gc_disc_create_dummy(menu->region, &disc) == GC_DISC_IMAGE_OK);
    assert(gc_scene_set_disc(scene, &disc));
    const GcDiscMetadata *metadata = gc_disc_metadata(&disc, GC_LANGUAGE_ENGLISH);
    assert(metadata);
    gc_menu_set_disc(menu, GC_DISC_ABSENT, NULL, NULL);
    gc_menu_press(menu, GC_BUTTON_CANCEL);
    draw_at(scene, menu, 2);
    gc_menu_press(menu, GC_BUTTON_CANCEL);
    assert(menu->page == GC_PAGE_CUBE);
    draw_at(scene, menu, 2);
    gc_menu_press(menu, GC_BUTTON_UP);
    assert(menu->face == GC_FACE_GAME_PLAY && menu->page == GC_PAGE_FACE);
    draw_at(scene, menu, 2);
    assert(scene->disc_face_ticks[1] == 20 && !scene->disc_face_ticks[2]);

    /* USA 1155c crossfades absent media into the questionmark for an open
     * lid. Repeated draws hold the native counters and actual pixels. */
    gc_menu_set_disc(menu, GC_DISC_LID_OPEN, NULL, NULL);
    draw_at(scene, menu, 2 + 10 / rate);
    assert(scene->disc_face_ticks[1] == 10 && scene->disc_face_ticks[2] == 10);
    uint64_t paused = gc_software_frame_hash(scene->platform);
    draw_at(scene, menu, 2 + 10 / rate);
    assert(paused == gc_software_frame_hash(scene->platform));
    draw_at(scene, menu, 2 + 20 / rate);
    assert(!scene->disc_face_ticks[1] && scene->disc_face_ticks[2] == 20);
    gc_menu_set_disc(menu, GC_DISC_READY, metadata->full_title, metadata->full_company);
    draw_at(scene, menu, 2 + 30 / rate);
    assert(scene->disc_face_ticks[0] == 10 && scene->disc_face_ticks[2] == 10);
    draw_at(scene, menu, 2 + 40 / rate);
    assert(scene->disc_face_ticks[0] == 20 && !scene->disc_face_ticks[2]);

    gc_menu_press(menu, GC_BUTTON_CONFIRM);
    assert(menu->page == GC_PAGE_DISC);
    draw_at(scene, menu, 3);
    assert(scene->disc_metadata_ticks == 20 && scene->disc_text_ticks[0] == 20);
    gc_menu_set_disc(menu, GC_DISC_ABSENT, NULL, NULL);
    draw_at(scene, menu, 3 + 1 / rate);
    assert(scene->disc_metadata_ticks == 19);
    assert(scene->disc_text_ticks[0] == 19 && scene->disc_text_ticks[3] == 1);
    assert(scene->value_morph_state.drawn.kind == GC_VALUE_MORPH_DISC);
    assert(scene->value_morph_state.drawn.previous[0] == 0);
    assert(scene->value_morph_state.drawn.next[0] == 0);
    assert(scene->value_morph_state.current.next[0] == 1);
    assert(scene->value_morph_state.current.ticks[0] == 1);
    /* Fullscreen status faders are independent from the frozen cube face. */
    assert(scene->disc_face_ticks[0] == 20 && !scene->disc_face_ticks[1]);
    paused = gc_software_frame_hash(scene->platform);
    draw_at(scene, menu, 3 + 1 / rate);
    assert(paused == gc_software_frame_hash(scene->platform));
    draw_at(scene, menu, 3 + 2 / rate);
    assert(scene->value_morph_state.drawn.next[0] == 1);
    assert(scene->value_morph_state.drawn.ticks[0] == 1);
    draw_at(scene, menu, 3 + 20 / rate);
    assert(!scene->disc_metadata_ticks && !scene->disc_text_ticks[0]);
    assert(scene->disc_text_ticks[3] == 20);
    gc_menu_set_disc(menu, GC_DISC_UNREADABLE, NULL, NULL);
    draw_at(scene, menu, 3 + 40 / rate);
    assert(!scene->disc_text_ticks[3] && scene->disc_text_ticks[2] == 20);
    gc_menu_set_disc(menu, GC_DISC_READING, NULL, NULL);
    draw_at(scene, menu, 3 + 50 / rate);
    assert(scene->disc_text_ticks[2] == 10 && scene->disc_text_ticks[4] == 10);
    draw_at(scene, menu, 3 + 60 / rate);
    assert(!scene->disc_text_ticks[2] && scene->disc_text_ticks[4] == 20);
    gc_menu_set_disc(menu, GC_DISC_FATAL, NULL, NULL);
    draw_at(scene, menu, 3 + 70 / rate);
    assert(scene->fatal_error_latched && scene->fatal_error_ticks == 10);
    assert(!gc_scene_can_press(scene, menu, GC_BUTTON_CANCEL));
    uint64_t fatal_pixels = gc_software_frame_hash(scene->platform);
    draw_at(scene, menu, 3 + 70 / rate);
    assert(fatal_pixels == gc_software_frame_hash(scene->platform));
    draw_at(scene, menu, 3 + 80 / rate);
    assert(!scene->disc_text_ticks[4] && scene->disc_text_ticks[1] == 20);
    assert(scene->fatal_error_ticks == 20);
    gc_menu_press(menu, GC_BUTTON_CANCEL);
    assert(menu->page == GC_PAGE_DISC);
    gc_menu_set_disc(menu, GC_DISC_LID_OPEN, NULL, NULL);
    draw_at(scene, menu, 3 + 90 / rate);
    assert(scene->fatal_error_latched && scene->fatal_error_ticks == 20);
    assert(!gc_scene_can_press(scene, menu, GC_BUTTON_CANCEL));
    assert(gc_scene_set_disc(scene, NULL));
    gc_disc_destroy(&disc);
}

int main(int argc, char **argv) {
    if (argc < 2)
        return EXIT_SUCCESS;
    CcPlatform *platform = cc_platform_create("Scene verification", 640, 480);
    GcScene scene = {0};
    gc_menu *menu = calloc(1, sizeof(*menu));
    assert(platform && menu && gc_scene_init(&scene, platform, argv[1]));
    gc_menu_init(menu,
                 scene.startup.frame_rate == 50 ? GC_REGION_EUROPE : GC_REGION_USA);
    startup_projection(&scene, menu);
    page_lifecycle(&scene, menu);
    calendar_captions(&scene, menu);
    disc_launch(&scene, menu);
    disc_status_fades(&scene, menu);
    if (argc > 2)
        assert(gc_software_write_frame(platform, argv[2]));
    gc_scene_destroy(&scene);
    cc_platform_destroy(platform);
    free(menu);
    puts("Native scene lifecycle passed.");
    return EXIT_SUCCESS;
}
