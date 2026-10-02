#include "gamecube/menu.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static void open_editor(gc_menu *menu, gc_button direction) {
    gc_menu_skip_startup(menu);
    gc_menu_press(menu, direction);
    assert(menu->page == GC_PAGE_FACE);
    gc_menu_press(menu, GC_BUTTON_CONFIRM);
}

static gc_card example_card(unsigned file_count, uint16_t capacity) {
    gc_card card = {0};
    unsigned index;

    assert(file_count <= GC_CARD_FILE_LIMIT);
    card.status = GC_CARD_READY;
    card.capacity_blocks = capacity;
    card.file_count = file_count;
    for (index = 0; index < file_count; ++index) {
        gc_card_file *file = &card.files[index];
        file->blocks = 2;
        memcpy(file->game_code, "TEST", 4);
        memcpy(file->maker_code, "00", 2);
        snprintf(file->filename, sizeof(file->filename), "test%u", index);
        snprintf(file->title, sizeof(file->title), "Local test file %u", index);
        file->allow_copy = true;
        file->allow_move = true;
    }
    return card;
}

static void acknowledge(gc_menu *menu) {
    assert(menu->page == GC_PAGE_MESSAGE);
    gc_menu_press(menu, GC_BUTTON_CONFIRM);
    assert(menu->page == menu->message_return_page);
}

static void confirm_yes(gc_menu *menu) {
    assert(menu->page == GC_PAGE_CARD_CONFIRM);
    assert(!menu->confirm_yes);
    gc_menu_press(menu, GC_BUTTON_UP);
    gc_menu_press(menu, GC_BUTTON_CONFIRM);
}

static void test_startup_and_navigation(void) {
    gc_menu menu;

    gc_menu_init(&menu, GC_REGION_EUROPE);
    assert(menu.page == GC_PAGE_STARTUP);
    assert(menu.startup_duration == 495.0 / 50.0);
    gc_menu_tick(&menu, 9.89);
    assert(menu.page == GC_PAGE_STARTUP);
    gc_menu_tick(&menu, 0.02);
    assert(menu.page == GC_PAGE_CUBE);
    gc_menu_press(&menu, GC_BUTTON_RIGHT);
    assert(menu.face == GC_FACE_CALENDAR);
    assert(menu.page == GC_PAGE_FACE);
    gc_menu_press(&menu, GC_BUTTON_CANCEL);
    assert(menu.page == GC_PAGE_CUBE);
    gc_menu_press(&menu, GC_BUTTON_LEFT);
    assert(menu.face == GC_FACE_OPTIONS);
    gc_menu_press(&menu, GC_BUTTON_CANCEL);
    gc_menu_press(&menu, GC_BUTTON_DOWN);
    assert(menu.face == GC_FACE_MEMORY_CARD);
    gc_menu_press(&menu, GC_BUTTON_RIGHT);
    assert(menu.face == GC_FACE_MEMORY_CARD && menu.page == GC_PAGE_FACE);
    gc_menu_press(&menu, GC_BUTTON_LEFT);
    assert(menu.face == GC_FACE_MEMORY_CARD && menu.page == GC_PAGE_FACE);
    gc_menu_press(&menu, GC_BUTTON_UP);
    assert(menu.page == GC_PAGE_CUBE);
    gc_menu_press(&menu, GC_BUTTON_CANCEL);
    gc_menu_press(&menu, GC_BUTTON_CONFIRM);
    gc_menu_press(&menu, GC_BUTTON_START);
    assert(menu.page == GC_PAGE_CUBE);
    gc_menu_press(&menu, GC_BUTTON_UP);
    assert(menu.face == GC_FACE_GAME_PLAY);
    gc_menu_press(&menu, GC_BUTTON_START);
    assert(menu.page == GC_PAGE_FACE);
    gc_menu_press(&menu, GC_BUTTON_CONFIRM);
    assert(menu.page == GC_PAGE_DISC);
    gc_menu_press(&menu, GC_BUTTON_START);
    assert(menu.page == GC_PAGE_DISC && !menu.launch_requested);
    gc_menu_press(&menu, GC_BUTTON_CANCEL);
    assert(menu.page == GC_PAGE_FACE);
}

static void test_directional_face_navigation(void) {
    const gc_button directions[4] = {GC_BUTTON_UP, GC_BUTTON_RIGHT, GC_BUTTON_DOWN,
                                     GC_BUTTON_LEFT};
    const gc_page pages[4] = {GC_PAGE_DISC, GC_PAGE_CALENDAR, GC_PAGE_CARDS,
                              GC_PAGE_OPTIONS};
    gc_menu menu;
    for (unsigned face = 0; face < 4; ++face) {
        gc_menu_init(&menu, GC_REGION_EUROPE);
        gc_menu_skip_startup(&menu);
        gc_menu_press(&menu, directions[face]);
        assert(menu.page == GC_PAGE_FACE && (unsigned)menu.face == face);
        menu.page_elapsed = 4;
        gc_menu_press(&menu, directions[(face + 2) % 4]);
        assert(menu.page == GC_PAGE_CUBE && menu.page_elapsed == 0);
        gc_menu_press(&menu, directions[face]);
        gc_menu_press(&menu, directions[face]);
        assert(menu.page == pages[face] && menu.page_elapsed == 0);
        gc_menu_press(&menu, GC_BUTTON_CANCEL);
        assert(menu.page == GC_PAGE_FACE && (unsigned)menu.face == face);
        menu.page_elapsed = 4;
        gc_menu_press(&menu, directions[(face + 1) % 4]);
        assert(menu.page == GC_PAGE_FACE && (unsigned)menu.face == face &&
               menu.page_elapsed == 4);
        gc_menu_press(&menu, directions[(face + 3) % 4]);
        assert(menu.page == GC_PAGE_FACE && (unsigned)menu.face == face);
        gc_menu_press(&menu, GC_BUTTON_CONFIRM);
        assert(menu.page == pages[face]);
    }
}

