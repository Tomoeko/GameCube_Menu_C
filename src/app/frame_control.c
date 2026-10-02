#include "gamecube/frame_control.h"

#include <limits.h>
#include <math.h>

void gc_frame_control_init(GcFrameControl *control, bool enabled) {
    if (control)
        *control = (GcFrameControl){.enabled = enabled, .paused = enabled};
}

bool gc_frame_control_key(GcFrameControl *control, int key, bool down) {
    if (!control || !control->enabled)
        return false;
    unsigned mask = key == ',' ? 1u : key == '.' ? 2u : key == ' ' ? 4u : 0;
    if (!mask)
        return false;
    if (!down) {
        control->held_keys &= ~mask;
        return true;
    }
    if (control->held_keys & mask)
        return true;
    control->held_keys |= mask;
    control->fraction = 0;
    if (key == ' ')
        control->paused = !control->paused;
    else {
        control->paused = true;
        if (key == ',' && control->pending_steps > INT_MIN)
            --control->pending_steps;
        if (key == '.' && control->pending_steps < INT_MAX)
            ++control->pending_steps;
    }
    return true;
}

bool gc_frame_control_elapsed(GcFrameControl *control, double seconds,
                              unsigned frame_rate) {
    if (!control || !isfinite(seconds) || seconds < 0 || !frame_rate)
        return false;
    if (!control->paused) {
        double pending_video_ticks = control->fraction + seconds * frame_rate;
        if (!isfinite(pending_video_ticks))
            return false;
        /* Preserve sub-tick phase, but only the latest due presentation.
         * Blocking card work must not leave an accelerated replay backlog. */
        control->fraction = pending_video_ticks >= 1 ? 1 + fmod(pending_video_ticks, 1)
                                                     : pending_video_ticks;
    }
    return true;
}

int gc_frame_control_next(GcFrameControl *control) {
    if (!control)
        return 0;
    if (control->pending_steps < 0) {
        ++control->pending_steps;
        return -1;
    }
    if (control->pending_steps > 0) {
        --control->pending_steps;
        return 1;
    }
    if (!control->paused && control->fraction >= 1) {
        control->fraction -= 1;
        return 1;
    }
    return 0;
}
