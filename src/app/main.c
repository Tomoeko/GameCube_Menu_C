#define _POSIX_C_SOURCE 200809L

#include "gamecube/render.h"
#include "gamecube/services.h"
#include "gamecube/state.h"
#include "gamecube/audio.h"
#include "gamecube/navigation_sound.h"
#include "gamecube/launch_control.h"
#include "gamecube/input_control.h"
#include "gamecube/card_runtime.h"
#include "gamecube/config.h"
#include "gamecube/frame_history.h"
#include "gamecube/frame_control.h"

#include "app_options.h"
#include "app/recording.h"
#include <limits.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "console_common/support/host.h"
#include "console_common/support/display_settings.h"

static volatile sig_atomic_t application_exit_requested;

static void request_application_exit(int signal_number) {
    (void)signal_number;
    application_exit_requested = 1;
}

static bool files_directory(void) {
    return cc_host_enter_project("Files");
}

static bool seconds_now(double *seconds) {
    struct timespec value;
    if (!seconds || !cc_host_time(&value))
        return false;
    *seconds = (double)value.tv_sec + (double)value.tv_nsec / 1000000000.0;
    return isfinite(*seconds);
}

static bool private_config_path(const char *path) {
    return cc_host_path_inside("Files", path);
}

static uint32_t time_base_low(void) {
    struct timespec value;
    if (!cc_host_time(&value))
        return 0;
    /* Host timer samples seed recovered random streams independently of
     * console time-base values. */
    return (uint32_t)((uint64_t)value.tv_sec * UINT64_C(1000000000) +
                      (uint64_t)value.tv_nsec);
}

static bool key_button(CcKey key, gc_button *button) {
    switch ((int)key) {
        case CC_KEY_UP:
            *button = GC_BUTTON_UP;
            return true;
        case CC_KEY_DOWN:
            *button = GC_BUTTON_DOWN;
            return true;
        case CC_KEY_LEFT:
            *button = GC_BUTTON_LEFT;
            return true;
        case CC_KEY_RIGHT:
            *button = GC_BUTTON_RIGHT;
            return true;
        case CC_KEY_ENTER:
        case 'a':
        case 'A':
            *button = GC_BUTTON_CONFIRM;
            return true;
        case CC_KEY_ESCAPE:
        case CC_KEY_BACKSPACE:
        case 'b':
        case 'B':
            *button = GC_BUTTON_CANCEL;
            return true;
        case 's':
        case 'S':
            *button = GC_BUTTON_START;
            return true;
        default:
            return false;
    }
}

static uint8_t boot_drive(gc_disc_status status) {
    switch (status) {
        case GC_DISC_READY:
            return GC_BOOT_DRIVE_READY;
        case GC_DISC_UNREADABLE:
            return GC_BOOT_DRIVE_UNRECOGNIZED;
        case GC_DISC_READING:
            return GC_BOOT_DRIVE_PENDING;
        case GC_DISC_FATAL:
            return GC_BOOT_DRIVE_FATAL;
        case GC_DISC_LID_OPEN:
            return GC_BOOT_DRIVE_LID_OPEN;
        default:
            return GC_BOOT_DRIVE_ABSENT;
    }
}

static void boot_key(GcBootInput *input, const CcEvent *event) {
    if (event->cancel_capture) {
        for (unsigned port = 0; port < GC_BOOT_CONTROLLER_COUNT; port++)
            input->controllers[port].held = 0;
    }
    if (event->type != CC_EVENT_KEY_DOWN && event->type != CC_EVENT_KEY_UP)
        return;
    uint16_t button;
    gc_button menu_button;
    if (event->key == 'z' || event->key == 'Z')
        button = GC_BOOT_PAD_Z;
    else if (key_button(event->key, &menu_button) && menu_button != GC_BUTTON_START)
        button = gc_input_button_mask(menu_button);
    else
        return;
    input->controllers[0].valid = true;
    if (event->type == CC_EVENT_KEY_DOWN)
        input->controllers[0].held |= button;
    else
        input->controllers[0].held &= (uint16_t)~button;
}

static void set_current_clock(gc_menu *menu) {
    time_t now = time(NULL);
    struct tm value;
    if (!cc_host_localtime(&now, &value))
        return;
    gc_date_time clock = {value.tm_year + 1900, value.tm_mon + 1, value.tm_mday,
                          value.tm_hour,        value.tm_min,     value.tm_sec};
    gc_menu_set_clock(menu, &clock);
    menu->clock_changed = false;
}

static void press_with_audio(GcServices *services, gc_menu *menu, GcAudio *audio,
                             gc_button button, bool repeated) {
    gc_page old_page = menu->page;
    unsigned old_editor = menu->editor_index;
    unsigned old_slot = menu->card_slot;
    size_t old_index = menu->card_index;
    gc_card_action old_action = menu->card_action;
    bool old_confirmation = menu->confirm_yes;
    bool was_editing = menu->editing;
    gc_settings old_settings = menu->settings;
    gc_services_press(services, menu, button);
    /* These event IDs come from the cube/dialog/card native call sites. */
    unsigned event = gc_navigation_sound(old_page, menu->page);
    if (event) {
        gc_audio_event(audio, event);
        return;
    }
    if (old_page == GC_PAGE_CALENDAR || old_page == GC_PAGE_OPTIONS) {
        /* USA 26d8c/276c4 and their four editor controllers distinguish
         * page departure, editor opening, apply, cancel and value changes. */
        if (menu->page == GC_PAGE_FACE)
            event = 6;
        else if (!was_editing && menu->editing)
            event = 8;
        else if (was_editing && !menu->editing)
            event = button == GC_BUTTON_CONFIRM ? 12 : 7;
        else if (was_editing) {
            if (old_page == GC_PAGE_CALENDAR && button <= GC_BUTTON_RIGHT)
                event = button == GC_BUTTON_LEFT || button == GC_BUTTON_RIGHT ? 10 : 9;
            else if (old_page == GC_PAGE_OPTIONS &&
                     (button == GC_BUTTON_LEFT || button == GC_BUTTON_RIGHT)) {
                bool changed =
                    old_settings.sound != menu->settings.sound ||
                    old_settings.screen_position != menu->settings.screen_position ||
                    old_settings.language != menu->settings.language;
                event = changed ? 9 : 13;
            }
        } else if (old_editor != menu->editor_index) {
            event = 11;
        }
    } else if (button == GC_BUTTON_CANCEL && old_page != menu->page)
        event = 7;
    else if (button == GC_BUTTON_CONFIRM &&
             (old_page != menu->page || old_confirmation != menu->confirm_yes))
        event = 8;
    else if (old_action != menu->card_action)
        event = 10;
    else if (old_confirmation != menu->confirm_yes)
        event = 9;
    else if (old_editor != menu->editor_index || old_slot != menu->card_slot ||
             old_index != menu->card_index)
        event = 11;
    else if (old_page == GC_PAGE_CARDS && button <= GC_BUTTON_RIGHT && !repeated)
        event = 13;
    if (old_page == GC_PAGE_CARD_CONFIRM && !old_confirmation &&
        button == GC_BUTTON_CONFIRM && menu->page != old_page)
        event = 7;
    if (menu->page == GC_PAGE_MESSAGE &&
        (old_page == GC_PAGE_CARD_ACTION || old_page == GC_PAGE_CARD_CONFIRM) &&
        menu->message != GC_MESSAGE_CARD_ERASED &&
        menu->message != GC_MESSAGE_CARD_COPIED &&
        menu->message != GC_MESSAGE_CARD_MOVED &&
        menu->message != GC_MESSAGE_CARD_FORMATTED)
        event = 13;
    if (old_page == GC_PAGE_MESSAGE && menu->page == GC_PAGE_CARDS) {
        event = 7;
    }
    if (menu->launch_requested) {
        gc_audio_menu_end(audio);
        event = 22;
    }
    if (event)
        gc_audio_event(audio, event);
}

