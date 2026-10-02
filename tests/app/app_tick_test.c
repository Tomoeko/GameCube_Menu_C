/* Exercise the application's video orchestration with the first-party
 * software backend, including actual IPL assets and sequencer state.
 */
#define main gc_application_main
#include "app/main.c"
#undef main
#include "render/software/software.h"
#include "audio/audio_internal.h"

#include <assert.h>

static void tick(AppRuntime *app) {
    assert(advance_video_tick(app));
    ++app->counter;
    draw_video_frame(app);
    assert(save_video_frame(app));
}

static void equal_files(const char *first, const char *second) {
    FILE *a = fopen(first, "rb"), *b = fopen(second, "rb");
    assert(a && b);
    int left, right;
    do {
        left = fgetc(a);
        right = fgetc(b);
        assert(left == right);
    } while (left != EOF);
    assert(!ferror(a) && !ferror(b));
    assert(!fclose(a) && !fclose(b));
}

static void inspect_exact_waits(AppRuntime *app) {
    unsigned rate = app->scene->startup.frame_rate;
    uint64_t count = 0;
    assert(startup_delay_ticks(5, rate, &count) && count == 5u * rate);
    assert(startup_delay_ticks(0, rate, &count) && count == 0);
    assert(startup_delay_ticks(0.5 / rate, rate, &count) && count == 1);
    assert(!startup_delay_ticks(-1, rate, &count));
    assert(!startup_delay_ticks(INFINITY, rate, &count));
    assert(!startup_delay_ticks(ldexp(1.0, 64), rate, &count));
    const double waits[] = {5, 0, 0.5 / rate, 3.0 / rate};
    GcBootControl initial_boot = *app->boot;
    for (size_t index = 0; index < sizeof(waits) / sizeof(waits[0]); ++index) {
        *app->boot = initial_boot;
        app->runtime->startup_waiting = true;
        app->runtime->startup_wait_ticks = 0;
        app->runtime->startup_delay_elapsed = 0;
        app->timed_start = true;
        assert(startup_delay_ticks(waits[index], rate, &app->startup_delay_ticks));
        uint64_t updates = app->startup_delay_ticks ? app->startup_delay_ticks : 1;
        for (uint64_t tick_index = 1; tick_index < updates; ++tick_index) {
            assert(advance_video_tick(app));
            assert(app->runtime->startup_waiting && !app->boot->has_frame);
            assert(app->runtime->startup_wait_ticks == tick_index);
        }
        assert(advance_video_tick(app));
        assert(!app->runtime->startup_waiting && app->boot->has_frame);
        assert(app->runtime->startup_wait_ticks == updates);
    }
    *app->boot = initial_boot;
    app->runtime->startup_waiting = true;
    app->runtime->startup_wait_ticks = 0;
    app->runtime->startup_delay_elapsed = 0;
    app->menu->page_elapsed = app->menu->startup_elapsed = 0;
}

static void inspect_wait_and_boot(AppRuntime *app) {
    GcFrameRuntime *runtime = app->runtime;
    unsigned rate = app->scene->startup.frame_rate;
    runtime->startup_waiting = true;
    app->timed_start = true;
    assert(startup_delay_ticks(3.0 / rate, rate, &app->startup_delay_ticks));
    draw_video_frame(app);
    assert(save_video_frame(app));
    assert(gc_software_write_frame(app->scene->platform, "wait-frame.ppm"));
    tick(app);
    tick(app);
    assert(runtime->startup_waiting && app->boot->video_tick == 0);
    tick(app);
    assert(!runtime->startup_waiting && app->boot->has_frame &&
           app->boot->video_tick == 0);
    assert(gc_software_write_frame(app->scene->platform, "boot-frame.ppm"));
    GcAudioInfo info;
    gc_audio_info(app->audio, &info);
    uint64_t sequence_ticks = info.sequence_ticks;
    assert(gc_frame_history_seek(app->history, -1, &app->counter, app->menu, app->boot,
                                 app->scene, runtime));
    assert(app->counter == 2 && runtime->startup_waiting);
    draw_video_frame(app);
    assert(gc_frame_history_seek(app->history, 1, &app->counter, app->menu, app->boot,
                                 app->scene, runtime));
    draw_video_frame(app);
    assert(app->counter == 3 && app->boot->has_frame && app->boot->video_tick == 0);
    assert(gc_software_write_frame(app->scene->platform, "boot-replayed.ppm"));
    equal_files("boot-frame.ppm", "boot-replayed.ppm");
    gc_audio_info(app->audio, &info);
    assert(info.sequence_ticks == sequence_ticks);
    tick(app);
    assert(gc_frame_history_seek(app->history, -2, &app->counter, app->menu, app->boot,
                                 app->scene, runtime));
    runtime->boot_input.controllers[0].held = GC_BOOT_PAD_A;
    tick(app);
    assert(app->counter == 3);
    assert(!gc_frame_history_seek(app->history, 1, &app->counter, app->menu, app->boot,
                                  app->scene, runtime));
    runtime->boot_input.controllers[0].held = 0;
}

