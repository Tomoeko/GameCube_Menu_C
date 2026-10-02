#include "gamecube/page_transition.h"
#include "native_constants.h"

#include <string.h>

static bool valid_style(const GcPageTransitionStyle *style) {
    if (!style || !style->duration || style->duration > 1000)
        return false;
    for (unsigned index = 0; index < GC_TRANSITION_CHANNEL_COUNT; ++index)
        if (style->delay[index] > 1000)
            return false;
    return true;
}

bool gc_page_transition_style_decode(const GcText *text, GcPageTransitionStyle *style) {
    GcPageTransitionStyle candidate;
    if (!text || !style ||
        !gc_native_halfwords(text, text->europe ? 0x11c84 : 0x111cc,
                             text->europe ? 0x12078 : 0x11588, 31,
                             text->europe ? 0x1b0 : 0x178, GC_TRANSITION_CHANNEL_COUNT,
                             candidate.delay) ||
        !gc_native_halfwords(text, text->europe ? 0xb30c : 0xb4e8,
                             text->europe ? 0xb334 : 0xb510, 3, 2, 1,
                             &candidate.duration) ||
        !valid_style(&candidate))
        return false;
    *style = candidate;
    return true;
}

void gc_page_transitions_init(GcPageTransitions *state) {
    if (state)
        memset(state, 0, sizeof(*state));
}

int gc_page_transition_group(const gc_menu *menu) {
    if (!menu)
        return -1;
    switch (menu->page) {
        case GC_PAGE_CARD_ACTION:
        case GC_PAGE_CARD_CONFIRM:
        case GC_PAGE_CARDS:
            return GC_TRANSITION_CARD;
        case GC_PAGE_MESSAGE:
            return menu->message_return_page == GC_PAGE_CARDS ? GC_TRANSITION_CARD : -1;
        case GC_PAGE_DISC:
            return GC_TRANSITION_DISC;
        case GC_PAGE_OPTIONS:
            return GC_TRANSITION_OPTIONS;
        case GC_PAGE_CALENDAR:
            return GC_TRANSITION_CALENDAR;
        default:
            return -1;
    }
}

bool gc_page_transitions_advance(const GcPageTransitionStyle *style,
                                 GcPageTransitions *state, const gc_menu *menu,
                                 uint64_t ticks) {
    if (!valid_style(style) || !state || !menu)
        return false;
    for (unsigned group = 0; group < GC_TRANSITION_GROUP_COUNT; ++group)
        for (unsigned channel = 0; channel < GC_TRANSITION_CHANNEL_COUNT; ++channel)
            if (state->counters[group][channel] >
                style->delay[channel] + style->duration)
                return false;
    int selected = gc_page_transition_group(menu);
    for (unsigned group = 0; group < GC_TRANSITION_GROUP_COUNT; ++group)
        for (unsigned channel = 0; channel < GC_TRANSITION_CHANNEL_COUNT; ++channel) {
            unsigned counter = state->counters[group][channel];
            unsigned delay = style->delay[channel];
            unsigned limit = delay + style->duration;
            /* USA 0x81311f60 calls 0xaa58 on all four groups each video tick.
             * Fade-out counters snap to zero once they cross their delay. */
            if ((int)group == selected)
                counter = ticks >= limit - counter ? limit : counter + (unsigned)ticks;
            else
                counter = ticks >= counter || counter - (unsigned)ticks < delay
                              ? 0
                              : counter - (unsigned)ticks;
            state->counters[group][channel] = (uint16_t)counter;
        }
    return true;
}

uint8_t gc_page_transition_alpha(const GcPageTransitionStyle *style,
                                 const GcPageTransitions *state,
                                 GcTransitionGroup group, GcTransitionChannel channel) {
    if (!valid_style(style) || !state || (unsigned)group >= GC_TRANSITION_GROUP_COUNT ||
        (unsigned)channel >= GC_TRANSITION_CHANNEL_COUNT)
        return 0;
    unsigned counter = state->counters[group][channel];
    unsigned delay = style->delay[channel];
    if (counter <= delay || counter > delay + style->duration)
        return 0;
    return (uint8_t)((counter - delay) * 255 / style->duration);
}

bool gc_page_transition_visible(const GcPageTransitionStyle *style,
                                const GcPageTransitions *state,
                                GcTransitionGroup group) {
    for (unsigned channel = 0; channel < GC_TRANSITION_CHANNEL_COUNT; ++channel)
        if (gc_page_transition_alpha(style, state, group, (GcTransitionChannel)channel))
            return true;
    return false;
}