static void test_startup_native_duration_and_override(void) {
    gc_menu menu;

    for (unsigned region = GC_REGION_JAPAN; region <= GC_REGION_USA; ++region) {
        gc_menu_init(&menu, (gc_region)region);
        assert(menu.startup_duration == 537.0 / 60.0);
        gc_menu_tick(&menu, 536.0 / 60.0);
        assert(menu.page == GC_PAGE_STARTUP);
        gc_menu_tick(&menu, 1.01 / 60.0);
        assert(menu.page == GC_PAGE_CUBE);
        assert(menu.startup_elapsed == 537.0 / 60.0);
    }
    gc_menu_init(&menu, GC_REGION_EUROPE);
    menu.startup_duration = 10.5;
    gc_menu_tick(&menu, 10.49);
    assert(menu.page == GC_PAGE_STARTUP);
    gc_menu_tick(&menu, 0.02);
    assert(menu.page == GC_PAGE_CUBE && menu.startup_elapsed == 10.5);
    gc_menu_init(&menu, GC_REGION_EUROPE);
    menu.startup_duration = NAN;
    gc_menu_skip_startup(&menu);
    assert(menu.startup_elapsed == 495.0 / 50.0);
}

static void test_clock_rollover(void) {
    gc_menu menu;
    gc_date_time clock = {2000, 2, 28, 23, 59, 59};

    gc_menu_init(&menu, GC_REGION_EUROPE);
    assert(gc_menu_set_clock(&menu, &clock));
    gc_menu_tick(&menu, 1.0);
    assert(menu.clock.day == 29 && menu.clock.month == 2);
    gc_menu_tick(&menu, 86400.0);
    assert(menu.clock.day == 1 && menu.clock.month == 3);
    clock = (gc_date_time){2001, 2, 29, 0, 0, 0};
    assert(!gc_menu_set_clock(&menu, &clock));
    clock = (gc_date_time){2099, 12, 31, 23, 59, 59};
    assert(gc_menu_set_clock(&menu, &clock));
    gc_menu_tick(&menu, 1.0);
    assert(menu.clock.year == 2000 && menu.clock.month == 1 && menu.clock.day == 1);
    assert(gc_date_time_weekday(&menu.clock) == 6);
    gc_menu_tick(&menu, 0.25);
    gc_menu_tick(&menu, 0.25);
    gc_menu_tick(&menu, 0.5);
    assert(menu.clock.second == 1);
    gc_menu_tick(&menu, NAN);
    gc_menu_tick(&menu, INFINITY);
    gc_menu_tick(&menu, -1.0);
    assert(menu.clock.second == 1);
    gc_menu_tick(&menu, 3155760000.0);
    assert(menu.clock.second == 1 && menu.clock.year == 2000);
}

static void test_independent_clock_and_presentation(void) {
    gc_menu menu;
    gc_menu_init(&menu, GC_REGION_USA);
    gc_menu_skip_startup(&menu);
    gc_date_time initial = {2024, 2, 29, 23, 59, 59};
    assert(gc_menu_set_clock(&menu, &initial));
    menu.clock_fraction = 0.75;
    gc_menu_tick_presentation(&menu, 2);
    assert(menu.page_elapsed == 2);
    assert(menu.clock.year == 2024 && menu.clock.month == 2 && menu.clock.day == 29 &&
           menu.clock.hour == 23 && menu.clock.minute == 59 && menu.clock.second == 59);
    assert(menu.clock_fraction == 0.75);
    gc_menu_advance_clock(&menu, 0.5);
    assert(menu.clock.year == 2024 && menu.clock.month == 3 && menu.clock.day == 1 &&
           menu.clock.hour == 0 && menu.clock.minute == 0 && menu.clock.second == 0);
    assert(menu.clock_fraction == 0.25 && menu.page_elapsed == 2);
    gc_menu_tick(&menu, 0.75);
    assert(menu.clock.second == 1 && menu.clock_fraction == 0 &&
           menu.page_elapsed == 2.75);
}

