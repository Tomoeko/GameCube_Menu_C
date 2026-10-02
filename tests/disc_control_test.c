#include "gamecube/disc_control.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_caption_mapping(void) {
    assert(gc_disc_control_text_index(GC_DISC_READY) == 0);
    assert(gc_disc_control_text_index(GC_DISC_FATAL) == 1);
    assert(gc_disc_control_text_index(GC_DISC_UNREADABLE) == 2);
    assert(gc_disc_control_text_index(GC_DISC_ABSENT) == 3);
    assert(gc_disc_control_text_index(GC_DISC_LID_OPEN) == 3);
    assert(gc_disc_control_text_index(GC_DISC_READING) == 4);
    assert(gc_disc_control_text_index((gc_disc_status)-1) == GC_DISC_CONTROL_TEXT_NONE);
    assert(gc_disc_control_text_index((gc_disc_status)99) == GC_DISC_CONTROL_TEXT_NONE);
}

static void test_pending_read_and_launch_gate(void) {
    const gc_region regions[] = {GC_REGION_USA, GC_REGION_EUROPE, GC_REGION_JAPAN};
    for (size_t region = 0; region < sizeof(regions) / sizeof(regions[0]); ++region) {
        GcDiscControl drive;
        gc_menu menu;
        gc_menu_init(&menu, regions[region]);
        menu.page = GC_PAGE_DISC;
        assert(gc_disc_control_init(&drive, menu.disc_status));
        assert(!drive.media_present && drive.phase == GC_DISC_CONTROL_IDLE);
        assert(gc_disc_control_toggle(&drive));
        assert(drive.media_present && drive.status == GC_DISC_LID_OPEN);
        gc_menu_set_disc(&menu, drive.status, NULL, NULL);
        gc_menu_press(&menu, GC_BUTTON_START);
        assert(menu.page == GC_PAGE_DISC && !menu.launch_requested);

        assert(gc_disc_control_advance(&drive, GC_DISC_CONTROL_CLOSING_TICKS - 1));
        assert(drive.status == GC_DISC_LID_OPEN && drive.remaining_ticks == 1);
        assert(gc_disc_control_advance(&drive, 0));
        assert(drive.status == GC_DISC_LID_OPEN && drive.remaining_ticks == 1);
        assert(gc_disc_control_advance(&drive, 1));
        assert(drive.status == GC_DISC_READING &&
               drive.phase == GC_DISC_CONTROL_READING &&
               drive.remaining_ticks == GC_DISC_CONTROL_READING_TICKS);
        gc_menu_set_disc(&menu, drive.status, NULL, NULL);
        assert(menu.disc_status == GC_DISC_READING);
        gc_menu_press(&menu, GC_BUTTON_START);
        assert(menu.page == GC_PAGE_DISC && !menu.launch_requested);
        assert(gc_disc_control_advance(&drive, GC_DISC_CONTROL_READING_TICKS - 1));
        assert(drive.status == GC_DISC_READING && drive.remaining_ticks == 1);
        assert(gc_disc_control_advance(&drive, 1));
        assert(drive.status == GC_DISC_READY && drive.media_present &&
               drive.phase == GC_DISC_CONTROL_IDLE && !drive.remaining_ticks);
        gc_menu_set_disc(&menu, drive.status, "Local test media", "Test publisher");
        gc_menu_press(&menu, GC_BUTTON_START);
        assert(menu.page == GC_PAGE_GAME_STARTED && menu.launch_requested);
    }
}

static void test_ejection_interrupts_every_pending_phase(void) {
    const unsigned delays[] = {0, GC_DISC_CONTROL_CLOSING_TICKS - 1,
                               GC_DISC_CONTROL_CLOSING_TICKS,
                               GC_DISC_CONTROL_CLOSING_TICKS + 47};
    for (size_t index = 0; index < sizeof(delays) / sizeof(delays[0]); ++index) {
        GcDiscControl drive;
        assert(gc_disc_control_init(&drive, GC_DISC_ABSENT));
        assert(gc_disc_control_toggle(&drive));
        assert(gc_disc_control_advance(&drive, delays[index]));
        assert(gc_disc_control_toggle(&drive));
        assert(!drive.media_present && drive.status == GC_DISC_LID_OPEN &&
               drive.phase == GC_DISC_CONTROL_IDLE && !drive.remaining_ticks);
        assert(gc_disc_control_advance(&drive, UINT64_MAX));
        assert(drive.status == GC_DISC_LID_OPEN && !drive.media_present);
        assert(gc_disc_control_toggle(&drive));
        assert(drive.remaining_ticks == GC_DISC_CONTROL_CLOSING_TICKS);
        assert(gc_disc_control_advance(&drive, GC_DISC_CONTROL_CLOSING_TICKS));
        assert(drive.status == GC_DISC_READING);
        assert(gc_disc_control_advance(&drive, UINT64_MAX));
        assert(drive.status == GC_DISC_READY && !drive.remaining_ticks);
    }
}