static void inspect_menu_ticks(AppRuntime *app) {
    GcFrameRuntime *runtime = app->runtime;
    runtime->startup_waiting = false;
    gc_menu_skip_startup(app->menu);
    gc_frame_history_reset(app->history);
    draw_video_frame(app);
    assert(save_video_frame(app));
    unsigned rate = app->scene->startup.frame_rate;
    for (unsigned index = 0; index < rate; ++index) {
        uint16_t phase = app->scene->menu_animation.oscillator_phase;
        tick(app);
        if (app->scene->menu_animation.oscillator_phase != (uint16_t)(phase + 7)) {
            fprintf(stderr, "Native step %u at %uHz advanced phase %u to %u.\n",
                    index + 1, rate, phase,
                    app->scene->menu_animation.oscillator_phase);
            assert(false);
        }
    }
    uint16_t phase = app->scene->menu_animation.oscillator_phase;
    uint64_t ui_ticks = app->scene->ui_ticks;
    draw_video_frame(app);
    draw_video_frame(app);
    assert(app->scene->menu_animation.oscillator_phase == phase);
    assert(app->scene->ui_ticks == ui_ticks);
    assert(gc_software_write_frame(app->scene->platform, "menu-frame.ppm"));
    uint64_t saved_counter = app->counter;
    assert(gc_frame_history_seek(app->history, -7, &app->counter, app->menu, app->boot,
                                 app->scene, runtime));
    draw_video_frame(app);
    assert(gc_frame_history_seek(app->history, 7, &app->counter, app->menu, app->boot,
                                 app->scene, runtime));
    draw_video_frame(app);
    assert(app->counter == saved_counter);
    assert(app->scene->menu_animation.oscillator_phase == phase);
    assert(app->scene->ui_ticks == ui_ticks);
    assert(gc_software_write_frame(app->scene->platform, "menu-replayed.ppm"));
    equal_files("menu-frame.ppm", "menu-replayed.ppm");
    gc_menu_tick(app->menu, 0.5 / rate);
    draw_video_frame(app);
    assert(app->scene->menu_animation.oscillator_phase == phase);
    assert(fabs(app->scene->animation_fraction - 0.5) < 1e-9);
    gc_menu_tick(app->menu, 0.5 / rate);
    draw_video_frame(app);
    assert(app->scene->menu_animation.oscillator_phase == (uint16_t)(phase + 7));
}

static void inspect_sound_commit(AppRuntime *app) {
    gc_menu *menu = app->menu;
    menu->page = GC_PAGE_OPTIONS;
    menu->face = GC_FACE_OPTIONS;
    menu->editor_index = 0;
    menu->editing = false;
    menu->settings.sound = GC_SOUND_STEREO;
    menu->settings_changed = menu->clock_changed = false;
    assert(update_resources_and_settings(app));
    assert(!atomic_load(&app->audio->mono));
    gc_menu_press(menu, GC_BUTTON_CONFIRM);
    gc_menu_press(menu, GC_BUTTON_LEFT);
    assert(menu->editing && menu->settings.sound == GC_SOUND_MONO);
    assert(update_resources_and_settings(app));
    assert(!atomic_load(&app->audio->mono));
    gc_menu_press(menu, GC_BUTTON_CANCEL);
    assert(menu->settings.sound == GC_SOUND_STEREO);
    assert(update_resources_and_settings(app));
    assert(!atomic_load(&app->audio->mono));
    gc_menu_press(menu, GC_BUTTON_CONFIRM);
    gc_menu_press(menu, GC_BUTTON_LEFT);
    gc_menu_press(menu, GC_BUTTON_CONFIRM);
    assert(!menu->editing && menu->settings.sound == GC_SOUND_MONO);
    assert(update_resources_and_settings(app));
    assert(atomic_load(&app->audio->mono));
}

