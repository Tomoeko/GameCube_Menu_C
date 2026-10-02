#include "menu_internal.h"

#include <string.h>

static bool card_valid(const gc_card *card) {
    size_t index;
    uint32_t used_blocks = 0;

    if (!card || card->status < GC_CARD_ABSENT || card->status > GC_CARD_DAMAGED ||
        card->file_count > GC_CARD_FILE_LIMIT)
        return false;
    if (card->status != GC_CARD_READY)
        return card->file_count == 0;
    if (!card->capacity_blocks)
        return false;
    for (index = 0; index < card->file_count; ++index) {
        if (!card->files[index].blocks)
            return false;
        used_blocks += card->files[index].blocks;
    }
    return used_blocks <= card->capacity_blocks;
}

uint16_t gc_card_free_blocks(const gc_card *card) {
    size_t index;
    uint32_t used_blocks = 0;

    if (!card || card->status != GC_CARD_READY || card->file_count > GC_CARD_FILE_LIMIT)
        return 0;
    for (index = 0; index < card->file_count; ++index)
        used_blocks += card->files[index].blocks;
    if (used_blocks > card->capacity_blocks)
        return 0;
    return (uint16_t)(card->capacity_blocks - used_blocks);
}

static size_t card_scroll_limit(const gc_card *card) {
    size_t rows = (card->file_count + GC_CARD_COLUMNS - 1) / GC_CARD_COLUMNS;
    return rows > GC_CARD_VISIBLE_ROWS ? rows - GC_CARD_VISIBLE_ROWS : 0;
}

size_t gc_menu_card_first_row(const gc_menu *menu, unsigned slot) {
    if (!menu || slot > 1)
        return 0;
    return slot == menu->card_slot ? menu->card_first_row
                                   : menu->card_cursors[slot].first_row;
}

size_t gc_menu_card_cursor_index(const gc_menu *menu, unsigned slot) {
    if (!menu || slot > 1)
        return 0;
    return slot == menu->card_slot ? menu->card_index : menu->card_cursors[slot].index;
}

static void store_card_cursor(gc_menu *menu) {
    menu->card_cursors[menu->card_slot] =
        (gc_card_cursor){menu->card_index, menu->card_first_row};
}

static void normalize_card_cursor_slot(gc_menu *menu, unsigned slot) {
    gc_card_cursor cursor = {gc_menu_card_cursor_index(menu, slot),
                             gc_menu_card_first_row(menu, slot)};
    size_t row = cursor.index / GC_CARD_COLUMNS;
    size_t visible_row = row >= cursor.first_row ? row - cursor.first_row : 0;
    if (visible_row >= GC_CARD_VISIBLE_ROWS)
        visible_row = GC_CARD_VISIBLE_ROWS - 1;
    size_t limit = card_scroll_limit(&menu->cards[slot]);
    if (cursor.first_row > limit)
        cursor.first_row = limit;
    cursor.index = (cursor.first_row + visible_row) * GC_CARD_COLUMNS +
                   cursor.index % GC_CARD_COLUMNS;
    if (cursor.index >= GC_CARD_FILE_LIMIT)
        cursor.index = GC_CARD_FILE_LIMIT - 1;
    menu->card_cursors[slot] = cursor;
    if (slot == menu->card_slot) {
        menu->card_index = cursor.index;
        menu->card_first_row = cursor.first_row;
    }
}

static void normalize_card_cursor(gc_menu *menu) {
    normalize_card_cursor_slot(menu, menu->card_slot);
}

static void select_card_slot(gc_menu *menu, unsigned slot) {
    store_card_cursor(menu);
    menu->card_slot = slot;
    menu->card_index = menu->card_cursors[slot].index;
    menu->card_first_row = menu->card_cursors[slot].first_row;
}