static void test_read_errors_remain_bound_to_media(void) {
    const gc_disc_status errors[] = {GC_DISC_UNREADABLE, GC_DISC_FATAL};
    for (size_t index = 0; index < sizeof(errors) / sizeof(errors[0]); ++index) {
        GcDiscControl drive;
        assert(gc_disc_control_init(&drive, errors[index]));
        assert(drive.media_present && drive.read_result == errors[index]);
        assert(gc_disc_control_toggle(&drive));
        assert(gc_disc_control_toggle(&drive));
        assert(gc_disc_control_advance(&drive, UINT64_MAX));
        assert(drive.status == errors[index] && drive.media_present);
    }
}

static void test_value_snapshot_and_batched_updates(void) {
    GcDiscControl drive;
    assert(gc_disc_control_init(&drive, GC_DISC_LID_OPEN));
    assert(gc_disc_control_toggle(&drive));
    assert(gc_disc_control_advance(&drive, GC_DISC_CONTROL_CLOSING_TICKS + 37));
    GcDiscControl saved = drive;
    assert(drive.status == GC_DISC_READING && drive.remaining_ticks == 83);
    for (unsigned tick = 0; tick < 83; ++tick)
        assert(gc_disc_control_advance(&drive, 1));
    assert(drive.status == GC_DISC_READY);
    assert(gc_disc_control_toggle(&drive));
    assert(!drive.media_present);
    drive = saved;
    assert(gc_disc_control_advance(&drive, 83));
    assert(drive.status == GC_DISC_READY && drive.media_present);

    GcDiscControl batched;
    assert(gc_disc_control_init(&batched, GC_DISC_ABSENT));
    assert(gc_disc_control_toggle(&batched));
    assert(gc_disc_control_advance(&batched, UINT64_MAX));
    assert(batched.status == drive.status && batched.phase == drive.phase &&
           batched.remaining_ticks == drive.remaining_ticks &&
           batched.media_present == drive.media_present);
}

static void test_invalid_arguments_leave_state_intact(void) {
    GcDiscControl drive;
    assert(gc_disc_control_init(&drive, GC_DISC_READY));
    GcDiscControl saved = drive;
    assert(!gc_disc_control_init(&drive, (gc_disc_status)99));
    assert(!memcmp(&drive, &saved, sizeof(drive)));
    assert(!gc_disc_control_init(NULL, GC_DISC_READY));
    assert(!gc_disc_control_toggle(NULL));
    assert(!gc_disc_control_advance(NULL, 1));
    drive.phase = (GcDiscControlPhase)99;
    saved = drive;
    assert(!gc_disc_control_toggle(&drive));
    assert(!gc_disc_control_advance(&drive, 1000));
    assert(!memcmp(&drive, &saved, sizeof(drive)));
    drive = (GcDiscControl){.status = GC_DISC_READING,
                            .phase = GC_DISC_CONTROL_READING,
                            .read_result = GC_DISC_READY,
                            .remaining_ticks = GC_DISC_CONTROL_READING_TICKS + 1,
                            .media_present = true};
    saved = drive;
    assert(!gc_disc_control_advance(&drive, 1));
    assert(!memcmp(&drive, &saved, sizeof(drive)));
}

int main(void) {
    test_caption_mapping();
    test_pending_read_and_launch_gate();
    test_ejection_interrupts_every_pending_phase();
    test_read_errors_remain_bound_to_media();
    test_value_snapshot_and_batched_updates();
    test_invalid_arguments_leave_state_intact();
    puts("Simulated drive transitions and native caption mapping passed.");
    return 0;
}