static void inspect_calendar_diagonals(AppRuntime *app) {
    gc_menu *menu = app->menu;
    menu->page = GC_PAGE_CUBE;
    menu->page_elapsed = 0;
    draw_video_frame(app);
    gc_menu_press(menu, GC_BUTTON_RIGHT);
    menu->page_elapsed = 2;
    draw_video_frame(app);
    assert(gc_scene_can_press(app->scene, menu, GC_BUTTON_CONFIRM));
    gc_menu_press(menu, GC_BUTTON_CONFIRM);
    menu->page_elapsed = 2;
    draw_video_frame(app);
    assert(gc_scene_can_press(app->scene, menu, GC_BUTTON_CONFIRM));
    const uint16_t vertical[] = {GC_INPUT_UP, GC_INPUT_DOWN};
    const uint16_t horizontal[] = {GC_INPUT_LEFT, GC_INPUT_RIGHT};
    for (unsigned row = 0; row < 2; ++row) {
        for (unsigned y = 0; y < 2; ++y) {
            for (unsigned x = 0; x < 2; ++x) {
                menu->clock = (gc_date_time){2024, 5, 8, 10, 20, 30};
                menu->editor_index = row * 3;
                gc_menu_press(menu, GC_BUTTON_CONFIRM);
                assert(menu->editing);
                GcInputFrame input = {.navigation = vertical[y] | horizontal[x]};
                assert(menu_input(&app->runtime->cards, app->services, menu, app->audio,
                                  app->scene, &input));
                assert(menu->editor_index == row * 3);
                int direction = y ? -1 : 1;
                if (row)
                    assert(menu->clock.hour == 10 + direction);
                else if (menu->region == GC_REGION_USA)
                    assert(menu->clock.month == 5 + direction);
                else if (menu->region == GC_REGION_JAPAN)
                    assert(menu->clock.year == 2024 + direction);
                else
                    assert(menu->clock.day == 8 + direction);
                gc_menu_press(menu, GC_BUTTON_CANCEL);
            }
        }
    }
}

static void expect_button_cue(AppRuntime *app, gc_button button, unsigned expected) {
    unsigned write = atomic_load(&app->audio->event_write);
    press_with_audio(app->services, app->menu, app->audio, button, false);
    assert(atomic_load(&app->audio->event_write) == (write + 1) % GC_AUDIO_EVENT_QUEUE);
    assert(app->audio->pending_events[write].number == expected);
}

static void inspect_card_cues(AppRuntime *app) {
    gc_menu *menu = app->menu;
    gc_card source = {.status = GC_CARD_READY, .capacity_blocks = 59, .file_count = 1};
    source.files[0] =
        (gc_card_file){.blocks = 1, .allow_copy = true, .allow_move = true};
    gc_card target = {.status = GC_CARD_ABSENT};
    assert(gc_menu_set_card(menu, 0, &source));
    assert(gc_menu_set_card(menu, 1, &target));
    menu->card_slot = 0;
    menu->card_index = 0;
    menu->face = GC_FACE_MEMORY_CARD;
    menu->page = GC_PAGE_CARDS;
    expect_button_cue(app, GC_BUTTON_UP, 13);
    unsigned write = atomic_load(&app->audio->event_write);
    press_with_audio(app->services, menu, app->audio, GC_BUTTON_UP, true);
    assert(atomic_load(&app->audio->event_write) == write);
    expect_button_cue(app, GC_BUTTON_RIGHT, 11);
    assert(menu->card_index == 1 && !gc_menu_card_selected(menu));
    menu->card_index = 0;
    for (gc_card_action action = GC_CARD_ACTION_MOVE; action <= GC_CARD_ACTION_COPY;
         ++action) {
        menu->page = GC_PAGE_CARD_ACTION;
        menu->card_action = action;
        expect_button_cue(app, GC_BUTTON_CONFIRM, 13);
        assert(menu->page == GC_PAGE_MESSAGE &&
               menu->message == GC_MESSAGE_CARD_ABSENT);
    }
    menu->page = GC_PAGE_CARD_CONFIRM;
    menu->card_action = GC_CARD_ACTION_ERASE;
    menu->confirm_yes = false;
    expect_button_cue(app, GC_BUTTON_CONFIRM, 7);
    assert(menu->page == GC_PAGE_CARD_ACTION);
    menu->page = GC_PAGE_CARD_CONFIRM;
    expect_button_cue(app, GC_BUTTON_CANCEL, 7);
    assert(menu->page == GC_PAGE_CARD_ACTION);
}

