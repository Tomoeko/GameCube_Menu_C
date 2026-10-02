#include "gamecube/text.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void put_u16(uint8_t *bytes, unsigned value) {
    bytes[0] = (uint8_t)(value >> 8);
    bytes[1] = (uint8_t)value;
}

static void put_u32(uint8_t *bytes, uint32_t value) {
    bytes[0] = (uint8_t)(value >> 24);
    bytes[1] = (uint8_t)(value >> 16);
    bytes[2] = (uint8_t)(value >> 8);
    bytes[3] = (uint8_t)value;
}

static void build_table(uint8_t bytes[40]) {
    memset(bytes, 0, 40);
    memcpy(bytes, "STH0", 4);
    put_u16(bytes + 4, 2);
    put_u16(bytes + 8, 0x201);
    put_u16(bytes + 10, 24);
    put_u32(bytes + 12, 16);
    put_u16(bytes + 16, 0x408);
    put_u16(bytes + 18, 32);
    put_u32(bytes + 20, 13);
    memcpy(bytes + 24, "one", 4);
    bytes[29] = 0x82;
    bytes[30] = 0xa0;
    bytes[31] = 0;
}

static void test_relative_records_and_encoded_bytes(void) {
    uint8_t bytes[40];
    GcTextTable table = {0};
    GcTextEntry entry = {0};

    build_table(bytes);
    assert(gc_text_table_decode(bytes, sizeof(bytes), GC_TEXT_SHIFT_JIS, &table));
    assert(table.count == 2 && table.encoding == GC_TEXT_SHIFT_JIS);
    assert(gc_text_table_entry(&table, 0, &entry));
    assert(entry.bytes == (const char *)bytes + 24 && entry.byte_length == 3);
    assert(entry.flags == 0x201 && entry.display_size == 24);
    assert(gc_text_table_entry(&table, 1, &entry));
    assert(entry.bytes == (const char *)bytes + 29 && entry.byte_length == 2);
    assert((unsigned char)entry.bytes[0] == 0x82);
    assert((unsigned char)entry.bytes[1] == 0xa0);
    assert(entry.flags == 0x408 && entry.display_size == 32);
    assert(!gc_text_table_entry(&table, 2, &entry));
    assert(gc_text_table_decode(bytes, sizeof(bytes), GC_TEXT_LATIN1, &table));
    assert(table.encoding == GC_TEXT_LATIN1);
}

static void test_untrusted_table_bounds(void) {
    uint8_t bytes[40];
    GcTextTable table = {0};
    GcTextTable saved;
    GcTextEntry entry;

    build_table(bytes);
    assert(gc_text_table_decode(bytes, sizeof(bytes), GC_TEXT_LATIN1, &table));
    saved = table;
    assert(!gc_text_table_decode(bytes, 23, GC_TEXT_LATIN1, &table));
    assert(memcmp(&table, &saved, sizeof(table)) == 0);
    assert(!gc_text_table_decode(bytes, sizeof(bytes), (GcTextEncoding)-1, &table));
    put_u32(bytes + 12, UINT32_MAX);
    assert(!gc_text_table_decode(bytes, sizeof(bytes), GC_TEXT_LATIN1, &table));
    build_table(bytes);
    put_u32(bytes + 20, 0);
    assert(!gc_text_table_decode(bytes, sizeof(bytes), GC_TEXT_LATIN1, &table));
    build_table(bytes);
    memset(bytes + 29, 1, sizeof(bytes) - 29);
    assert(!gc_text_table_decode(bytes, sizeof(bytes), GC_TEXT_SHIFT_JIS, &table));
    build_table(bytes);
    put_u16(bytes + 4, 129);
    assert(!gc_text_table_decode(bytes, sizeof(bytes), GC_TEXT_LATIN1, &table));
    saved.count = UINT32_MAX;
    assert(!gc_text_table_entry(&saved, 0, &entry));
    assert(!gc_text_table_entry(NULL, 0, &entry));
}

