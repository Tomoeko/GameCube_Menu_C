#include "gamecube/text.h"
#include "console_common/support/endian.h"
#include "gamecube/ipl.h"

#include <stdlib.h>
#include <string.h>

enum {
    TABLE_HEADER_BYTES = 8,
    TABLE_RECORD_BYTES = 8,
    TABLE_MAX_RECORDS = 128,
    TABLE_MAX_STRING_BYTES = 2048
};

bool gc_text_table_entry(const GcTextTable *table, unsigned index, GcTextEntry *entry) {
    size_t record;
    size_t relative_string;
    size_t string_offset;
    size_t available;
    const uint8_t *terminator;

    if (!table || !table->bytes || !entry || table->count > TABLE_MAX_RECORDS ||
        index >= table->count)
        return false;
    record = TABLE_HEADER_BYTES + (size_t)index * TABLE_RECORD_BYTES;
    if (record > table->byte_count || TABLE_RECORD_BYTES > table->byte_count - record)
        return false;
    relative_string = cc_read_be32(table->bytes + record + 4);
    if (relative_string > table->byte_count - record)
        return false;
    string_offset = record + relative_string;
    if (string_offset < TABLE_HEADER_BYTES + (size_t)table->count * TABLE_RECORD_BYTES)
        return false;
    available = table->byte_count - string_offset;
    if (available > TABLE_MAX_STRING_BYTES)
        available = TABLE_MAX_STRING_BYTES;
    terminator = memchr(table->bytes + string_offset, 0, available);
    if (!terminator)
        return false;
    entry->bytes = (const char *)(table->bytes + string_offset);
    entry->byte_length = (size_t)(terminator - (table->bytes + string_offset));
    entry->flags = cc_read_be16(table->bytes + record);
    entry->display_size = cc_read_be16(table->bytes + record + 2);
    return true;
}

bool gc_text_table_decode(const uint8_t *bytes, size_t byte_count,
                          GcTextEncoding encoding, GcTextTable *table) {
    GcTextTable candidate;
    unsigned count;
    unsigned index;

    if (!bytes || !table || byte_count < TABLE_HEADER_BYTES ||
        memcmp(bytes, "STH0", 4) != 0 || encoding < GC_TEXT_LATIN1 ||
        encoding > GC_TEXT_SHIFT_JIS)
        return false;
    count = cc_read_be16(bytes + 4);
    if (!count || count > TABLE_MAX_RECORDS ||
        (size_t)count > (byte_count - TABLE_HEADER_BYTES) / TABLE_RECORD_BYTES)
        return false;
    candidate = (GcTextTable){bytes, byte_count, count, encoding};
    for (index = 0; index < count; ++index) {
        GcTextEntry entry;
        if (!gc_text_table_entry(&candidate, index, &entry))
            return false;
    }
    *table = candidate;
    return true;
}

/* File offsets are recovered metadata for the supplied IPL revisions. Original
 * strings remain in the user's ROM. USA/JAP resource pairs share these offsets. */
static const uint32_t ntsc_offsets[2][GC_TEXT_GROUP_COUNT] = {
    {0x77660, 0x5f4c0, 0x61fe0, 0x5fc40, 0x60700, 0x76f40, 0x773e0, 0x5f6e0},
    {0x627c0, 0x5f520, 0x620e0, 0x5fe20, 0x61160, 0x61d80, 0x62540, 0x5f8c0}};

static const uint32_t europe_offsets[6][GC_TEXT_GROUP_COUNT] = {
    {0xadca0, 0x92ec0, 0xd26c0, 0x93400, 0x950a0, 0xd1cc0, 0xd35e0, 0xd0820},
    {0xd4760, 0x92f80, 0xd27e0, 0x93700, 0x967c0, 0xd1fe0, 0xd44e0, 0xd0e80},
    {0xd3e00, 0x92f20, 0xd2740, 0x93560, 0x95b00, 0xd1e40, 0xd3b80, 0xd0b20},
    {0xd5a60, 0x93040, 0xd28e0, 0x93a40, 0x98260, 0xd2300, 0xd57c0, 0xd1500},
    {0xd50e0, 0x92fe0, 0xd2860, 0x938c0, 0x97640, 0xd2180, 0xd4e60, 0xd1200},
    {0xd2f00, 0x92e60, 0xd2640, 0x93280, 0x94280, 0xd1b20, 0xd2c80, 0xd0520}};

