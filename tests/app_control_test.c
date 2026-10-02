#define _POSIX_C_SOURCE 200809L
#include "gamecube/render.h"
#include "software.h"

#include <assert.h>
#include <stdio.h>
#include <time.h>

static int test_clock(clockid_t clock_id, struct timespec *value);
static bool test_poll(CcPlatform *platform, CcEvent *event);
static bool test_is_fullscreen(CcPlatform *platform);
static bool test_set_fullscreen(CcPlatform *platform, bool fullscreen);
static void test_draw(GcScene *scene, const gc_menu *menu);

/* Mock only the host clock/event boundary. The actual CLI, configuration,
 * native controllers, resource decoder and scene renderer remain active.
 */
#define clock_gettime test_clock
#define cc_platform_poll test_poll
#define cc_platform_is_fullscreen test_is_fullscreen
#define cc_platform_set_fullscreen test_set_fullscreen
#define gc_scene_draw test_draw
#define main test_application_main
#include "../src/main.c"
#undef main
#undef gc_scene_draw
#undef cc_platform_poll
#undef cc_platform_is_fullscreen
#undef cc_platform_set_fullscreen
#undef clock_gettime

static unsigned event_phase;
static unsigned absent_mask;
static uint64_t drawn_counter;
static unsigned drawn_frames;
static gc_region expected_region;
static bool test_disc_events;
static bool test_restart_events;
static uint32_t initial_disc_texture;
static bool clock_failure;
static bool test_window_events;
static bool window_event_pending;
static CcEvent window_event;
static bool window_fullscreen;
static bool window_request_failure;
static unsigned window_requests;

static bool test_is_fullscreen(CcPlatform *platform) {
    (void)platform;
    return window_fullscreen;
}

static bool test_set_fullscreen(CcPlatform *platform, bool fullscreen) {
    (void)platform;
    ++window_requests;
    if (window_request_failure)
        return false;
    window_fullscreen = fullscreen;
    return true;
}

static bool poll_disc_events(CcEvent *event) {
    static const CcKey keys[] = {'d', 'd', '.', '.', 'd',           CC_KEY_UNKNOWN,
                                 'D', '.', '.', 'D', CC_KEY_UNKNOWN};
    unsigned phase = event_phase++;
    if (phase == sizeof(keys) / sizeof(keys[0])) {
        assert(drawn_counter == 2 && drawn_frames == 3);
        event->type = CC_EVENT_QUIT;
        return true;
    }
    if (phase >= sizeof(keys) / sizeof(keys[0]))
        return false;
    if (keys[phase] == CC_KEY_UNKNOWN)
        return false;
    bool up = phase == 3 || phase == 4 || phase == 8 || phase == 9;
    *event =
        (CcEvent){.type = up ? CC_EVENT_KEY_UP : CC_EVENT_KEY_DOWN, .key = keys[phase]};
    return true;
}

static bool poll_restart_events(CcEvent *event) {
    static const CcKey keys[] = {
        '.', '.', CC_KEY_UNKNOWN, 'r', CC_KEY_UNKNOWN, 'R', CC_KEY_UNKNOWN, 'r',
        '.', '.', CC_KEY_UNKNOWN};
    unsigned phase = event_phase++;
    if (phase == sizeof(keys) / sizeof(keys[0])) {
        assert(drawn_counter == 1 && drawn_frames == 4);
        event->type = CC_EVENT_QUIT;
        return true;
    }
    if (phase >= sizeof(keys) / sizeof(keys[0]) || keys[phase] == CC_KEY_UNKNOWN)
        return false;
    bool up = phase == 1 || phase == 7 || phase == 9;
    *event = (CcEvent){.type = up ? CC_EVENT_KEY_UP : CC_EVENT_KEY_DOWN,
                       .key = keys[phase],
                       .key_repeat = phase == 5};
    return true;
}

static int test_clock(clockid_t clock_id, struct timespec *value) {
    (void)clock_id;
    if (clock_failure)
        return -1;
    *value = (struct timespec){.tv_sec = event_phase >= 3 ? 1 : 0};
    return 0;
}