void gc_menu_select_present_card(gc_menu *menu) {
    unsigned other = menu->card_slot ^ 1u;
    if (menu->cards[menu->card_slot].status == GC_CARD_ABSENT &&
        menu->cards[other].status == GC_CARD_READY)
        select_card_slot(menu, other);
}

bool gc_menu_set_card(gc_menu *menu, unsigned slot, const gc_card *card) {
    size_t index;

    if (!menu || slot >= 2 || !card_valid(card))
        return false;
    menu->cards[slot] = *card;
    for (index = 0; index < card->file_count; ++index) {
        gc_card_file *file = &menu->cards[slot].files[index];
        file->filename[sizeof(file->filename) - 1] = '\0';
        file->title[sizeof(file->title) - 1] = '\0';
        file->comment[sizeof(file->comment) - 1] = '\0';
    }
    normalize_card_cursor_slot(menu, slot);
    if (menu->page == GC_PAGE_CARD_ACTION || menu->page == GC_PAGE_CARD_CONFIRM)
        gc_menu_enter_page(menu, GC_PAGE_CARDS);
    if (menu->page == GC_PAGE_CARDS)
        gc_menu_select_present_card(menu);
    return true;
}

const gc_card_file *gc_menu_card_selected(const gc_menu *menu) {
    const gc_card *card;

    if (!menu || menu->card_slot >= 2)
        return NULL;
    card = &menu->cards[menu->card_slot];
    if (card->status != GC_CARD_READY || menu->card_index >= card->file_count)
        return NULL;
    return &card->files[menu->card_index];
}

static void show_message(gc_menu *menu, gc_message message, gc_page return_page) {
    menu->message = message;
    menu->message_return_page = return_page;
    gc_menu_enter_page(menu, GC_PAGE_MESSAGE);
}

static bool same_file(const gc_card_file *left, const gc_card_file *right) {
    return memcmp(left->game_code, right->game_code, sizeof(left->game_code)) == 0 &&
           memcmp(left->maker_code, right->maker_code, sizeof(left->maker_code)) == 0 &&
           strncmp(left->filename, right->filename, sizeof(left->filename)) == 0;
}

static gc_message transfer_error(const gc_menu *menu, gc_card_action action) {
    const gc_card_file *file = gc_menu_card_selected(menu);
    const gc_card *target = &menu->cards[1 - menu->card_slot];
    size_t index;

    if (!file)
        return GC_MESSAGE_CARD_ABSENT;
    if (action == GC_CARD_ACTION_COPY && !file->allow_copy)
        return GC_MESSAGE_CARD_COPY_FORBIDDEN;
    if (action == GC_CARD_ACTION_MOVE && !file->allow_move)
        return GC_MESSAGE_CARD_MOVE_FORBIDDEN;
    if (target->status == GC_CARD_ABSENT)
        return GC_MESSAGE_CARD_ABSENT;
    if (target->status != GC_CARD_READY)
        return GC_MESSAGE_CARD_DAMAGED;
    if (target->file_count >= GC_CARD_FILE_LIMIT ||
        gc_card_free_blocks(target) < file->blocks)
        return GC_MESSAGE_CARD_NO_SPACE;
    for (index = 0; index < target->file_count; ++index) {
        if (same_file(file, &target->files[index]))
            return GC_MESSAGE_CARD_FILE_EXISTS;
    }
    return GC_MESSAGE_NONE;
}

static void change_card_action(gc_menu *menu, int direction) {
    int action = (int)menu->card_action + direction;
    /* USA 0x81315e50 and PAL 0x81316bac keep all three rows selectable.
     * A validates the transfer; direction changes wrap independently. */
    if (action < GC_CARD_ACTION_MOVE)
        action = GC_CARD_ACTION_ERASE;
    if (action > GC_CARD_ACTION_ERASE)
        action = GC_CARD_ACTION_MOVE;
    menu->card_window_action = menu->card_action = (gc_card_action)action;
}

