#ifndef GAMECUBE_FRAME_HISTORY_H
#define GAMECUBE_FRAME_HISTORY_H

#include "gamecube/render.h"
#include "gamecube/input_control.h"
#include "gamecube/card_runtime.h"
#include "gamecube/launch_control.h"
#include "gamecube/disc_control.h"
#include "gamecube/error_control.h"

typedef struct GcFrameHistory GcFrameHistory;
typedef struct {
    GcInputControl input;
    GcBootInput boot_input;
    GcFaceRandom random;
    GcCardRuntime cards;
    GcLaunchControl launch;
    GcDiscControl disc;
    GcErrorControl error;
    bool pending_disc_toggle;
    bool disc_toggle_held;
    bool pending_error_toggle;
    bool error_toggle_held;
    bool launching;
    gc_menu *launch_menu; /* Borrowed; retain presentations until history resets. */
    double boot_fraction;
    double input_fraction;
    double launch_fraction;
    bool startup_waiting;
    uint64_t startup_wait_ticks;
    double startup_delay_elapsed;
} GcFrameRuntime;

/* Allocates a bounded visual history once. Capacity must be at least two.
 * Resources and card metadata retain their live ownership. Reset history
 * whenever storage, card art, disc art, or native resources are replaced.
 */
GcFrameHistory *gc_frame_history_create(size_t capacity);
void gc_frame_history_destroy(GcFrameHistory *history);
void gc_frame_history_reset(GcFrameHistory *history);
/* Save after drawing a video tick. Saving after rewind discards its future.
 * Existing frames use their saved visual state; no service work is replayed.
 */
bool gc_frame_history_save(GcFrameHistory *history, uint64_t counter,
                           const gc_menu *menu, const GcBootControl *boot,
                           const GcScene *scene, const GcFrameRuntime *runtime);
/* Move by signed frame count. Out-of-range requests leave all state intact.
 * HUD preferences, resource pointers and live card metadata are preserved.
 */
bool gc_frame_history_seek(GcFrameHistory *history, int delta, uint64_t *counter,
                           gc_menu *menu, GcBootControl *boot, GcScene *scene,
                           GcFrameRuntime *runtime);
size_t gc_frame_history_count(const GcFrameHistory *history);
size_t gc_frame_history_position(const GcFrameHistory *history);

#endif