static bool card_random_sample(void *context, uint32_t *value) {
    return gc_face_random_next(context, time_base_low(), value);
}

static void card_sound_events(GcAudio *audio, const GcCardRuntimeEvents *events) {
    for (unsigned cue = 0; cue < events->operation.sound_event_count; ++cue)
        gc_audio_event(audio, events->operation.sound_events[cue]);
}

static bool menu_input(GcCardRuntime *cards, GcServices *services, gc_menu *menu,
                       GcAudio *audio, GcScene *scene, const GcInputFrame *input) {
    uint16_t actions = input->navigation | input->pressed;
    /* Original Date and Time editors give vertical value changes priority
     * over horizontal field changes when both directions are held. */
    if (menu->page == GC_PAGE_CALENDAR && menu->editing &&
        (actions & (GC_INPUT_UP | GC_INPUT_DOWN)))
        actions &= (uint16_t) ~(GC_INPUT_LEFT | GC_INPUT_RIGHT);
    for (gc_button button = GC_BUTTON_UP; button <= GC_BUTTON_START; ++button) {
        if (!(actions & gc_input_button_mask(button)))
            continue;
        if (!cards->active && !gc_scene_can_press(scene, menu, button))
            continue;
        GcCardRuntimeEvents events;
        if (!gc_card_runtime_press(cards, button, NULL, NULL, &events))
            return false;
        card_sound_events(audio, &events);
        if (!events.handled) {
            bool repeated =
                input->repeated && !(input->pressed & gc_input_button_mask(button));
            press_with_audio(services, menu, audio, button, repeated);
        }
    }
    return true;
}

typedef struct LaunchPresentation {
    gc_menu menu;
    struct LaunchPresentation *next;
} LaunchPresentation;

typedef struct {
    gc_menu *menu;
    GcServices *services;
    GcScene *scene;
    GcAudio *audio;
    GcDisc *disc;
    GcBootConfig *boot_config;
    GcBootControl *boot;
    GcFrameRuntime *runtime;
    GcFrameHistory *history;
    LaunchPresentation *presentations;
    const char *state_path;
    uint64_t card_revision;
    uint64_t counter;
    bool inspection;
    bool timed_start;
    uint64_t startup_delay_ticks;
    CcRecording *recording;
    bool audio_started;
} AppRuntime;

static bool start_audio(AppRuntime *app) {
    if (app->inspection || app->audio_started)
        return true;
    if (!gc_audio_device_start(app->audio))
        return false;
    double now;
    if (app->recording &&
        (!seconds_now(&now) || !cc_recording_audio_start(app->recording, now))) {
        gc_audio_device_stop(app->audio);
        return false;
    }
    app->audio_started = true;
    return true;
}

static bool suspend_audio(AppRuntime *app) {
    double now = NAN;
    bool clock_valid = !app->recording || seconds_now(&now);
    gc_audio_device_stop(app->audio);
    app->audio_started = false;
    return !app->recording ||
           (cc_recording_audio_stop(app->recording, now) && clock_valid);
}

static bool record_video_frame(AppRuntime *app) {
    if (!app->recording)
        return true;
    double now;
    if (seconds_now(&now) && cc_recording_frame(app->recording, now))
        return true;
    fprintf(stderr, "Recording failed: %s\n", cc_recording_error(app->recording));
    return false;
}

static bool startup_delay_ticks(double seconds, unsigned rate, uint64_t *ticks) {
    if (!ticks || !rate || !isfinite(seconds) || seconds < 0)
        return false;
    double count = ceil(seconds * rate);
    if (!isfinite(count) || count >= ldexp(1.0, 64))
        return false;
    *ticks = (uint64_t)count;
    return true;
}

static bool launch_presentation(AppRuntime *app, gc_page page, double elapsed) {
    LaunchPresentation *node = malloc(sizeof(*node));
    if (!node)
        return false;
    node->menu = *app->menu;
    node->menu.page = page;
    node->menu.page_elapsed = elapsed;
    node->next = app->presentations;
    app->presentations = node;
    app->runtime->launch_menu = &node->menu;
    return true;
}

