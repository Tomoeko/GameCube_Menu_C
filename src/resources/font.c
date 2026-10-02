#include "gamecube/font.h"
#include "ipl_internal.h"
#include "console_common/support/endian.h"

#include <stdlib.h>
#include <string.h>

enum {
    FONT_HEADER_BYTES = 48,
    FONT_MAX_DECODED_BYTES = 1024 * 1024,
    FONT_MAX_ATLAS_DIMENSION = 2048,
    SJIS_HALFWIDTH_ROM_OFFSET = 0xa5718,
    SJIS_FULLWIDTH_ROM_OFFSET = 0xa5898,
    EUROPE_OPTIONS_ROM_OFFSET = 0xd26c0
};

static bool sjis_trail(unsigned byte) {
    return byte >= 0x40 && byte <= 0xfc && byte != 0x7f;
}

static bool sjis_lead(unsigned byte) {
    return (byte >= 0x81 && byte <= 0x9f) || (byte >= 0xe0 && byte <= 0xef);
}

static unsigned sjis_row_index(uint32_t character, unsigned first_row) {
    unsigned trail = (character & 0xff) - 0x40;
    if (trail >= 0x40)
        --trail;
    return ((character >> 8) - first_row) * 188 + trail;
}

static bool sjis_metadata(const uint8_t *bytes, size_t size, GcSjisFont *font,
                          size_t *width_offset, size_t *pixels_offset,
                          size_t *sheet_count) {
    size_t expanded_bytes;
    size_t sheet_pixels;
    unsigned atlas_rows;

    if (!bytes || size < FONT_HEADER_BYTES || cc_read_be16(bytes) != 2)
        return false;
    font->first_character = cc_read_be16(bytes + 2);
    font->last_character = cc_read_be16(bytes + 4);
    font->ascent = cc_read_be16(bytes + 8);
    font->descent = cc_read_be16(bytes + 10);
    font->leading = cc_read_be16(bytes + 14);
    font->cell_width = cc_read_be16(bytes + 16);
    font->cell_height = cc_read_be16(bytes + 18);
    font->columns = cc_read_be16(bytes + 26);
    font->rows = cc_read_be16(bytes + 28);
    font->sheet_width = cc_read_be16(bytes + 30);
    font->sheet_height = cc_read_be16(bytes + 32);
    *width_offset = cc_read_be16(bytes + 34);
    *pixels_offset = cc_read_be32(bytes + 36);
    expanded_bytes = cc_read_be32(bytes + 40);
    if (font->first_character != 0x8140 || font->last_character < 0x889f ||
        font->last_character > 0xeffc || !sjis_trail(font->last_character & 0xff) ||
        !font->cell_width || !font->cell_height || !font->columns || !font->rows ||
        !font->sheet_width || !font->sheet_height || font->sheet_width > 1024 ||
        font->sheet_height > 1024 || font->sheet_width % 8 || font->sheet_height % 8 ||
        (size_t)font->columns * font->cell_width > font->sheet_width ||
        (size_t)font->rows * font->cell_height > font->sheet_height)
        return false;
    sheet_pixels = (size_t)font->sheet_width * font->sheet_height;
    if (!expanded_bytes || expanded_bytes % (sheet_pixels / 2))
        return false;
    *sheet_count = expanded_bytes / (sheet_pixels / 2);
    if (*sheet_count > 16 || *pixels_offset > size ||
        expanded_bytes / 2 > size - *pixels_offset)
        return false;
    /* USA BS2 0x813096f4 maps the Kanji range arithmetically after glyph 701.
     * Original lookup tables cover halfwidth and the earlier symbol/kana rows. */
    font->glyph_count = (size_t)sjis_row_index(font->last_character, 0x88) + 703;
    if (font->glyph_count > *sheet_count * font->columns * font->rows ||
        *width_offset < FONT_HEADER_BYTES || *width_offset > *pixels_offset ||
        font->glyph_count > *pixels_offset - *width_offset)
        return false;
    font->sheet_columns = 1;
    while ((size_t)font->sheet_columns * font->sheet_columns < *sheet_count)
        ++font->sheet_columns;
    atlas_rows =
        (unsigned)((*sheet_count + font->sheet_columns - 1) / font->sheet_columns);
    font->atlas.width = font->sheet_columns * font->sheet_width;
    font->atlas.height = atlas_rows * font->sheet_height;
    return font->atlas.width <= FONT_MAX_ATLAS_DIMENSION &&
           font->atlas.height <= FONT_MAX_ATLAS_DIMENSION;
}

