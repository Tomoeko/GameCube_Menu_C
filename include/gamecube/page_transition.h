#ifndef GAMECUBE_PAGE_TRANSITION_H
#define GAMECUBE_PAGE_TRANSITION_H

#include "gamecube/text.h"

typedef enum {
    GC_TRANSITION_CARD,
    GC_TRANSITION_DISC,
    GC_TRANSITION_OPTIONS,
    GC_TRANSITION_CALENDAR,
    GC_TRANSITION_GROUP_COUNT
} GcTransitionGroup;

typedef enum {
    GC_TRANSITION_VALUES,
    GC_TRANSITION_GRID,
    GC_TRANSITION_TEXT,
    GC_TRANSITION_CHANNEL_COUNT
} GcTransitionChannel;

typedef struct {
    uint16_t delay[GC_TRANSITION_CHANNEL_COUNT];
    uint16_t duration;
} GcPageTransitionStyle;

typedef struct {
    uint16_t counters[GC_TRANSITION_GROUP_COUNT][GC_TRANSITION_CHANNEL_COUNT];
} GcPageTransitions;

bool gc_page_transition_style_decode(const GcText *text, GcPageTransitionStyle *style);
void gc_page_transitions_init(GcPageTransitions *state);
/* Returns -1 for a cube/face/startup page. Card dialogs retain their group. */
int gc_page_transition_group(const gc_menu *menu);
bool gc_page_transitions_advance(const GcPageTransitionStyle *style,
                                 GcPageTransitions *state, const gc_menu *menu,
                                 uint64_t ticks);
uint8_t gc_page_transition_alpha(const GcPageTransitionStyle *style,
                                 const GcPageTransitions *state,
                                 GcTransitionGroup group, GcTransitionChannel channel);
bool gc_page_transition_visible(const GcPageTransitionStyle *style,
                                const GcPageTransitions *state,
                                GcTransitionGroup group);

#endif