static bool startup_tick(AppRuntime *app) {
    GcFrameRuntime *runtime = app->runtime;
    runtime->boot_input.drive_state = boot_drive(app->menu->disc_status);
    GcInputFrame sampled;
    gc_input_control_sample(&runtime->input, &sampled);
    GcBootInput input = runtime->boot_input;
    input.controllers[0].held |= sampled.pressed;
    GcBootEvents events;
    if (!gc_boot_control_step(app->boot_config, app->boot, &input, &events))
        return false;
    for (unsigned cue = 0; cue < events.sound_event_count; ++cue)
        gc_audio_event(app->audio, events.sound_events[cue]);
    if (events.language_applied) {
        gc_settings settings = app->menu->settings;
        settings.language = (gc_language)events.language;
        gc_menu_set_settings(app->menu, &settings);
        app->menu->settings_changed = true;
    }
    if (events.cube_motion)
        gc_audio_cube_motion(app->audio, events.cube_direction, events.cube_fraction);
    if (events.menu_begin) {
        gc_menu_skip_startup(app->menu);
        gc_audio_menu_begin(app->audio);
    }
    if (events.disc_handoff) {
        app->menu->launch_requested = true;
        gc_launch_control_init(&runtime->launch, GC_LAUNCH_FROM_BOOT);
        runtime->launching = true;
    }
    return true;
}

static bool menu_tick(AppRuntime *app) {
    GcFrameRuntime *runtime = app->runtime;
    GcInputFrame sampled;
    gc_input_control_sample(&runtime->input, &sampled);
    gc_page page = app->menu->page;
    double elapsed = app->menu->page_elapsed;
    if (!menu_input(&runtime->cards, app->services, app->menu, app->audio, app->scene,
                    &sampled))
        return false;
    GcCardRuntimeEvents events;
    if (!gc_card_runtime_tick(&runtime->cards, 1, &events))
        return false;
    card_sound_events(app->audio, &events);
    if (app->menu->launch_requested) {
        if (!launch_presentation(app, page, elapsed))
            return false;
        gc_launch_control_init(&runtime->launch, GC_LAUNCH_FROM_MENU);
        runtime->launching = true;
    }
    return true;
}

static bool update_resources_and_settings(AppRuntime *app) {
    gc_menu *menu = app->menu;
    if (app->card_revision != app->services->revision) {
        gc_frame_history_reset(app->history);
        if (!gc_scene_set_cards(app->scene, app->services->cards)) {
            fprintf(stderr, "Could not update Memory Card artwork.\n");
            return false;
        }
        app->card_revision = app->services->revision;
    }
    /* Native Sound editing applies its output mode only on confirmation.
     * The candidate still drives the value cubes while the existing mix runs. */
    gc_sound sound = menu->page == GC_PAGE_OPTIONS && menu->editing
                         ? menu->settings_before_edit.sound
                         : menu->settings.sound;
    gc_audio_set_mono(app->audio, sound == GC_SOUND_MONO);
    if (!menu->settings_changed && !menu->clock_changed)
        return true;
    if (menu->disc_status == GC_DISC_READY && app->disc->language_count) {
        const GcDiscMetadata *metadata =
            gc_disc_metadata(app->disc, menu->settings.language);
        gc_menu_set_disc(menu, GC_DISC_READY, metadata->game_name, metadata->company);
    }
    if (gc_state_save(menu, app->state_path, (int64_t)time(NULL)) == GC_STATE_OK)
        return true;
    fprintf(stderr, "Could not save system settings.\n");
    return false;
}

static bool disc_test_key(GcFrameRuntime *runtime, const CcEvent *event) {
    if (!runtime || !event ||
        (event->type != CC_EVENT_KEY_DOWN && event->type != CC_EVENT_KEY_UP) ||
        (event->key != 'd' && event->key != 'D'))
        return false;
    bool down = event->type == CC_EVENT_KEY_DOWN;
    if (down && !runtime->disc_toggle_held)
        runtime->pending_disc_toggle = !runtime->pending_disc_toggle;
    runtime->disc_toggle_held = down;
    return true;
}

static bool error_test_key(GcFrameRuntime *runtime, const CcEvent *event) {
    if (!runtime || !event ||
        (event->type != CC_EVENT_KEY_DOWN && event->type != CC_EVENT_KEY_UP) ||
        (event->key != 'e' && event->key != 'E'))
        return false;
    bool down = event->type == CC_EVENT_KEY_DOWN;
    if (down && !runtime->error_toggle_held)
        runtime->pending_error_toggle = !runtime->pending_error_toggle;
    runtime->error_toggle_held = down;
    return true;
}

static bool advance_disc_drive(AppRuntime *app) {
    GcFrameRuntime *runtime = app->runtime;
    if (runtime->pending_disc_toggle) {
        runtime->pending_disc_toggle = false;
        if (!gc_disc_control_toggle(&runtime->disc))
            return false;
    }
    if (!gc_disc_control_advance(&runtime->disc, 1))
        return false;
    if (runtime->disc.status == app->menu->disc_status)
        return true;
    const GcDiscMetadata *metadata =
        runtime->disc.status == GC_DISC_READY
            ? gc_disc_metadata(app->disc, app->menu->settings.language)
            : NULL;
    gc_menu_set_disc(app->menu, runtime->disc.status,
                     metadata ? metadata->game_name : NULL,
                     metadata ? metadata->company : NULL);
    return true;
}

static bool advance_host_time(AppRuntime *app, GcFrameControl *control,
                              double elapsed) {
    if (!app || !app->scene || !app->menu ||
        !gc_frame_control_elapsed(control, elapsed, app->scene->startup.frame_rate))
        return false;
    if (!app->inspection) {
        gc_menu_advance_clock(app->menu, elapsed);
        if (app->runtime && app->runtime->launch_menu &&
            app->runtime->launch_menu != app->menu)
            gc_menu_advance_clock(app->runtime->launch_menu, elapsed);
    }
    return true;
}

