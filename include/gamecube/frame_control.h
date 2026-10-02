#ifndef GAMECUBE_FRAME_CONTROL_H
#define GAMECUBE_FRAME_CONTROL_H

#include <stdbool.h>

typedef struct {
    bool enabled;
    bool paused;
    unsigned held_keys;
    int pending_steps;
    double fraction;
} GcFrameControl;

void gc_frame_control_init(GcFrameControl *control, bool enabled);
/* ASCII comma steps backward, period steps forward, Space toggles pause.
 * Repeated host key-down events do not create extra steps.
 */
bool gc_frame_control_key(GcFrameControl *control, int key, bool down);
/* Feed elapsed host time once. Paused time and missed presentation ticks are
 * discarded. Retain at most the latest due tick and its fractional phase,
 * so a blocking service cannot create a burst of accelerated animation.
 * Explicit inspection steps remain queued independently of wall time.
 */
bool gc_frame_control_elapsed(GcFrameControl *control, double seconds,
                              unsigned frame_rate);
int gc_frame_control_next(GcFrameControl *control);

#endif