static void test_calendar_commit_cancel_and_regions(void) {
    gc_menu menu;
    gc_date_time clock = {2024, 1, 31, 12, 34, 56};

    gc_menu_init(&menu, GC_REGION_EUROPE);
    assert(gc_menu_set_clock(&menu, &clock));
    open_editor(&menu, GC_BUTTON_RIGHT);
    assert(menu.page == GC_PAGE_CALENDAR);
    assert(gc_menu_calendar_field(&menu) == GC_CALENDAR_YEAR);
    gc_menu_press(&menu, GC_BUTTON_LEFT);
    assert(gc_menu_calendar_field(&menu) == GC_CALENDAR_YEAR);
    gc_menu_press(&menu, GC_BUTTON_CONFIRM);
    gc_menu_press(&menu, GC_BUTTON_LEFT);
    assert(gc_menu_calendar_field(&menu) == GC_CALENDAR_MONTH);
    gc_menu_press(&menu, GC_BUTTON_UP);
    assert(menu.clock.month == 2 && menu.clock.day == 29);
    gc_menu_tick(&menu, 10.0);
    assert(menu.clock.minute == 35 && menu.clock.second == 6);
    gc_menu_press(&menu, GC_BUTTON_CANCEL);
    assert(menu.clock.month == 1 && menu.clock.day == 31);
    assert(menu.clock.minute == 35 && menu.clock.second == 6);
    assert(!menu.clock_changed);
    gc_menu_press(&menu, GC_BUTTON_CONFIRM);
    gc_menu_press(&menu, GC_BUTTON_UP);
    gc_menu_press(&menu, GC_BUTTON_CONFIRM);
    assert(menu.clock.month == 2 && menu.clock.day == 29);
    assert(menu.clock_changed);
    gc_menu_press(&menu, GC_BUTTON_CONFIRM);
    gc_menu_press(&menu, GC_BUTTON_RIGHT);
    assert(gc_menu_calendar_field(&menu) == GC_CALENDAR_YEAR);
    gc_menu_press(&menu, GC_BUTTON_UP);
    gc_menu_press(&menu, GC_BUTTON_CONFIRM);
    assert(menu.clock.year == 2025 && menu.clock.day == 28);
    gc_menu_press(&menu, GC_BUTTON_DOWN);
    assert(gc_menu_calendar_field(&menu) == GC_CALENDAR_HOUR);
    gc_menu_press(&menu, GC_BUTTON_CONFIRM);
    gc_menu_press(&menu, GC_BUTTON_LEFT);
    assert(gc_menu_calendar_field(&menu) == GC_CALENDAR_SECOND);
    gc_menu_press(&menu, GC_BUTTON_CANCEL);
    gc_menu_press(&menu, GC_BUTTON_UP);
    assert(gc_menu_calendar_field(&menu) == GC_CALENDAR_YEAR);
    gc_menu_press(&menu, GC_BUTTON_DOWN);
    assert(gc_menu_calendar_field(&menu) == GC_CALENDAR_SECOND);
    gc_menu_press(&menu, GC_BUTTON_CANCEL);
    assert(menu.page == GC_PAGE_FACE);
    gc_menu_init(&menu, GC_REGION_USA);
    assert(gc_menu_calendar_field(&menu) == GC_CALENDAR_MONTH);
    gc_menu_init(&menu, GC_REGION_JAPAN);
    assert(gc_menu_calendar_field(&menu) == GC_CALENDAR_YEAR);
}

static void test_calendar_live_clock_and_tuple_commits(void) {
    for (unsigned region = GC_REGION_JAPAN; region <= GC_REGION_USA; ++region) {
        gc_menu menu;
        gc_date_time clock = {2024, 4, 30, 23, 59, 59};
        gc_menu_init(&menu, (gc_region)region);
        assert(gc_menu_set_clock(&menu, &clock));
        menu.clock_fraction = 0.25;
        open_editor(&menu, GC_BUTTON_RIGHT);
        gc_menu_press(&menu, GC_BUTTON_CONFIRM);
        assert(gc_menu_calendar_field(&menu) == GC_CALENDAR_YEAR);
        gc_menu_press(&menu, GC_BUTTON_UP);
        gc_menu_tick(&menu, 2.5);
        assert(menu.clock.year == 2025 && menu.clock.month == 4 &&
               menu.clock.day == 30 && menu.clock.second == 1);
        gc_menu_press(&menu, GC_BUTTON_CANCEL);
        assert(menu.clock.year == 2024 && menu.clock.month == 5 && menu.clock.day == 1);
        assert(menu.clock.hour == 0 && menu.clock.minute == 0 &&
               menu.clock.second == 1);
        assert(menu.clock_fraction == 0.75 && !menu.clock_changed);

        assert(gc_menu_set_clock(&menu, &clock));
        menu.clock_fraction = 0.25;
        gc_menu_press(&menu, GC_BUTTON_CONFIRM);
        gc_menu_press(&menu, GC_BUTTON_UP);
        gc_menu_tick(&menu, 1.5);
        gc_menu_press(&menu, GC_BUTTON_CONFIRM);
        assert(menu.clock.year == 2025 && menu.clock.month == 4 &&
               menu.clock.day == 30);
        assert(menu.clock.hour == 0 && menu.clock.minute == 0 &&
               menu.clock.second == 0);
        assert(menu.clock_fraction == 0.75 && menu.clock_changed);

        clock = (gc_date_time){2024, 12, 31, 23, 59, 59};
        assert(gc_menu_set_clock(&menu, &clock));
        menu.clock_fraction = 0.5;
        gc_menu_press(&menu, GC_BUTTON_DOWN);
        assert(gc_menu_calendar_field(&menu) == GC_CALENDAR_HOUR);
        gc_menu_press(&menu, GC_BUTTON_CONFIRM);
        assert(menu.clock.hour == 23 && menu.clock.minute == 59 &&
               menu.clock.second == 0);
        gc_menu_press(&menu, GC_BUTTON_UP);
        gc_menu_tick(&menu, 2);
        gc_menu_press(&menu, GC_BUTTON_CONFIRM);
        assert(menu.clock.year == 2025 && menu.clock.month == 1 && menu.clock.day == 1);
        assert(menu.clock.hour == 0 && menu.clock.minute == 59 &&
               menu.clock.second == 0);
        assert(menu.clock_fraction == 0.5);

        gc_menu_press(&menu, GC_BUTTON_CONFIRM);
        gc_menu_press(&menu, GC_BUTTON_UP);
        gc_menu_tick(&menu, 3.25);
        gc_menu_press(&menu, GC_BUTTON_CANCEL);
        assert(menu.clock.hour == 0 && menu.clock.minute == 59 &&
               menu.clock.second == 3);
        assert(menu.clock_fraction == 0.75);
    }
}