static bool advance_video_tick(AppRuntime *app) {
    GcFrameRuntime *runtime = app->runtime;
    double elapsed = 1.0 / app->scene->startup.frame_rate;
    if (!advance_disc_drive(app))
        return false;
    if (runtime->pending_error_toggle) {
        runtime->pending_error_toggle = false;
        if (!runtime->error.requested && !runtime->error.ticks)
            gc_error_control_init(&runtime->error, app->menu->page == GC_PAGE_STARTUP ||
                                                       runtime->startup_waiting);
        if (!gc_error_control_toggle(&runtime->error))
            return false;
    }
    if (!gc_error_control_advance(&runtime->error, 1))
        return false;
    /* The reversible error fixture holds the underlying controllers. The
     * native fatal latch remains independent and cannot be dismissed. */
    if (runtime->error.ticks) {
        GcInputFrame discarded;
        gc_input_control_sample(&runtime->input, &discarded);
        return true;
    }
    if (runtime->startup_waiting) {
        if (runtime->startup_wait_ticks < UINT64_MAX)
            ++runtime->startup_wait_ticks;
        runtime->startup_delay_elapsed =
            (double)runtime->startup_wait_ticks / app->scene->startup.frame_rate;
        if (!app->timed_start || runtime->startup_wait_ticks < app->startup_delay_ticks)
            return true;
        runtime->startup_waiting = false;
        if (!start_audio(app)) {
            fprintf(stderr, "Could not open the audio output device.\n");
            return false;
        }
    }
    bool operation_active = runtime->cards.active;
    if (app->inspection)
        gc_menu_tick(app->menu, elapsed);
    else
        gc_menu_tick_presentation(app->menu, elapsed);
    bool success = app->menu->page == GC_PAGE_STARTUP ? startup_tick(app)
                   : runtime->launching               ? true
                                                      : menu_tick(app);
    if (!success || !update_resources_and_settings(app))
        return false;
    if (operation_active && !runtime->cards.active)
        gc_frame_history_reset(app->history);
    if (runtime->launching) {
        if (gc_launch_control_tick(&runtime->launch))
            gc_audio_stop_sequence(app->audio);
        if (runtime->launch_menu) {
            if (app->inspection)
                gc_menu_tick(runtime->launch_menu, elapsed);
            else
                gc_menu_tick_presentation(runtime->launch_menu, elapsed);
        }
    }
    if (app->inspection) {
        float samples[960 * 2];
        size_t frames = 48000u / app->scene->startup.frame_rate;
        gc_audio_render(app->audio, samples, frames);
    }
    return true;
}

static void draw_video_frame_mode(AppRuntime *app, bool update_animation) {
    GcFrameRuntime *runtime = app->runtime;
    uint8_t alpha = runtime->launching ? (uint8_t)runtime->launch.alpha
                    : app->menu->page == GC_PAGE_STARTUP ? (uint8_t)app->boot->fader
                                                         : 0;
    app->scene->test_error_alpha = gc_error_control_alpha(&runtime->error);
    if (app->inspection)
        gc_scene_frame_counter(app->scene, app->counter);
    if (app->inspection || app->scene->test_error_alpha ||
        app->scene->volume_indicator_alpha > 0) {
        app->scene->inspection_fade_alpha = alpha;
        cc_platform_set_fade_alpha(app->scene->platform, 0);
    } else
        cc_platform_set_fade_alpha(app->scene->platform, (float)alpha / 255);
    if (runtime->startup_waiting)
        gc_scene_draw_wait(app->scene);
    else if (update_animation)
        gc_scene_draw(app->scene,
                      runtime->launch_menu ? runtime->launch_menu : app->menu);
    else
        gc_scene_redraw(app->scene,
                        runtime->launch_menu ? runtime->launch_menu : app->menu);
}

static void draw_video_frame(AppRuntime *app) {
    draw_video_frame_mode(app, true);
}

static bool save_video_frame(AppRuntime *app) {
    return !app->history || gc_frame_history_save(app->history, app->counter, app->menu,
                                                  app->boot, app->scene, app->runtime);
}

static void destroy_presentations(LaunchPresentation *node) {
    while (node) {
        LaunchPresentation *next = node->next;
        free(node);
        node = next;
    }
}

typedef struct {
    GcFrameControl frame_control;
    bool running;
    bool history_changed;
    bool start_requested;
    bool escape_held;
    bool escape_window_control;
    bool fullscreen_key_held;
    bool restart_key_held;
    bool restart_requested;
    bool surface_changed;
    unsigned volume_keys_held;
    double volume_repeat_elapsed;
    double volume_indicator_remaining;
    double volume_refresh_elapsed;
    bool volume_overlay_changed;
    unsigned long frames;
    double last;
} AppPlayback;

enum { MENU_VOLUME_STEP = 100 };

static void adjust_menu_volume(AppRuntime *app, AppPlayback *playback,
                               unsigned direction, unsigned steps) {
    unsigned volume = gc_audio_menu_volume(app->audio);
    unsigned change = steps * MENU_VOLUME_STEP;
    if (direction == 1)
        volume = volume < change ? 0 : volume - change;
    else
        volume = change > GC_AUDIO_MENU_VOLUME_MAX - volume ? GC_AUDIO_MENU_VOLUME_MAX
                                                            : volume + change;
    gc_audio_set_menu_volume(app->audio, volume);
    playback->volume_indicator_remaining = 1.8;
    playback->volume_overlay_changed = true;
    gc_scene_volume_indicator(app->scene, volume, 1);
}

/* This host overlay remains responsive while native frame stepping is paused. */
static void advance_volume_overlay(AppRuntime *app, AppPlayback *playback,
                                   double elapsed) {
    if (!isfinite(elapsed) || elapsed < 0)
        return;
    if (!playback->volume_keys_held && playback->volume_indicator_remaining == 0 &&
        !playback->volume_overlay_changed && app->scene->volume_indicator_alpha == 0)
        return;
    playback->volume_refresh_elapsed += elapsed;
    if (playback->volume_keys_held) {
        if (playback->volume_keys_held != 3) {
            playback->volume_repeat_elapsed += elapsed;
            if (playback->volume_repeat_elapsed >= 0.1) {
                unsigned steps =
                    (unsigned)fmin(GC_AUDIO_MENU_VOLUME_MAX / MENU_VOLUME_STEP,
                                   floor(playback->volume_repeat_elapsed / 0.1));
                playback->volume_repeat_elapsed =
                    fmod(playback->volume_repeat_elapsed, 0.1);
                adjust_menu_volume(app, playback, playback->volume_keys_held, steps);
            }
        } else
            playback->volume_repeat_elapsed = -0.25;
        playback->volume_indicator_remaining = 1.8;
    } else {
        playback->volume_indicator_remaining =
            fmax(0, playback->volume_indicator_remaining - elapsed);
    }
    float alpha = (float)fmin(1, playback->volume_indicator_remaining / 1.2);
    if (alpha != app->scene->volume_indicator_alpha) {
        gc_scene_volume_indicator(app->scene, gc_audio_menu_volume(app->audio), alpha);
        playback->volume_overlay_changed = true;
    }
}