static void inspect_realtime_card_stall(AppRuntime *app) {
    gc_menu *menu = app->menu;
    GcFrameRuntime *runtime = app->runtime;
    GcConfig config;
    gc_config_init(&config);
    config.dummy_data[0] = config.dummy_data[1] = true;
    config.dummy_count[0] = config.dummy_count[1] = 25;
    const char *inputs[2] = {NULL, NULL};
    const char *paths[2] = {app->services->card_paths[0], app->services->card_paths[1]};
    gc_services_destroy(app->services);
    assert(gc_config_prepare_services(&config, app->services, menu, inputs, paths));
    assert(gc_scene_set_cards(app->scene, app->services->cards));
    app->card_revision = app->services->revision;
    GcFrameHistory *history = app->history;
    gc_frame_history_reset(history);
    app->history = NULL;
    app->inspection = false;
    menu->page = GC_PAGE_CARDS;
    menu->face = GC_FACE_MEMORY_CARD;
    menu->page_elapsed = 0;
    menu->editing = false;
    menu->card_slot = 0;
    menu->card_index = menu->card_first_row = 0;
    menu->settings_changed = menu->clock_changed = false;
    runtime->startup_waiting = false;
    gc_date_time clock = {2024, 5, 8, 10, 20, 40};
    assert(gc_menu_set_clock(menu, &clock));
    draw_video_frame(app);
    gc_services_press(app->services, menu, GC_BUTTON_CONFIRM);
    assert(menu->page == GC_PAGE_CARD_ACTION);
    while (menu->card_action != GC_CARD_ACTION_ERASE)
        gc_services_press(app->services, menu, GC_BUTTON_DOWN);
    gc_services_press(app->services, menu, GC_BUTTON_CONFIRM);
    gc_services_press(app->services, menu, GC_BUTTON_UP);
    gc_services_press(app->services, menu, GC_BUTTON_CONFIRM);
    assert(menu->message == GC_MESSAGE_CARD_ERASED &&
           app->services->cards[0].card.file_count == 24);
    gc_services_press(app->services, menu, GC_BUTTON_CONFIRM);
    gc_services_press(app->services, menu, GC_BUTTON_CANCEL);
    gc_services_press(app->services, menu, GC_BUTTON_CANCEL);
    assert(menu->page == GC_PAGE_CUBE);
    draw_video_frame(app);
    GcFrameControl control;
    gc_frame_control_init(&control, false);
    unsigned rate = app->scene->startup.frame_rate;
    uint64_t before = app->counter;
    uint16_t phase = app->scene->menu_animation.oscillator_phase;
    /* An actual save completed above. Supply a deterministic three-second
     * host stall for its durability/artwork work, then resume cheap frames. */
    assert(advance_host_time(app, &control, 3));
    assert(menu->clock.second == 43);
    assert(gc_frame_control_next(&control) == 1);
    tick(app);
    assert(menu->clock.second == 43 && app->counter == before + 1);
    assert(app->scene->menu_animation.oscillator_phase == (uint16_t)(phase + 7));
    assert(!gc_frame_control_next(&control));
    for (unsigned poll = 0; poll < 250; ++poll) {
        assert(advance_host_time(app, &control, 0.001));
        if (gc_frame_control_next(&control))
            tick(app);
        assert(!gc_frame_control_next(&control));
    }
    uint64_t presented = app->counter - before;
    assert(presented >= rate / 4 && presented <= rate / 4 + 2);
    assert(menu->page_elapsed < 0.3);
    assert(menu->clock.second == 43 && fabs(menu->clock_fraction - 0.25) < 1e-8);

    /* Inspection freezes its RTC with the frame timeline, including pause.
     * A blocked startup resumes one update rather than queuing old cues. */
    app->inspection = true;
    gc_frame_control_init(&control, true);
    assert(advance_host_time(app, &control, 7));
    assert(menu->clock.second == 43 && fabs(menu->clock_fraction - 0.25) < 1e-8);
    assert(!gc_frame_control_next(&control));
    app->inspection = false;
    gc_frame_control_init(&control, false);
    menu->page = GC_PAGE_STARTUP;
    menu->page_elapsed = menu->startup_elapsed = 0;
    assert(gc_boot_control_init(app->boot_config, app->boot, GC_BOOT_NORMAL));
    unsigned audio_write = atomic_load(&app->audio->event_write);
    assert(advance_host_time(app, &control, 3));
    assert(gc_frame_control_next(&control) == 1);
    tick(app);
    assert(app->boot->has_frame && app->boot->video_tick == 0);
    assert(atomic_load(&app->audio->event_write) == audio_write);
    assert(!gc_frame_control_next(&control));
    app->inspection = true;
    app->history = history;
    gc_frame_history_reset(history);
}

