#include "gamecube/font.h"

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

static uint8_t *build_sjis_font(size_t *byte_count) {
    uint8_t *bytes;
    const size_t pixels = 3812;
    const size_t sheet_bytes = 65536;

    *byte_count = pixels + sheet_bytes * 9;
    bytes = calloc(*byte_count, 1);
    assert(bytes);
    put_u16(bytes, 2);
    put_u16(bytes + 2, 0x8140);
    put_u16(bytes + 4, 0x9872);
    put_u16(bytes + 6, 0x20);
    put_u16(bytes + 8, 24);
    put_u16(bytes + 14, 28);
    put_u16(bytes + 16, 24);
    put_u16(bytes + 18, 24);
    put_u16(bytes + 20, 2);
    put_u16(bytes + 26, 21);
    put_u16(bytes + 28, 21);
    put_u16(bytes + 30, 512);
    put_u16(bytes + 32, 512);
    put_u16(bytes + 34, 48);
    put_u32(bytes + 36, (uint32_t)pixels);
    put_u32(bytes + 40, UINT32_C(0x120000));
    memset(bytes + 48, 19, 3761);
    for (size_t sheet = 0; sheet < 9; ++sheet) {
        bytes[pixels + sheet * sheet_bytes] = 0x1b;
        bytes[pixels + sheet * sheet_bytes + 16] = 0xe4;
        bytes[pixels + sheet * sheet_bytes + 1024] = 0xff;
    }
    return bytes;
}

static unsigned alpha_at(const GcIplImage *image, unsigned x, unsigned y) {
    assert(x < image->width && y < image->height);
    return image->rgba[((size_t)y * image->width + x) * 4 + 3];
}

static void test_tiled_sheets_and_bounded_metadata(void) {
    GcSjisFont font = {0};
    GcSjisFont invalid = {0};
    size_t size;
    uint8_t *bytes = build_sjis_font(&size);

    assert(gc_font_sjis_decode(bytes, size, &font));
    assert(font.atlas.width == 1536 && font.atlas.height == 1536);
    assert(font.glyph_count == 3761 && font.sheet_columns == 3);
    for (unsigned sheet = 0; sheet < 9; ++sheet) {
        unsigned x = sheet % 3 * 512;
        unsigned y = sheet / 3 * 512;
        assert(alpha_at(&font.atlas, x, y) == 0);
        assert(alpha_at(&font.atlas, x + 1, y) == 85);
        assert(alpha_at(&font.atlas, x + 2, y) == 170);
        assert(alpha_at(&font.atlas, x + 3, y) == 255);
        assert(alpha_at(&font.atlas, x + 8, y) == 255);
        assert(alpha_at(&font.atlas, x + 9, y) == 170);
        assert(alpha_at(&font.atlas, x, y + 8) == 255);
    }
    assert(!gc_font_sjis_decode(bytes, size, &font));
    assert(!gc_font_sjis_decode(bytes, size - 1, &invalid));
    assert(!invalid.atlas.rgba && !invalid.advances);
    put_u32(bytes + 36, UINT32_MAX);
    assert(!gc_font_sjis_decode(bytes, size, &invalid));
    put_u32(bytes + 36, 3812);
    put_u16(bytes + 34, 3810);
    assert(!gc_font_sjis_decode(bytes, size, &invalid));
    put_u16(bytes + 34, 48);
    bytes[48 + 3759] = 25;
    assert(!gc_font_sjis_decode(bytes, size, &invalid));
    gc_font_sjis_destroy(&font);
    assert(!font.atlas.rgba && !font.advances);
    free(bytes);
}