static void volume_overlay_presented(AppPlayback *playback) {
    playback->volume_overlay_changed = false;
    playback->volume_refresh_elapsed = 0;
}

static bool present_host_redraw(AppRuntime *app, AppPlayback *playback) {
    draw_video_frame_mode(app, false);
    playback->surface_changed = false;
    volume_overlay_presented(playback);
    return record_video_frame(app);
}

static bool host_control_key(AppRuntime *app, AppPlayback *playback,
                             const CcEvent *event) {
    if (event->type != CC_EVENT_KEY_DOWN && event->type != CC_EVENT_KEY_UP)
        return false;
    bool down = event->type == CC_EVENT_KEY_DOWN;
    if (event->key == '-' || event->key == '=' || event->key == '+') {
        unsigned mask = event->key == '-' ? 1u : 2u;
        bool was_held = (playback->volume_keys_held & mask) != 0;
        if (down && event->key_repeat && !was_held)
            return true;
        if (down)
            playback->volume_keys_held |= mask;
        else
            playback->volume_keys_held &= ~mask;
        if (down && !was_held && !event->key_repeat) {
            adjust_menu_volume(app, playback, mask, 1);
            playback->volume_repeat_elapsed = -0.25;
        }
        return true;
    }
    if (down && event->key_repeat &&
        (event->key == 'r' || event->key == 'R' || event->key == 'f' ||
         event->key == 'F' || event->key == CC_KEY_ESCAPE))
        return true;
    if (event->key == 'r' || event->key == 'R') {
        if (down && !playback->restart_key_held)
            playback->restart_requested = true;
        playback->restart_key_held = down;
        return true;
    }
    if (event->key == 'f' || event->key == 'F') {
        bool was_held = playback->fullscreen_key_held;
        playback->fullscreen_key_held = down;
        if (down && !was_held)
            cc_platform_set_fullscreen(
                app->scene->platform, !cc_platform_is_fullscreen(app->scene->platform));
        return true;
    }
    if (event->key != CC_KEY_ESCAPE)
        return false;
    bool was_held = playback->escape_held;
    playback->escape_held = down;
    if (!down) {
        bool consumed = playback->escape_window_control;
        playback->escape_window_control = false;
        return consumed;
    }
    /* A held Cancel must not become a window shortcut after returning home. */
    if (was_held)
        return true;
    playback->escape_window_control = app->menu->page == GC_PAGE_CUBE &&
                                      !app->runtime->startup_waiting &&
                                      !app->menu->launch_requested;
    if (!playback->escape_window_control)
        return false;
    if (cc_platform_is_fullscreen(app->scene->platform))
        cc_platform_set_fullscreen(app->scene->platform, false);
    return true;
}

/* Host events update pending input. Controllers consume it only at video ticks,
 * so paused inspection and restored history retain the same input ordering. */
static void poll_host_events(AppRuntime *app, const GcAppOptions *options,
                             AppPlayback *playback) {
    CcEvent event;
    while (cc_platform_poll(app->scene->platform, &event)) {
        if (event.type == CC_EVENT_QUIT) {
            playback->running = false;
            continue;
        }
        if (event.type == CC_EVENT_WINDOW_RESIZED) {
            playback->surface_changed = true;
            continue;
        }
        bool key_event =
            event.type == CC_EVENT_KEY_DOWN || event.type == CC_EVENT_KEY_UP;
        bool down = event.type == CC_EVENT_KEY_DOWN;
        if (key_event &&
            gc_frame_control_key(&playback->frame_control, (int)event.key, down)) {
            if (app->runtime->startup_waiting && !options->timed_start && down &&
                (event.key == '.' || event.key == ' '))
                playback->start_requested = true;
            continue;
        }
        if (event.cancel_capture) {
            if (app->runtime->input.held || app->runtime->input.pending_pressed ||
                app->runtime->disc_toggle_held || app->runtime->pending_disc_toggle ||
                app->runtime->error_toggle_held || app->runtime->pending_error_toggle)
                playback->history_changed = true;
            gc_input_control_init(&app->runtime->input);
            playback->frame_control.held_keys = 0;
            app->runtime->disc_toggle_held = false;
            app->runtime->error_toggle_held = false;
            playback->escape_held = false;
            playback->escape_window_control = false;
            playback->fullscreen_key_held = false;
            playback->restart_key_held = false;
            playback->volume_keys_held = 0;
            playback->volume_repeat_elapsed = -0.25;
        }
        if (host_control_key(app, playback, &event))
            continue;
        bool previous_error_toggle = app->runtime->pending_error_toggle;
        bool previous_error_held = app->runtime->error_toggle_held;
        if (error_test_key(app->runtime, &event)) {
            if (app->runtime->pending_error_toggle != previous_error_toggle ||
                app->runtime->error_toggle_held != previous_error_held)
                playback->history_changed = true;
            continue;
        }
        if (!app->menu->launch_requested) {
            bool previous_toggle = app->runtime->pending_disc_toggle;
            bool previous_held = app->runtime->disc_toggle_held;
            if (disc_test_key(app->runtime, &event)) {
                if (app->runtime->pending_disc_toggle != previous_toggle ||
                    app->runtime->disc_toggle_held != previous_held)
                    playback->history_changed = true;
                continue;
            }
        }
        if (app->runtime->startup_waiting && !options->timed_start && down) {
            gc_button button;
            if (key_button(event.key, &button) && button != GC_BUTTON_START)
                playback->start_requested = true;
        }
        if (!app->runtime->startup_waiting) {
            GcBootInput previous = app->runtime->boot_input;
            boot_key(&app->runtime->boot_input, &event);
            if (memcmp(&previous, &app->runtime->boot_input, sizeof(previous)))
                playback->history_changed = true;
        }
        if (!app->runtime->startup_waiting && !app->menu->launch_requested &&
            key_event) {
            gc_button button;
            if (key_button(event.key, &button)) {
                GcInputControl previous = app->runtime->input;
                gc_input_control_button(&app->runtime->input, button, down);
                if (memcmp(&previous, &app->runtime->input, sizeof(previous)))
                    playback->history_changed = true;
            }
        }
    }
}