static bool index_tables(GcText *text) {
    unsigned language;
    unsigned group;

    text->europe =
        memcmp(text->rom + europe_offsets[0][GC_TEXT_OPTIONS], "STH0", 4) == 0;
    for (language = 0; language < 7; ++language) {
        const uint32_t *offsets;
        GcTextEncoding encoding;

        if (text->europe) {
            if (language == GC_LANGUAGE_JAPANESE)
                continue;
            offsets = europe_offsets[language];
            encoding = GC_TEXT_LATIN1;
        } else {
            if (language != GC_LANGUAGE_ENGLISH && language != GC_LANGUAGE_JAPANESE)
                continue;
            offsets = ntsc_offsets[language == GC_LANGUAGE_JAPANESE ? 1 : 0];
            encoding =
                language == GC_LANGUAGE_JAPANESE ? GC_TEXT_SHIFT_JIS : GC_TEXT_LATIN1;
        }
        for (group = 0; group < GC_TEXT_GROUP_COUNT; ++group) {
            size_t offset = offsets[group];
            if (offset > text->rom_size ||
                !gc_text_table_decode(text->rom + offset, text->rom_size - offset,
                                      encoding, &text->tables[language][group]))
                return false;
        }
    }
    return true;
}

void gc_text_destroy(GcText *text) {
    if (!text)
        return;
    free(text->rom);
    memset(text, 0, sizeof(*text));
}

bool gc_text_decode(const uint8_t *rom, size_t rom_size, GcText *text) {
    GcText candidate = {0};

    if (!rom || !text || rom_size != GC_IPL_ROM_SIZE)
        return false;
    candidate.rom = malloc(rom_size);
    if (!candidate.rom)
        return false;
    candidate.rom_size = rom_size;
    memcpy(candidate.rom, rom, rom_size);
    if (!index_tables(&candidate)) {
        gc_text_destroy(&candidate);
        return false;
    }
    gc_text_destroy(text);
    *text = candidate;
    return true;
}

bool gc_text_load(const char *ipl_path, GcText *text) {
    GcText candidate = {0};

    if (!ipl_path || !text)
        return false;
    candidate.rom_size = GC_IPL_ROM_SIZE;
    if (!gc_ipl_rom_read(ipl_path, &candidate.rom) ||
        !gc_ipl_descramble(candidate.rom, GC_IPL_ROM_SIZE) ||
        !index_tables(&candidate)) {
        gc_text_destroy(&candidate);
        return false;
    }
    gc_text_destroy(text);
    *text = candidate;
    return true;
}

const GcTextTable *gc_text_table(const GcText *text, gc_language language,
                                 GcTextGroup group) {
    if (!text || language < GC_LANGUAGE_ENGLISH || language > GC_LANGUAGE_JAPANESE ||
        group < GC_TEXT_MENU || group >= GC_TEXT_GROUP_COUNT ||
        !text->tables[language][group].bytes)
        return NULL;
    return &text->tables[language][group];
}

const char *gc_text_get(const GcText *text, gc_language language, GcTextGroup group,
                        unsigned index) {
    const GcTextTable *table = gc_text_table(text, language, group);
    GcTextEntry entry;

    return gc_text_table_entry(table, index, &entry) ? entry.bytes : NULL;
}

const char *gc_text_face(const GcText *text, gc_language language, gc_face face) {
    static const unsigned indices[] = {0, 2, 1, 3};

    if (face < GC_FACE_GAME_PLAY || face > GC_FACE_OPTIONS)
        return NULL;
    return gc_text_get(text, language, GC_TEXT_MENU, indices[face]);
}

const char *gc_text_option(const GcText *text, gc_language language,
                           GcTextOption option) {
    unsigned index;

    if (!text || option < GC_TEXT_OPTION_INSTRUCTION || option > GC_TEXT_OPTION_MONO)
        return NULL;
    if (text->europe) {
        if (option > GC_TEXT_OPTION_LANGUAGE)
            return NULL;
        index = (unsigned)option;
    } else {
        static const unsigned indices[] = {0, 1, 4, 5, 2, 3};
        index = indices[option];
    }
    return gc_text_get(text, language, GC_TEXT_OPTIONS, index);
}

