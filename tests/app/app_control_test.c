#define _POSIX_C_SOURCE 200809L
#include "gamecube/render.h"
#include "render/software/software.h"
#include "audio/audio_internal.h"
#include "app/recording.h"

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>

static int test_clock(clockid_t clock_id, struct timespec *value);
static bool test_poll(CcPlatform *platform, CcEvent *event);
static bool test_is_fullscreen(CcPlatform *platform);
static bool test_set_fullscreen(CcPlatform *platform, bool fullscreen);
static void test_draw(GcScene *scene, const gc_menu *menu);
static void test_redraw(GcScene *scene, const gc_menu *menu);
static bool test_recording_frame(CcRecording *recording, double now);
static CcRecording *test_recording_open(CcPlatform *platform, GcAudio *audio,
                                        unsigned video_rate, bool audible,
                                        bool half_size, CcCaptureAudioMode mode);

/* Mock only the host clock/event boundary. The actual CLI, configuration,
 * native controllers, resource decoder and scene renderer remain active.
 */
#define clock_gettime test_clock
#define cc_platform_poll test_poll
#define cc_platform_is_fullscreen test_is_fullscreen
#define cc_platform_set_fullscreen test_set_fullscreen
#define gc_scene_draw test_draw
#define gc_scene_redraw test_redraw
#define cc_recording_frame test_recording_frame
#define gc_recording_open_with_audio test_recording_open
#define main test_application_main
#include "app/main.c"
#undef main
#undef gc_scene_draw
#undef gc_scene_redraw
#undef cc_recording_frame
#undef gc_recording_open_with_audio
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
static bool test_recording_events;
static bool recording_fail_clock_on_quit;
static bool recording_clock_failed;
static bool recording_interrupt_on_open;
static volatile sig_atomic_t retained_signal_calls;
static bool test_volume_events;
static unsigned volume_poll_ticks;
static unsigned redrawn_frames;
static float volume_last_alpha;
static GcScene *volume_recording_scene;
static unsigned volume_recording_submissions;
static bool volume_recording_failure;

static bool test_recording_frame(CcRecording *recording, double now) {
    if (volume_recording_scene) {
        uint8_t rgba[4];
        assert(gc_software_read_pixel(volume_recording_scene->platform, 496, 21, rgba));
        assert(rgba[0] == 255 && rgba[1] == 255 && rgba[2] == 255 && rgba[3] == 255);
        ++volume_recording_submissions;
        if (volume_recording_failure)
            return false;
    }
    return cc_recording_frame(recording, now);
}

static void retained_test_signal(int signal_number) {
    (void)signal_number;
    ++retained_signal_calls;
}

static CcRecording *test_recording_open(CcPlatform *platform, GcAudio *audio,
                                        unsigned video_rate, bool audible,
                                        bool half_size, CcCaptureAudioMode mode) {
    CcRecording *recording = gc_recording_open_with_audio(platform, audio, video_rate,
                                                          audible, half_size, mode);
    if (recording && recording_interrupt_on_open)
        assert(raise(SIGTERM) == 0);
    return recording;
}

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

static bool poll_recording_events(CcEvent *event) {
    static const CcKey keys[] = {CC_KEY_UNKNOWN,
                                 '.',
                                 '.',
                                 CC_KEY_UNKNOWN,
                                 'r',
                                 CC_KEY_UNKNOWN,
                                 'r',
                                 '.',
                                 '.',
                                 CC_KEY_UNKNOWN,
                                 CC_KEY_UNKNOWN,
                                 CC_KEY_UNKNOWN};
    unsigned phase = event_phase++;
    if (phase == sizeof(keys) / sizeof(keys[0])) {
        assert(drawn_counter == 1 && drawn_frames == 3);
        recording_clock_failed = recording_fail_clock_on_quit;
        event->type = CC_EVENT_QUIT;
        return true;
    }
    if (phase >= sizeof(keys) / sizeof(keys[0]) || keys[phase] == CC_KEY_UNKNOWN)
        return false;
    bool up = phase == 2 || phase == 6 || phase == 8;
    *event =
        (CcEvent){.type = up ? CC_EVENT_KEY_UP : CC_EVENT_KEY_DOWN, .key = keys[phase]};
    return true;
}