static void test_settings_commit_cancel_bounds(void) {
    gc_menu menu;
    unsigned index;

    gc_menu_init(&menu, GC_REGION_EUROPE);
    open_editor(&menu, GC_BUTTON_LEFT);
    assert(menu.page == GC_PAGE_OPTIONS);
    gc_menu_press(&menu, GC_BUTTON_CONFIRM);
    gc_menu_press(&menu, GC_BUTTON_LEFT);
    assert(menu.settings.sound == GC_SOUND_MONO);
    gc_menu_press(&menu, GC_BUTTON_CANCEL);
    assert(menu.settings.sound == GC_SOUND_STEREO && !menu.settings_changed);
    gc_menu_press(&menu, GC_BUTTON_CONFIRM);
    gc_menu_press(&menu, GC_BUTTON_LEFT);
    gc_menu_press(&menu, GC_BUTTON_CONFIRM);
    assert(menu.settings.sound == GC_SOUND_MONO && menu.settings_changed);
    gc_menu_press(&menu, GC_BUTTON_DOWN);
    gc_menu_press(&menu, GC_BUTTON_CONFIRM);
    for (index = 0; index < 100; ++index)
        gc_menu_press(&menu, GC_BUTTON_RIGHT);
    assert(menu.settings.screen_position == GC_SCREEN_POSITION_MAX);
    for (index = 0; index < 100; ++index)
        gc_menu_press(&menu, GC_BUTTON_LEFT);
    assert(menu.settings.screen_position == GC_SCREEN_POSITION_MIN);
    gc_menu_press(&menu, GC_BUTTON_CANCEL);
    assert(menu.settings.screen_position == 0);
    gc_menu_press(&menu, GC_BUTTON_DOWN);
    gc_menu_press(&menu, GC_BUTTON_CONFIRM);
    for (index = 0; index < 10; ++index)
        gc_menu_press(&menu, GC_BUTTON_RIGHT);
    assert(menu.settings.language == GC_LANGUAGE_DUTCH);
    gc_menu_press(&menu, GC_BUTTON_CONFIRM);
    gc_menu_press(&menu, GC_BUTTON_CANCEL);
    assert(menu.page == GC_PAGE_FACE);
    gc_menu_init(&menu, GC_REGION_USA);
    open_editor(&menu, GC_BUTTON_LEFT);
    gc_menu_press(&menu, GC_BUTTON_DOWN);
    gc_menu_press(&menu, GC_BUTTON_DOWN);
    assert(menu.editor_index == 1);
}

static void test_card_browser_and_erase(void) {
    gc_menu menu;
    gc_card card = example_card(25, 123);
    unsigned index;

    gc_menu_init(&menu, GC_REGION_EUROPE);
    assert(gc_menu_set_card(&menu, 0, &card));
    assert(gc_card_free_blocks(&menu.cards[0]) == 73);
    open_editor(&menu, GC_BUTTON_DOWN);
    assert(menu.page == GC_PAGE_CARDS);
    for (index = 0; index < 4; ++index)
        gc_menu_press(&menu, GC_BUTTON_DOWN);
    assert(menu.card_index == 16 && menu.card_first_row == 1);
    gc_menu_press(&menu, GC_BUTTON_RIGHT);
    assert(menu.card_index == 17);
    gc_menu_press(&menu, GC_BUTTON_CONFIRM);
    assert(menu.page == GC_PAGE_CARD_ACTION);
    assert(menu.card_action == GC_CARD_ACTION_ERASE);
    gc_menu_press(&menu, GC_BUTTON_UP);
    assert(menu.card_action == GC_CARD_ACTION_COPY);
    gc_menu_press(&menu, GC_BUTTON_DOWN);
    gc_menu_press(&menu, GC_BUTTON_CONFIRM);
    assert(menu.page == GC_PAGE_CARD_CONFIRM && !menu.confirm_yes);
    gc_menu_press(&menu, GC_BUTTON_CONFIRM);
    assert(menu.page == GC_PAGE_CARD_ACTION && menu.cards[0].file_count == 25);
    gc_menu_press(&menu, GC_BUTTON_CONFIRM);
    confirm_yes(&menu);
    assert(menu.message == GC_MESSAGE_CARD_ERASED);
    assert(menu.cards[0].file_count == 24 && menu.cards_changed[0]);
    assert(strcmp(menu.cards[0].files[17].filename, "test18") == 0);
    assert(gc_card_free_blocks(&menu.cards[0]) == 75);
    acknowledge(&menu);
}

