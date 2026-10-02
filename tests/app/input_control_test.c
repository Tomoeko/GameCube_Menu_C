#include "gamecube/input_control.h"

#include <assert.h>
#include <stddef.h>
#include <stdio.h>

static GcInputFrame sample(GcInputControl *control) {
    GcInputFrame frame;
    assert(gc_input_control_sample(control, &frame));
    return frame;
}

static void test_native_repeat_and_host_repeat(void) {
    GcInputControl control;
    gc_input_control_init(&control);
    assert(gc_input_control_button(&control, GC_BUTTON_UP, true));
    GcInputFrame frame = sample(&control);
    assert(frame.held == GC_INPUT_UP && frame.pressed == GC_INPUT_UP);
    assert(frame.navigation == GC_INPUT_UP && !frame.repeated);
    for (unsigned tick = 1; tick <= 76; ++tick) {
        assert(gc_input_control_button(&control, GC_BUTTON_UP, true));
        frame = sample(&control);
        bool repeat = tick == 35 || (tick >= 46 && (tick - 46) % 10 == 0);
        assert(frame.pressed == 0);
        assert(frame.navigation == (repeat ? GC_INPUT_UP : 0));
        assert(frame.repeated == repeat);
    }
    assert(gc_input_control_button(&control, GC_BUTTON_UP, false));
    frame = sample(&control);
    assert(frame.held == 0 && frame.released == GC_INPUT_UP);
    assert(!frame.navigation && !frame.repeated && !control.repeat_ticks);
}

static void test_changed_direction_and_quick_taps(void) {
    GcInputControl control;
    gc_input_control_init(&control);
    assert(gc_input_control_button(&control, GC_BUTTON_LEFT, true));
    sample(&control);
    for (unsigned tick = 0; tick < 34; ++tick)
        assert(!sample(&control).repeated);
    assert(gc_input_control_button(&control, GC_BUTTON_RIGHT, true));
    GcInputFrame frame = sample(&control);
    assert(frame.navigation == GC_INPUT_RIGHT && !frame.repeated);
    assert(!control.repeat_ticks);
    for (unsigned tick = 0; tick < 34; ++tick)
        assert(!sample(&control).repeated);
    frame = sample(&control);
    assert(frame.navigation == (GC_INPUT_LEFT | GC_INPUT_RIGHT) && frame.repeated);
    gc_input_control_init(&control);
    assert(gc_input_control_button(&control, GC_BUTTON_DOWN, true));
    assert(gc_input_control_button(&control, GC_BUTTON_DOWN, false));
    frame = sample(&control);
    assert(frame.held == 0 && frame.pressed == GC_INPUT_DOWN);
    assert(frame.released == GC_INPUT_DOWN && frame.navigation == GC_INPUT_DOWN);
    frame = sample(&control);
    assert(!frame.pressed && !frame.released && !frame.navigation);
    assert(gc_input_control_button(&control, GC_BUTTON_CONFIRM, true));
    assert(gc_input_control_button(&control, GC_BUTTON_CONFIRM, false));
    assert(sample(&control).pressed == GC_INPUT_A);
}

static void test_native_cancel_priority(void) {
    GcInputControl control;
    gc_input_control_init(&control);
    assert(gc_input_control_button(&control, GC_BUTTON_CONFIRM, true));
    assert(gc_input_control_button(&control, GC_BUTTON_START, true));
    assert(gc_input_control_button(&control, GC_BUTTON_CANCEL, true));
    assert(gc_input_control_button(&control, GC_BUTTON_UP, true));
    GcInputFrame frame = sample(&control);
    assert(frame.held == (GC_INPUT_A | GC_INPUT_B | GC_INPUT_START | GC_INPUT_UP));
    assert(frame.pressed == (GC_INPUT_B | GC_INPUT_UP));
    assert(frame.navigation == GC_INPUT_UP);
    assert(gc_input_control_button(&control, GC_BUTTON_CONFIRM, false));
    sample(&control);
    assert(gc_input_control_button(&control, GC_BUTTON_CONFIRM, true));
    frame = sample(&control);
    assert(frame.pressed == GC_INPUT_A); /* Already-held B has no new edge. */
    for (unsigned tick = 0; tick < 100; ++tick) {
        frame = sample(&control);
        assert(!(frame.pressed & (GC_INPUT_A | GC_INPUT_B | GC_INPUT_START)));
    }
}

int main(void) {
    test_native_repeat_and_host_repeat();
    test_changed_direction_and_quick_taps();
    test_native_cancel_priority();
    GcInputControl control;
    GcInputFrame frame;
    gc_input_control_init(NULL);
    assert(gc_input_button_mask((gc_button)7) == 0);
    assert(!gc_input_control_button(NULL, GC_BUTTON_UP, true));
    assert(!gc_input_control_button(&control, (gc_button)7, true));
    assert(!gc_input_control_sample(NULL, &frame));
    assert(!gc_input_control_sample(&control, NULL));
    puts("input control tests passed");
    return 0;
}
