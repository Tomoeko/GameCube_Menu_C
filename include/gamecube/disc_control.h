#ifndef GAMECUBE_DISC_CONTROL_H
#define GAMECUBE_DISC_CONTROL_H

#include "gamecube/menu.h"

/* These delays belong to the local drive fixture. Native physical reads
 * complete asynchronously; neither duration claims hardware I/O timing. */
enum {
    GC_DISC_CONTROL_CLOSING_TICKS = 20,
    GC_DISC_CONTROL_READING_TICKS = 120,
    GC_DISC_CONTROL_TEXT_NONE = 5
};

typedef enum {
    GC_DISC_CONTROL_IDLE,
    GC_DISC_CONTROL_CLOSING,
    GC_DISC_CONTROL_READING
} GcDiscControlPhase;

typedef struct {
    gc_disc_status status;
    GcDiscControlPhase phase;
    gc_disc_status read_result;
    uint16_t remaining_ticks;
    bool media_present;
} GcDiscControl;

/* State is plain data, including pending work, for pause and rewind snapshots.
 * Initial read errors remain the result when that same media is reinserted.
 * Invalid arguments leave the supplied state untouched. */
bool gc_disc_control_init(GcDiscControl *state, gc_disc_status status);
/* Eject immediately, including during a pending read, or begin insertion. */
bool gc_disc_control_toggle(GcDiscControl *state);
bool gc_disc_control_advance(GcDiscControl *state, uint64_t video_ticks);
/* Original DISC STH entry, not the reordered native fader-array position. */
unsigned gc_disc_control_text_index(gc_disc_status status);

#endif
