#include "gamecube/help.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_fades_and_transitions(void) {
    GcHelpState state;
    gc_menu menu;
    gc_help_state_init(&state);
    gc_menu_init(&menu, GC_REGION_USA);
    gc_menu_skip_startup(&menu);
    assert(gc_help_advance(&state, &menu, 1));
    assert(state.labels[0] == 1 && state.icons[3] == 1);
    assert(gc_help_entry_alpha(&state, 0) == 12);
    assert(state.phase == 1 && state.control_intro == 4);
    assert(gc_help_advance(&state, &menu, 19));
    assert(gc_help_entry_alpha(&state, 0) == 255);
    assert(state.icons[3] == 20 && state.icons[0] == 0);
    menu.page = GC_PAGE_FACE;
    assert(gc_help_advance(&state, &menu, 10));
    assert(state.labels[0] == 10 && state.labels[1] == 10);
    assert(state.icons[3] == 10 && state.icons[4] == 10 && state.icons[5] == 10);
    menu.page = GC_PAGE_OPTIONS;
    assert(gc_help_advance(&state, &menu, 20));
    assert(state.labels[4] == 20 && state.labels[11] == 20 && state.labels[6] == 20);
    assert(state.labels[0] == 0 && state.labels[1] == 0);
    assert(state.icons[0] == 20 && state.icons[1] == 20 && state.icons[2] == 20);
    menu.editing = true;
    assert(gc_help_advance(&state, &menu, 10));
    assert(state.labels[4] == 10 && state.labels[13] == 10);
    assert(state.labels[11] == 10 && state.labels[8] == 10 && state.labels[6] == 20);
    menu.page = GC_PAGE_DISC;
    menu.disc_status = GC_DISC_READY;
    assert(gc_help_advance(&state, &menu, UINT64_MAX));
    assert(state.labels[8] == 20 && state.labels[6] == 0);
    assert(state.icons[1] == 20 && state.icons[0] == 0 && state.icons[2] == 0);
    assert(state.control_intro == 255);
    assert(gc_help_entry_alpha(&state, GC_HELP_ENTRY_LIMIT) == 0);
    GcHelpState saved = state;
    state.icons[0] = 21;
    GcHelpState bad = state;
    assert(!gc_help_advance(&state, &menu, 1));
    assert(memcmp(&state, &bad, sizeof(state)) == 0);
    state = saved;
    menu.page = (gc_page)100;
    assert(!gc_help_advance(&state, &menu, 1));
    assert(memcmp(&state, &saved, sizeof(state)) == 0);
}

static void test_native_rom(const char *path) {
    GcText owner = {0};
    GcLayouts layouts = {0};
    GcHelpStyle style;
    GcHelpState state;
    GcHelpPane pane;
    gc_menu menu;
    assert(gc_text_load(path, &owner));
    assert(gc_layouts_index(&owner, &layouts));
    assert(gc_help_style_decode(&owner, &style));
    assert(style.backgrounds[0] == 0x969696ff);
    assert(style.backgrounds[1] == 0xd20046ff);
    assert(style.backgrounds[2] == 0x00c88cff);
    assert(style.controls[0] == 0xffffffff && style.dots == 0xffffffff);
    const GcLayoutTable *table =
        gc_layout_table(&layouts, GC_LANGUAGE_ENGLISH, GC_LAYOUT_HELP);
    gc_help_state_init(&state);
    gc_menu_init(&menu, GC_REGION_USA);
    menu.page = GC_PAGE_OPTIONS;
    assert(gc_help_advance(&state, &menu, 20));
    for (unsigned index = 0; index < GC_HELP_PANE_DRAWS; ++index)
        assert(gc_help_pane(&style, &state, table, index, 255, &pane));
    state.phase = 0;
    state.control_intro = 255;
    assert(gc_help_pane(&style, &state, table, 0, 255, &pane));
    assert(strcmp(pane.pane.name, "bac1") == 0 && pane.tint == 0x969696ff);
    state.phase = 127;
    assert(gc_help_pane(&style, &state, table, 0, 255, &pane));
    assert((pane.tint & 255) == 100);
    state.phase = 255;
    assert(gc_help_pane(&style, &state, table, 0, 255, &pane));
    assert((pane.tint & 255) == 255);
    state.phase = 0;
    assert(gc_help_pane(&style, &state, table, 6, 255, &pane));
    assert(strcmp(pane.pane.name, "stk4") == 0 && (pane.tint & 255) == 255);
    assert(gc_help_pane(&style, &state, table, 7, 255, &pane));
    assert(strcmp(pane.pane.name, "stk0") == 0 && (pane.tint & 255) == 0);
    state.phase = 127;
    assert(gc_help_pane(&style, &state, table, 7, 255, &pane));
    assert((pane.tint & 255) == 255);
    state.phase = 128;
    assert(gc_help_pane(&style, &state, table, 6, 255, &pane));
    assert(strcmp(pane.pane.name, "stk1") == 0 && (pane.tint & 255) == 0);
    assert(gc_help_pane(&style, &state, table, 10, 255, &pane));
    assert(strcmp(pane.pane.name, "butb") == 0 && (pane.tint & 255) == 255);
    GcHelpPane saved = pane;
    assert(!gc_help_pane(&style, &state, table, 20, 255, &pane));
    assert(memcmp(&pane, &saved, sizeof(pane)) == 0);
    GcHelpStyle saved_style = style;
    owner.rom_size = 10;
    assert(!gc_help_style_decode(&owner, &style));
    assert(memcmp(&style, &saved_style, sizeof(style)) == 0);
    gc_text_destroy(&owner);
}

int main(int argc, char **argv) {
    test_fades_and_transitions();
    for (int index = 1; index < argc; ++index)
        test_native_rom(argv[index]);
    puts("native help state and geometry tests passed");
    return 0;
}
