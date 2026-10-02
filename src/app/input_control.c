#include "gamecube/input_control.h"

void gc_input_control_init(GcInputControl *control) {
    if (control)
        *control = (GcInputControl){0};
}

uint16_t gc_input_button_mask(gc_button button) {
    switch (button) {
        case GC_BUTTON_UP:
            return GC_INPUT_UP;
        case GC_BUTTON_DOWN:
            return GC_INPUT_DOWN;
        case GC_BUTTON_LEFT:
            return GC_INPUT_LEFT;
        case GC_BUTTON_RIGHT:
            return GC_INPUT_RIGHT;
        case GC_BUTTON_CONFIRM:
            return GC_INPUT_A;
        case GC_BUTTON_CANCEL:
            return GC_INPUT_B;
        case GC_BUTTON_START:
            return GC_INPUT_START;
        default:
            return 0;
    }
}

bool gc_input_control_button(GcInputControl *control, gc_button button, bool down) {
    uint16_t mask = gc_input_button_mask(button);
    if (!control || !mask)
        return false;
    if (down && !(control->held & mask)) {
        control->held |= mask;
        control->pending_pressed |= mask;
    } else if (!down && control->held & mask) {
        control->held &= (uint16_t)~mask;
        control->pending_released |= mask;
    }
    return true;
}

bool gc_input_control_sample(GcInputControl *control, GcInputFrame *frame) {
    if (!control || !frame)
        return false;
    *frame =
        (GcInputFrame){.held = control->held,
                       .pressed = control->pending_pressed,
                       .released = control->pending_released,
                       .navigation = control->pending_pressed & GC_INPUT_DIRECTIONS};
    uint16_t directions = control->held & GC_INPUT_DIRECTIONS;
    /* USA sub_81302818 resets on any changed direction mask. The strict
     * >35+10 test creates the original one-tick gap after the first repeat.
     * The same 35/10 initializer constants are used by the PAL profile.
     */
    if (!directions || directions != control->previous_directions)
        control->repeat_ticks = 0;
    else {
        ++control->repeat_ticks;
        if (control->repeat_ticks == 35 || control->repeat_ticks > 45) {
            frame->navigation |= directions;
            frame->repeated = true;
            if (control->repeat_ticks > 45)
                control->repeat_ticks -= 10;
        }
    }
    control->previous_directions = directions;
    control->pending_pressed = control->pending_released = 0;
    if (frame->pressed & GC_INPUT_B)
        frame->pressed &= (uint16_t) ~(GC_INPUT_A | GC_INPUT_START);
    return true;
}