static void inspect_error_toggle(AppRuntime *app) {
    GcFrameRuntime *runtime = app->runtime;
    const gc_page pages[] = {GC_PAGE_STARTUP, GC_PAGE_DISC, GC_PAGE_GAME_STARTED};
    CcEvent down = {.type = CC_EVENT_KEY_DOWN, .key = (CcKey)'e'};
    CcEvent up = {.type = CC_EVENT_KEY_UP, .key = (CcKey)'e'};
    for (unsigned index = 0; index < sizeof(pages) / sizeof(pages[0]); ++index) {
        app->menu->page = pages[index];
        app->menu->page_elapsed = 0;
        runtime->startup_waiting = index == 0;
        gc_error_control_init(&runtime->error, index == 0);
        gc_frame_history_reset(app->history);
        draw_video_frame(app);
        assert(save_video_frame(app));
        double startup_elapsed = app->menu->startup_elapsed;
        assert(error_test_key(runtime, &down));
        assert(error_test_key(runtime, &down));
        assert(runtime->pending_error_toggle);
        tick(app);
        assert(runtime->error.requested && runtime->error.ticks == 1);
        unsigned duration =
            index == 0 ? GC_ERROR_CONTROL_STARTUP_TICKS : GC_ERROR_CONTROL_MENU_TICKS;
        assert(runtime->error.fade_ticks == duration);
        assert(app->scene->test_error_alpha == 255 / duration);
        assert(!runtime->pending_error_toggle && runtime->error_toggle_held);
        assert(app->menu->page == pages[index] && app->menu->page_elapsed == 0);
        assert(app->menu->startup_elapsed == startup_elapsed);
        uint64_t first_hash = gc_software_frame_hash(app->scene->platform);
        runtime->error.ticks = (uint8_t)(duration - 1);
        tick(app);
        assert(app->scene->test_error_alpha == 255);
        uint64_t full_hash = gc_software_frame_hash(app->scene->platform);
        assert(first_hash != full_hash);
        assert(gc_frame_history_seek(app->history, -1, &app->counter, app->menu,
                                     app->boot, app->scene, runtime));
        draw_video_frame(app);
        assert(runtime->error.ticks == 1 &&
               gc_software_frame_hash(app->scene->platform) == first_hash);
        assert(gc_frame_history_seek(app->history, 1, &app->counter, app->menu,
                                     app->boot, app->scene, runtime));
        draw_video_frame(app);
        assert(gc_software_frame_hash(app->scene->platform) == full_hash);
        assert(error_test_key(runtime, &up));
        assert(error_test_key(runtime, &down));
        tick(app);
        assert(!runtime->error.requested && runtime->error.ticks == duration - 1);
        assert(app->scene->test_error_alpha == (duration - 1) * 255 / duration);
        assert(error_test_key(runtime, &up));
        assert(gc_error_control_advance(&runtime->error, UINT64_MAX));
        draw_video_frame(app);
        assert(app->scene->test_error_alpha == 0);
        runtime->pending_error_toggle = runtime->error_toggle_held = false;
    }
    runtime->startup_waiting = false;
}

