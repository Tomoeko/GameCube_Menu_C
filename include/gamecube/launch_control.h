#ifndef GAMECUBE_LAUNCH_CONTROL_H
#define GAMECUBE_LAUNCH_CONTROL_H

#include <stdbool.h>
#include <stdint.h>

typedef enum { GC_LAUNCH_FROM_MENU, GC_LAUNCH_FROM_BOOT } GcLaunchPath;

typedef struct {
    uint16_t alpha;
    unsigned idle_polls;
    bool stop_requested;
    bool complete;
} GcLaunchControl;

bool gc_launch_control_init(GcLaunchControl *control, GcLaunchPath path);
/* Once per native video tick. Returns a one-shot audio stop request. */
bool gc_launch_control_tick(GcLaunchControl *control);
/* Native sub_81359bc0 polls the channel pool in a CPU loop after stopping
 * the sequence. These calls are independent of video ticks. Returns a
 * one-shot handoff when 30 consecutive observed polls have no channels.
 */
bool gc_launch_control_poll(GcLaunchControl *control, bool sequence_stopped,
                            unsigned active_voices);

#endif
