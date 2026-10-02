#ifndef GAMECUBE_CARD_USAGE_H
#define GAMECUBE_CARD_USAGE_H

#include "gamecube/menu.h"

/* The last measured usage survives removal while its fill fades away. */
typedef struct {
    uint8_t alpha[2];
    uint8_t level[2];
} GcCardUsage;

/* Zero initializes both slots. Advance on every native menu video tick,
 * including ticks spent on other faces and full pages. Invalid metadata
 * preserves the state; zero ticks do not refresh the measured usage. */
bool gc_card_usage_advance(GcCardUsage *state, const gc_card cards[2], uint64_t ticks);
uint8_t gc_card_usage_alpha(const GcCardUsage *state, unsigned slot,
                            uint8_t face_alpha);
/* Native 0..255 height ratio, after the slot's availability animation. */
uint8_t gc_card_usage_fill(const GcCardUsage *state, unsigned slot);

#endif
