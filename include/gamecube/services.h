#ifndef GAMECUBE_SERVICES_H
#define GAMECUBE_SERVICES_H

#include "gamecube/card_image.h"

typedef struct {
    gc_card_image cards[2];
    const char *card_paths[2]; /* Borrowed fixed local shadow paths. */
    uint64_t revision;
} GcServices;

/* Imported images are copied to local state before edits are allowed. */
bool gc_services_init(GcServices *services, gc_menu *menu, const char *inputs[2],
                      const char *state_paths[2]);
void gc_services_destroy(GcServices *services);
void gc_services_press(GcServices *services, gc_menu *menu, gc_button button);

#endif