static void remove_card_file(gc_menu *menu) {
    gc_card *card = &menu->cards[menu->card_slot];
    size_t index;

    for (index = menu->card_index + 1; index < card->file_count; ++index)
        card->files[index - 1] = card->files[index];
    --card->file_count;
    memset(&card->files[card->file_count], 0, sizeof(card->files[card->file_count]));
    menu->cards_changed[menu->card_slot] = true;
    normalize_card_cursor(menu);
}

static void perform_card_action(gc_menu *menu) {
    gc_card *card = &menu->cards[menu->card_slot];
    gc_card *target = &menu->cards[1 - menu->card_slot];
    const gc_card_file *file = gc_menu_card_selected(menu);
    gc_message error;

    if (menu->card_action == GC_CARD_ACTION_FORMAT) {
        if (card->status == GC_CARD_ABSENT || !card->capacity_blocks) {
            show_message(menu, GC_MESSAGE_CARD_ABSENT, GC_PAGE_CARDS);
            return;
        }
        card->status = GC_CARD_READY;
        card->file_count = 0;
        memset(card->files, 0, sizeof(card->files));
        menu->cards_changed[menu->card_slot] = true;
        normalize_card_cursor(menu);
        show_message(menu, GC_MESSAGE_CARD_FORMATTED, GC_PAGE_CARDS);
        return;
    }
    if (!file) {
        show_message(menu, GC_MESSAGE_CARD_ABSENT, GC_PAGE_CARDS);
        return;
    }
    if (menu->card_action == GC_CARD_ACTION_ERASE) {
        remove_card_file(menu);
        show_message(menu, GC_MESSAGE_CARD_ERASED, GC_PAGE_CARDS);
        return;
    }
    error = transfer_error(menu, menu->card_action);
    if (error != GC_MESSAGE_NONE) {
        show_message(menu, error, GC_PAGE_CARDS);
        return;
    }
    target->files[target->file_count++] = *file;
    menu->cards_changed[1 - menu->card_slot] = true;
    if (menu->card_action == GC_CARD_ACTION_MOVE) {
        remove_card_file(menu);
        show_message(menu, GC_MESSAGE_CARD_MOVED, GC_PAGE_CARDS);
    } else {
        show_message(menu, GC_MESSAGE_CARD_COPIED, GC_PAGE_CARDS);
    }
}

static void card_cursor_cross(gc_menu *menu, unsigned target, size_t column,
                              size_t visible_row) {
    if (menu->cards[target].status != GC_CARD_READY)
        return;
    size_t first_row = gc_menu_card_first_row(menu, target);
    size_t index = (first_row + visible_row) * GC_CARD_COLUMNS + column;
    if (index >= GC_CARD_FILE_LIMIT)
        return;
    store_card_cursor(menu);
    menu->card_slot = target;
    menu->card_index = index;
    menu->card_first_row = first_row;
}

static void move_card_cursor(gc_menu *menu, gc_button button) {
    if (menu->cards[menu->card_slot].status != GC_CARD_READY)
        return;
    normalize_card_cursor(menu);
    size_t column = menu->card_index % GC_CARD_COLUMNS;
    size_t visible_row = menu->card_index / GC_CARD_COLUMNS - menu->card_first_row;
    /* USA 0x813157a0 navigates the visible 4x4 cells, including empty ones.
     * Only directory occupancy limits scrolling; storage capacity does not. */
    if (button == GC_BUTTON_UP) {
        if (visible_row)
            menu->card_index -= GC_CARD_COLUMNS;
        else if (menu->card_first_row) {
            --menu->card_first_row;
            menu->card_index -= GC_CARD_COLUMNS;
        }
    } else if (button == GC_BUTTON_DOWN &&
               menu->card_index + GC_CARD_COLUMNS < GC_CARD_FILE_LIMIT) {
        if (visible_row < GC_CARD_VISIBLE_ROWS - 1)
            menu->card_index += GC_CARD_COLUMNS;
        else if (menu->card_first_row <
                 card_scroll_limit(&menu->cards[menu->card_slot])) {
            ++menu->card_first_row;
            menu->card_index += GC_CARD_COLUMNS;
        }
    } else if (button == GC_BUTTON_LEFT) {
        if (column)
            --menu->card_index;
        else if (menu->card_slot == 1)
            card_cursor_cross(menu, 0, GC_CARD_COLUMNS - 1, visible_row);
    } else if (button == GC_BUTTON_RIGHT && menu->card_index + 1 < GC_CARD_FILE_LIMIT) {
        if (column < GC_CARD_COLUMNS - 1)
            ++menu->card_index;
        else if (menu->card_slot == 0)
            card_cursor_cross(menu, 1, 0, visible_row);
    }
    store_card_cursor(menu);
}

