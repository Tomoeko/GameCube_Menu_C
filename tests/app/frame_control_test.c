#include "gamecube/frame_control.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>

static void key(GcFrameControl *control, int value) {
    assert(gc_frame_control_key(control, value, true));
    assert(gc_frame_control_key(control, value, false));
}

static void test_paused_steps(void) {
    GcFrameControl control;
    gc_frame_control_init(&control, true);
    assert(control.paused);
    assert(gc_frame_control_elapsed(&control, 100, 50));
    assert(!gc_frame_control_next(&control));
    assert(gc_frame_control_key(&control, '.', true));
    assert(gc_frame_control_key(&control, '.', true));
    assert(gc_frame_control_next(&control) == 1);
    assert(!gc_frame_control_next(&control));
    assert(gc_frame_control_key(&control, '.', false));
    key(&control, ',');
    assert(gc_frame_control_next(&control) == -1);
    assert(!gc_frame_control_next(&control));
    assert(!gc_frame_control_key(&control, 'a', true));
}

static void test_native_rates(void) {
    for (unsigned rate = 50; rate <= 60; rate += 10) {
        GcFrameControl control;
        gc_frame_control_init(&control, true);
        key(&control, ' ');
        assert(!control.paused);
        for (unsigned tick = 0; tick < rate; ++tick) {
            assert(gc_frame_control_elapsed(&control, 1.0 / rate, rate));
            assert(gc_frame_control_next(&control) == 1);
            assert(!gc_frame_control_next(&control));
        }
        assert(gc_frame_control_elapsed(&control, 0.5 / rate, rate));
        assert(!gc_frame_control_next(&control));
        key(&control, ',');
        assert(control.paused && control.fraction == 0);
        assert(gc_frame_control_next(&control) == -1);
        key(&control, ' ');
        assert(gc_frame_control_elapsed(&control, 1.0 / rate, rate));
        assert(gc_frame_control_next(&control) == 1);
        assert(!gc_frame_control_next(&control));
    }
    GcFrameControl normal;
    gc_frame_control_init(&normal, false);
    assert(!normal.paused && !gc_frame_control_key(&normal, '.', true));
    assert(!gc_frame_control_elapsed(&normal, NAN, 50));
    assert(!gc_frame_control_elapsed(&normal, -1, 50));
    assert(!gc_frame_control_elapsed(&normal, 1, 0));
}

static void test_realtime_stall_discards_stale_ticks(void) {
    for (unsigned rate = 50; rate <= 60; rate += 10) {
        GcFrameControl control;
        gc_frame_control_init(&control, false);
        assert(gc_frame_control_elapsed(&control, 3, rate));
        assert(gc_frame_control_next(&control) == 1);
        assert(!gc_frame_control_next(&control));
        unsigned updates = 0;
        for (unsigned poll = 0; poll < 250; ++poll) {
            assert(gc_frame_control_elapsed(&control, 0.001, rate));
            if (gc_frame_control_next(&control))
                ++updates;
            assert(!gc_frame_control_next(&control));
        }
        assert(updates >= rate / 4 - 1 && updates <= rate / 4 + 1);
        assert(gc_frame_control_elapsed(&control, 3600, rate));
        assert(gc_frame_control_next(&control) == 1);
        assert(!gc_frame_control_next(&control));
    }
}

int main(void) {
    test_paused_steps();
    test_native_rates();
    test_realtime_stall_discards_stale_ticks();
    puts("Frame pause and native video-step tests passed.");
    return 0;
}
