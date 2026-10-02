#include "gamecube/frame_history.h"

#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void frame_state(gc_menu *menu, GcBootControl *boot, GcScene *scene,
                        unsigned tick) {
    menu->page = tick & 1 ? GC_PAGE_CALENDAR : GC_PAGE_FACE;
    menu->page_elapsed = (double)tick / 60;
    menu->editor_index = tick % 3;
    menu->calendar_date_index = tick % 3;
    menu->editing = (tick & 1) != 0;
    menu->card_action = (gc_card_action)(tick % 3);
    menu->card_window_action = menu->card_action;
    menu->card_cursors[1] = (gc_card_cursor){tick * 7, tick};
    boot->video_tick = tick;
    boot->drawing_tick = tick / 2;
    boot->transition_tick = tick / 3;
    boot->waves[0] = tick * 7;
    boot->wave_phase = tick * 11;
    boot->kinetic_phase = tick * 13;
    scene->ui_ticks = tick;
    scene->animation_elapsed = menu->page_elapsed;
    scene->animation_fraction = 0.25;
    scene->animation_page = menu->page;
    scene->menu_animation.oscillator_phase = (uint16_t)(tick * 7);
    scene->menu_animation.focus_angle = (uint16_t)(tick * 31);
    scene->face_geometry_state.gameplay_tick = tick;
    scene->face_geometry_state.memory[12].alpha = (uint8_t)tick;
    scene->edit_state.entrance_counter = (uint16_t)tick;
    scene->edit_state.selection[1500] = (uint16_t)(tick % 21);
    scene->edit_state.next_movement[1500] = (uint16_t)(tick % 17);
    scene->edit_state.disc_launching = tick >= 2;
    scene->edit_state.disc_launch_motion_active = (tick & 1) == 0;
    scene->edit_state.disc_launch_motion = (uint16_t)(tick * 13);
    scene->help_state.labels[7] = (uint8_t)(tick % 21);
    scene->help_state.phase = (uint16_t)(tick * 9);
    scene->card_popups.action_ticks[1] = (uint16_t)(tick * 3);
    scene->card_popups.confirmation_ticks[1] = (uint16_t)(tick * 5);
    scene->card_popups.format_ticks[0][1] = (uint16_t)(tick * 7);
    scene->card_popups.message_ticks[47] = (uint16_t)(tick * 11);
    scene->card_popups.actions[1] = (GcCardPopupRecord){.file_index = tick,
                                                        .first_row = tick / 2,
                                                        .action = GC_CARD_ACTION_COPY,
                                                        .confirm_yes = (tick & 1) != 0,
                                                        .valid = true};
    scene->card_popups.format_yes[1][0] = (tick & 1) != 0;
    scene->card_usage.alpha[0] = (uint8_t)(60 + tick * 4);
    scene->card_usage.level[0] = (uint8_t)(tick * 5);
    scene->card_usage.alpha[1] = (uint8_t)(255 - tick * 4);
    scene->card_usage.level[1] = (uint8_t)(255 - tick * 3);
    scene->card_lighting.focus[0] = (uint16_t)(tick * 3);
    scene->card_lighting.focus[1] = (uint16_t)(tick * 5);
    scene->card_lighting.center = (uint16_t)(tick * 7);
    scene->card_lighting.blink_phase = (uint16_t)(tick * 11);
    scene->card_lighting.active = (tick & 1) != 0;
    scene->card_cells.phase = (uint16_t)(tick * 17);
    scene->card_cells.cells[1][126].counter = (uint16_t)(tick * 3);
    scene->card_cells.cells[1][126].position[2] = (float)tick * 7;
    scene->card_cells.cells[1][126].drawn_position[1] = (float)tick * 11;
    scene->card_cells.cells[1][126].drawn_alpha = (uint8_t)(tick * 13);
    scene->card_cells.first_rows[1] = tick;
    scene->value_morph_state.current.ticks[1] = (uint16_t)(tick * 3);
    scene->value_morph_state.current.offsets[1429] = (int16_t)(tick * 7);
    scene->value_morph_state.drawn.offsets[112] = (int16_t)(tick * 11);
    scene->page_transitions.counters[GC_TRANSITION_CALENDAR][GC_TRANSITION_TEXT] =
        (uint16_t)tick;
    scene->card_selection_ticks[1][13] = (uint8_t)(tick % 7);
    scene->card_arrow_alpha[1][1] = (uint8_t)tick;
    scene->card_erase_angles[7] = (int16_t)tick;
    scene->card_operation_centers[1][3][0] = (float)tick;
    scene->disc_text_ticks[4] = (uint8_t)tick;
    scene->disc_metadata_ticks = (uint8_t)tick;
    scene->disc_face_ticks[0] = (uint8_t)(tick % 21);
    scene->disc_face_ticks[1] = (uint8_t)((tick * 3) % 21);
    scene->disc_face_ticks[2] = (uint8_t)((tick * 7) % 21);
    scene->page_snapshots->saved[GC_TRANSITION_CALENDAR] = true;
    scene->page_snapshots->menus[GC_TRANSITION_CALENDAR].editor_index = tick % 3;
    scene->page_snapshots->editors[GC_TRANSITION_CALENDAR].movement[1600] =
        (uint16_t)tick;
    scene->page_snapshots->morphs[GC_TRANSITION_CALENDAR].drawn.offsets[1429] =
        (int16_t)(tick * 13);
}