static bool test_poll(CcPlatform *platform, CcEvent *event) {
    (void)platform;
    *event = (CcEvent){0};
    if (test_window_events) {
        if (!window_event_pending)
            return false;
        window_event_pending = false;
        *event = window_event;
        return true;
    }
    if (test_disc_events)
        return poll_disc_events(event);
    if (test_restart_events)
        return poll_restart_events(event);
    switch (event_phase++) {
        case 0:
        case 3:
            if (event_phase == 4) {
                /* A full second of render backlog must yield to the next
                 * host poll after ONE frame, so Space can pause promptly. */
                assert(drawn_counter == 1 && drawn_frames == 2);
            }
            *event = (CcEvent){.type = CC_EVENT_KEY_DOWN, .key = (CcKey)' '};
            return true;
        case 1:
        case 4:
            *event = (CcEvent){.type = CC_EVENT_KEY_UP, .key = (CcKey)' '};
            return true;
        case 2:
        case 5:
            return false;
        case 6:
            assert(drawn_counter == 1 && drawn_frames == 2);
            event->type = CC_EVENT_QUIT;
            return true;
        default:
            return false;
    }
}

static void test_draw(GcScene *scene, const gc_menu *menu) {
    assert(scene->frame_counter_enabled);
    assert(menu->region == expected_region);
    if (expected_region == GC_REGION_USA)
        assert(menu->settings.language == GC_LANGUAGE_ENGLISH);
    drawn_counter = scene->frame_counter;
    ++drawn_frames;
    if (test_restart_events)
        assert(menu->page == (drawn_frames >= 3 ? GC_PAGE_STARTUP : GC_PAGE_CUBE));
    if (test_disc_events) {
        gc_disc_status expected_disc =
            drawn_counter ? GC_DISC_LID_OPEN : GC_DISC_ABSENT;
        assert(menu->disc_status == expected_disc);
        assert(scene->disc && scene->disc->simulated && scene->disc_banner);
        if (!drawn_counter)
            initial_disc_texture = scene->disc_banner;
        else
            assert(scene->disc_banner == initial_disc_texture);
    }
    for (unsigned slot = 0; slot < 2; ++slot)
        assert(menu->cards[slot].status ==
               (absent_mask & (1u << slot) ? GC_CARD_ABSENT : GC_CARD_READY));
    gc_scene_draw(scene, menu);
}

static void send_window_event(AppRuntime *app, AppPlayback *playback, CcEvent event) {
    GcAppOptions options = {0};
    window_event = event;
    window_event_pending = true;
    poll_host_events(app, &options, playback);
    assert(!window_event_pending && playback->running);
}

static void send_window_key(AppRuntime *app, AppPlayback *playback, CcKey key,
                            bool down) {
    send_window_event(
        app, playback,
        (CcEvent){.type = down ? CC_EVENT_KEY_DOWN : CC_EVENT_KEY_UP, .key = key});
}

