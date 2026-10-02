#include "gamecube/services.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void gc_services_destroy(GcServices *services) {
    if (!services)
        return;
    for (unsigned slot = 0; slot < 2; slot++)
        gc_card_image_free(&services->cards[slot]);
    *services = (GcServices){0};
}

bool gc_services_init(GcServices *services, gc_menu *menu, const char *inputs[2],
                      const char *state_paths[2]) {
    if (!services || !menu || !inputs || !state_paths)
        return false;
    *services = (GcServices){0};
    for (unsigned slot = 0; slot < 2; slot++) {
        services->card_paths[slot] = state_paths[slot];
        if (!inputs[slot])
            continue;
        if (!state_paths[slot]) {
            gc_services_destroy(services);
            return false;
        }
        gc_card_image_result result =
            gc_card_image_open_present(&services->cards[slot], inputs[slot],
                                       menu->region == GC_REGION_JAPAN ? 1 : 0);
        if (result == GC_CARD_IMAGE_OK && strcmp(inputs[slot], state_paths[slot]))
            result = gc_card_image_write(&services->cards[slot], state_paths[slot]);
        if (result != GC_CARD_IMAGE_OK) {
            fprintf(stderr, "Card %c: %s\n", slot ? 'B' : 'A',
                    gc_card_image_result_text(result));
            gc_services_destroy(services);
            return false;
        }
        gc_menu_set_card(menu, slot, &services->cards[slot].card);
    }
    return true;
}

static void sync_cards(GcServices *services, gc_menu *menu) {
    for (unsigned slot = 0; slot < 2; slot++) {
        if (services->cards[slot].bytes)
            gc_menu_set_card(menu, slot, &services->cards[slot].card);
        menu->cards_changed[slot] = false;
    }
}

void gc_services_press(GcServices *services, gc_menu *menu, gc_button button) {
    if (!services || !menu)
        return;
    unsigned slot = menu->card_slot;
    size_t index = menu->card_index;
    gc_card_action action = menu->card_action;
    gc_menu_press(menu, button);
    if (slot >= 2 || (!menu->cards_changed[0] && !menu->cards_changed[1]))
        return;
    gc_card_image backups[2] = {{0}};
    gc_card_image_result result = GC_CARD_IMAGE_OK;
    for (unsigned backup = 0; backup < 2; backup++) {
        if (!services->cards[backup].bytes)
            continue;
        result = gc_card_image_clone(&backups[backup], &services->cards[backup]);
        if (result != GC_CARD_IMAGE_OK)
            break;
    }
    if (result != GC_CARD_IMAGE_OK)
        goto finish_operation;
    if (action == GC_CARD_ACTION_ERASE)
        result = gc_card_image_erase(&services->cards[slot], index);
    else if (action == GC_CARD_ACTION_FORMAT)
        result = gc_card_image_format(&services->cards[slot]);
    else
        result = gc_card_image_copy(&services->cards[slot], &services->cards[slot ^ 1],
                                    index, action == GC_CARD_ACTION_MOVE);
    if (result == GC_CARD_IMAGE_OK) {
        /* A move saves the complete destination before the source is changed
         * on disk. A failed second write preserves both copies of the save. */
        unsigned first = action <= GC_CARD_ACTION_COPY ? slot ^ 1 : slot;
        result =
            gc_card_image_write(&services->cards[first], services->card_paths[first]);
        bool destination_saved = result == GC_CARD_IMAGE_OK;
        if (destination_saved && action == GC_CARD_ACTION_MOVE)
            result =
                gc_card_image_write(&services->cards[slot], services->card_paths[slot]);
        if (result != GC_CARD_IMAGE_OK) {
            for (unsigned restore = 0; restore < 2; restore++) {
                if (!backups[restore].bytes || (destination_saved && restore != slot))
                    continue;
                gc_card_image_free(&services->cards[restore]);
                services->cards[restore] = backups[restore];
                backups[restore] = (gc_card_image){0};
            }
        }
    }
finish_operation:
    services->revision++;
    for (unsigned backup = 0; backup < 2; backup++)
        gc_card_image_free(&backups[backup]);
    sync_cards(services, menu);
    if (result != GC_CARD_IMAGE_OK) {
        fprintf(stderr, "Card operation failed: %s\n",
                gc_card_image_result_text(result));
        menu->page = GC_PAGE_MESSAGE;
        menu->message_return_page = GC_PAGE_CARDS;
        menu->message =
            result == GC_CARD_IMAGE_NO_SPACE    ? GC_MESSAGE_CARD_NO_SPACE
            : result == GC_CARD_IMAGE_DUPLICATE ? GC_MESSAGE_CARD_FILE_EXISTS
            : result == GC_CARD_IMAGE_PERMISSION
                ? (action == GC_CARD_ACTION_MOVE ? GC_MESSAGE_CARD_MOVE_FORBIDDEN
                                                 : GC_MESSAGE_CARD_COPY_FORBIDDEN)
                : GC_MESSAGE_CARD_SAVE_FAILED;
        menu->page_elapsed = 0;
    }
}