static void visual_rewind(void) {
    GcFrameHistory *history = gc_frame_history_create(8);
    gc_menu *menu = calloc(1, sizeof(*menu));
    GcScene *scene = calloc(1, sizeof(*scene));
    GcPageSnapshots *pages = calloc(1, sizeof(*pages));
    GcBootControl boot = {0};
    unsigned resource = 0;
    assert(history && menu && scene && pages);
    gc_menu_init(menu, GC_REGION_USA);
    scene->page_snapshots = pages;
    scene->platform = (CcPlatform *)&resource;
    scene->menu_cube = (GcMesh *)&resource;
    scene->font_texture = 123;
    scene->startup.roll_ticks = 18;
    scene->card_lighting_style.focus_ticks = 17;
    scene->value_morph_style.duration = 29;
    uint64_t counter = 0;
    assert(!gc_frame_history_seek(history, -1, &counter, menu, &boot, scene, NULL));
    for (unsigned tick = 0; tick < 8; ++tick) {
        frame_state(menu, &boot, scene, tick);
        assert(gc_frame_history_save(history, tick, menu, &boot, scene, NULL));
    }
    menu->cards[0].status = GC_CARD_READY;
    menu->cards[0].file_count = 5;
    scene->frame_counter_enabled = true;
    scene->frame_counter = 900;
    scene->inspection_fade_alpha = 255;
    assert(gc_frame_history_seek(history, -5, &counter, menu, &boot, scene, NULL));
    assert(counter == 2 && gc_frame_history_position(history) == 2);
    assert(menu->page == GC_PAGE_FACE && menu->editor_index == 2);
    assert(menu->card_action == GC_CARD_ACTION_ERASE &&
           menu->card_window_action == GC_CARD_ACTION_ERASE);
    assert(menu->card_cursors[1].index == 14 && menu->card_cursors[1].first_row == 2);
    assert(boot.video_tick == 2 && boot.wave_phase == 22 && boot.kinetic_phase == 26);
    assert(scene->ui_ticks == 2 && scene->menu_animation.oscillator_phase == 14);
    assert(scene->face_geometry_state.gameplay_tick == 2);
    assert(scene->face_geometry_state.memory[12].alpha == 2);
    assert(scene->edit_state.selection[1500] == 2);
    assert(scene->edit_state.next_movement[1500] == 2);
    assert(scene->edit_state.disc_launching &&
           scene->edit_state.disc_launch_motion_active);
    assert(scene->edit_state.disc_launch_motion == 26);
    assert(scene->help_state.labels[7] == 2 && scene->help_state.phase == 18);
    assert(scene->card_popups.action_ticks[1] == 6);
    assert(scene->card_popups.confirmation_ticks[1] == 10);
    assert(scene->card_popups.format_ticks[0][1] == 14);
    assert(scene->card_popups.message_ticks[47] == 22);
    assert(scene->card_popups.actions[1].file_index == 2);
    assert(scene->card_popups.actions[1].first_row == 1);
    assert(scene->card_popups.actions[1].action == GC_CARD_ACTION_COPY);
    assert(scene->card_popups.actions[1].valid &&
           !scene->card_popups.actions[1].confirm_yes);
    assert(!scene->card_popups.format_yes[1][0]);
    assert(scene->card_usage.alpha[0] == 68 && scene->card_usage.level[0] == 10);
    assert(scene->card_usage.alpha[1] == 247 && scene->card_usage.level[1] == 249);
    assert(scene->card_lighting.focus[0] == 6 && scene->card_lighting.focus[1] == 10);
    assert(scene->card_lighting.center == 14 && scene->card_lighting.blink_phase == 22);
    assert(!scene->card_lighting.active);
    assert(scene->card_cells.phase == 34);
    assert(scene->card_cells.cells[1][126].counter == 6);
    assert(scene->card_cells.cells[1][126].position[2] == 14);
    assert(scene->card_cells.cells[1][126].drawn_position[1] == 22);
    assert(scene->card_cells.cells[1][126].drawn_alpha == 26);
    assert(scene->card_cells.first_rows[1] == 2);
    assert(scene->value_morph_state.current.ticks[1] == 6);
    assert(scene->value_morph_state.current.offsets[1429] == 14);
    assert(scene->value_morph_state.drawn.offsets[112] == 22);
    assert(
        scene->page_transitions.counters[GC_TRANSITION_CALENDAR][GC_TRANSITION_TEXT] ==
        2);
    assert(scene->card_selection_ticks[1][13] == 2);
    assert(scene->card_arrow_alpha[1][1] == 2 && scene->card_erase_angles[7] == 2);
    assert(scene->card_operation_centers[1][3][0] == 2 &&
           scene->disc_text_ticks[4] == 2);
    assert(scene->disc_face_ticks[0] == 2 && scene->disc_face_ticks[1] == 6 &&
           scene->disc_face_ticks[2] == 14);
    assert(scene->disc_metadata_ticks == 2);
    assert(pages->saved[GC_TRANSITION_CALENDAR]);
    assert(pages->menus[GC_TRANSITION_CALENDAR].editor_index == 2);
    assert(pages->editors[GC_TRANSITION_CALENDAR].movement[1600] == 2);
    assert(pages->morphs[GC_TRANSITION_CALENDAR].drawn.offsets[1429] == 26);
    assert(menu->cards[0].status == GC_CARD_READY && menu->cards[0].file_count == 5);
    assert(scene->platform == (CcPlatform *)&resource &&
           scene->menu_cube == (GcMesh *)&resource);
    assert(scene->font_texture == 123 && scene->startup.roll_ticks == 18);
    assert(scene->card_lighting_style.focus_ticks == 17);
    assert(scene->value_morph_style.duration == 29);
    assert(scene->frame_counter_enabled && scene->frame_counter == 900);
    assert(scene->inspection_fade_alpha == 255);
    assert(gc_frame_history_seek(history, 5, &counter, menu, &boot, scene, NULL));
    assert(counter == 7 && boot.video_tick == 7);
    assert(
        !gc_frame_history_seek(history, INT_MAX, &counter, menu, &boot, scene, NULL));
    assert(
        !gc_frame_history_seek(history, INT_MIN, &counter, menu, &boot, scene, NULL));
    assert(counter == 7 && boot.video_tick == 7);
    assert(gc_frame_history_seek(history, -3, &counter, menu, &boot, scene, NULL));
    frame_state(menu, &boot, scene, 50);
    assert(gc_frame_history_save(history, 50, menu, &boot, scene, NULL));
    assert(gc_frame_history_count(history) == 6);
    assert(!gc_frame_history_seek(history, 1, &counter, menu, &boot, scene, NULL));
    assert(gc_frame_history_seek(history, -1, &counter, menu, &boot, scene, NULL));
    assert(counter == 4 && boot.video_tick == 4);
    assert(gc_frame_history_seek(history, 1, &counter, menu, &boot, scene, NULL));
    assert(counter == 50 && boot.video_tick == 50);
    gc_frame_history_reset(history);
    assert(gc_frame_history_count(history) == 0);
    assert(!gc_frame_history_seek(history, 0, &counter, menu, &boot, scene, NULL));
    gc_frame_history_destroy(history);
    free(pages);
    free(scene);
    free(menu);
}

