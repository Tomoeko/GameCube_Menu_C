#ifndef GAMECUBE_TEXT_H
#define GAMECUBE_TEXT_H

#include "gamecube/menu.h"

typedef enum {
    GC_TEXT_MENU,
    GC_TEXT_CALENDAR,
    GC_TEXT_OPTIONS,
    GC_TEXT_DISC,
    GC_TEXT_CARD,
    GC_TEXT_HELP,
    GC_TEXT_WEEKDAYS,
    GC_TEXT_ERROR,
    GC_TEXT_GROUP_COUNT
} GcTextGroup;

typedef enum { GC_TEXT_LATIN1, GC_TEXT_SHIFT_JIS } GcTextEncoding;

typedef struct {
    const char *bytes;
    size_t byte_length;
    uint16_t flags;
    uint16_t display_size;
} GcTextEntry;

typedef struct {
    const uint8_t *bytes;
    size_t byte_count;
    unsigned count;
    GcTextEncoding encoding;
} GcTextTable;

typedef struct {
    uint8_t *rom; /* Owned; all table and string views borrow from this buffer. */
    size_t rom_size;
    bool europe;
    GcTextTable tables[7][GC_TEXT_GROUP_COUNT];
} GcText;

typedef enum {
    GC_TEXT_OPTION_INSTRUCTION,
    GC_TEXT_OPTION_SOUND,
    GC_TEXT_OPTION_SCREEN_POSITION,
    GC_TEXT_OPTION_LANGUAGE,
    GC_TEXT_OPTION_STEREO,
    GC_TEXT_OPTION_MONO
} GcTextOption;

bool gc_text_table_decode(const uint8_t *bytes, size_t byte_count,
                          GcTextEncoding encoding, GcTextTable *table);
bool gc_text_table_entry(const GcTextTable *table, unsigned index, GcTextEntry *entry);
/* The decode API accepts a descrambled ROM. Load accepts the original local IPL. */
bool gc_text_decode(const uint8_t *rom, size_t rom_size, GcText *text);
bool gc_text_load(const char *ipl_path, GcText *text);
void gc_text_destroy(GcText *text);
const GcTextTable *gc_text_table(const GcText *text, gc_language language,
                                 GcTextGroup group);
const char *gc_text_get(const GcText *text, gc_language language, GcTextGroup group,
                        unsigned index);
const char *gc_text_face(const GcText *text, gc_language language, gc_face face);
const char *gc_text_option(const GcText *text, gc_language language,
                           GcTextOption option);
const char *gc_text_language(const GcText *text, gc_language selected_language);
const char *gc_text_message(const GcText *text, const gc_menu *menu);
bool gc_text_message_index(const gc_menu *menu, GcTextGroup *group, unsigned *index);
/* Returns native HELP table entries in left-to-right display order.
 * At most three entries are produced; insufficient capacity leaves output intact. */
size_t gc_text_help_entries(const gc_menu *menu, unsigned *entries, size_t capacity);

#endif