static void test_window_controls(void) {
    gc_menu menu;
    gc_menu_init(&menu, GC_REGION_USA);
    gc_menu_skip_startup(&menu);
    gc_menu_press(&menu, GC_BUTTON_LEFT);
    assert(menu.page == GC_PAGE_FACE);
    GcScene scene = {0};
    GcFrameRuntime runtime = {0};
    AppRuntime app = {.menu = &menu, .scene = &scene, .runtime = &runtime};
    AppPlayback playback = {.running = true};
    gc_frame_control_init(&playback.frame_control, true);
    test_window_events = window_fullscreen = true;
    window_requests = 0;

    /* Cancel returns home, but autorepeat from that same press stays Cancel. */
    send_window_key(&app, &playback, CC_KEY_ESCAPE, true);
    GcInputFrame input;
    assert(gc_input_control_sample(&runtime.input, &input));
    assert(input.pressed == GC_INPUT_B && window_requests == 0);
    gc_menu_press(&menu, GC_BUTTON_CANCEL);
    assert(menu.page == GC_PAGE_CUBE);
    send_window_key(&app, &playback, CC_KEY_ESCAPE, true);
    assert(window_fullscreen && window_requests == 0);
    assert(gc_input_control_sample(&runtime.input, &input) && input.pressed == 0);
    send_window_key(&app, &playback, CC_KEY_ESCAPE, false);
    assert(runtime.input.held == 0 && runtime.boot_input.controllers[0].held == 0);
    gc_input_control_sample(&runtime.input, &input);

    /* A fresh home press changes only the window, even with inspection paused. */
    playback.history_changed = false;
    send_window_key(&app, &playback, CC_KEY_ESCAPE, true);
    send_window_key(&app, &playback, CC_KEY_ESCAPE, true);
    send_window_key(&app, &playback, CC_KEY_ESCAPE, false);
    assert(!window_fullscreen && window_requests == 1);
    assert(!runtime.input.held && !runtime.input.pending_pressed);
    assert(!runtime.boot_input.controllers[0].held && !playback.history_changed);
    assert(playback.frame_control.paused && playback.frame_control.pending_steps == 0);
    send_window_key(&app, &playback, CC_KEY_ESCAPE, true);
    send_window_key(&app, &playback, CC_KEY_ESCAPE, false);
    assert(window_requests == 1); /* Windowed Escape never enters fullscreen. */

    const gc_page pages[] = {GC_PAGE_STARTUP, GC_PAGE_CALENDAR,    GC_PAGE_OPTIONS,
                             GC_PAGE_CARDS,   GC_PAGE_CARD_ACTION, GC_PAGE_CARD_CONFIRM,
                             GC_PAGE_MESSAGE, GC_PAGE_DISC};
    for (size_t index = 0; index < sizeof(pages) / sizeof(pages[0]); ++index) {
        menu.page = pages[index];
        gc_input_control_init(&runtime.input);
        send_window_key(&app, &playback, CC_KEY_ESCAPE, true);
        assert(runtime.input.pending_pressed == GC_INPUT_B && window_requests == 1);
        send_window_key(&app, &playback, CC_KEY_ESCAPE, false);
    }
    gc_input_control_init(&runtime.input);
    runtime.startup_waiting = true;
    menu.page = GC_PAGE_CUBE;
    send_window_key(&app, &playback, CC_KEY_ESCAPE, true);
    assert(playback.start_requested && window_requests == 1);
    send_window_key(&app, &playback, CC_KEY_ESCAPE, false);

    /* F is available during startup; Shift and OS repeat cannot toggle twice. */
    playback.start_requested = false;
    send_window_key(&app, &playback, (CcKey)'f', true);
    send_window_key(&app, &playback, (CcKey)'F', true);
    assert(window_fullscreen && window_requests == 2 && !playback.start_requested);
    send_window_key(&app, &playback, (CcKey)'F', false);
    send_window_key(&app, &playback, (CcKey)'F', true);
    assert(!window_fullscreen && window_requests == 3);
    send_window_event(
        &app, &playback,
        (CcEvent){.type = CC_EVENT_POINTER_LEAVE, .cancel_capture = true});
    assert(!playback.fullscreen_key_held && !playback.escape_held);
    runtime.startup_waiting = false;
    send_window_event(
        &app, &playback,
        (CcEvent){.type = CC_EVENT_KEY_DOWN, .key = (CcKey)'f', .key_repeat = true});
    assert(!window_fullscreen && window_requests == 3);
    send_window_key(&app, &playback, (CcKey)'f', true);
    assert(window_fullscreen && window_requests == 4);
    send_window_key(&app, &playback, (CcKey)'f', false);
    window_request_failure = true;
    send_window_key(&app, &playback, CC_KEY_ESCAPE, true);
    send_window_key(&app, &playback, CC_KEY_ESCAPE, false);
    assert(window_fullscreen && window_requests == 5);
    assert(!runtime.input.pending_pressed && !runtime.input.held);
    window_request_failure = false;
    playback.restart_requested = false;
    send_window_key(&app, &playback, (CcKey)'r', true);
    assert(playback.restart_requested && !runtime.input.pending_pressed);
    playback.restart_requested = false;
    send_window_key(&app, &playback, (CcKey)'R', true);
    assert(!playback.restart_requested); /* A held R cannot continually restart. */
    send_window_key(&app, &playback, (CcKey)'R', false);
    send_window_key(&app, &playback, (CcKey)'R', true);
    assert(playback.restart_requested && window_requests == 5);
    send_window_event(
        &app, &playback,
        (CcEvent){.type = CC_EVENT_POINTER_LEAVE, .cancel_capture = true});
    assert(!playback.restart_key_held);
    playback.restart_requested = false;
    send_window_event(
        &app, &playback,
        (CcEvent){.type = CC_EVENT_KEY_DOWN, .key = (CcKey)'r', .key_repeat = true});
    assert(!playback.restart_requested);
    test_window_events = false;
}