static void bounded_history(void) {
    GcFrameHistory *history = gc_frame_history_create(3);
    gc_menu *menu = calloc(1, sizeof(*menu));
    GcScene *scene = calloc(1, sizeof(*scene));
    GcBootControl boot = {0};
    assert(history && menu && scene);
    for (unsigned tick = 0; tick < 600; ++tick) {
        boot.video_tick = tick;
        assert(gc_frame_history_save(history, tick, menu, &boot, scene, NULL));
    }
    uint64_t counter = 0;
    assert(gc_frame_history_count(history) == 3);
    assert(gc_frame_history_seek(history, -2, &counter, menu, &boot, scene, NULL));
    assert(counter == 597 && boot.video_tick == 597);
    assert(!gc_frame_history_seek(history, -1, &counter, menu, &boot, scene, NULL));
    assert(gc_frame_history_seek(history, 2, &counter, menu, &boot, scene, NULL));
    assert(counter == 599 && boot.video_tick == 599);
    gc_frame_history_destroy(history);
    free(scene);
    free(menu);
}

static bool retained_random(void *context, uint32_t *value) {
    (void)context;
    (void)value;
    return false;
}

static void runtime_rewind(void) {
    GcFrameHistory *history = gc_frame_history_create(2);
    gc_menu *menu = calloc(1, sizeof(*menu));
    GcScene *scene = calloc(1, sizeof(*scene));
    gc_menu *launch_menu = calloc(1, sizeof(*launch_menu));
    GcBootControl boot = {0};
    GcFrameRuntime runtime = {0};
    unsigned binding = 0;
    assert(history && menu && scene && launch_menu);
    runtime.input.held = GC_INPUT_UP;
    runtime.input.previous_directions = GC_INPUT_UP;
    runtime.input.repeat_ticks = 34;
    runtime.boot_input.controllers[0] = (GcBootPad){true, GC_BOOT_PAD_A};
    runtime.boot_input.controllers[3] = (GcBootPad){true, GC_BOOT_PAD_Z};
    runtime.boot_input.drive_state = GC_BOOT_DRIVE_LID_OPEN;
    runtime.random = (GcFaceRandom){{UINT32_C(0x1245789), UINT32_C(0xabcd1234),
                                     UINT32_C(0x4531a578), UINT32_C(0x81460320)}};
    GcFaceRandom expected_random = runtime.random;
    uint32_t expected_value = 0;
    assert(gc_face_random_next(&expected_random, 0x100, &expected_value));
    runtime.cards.operation.native_state = GC_CARD_OPERATION_NATIVE_ERASE;
    runtime.cards.operation.shrink_remaining = 13;
    runtime.cards.operation.piece_delays[5] = 3;
    runtime.cards.fraction = 0.125;
    runtime.cards.metadata_pending = 2;
    runtime.cards.random_pending = true;
    runtime.cards.active = true;
    runtime.launch.alpha = 132;
    runtime.launch.idle_polls = 7;
    runtime.launching = true;
    runtime.pending_disc_toggle = true;
    runtime.disc_toggle_held = true;
    runtime.pending_error_toggle = true;
    runtime.error_toggle_held = true;
    runtime.error = (GcErrorControl){
        .requested = true, .ticks = 17, .fade_ticks = GC_ERROR_CONTROL_STARTUP_TICKS};
    scene->test_error_alpha = gc_error_control_alpha(&runtime.error);
    assert(gc_disc_control_init(&runtime.disc, GC_DISC_ABSENT));
    assert(gc_disc_control_toggle(&runtime.disc));
    assert(gc_disc_control_advance(&runtime.disc, GC_DISC_CONTROL_CLOSING_TICKS + 7));
    runtime.launch_menu = launch_menu;
    menu->page_elapsed = 0.125;
    launch_menu->page_elapsed = 0.75;
    runtime.boot_fraction = 0.25;
    runtime.input_fraction = 0.5;
    runtime.launch_fraction = 0.75;
    runtime.startup_waiting = true;
    runtime.startup_wait_ticks = 2;
    runtime.startup_delay_elapsed = 0.03125;
    assert(gc_frame_history_save(history, 44, menu, &boot, scene, &runtime));
    menu->page_elapsed = 9;
    launch_menu->page_elapsed = 10;
    runtime = (GcFrameRuntime){0};
    runtime.cards.services = (GcServices *)&binding;
    runtime.cards.menu = menu;
    runtime.cards.scene = scene;
    runtime.cards.random = retained_random;
    runtime.cards.random_context = &binding;
    uint64_t counter = 0;
    assert(gc_frame_history_seek(history, 0, &counter, menu, &boot, scene, &runtime));
    assert(counter == 44 && runtime.input.repeat_ticks == 34);
    assert(runtime.boot_input.controllers[0].held == GC_BOOT_PAD_A);
    assert(runtime.boot_input.controllers[3].valid);
    assert(runtime.boot_input.controllers[3].held == GC_BOOT_PAD_Z);
    assert(runtime.boot_input.drive_state == GC_BOOT_DRIVE_LID_OPEN);
    GcInputFrame input = {0};
    assert(gc_input_control_sample(&runtime.input, &input));
    assert(input.repeated && input.navigation == GC_INPUT_UP);
    uint32_t actual_value = 0;
    assert(gc_face_random_next(&runtime.random, 0x100, &actual_value));
    assert(actual_value == expected_value);
    assert(memcmp(&runtime.random, &expected_random, sizeof(expected_random)) == 0);
    assert(runtime.cards.operation.native_state == GC_CARD_OPERATION_NATIVE_ERASE);
    assert(runtime.cards.operation.shrink_remaining == 13);
    assert(runtime.cards.operation.piece_delays[5] == 3);
    assert(runtime.cards.fraction == 0.125 && runtime.cards.metadata_pending == 2);
    assert(runtime.cards.random_pending && runtime.cards.active);
    assert(runtime.cards.services == (GcServices *)&binding);
    assert(runtime.cards.menu == menu && runtime.cards.scene == scene);
    assert(runtime.cards.random == retained_random &&
           runtime.cards.random_context == &binding);
    assert(runtime.launching && runtime.launch.alpha == 132 &&
           runtime.launch.idle_polls == 7);
    assert(runtime.pending_disc_toggle && runtime.disc_toggle_held);
    assert(runtime.pending_error_toggle && runtime.error_toggle_held);
    assert(runtime.error.requested && runtime.error.ticks == 17);
    assert(scene->test_error_alpha == 144);
    assert(runtime.disc.status == GC_DISC_READING && runtime.disc.media_present);
    assert(runtime.disc.phase == GC_DISC_CONTROL_READING &&
           runtime.disc.remaining_ticks == GC_DISC_CONTROL_READING_TICKS - 7);
    assert(runtime.launch_menu == launch_menu);
    assert(menu->page_elapsed == 0.125);
    assert(runtime.launch_menu->page_elapsed == 0.75);
    assert(runtime.boot_fraction == 0.25 && runtime.input_fraction == 0.5);
    assert(runtime.launch_fraction == 0.75);
    assert(runtime.startup_waiting && runtime.startup_delay_elapsed == 0.03125);
    assert(runtime.startup_wait_ticks == 2);
    gc_frame_history_destroy(history);
    free(scene);
    free(menu);
    free(launch_menu);
}

