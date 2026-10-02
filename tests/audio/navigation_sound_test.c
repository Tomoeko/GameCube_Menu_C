#include "gamecube/navigation_sound.h"

#include <assert.h>

int main(void) {
    const gc_button directions[4] = {GC_BUTTON_UP, GC_BUTTON_RIGHT, GC_BUTTON_DOWN,
                                     GC_BUTTON_LEFT};
    const gc_page pages[4] = {GC_PAGE_DISC, GC_PAGE_CALENDAR, GC_PAGE_CARDS,
                              GC_PAGE_OPTIONS};
    gc_menu menu;
    for (unsigned face = 0; face < 4; ++face) {
        for (unsigned confirm = 0; confirm < 2; ++confirm) {
            gc_menu_init(&menu, GC_REGION_EUROPE);
            gc_menu_skip_startup(&menu);
            gc_page previous = menu.page;
            gc_menu_press(&menu, directions[face]);
            assert(gc_navigation_sound(previous, menu.page) == 3);
            previous = menu.page;
            gc_menu_press(&menu, confirm ? GC_BUTTON_CONFIRM : directions[face]);
            assert(menu.page == pages[face]);
            assert(gc_navigation_sound(previous, menu.page) == 5);
            previous = menu.page;
            gc_menu_press(&menu, GC_BUTTON_CANCEL);
            assert(gc_navigation_sound(previous, menu.page) == 6);
            previous = menu.page;
            gc_menu_press(&menu,
                          confirm ? GC_BUTTON_CANCEL : directions[(face + 2) % 4]);
            assert(gc_navigation_sound(previous, menu.page) == 4);
        }
    }
    assert(gc_navigation_sound(GC_PAGE_FACE, GC_PAGE_FACE) == 0);
    assert(gc_navigation_sound(GC_PAGE_OPTIONS, GC_PAGE_OPTIONS) == 0);
    assert(gc_navigation_sound(GC_PAGE_CARDS, GC_PAGE_CARD_ACTION) == 0);
    assert(gc_navigation_sound(GC_PAGE_STARTUP, GC_PAGE_CUBE) == 0);
    return 0;
}