static void test_original_rom(const char *path) {
    GcText text = {0};
    gc_menu menu;
    uint8_t *saved_rom;
    GcTextEntry entry;
    const unsigned expected[] = {4, 3, 12, 11, 47, 22, 7, 6};

    assert(gc_text_load(path, &text));
    for (unsigned language = 0; language < 7; ++language) {
        bool available = text.europe ? language != GC_LANGUAGE_JAPANESE
                                     : language == GC_LANGUAGE_ENGLISH ||
                                           language == GC_LANGUAGE_JAPANESE;
        for (unsigned group = 0; group < GC_TEXT_GROUP_COUNT; ++group) {
            const GcTextTable *table =
                gc_text_table(&text, (gc_language)language, (GcTextGroup)group);
            unsigned count = expected[group];
            if (!available) {
                assert(!table);
                continue;
            }
            if (text.europe && group == GC_TEXT_OPTIONS)
                count = 4;
            if (text.europe && group == GC_TEXT_DISC)
                count = 5;
            if (text.europe && group == GC_TEXT_ERROR)
                count = 18;
            assert(table && table->count == count);
            assert(table->encoding == (language == GC_LANGUAGE_JAPANESE
                                           ? GC_TEXT_SHIFT_JIS
                                           : GC_TEXT_LATIN1));
            for (unsigned index = 0; index < count; ++index) {
                assert(gc_text_table_entry(table, index, &entry));
                assert(entry.bytes >= (const char *)text.rom);
                assert(entry.bytes < (const char *)text.rom + text.rom_size);
                assert(strlen(entry.bytes) == entry.byte_length);
            }
        }
        if (available) {
            for (unsigned face = 0; face < 4; ++face)
                assert(gc_text_face(&text, (gc_language)language, (gc_face)face));
        }
    }
    for (unsigned language = 0; language < 6; ++language)
        assert(gc_text_language(&text, (gc_language)language));
    assert(gc_text_option(&text, GC_LANGUAGE_ENGLISH, GC_TEXT_OPTION_SOUND));
    gc_menu_init(&menu, text.europe ? GC_REGION_EUROPE : GC_REGION_USA);
    menu.message = GC_MESSAGE_CARD_COPIED;
    assert(gc_text_message(&text, &menu) ==
           gc_text_get(&text, GC_LANGUAGE_ENGLISH, GC_TEXT_CARD, 19));
    menu.message = GC_MESSAGE_CARD_SAVE_FAILED;
    assert(!gc_text_message(&text, &menu));
    saved_rom = text.rom;
    assert(!gc_text_load("", &text));
    assert(text.rom == saved_rom);
    assert(!gc_text_decode(saved_rom, text.rom_size - 1, &text));
    assert(text.rom == saved_rom);
    saved_rom[0x60700] = 0;
    if (!text.europe) {
        assert(!gc_text_decode(saved_rom, text.rom_size, &text));
        assert(text.rom == saved_rom);
    }
    gc_text_destroy(&text);
    assert(!text.rom && !gc_text_table(&text, GC_LANGUAGE_ENGLISH, GC_TEXT_MENU));
}

static void test_native_help_states(void) {
    gc_menu menu;
    unsigned entries[3] = {99, 98, 97};
    const unsigned saved[3] = {99, 98, 97};

    gc_menu_init(&menu, GC_REGION_USA);
    assert(gc_text_help_entries(&menu, entries, 3) == 0);
    menu.page = GC_PAGE_CUBE;
    assert(gc_text_help_entries(&menu, entries, 3) == 1 && entries[0] == 0);
    menu.page = GC_PAGE_FACE;
    assert(gc_text_help_entries(&menu, entries, 3) == 2);
    assert(entries[0] == 1 && entries[1] == 2);
    menu.page = GC_PAGE_CALENDAR;
    assert(gc_text_help_entries(&menu, entries, 3) == 3);
    assert(entries[0] == 4 && entries[1] == 11 && entries[2] == 6);
    menu.editing = true;
    assert(gc_text_help_entries(&menu, entries, 3) == 3);
    assert(entries[0] == 13 && entries[1] == 8 && entries[2] == 6);
    menu.page = GC_PAGE_OPTIONS;
    assert(gc_text_help_entries(&menu, entries, 3) == 3);
    assert(entries[0] == 13 && entries[1] == 8 && entries[2] == 6);
    menu.page = GC_PAGE_CARDS;
    assert(gc_text_help_entries(&menu, entries, 3) == 3);
    assert(entries[0] == 4 && entries[1] == 11 && entries[2] == 6);
    menu.page = GC_PAGE_CARD_ACTION;
    assert(gc_text_help_entries(&menu, entries, 3) == 3);
    assert(entries[0] == 4 && entries[1] == 8 && entries[2] == 6);
    memcpy(entries, saved, sizeof(entries));
    assert(gc_text_help_entries(&menu, entries, 2) == 0);
    assert(memcmp(entries, saved, sizeof(entries)) == 0);
    assert(gc_text_help_entries(&menu, NULL, 0) == 3);
    menu.page = GC_PAGE_DISC;
    menu.disc_status = GC_DISC_ABSENT;
    assert(gc_text_help_entries(&menu, entries, 3) == 1 && entries[0] == 8);
    menu.disc_status = GC_DISC_READY;
    assert(gc_text_help_entries(&menu, entries, 3) == 1 && entries[0] == 8);
}

int main(int argc, char **argv) {
    test_native_help_states();
    test_relative_records_and_encoded_bytes();
    test_untrusted_table_bounds();
    for (int index = 1; index < argc; ++index)
        test_original_rom(argv[index]);
    puts("Native text table bounds and ROM lookup tests passed.");
    return 0;
}
