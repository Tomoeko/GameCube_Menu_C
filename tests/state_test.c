#include "gamecube/state.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_roundtrip_and_elapsed_time(const char *path) {
    gc_menu source;
    gc_menu restored;
    gc_date_time clock = {2024, 2, 28, 23, 59, 59};

    gc_menu_init(&source, GC_REGION_EUROPE);
    gc_menu_init(&restored, GC_REGION_EUROPE);
    assert(gc_menu_set_clock(&source, &clock));
    source.clock_fraction = 0.25;
    source.settings.sound = GC_SOUND_MONO;
    source.settings.screen_position = -17;
    source.settings.language = GC_LANGUAGE_FRENCH;
    source.settings_changed = true;
    source.clock_changed = true;
    assert(gc_state_save(&source, path, 1000) == GC_STATE_OK);
    assert(!source.settings_changed && !source.clock_changed);
    restored.startup_elapsed = 2.0;
    restored.page_elapsed = 2.0;
    assert(gc_state_load(&restored, path, 1002) == GC_STATE_OK);
    assert(restored.settings.sound == GC_SOUND_MONO);
    assert(restored.settings.screen_position == -17);
    assert(restored.settings.language == GC_LANGUAGE_FRENCH);
    assert(restored.clock.month == 2 && restored.clock.day == 29);
    assert(restored.clock.hour == 0 && restored.clock.minute == 0 &&
           restored.clock.second == 1);
    assert(restored.clock_fraction == 0.25);
    assert(restored.page == GC_PAGE_STARTUP && restored.startup_elapsed == 2.0 &&
           restored.page_elapsed == 2.0);
    assert(gc_state_load(&restored, path, 900) == GC_STATE_OK);
    assert(restored.clock.day == 28 && restored.clock.second == 59);
    gc_menu_init(&restored, GC_REGION_USA);
    assert(gc_state_load(&restored, path, 1000) == GC_STATE_REGION);
    assert(restored.clock.year == 2000 && restored.settings.sound == GC_SOUND_STEREO);
}

static void test_edit_preview_is_not_persisted(const char *path) {
    gc_menu source;
    gc_menu restored;

    gc_menu_init(&source, GC_REGION_EUROPE);
    gc_menu_init(&restored, GC_REGION_EUROPE);
    source.page = GC_PAGE_OPTIONS;
    source.settings_before_edit = source.settings;
    source.settings.sound = GC_SOUND_MONO;
    source.editing = true;
    assert(gc_state_save(&source, path, 1000) == GC_STATE_OK);
    assert(gc_state_load(&restored, path, 1000) == GC_STATE_OK);
    assert(restored.settings.sound == GC_SOUND_STEREO);
    source.page = GC_PAGE_CALENDAR;
    source.clock_before_edit = source.clock;
    source.clock.year = 2024;
    gc_menu_tick(&source, 3.5);
    assert(source.clock.year == 2024 && source.clock.second == 3);
    assert(source.clock_before_edit.second == 3 && source.clock_fraction == 0.5);
    assert(gc_state_save(&source, path, 1000) == GC_STATE_OK);
    assert(gc_state_load(&restored, path, 1002) == GC_STATE_OK);
    assert(restored.clock.year == 2000 && restored.clock.second == 5);
    assert(restored.clock_fraction == 0.5);
}

static void test_corruption_and_failed_save_flags(const char *path) {
    gc_menu source;
    gc_menu restored;
    FILE *file;

    gc_menu_init(&source, GC_REGION_EUROPE);
    gc_menu_init(&restored, GC_REGION_EUROPE);
    assert(gc_state_save(&source, path, 1000) == GC_STATE_OK);
    file = fopen(path, "r+b");
    assert(file);
    assert(fseek(file, 15, SEEK_SET) == 0);
    assert(fputc(255, file) != EOF);
    assert(fclose(file) == 0);
    assert(gc_state_load(&restored, path, 1000) == GC_STATE_FORMAT);
    assert(restored.settings.language == GC_LANGUAGE_ENGLISH);
    assert(gc_state_save(&source, path, 1000) == GC_STATE_OK);
    file = fopen(path, "ab");
    assert(file);
    assert(fputc(0, file) != EOF);
    assert(fclose(file) == 0);
    assert(gc_state_load(&restored, path, 1000) == GC_STATE_FORMAT);
    source.settings_changed = true;
    source.clock_changed = true;
    assert(gc_state_save(&source, "", 1000) == GC_STATE_IO);
    assert(source.settings_changed && source.clock_changed);
    assert(gc_state_save(&source, path, -1) == GC_STATE_ARGUMENT);
    source.settings.screen_position = 33;
    assert(gc_state_save(&source, path, 1000) == GC_STATE_ARGUMENT);
    assert(remove(path) == 0);
    assert(gc_state_load(&restored, path, 1000) == GC_STATE_MISSING);
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "state_test.dat";

    test_roundtrip_and_elapsed_time(path);
    test_edit_preview_is_not_persisted(path);
    test_corruption_and_failed_save_flags(path);
    puts("state tests passed");
    return 0;
}
