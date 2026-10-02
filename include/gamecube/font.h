#ifndef GAMECUBE_FONT_H
#define GAMECUBE_FONT_H

#include "gamecube/ipl.h"
#include "gamecube/text.h"

#define GC_FONT_SJIS_HALFWIDTH_COUNT 192
#define GC_FONT_SJIS_FULLWIDTH_COUNT 1221

typedef struct {
    GcIplImage atlas;
    unsigned sheet_width;
    unsigned sheet_height;
    unsigned sheet_columns;
    unsigned cell_width;
    unsigned cell_height;
    unsigned columns;
    unsigned rows;
    unsigned ascent;
    unsigned descent;
    unsigned leading;
    unsigned first_character;
    unsigned last_character;
    size_t glyph_count;
    uint8_t *advances; /* Owned, released with the atlas by gc_font_sjis_destroy. */
} GcSjisFont;

typedef struct {
    GcIplFont ansi;
    GcSjisFont sjis;
    uint16_t halfwidth[GC_FONT_SJIS_HALFWIDTH_COUNT];
    uint16_t fullwidth[GC_FONT_SJIS_FULLWIDTH_COUNT];
    bool sjis_mapping;
} GcFont;

typedef struct {
    GcIplGlyph cell;
    GcTextEncoding encoding;
    unsigned atlas_width;
    unsigned atlas_height;
    const uint8_t *atlas_rgba; /* Borrowed from GcFont. */
} GcFontGlyph;

/* Load requires a zero-initialized owner. Japanese mapping is absent in PAL IPLs. */
bool gc_font_load(const char *ipl_path, GcFont *font);
void gc_font_destroy(GcFont *font);
bool gc_font_sjis_decode(const uint8_t *decoded, size_t byte_count, GcSjisFont *font);
void gc_font_sjis_destroy(GcSjisFont *font);
bool gc_font_glyph(const GcFont *font, GcTextEncoding encoding, uint32_t character,
                   GcFontGlyph *glyph);
/* Byte iteration preserves native codes. A malformed Shift-JIS lead is consumed
 * alone and becomes the substitute character, so it cannot swallow ASCII text. */
bool gc_font_next(const char *bytes, size_t byte_count, size_t *offset,
                  GcTextEncoding encoding, uint32_t *character);

#endif