int main(int argc, char **argv) {
    test_window_controls();
    if (argc < 3)
        return EXIT_SUCCESS;
    GcConfig config;
    gc_config_init(&config);
    config.dummy_data[0] = config.dummy_data[1] = true;
    assert(gc_config_write(&config, "Files/config.ini") == GC_CONFIG_OK);
    gc_card_image original = {0};
    assert(gc_config_create_card(&config, 0, 0, &original) == GC_CARD_IMAGE_OK);
    assert(gc_card_image_write(&original, "Files/import.raw") == GC_CARD_IMAGE_OK);
    const char *slots[] = {"a", "b", "ab"};
    expected_region = !strcmp(argv[2], "EUR")   ? GC_REGION_EUROPE
                      : !strcmp(argv[2], "JAP") ? GC_REGION_JAPAN
                                                : GC_REGION_USA;
    for (unsigned index = 0; index < 3; ++index) {
        assert(gc_config_noinsert_mask(slots[index], &absent_mask));
        event_phase = drawn_frames = 0;
        drawn_counter = 0;
        char *arguments[] = {"gamecube-menu", "--ipl",           argv[1],
                             "--region",      argv[2],           "--skip-startup",
                             "--step",        "--noinsert",      (char *)slots[index],
                             "--card-a",      "Files/import.raw"};
        assert(test_application_main(11, arguments) == EXIT_SUCCESS);
        assert(drawn_counter == 1 && drawn_frames == 2);
    }
    if (expected_region == GC_REGION_USA) {
        absent_mask = 0;
        event_phase = drawn_frames = 0;
        drawn_counter = 0;
        char *arguments[] = {"gamecube-menu", "--ipl", argv[1], "--skip-startup",
                             "--step"};
        assert(test_application_main(5, arguments) == EXIT_SUCCESS);
        assert(drawn_counter == 1 && drawn_frames == 2);
    }
    test_disc_events = true;
    absent_mask = 0;
    event_phase = drawn_frames = 0;
    drawn_counter = 0;
    char *disc_arguments[] = {"gamecube-menu", "--ipl",          argv[1], "--region",
                              argv[2],         "--skip-startup", "--step"};
    assert(test_application_main(7, disc_arguments) == EXIT_SUCCESS);
    assert(drawn_counter == 2 && drawn_frames == 3);
    clock_failure = true;
    drawn_frames = 0;
    assert(test_application_main(7, disc_arguments) == EXIT_FAILURE);
    assert(drawn_frames == 0);
    clock_failure = false;
    test_disc_events = false;
    test_restart_events = true;
    event_phase = drawn_frames = 0;
    drawn_counter = 0;
    char *restart_arguments[] = {"gamecube-menu", "--ipl", argv[1],
                                 "--region",      argv[2], "--skip-startup",
                                 "--delaystart",  "--step"};
    assert(test_application_main(8, restart_arguments) == EXIT_SUCCESS);
    assert(drawn_counter == 1 && drawn_frames == 4);
    gc_card_image imported = {0};
    assert(gc_card_image_load(&imported, "Files/import.raw") == GC_CARD_IMAGE_OK);
    assert(imported.byte_count == original.byte_count &&
           !memcmp(imported.bytes, original.bytes, original.byte_count));
    gc_card_image_free(&imported);
    gc_card_image_free(&original);
    puts("Actual CLI card overrides and host-event responsiveness passed.");
    return EXIT_SUCCESS;
}