static int test_clock(clockid_t clock_id, struct timespec *value) {
    (void)clock_id;
    if (clock_failure || recording_clock_failed)
        return -1;
    if (test_volume_events) {
        *value =
            (struct timespec){.tv_sec = volume_poll_ticks / 10,
                              .tv_nsec = (long)(volume_poll_ticks % 10) * 100000000};
        return 0;
    }
    if (test_recording_events) {
        *value = (struct timespec){.tv_sec = event_phase / 8,
                                   .tv_nsec = (long)(event_phase % 8) * 125000000};
        return 0;
    }
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
    if (test_recording_events)
        return poll_recording_events(event);
    if (test_volume_events) {
        unsigned phase = event_phase++;
        if (phase == 0 || phase == 2) {
            *event = (CcEvent){.type = phase ? CC_EVENT_KEY_UP : CC_EVENT_KEY_DOWN,
                               .key = (CcKey)'='};
            return true;
        }
        if (phase == 24) {
            assert(drawn_counter == 0 && drawn_frames == 1 && redrawn_frames > 5);
            assert(volume_last_alpha == 0);
            event->type = CC_EVENT_QUIT;
            return true;
        }
        ++volume_poll_ticks;
        return false;
    }
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
    if (test_recording_events)
        assert(menu->page == GC_PAGE_STARTUP);
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

static void test_redraw(GcScene *scene, const gc_menu *menu) {
    assert(test_volume_events && scene->frame_counter == 0 && menu->page_elapsed == 0);
    assert(scene->volume_indicator_percent == 200);
    assert(scene->volume_indicator_alpha <= volume_last_alpha);
    volume_last_alpha = scene->volume_indicator_alpha;
    ++redrawn_frames;
    uint64_t ticks = scene->ui_ticks;
    double fraction = scene->animation_fraction;
    gc_scene_redraw(scene, menu);
    assert(scene->ui_ticks == ticks && scene->animation_fraction == fraction);
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

static void test_music_volume_controls(void) {
    GcAudio *audio = calloc(1, sizeof(*audio));
    assert(audio);
    atomic_init(&audio->menu_volume_adjustment, 0);
    gc_menu menu;
    gc_menu_init(&menu, GC_REGION_USA);
    GcScene scene = {0};
    GcFrameRuntime runtime = {.startup_waiting = true};
    AppRuntime app = {
        .menu = &menu, .scene = &scene, .runtime = &runtime, .audio = audio};
    AppPlayback playback = {.running = true};
    test_window_events = true;
    send_window_key(&app, &playback, (CcKey)'=', true);
    assert(gc_audio_menu_volume(audio) == 200);
    assert(scene.volume_indicator_alpha == 1);
    assert(scene.volume_indicator_percent == 200);
    send_window_key(&app, &playback, (CcKey)'=', false);
    send_window_key(&app, &playback, (CcKey)'-', true);
    assert(gc_audio_menu_volume(audio) == 100);
    send_window_key(&app, &playback, (CcKey)'-', true);
    assert(gc_audio_menu_volume(audio) == 100);
    send_window_key(&app, &playback, (CcKey)'-', false);
    for (unsigned press = 0; press < 8; ++press) {
        send_window_key(&app, &playback, (CcKey)'-', true);
        send_window_key(&app, &playback, (CcKey)'-', false);
    }
    assert(gc_audio_menu_volume(audio) == 0);
    for (unsigned press = 0; press < 8; ++press) {
        send_window_key(&app, &playback, (CcKey)'=', true);
        send_window_key(&app, &playback, (CcKey)'=', false);
        assert(gc_audio_menu_volume(audio) == (press + 1) * 100);
    }
    for (unsigned press = 0; press < 8; ++press) {
        send_window_key(&app, &playback, (CcKey)'-', true);
        send_window_key(&app, &playback, (CcKey)'-', false);
        assert(gc_audio_menu_volume(audio) == (7 - press) * 100);
    }
    send_window_key(&app, &playback, (CcKey)'+', true);
    send_window_key(&app, &playback, (CcKey)'=', true);
    assert(gc_audio_menu_volume(audio) == 100); /* Shift shares the same held key. */
    send_window_event(
        &app, &playback,
        (CcEvent){.type = CC_EVENT_KEY_DOWN, .key = (CcKey)'=', .key_repeat = true});
    assert(gc_audio_menu_volume(audio) == 100);
    advance_volume_overlay(&app, &playback, 0.34);
    assert(gc_audio_menu_volume(audio) == 100);
    advance_volume_overlay(&app, &playback, 0.02);
    assert(gc_audio_menu_volume(audio) == 200);
    advance_volume_overlay(&app, &playback, 0.1);
    assert(gc_audio_menu_volume(audio) == 300);
    send_window_event(
        &app, &playback,
        (CcEvent){.type = CC_EVENT_POINTER_LEAVE, .cancel_capture = true});
    assert(playback.volume_keys_held == 0);
    advance_volume_overlay(&app, &playback, 0.3);
    assert(scene.volume_indicator_alpha == 1);
    advance_volume_overlay(&app, &playback, 0.9);
    assert(fabsf(scene.volume_indicator_alpha - 0.5f) < 0.0001f);
    assert(playback.volume_overlay_changed);
    volume_overlay_presented(&playback);
    assert(!playback.volume_overlay_changed);
    advance_volume_overlay(&app, &playback, 0.6);
    assert(scene.volume_indicator_alpha < 0.0001f);
    advance_volume_overlay(&app, &playback, 1);
    assert(scene.volume_indicator_alpha == 0);
    send_window_key(&app, &playback, (CcKey)'=', true);
    send_window_key(&app, &playback, (CcKey)'=', false);
    assert(gc_audio_menu_volume(audio) == 400);
    assert(scene.volume_indicator_alpha == 1);
    send_window_key(&app, &playback, (CcKey)'=', true);
    advance_volume_overlay(&app, &playback, 10);
    assert(gc_audio_menu_volume(audio) == 800);
    advance_volume_overlay(&app, &playback, 5);
    assert(gc_audio_menu_volume(audio) == 800);
    send_window_key(&app, &playback, (CcKey)'=', false);
    send_window_key(&app, &playback, (CcKey)'-', true);
    send_window_key(&app, &playback, (CcKey)'=', true);
    unsigned before = gc_audio_menu_volume(audio);
    advance_volume_overlay(&app, &playback, 5);
    assert(gc_audio_menu_volume(audio) == before); /* Both held cancel repeat. */
    assert(scene.volume_indicator_alpha == 1);
    assert(app.counter == 0 && menu.page_elapsed == 0);
    assert(!playback.start_requested && !playback.history_changed);
    assert(!runtime.input.held && !runtime.input.pending_pressed);
    assert(!runtime.boot_input.controllers[0].held);
    test_window_events = false;
    free(audio);
}

static void test_paused_volume_recording(void) {
    const char *path = "Files/volume-overlay-recording-test.mp4";
    assert(mkdir("Files", 0700) == 0 || errno == EEXIST);
    CcPlatform *platform = cc_platform_create("Volume recording", 640, 480);
    GcScene *scene = calloc(1, sizeof(*scene));
    assert(platform && scene);
    scene->platform = platform;
    scene->font_texture = 1;
    scene->ui_ticks = 123;
    scene->animation_fraction = 0.75;
    gc_menu menu;
    gc_menu_init(&menu, GC_REGION_USA);
    menu.page = GC_PAGE_GAME_STARTED;
    GcFrameRuntime runtime = {0};
    CcRecordingOptions options = {
        .platform = platform, .sample_rate = 48000, .video_rate = 60};
    CcRecording *recording = cc_recording_open_path(&options, path);
    assert(recording);
    AppRuntime app = {.menu = &menu,
                      .scene = scene,
                      .runtime = &runtime,
                      .recording = recording,
                      .inspection = true};
    AppPlayback playback = {.frames = 37, .volume_overlay_changed = true};
    test_volume_events = true;
    volume_poll_ticks = 1;
    volume_last_alpha = 1;
    volume_recording_scene = scene;
    volume_recording_submissions = 0;
    gc_scene_volume_indicator(scene, 200, 1);
    assert(present_volume_overlay(&app, &playback));
    assert(volume_recording_submissions == 1 && !playback.volume_overlay_changed);
    assert(app.counter == 0 && playback.frames == 37 && !playback.history_changed);
    assert(menu.page_elapsed == 0 && scene->ui_ticks == 123);
    assert(scene->animation_fraction == 0.75);
    volume_recording_failure = true;
    assert(!present_volume_overlay(&app, &playback));
    assert(volume_recording_submissions == 2);
    assert(app.counter == 0 && playback.frames == 37 && !playback.history_changed);
    volume_recording_failure = false;
    volume_recording_scene = NULL;
    test_volume_events = false;
    assert(cc_recording_close(recording, 0.2));
    assert(remove(path) == 0);
    cc_platform_destroy(platform);
    free(scene);
}

static uint32_t recording_u32(const uint8_t *bytes) {
    return (uint32_t)bytes[0] << 24 | (uint32_t)bytes[1] << 16 |
           (uint32_t)bytes[2] << 8 | bytes[3];
}

static uint64_t recording_u64(const uint8_t *bytes) {
    return (uint64_t)recording_u32(bytes) << 32 | recording_u32(bytes + 4);
}

static const uint8_t *recording_box(const uint8_t *bytes, size_t size,
                                    const char type[4]) {
    size_t offset = 0;
    while (size - offset >= 8) {
        uint32_t length = recording_u32(bytes + offset);
        assert(length >= 8 && length <= size - offset);
        if (!memcmp(bytes + offset + 4, type, 4))
            return bytes + offset;
        offset += length;
    }
    return NULL;
}

static const uint8_t *recording_child(const uint8_t *parent, const char type[4]) {
    uint32_t length = recording_u32(parent);
    assert(length >= 8);
    const uint8_t *child = recording_box(parent + 8, length - 8, type);
    assert(child);
    return child;
}

static void check_recording_file(const char *path, uint64_t duration_ticks,
                                 bool half_size) {
    FILE *file = fopen(path, "rb");
    assert(file && fseek(file, 0, SEEK_END) == 0);
    long length = ftell(file);
    assert(length > 48 && fseek(file, 0, SEEK_SET) == 0);
    uint8_t *bytes = malloc((size_t)length);
    assert(bytes && fread(bytes, 1, (size_t)length, file) == (size_t)length);
    assert(fclose(file) == 0);
    assert(recording_u32(bytes) == 32 && !memcmp(bytes + 4, "ftyp", 4));
    assert(recording_u32(bytes + 32) == 1 && !memcmp(bytes + 36, "mdat", 4));
    uint64_t media_size = recording_u64(bytes + 40);
    assert(media_size >= 16 && media_size <= (uint64_t)length - 40);
    size_t movie_offset = 32 + (size_t)media_size;
    const uint8_t *movie = bytes + movie_offset;
    assert(recording_u32(movie) == (size_t)length - movie_offset &&
           !memcmp(movie + 4, "moov", 4));
    const uint8_t *header = recording_child(movie, "mvhd");
    assert(header[8] == 1 && recording_u32(header + 28) == 1000000 &&
           recording_u64(header + 32) == duration_ticks);
    size_t offset = 8;
    unsigned seen = 0;
    while (offset < recording_u32(movie)) {
        const uint8_t *track =
            recording_box(movie + offset, recording_u32(movie) - offset, "trak");
        if (!track)
            break;
        const uint8_t *media = recording_child(track, "mdia");
        const uint8_t *media_header = recording_child(media, "mdhd");
        const uint8_t *handler = recording_child(media, "hdlr");
        const uint8_t *table = recording_child(recording_child(media, "minf"), "stbl");
        const uint8_t *sizes = recording_child(table, "stsz");
        assert(media_header[8] == 1);
        if (!memcmp(handler + 16, "vide", 4)) {
            assert(!(seen & 1) && recording_u32(media_header + 28) == 1000000 &&
                   recording_u64(media_header + 32) == duration_ticks &&
                   recording_u32(sizes + 16) == 4);
            const uint8_t *description = recording_child(table, "stsd");
            assert(recording_u32(description) >= 16 + 86 &&
                   recording_u32(description + 12) == 1);
            const uint8_t *entry = description + 16;
            unsigned width = (unsigned)entry[32] << 8 | entry[33];
            unsigned height = (unsigned)entry[34] << 8 | entry[35];
            assert(width == (unsigned)CC_FRAME_WIDTH / (half_size ? 2u : 1u));
            assert(height == (unsigned)CC_FRAME_HEIGHT / (half_size ? 2u : 1u));
            seen |= 1;
        } else {
            uint64_t audio_frames = duration_ticks * 48000 / 1000000;
            assert(!(seen & 2) && !memcmp(handler + 16, "soun", 4) &&
                   recording_u32(media_header + 28) == 48000 &&
                   recording_u64(media_header + 32) == audio_frames &&
                   recording_u32(sizes + 12) == 8 &&
                   recording_u32(sizes + 16) == audio_frames);
            seen |= 2;
        }
        offset = (size_t)(track - movie) + recording_u32(track);
    }
    assert(seen == (duration_ticks ? 3u : 2u));
    free(bytes);
}

static void test_actual_recording(const char *ipl_path, const char *region,
                                  bool fail_clock, bool half_size, bool interrupt) {
    char relative_home[96];
    bool directory_created = false;
    for (unsigned attempt = 0; attempt < 1000; ++attempt) {
        int length =
            snprintf(relative_home, sizeof(relative_home),
                     "Files/app-recording-home-%ld-%u", (long)getpid(), attempt);
        assert(length > 0 && (size_t)length < sizeof(relative_home));
        if (mkdir(relative_home, 0700) == 0) {
            directory_created = true;
            break;
        }
        assert(errno == EEXIST);
    }
    assert(directory_created);
    char working_directory[PATH_MAX];
    assert(getcwd(working_directory, sizeof(working_directory)));
    char test_home[PATH_MAX];
    int count = snprintf(test_home, sizeof(test_home), "%s/%s", working_directory,
                         relative_home);
    assert(count > 0 && (size_t)count < sizeof(test_home));
    const char *original_home = getenv("HOME");
    char *saved_home = original_home ? strdup(original_home) : NULL;
    assert(!original_home || saved_home);
    assert(setenv("HOME", test_home, 1) == 0);
    test_recording_events = true;
    recording_fail_clock_on_quit = fail_clock;
    recording_clock_failed = false;
    recording_interrupt_on_open = interrupt;
    retained_signal_calls = 0;
    void (*previous_signal)(int) = SIG_ERR;
    if (interrupt) {
        previous_signal = signal(SIGTERM, retained_test_signal);
        assert(previous_signal != SIG_ERR);
    }
    event_phase = drawn_frames = 0;
    drawn_counter = 0;
    char *arguments[] = {"gamecube-menu", "--ipl",        (char *)ipl_path,
                         "--region",      (char *)region, "--step",
                         "--delaystart",  "--record",     "half"};
    assert(test_application_main(half_size ? 9 : 8, arguments) ==
           (fail_clock ? EXIT_FAILURE : EXIT_SUCCESS));
    if (interrupt) {
        assert(drawn_counter == 0 && drawn_frames == 0 && retained_signal_calls == 0);
        assert(signal(SIGTERM, previous_signal) == retained_test_signal);
    } else
        assert(drawn_counter == 1 && drawn_frames == 3);
    test_recording_events = false;
    recording_clock_failed = false;
    recording_interrupt_on_open = false;
    char movies[PATH_MAX];
    count = snprintf(movies, sizeof(movies), "%s/Movies", test_home);
    assert(count > 0 && (size_t)count < sizeof(movies));
    DIR *directory = opendir(movies);
    assert(directory);
    char path[PATH_MAX] = {0};
    struct dirent *entry;
    while ((entry = readdir(directory))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        assert(!path[0]);
        size_t name_length = strlen(entry->d_name);
        assert(name_length > 4 && !strcmp(entry->d_name + name_length - 4, ".mp4"));
        count = snprintf(path, sizeof(path), "%s/%s", movies, entry->d_name);
        assert(count > 0 && (size_t)count < sizeof(path));
    }
    assert(closedir(directory) == 0 && path[0]);
    check_recording_file(path,
                         interrupt    ? 0
                         : fail_clock ? 1500000
                                      : 1750000,
                         half_size);
    assert(remove(path) == 0 && rmdir(movies) == 0 && rmdir(test_home) == 0);
    if (saved_home)
        assert(setenv("HOME", saved_home, 1) == 0);
    else
        assert(unsetenv("HOME") == 0);
    free(saved_home);
}

int main(int argc, char **argv) {
    test_window_controls();
    test_music_volume_controls();
    test_paused_volume_recording();
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
    const char *mismatched_region = expected_region == GC_REGION_EUROPE ? "USA" : "EUR";
    char *mismatched_arguments[] = {
        "gamecube-menu",           "--ipl", argv[1], "--region",
        (char *)mismatched_region, "--step"};
    event_phase = drawn_frames = 0;
    assert(test_application_main(6, mismatched_arguments) == EXIT_FAILURE);
    assert(drawn_frames == 0);
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
    test_restart_events = false;
    test_volume_events = true;
    event_phase = drawn_frames = volume_poll_ticks = redrawn_frames = 0;
    drawn_counter = 0;
    volume_last_alpha = 1;
    char *volume_arguments[] = {"gamecube-menu", "--ipl",          argv[1], "--region",
                                argv[2],         "--skip-startup", "--step"};
    assert(test_application_main(7, volume_arguments) == EXIT_SUCCESS);
    assert(drawn_counter == 0 && drawn_frames == 1 && redrawn_frames > 5);
    test_volume_events = false;
    test_actual_recording(argv[1], argv[2], false, false, false);
    test_actual_recording(argv[1], argv[2], true, false, false);
    test_actual_recording(argv[1], argv[2], false, true, false);
    test_actual_recording(argv[1], argv[2], false, false, true);
    gc_card_image imported = {0};
    assert(gc_card_image_load(&imported, "Files/import.raw") == GC_CARD_IMAGE_OK);
    assert(imported.byte_count == original.byte_count &&
           !memcmp(imported.bytes, original.bytes, original.byte_count));
    gc_card_image_free(&imported);
    gc_card_image_free(&original);
    puts("Actual CLI card overrides and host-event responsiveness passed.");
    return EXIT_SUCCESS;
}
