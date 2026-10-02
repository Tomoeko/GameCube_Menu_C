#include "menu_internal.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

void gc_menu_enter_page(gc_menu *menu, gc_page page) {
    menu->page = page;
    menu->page_elapsed = 0.0;
}

static void copy_text(char *destination, size_t capacity, const char *source) {
    size_t length = 0;

    if (!capacity)
        return;
    if (source) {
        while (length + 1 < capacity && source[length]) {
            destination[length] = source[length];
            ++length;
        }
    }
    destination[length] = '\0';
}

double gc_region_startup_duration(gc_region region) {
    return region == GC_REGION_EUROPE ? 495.0 / 50.0 : 537.0 / 60.0;
}

static double startup_duration(const gc_menu *menu) {
    return isfinite(menu->startup_duration) && menu->startup_duration > 0
               ? menu->startup_duration
               : gc_region_startup_duration(menu->region);
}

void gc_menu_init(gc_menu *menu, gc_region region) {
    if (!menu)
        return;
    memset(menu, 0, sizeof(*menu));
    menu->region = region;
    menu->page = GC_PAGE_STARTUP;
    menu->face = GC_FACE_GAME_PLAY;
    menu->startup_duration = gc_region_startup_duration(region);
    menu->settings.sound = GC_SOUND_STEREO;
    menu->settings.language =
        region == GC_REGION_JAPAN ? GC_LANGUAGE_JAPANESE : GC_LANGUAGE_ENGLISH;
    menu->clock.year = 2000;
    menu->clock.month = 1;
    menu->clock.day = 1;
    menu->card_action = GC_CARD_ACTION_ERASE;
    menu->card_window_action = GC_CARD_ACTION_ERASE;
}

void gc_menu_skip_startup(gc_menu *menu) {
    if (!menu || menu->page != GC_PAGE_STARTUP)
        return;
    menu->startup_elapsed = startup_duration(menu);
    gc_menu_enter_page(menu, GC_PAGE_CUBE);
}

void gc_menu_restart_startup(gc_menu *menu) {
    if (!menu)
        return;
    gc_region region = menu->region;
    double duration = startup_duration(menu);
    gc_settings settings = menu->page == GC_PAGE_OPTIONS && menu->editing
                               ? menu->settings_before_edit
                               : menu->settings;
    gc_date_time clock = menu->page == GC_PAGE_CALENDAR && menu->editing
                             ? menu->clock_before_edit
                             : menu->clock;
    double clock_fraction = menu->clock_fraction;
    /* Card tables and disc metadata remain owned by the live services. */
    memset(menu, 0, offsetof(gc_menu, cards));
    menu->region = region;
    menu->page = GC_PAGE_STARTUP;
    menu->face = GC_FACE_GAME_PLAY;
    menu->startup_duration = duration;
    menu->startup_controlled = true;
    menu->settings = settings;
    menu->clock = clock;
    menu->clock_fraction = clock_fraction;
    menu->card_action = GC_CARD_ACTION_ERASE;
    menu->card_window_action = GC_CARD_ACTION_ERASE;
    menu->launch_requested = false;
}

bool gc_menu_set_settings(gc_menu *menu, const gc_settings *settings) {
    if (!menu || !settings || settings->sound < GC_SOUND_MONO ||
        settings->sound > GC_SOUND_STEREO ||
        settings->screen_position < GC_SCREEN_POSITION_MIN ||
        settings->screen_position > GC_SCREEN_POSITION_MAX ||
        settings->language < GC_LANGUAGE_ENGLISH ||
        settings->language > GC_LANGUAGE_JAPANESE)
        return false;
    menu->settings = *settings;
    return true;
}

void gc_menu_set_disc(gc_menu *menu, gc_disc_status status, const char *title,
                      const char *company) {
    if (!menu || status < GC_DISC_ABSENT || status > GC_DISC_FATAL)
        return;
    menu->disc_status = status;
    copy_text(menu->disc_title, sizeof(menu->disc_title), title);
    copy_text(menu->disc_company, sizeof(menu->disc_company), company);
}

void gc_menu_tick_presentation(gc_menu *menu, double elapsed_seconds) {
    if (!menu || !isfinite(elapsed_seconds) || elapsed_seconds <= 0.0)
        return;
    menu->page_elapsed += elapsed_seconds;
    if (menu->page == GC_PAGE_STARTUP) {
        menu->startup_elapsed += elapsed_seconds;
        if (!menu->startup_controlled &&
            menu->startup_elapsed >= startup_duration(menu))
            gc_menu_skip_startup(menu);
    }
}