static void test_card_copy_move_and_duplicates(void) {
    gc_menu menu;
    gc_card source = example_card(2, 59);
    gc_card target = example_card(0, 59);

    gc_menu_init(&menu, GC_REGION_EUROPE);
    assert(gc_menu_set_card(&menu, 0, &source));
    assert(gc_menu_set_card(&menu, 1, &target));
    open_editor(&menu, GC_BUTTON_DOWN);
    gc_menu_press(&menu, GC_BUTTON_CONFIRM);
    assert(menu.card_action == GC_CARD_ACTION_ERASE);
    gc_menu_press(&menu, GC_BUTTON_UP);
    assert(menu.card_action == GC_CARD_ACTION_COPY);
    gc_menu_press(&menu, GC_BUTTON_CONFIRM);
    confirm_yes(&menu);
    assert(menu.message == GC_MESSAGE_CARD_COPIED);
    assert(menu.cards[0].file_count == 2 && menu.cards[1].file_count == 1);
    assert(menu.cards_changed[1] && !menu.cards_changed[0]);
    acknowledge(&menu);
    gc_menu_press(&menu, GC_BUTTON_CONFIRM);
    gc_menu_press(&menu, GC_BUTTON_CONFIRM);
    assert(menu.message == GC_MESSAGE_CARD_FILE_EXISTS);
    acknowledge(&menu);
    gc_menu_press(&menu, GC_BUTTON_RIGHT);
    gc_menu_press(&menu, GC_BUTTON_CONFIRM);
    gc_menu_press(&menu, GC_BUTTON_UP);
    assert(menu.card_action == GC_CARD_ACTION_MOVE);
    gc_menu_press(&menu, GC_BUTTON_CONFIRM);
    confirm_yes(&menu);
    assert(menu.message == GC_MESSAGE_CARD_MOVED);
    assert(menu.cards[0].file_count == 1 && menu.cards[1].file_count == 2);
    assert(menu.cards_changed[0]);
    acknowledge(&menu);
}

static void test_card_errors_and_insertion_during_confirmation(void) {
    gc_menu menu;
    gc_card source = example_card(1, 59);
    gc_card target = example_card(0, 1);

    gc_menu_init(&menu, GC_REGION_EUROPE);
    assert(gc_menu_set_card(&menu, 0, &source));
    assert(gc_menu_set_card(&menu, 1, &target));
    open_editor(&menu, GC_BUTTON_DOWN);
    gc_menu_press(&menu, GC_BUTTON_CONFIRM);
    gc_menu_press(&menu, GC_BUTTON_DOWN);
    gc_menu_press(&menu, GC_BUTTON_CONFIRM);
    assert(menu.message == GC_MESSAGE_CARD_NO_SPACE);
    acknowledge(&menu);
    target = example_card(0, 59);
    assert(gc_menu_set_card(&menu, 1, &target));
    gc_menu_press(&menu, GC_BUTTON_CONFIRM);
    gc_menu_press(&menu, GC_BUTTON_CONFIRM);
    assert(menu.page == GC_PAGE_CARD_CONFIRM);
    target = (gc_card){0};
    assert(gc_menu_set_card(&menu, 1, &target));
    assert(menu.page == GC_PAGE_CARDS);
    assert(menu.cards[0].file_count == 1);
    source.files[0].allow_move = false;
    source.files[0].allow_copy = false;
    assert(gc_menu_set_card(&menu, 0, &source));
    target = example_card(0, 59);
    assert(gc_menu_set_card(&menu, 1, &target));
    gc_menu_press(&menu, GC_BUTTON_CONFIRM);
    assert(menu.card_action == GC_CARD_ACTION_MOVE);
    gc_menu_press(&menu, GC_BUTTON_CONFIRM);
    assert(menu.page == GC_PAGE_MESSAGE &&
           menu.message == GC_MESSAGE_CARD_MOVE_FORBIDDEN);
    source.file_count = GC_CARD_FILE_LIMIT + 1;
    assert(!gc_menu_set_card(&menu, 0, &source));
    source = example_card(1, 1);
    assert(!gc_menu_set_card(&menu, 0, &source));
    assert(!gc_menu_set_card(&menu, 2, &target));
}

static void test_card_format_two_confirmations(void) {
    gc_menu menu;
    gc_card card = {0};

    gc_menu_init(&menu, GC_REGION_EUROPE);
    card.status = GC_CARD_UNFORMATTED;
    card.capacity_blocks = 59;
    assert(gc_menu_set_card(&menu, 0, &card));
    open_editor(&menu, GC_BUTTON_DOWN);
    gc_menu_press(&menu, GC_BUTTON_CONFIRM);
    assert(menu.card_action == GC_CARD_ACTION_FORMAT);
    confirm_yes(&menu);
    assert(menu.page == GC_PAGE_CARD_CONFIRM);
    assert(menu.format_second_confirmation && !menu.confirm_yes);
    assert(menu.cards[0].status == GC_CARD_UNFORMATTED);
    confirm_yes(&menu);
    assert(menu.message == GC_MESSAGE_CARD_FORMATTED);
    assert(menu.cards[0].status == GC_CARD_READY && menu.cards_changed[0]);
    assert(gc_card_free_blocks(&menu.cards[0]) == 59);
    acknowledge(&menu);
    assert(!gc_menu_card_selected(&menu));
}