static void initialize_boot_input(GcBootInput *input, gc_disc_status disc,
                                  unsigned startup_sound) {
    *input = (GcBootInput){.drive_state = boot_drive(disc)};
    input->controllers[0].valid = true;
    if (startup_sound) {
        unsigned ports = startup_sound == 1 ? GC_BOOT_CONTROLLER_COUNT : 1;
        for (unsigned port = 0; port < ports; ++port)
            input->controllers[port] = (GcBootPad){true, GC_BOOT_PAD_Z};
    }
}

static bool restart_startup(AppRuntime *app, const GcAppOptions *options,
                            AppPlayback *playback) {
    if (!suspend_audio(app))
        return false;
    gc_audio_reset(app->audio);
    gc_frame_history_reset(app->history);
    destroy_presentations(app->presentations);
    app->presentations = NULL;
    gc_card_runtime_destroy(&app->runtime->cards);
    gc_menu_restart_startup(app->menu);
    app->boot_config->initial_language = (unsigned)app->menu->settings.language < 6
                                             ? (unsigned)app->menu->settings.language
                                             : 0;
    if (!gc_boot_control_init(app->boot_config, app->boot, GC_BOOT_NORMAL) ||
        !gc_scene_reset_presentation(app->scene))
        return false;
    GcDiscControl disc = app->runtime->disc;
    GcFaceRandom random = app->runtime->random;
    *app->runtime = (GcFrameRuntime){.disc = disc, .random = random};
    gc_error_control_init(&app->runtime->error, true);
    initialize_boot_input(&app->runtime->boot_input, disc.status,
                          options->startup_sound);
    gc_scene_set_boot(app->scene, app->boot_config, app->boot);
    if (!gc_card_runtime_init(&app->runtime->cards, app->services, app->menu,
                              app->scene))
        return false;
    gc_card_runtime_set_random(&app->runtime->cards, card_random_sample,
                               &app->runtime->random);
    gc_scene_set_card_random(app->scene, card_random_sample, &app->runtime->random);
    if (!update_resources_and_settings(app))
        return false;
    app->counter = 0;
    playback->frame_control.fraction = 0;
    playback->frame_control.pending_steps = 0;
    playback->start_requested = false;
    playback->restart_requested = false;
    playback->history_changed = false;
    draw_video_frame(app);
    volume_overlay_presented(playback);
    if (!record_video_frame(app) || !save_video_frame(app))
        return false;
    ++playback->frames;
    if (options->frame_limit && playback->frames >= options->frame_limit)
        playback->running = false;
    return start_audio(app);
}

static int run_application(AppRuntime *app, const GcAppOptions *options) {
    AppPlayback playback = {.running = true};
    int result = EXIT_SUCCESS;
    gc_frame_control_init(&playback.frame_control, options->inspect_frames);
    if (!seconds_now(&playback.last)) {
        fprintf(stderr, "Could not read the host monotonic clock.\n");
        return EXIT_FAILURE;
    }
    draw_video_frame(app);
    if (!record_video_frame(app) || !save_video_frame(app)) {
        playback.running = false;
        result = EXIT_FAILURE;
    }
    ++playback.frames;
    if (options->frame_limit && playback.frames >= options->frame_limit)
        playback.running = false;
    if (playback.running && !app->runtime->startup_waiting && !start_audio(app)) {
        fprintf(stderr, "Could not open the audio output device.\n");
        return EXIT_FAILURE;
    }
    while (playback.running && !application_exit_requested) {
        playback.start_requested = false;
        poll_host_events(app, options, &playback);
        double now;
        if (!seconds_now(&now)) {
            fprintf(stderr, "Could not read the host monotonic clock.\n");
            result = EXIT_FAILURE;
            break;
        }
        double elapsed = now >= playback.last ? now - playback.last : 0;
        playback.last = now;
        if (!playback.running)
            break;
        advance_volume_overlay(app, &playback, elapsed);
        if (!cc_recording_pump(app->recording, now)) {
            fprintf(stderr, "Recording failed: %s\n",
                    cc_recording_error(app->recording));
            result = EXIT_FAILURE;
            break;
        }
        if (playback.restart_requested) {
            if (!restart_startup(app, options, &playback) ||
                !seconds_now(&playback.last)) {
                fprintf(stderr, "Could not restart startup playback.\n");
                result = EXIT_FAILURE;
                break;
            }
            continue;
        }
        if (playback.start_requested) {
            app->runtime->startup_waiting = false;
            playback.history_changed = true;
            if (!start_audio(app)) {
                fprintf(stderr, "Could not open the audio output device.\n");
                result = EXIT_FAILURE;
                break;
            }
        }
        if (!advance_host_time(app, &playback.frame_control, elapsed)) {
            result = EXIT_FAILURE;
            break;
        }
        int step;
        if ((step = gc_frame_control_next(&playback.frame_control))) {
            bool restored = false;
            if (options->inspect_frames && (step < 0 || !playback.history_changed))
                restored =
                    gc_frame_history_seek(app->history, step, &app->counter, app->menu,
                                          app->boot, app->scene, app->runtime);
            if (step < 0 && !restored)
                continue;
            if (restored)
                playback.history_changed = false;
            else {
                if (!advance_video_tick(app)) {
                    playback.running = false;
                    result = EXIT_FAILURE;
                    break;
                }
                ++app->counter;
            }
            draw_video_frame(app);
            playback.surface_changed = false;
            volume_overlay_presented(&playback);
            if (!record_video_frame(app) || (!restored && !save_video_frame(app))) {
                playback.running = false;
                result = EXIT_FAILURE;
                break;
            }
            playback.history_changed = false;
            ++playback.frames;
            if (options->frame_limit && playback.frames >= options->frame_limit)
                playback.running = false;
            if (!options->inspect_frames && app->runtime->launching &&
                app->runtime->launch.stop_requested) {
                /* Audio channel checks run separately from video timing. */
                for (unsigned poll = 0; poll < 30; ++poll) {
                    if (gc_launch_control_poll(&app->runtime->launch,
                                               gc_audio_sequence_stopped(app->audio),
                                               gc_audio_active_voices(app->audio))) {
                        fprintf(stderr, "Disc launch requested; a console launch "
                                        "service is required.\n");
                        playback.running = false;
                        break;
                    }
                }
            }
        } else if (playback.surface_changed ||
                   (playback.volume_overlay_changed &&
                    playback.volume_refresh_elapsed >= 1.0 / 60)) {
            if (!present_host_redraw(app, &playback)) {
                result = EXIT_FAILURE;
                break;
            }
        }
        cc_host_sleep(1);
    }
    return result;
}