static void inspect_disc_toggle(AppRuntime *app) {
    app->menu->page = GC_PAGE_CUBE;
    app->menu->page_elapsed = 0;
    gc_menu_set_disc(app->menu, GC_DISC_ABSENT, NULL, NULL);
    assert(gc_disc_control_init(&app->runtime->disc, GC_DISC_ABSENT));
    gc_frame_history_reset(app->history);
    draw_video_frame(app);
    assert(save_video_frame(app));
    uint32_t banner = app->scene->disc_banner;
    assert(banner && app->disc->simulated);
    CcEvent down = {.type = CC_EVENT_KEY_DOWN, .key = (CcKey)'d'};
    CcEvent up = {.type = CC_EVENT_KEY_UP, .key = (CcKey)'d'};
    assert(disc_test_key(app->runtime, &down));
    assert(disc_test_key(app->runtime, &down));
    assert(app->runtime->pending_disc_toggle);
    tick(app);
    assert(app->menu->disc_status == GC_DISC_LID_OPEN);
    assert(app->runtime->disc.phase == GC_DISC_CONTROL_CLOSING);
    assert(app->runtime->disc.remaining_ticks == GC_DISC_CONTROL_CLOSING_TICKS - 1);
    assert(!app->runtime->pending_disc_toggle && app->runtime->disc_toggle_held);
    uint64_t closing_hash = gc_software_frame_hash(app->scene->platform);
    assert(disc_test_key(app->runtime, &up));
    assert(disc_test_key(app->runtime, &down));
    tick(app);
    assert(app->menu->disc_status == GC_DISC_LID_OPEN);
    uint64_t ejected_hash = gc_software_frame_hash(app->scene->platform);
    assert(app->scene->disc_banner == banner);
    assert(gc_frame_history_seek(app->history, -1, &app->counter, app->menu, app->boot,
                                 app->scene, app->runtime));
    assert(app->menu->disc_status == GC_DISC_LID_OPEN &&
           app->runtime->disc.phase == GC_DISC_CONTROL_CLOSING);
    draw_video_frame(app);
    assert(gc_software_frame_hash(app->scene->platform) == closing_hash);
    assert(gc_frame_history_seek(app->history, 1, &app->counter, app->menu, app->boot,
                                 app->scene, app->runtime));
    assert(app->menu->disc_status == GC_DISC_LID_OPEN);
    draw_video_frame(app);
    assert(gc_software_frame_hash(app->scene->platform) == ejected_hash);
    assert(app->scene->disc_banner == banner);
    assert(disc_test_key(app->runtime, &up));

    /* Controller tests cover every20/120-tick boundary. Shorten only the
     * local I/O fixture here to inspect each real video/render/history phase. */
    assert(disc_test_key(app->runtime, &down));
    tick(app);
    assert(disc_test_key(app->runtime, &up));
    app->runtime->disc.remaining_ticks = 1;
    tick(app);
    assert(app->menu->disc_status == GC_DISC_READING);
    assert(app->runtime->disc.phase == GC_DISC_CONTROL_READING &&
           app->runtime->disc.remaining_ticks == GC_DISC_CONTROL_READING_TICKS);
    tick(app);
    uint64_t reading_hash = gc_software_frame_hash(app->scene->platform);
    app->runtime->disc.remaining_ticks = 1;
    tick(app);
    assert(app->menu->disc_status == GC_DISC_READY &&
           !strcmp(app->menu->disc_title, "Dummy Disc"));
    assert(app->runtime->disc.media_present &&
           app->runtime->disc.phase == GC_DISC_CONTROL_IDLE);
    uint64_t ready_hash = gc_software_frame_hash(app->scene->platform);
    assert(disc_test_key(app->runtime, &down));
    tick(app);
    assert(app->menu->disc_status == GC_DISC_LID_OPEN &&
           !app->runtime->disc.media_present);
    assert(gc_frame_history_seek(app->history, -1, &app->counter, app->menu, app->boot,
                                 app->scene, app->runtime));
    draw_video_frame(app);
    assert(app->menu->disc_status == GC_DISC_READY && app->runtime->disc.media_present);
    assert(gc_software_frame_hash(app->scene->platform) == ready_hash);
    assert(gc_frame_history_seek(app->history, -1, &app->counter, app->menu, app->boot,
                                 app->scene, app->runtime));
    draw_video_frame(app);
    assert(app->menu->disc_status == GC_DISC_READING &&
           app->runtime->disc.remaining_ticks == GC_DISC_CONTROL_READING_TICKS - 1);
    assert(gc_software_frame_hash(app->scene->platform) == reading_hash);
    assert(gc_frame_history_seek(app->history, 2, &app->counter, app->menu, app->boot,
                                 app->scene, app->runtime));
    assert(disc_test_key(app->runtime, &up));
    assert(app->scene->disc_banner == banner);
}

