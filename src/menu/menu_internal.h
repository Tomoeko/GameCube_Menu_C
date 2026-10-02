#ifndef GAMECUBE_MENU_INTERNAL_H
#define GAMECUBE_MENU_INTERNAL_H

#include "gamecube/menu.h"

/* Page controllers share the logical menu owner; changing a page restarts
 * its presentation clock without changing the separately advanced RTC.
 */
void gc_menu_enter_page(gc_menu *menu, gc_page page);
void gc_menu_calendar_press(gc_menu *menu, gc_button button);
void gc_menu_select_present_card(gc_menu *menu);
void gc_menu_cards_press(gc_menu *menu, gc_button button);
void gc_menu_card_action_press(gc_menu *menu, gc_button button);
void gc_menu_card_confirm_press(gc_menu *menu, gc_button button);

#endif