static void test_card_action_navigation_and_decline(void) {
    for (unsigned region = GC_REGION_JAPAN; region <= GC_REGION_EUROPE; ++region) {
        gc_menu menu;
        gc_card source = example_card(1, 59);
        gc_card target = example_card(0, 59);
        gc_menu_init(&menu, (gc_region)region);
        assert(menu.card_window_action == GC_CARD_ACTION_ERASE);
        assert(gc_menu_set_card(&menu, 0, &source));
        open_editor(&menu, GC_BUTTON_DOWN);
        gc_menu_press(&menu, GC_BUTTON_CONFIRM);
        assert(menu.card_action == GC_CARD_ACTION_ERASE);
        gc_menu_press(&menu, GC_BUTTON_DOWN);
        assert(menu.card_action == GC_CARD_ACTION_MOVE);
        gc_menu_press(&menu, GC_BUTTON_DOWN);
        assert(menu.card_action == GC_CARD_ACTION_COPY);
        gc_menu_press(&menu, GC_BUTTON_DOWN);
        assert(menu.card_action == GC_CARD_ACTION_ERASE);
        gc_menu_press(&menu, GC_BUTTON_UP);
        assert(menu.card_action == GC_CARD_ACTION_COPY);
        gc_menu_press(&menu, GC_BUTTON_CONFIRM);
        assert(menu.page == GC_PAGE_MESSAGE && menu.message == GC_MESSAGE_CARD_ABSENT);
        acknowledge(&menu);
        gc_menu_press(&menu, GC_BUTTON_CONFIRM);
        assert(menu.card_action == GC_CARD_ACTION_COPY);
        gc_menu_press(&menu, GC_BUTTON_UP);
        assert(menu.card_action == GC_CARD_ACTION_MOVE);
        gc_menu_press(&menu, GC_BUTTON_CANCEL);
        assert(gc_menu_set_card(&menu, 1, &target));
        gc_menu_press(&menu, GC_BUTTON_CONFIRM);
        assert(menu.card_action == GC_CARD_ACTION_MOVE);
        gc_menu_press(&menu, GC_BUTTON_CONFIRM);
        assert(menu.page == GC_PAGE_CARD_CONFIRM && !menu.confirm_yes);
        gc_menu_press(&menu, GC_BUTTON_LEFT);
        gc_menu_press(&menu, GC_BUTTON_RIGHT);
        assert(!menu.confirm_yes);
        gc_menu_press(&menu, GC_BUTTON_UP);
        assert(menu.confirm_yes);
        gc_menu_press(&menu, GC_BUTTON_RIGHT);
        assert(menu.confirm_yes);
        gc_menu_press(&menu, GC_BUTTON_DOWN);
        gc_menu_press(&menu, GC_BUTTON_CONFIRM);
        assert(menu.page == GC_PAGE_CARD_ACTION);
        assert(menu.cards[0].file_count == 1 && menu.cards[1].file_count == 0);
        assert(!menu.cards_changed[0] && !menu.cards_changed[1]);
        gc_menu_press(&menu, GC_BUTTON_CANCEL);
        target = (gc_card){.status = GC_CARD_UNFORMATTED, .capacity_blocks = 59};
        assert(gc_menu_set_card(&menu, 1, &target));
        menu.card_slot = 1;
        gc_menu_press(&menu, GC_BUTTON_CONFIRM);
        assert(menu.card_action == GC_CARD_ACTION_FORMAT);
        gc_menu_press(&menu, GC_BUTTON_LEFT);
        assert(!menu.confirm_yes);
        gc_menu_press(&menu, GC_BUTTON_CONFIRM);
        assert(menu.page == GC_PAGE_CARDS);
        menu.card_slot = 0;
        gc_menu_press(&menu, GC_BUTTON_CONFIRM);
        assert(menu.card_action == GC_CARD_ACTION_MOVE);
        assert(menu.card_window_action == GC_CARD_ACTION_MOVE);
    }
}

static void press_many(gc_menu *menu, gc_button button, unsigned count) {
    for (unsigned index = 0; index < count; ++index)
        gc_menu_press(menu, button);
}

static void test_empty_card_cells_and_slot_edges(void) {
    for (unsigned region = GC_REGION_JAPAN; region <= GC_REGION_EUROPE; ++region) {
        gc_menu menu;
        gc_card first = example_card(0, 59);
        gc_card second = example_card(0, 1019);
        gc_menu_init(&menu, (gc_region)region);
        assert(gc_menu_set_card(&menu, 0, &first));
        assert(gc_menu_set_card(&menu, 1, &second));
        open_editor(&menu, GC_BUTTON_DOWN);
        gc_menu_press(&menu, GC_BUTTON_LEFT);
        gc_menu_press(&menu, GC_BUTTON_UP);
        assert(menu.card_slot == 0 && menu.card_index == 0);
        press_many(&menu, GC_BUTTON_RIGHT, 3);
        assert(menu.card_index == 3);
        gc_menu_press(&menu, GC_BUTTON_RIGHT);
        assert(menu.card_slot == 1 && menu.card_index == 0);
        assert(gc_menu_card_cursor_index(&menu, 0) == 3);
        press_many(&menu, GC_BUTTON_DOWN, 8);
        press_many(&menu, GC_BUTTON_RIGHT, 8);
        assert(menu.card_slot == 1 && menu.card_index == 15);
        assert(menu.card_first_row == 0 && !gc_menu_card_selected(&menu));
        gc_menu_press(&menu, GC_BUTTON_CONFIRM);
        assert(menu.page == GC_PAGE_CARDS);
        press_many(&menu, GC_BUTTON_LEFT, 4);
        assert(menu.card_slot == 0 && menu.card_index == 15);
        assert(gc_menu_card_cursor_index(&menu, 1) == 12);
        press_many(&menu, GC_BUTTON_LEFT, 8);
        press_many(&menu, GC_BUTTON_UP, 8);
        assert(menu.card_slot == 0 && menu.card_index == 0);
        for (unsigned status = GC_CARD_ABSENT; status <= GC_CARD_DAMAGED; ++status) {
            if (status == GC_CARD_READY)
                continue;
            second = (gc_card){.status = (gc_card_status)status, .capacity_blocks = 59};
            assert(gc_menu_set_card(&menu, 1, &second));
            menu.card_index = 3;
            gc_menu_press(&menu, GC_BUTTON_RIGHT);
            assert(menu.card_slot == 0 && menu.card_index == 3);
        }
        first = example_card(1, 59);
        assert(gc_menu_set_card(&menu, 0, &first));
        press_many(&menu, GC_BUTTON_DOWN, 4);
        assert(menu.card_index == 15 && !gc_menu_card_selected(&menu));
        gc_menu_press(&menu, GC_BUTTON_CONFIRM);
        assert(menu.page == GC_PAGE_CARDS);
    }
}