void gc_menu_tick(gc_menu *menu, double elapsed_seconds) {
    gc_menu_tick_presentation(menu, elapsed_seconds);
    gc_menu_advance_clock(menu, elapsed_seconds);
}

static void change_option(gc_menu *menu, int direction) {
    gc_settings *settings = &menu->settings;

    switch (menu->editor_index) {
        case 0:
            settings->sound = direction < 0 ? GC_SOUND_MONO : GC_SOUND_STEREO;
            break;
        case 1:
            settings->screen_position += direction;
            if (settings->screen_position < GC_SCREEN_POSITION_MIN)
                settings->screen_position = GC_SCREEN_POSITION_MIN;
            if (settings->screen_position > GC_SCREEN_POSITION_MAX)
                settings->screen_position = GC_SCREEN_POSITION_MAX;
            break;
        case 2:
            if (menu->region == GC_REGION_EUROPE) {
                int next_language = (int)settings->language + direction;
                if (next_language >= GC_LANGUAGE_ENGLISH &&
                    next_language <= GC_LANGUAGE_DUTCH)
                    settings->language = (gc_language)next_language;
            }
            break;
        default:
            break;
    }
}

static void options_press(gc_menu *menu, gc_button button) {
    unsigned last_option = menu->region == GC_REGION_EUROPE ? 2 : 1;

    if (menu->editing) {
        if (button == GC_BUTTON_CANCEL) {
            menu->settings = menu->settings_before_edit;
            menu->editing = false;
        } else if (button == GC_BUTTON_CONFIRM) {
            menu->settings_changed = true;
            menu->editing = false;
        } else if (button == GC_BUTTON_RIGHT) {
            change_option(menu, 1);
        } else if (button == GC_BUTTON_LEFT) {
            change_option(menu, -1);
        }
        return;
    }
    if (button == GC_BUTTON_CANCEL) {
        gc_menu_enter_page(menu, GC_PAGE_FACE);
    } else if (button == GC_BUTTON_CONFIRM) {
        menu->settings_before_edit = menu->settings;
        menu->editing = true;
    } else if (button == GC_BUTTON_UP && menu->editor_index) {
        --menu->editor_index;
    } else if (button == GC_BUTTON_DOWN && menu->editor_index < last_option) {
        ++menu->editor_index;
    }
}

static void open_face(gc_menu *menu) {
    menu->editor_index = 0;
    menu->editing = false;
    switch (menu->face) {
        case GC_FACE_GAME_PLAY:
            /* Native mode2 remains on the full Disc page while the drive
             * status changes; its own word/banner faders present the status. */
            gc_menu_enter_page(menu, GC_PAGE_DISC);
            break;
        case GC_FACE_CALENDAR:
            /* USA 0x81327660 initializes the independent logical cursors to
             * Year and Hour. Only Date/Time rows navigate before editing. */
            menu->calendar_date_index = menu->region == GC_REGION_JAPAN ? 0 : 2;
            menu->calendar_time_index = 3;
            menu->editor_index = menu->calendar_date_index;
            gc_menu_enter_page(menu, GC_PAGE_CALENDAR);
            break;
        case GC_FACE_MEMORY_CARD:
            gc_menu_select_present_card(menu);
            gc_menu_enter_page(menu, GC_PAGE_CARDS);
            break;
        case GC_FACE_OPTIONS:
            gc_menu_enter_page(menu, GC_PAGE_OPTIONS);
            break;
    }
}

static bool direction_face(gc_button button, gc_face *face) {
    switch (button) {
        case GC_BUTTON_UP:
            *face = GC_FACE_GAME_PLAY;
            break;
        case GC_BUTTON_RIGHT:
            *face = GC_FACE_CALENDAR;
            break;
        case GC_BUTTON_DOWN:
            *face = GC_FACE_MEMORY_CARD;
            break;
        case GC_BUTTON_LEFT:
            *face = GC_FACE_OPTIONS;
            break;
        default:
            return false;
    }
    return true;
}

static void cube_press(gc_menu *menu, gc_button button) {
    gc_face direction;
    if (!direction_face(button, &direction))
        return;
    menu->face = direction;
    gc_menu_enter_page(menu, GC_PAGE_FACE);
}

static void face_press(gc_menu *menu, gc_button button) {
    gc_face direction;
    if (button == GC_BUTTON_CANCEL) {
        gc_menu_enter_page(menu, GC_PAGE_CUBE);
    } else if (button == GC_BUTTON_CONFIRM) {
        open_face(menu);
    } else if (direction_face(button, &direction)) {
        if (direction == menu->face)
            open_face(menu);
        else if (((unsigned)direction + 2) % 4 == (unsigned)menu->face)
            gc_menu_enter_page(menu, GC_PAGE_CUBE);
    }
}

