#ifndef GAMECUBE_ERROR_CONTROL_H
#define GAMECUBE_ERROR_CONTROL_H

#include <stdbool.h>
#include <stdint.h>

enum {
    GC_ERROR_CONTROL_STARTUP_TICKS = 30,
    GC_ERROR_CONTROL_MENU_TICKS = 20,
    GC_ERROR_CONTROL_FADE_TICKS = GC_ERROR_CONTROL_STARTUP_TICKS
};

typedef struct {
    bool requested;
    uint8_t ticks;
    uint8_t fade_ticks;
} GcErrorControl;

/* Initialize a hidden overlay with the native startup or menu fade profile.
 * A zero-only state stays hidden but must be initialized before mutation.
 * State is plain data for frame snapshots. Toggling it off is a local
 * inspection adapter, not native fatal-error dismissal. */
void gc_error_control_init(GcErrorControl *state, bool startup);
bool gc_error_control_toggle(GcErrorControl *state);
bool gc_error_control_advance(GcErrorControl *state, uint64_t video_ticks);
uint8_t gc_error_control_alpha(const GcErrorControl *state);

#endif