static void inspect_startup_restart(AppRuntime *app) {
    GcAppOptions options = {.inspect_frames = true,
                            .skip_startup = true,
                            .delay_start = true,
                            .timed_start = true,
                            .startup_delay_seconds = 5};
    AppPlayback playback = {.running = true};
    gc_frame_control_init(&playback.frame_control, true);
    gc_menu_set_disc(app->menu, GC_DISC_ABSENT, NULL, NULL);
    assert(gc_disc_control_init(&app->runtime->disc, GC_DISC_ABSENT));
    app->runtime->startup_waiting = true;
    playback.frame_control.pending_steps = -4;
    playback.frame_control.fraction = 0.9;
    uint32_t font = app->scene->font_texture;
    uint32_t banner = app->scene->disc_banner;
    GcMesh *cube = app->scene->menu_cube;
    size_t files[2] = {app->menu->cards[0].file_count, app->menu->cards[1].file_count};
    uint64_t revision = app->services->revision;
    assert(restart_startup(app, &options, &playback));
    assert(app->counter == 0 && app->boot->video_tick == 0 && !app->boot->has_frame);
    assert(app->menu->page == GC_PAGE_STARTUP && !app->runtime->startup_waiting);
    assert(playback.frame_control.paused && playback.frame_control.pending_steps == 0 &&
           playback.frame_control.fraction == 0);
    assert(gc_frame_history_count(app->history) == 1 &&
           gc_frame_history_position(app->history) == 0);
    uint64_t frames[65];
    frames[0] = gc_software_frame_hash(app->scene->platform);
    for (unsigned index = 1; index < 65; ++index) {
        tick(app);
        frames[index] = gc_software_frame_hash(app->scene->platform);
    }

    /* Restart from an editor with stale menu/launch/error presentation state. */
    app->menu->page = GC_PAGE_OPTIONS;
    app->menu->editing = true;
    app->menu->settings_before_edit = app->menu->settings;
    gc_sound sound = app->menu->settings.sound;
    app->menu->settings.sound =
        sound == GC_SOUND_STEREO ? GC_SOUND_MONO : GC_SOUND_STEREO;
    gc_date_time clock = app->menu->clock;
    assert(launch_presentation(app, GC_PAGE_DISC, 3));
    app->runtime->launching = true;
    app->runtime->error.requested = true;
    app->runtime->pending_error_toggle = app->runtime->pending_disc_toggle = true;
    app->runtime->input.held = GC_INPUT_A | GC_INPUT_B;
    app->scene->fatal_error_latched = true;
    app->scene->card_erasing = true;
    app->scene->animation_started = true;
    app->scene->ui_ticks = 12345;
    playback.frame_control.paused = false;
    assert(restart_startup(app, &options, &playback));
    assert(!playback.frame_control.paused && app->counter == 0);
    assert(app->menu->settings.sound == sound &&
           !memcmp(&app->menu->clock, &clock, sizeof(clock)));
    assert(!app->runtime->launching && !app->runtime->launch_menu &&
           !app->presentations);
    assert(!app->runtime->input.held && !app->runtime->error.requested &&
           !app->runtime->pending_error_toggle && !app->runtime->pending_disc_toggle);
    assert(!app->scene->fatal_error_latched && !app->scene->card_erasing &&
           !app->scene->animation_started && app->scene->ui_ticks == 0);
    assert(app->scene->font_texture == font && app->scene->disc_banner == banner &&
           app->scene->menu_cube == cube);
    assert(app->services->revision == revision &&
           app->menu->cards[0].file_count == files[0] &&
           app->menu->cards[1].file_count == files[1]);
    assert(gc_software_frame_hash(app->scene->platform) == frames[0]);
    for (unsigned index = 1; index < 65; ++index) {
        tick(app);
        assert(gc_software_frame_hash(app->scene->platform) == frames[index]);
    }
    /* The local disc fixture survives a restart with its current pending read. */
    assert(gc_disc_control_toggle(&app->runtime->disc));
    gc_disc_control_advance(&app->runtime->disc, 3);
    GcDiscControl disc = app->runtime->disc;
    options.startup_sound = 1;
    assert(restart_startup(app, &options, &playback));
    assert(!memcmp(&app->runtime->disc, &disc, sizeof(disc)));
    for (unsigned port = 0; port < GC_BOOT_CONTROLLER_COUNT; ++port)
        assert(app->runtime->boot_input.controllers[port].valid &&
               app->runtime->boot_input.controllers[port].held == GC_BOOT_PAD_Z);
}