static void test_card_scroll_retains_each_slot(void) {
    for (unsigned region = GC_REGION_JAPAN; region <= GC_REGION_EUROPE; ++region) {
        gc_menu menu;
        gc_card first = example_card(25, 59);
        gc_card second = example_card(100, 1019);
        gc_menu_init(&menu, (gc_region)region);
        assert(gc_menu_set_card(&menu, 0, &first));
        assert(gc_menu_set_card(&menu, 1, &second));
        open_editor(&menu, GC_BUTTON_DOWN);
        press_many(&menu, GC_BUTTON_DOWN, 10);
        assert(menu.card_index == 24 && menu.card_first_row == 3);
        press_many(&menu, GC_BUTTON_RIGHT, 3);
        assert(menu.card_index == 27 && !gc_menu_card_selected(&menu));
        gc_menu_press(&menu, GC_BUTTON_DOWN);
        assert(menu.card_index == 27 && menu.card_first_row == 3);
        gc_menu_press(&menu, GC_BUTTON_RIGHT);
        assert(menu.card_slot == 1 && menu.card_index == 12 &&
               menu.card_first_row == 0);
        press_many(&menu, GC_BUTTON_DOWN, 8);
        assert(menu.card_index == 44 && menu.card_first_row == 8);
        assert(gc_menu_card_first_row(&menu, 0) == 3);
        gc_menu_press(&menu, GC_BUTTON_LEFT);
        assert(menu.card_slot == 0 && menu.card_index == 27 &&
               menu.card_first_row == 3);
        assert(gc_menu_card_first_row(&menu, 1) == 8);
        gc_menu_press(&menu, GC_BUTTON_RIGHT);
        assert(menu.card_slot == 1 && menu.card_index == 44 &&
               menu.card_first_row == 8);
        gc_menu_press(&menu, GC_BUTTON_LEFT);
        press_many(&menu, GC_BUTTON_UP, 4);
        assert(menu.card_index == 11 && menu.card_first_row == 2);
        second = example_card(1, 59);
        assert(gc_menu_set_card(&menu, 1, &second));
        assert(gc_menu_card_first_row(&menu, 1) == 0);
        assert(gc_menu_card_cursor_index(&menu, 1) == 12);
        gc_menu_press(&menu, GC_BUTTON_RIGHT);
        assert(menu.card_slot == 1 && menu.card_index == 0 && menu.card_first_row == 0);
        gc_menu_press(&menu, GC_BUTTON_CANCEL);
        assert(menu.page == GC_PAGE_FACE);
        gc_menu_press(&menu, GC_BUTTON_CONFIRM);
        assert(menu.page == GC_PAGE_CARDS && menu.card_slot == 1 &&
               menu.card_index == 0);
        assert(gc_menu_card_first_row(&menu, 0) == 2);
        gc_menu_press(&menu, GC_BUTTON_LEFT);
        assert(menu.card_slot == 0 && menu.card_index == 11 &&
               menu.card_first_row == 2);
    }
}

static void test_card_directory_final_cell(void) {
    gc_menu menu;
    gc_card first = example_card(127, 254);
    gc_card second = example_card(0, 59);
    gc_menu_init(&menu, GC_REGION_USA);
    assert(gc_menu_set_card(&menu, 0, &first));
    assert(gc_menu_set_card(&menu, 1, &second));
    open_editor(&menu, GC_BUTTON_DOWN);
    press_many(&menu, GC_BUTTON_DOWN, 100);
    assert(menu.card_index == 124 && menu.card_first_row == 28);
    press_many(&menu, GC_BUTTON_RIGHT, 2);
    assert(menu.card_index == 126);
    gc_menu_press(&menu, GC_BUTTON_RIGHT);
    gc_menu_press(&menu, GC_BUTTON_DOWN);
    assert(menu.card_slot == 0 && menu.card_index == 126);
    gc_menu_press(&menu, GC_BUTTON_UP);
    gc_menu_press(&menu, GC_BUTTON_RIGHT);
    assert(menu.card_index == 123);
    gc_menu_press(&menu, GC_BUTTON_RIGHT);
    assert(menu.card_slot == 1 && menu.card_index == 8);
    gc_menu_press(&menu, GC_BUTTON_DOWN);
    gc_menu_press(&menu, GC_BUTTON_LEFT);
    assert(menu.card_slot == 1 && menu.card_index == 12);
    gc_menu_press(&menu, GC_BUTTON_UP);
    gc_menu_press(&menu, GC_BUTTON_LEFT);
    assert(menu.card_slot == 0 && menu.card_index == 123);
}