const char *gc_text_language(const GcText *text, gc_language selected_language) {
    if (!text || selected_language < GC_LANGUAGE_ENGLISH ||
        selected_language > GC_LANGUAGE_DUTCH)
        return NULL;
    if (text->europe)
        return gc_text_get(text, GC_LANGUAGE_ENGLISH, GC_TEXT_ERROR,
                           12 + (unsigned)selected_language);
    else {
        static const unsigned indices[] = {6, 8, 7, 11, 9, 10};
        return gc_text_get(text, GC_LANGUAGE_ENGLISH, GC_TEXT_OPTIONS,
                           indices[selected_language]);
    }
}

bool gc_text_message_index(const gc_menu *menu, GcTextGroup *group, unsigned *output) {
    unsigned index;
    GcTextGroup selected_group = GC_TEXT_CARD;
    unsigned slot;
    bool moving;

    if (!menu || !group || !output || menu->card_slot >= 2)
        return false;
    slot = menu->card_slot;
    moving = menu->card_action == GC_CARD_ACTION_MOVE;
    switch (menu->message) {
        case GC_MESSAGE_CARD_ABSENT:
            index = gc_menu_card_selected(menu) &&
                            menu->card_action <= GC_CARD_ACTION_COPY &&
                            menu->cards[slot ^ 1].status == GC_CARD_ABSENT
                        ? (moving ? 9u : 21u) + (slot ^ 1)
                        : 33 + slot;
            break;
        case GC_MESSAGE_CARD_DAMAGED:
            index = gc_menu_card_selected(menu) &&
                            menu->card_action <= GC_CARD_ACTION_COPY &&
                            menu->cards[slot ^ 1].status != GC_CARD_READY
                        ? (moving ? 11u : 23u) + (slot ^ 1)
                        : 35 + slot;
            break;
        case GC_MESSAGE_CARD_NO_SPACE:
            index = moving ? 17 : 29;
            break;
        case GC_MESSAGE_CARD_FILE_EXISTS:
            index = moving ? 16 : 28;
            break;
        case GC_MESSAGE_CARD_COPY_FORBIDDEN:
            index = 20;
            break;
        case GC_MESSAGE_CARD_MOVE_FORBIDDEN:
            index = 8;
            break;
        case GC_MESSAGE_CARD_COPIED:
            index = 19;
            break;
        case GC_MESSAGE_CARD_MOVED:
            index = 7;
            break;
        case GC_MESSAGE_CARD_ERASED:
            index = 31;
            break;
        case GC_MESSAGE_CARD_FORMATTED:
            index = 43 + slot;
            break;
        case GC_MESSAGE_DISC_ABSENT:
        case GC_MESSAGE_DISC_LID_OPEN:
            selected_group = GC_TEXT_DISC;
            index = 3;
            break;
        case GC_MESSAGE_DISC_UNREADABLE:
            selected_group = GC_TEXT_DISC;
            index = 2;
            break;
        default:
            return false;
    }
    *group = selected_group;
    *output = index;
    return true;
}

const char *gc_text_message(const GcText *text, const gc_menu *menu) {
    unsigned index;
    GcTextGroup group;
    if (!text || !gc_text_message_index(menu, &group, &index))
        return NULL;
    return gc_text_get(text, menu->settings.language, group, index);
}

size_t gc_text_help_entries(const gc_menu *menu, unsigned *entries, size_t capacity) {
    unsigned candidate[3];
    size_t count = 0;

    if (!menu)
        return 0;
    /* Original help initializer 0x81310620 and page selector 0x81311a98:
     * navigating shows Select/Finish; editing shows Change/Cancel. */
    switch (menu->page) {
        case GC_PAGE_CUBE:
            candidate[count++] = 0;
            break;
        case GC_PAGE_FACE:
            candidate[count++] = 1;
            candidate[count++] = 2;
            break;
        case GC_PAGE_CALENDAR:
        case GC_PAGE_OPTIONS:
            candidate[count++] = menu->editing ? 13u : 4u;
            candidate[count++] = menu->editing ? 8u : 11u;
            candidate[count++] = 6;
            break;
        case GC_PAGE_CARDS:
            candidate[count++] = 4;
            candidate[count++] = 11;
            candidate[count++] = 6;
            break;
        case GC_PAGE_CARD_ACTION:
        case GC_PAGE_CARD_CONFIRM:
            candidate[count++] = 4;
            candidate[count++] = 8;
            candidate[count++] = 6;
            break;
        case GC_PAGE_DISC:
            candidate[count++] = 8;
            break;
        default:
            break;
    }
    if (!entries)
        return count;
    if (capacity < count)
        return 0;
    memcpy(entries, candidate, count * sizeof(*entries));
    return count;
}