static void test_native_byte_iteration(void) {
    const char bytes[] = {'A',        (char)0x82, (char)0xa0, (char)0xa6, '\n',
                          (char)0x82, ' ',        (char)0x82, 0};
    const uint32_t expected[] = {'A', 0x82a0, 0xa6, '\n', 0x8140, ' ', 0x8140};
    size_t offset = 0;
    uint32_t character = 0;

    for (unsigned index = 0; index < sizeof(expected) / sizeof(expected[0]); ++index) {
        assert(
            gc_font_next(bytes, sizeof(bytes), &offset, GC_TEXT_SHIFT_JIS, &character));
        assert(character == expected[index]);
    }
    assert(offset == 8);
    assert(!gc_font_next(bytes, sizeof(bytes), &offset, GC_TEXT_SHIFT_JIS, &character));
    offset = 1;
    assert(gc_font_next(bytes, sizeof(bytes), &offset, GC_TEXT_LATIN1, &character));
    assert(character == 0x82 && offset == 2);
    offset = 7;
    assert(gc_font_next(bytes, 8, &offset, GC_TEXT_SHIFT_JIS, &character));
    assert(character == 0x8140 && offset == 8);
    assert(!gc_font_next(bytes, 8, &offset, GC_TEXT_SHIFT_JIS, &character));
    assert(!gc_font_next(bytes, 8, &offset, (GcTextEncoding)-1, &character));
}

static void test_native_rom(const char *path) {
    GcFont font = {0};
    GcText text = {0};
    GcFontGlyph glyph;

    assert(gc_font_load(path, &font));
    assert(font.ansi.width == 512 && font.ansi.height == 512);
    assert(gc_text_load(path, &text));
    assert(font.sjis_mapping == !text.europe);
    assert(gc_font_glyph(&font, GC_TEXT_LATIN1, 'A', &glyph));
    assert(glyph.atlas_rgba == font.ansi.rgba);
    if (!text.europe) {
        assert(font.sjis.glyph_count == 3761);
        assert(gc_font_glyph(&font, GC_TEXT_SHIFT_JIS, 0x8140, &glyph));
        assert(glyph.cell.x == 0 && glyph.cell.y == 0);
        assert(gc_font_glyph(&font, GC_TEXT_SHIFT_JIS, 0x82a0, &glyph));
        assert(glyph.cell.x == 0 && glyph.cell.y == 10 * 24);
        assert(gc_font_glyph(&font, GC_TEXT_SHIFT_JIS, 0x889f, &glyph));
        assert(glyph.cell.x == 512 + 19 * 24 && glyph.cell.y == 16 * 24);
        assert(gc_font_glyph(&font, GC_TEXT_SHIFT_JIS, 0x9872, &glyph));
        assert(glyph.cell.x == 1048 && glyph.cell.y == 1288);
        assert(gc_font_glyph(&font, GC_TEXT_SHIFT_JIS, 0xffff, &glyph));
        assert(glyph.cell.x == 0 && glyph.cell.y == 0);
        for (unsigned group = 0; group < GC_TEXT_GROUP_COUNT; ++group) {
            const GcTextTable *table =
                gc_text_table(&text, GC_LANGUAGE_JAPANESE, (GcTextGroup)group);
            for (unsigned index = 0; index < table->count; ++index) {
                GcTextEntry entry;
                size_t offset = 0;
                uint32_t character;
                assert(gc_text_table_entry(table, index, &entry));
                while (gc_font_next(entry.bytes, entry.byte_length, &offset,
                                    GC_TEXT_SHIFT_JIS, &character)) {
                    assert(gc_font_glyph(&font, GC_TEXT_SHIFT_JIS, character, &glyph));
                    assert(glyph.cell.x + glyph.cell.width <= glyph.atlas_width);
                    assert(glyph.cell.y + glyph.cell.height <= glyph.atlas_height);
                }
                assert(offset == entry.byte_length);
            }
        }
    } else {
        assert(!gc_font_glyph(&font, GC_TEXT_SHIFT_JIS, 0x82a0, &glyph));
    }
    assert(!gc_font_load(path, &font));
    gc_font_destroy(&font);
    gc_text_destroy(&text);
    assert(!font.ansi.rgba && !font.sjis.atlas.rgba && !font.sjis.advances);
}

int main(int argc, char **argv) {
    test_tiled_sheets_and_bounded_metadata();
    test_native_byte_iteration();
    for (int index = 1; index < argc; ++index)
        test_native_rom(argv[index]);
    puts("Native font sheets, mappings, and byte iteration tests passed.");
    return 0;
}
