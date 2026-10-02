#include "gamecube/launch_control.h"

#include <assert.h>
#include <stddef.h>
#include <stdio.h>

static void test_menu_fader_and_handoff(void) {
    GcLaunchControl control;
    assert(gc_launch_control_init(&control, GC_LAUNCH_FROM_MENU));
    assert(control.alpha == 0);
    for (unsigned tick = 1; tick <= 85; ++tick) {
        assert(!gc_launch_control_tick(&control));
        assert(control.alpha == tick * 3);
        assert(!gc_launch_control_poll(&control, true, 0));
    }
    assert(control.alpha == 255 && !control.stop_requested);
    assert(gc_launch_control_tick(&control));
    assert(control.alpha == 255 && control.stop_requested);
    assert(!gc_launch_control_tick(&control));
    for (unsigned poll = 0; poll < 100; ++poll)
        assert(!gc_launch_control_poll(&control, false, 0));
    assert(control.idle_polls == 0);
    for (unsigned poll = 1; poll < 30; ++poll) {
        assert(!gc_launch_control_poll(&control, true, 0));
        assert(control.idle_polls == poll);
    }
    assert(gc_launch_control_poll(&control, true, 0));
    assert(control.complete && control.idle_polls == 30);
    assert(!gc_launch_control_poll(&control, true, 0));
}

static void test_boot_fade_and_idle_reset(void) {
    GcLaunchControl control;
    assert(gc_launch_control_init(&control, GC_LAUNCH_FROM_BOOT));
    assert(control.alpha == 255);
    assert(gc_launch_control_tick(&control));
    for (unsigned poll = 0; poll < 29; ++poll)
        assert(!gc_launch_control_poll(&control, true, 0));
    assert(!gc_launch_control_poll(&control, true, 1));
    assert(control.idle_polls == 0);
    for (unsigned poll = 0; poll < 29; ++poll)
        assert(!gc_launch_control_poll(&control, true, 0));
    assert(gc_launch_control_poll(&control, true, 0));
    assert(control.alpha == 255);
}

int main(void) {
    test_menu_fader_and_handoff();
    test_boot_fade_and_idle_reset();
    GcLaunchControl control;
    assert(!gc_launch_control_init(NULL, GC_LAUNCH_FROM_MENU));
    assert(!gc_launch_control_init(&control, (GcLaunchPath)2));
    assert(!gc_launch_control_tick(NULL));
    assert(!gc_launch_control_poll(NULL, true, 0));
    puts("launch control tests passed");
    return 0;
}
