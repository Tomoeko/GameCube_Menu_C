#include "gamecube/page_transition.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_delayed_layers(void) {
    GcPageTransitionStyle style = {{0, 20, 70}, 20};
    GcPageTransitions state = {0};
    gc_menu menu;
    gc_menu_init(&menu, GC_REGION_USA);
    menu.page = GC_PAGE_OPTIONS;
    assert(gc_page_transitions_advance(&style, &state, &menu, 1));
    assert(gc_page_transition_alpha(&style, &state, GC_TRANSITION_OPTIONS,
                                    GC_TRANSITION_VALUES) == 12);
    assert(gc_page_transition_alpha(&style, &state, GC_TRANSITION_OPTIONS,
                                    GC_TRANSITION_GRID) == 0);
    assert(gc_page_transition_visible(&style, &state, GC_TRANSITION_OPTIONS));
    assert(gc_page_transitions_advance(&style, &state, &menu, 19));
    assert(gc_page_transition_alpha(&style, &state, GC_TRANSITION_OPTIONS,
                                    GC_TRANSITION_VALUES) == 255);
    assert(gc_page_transition_alpha(&style, &state, GC_TRANSITION_OPTIONS,
                                    GC_TRANSITION_GRID) == 0);
    assert(gc_page_transitions_advance(&style, &state, &menu, 1));
    assert(gc_page_transition_alpha(&style, &state, GC_TRANSITION_OPTIONS,
                                    GC_TRANSITION_GRID) == 12);
    assert(gc_page_transitions_advance(&style, &state, &menu, 49));
    assert(gc_page_transition_alpha(&style, &state, GC_TRANSITION_OPTIONS,
                                    GC_TRANSITION_GRID) == 255);
    assert(gc_page_transition_alpha(&style, &state, GC_TRANSITION_OPTIONS,
                                    GC_TRANSITION_TEXT) == 0);
    assert(gc_page_transitions_advance(&style, &state, &menu, 1));
    assert(gc_page_transition_alpha(&style, &state, GC_TRANSITION_OPTIONS,
                                    GC_TRANSITION_TEXT) == 12);
    assert(gc_page_transitions_advance(&style, &state, &menu, UINT64_MAX));
    assert(gc_page_transition_alpha(&style, &state, GC_TRANSITION_OPTIONS,
                                    GC_TRANSITION_TEXT) == 255);
    menu.page = GC_PAGE_FACE;
    assert(gc_page_transitions_advance(&style, &state, &menu, 1));
    for (unsigned channel = 0; channel < GC_TRANSITION_CHANNEL_COUNT; ++channel)
        assert(gc_page_transition_alpha(&style, &state, GC_TRANSITION_OPTIONS,
                                        (GcTransitionChannel)channel) == 242);
    menu.page = GC_PAGE_CALENDAR;
    assert(gc_page_transitions_advance(&style, &state, &menu, 1));
    assert(gc_page_transition_alpha(&style, &state, GC_TRANSITION_OPTIONS,
                                    GC_TRANSITION_TEXT) == 229);
    assert(gc_page_transition_alpha(&style, &state, GC_TRANSITION_CALENDAR,
                                    GC_TRANSITION_VALUES) == 12);
    assert(gc_page_transitions_advance(&style, &state, &menu, 18));
    assert(!gc_page_transition_visible(&style, &state, GC_TRANSITION_OPTIONS));
    assert(state.counters[GC_TRANSITION_OPTIONS][GC_TRANSITION_TEXT] == 70);
    assert(gc_page_transitions_advance(&style, &state, &menu, 1));
    assert(state.counters[GC_TRANSITION_OPTIONS][GC_TRANSITION_TEXT] == 0);
    GcPageTransitions saved = state;
    GcPageTransitionStyle invalid = style;
    invalid.duration = 0;
    assert(!gc_page_transitions_advance(&invalid, &state, &menu, 1));
    assert(memcmp(&saved, &state, sizeof(state)) == 0);
    assert(gc_page_transition_alpha(&style, &state, GC_TRANSITION_GROUP_COUNT,
                                    GC_TRANSITION_VALUES) == 0);
    gc_page_transitions_init(&state);
    assert(!gc_page_transition_visible(&style, &state, GC_TRANSITION_CALENDAR));
    menu.page = GC_PAGE_MESSAGE;
    menu.message_return_page = GC_PAGE_CARDS;
    assert(gc_page_transition_group(&menu) == GC_TRANSITION_CARD);
    menu.page = GC_PAGE_CARD_CONFIRM;
    assert(gc_page_transition_group(&menu) == GC_TRANSITION_CARD);
}

static void test_original_rom(const char *path) {
    GcText text = {0};
    GcPageTransitionStyle style;
    assert(gc_text_load(path, &text));
    assert(gc_page_transition_style_decode(&text, &style));
    assert(style.duration == 20 && style.delay[0] == 0 && style.delay[1] == 20 &&
           style.delay[2] == 70);
    GcPageTransitionStyle saved = style;
    text.rom_size = 128;
    assert(!gc_page_transition_style_decode(&text, &style));
    assert(memcmp(&saved, &style, sizeof(style)) == 0);
    gc_text_destroy(&text);
}

int main(int argc, char **argv) {
    test_delayed_layers();
    for (int index = 1; index < argc; ++index)
        test_original_rom(argv[index]);
    puts("Native delayed page layers and retained fade-out tests passed.");
    return 0;
}