void gc_menu_press(gc_menu *menu, gc_button button) {
    if (!menu || button < GC_BUTTON_UP || button > GC_BUTTON_START)
        return;
    if (menu->disc_status == GC_DISC_FATAL)
        return;
    switch (menu->page) {
        case GC_PAGE_STARTUP:
            if (!menu->startup_controlled &&
                (button == GC_BUTTON_CONFIRM || button == GC_BUTTON_START))
                gc_menu_skip_startup(menu);
            break;
        case GC_PAGE_CUBE:
            cube_press(menu, button);
            break;
        case GC_PAGE_FACE:
            face_press(menu, button);
            break;
        case GC_PAGE_CALENDAR:
            gc_menu_calendar_press(menu, button);
            break;
        case GC_PAGE_OPTIONS:
            options_press(menu, button);
            break;
        case GC_PAGE_CARDS:
            gc_menu_cards_press(menu, button);
            break;
        case GC_PAGE_CARD_ACTION:
            gc_menu_card_action_press(menu, button);
            break;
        case GC_PAGE_CARD_CONFIRM:
            gc_menu_card_confirm_press(menu, button);
            break;
        case GC_PAGE_MESSAGE:
            if (button == GC_BUTTON_CONFIRM || button == GC_BUTTON_CANCEL) {
                menu->message = GC_MESSAGE_NONE;
                gc_menu_enter_page(menu, menu->message_return_page);
            }
            break;
        case GC_PAGE_DISC:
            if (button == GC_BUTTON_CANCEL) {
                gc_menu_enter_page(menu, GC_PAGE_FACE);
            } else if (button == GC_BUTTON_START &&
                       menu->disc_status == GC_DISC_READY) {
                menu->launch_requested = true;
                gc_menu_enter_page(menu, GC_PAGE_GAME_STARTED);
            }
            break;
        case GC_PAGE_GAME_STARTED:
            if (button == GC_BUTTON_CANCEL) {
                menu->launch_requested = false;
                gc_menu_enter_page(menu, GC_PAGE_CUBE);
            }
            break;
    }
}

const char *gc_menu_face_name(gc_face face) {
    switch (face) {
        case GC_FACE_GAME_PLAY:
            return "Game Play";
        case GC_FACE_CALENDAR:
            return "Calendar";
        case GC_FACE_MEMORY_CARD:
            return "Memory Card";
        case GC_FACE_OPTIONS:
            return "Options";
    }
    return "";
}

const char *gc_menu_language_name(gc_language language) {
    switch (language) {
        case GC_LANGUAGE_ENGLISH:
            return "English";
        case GC_LANGUAGE_GERMAN:
            return "Deutsch";
        case GC_LANGUAGE_FRENCH:
            return "Francais";
        case GC_LANGUAGE_SPANISH:
            return "Espanol";
        case GC_LANGUAGE_ITALIAN:
            return "Italiano";
        case GC_LANGUAGE_DUTCH:
            return "Nederlands";
        case GC_LANGUAGE_JAPANESE:
            return "Japanese";
    }
    return "";
}

const char *gc_menu_message_text(gc_message message) {
    switch (message) {
        case GC_MESSAGE_CARD_SAVE_FAILED:
            return "The Memory Card data could not be saved.";
        case GC_MESSAGE_NONE:
            return "";
        case GC_MESSAGE_CARD_ABSENT:
            return "Nothing is inserted in the Memory Card slot.";
        case GC_MESSAGE_CARD_DAMAGED:
            return "The Memory Card cannot be used.";
        case GC_MESSAGE_CARD_NO_SPACE:
            return "There is not enough space on the Memory Card.";
        case GC_MESSAGE_CARD_FILE_EXISTS:
            return "The same file already exists on the other Memory Card.";
        case GC_MESSAGE_CARD_COPY_FORBIDDEN:
            return "This file cannot be copied.";
        case GC_MESSAGE_CARD_MOVE_FORBIDDEN:
            return "This file cannot be moved.";
        case GC_MESSAGE_CARD_COPIED:
            return "The data has been copied.";
        case GC_MESSAGE_CARD_MOVED:
            return "The data has been moved.";
        case GC_MESSAGE_CARD_ERASED:
            return "The data has been erased.";
        case GC_MESSAGE_CARD_FORMATTED:
            return "The Memory Card has been formatted.";
        case GC_MESSAGE_DISC_ABSENT:
            return "Please insert a GAMECUBE Disc.";
        case GC_MESSAGE_DISC_UNREADABLE:
            return "The Disc could not be read.";
        case GC_MESSAGE_DISC_LID_OPEN:
            return "Please close the Disc Cover.";
    }
    return "";
}
