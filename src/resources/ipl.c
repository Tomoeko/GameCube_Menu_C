#include "gamecube/ipl.h"
#include "console_common/support/endian.h"
#include "console_common/support/regular_file.h"

#include "ipl_internal.h"

#include <stdlib.h>
#include <string.h>

bool gc_ipl_rom_read(const char *ipl_path, uint8_t **rom) {
    size_t size;
    return cc_regular_file_read_bytes(ipl_path, GC_IPL_ROM_SIZE, GC_IPL_ROM_SIZE, rom,
                                      &size) == CC_REGULAR_FILE_OK;
}

typedef struct {
    uint16_t register_t;
    uint16_t register_u;
    uint16_t register_v;
    unsigned output_bit;
} IplScrambler;

/* Hardware recurrence documented by Segher Boessenkool's IPL research.
 * This first-party implementation is a bit-clock state machine; no emulator
 * source or dependency is incorporated. Native verification: USA ROM at
 * 0x100 decodes to PPC boot code, BS2 at 0x820 executes at 0x81300000.
 */
static unsigned scrambler_clock(IplScrambler *state) {
    unsigned t_low = state->register_t & 1;
    unsigned t_next = (state->register_t >> 1) & 1;
    unsigned u_low = state->register_u & 1;
    unsigned u_next = (state->register_u >> 1) & 1;
    unsigned v_low = state->register_v & 1;
    unsigned nonlinear = (t_low ^ u_next ^ v_low) & (t_low ^ u_low);

    state->output_bit ^= t_next ^ v_low ^ (u_low | u_next) ^ nonlinear;
    if (t_low == u_low) {
        state->register_v >>= 1;
        state->register_v ^= v_low ? UINT16_C(0xb3d0) : 0;
    }
    if (!t_low) {
        state->register_u >>= 1;
        state->register_u ^= u_low ? UINT16_C(0xfb10) : 0;
    }
    state->register_t >>= 1;
    state->register_t ^= t_low ? UINT16_C(0xa740) : 0;
    return state->output_bit;
}

bool gc_ipl_descramble(uint8_t *rom, size_t size) {
    IplScrambler state = {UINT16_C(0x2953), UINT16_C(0xd9c2), UINT16_C(0x3ff1), 1};

    if (!rom || size != GC_IPL_ROM_SIZE)
        return false;
    for (size_t offset = GC_IPL_SCRAMBLED_START; offset < GC_IPL_SCRAMBLED_END;
         ++offset) {
        uint8_t mask = 0;
        for (unsigned bit = 0; bit < 8; ++bit)
            mask = (uint8_t)((unsigned)mask << 1 | scrambler_clock(&state));
        rom[offset] ^= mask;
    }
    return true;
}

bool gc_ipl_yay0_size(const uint8_t *input, size_t input_size, size_t *decoded_size) {
    if (!input || !decoded_size || input_size < 16 || memcmp(input, "Yay0", 4))
        return false;
    *decoded_size = cc_read_be32(input + 4);
    return true;
}

typedef struct {
    size_t mask_offset;
    size_t link_offset;
    size_t chunk_offset;
    size_t link_start;
    size_t chunk_start;
    uint32_t mask;
    unsigned remaining_bits;
} Yay0Cursor;

static bool yay0_copy_link(const uint8_t *input, size_t input_size, Yay0Cursor *cursor,
                           uint8_t *output, size_t size, size_t *output_offset) {
    if (cursor->link_offset > cursor->chunk_start ||
        cursor->chunk_start - cursor->link_offset < 2)
        return false;
    uint16_t link = cc_read_be16(input + cursor->link_offset);
    cursor->link_offset += 2;
    size_t distance = (link & 0xfff) + 1;
    size_t count = link >> 12;
    if (!count) {
        if (cursor->chunk_offset >= input_size)
            return false;
        count = input[cursor->chunk_offset++] + 18;
    } else {
        count += 2;
    }
    if (distance > *output_offset || count > size - *output_offset)
        return false;
    /* Forward byte copying intentionally permits overlapping LZ matches. */
    for (size_t copied = 0; copied < count; ++copied) {
        output[*output_offset] = output[*output_offset - distance];
        ++*output_offset;
    }
    return true;
}