void gc_font_sjis_destroy(GcSjisFont *font) {
    if (!font)
        return;
    gc_ipl_image_destroy(&font->atlas);
    free(font->advances);
    memset(font, 0, sizeof(*font));
}

bool gc_font_sjis_decode(const uint8_t *decoded, size_t byte_count, GcSjisFont *font) {
    GcSjisFont candidate = {0};
    size_t width_offset;
    size_t pixels_offset;
    size_t sheet_count;

    if (!font || font->atlas.rgba || font->advances ||
        !sjis_metadata(decoded, byte_count, &candidate, &width_offset, &pixels_offset,
                       &sheet_count))
        return false;
    candidate.advances = malloc(candidate.glyph_count);
    candidate.atlas.rgba =
        calloc((size_t)candidate.atlas.width * candidate.atlas.height, 4);
    if (!candidate.advances || !candidate.atlas.rgba)
        goto release_candidate;
    memcpy(candidate.advances, decoded + width_offset, candidate.glyph_count);
    for (size_t index = 0; index < candidate.glyph_count; ++index) {
        if (candidate.advances[index] > candidate.cell_width)
            goto release_candidate;
    }
    for (size_t sheet = 0; sheet < sheet_count; ++sheet) {
        size_t packed_sheet =
            pixels_offset + sheet * candidate.sheet_width * candidate.sheet_height / 4;
        unsigned atlas_x =
            (unsigned)(sheet % candidate.sheet_columns) * candidate.sheet_width;
        unsigned atlas_y =
            (unsigned)(sheet / candidate.sheet_columns) * candidate.sheet_height;
        for (unsigned y = 0; y < candidate.sheet_height; ++y) {
            for (unsigned x = 0; x < candidate.sheet_width; ++x) {
                size_t tile = (size_t)(y / 8) * (candidate.sheet_width / 8) + x / 8;
                size_t source = packed_sheet + tile * 16 + (y % 8) * 2 + (x % 8) / 4;
                unsigned shift = 6 - (x % 4) * 2;
                uint8_t alpha = (uint8_t)(((decoded[source] >> shift) & 3) * 85);
                size_t target =
                    ((size_t)(atlas_y + y) * candidate.atlas.width + atlas_x + x) * 4;
                candidate.atlas.rgba[target] = 255;
                candidate.atlas.rgba[target + 1] = 255;
                candidate.atlas.rgba[target + 2] = 255;
                candidate.atlas.rgba[target + 3] = alpha;
            }
        }
    }
    *font = candidate;
    return true;

release_candidate:
    gc_font_sjis_destroy(&candidate);
    return false;
}

static bool load_sjis_font(const uint8_t *rom, GcSjisFont *font) {
    const uint8_t *packed = rom + GC_IPL_FONT_SJIS_OFFSET;
    size_t packed_size = GC_IPL_FONT_ANSI_OFFSET - GC_IPL_FONT_SJIS_OFFSET;
    size_t decoded_size;
    uint8_t *decoded;
    bool okay;

    if (!gc_ipl_yay0_size(packed, packed_size, &decoded_size) || !decoded_size ||
        decoded_size > FONT_MAX_DECODED_BYTES)
        return false;
    decoded = malloc(decoded_size);
    if (!decoded)
        return false;
    okay = gc_ipl_yay0_decode(packed, packed_size, decoded, decoded_size, NULL, NULL) &&
           gc_font_sjis_decode(decoded, decoded_size, font);
    free(decoded);
    return okay;
}

static bool load_sjis_mapping(const uint8_t *rom, GcFont *font) {
    for (unsigned index = 0; index < GC_FONT_SJIS_HALFWIDTH_COUNT; ++index) {
        unsigned glyph = cc_read_be16(rom + SJIS_HALFWIDTH_ROM_OFFSET + index * 2);
        if (glyph >= font->sjis.glyph_count)
            return false;
        font->halfwidth[index] = (uint16_t)glyph;
    }
    for (unsigned index = 0; index < GC_FONT_SJIS_FULLWIDTH_COUNT; ++index) {
        unsigned glyph = cc_read_be16(rom + SJIS_FULLWIDTH_ROM_OFFSET + index * 2);
        if (glyph >= font->sjis.glyph_count)
            return false;
        font->fullwidth[index] = (uint16_t)glyph;
    }
    font->sjis_mapping = true;
    return true;
}

