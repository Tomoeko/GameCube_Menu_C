#include "gamecube/error_control.h"

static bool valid_state(const GcErrorControl *state) {
    return state &&
           (state->fade_ticks == GC_ERROR_CONTROL_STARTUP_TICKS ||
            state->fade_ticks == GC_ERROR_CONTROL_MENU_TICKS) &&
           state->ticks <= state->fade_ticks;
}

void gc_error_control_init(GcErrorControl *state, bool startup) {
    if (state)
        *state = (GcErrorControl){.fade_ticks = startup ? GC_ERROR_CONTROL_STARTUP_TICKS
                                                        : GC_ERROR_CONTROL_MENU_TICKS};
}

bool gc_error_control_toggle(GcErrorControl *state) {
    if (!valid_state(state))
        return false;
    state->requested = !state->requested;
    return true;
}

bool gc_error_control_advance(GcErrorControl *state, uint64_t video_ticks) {
    if (!valid_state(state))
        return false;
    if (state->requested) {
        unsigned remaining = state->fade_ticks - state->ticks;
        state->ticks = video_ticks >= remaining ? state->fade_ticks
                                                : (uint8_t)(state->ticks + video_ticks);
    } else {
        state->ticks =
            video_ticks >= state->ticks ? 0 : (uint8_t)(state->ticks - video_ticks);
    }
    return true;
}

uint8_t gc_error_control_alpha(const GcErrorControl *state) {
    if (!valid_state(state))
        return 0;
    return (uint8_t)(state->ticks * 255 / state->fade_ticks);
}