void gc_menu_cards_press(gc_menu *menu, gc_button button) {
    const gc_card *card = &menu->cards[menu->card_slot];

    if (button == GC_BUTTON_CANCEL) {
        gc_menu_enter_page(menu, GC_PAGE_FACE);
    } else if (button == GC_BUTTON_CONFIRM) {
        if (card->status == GC_CARD_UNFORMATTED) {
            menu->card_action = GC_CARD_ACTION_FORMAT;
            menu->confirm_yes = false;
            menu->format_second_confirmation = false;
            gc_menu_enter_page(menu, GC_PAGE_CARD_CONFIRM);
        } else if (card->status == GC_CARD_DAMAGED) {
            show_message(menu, GC_MESSAGE_CARD_DAMAGED, GC_PAGE_CARDS);
        } else if (gc_menu_card_selected(menu)) {
            menu->card_action = menu->card_window_action;
            gc_menu_enter_page(menu, GC_PAGE_CARD_ACTION);
        }
    } else
        move_card_cursor(menu, button);
}

void gc_menu_card_action_press(gc_menu *menu, gc_button button) {
    gc_message error;

    if (button == GC_BUTTON_CANCEL) {
        gc_menu_enter_page(menu, GC_PAGE_CARDS);
    } else if (button == GC_BUTTON_UP) {
        change_card_action(menu, -1);
    } else if (button == GC_BUTTON_DOWN) {
        change_card_action(menu, 1);
    } else if (button == GC_BUTTON_CONFIRM) {
        if (menu->card_action != GC_CARD_ACTION_ERASE) {
            error = transfer_error(menu, menu->card_action);
            if (error != GC_MESSAGE_NONE) {
                show_message(menu, error, GC_PAGE_CARDS);
                return;
            }
        }
        /* Native action confirmation starts on No for every command. */
        menu->confirm_yes = false;
        gc_menu_enter_page(menu, GC_PAGE_CARD_CONFIRM);
    }
}

void gc_menu_card_confirm_press(gc_menu *menu, gc_button button) {
    if (button == GC_BUTTON_CANCEL) {
        gc_menu_enter_page(menu, menu->card_action == GC_CARD_ACTION_FORMAT
                                     ? GC_PAGE_CARDS
                                     : GC_PAGE_CARD_ACTION);
    } else if (button == GC_BUTTON_UP) {
        menu->confirm_yes = true;
    } else if (button == GC_BUTTON_DOWN) {
        menu->confirm_yes = false;
    } else if (button == GC_BUTTON_CONFIRM) {
        if (menu->confirm_yes && menu->card_action == GC_CARD_ACTION_FORMAT &&
            !menu->format_second_confirmation) {
            menu->format_second_confirmation = true;
            menu->confirm_yes = false;
            menu->page_elapsed = 0.0;
        } else if (menu->confirm_yes) {
            perform_card_action(menu);
        } else {
            gc_menu_enter_page(menu, menu->card_action == GC_CARD_ACTION_FORMAT
                                         ? GC_PAGE_CARDS
                                         : GC_PAGE_CARD_ACTION);
        }
    }
}