bool gc_ipl_yay0_decode(const uint8_t *input, size_t input_size, uint8_t *output,
                        size_t output_capacity, size_t *decoded_size,
                        size_t *consumed_size) {
    size_t size;
    if (!gc_ipl_yay0_size(input, input_size, &size) || size > output_capacity ||
        (size && !output))
        return false;
    Yay0Cursor cursor = {0};
    cursor.mask_offset = 16;
    cursor.link_start = cursor.link_offset = cc_read_be32(input + 8);
    cursor.chunk_start = cursor.chunk_offset = cc_read_be32(input + 12);
    if (cursor.link_start < 16 || cursor.link_start > cursor.chunk_start ||
        cursor.chunk_start > input_size)
        return false;
    size_t output_offset = 0;
    while (output_offset < size) {
        if (!cursor.remaining_bits) {
            if (cursor.mask_offset > cursor.link_start ||
                cursor.link_start - cursor.mask_offset < 4)
                return false;
            cursor.mask = cc_read_be32(input + cursor.mask_offset);
            cursor.mask_offset += 4;
            cursor.remaining_bits = 32;
        }
        if (cursor.mask & UINT32_C(0x80000000)) {
            if (cursor.chunk_offset >= input_size)
                return false;
            output[output_offset++] = input[cursor.chunk_offset++];
        } else if (!yay0_copy_link(input, input_size, &cursor, output, size,
                                   &output_offset)) {
            return false;
        }
        cursor.mask <<= 1;
        --cursor.remaining_bits;
    }
    if (decoded_size)
        *decoded_size = size;
    if (consumed_size)
        *consumed_size = cursor.chunk_offset;
    return true;
}

static bool font_metadata(const uint8_t *bytes, size_t size, GcIplFont *font,
                          size_t *width_table, size_t *pixels_offset) {
    if (!bytes || size < 48 || cc_read_be16(bytes) != 0)
        return false;
    font->first_character = cc_read_be16(bytes + 2);
    font->last_character = cc_read_be16(bytes + 4);
    font->substitute_character = cc_read_be16(bytes + 6);
    font->ascent = cc_read_be16(bytes + 8);
    font->descent = cc_read_be16(bytes + 10);
    font->leading = cc_read_be16(bytes + 14);
    font->cell_width = cc_read_be16(bytes + 16);
    font->cell_height = cc_read_be16(bytes + 18);
    font->columns = cc_read_be16(bytes + 26);
    font->rows = cc_read_be16(bytes + 28);
    font->width = cc_read_be16(bytes + 30);
    font->height = cc_read_be16(bytes + 32);
    *width_table = cc_read_be16(bytes + 34);
    *pixels_offset = cc_read_be32(bytes + 36);
    if (font->last_character > 255 || font->first_character > font->last_character ||
        !font->cell_width || !font->cell_height || !font->columns || !font->rows ||
        !font->width || !font->height || font->width > 1024 || font->height > 1024 ||
        font->width % 8 || font->height % 8)
        return false;
    size_t glyph_count = font->last_character - font->first_character + 1;
    size_t packed_size = (size_t)font->width * font->height / 4;
    return glyph_count <= (size_t)font->columns * font->rows &&
           (size_t)font->columns * font->cell_width <= font->width &&
           (size_t)font->rows * font->cell_height <= font->height &&
           *width_table <= size && glyph_count <= size - *width_table &&
           *pixels_offset <= size && packed_size <= size - *pixels_offset;
}

