#ifndef GAMECUBE_STATE_H
#define GAMECUBE_STATE_H

#include "gamecube/menu.h"

typedef enum {
    GC_STATE_OK,
    GC_STATE_MISSING,
    GC_STATE_ARGUMENT,
    GC_STATE_IO,
    GC_STATE_FORMAT,
    GC_STATE_REGION
} gc_state_result;

/* The caller chooses a private Files path and supplies UTC Unix time in seconds. */
gc_state_result gc_state_load(gc_menu *menu, const char *path,
                              int64_t now_unix_seconds);
gc_state_result gc_state_save(gc_menu *menu, const char *path,
                              int64_t now_unix_seconds);
const char *gc_state_result_text(gc_state_result result);

#endif