static void test_card_inserted_slot_selection(void) {
    for (unsigned region = GC_REGION_JAPAN; region <= GC_REGION_EUROPE; ++region) {
        gc_menu menu;
        gc_card ready = example_card(0, 59);
        gc_card absent = {0};
        gc_menu_init(&menu, (gc_region)region);
        assert(gc_menu_set_card(&menu, 1, &ready));
        open_editor(&menu, GC_BUTTON_DOWN);
        assert(menu.card_slot == 1 && menu.card_index == 0);
        press_many(&menu, GC_BUTTON_DOWN, 3);
        assert(menu.card_index == 12);
        assert(gc_menu_set_card(&menu, 0, &ready));
        assert(menu.card_slot == 1 && menu.card_index == 12);
        assert(gc_menu_set_card(&menu, 1, &absent));
        assert(menu.card_slot == 0 && menu.card_index == 0);
        press_many(&menu, GC_BUTTON_RIGHT, 4);
        assert(menu.card_slot == 0 && menu.card_index == 3);
        assert(gc_menu_set_card(&menu, 0, &absent));
        gc_menu_press(&menu, GC_BUTTON_DOWN);
        gc_menu_press(&menu, GC_BUTTON_RIGHT);
        assert(menu.card_slot == 0 && menu.card_index == 3);
        assert(gc_menu_set_card(&menu, 1, &ready));
        assert(menu.card_slot == 1 && menu.card_index == 12);
    }
}

static void test_disc_launch_and_ejection(void) {
    gc_menu menu;

    gc_menu_init(&menu, GC_REGION_USA);
    gc_menu_set_disc(&menu, GC_DISC_READY, "Local disc title", "Local publisher");
    open_editor(&menu, GC_BUTTON_UP);
    assert(menu.page == GC_PAGE_DISC);
    gc_menu_press(&menu, GC_BUTTON_CONFIRM);
    assert(menu.page == GC_PAGE_DISC && !menu.launch_requested);
    gc_menu_press(&menu, GC_BUTTON_START);
    assert(menu.page == GC_PAGE_GAME_STARTED && menu.launch_requested);
    gc_menu_press(&menu, GC_BUTTON_CANCEL);
    assert(menu.page == GC_PAGE_CUBE && !menu.launch_requested);
    gc_menu_press(&menu, GC_BUTTON_UP);
    gc_menu_press(&menu, GC_BUTTON_CONFIRM);
    gc_menu_set_disc(&menu, GC_DISC_LID_OPEN, NULL, NULL);
    assert(menu.page == GC_PAGE_DISC && menu.disc_status == GC_DISC_LID_OPEN);
    gc_menu_press(&menu, GC_BUTTON_START);
    assert(menu.page == GC_PAGE_DISC && !menu.launch_requested);
    gc_menu_set_disc(&menu, GC_DISC_UNREADABLE, NULL, NULL);
    assert(menu.page == GC_PAGE_DISC && menu.disc_status == GC_DISC_UNREADABLE);
    gc_menu_press(&menu, GC_BUTTON_CANCEL);
    assert(menu.page == GC_PAGE_FACE);
    gc_menu_press(&menu, GC_BUTTON_CONFIRM);
    assert(menu.page == GC_PAGE_DISC);
}

static void test_live_boot_owns_completion(void) {
    gc_menu menu;
    gc_menu_init(&menu, GC_REGION_EUROPE);
    menu.startup_controlled = true;
    gc_menu_tick(&menu, 30);
    gc_menu_press(&menu, GC_BUTTON_CONFIRM);
    gc_menu_press(&menu, GC_BUTTON_START);
    assert(menu.page == GC_PAGE_STARTUP);
    gc_menu_skip_startup(&menu);
    assert(menu.page == GC_PAGE_CUBE && menu.page_elapsed == 0);
    gc_menu_press(&menu, GC_BUTTON_RIGHT);
    assert(menu.page == GC_PAGE_FACE && menu.face == GC_FACE_CALENDAR);
}

int main(void) {
    test_startup_and_navigation();
    test_directional_face_navigation();
    test_startup_native_duration_and_override();
    test_clock_rollover();
    test_independent_clock_and_presentation();
    test_calendar_commit_cancel_and_regions();
    test_calendar_live_clock_and_tuple_commits();
    test_settings_commit_cancel_bounds();
    test_card_browser_and_erase();
    test_card_copy_move_and_duplicates();
    test_card_errors_and_insertion_during_confirmation();
    test_card_format_two_confirmations();
    test_card_action_navigation_and_decline();
    test_empty_card_cells_and_slot_edges();
    test_card_scroll_retains_each_slot();
    test_card_directory_final_cell();
    test_card_inserted_slot_selection();
    test_disc_launch_and_ejection();
    test_live_boot_owns_completion();
    puts("menu tests passed");
    return 0;
}