static void usage_replay(void) {
    GcFrameHistory *history = gc_frame_history_create(5);
    gc_menu *menu = calloc(1, sizeof(*menu));
    GcScene *scene = calloc(1, sizeof(*scene));
    GcBootControl boot = {0};
    assert(history && menu && scene);
    gc_menu_init(menu, GC_REGION_USA);
    menu->cards[0] = (gc_card){.status = GC_CARD_READY,
                               .capacity_blocks = 59,
                               .file_count = 1,
                               .files = {{.blocks = 59}}};
    assert(gc_frame_history_save(history, 0, menu, &boot, scene, NULL));
    for (unsigned tick = 1; tick <= 4; ++tick) {
        assert(gc_card_usage_advance(&scene->card_usage, menu->cards, 1));
        assert(gc_frame_history_save(history, tick, menu, &boot, scene, NULL));
    }
    assert(scene->card_usage.alpha[0] == 72);
    assert(gc_card_usage_fill(&scene->card_usage, 0) == 15);
    GcCardUsage expected = scene->card_usage;
    uint64_t counter = 0;
    assert(gc_frame_history_seek(history, -2, &counter, menu, &boot, scene, NULL));
    assert(counter == 2 && scene->card_usage.alpha[0] == 64);
    assert(gc_card_usage_fill(&scene->card_usage, 0) == 5);
    assert(gc_card_usage_advance(&scene->card_usage, menu->cards, 2));
    assert(memcmp(&scene->card_usage, &expected, sizeof(expected)) == 0);
    gc_frame_history_destroy(history);
    free(scene);
    free(menu);
}

int main(void) {
    assert(!gc_frame_history_create(0));
    assert(!gc_frame_history_create(1));
    assert(!gc_frame_history_create(SIZE_MAX));
    visual_rewind();
    bounded_history();
    runtime_rewind();
    usage_replay();
    puts("Bounded native visual frame history passed.");
    return EXIT_SUCCESS;
}
