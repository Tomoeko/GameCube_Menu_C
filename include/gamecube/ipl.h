#ifndef GAMECUBE_IPL_H
#define GAMECUBE_IPL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GC_IPL_ROM_SIZE ((size_t)0x200000)
#define GC_IPL_SCRAMBLED_START ((size_t)0x100)
#define GC_IPL_SCRAMBLED_END ((size_t)0x1aff00)
#define GC_IPL_BS2_OFFSET ((size_t)0x820)
#define GC_IPL_BS2_ADDRESS UINT32_C(0x81300000)
#define GC_IPL_FONT_ANSI_OFFSET ((size_t)0x1fcf00)
#define GC_IPL_FONT_SJIS_OFFSET ((size_t)0x1aff00)

typedef struct {
    unsigned x;
    unsigned y;
    unsigned width;
    unsigned height;
    unsigned advance;
} GcIplGlyph;

typedef struct {
    unsigned width;
    unsigned height;
    unsigned cell_width;
    unsigned cell_height;
    unsigned columns;
    unsigned rows;
    unsigned first_character;
    unsigned last_character;
    unsigned substitute_character;
    unsigned ascent;
    unsigned descent;
    unsigned leading;
    uint8_t advances[256];
    uint8_t *rgba; /* Owned, released by gc_ipl_font_destroy. */
} GcIplFont;

typedef struct {
    unsigned width;
    unsigned height;
    uint8_t *rgba;  /* Owned, released by gc_ipl_image_destroy. */
    uint8_t wrap_s; /* Native GX: 0 clamp, 1 repeat, 2 mirror. */
    uint8_t wrap_t;
} GcIplImage;

/* Read exactly one raw ROM from a regular file. The caller owns *rom on
 * success and frees it; failure sets it to NULL. No XOR transform is applied. */
bool gc_ipl_rom_read(const char *ipl_path, uint8_t **rom);

/* The XOR transform is involutive. Exactly one complete ROM is required. */
bool gc_ipl_descramble(uint8_t *rom, size_t size);

/* Buffers are borrowed and must not overlap. No allocation is performed. */
bool gc_ipl_yay0_size(const uint8_t *input, size_t input_size, size_t *decoded_size);
bool gc_ipl_yay0_decode(const uint8_t *input, size_t input_size, uint8_t *output,
                        size_t output_capacity, size_t *decoded_size,
                        size_t *consumed_size);

/* ANSI font only. The caller supplies an empty/zero-initialized font. */
bool gc_ipl_font_load(const char *ipl_path, GcIplFont *font);
bool gc_ipl_font_decode(const uint8_t *decoded_font, size_t size, GcIplFont *font);
bool gc_ipl_font_glyph(const GcIplFont *font, uint32_t character, GcIplGlyph *glyph);
void gc_ipl_font_destroy(GcIplFont *font);

/* Finds the native logo_gamecube texture by resource identifier across regions. */
bool gc_ipl_wordmark_load(const char *ipl_path, GcIplImage *image);
void gc_ipl_image_destroy(GcIplImage *image);

/* Decodes a bounded native BTI texture (GX formats 0 through 6). */
bool gc_ipl_texture_decode(const uint8_t *data, size_t size, GcIplImage *image);

#endif
