#include "gamecube/disc_control.h"

static bool valid_result(gc_disc_status status) {
    return status == GC_DISC_READY || status == GC_DISC_UNREADABLE ||
           status == GC_DISC_FATAL;
}

static bool valid_state(const GcDiscControl *state) {
    if (!state || (unsigned)state->status > GC_DISC_FATAL ||
        !valid_result(state->read_result))
        return false;
    switch (state->phase) {
        case GC_DISC_CONTROL_IDLE:
            return state->remaining_ticks == 0 &&
                   (state->media_present ? state->status == state->read_result
                                         : state->status == GC_DISC_ABSENT ||
                                               state->status == GC_DISC_LID_OPEN);
        case GC_DISC_CONTROL_CLOSING:
            return state->media_present && state->status == GC_DISC_LID_OPEN &&
                   state->remaining_ticks > 0 &&
                   state->remaining_ticks <= GC_DISC_CONTROL_CLOSING_TICKS;
        case GC_DISC_CONTROL_READING:
            return state->media_present && state->status == GC_DISC_READING &&
                   state->remaining_ticks > 0 &&
                   state->remaining_ticks <= GC_DISC_CONTROL_READING_TICKS;
        default:
            return false;
    }
}

bool gc_disc_control_init(GcDiscControl *state, gc_disc_status status) {
    if (!state || (unsigned)status > GC_DISC_FATAL)
        return false;
    GcDiscControl candidate = {
        .status = status,
        .phase =
            status == GC_DISC_READING ? GC_DISC_CONTROL_READING : GC_DISC_CONTROL_IDLE,
        .read_result = valid_result(status) ? status : GC_DISC_READY,
        .remaining_ticks =
            status == GC_DISC_READING ? GC_DISC_CONTROL_READING_TICKS : 0,
        .media_present = status != GC_DISC_ABSENT && status != GC_DISC_LID_OPEN};
    *state = candidate;
    return true;
}

bool gc_disc_control_toggle(GcDiscControl *state) {
    if (!valid_state(state))
        return false;
    if (state->media_present) {
        state->phase = GC_DISC_CONTROL_IDLE;
        state->remaining_ticks = 0;
        state->media_present = false;
    } else {
        state->phase = GC_DISC_CONTROL_CLOSING;
        state->remaining_ticks = GC_DISC_CONTROL_CLOSING_TICKS;
        state->media_present = true;
    }
    state->status = GC_DISC_LID_OPEN;
    return true;
}

bool gc_disc_control_advance(GcDiscControl *state, uint64_t video_ticks) {
    if (!valid_state(state))
        return false;
    while (video_ticks && state->phase != GC_DISC_CONTROL_IDLE) {
        if (video_ticks < state->remaining_ticks) {
            state->remaining_ticks = (uint16_t)(state->remaining_ticks - video_ticks);
            break;
        }
        video_ticks -= state->remaining_ticks;
        if (state->phase == GC_DISC_CONTROL_CLOSING) {
            state->phase = GC_DISC_CONTROL_READING;
            state->status = GC_DISC_READING;
            state->remaining_ticks = GC_DISC_CONTROL_READING_TICKS;
        } else {
            state->phase = GC_DISC_CONTROL_IDLE;
            state->status = state->read_result;
            state->remaining_ticks = 0;
        }
    }
    return true;
}

unsigned gc_disc_control_text_index(gc_disc_status status) {
    /* USA 0x813264d4 assigns STH entries to five faders; 0x81326674
     * selects them from the drive status. PAL uses the same semantics. */
    switch (status) {
        case GC_DISC_READY:
            return 0;
        case GC_DISC_FATAL:
            return 1;
        case GC_DISC_UNREADABLE:
            return 2;
        case GC_DISC_ABSENT:
        case GC_DISC_LID_OPEN:
            return 3;
        case GC_DISC_READING:
            return 4;
        default:
            return GC_DISC_CONTROL_TEXT_NONE;
    }
}