bool gc_ipl_font_decode(const uint8_t *decoded_font, size_t size, GcIplFont *font) {
    GcIplFont result = {0};
    size_t widths;
    size_t pixels;
    if (!font || font->rgba ||
        !font_metadata(decoded_font, size, &result, &widths, &pixels))
        return false;
    result.rgba = malloc((size_t)result.width * result.height * 4);
    if (!result.rgba)
        return false;
    for (unsigned character = result.first_character;
         character <= result.last_character; ++character)
        result.advances[character] =
            decoded_font[widths + character - result.first_character];
    /* OS font payloads pack four intensities per byte in 8x8 GX tiles.
     * ROM ANSI font at 0x1fcf00 expands to 0x10110 bytes; header at +0x24
     * points to 0x10000 packed bytes. The header's image size describes
     * the expanded I4 sheet, so it cannot bound the packed input directly.
     */
    for (unsigned y = 0; y < result.height; ++y) {
        for (unsigned x = 0; x < result.width; ++x) {
            size_t tile = (size_t)(y / 8) * (result.width / 8) + x / 8;
            size_t packed_offset = pixels + tile * 16 + (y % 8) * 2 + (x % 8) / 4;
            unsigned shift = 6 - (x % 4) * 2;
            uint8_t alpha =
                (uint8_t)(((decoded_font[packed_offset] >> shift) & 3) * 85);
            size_t pixel_offset = ((size_t)y * result.width + x) * 4;
            result.rgba[pixel_offset] = 255;
            result.rgba[pixel_offset + 1] = 255;
            result.rgba[pixel_offset + 2] = 255;
            result.rgba[pixel_offset + 3] = alpha;
        }
    }
    *font = result;
    return true;
}

bool gc_ipl_ansi_font_decode(const uint8_t *rom, size_t size, GcIplFont *font) {
    if (!rom || size != GC_IPL_ROM_SIZE || !font || font->rgba)
        return false;
    const uint8_t *packed = rom + GC_IPL_FONT_ANSI_OFFSET;
    size_t packed_size = GC_IPL_ROM_SIZE - GC_IPL_FONT_ANSI_OFFSET;
    size_t decoded_size;
    if (!gc_ipl_yay0_size(packed, packed_size, &decoded_size) ||
        decoded_size > 1024 * 1024)
        return false;
    uint8_t *decoded = malloc(decoded_size);
    bool okay =
        decoded &&
        gc_ipl_yay0_decode(packed, packed_size, decoded, decoded_size, NULL, NULL) &&
        gc_ipl_font_decode(decoded, decoded_size, font);
    free(decoded);
    return okay;
}

bool gc_ipl_font_load(const char *ipl_path, GcIplFont *font) {
    if (!ipl_path || !font || font->rgba)
        return false;
    uint8_t *rom = NULL;
    bool okay = gc_ipl_rom_read(ipl_path, &rom) &&
                gc_ipl_ansi_font_decode(rom, GC_IPL_ROM_SIZE, font);
    free(rom);
    return okay;
}

bool gc_ipl_font_glyph(const GcIplFont *font, uint32_t character, GcIplGlyph *glyph) {
    if (!font || !font->rgba || !glyph || !font->columns)
        return false;
    if (character < font->first_character || character > font->last_character)
        character = font->substitute_character;
    if (character < font->first_character || character > font->last_character)
        return false;
    unsigned index = character - font->first_character;
    glyph->x = index % font->columns * font->cell_width;
    glyph->y = index / font->columns * font->cell_height;
    glyph->width = font->cell_width;
    glyph->height = font->cell_height;
    glyph->advance = font->advances[character];
    return true;
}

void gc_ipl_font_destroy(GcIplFont *font) {
    if (!font)
        return;
    free(font->rgba);
    memset(font, 0, sizeof(*font));
}