int main(int argc, char **argv) {
    if (!files_directory()) {
        fprintf(stderr, "Run from the project directory containing Files.\n");
        return EXIT_FAILURE;
    }
    GcAppOptions options;
    GcAppOptionsResult parsed_options = gc_app_options_parse(&options, argc, argv);
    if (parsed_options != GC_APP_OPTIONS_OK) {
        gc_app_options_usage(stderr);
        return parsed_options == GC_APP_OPTIONS_HELP ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    CcDisplaySettings display = {.antialiasing = true};
    if (!cc_display_settings_load_project(&display)) {
        fprintf(stderr, "Could not read or create Files/display.json.\n");
        return EXIT_FAILURE;
    }
    if (!options.antialiasing_override)
        options.antialiasing = display.antialiasing;
    if (options.record && !cc_capture_audio_mode_supported(options.record_audio)) {
        fprintf(stderr, "Web recording audio is unavailable on this platform.\n");
        return EXIT_FAILURE;
    }
    const char *card_states[2] = {"Files/card-a.raw", "Files/card-b.raw"};
    gc_menu *menu = calloc(1, sizeof(*menu));
    if (!menu)
        return EXIT_FAILURE;
    gc_menu_init(menu, options.region);
    set_current_clock(menu);
    gc_state_result state_result =
        gc_state_load(menu, options.state_path, (int64_t)time(NULL));
    if (state_result != GC_STATE_OK && state_result != GC_STATE_MISSING) {
        fprintf(stderr, "Saved system settings could not be loaded.\n");
        free(menu);
        return EXIT_FAILURE;
    }
    GcConfig config;
    gc_config_init(&config);
    if (!private_config_path(options.config_path)) {
        fprintf(stderr, "Keep the card configuration inside Files.\n");
        free(menu);
        return EXIT_FAILURE;
    }
    size_t config_line = 0;
    GcConfigResult loaded_config =
        gc_config_load(&config, options.config_path, &config_line);
    if (loaded_config == GC_CONFIG_MISSING)
        loaded_config = gc_config_write(&config, options.config_path);
    if (loaded_config != GC_CONFIG_OK) {
        if (loaded_config == GC_CONFIG_INVALID)
            fprintf(stderr, "Invalid card configuration at line %zu.\n", config_line);
        else
            fprintf(stderr, "Could not read or create the card configuration.\n");
        free(menu);
        return EXIT_FAILURE;
    }
    for (unsigned slot = 0; slot < 2; ++slot) {
        if (options.absent_cards & (1u << slot)) {
            config.slot_present[slot] = false;
            options.card_inputs[slot] = NULL;
        }
        if (!options.card_inputs[slot])
            card_states[slot] =
                slot ? "Files/test-card-b.raw" : "Files/test-card-a.raw";
    }
    GcServices *services = calloc(1, sizeof(*services));
    if (!services || !gc_config_prepare_services(&config, services, menu,
                                                 options.card_inputs, card_states)) {
        free(services);
        free(menu);
        return EXIT_FAILURE;
    }
    if (options.skip_startup)
        gc_menu_skip_startup(menu);
    GcDisc disc = {0};
    if (options.disc_path) {
        GcDiscResult loaded = gc_disc_load(options.disc_path, &disc);
        if (loaded == GC_DISC_IMAGE_OK) {
            const GcDiscMetadata *metadata =
                gc_disc_metadata(&disc, menu->settings.language);
            gc_menu_set_disc(menu, GC_DISC_READY,
                             metadata ? metadata->game_name : disc.header_title,
                             metadata ? metadata->company : "");
        } else {
            fprintf(stderr, "Disc: %s\n", gc_disc_result_text(loaded));
            gc_menu_set_disc(menu, GC_DISC_UNREADABLE, NULL, NULL);
        }
    } else if (gc_disc_create_dummy(options.region, &disc) != GC_DISC_IMAGE_OK) {
        fprintf(stderr, "Could not prepare the local dummy disc.\n");
        gc_services_destroy(services);
        free(services);
        free(menu);
        return EXIT_FAILURE;
    }
    CcPlatform *platform = cc_platform_create("GameCube Menu", 960, 720);
    GcScene scene = {0};
    bool graphics_ready = platform != NULL;
    if (graphics_ready && options.antialiasing &&
        !cc_platform_set_antialiasing(platform, true)) {
        fprintf(stderr, "Anti-aliasing is unavailable for this display.\n");
        graphics_ready = false;
    }
    if (!graphics_ready || !gc_scene_init(&scene, platform, options.ipl_path)) {
        if (graphics_ready || !platform)
            fprintf(stderr, "Cannot initialize graphics or the local IPL font.\n");
        cc_platform_destroy(platform);
        gc_services_destroy(services);
        gc_disc_destroy(&disc);
        free(services);
        free(menu);
        return EXIT_FAILURE;
    }
    /* PAL assets supply a different clock, projection and language set.
     * Mixing them with an NTSC menu would silently select incompatible data.
     */
    if (scene.native_text.europe != (options.region == GC_REGION_EUROPE)) {
        fprintf(stderr,
                "The selected region does not match the IPL resource profile.\n");
        gc_scene_destroy(&scene);
        cc_platform_destroy(platform);
        gc_services_destroy(services);
        gc_disc_destroy(&disc);
        free(services);
        free(menu);
        return EXIT_FAILURE;
    }
    menu->startup_duration =
        (double)scene.startup.menu_ticks / scene.startup.frame_rate;
    GcBootConfig boot_config;
    GcBootControl boot;
    GcFrameRuntime runtime = {0};
    gc_error_control_init(&runtime.error, menu->page == GC_PAGE_STARTUP);
    initialize_boot_input(&runtime.boot_input, menu->disc_status,
                          options.startup_sound);
    bool boot_ready = gc_boot_config_init(&boot_config, &scene.startup) &&
                      gc_disc_control_init(&runtime.disc, menu->disc_status);
    if (boot_ready) {
        boot_config.initial_language = (unsigned)menu->settings.language < 6
                                           ? (unsigned)menu->settings.language
                                           : 0;
        if (options.boot_phase == GC_BOOT_NORMAL)
            options.boot_phase = gc_boot_initial_phase(runtime.boot_input.drive_state,
                                                       false, options.skip_startup);
        boot_ready = gc_boot_control_init(&boot_config, &boot, options.boot_phase);
    }
    if (boot_ready && !options.skip_startup) {
        menu->startup_controlled = true;
        gc_scene_set_boot(&scene, &boot_config, &boot);
    }
    bool running = true;
    int result = EXIT_SUCCESS;
    uint32_t samples[5];
    for (unsigned sample = 0; sample < 5; sample++)
        samples[sample] = time_base_low();
    gc_face_random_init(samples, &runtime.random);
    uint8_t memory_delays[GC_FACE_MEMORY_POINTS];
    for (unsigned cell = 0; cell < GC_FACE_MEMORY_POINTS; cell++) {
        uint32_t value;
        gc_face_random_next(&runtime.random, time_base_low(), &value);
        memory_delays[cell] = (uint8_t)(value & 31);
    }
    gc_face_geometry_init(&scene.face_geometry, memory_delays,
                          &scene.face_geometry_state);
    if (!boot_ready) {
        fprintf(stderr, "Could not initialize the native startup controller.\n");
        running = false;
        result = EXIT_FAILURE;
    }
    GcAudio *audio = gc_audio_create(options.ipl_path, 48000);
    if (!audio) {
        fprintf(stderr, "Could not decode the local IPL audio bank.\n");
        running = false;
        result = EXIT_FAILURE;
    } else {
        gc_audio_set_mono(audio, menu->settings.sound == GC_SOUND_MONO);
        if (options.skip_startup)
            gc_audio_menu_begin(audio);
    }
    if (!gc_scene_set_cards(&scene, services->cards)) {
        fprintf(stderr, "Could not prepare Memory Card artwork.\n");
        running = false;
        result = EXIT_FAILURE;
    }
    if (!gc_scene_set_disc(&scene, &disc)) {
        fprintf(stderr, "Could not prepare disc artwork.\n");
        running = false;
        result = EXIT_FAILURE;
    }
    bool audio_started = running && !options.record && !options.inspect_frames &&
                         (options.skip_startup || !options.delay_start);
    if (audio_started && !gc_audio_device_start(audio)) {
        fprintf(stderr, "Could not open the audio output device.\n");
        audio_started = false;
        running = false;
        result = EXIT_FAILURE;
    }
    runtime.startup_waiting = !options.skip_startup && options.delay_start;
    gc_input_control_init(&runtime.input);
    if (!gc_card_runtime_init(&runtime.cards, services, menu, &scene)) {
        running = false;
        result = EXIT_FAILURE;
    }
    gc_card_runtime_set_random(&runtime.cards, card_random_sample, &runtime.random);
    gc_scene_set_card_random(&scene, card_random_sample, &runtime.random);
    uint64_t delay_ticks = 0;
    if (!startup_delay_ticks(options.startup_delay_seconds, scene.startup.frame_rate,
                             &delay_ticks)) {
        fprintf(stderr, "The startup delay exceeds the video tick range.\n");
        running = false;
        result = EXIT_FAILURE;
    }
    GcFrameHistory *history =
        options.inspect_frames ? gc_frame_history_create(2048) : NULL;
    if (options.inspect_frames && !history) {
        fprintf(stderr, "Could not allocate frame inspection history.\n");
        running = false;
        result = EXIT_FAILURE;
    }
    AppRuntime app = {.menu = menu,
                      .services = services,
                      .scene = &scene,
                      .audio = audio,
                      .disc = &disc,
                      .boot_config = &boot_config,
                      .boot = &boot,
                      .runtime = &runtime,
                      .history = history,
                      .state_path = options.state_path,
                      .card_revision = services->revision,
                      .inspection = options.inspect_frames,
                      .timed_start = options.timed_start,
                      .startup_delay_ticks = delay_ticks,
                      .audio_started = audio_started};
    application_exit_requested = 0;
    void (*previous_interrupt)(int) = signal(SIGINT, request_application_exit);
    void (*previous_terminate)(int) = signal(SIGTERM, request_application_exit);
    if (previous_interrupt == SIG_ERR || previous_terminate == SIG_ERR) {
        fprintf(stderr, "Could not install application exit handlers.\n");
        running = false;
        result = EXIT_FAILURE;
    }
    if (running && options.record) {
        app.recording = gc_recording_open_with_audio(
            platform, audio, scene.startup.frame_rate, !options.inspect_frames,
            options.record_half, options.record_audio);
        if (!app.recording) {
            fprintf(stderr, "Could not start recording in the Movies folder.\n");
            running = false;
            result = EXIT_FAILURE;
        } else
            fprintf(stderr, "Recording: %s\n", cc_recording_path(app.recording));
    }
    if (running && !application_exit_requested)
        result = run_application(&app, &options);
    double recording_end = NAN;
    bool recording_clock_valid = !app.recording || seconds_now(&recording_end);
    gc_audio_device_stop(audio);
    if (app.recording) {
        if (!cc_recording_close(app.recording, recording_end) ||
            !recording_clock_valid) {
            fprintf(stderr, "Recording could not be finalized completely.\n");
            result = EXIT_FAILURE;
        }
    }
    gc_frame_history_destroy(history);
    destroy_presentations(app.presentations);
    gc_card_runtime_destroy(&runtime.cards);
    gc_scene_destroy(&scene);
    cc_platform_destroy(platform);
    gc_audio_device_stop(audio);
    gc_audio_destroy(audio);
    gc_services_destroy(services);
    gc_disc_destroy(&disc);
    free(services);
    free(menu);
    if (previous_interrupt != SIG_ERR)
        signal(SIGINT, previous_interrupt);
    if (previous_terminate != SIG_ERR)
        signal(SIGTERM, previous_terminate);
    return result;
}
