#ifndef GAMECUBE_MENU_H
#define GAMECUBE_MENU_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GC_CARD_FILE_LIMIT 127
#define GC_CARD_COLUMNS 4
#define GC_CARD_VISIBLE_ROWS 4
#define GC_SCREEN_POSITION_MIN (-32)
#define GC_SCREEN_POSITION_MAX 32

typedef enum { GC_REGION_JAPAN, GC_REGION_USA, GC_REGION_EUROPE } gc_region;

typedef enum {
    GC_PAGE_STARTUP,
    GC_PAGE_CUBE,
    GC_PAGE_FACE,
    GC_PAGE_CALENDAR,
    GC_PAGE_OPTIONS,
    GC_PAGE_CARDS,
    GC_PAGE_CARD_ACTION,
    GC_PAGE_CARD_CONFIRM,
    GC_PAGE_MESSAGE,
    GC_PAGE_DISC,
    GC_PAGE_GAME_STARTED
} gc_page;

typedef enum {
    GC_FACE_GAME_PLAY,
    GC_FACE_CALENDAR,
    GC_FACE_MEMORY_CARD,
    GC_FACE_OPTIONS
} gc_face;

typedef enum {
    GC_BUTTON_UP,
    GC_BUTTON_DOWN,
    GC_BUTTON_LEFT,
    GC_BUTTON_RIGHT,
    GC_BUTTON_CONFIRM,
    GC_BUTTON_CANCEL,
    GC_BUTTON_START
} gc_button;

typedef enum {
    GC_LANGUAGE_ENGLISH,
    GC_LANGUAGE_GERMAN,
    GC_LANGUAGE_FRENCH,
    GC_LANGUAGE_SPANISH,
    GC_LANGUAGE_ITALIAN,
    GC_LANGUAGE_DUTCH,
    GC_LANGUAGE_JAPANESE
} gc_language;

typedef enum { GC_SOUND_MONO, GC_SOUND_STEREO } gc_sound;

typedef struct {
    gc_sound sound;
    int screen_position;
    gc_language language;
} gc_settings;

typedef struct {
    int year;
    int month;
    int day;
    int hour;
    int minute;
    int second;
} gc_date_time;

typedef enum {
    GC_CALENDAR_DAY,
    GC_CALENDAR_MONTH,
    GC_CALENDAR_YEAR,
    GC_CALENDAR_HOUR,
    GC_CALENDAR_MINUTE,
    GC_CALENDAR_SECOND
} gc_calendar_field;

typedef enum {
    GC_CARD_ABSENT,
    GC_CARD_READY,
    GC_CARD_UNFORMATTED,
    GC_CARD_DAMAGED
} gc_card_status;

typedef struct {
    uint16_t blocks;
    uint8_t game_code[4];
    uint8_t maker_code[2];
    char filename[33];
    char title[65];
    char comment[65];
    bool allow_copy;
    bool allow_move;
} gc_card_file;

/* Cards are copied into the menu; no borrowed resource pointers are retained. */
typedef struct {
    gc_card_status status;
    uint16_t capacity_blocks;
    size_t file_count;
    gc_card_file files[GC_CARD_FILE_LIMIT];
} gc_card;

typedef enum {
    GC_CARD_ACTION_MOVE,
    GC_CARD_ACTION_COPY,
    GC_CARD_ACTION_ERASE,
    GC_CARD_ACTION_FORMAT
} gc_card_action;

typedef enum {
    GC_MESSAGE_NONE,
    GC_MESSAGE_CARD_ABSENT,
    GC_MESSAGE_CARD_DAMAGED,
    GC_MESSAGE_CARD_NO_SPACE,
    GC_MESSAGE_CARD_FILE_EXISTS,
    GC_MESSAGE_CARD_COPY_FORBIDDEN,
    GC_MESSAGE_CARD_MOVE_FORBIDDEN,
    GC_MESSAGE_CARD_COPIED,
    GC_MESSAGE_CARD_MOVED,
    GC_MESSAGE_CARD_ERASED,
    GC_MESSAGE_CARD_FORMATTED,
    GC_MESSAGE_CARD_SAVE_FAILED,
    GC_MESSAGE_DISC_ABSENT,
    GC_MESSAGE_DISC_UNREADABLE,
    GC_MESSAGE_DISC_LID_OPEN
} gc_message;

typedef enum {
    GC_DISC_ABSENT,
    GC_DISC_READY,
    GC_DISC_UNREADABLE,
    GC_DISC_LID_OPEN,
    GC_DISC_READING,
    GC_DISC_FATAL
} gc_disc_status;

typedef struct {
    size_t index;
    size_t first_row;
} gc_card_cursor;

typedef struct {
    gc_region region;
    gc_page page;
    gc_face face;
    double startup_duration; /* Native menu-entry ticks divided by region rate. */
    double startup_elapsed;
    bool startup_controlled; /* Native boot dispatcher owns startup completion. */
    double page_elapsed;
    double clock_fraction;
    gc_date_time clock; /* Displayed Calendar draft tuple over the live RTC. */
    gc_date_time clock_before_edit; /* Running RTC while the Calendar draft is open. */
    gc_settings settings;
    gc_settings settings_before_edit;
    unsigned editor_index;
    unsigned calendar_date_index; /* Separate native Date and Time field cursors. */
    unsigned calendar_time_index;
    bool editing;
    unsigned card_slot;
    size_t card_index;
    size_t card_first_row;
    gc_card_cursor card_cursors[2]; /* Retain each slot's visible row and scroll. */
    gc_card_action card_action;
    gc_card_action card_window_action; /* Retained Move/Copy/Erase selection. */
    bool confirm_yes;
    bool format_second_confirmation;
    gc_message message;
    gc_page message_return_page;
    gc_card cards[2];
    gc_disc_status disc_status;
    char disc_title[65];
    char disc_company[65];
    bool launch_requested;
    bool settings_changed;
    bool clock_changed;
    bool cards_changed[2];
} gc_menu;

void gc_menu_init(gc_menu *menu, gc_region region);
double gc_region_startup_duration(gc_region region);
void gc_menu_tick(gc_menu *menu, double elapsed_seconds);
/* Presentation and RTC have separate host clocks during real-time playback.
 * gc_menu_tick combines both for deterministic native-frame inspection. */
void gc_menu_tick_presentation(gc_menu *menu, double elapsed_seconds);
void gc_menu_advance_clock(gc_menu *menu, double elapsed_seconds);
void gc_menu_press(gc_menu *menu, gc_button button);
void gc_menu_skip_startup(gc_menu *menu);
bool gc_menu_set_clock(gc_menu *menu, const gc_date_time *clock);
bool gc_menu_set_settings(gc_menu *menu, const gc_settings *settings);
bool gc_menu_set_card(gc_menu *menu, unsigned slot, const gc_card *card);
void gc_menu_set_disc(gc_menu *menu, gc_disc_status status, const char *title,
                      const char *company);
const gc_card_file *gc_menu_card_selected(const gc_menu *menu);
size_t gc_menu_card_first_row(const gc_menu *menu, unsigned slot);
size_t gc_menu_card_cursor_index(const gc_menu *menu, unsigned slot);
uint16_t gc_card_free_blocks(const gc_card *card);
unsigned gc_date_time_weekday(const gc_date_time *clock);
gc_calendar_field gc_menu_calendar_field(const gc_menu *menu);
bool gc_date_time_valid(const gc_date_time *clock);
const char *gc_menu_face_name(gc_face face);
const char *gc_menu_language_name(gc_language language);
const char *gc_menu_message_text(gc_message message);

#endif