void gc_font_destroy(GcFont *font) {
    if (!font)
        return;
    gc_ipl_font_destroy(&font->ansi);
    gc_font_sjis_destroy(&font->sjis);
    memset(font, 0, sizeof(*font));
}

bool gc_font_load(const char *ipl_path, GcFont *font) {
    GcFont candidate = {0};
    uint8_t *rom = NULL;
    bool okay = false;

    if (!ipl_path || !font || font->ansi.rgba || font->sjis.atlas.rgba ||
        font->sjis.advances)
        return false;
    if (!gc_ipl_rom_read(ipl_path, &rom) ||
        !gc_ipl_ansi_font_decode(rom, GC_IPL_ROM_SIZE, &candidate.ansi) ||
        !gc_ipl_descramble(rom, GC_IPL_ROM_SIZE))
        goto release_font_data;
    if (memcmp(rom + EUROPE_OPTIONS_ROM_OFFSET, "STH0", 4) != 0 &&
        (!load_sjis_font(rom, &candidate.sjis) || !load_sjis_mapping(rom, &candidate)))
        goto release_font_data;
    *font = candidate;
    memset(&candidate, 0, sizeof(candidate));
    okay = true;

release_font_data:
    gc_font_destroy(&candidate);
    free(rom);
    return okay;
}

static unsigned sjis_glyph_index(const GcFont *font, uint32_t character) {
    if (character >= 0x20 && character <= 0xdf)
        return font->halfwidth[character - 0x20];
    if (character < font->sjis.first_character ||
        character > font->sjis.last_character || !sjis_trail(character & 0xff))
        return 0;
    if (character >= 0x889f)
        return sjis_row_index(character, 0x88) + 702;
    if (character >= 0x879e)
        return 0;
    return font->fullwidth[sjis_row_index(character, 0x81)];
}

bool gc_font_glyph(const GcFont *font, GcTextEncoding encoding, uint32_t character,
                   GcFontGlyph *glyph) {
    if (!font || !glyph)
        return false;
    if (encoding == GC_TEXT_LATIN1) {
        if (!gc_ipl_font_glyph(&font->ansi, character, &glyph->cell))
            return false;
        glyph->encoding = GC_TEXT_LATIN1;
        glyph->atlas_width = font->ansi.width;
        glyph->atlas_height = font->ansi.height;
        glyph->atlas_rgba = font->ansi.rgba;
        return true;
    }
    if (encoding != GC_TEXT_SHIFT_JIS || !font->sjis_mapping ||
        !font->sjis.atlas.rgba || !font->sjis.advances || !font->sjis.columns ||
        !font->sjis.rows || !font->sjis.sheet_columns)
        return false;
    unsigned index = sjis_glyph_index(font, character);
    if (index >= font->sjis.glyph_count)
        return false;
    unsigned glyphs_per_sheet = font->sjis.columns * font->sjis.rows;
    unsigned sheet = index / glyphs_per_sheet;
    unsigned cell = index % glyphs_per_sheet;
    glyph->cell.x = sheet % font->sjis.sheet_columns * font->sjis.sheet_width +
                    cell % font->sjis.columns * font->sjis.cell_width;
    glyph->cell.y = sheet / font->sjis.sheet_columns * font->sjis.sheet_height +
                    cell / font->sjis.columns * font->sjis.cell_height;
    glyph->cell.width = font->sjis.cell_width;
    glyph->cell.height = font->sjis.cell_height;
    glyph->cell.advance = font->sjis.advances[index];
    glyph->encoding = GC_TEXT_SHIFT_JIS;
    glyph->atlas_width = font->sjis.atlas.width;
    glyph->atlas_height = font->sjis.atlas.height;
    glyph->atlas_rgba = font->sjis.atlas.rgba;
    return true;
}

bool gc_font_next(const char *bytes, size_t byte_count, size_t *offset,
                  GcTextEncoding encoding, uint32_t *character) {
    unsigned first;

    if (!bytes || !offset || !character || *offset >= byte_count ||
        encoding < GC_TEXT_LATIN1 || encoding > GC_TEXT_SHIFT_JIS)
        return false;
    first = (unsigned char)bytes[*offset];
    if (!first)
        return false;
    ++*offset;
    if (encoding == GC_TEXT_SHIFT_JIS && sjis_lead(first)) {
        if (*offset < byte_count && sjis_trail((unsigned char)bytes[*offset])) {
            *character = (first << 8) | (unsigned char)bytes[*offset];
            ++*offset;
        } else {
            *character = 0x8140;
        }
    } else {
        *character = first;
    }
    return true;
}
