#include "gamecube/launch_control.h"

/* USA sub_813023f8 advances the launch fader by three, then stops audio
 * only after exceeding 255. Boot handoff reaches this path already faded.
 * sub_81359bc0 resets its idle count whenever a channel remains active.
 */
bool gc_launch_control_init(GcLaunchControl *control, GcLaunchPath path) {
    if (!control || (path != GC_LAUNCH_FROM_MENU && path != GC_LAUNCH_FROM_BOOT))
        return false;
    *control = (GcLaunchControl){.alpha = path == GC_LAUNCH_FROM_BOOT ? 255 : 0};
    return true;
}

bool gc_launch_control_tick(GcLaunchControl *control) {
    if (!control || control->stop_requested || control->complete)
        return false;
    unsigned alpha = control->alpha + 3u;
    if (alpha > 255) {
        control->alpha = 255;
        control->stop_requested = true;
        return true;
    }
    control->alpha = (uint16_t)alpha;
    return false;
}

bool gc_launch_control_poll(GcLaunchControl *control, bool sequence_stopped,
                            unsigned active_voices) {
    if (!control || control->complete)
        return false;
    if (!control->stop_requested || !sequence_stopped || active_voices) {
        control->idle_polls = 0;
        return false;
    }
    if (++control->idle_polls < 30)
        return false;
    control->complete = true;
    return true;
}