static bool decode_wordmark_texture(const uint8_t *texture, size_t size,
                                    GcIplImage *image) {
    if (size < 32 || texture[0] != 1 || texture[6] > 2 || texture[7] > 2)
        return false;
    unsigned width = cc_read_be16(texture + 2);
    unsigned height = cc_read_be16(texture + 4);
    if (!width || !height || width > 1024 || height > 1024 || width % 8 || height % 4)
        return false;
    GcIplImage result = {0};
    if (!gc_ipl_texture_decode(texture, size, &result))
        return false;
    /* This I8 wordmark is used as an opacity mask; white RGB permits tinting. */
    for (size_t pixel = 0; pixel < (size_t)width * height; ++pixel)
        result.rgba[pixel * 4] = result.rgba[pixel * 4 + 1] =
            result.rgba[pixel * 4 + 2] = 255;
    *image = result;
    return true;
}

static bool find_wordmark_texture(const uint8_t *model, size_t size,
                                  GcIplImage *image) {
    if (size < 32 || memcmp(model, "J3D1bmd1", 8) || cc_read_be32(model + 8) != size)
        return false;
    unsigned count = cc_read_be32(model + 12);
    size_t offset = 32;
    for (unsigned section = 0; section < count; ++section) {
        if (offset > size || size - offset < 8)
            return false;
        const uint8_t *block = model + offset;
        size_t block_size = cc_read_be32(block + 4);
        if (block_size < 8 || block_size > size - offset)
            return false;
        if (!memcmp(block, "TEX1", 4)) {
            if (block_size < 20)
                return false;
            unsigned texture_count = cc_read_be16(block + 8);
            size_t headers = cc_read_be32(block + 12);
            size_t names = cc_read_be32(block + 16);
            if (headers > block_size || texture_count > (block_size - headers) / 32 ||
                names > block_size || texture_count > (block_size - names) / 4)
                return false;
            /* J3D1 has hash/offset pairs directly at the name table; unlike
             * J3D2, no count/padding prefix is present. Verified on the native
             * logotype model (USA ROM 0x95080 / EUR ROM 0xce080).
             */
            for (unsigned index = 0; index < texture_count; ++index) {
                size_t name = cc_read_be16(block + names + index * 4 + 2);
                if (name > block_size - names)
                    return false;
                name += names;
                static const char identifier[] = "logo_gamecube";
                if (sizeof(identifier) > block_size - name ||
                    memcmp(block + name, identifier, sizeof(identifier)))
                    continue;
                size_t header = headers + index * 32;
                return decode_wordmark_texture(block + header, block_size - header,
                                               image);
            }
        }
        offset += block_size;
    }
    return false;
}

bool gc_ipl_wordmark_load(const char *ipl_path, GcIplImage *image) {
    if (!ipl_path || !image || image->rgba)
        return false;
    uint8_t *rom = NULL;
    if (!gc_ipl_rom_read(ipl_path, &rom))
        return false;
    uint8_t *decoded = NULL;
    bool okay = false;
    if (cc_read_be32(rom + GC_IPL_SCRAMBLED_START) != UINT32_C(0x3c800011))
        gc_ipl_descramble(rom, GC_IPL_ROM_SIZE);
    okay = false;
    for (size_t offset = GC_IPL_BS2_OFFSET; offset < GC_IPL_SCRAMBLED_END - 16;
         ++offset) {
        size_t size;
        if (!gc_ipl_yay0_size(rom + offset, GC_IPL_SCRAMBLED_END - offset, &size) ||
            size < 32 || size > 1024 * 1024)
            continue;
        decoded = malloc(size);
        size_t consumed = 0;
        if (!decoded)
            break;
        bool valid = gc_ipl_yay0_decode(rom + offset, GC_IPL_SCRAMBLED_END - offset,
                                        decoded, size, NULL, &consumed);
        if (valid)
            okay = find_wordmark_texture(decoded, size, image);
        free(decoded);
        decoded = NULL;
        if (okay)
            break;
        if (valid && consumed)
            offset += consumed - 1;
    }

    free(decoded);
    free(rom);
    return okay;
}

void gc_ipl_image_destroy(GcIplImage *image) {
    if (!image)
        return;
    free(image->rgba);
    memset(image, 0, sizeof(*image));
}
