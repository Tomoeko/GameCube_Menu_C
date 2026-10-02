#ifndef GAMECUBE_INPUT_CONTROL_H
#define GAMECUBE_INPUT_CONTROL_H

#include "gamecube/menu.h"

#define GC_INPUT_LEFT UINT16_C(0x0001)
#define GC_INPUT_RIGHT UINT16_C(0x0002)
#define GC_INPUT_DOWN UINT16_C(0x0004)
#define GC_INPUT_UP UINT16_C(0x0008)
#define GC_INPUT_A UINT16_C(0x0100)
#define GC_INPUT_B UINT16_C(0x0200)
#define GC_INPUT_START UINT16_C(0x1000)
#define GC_INPUT_DIRECTIONS UINT16_C(0x000f)

typedef struct {
    uint16_t held;
    uint16_t pending_pressed;
    uint16_t pending_released;
    uint16_t previous_directions;
    unsigned repeat_ticks;
} GcInputControl;

typedef struct {
    uint16_t held;
    uint16_t pressed;
    uint16_t released;
    uint16_t navigation;
    bool repeated;
} GcInputFrame;

void gc_input_control_init(GcInputControl *control);
uint16_t gc_input_button_mask(gc_button button);
/* Feed host key edges. Already-held downs are ignored, including OS repeat.
 * A down/up pair between video updates preserves its pressed edge.
 */
bool gc_input_control_button(GcInputControl *control, gc_button button, bool down);
/* Once per native video tick. Direction repeat begins after 35 ticks, repeats
 * at 46, then every 10 ticks. Newly pressed B suppresses simultaneous A/START.
 * Page and animation readiness gates belong to the native page controllers.
 */
bool gc_input_control_sample(GcInputControl *control, GcInputFrame *frame);

#endif