int main(int argc, char **argv) {
    if (argc < 2)
        return EXIT_SUCCESS;
    gc_menu *menu = calloc(1, sizeof(*menu));
    GcServices *services = calloc(1, sizeof(*services));
    GcScene *scene = calloc(1, sizeof(*scene));
    CcPlatform *platform = cc_platform_create("Video-step verification", 640, 480);
    assert(menu && services && scene && platform);
    assert(gc_scene_init(scene, platform, argv[1]));
    gc_region region = argc > 2 && !strcmp(argv[2], "JAP") ? GC_REGION_JAPAN
                       : scene->startup.frame_rate == 50   ? GC_REGION_EUROPE
                                                           : GC_REGION_USA;
    gc_menu_init(menu, region);
    GcConfig config;
    gc_config_init(&config);
    const char *inputs[2] = {NULL, NULL};
    const char *states[2] = {"app-card-a.raw", "app-card-b.raw"};
    assert(gc_config_prepare_services(&config, services, menu, inputs, states));
    assert(gc_scene_set_cards(scene, services->cards));
    GcBootConfig boot_config;
    GcBootControl boot;
    assert(gc_boot_config_init(&boot_config, &scene->startup));
    assert(gc_boot_control_init(&boot_config, &boot, GC_BOOT_NORMAL));
    menu->startup_controlled = true;
    gc_scene_set_boot(scene, &boot_config, &boot);
    GcFrameRuntime runtime = {0};
    gc_error_control_init(&runtime.error, true);
    assert(gc_disc_control_init(&runtime.disc, menu->disc_status));
    runtime.boot_input.drive_state = GC_BOOT_DRIVE_ABSENT;
    runtime.boot_input.controllers[0].valid = true;
    assert(gc_card_runtime_init(&runtime.cards, services, menu, scene));
    uint32_t samples[5] = {1, 2, 3, 4, 5};
    assert(gc_face_random_init(samples, &runtime.random));
    gc_card_runtime_set_random(&runtime.cards, card_random_sample, &runtime.random);
    GcDisc disc = {0};
    assert(gc_disc_create_dummy(region, &disc) == GC_DISC_IMAGE_OK);
    assert(gc_scene_set_disc(scene, &disc));
    GcAudio *audio = gc_audio_create(argv[1], 48000);
    GcFrameHistory *history = gc_frame_history_create(128);
    assert(audio && history);
    AppRuntime app = {.menu = menu,
                      .services = services,
                      .scene = scene,
                      .audio = audio,
                      .disc = &disc,
                      .boot_config = &boot_config,
                      .boot = &boot,
                      .runtime = &runtime,
                      .history = history,
                      .state_path = "app-state.dat",
                      .card_revision = services->revision,
                      .inspection = true};
    inspect_exact_waits(&app);
    inspect_wait_and_boot(&app);
    inspect_menu_ticks(&app);
    inspect_calendar_diagonals(&app);
    inspect_sound_commit(&app);
    inspect_card_cues(&app);
    inspect_disc_toggle(&app);
    inspect_error_toggle(&app);
    inspect_realtime_card_stall(&app);
    inspect_startup_restart(&app);
    gc_frame_history_destroy(history);
    destroy_presentations(app.presentations);
    gc_card_runtime_destroy(&runtime.cards);
    gc_scene_destroy(scene);
    cc_platform_destroy(platform);
    gc_audio_destroy(audio);
    gc_disc_destroy(&disc);
    gc_services_destroy(services);
    assert(!remove(states[0]) && !remove(states[1]));
    free(scene);
    free(services);
    free(menu);
    puts("Native application stepping and replayed captures passed.");
    return EXIT_SUCCESS;
}
