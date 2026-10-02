#ifndef GAMECUBE_TEXTURE_COLLECTION_H
#define GAMECUBE_TEXTURE_COLLECTION_H

#include "gamecube/ipl.h"
#include "gamecube/layout.h"

typedef struct {
    const uint8_t *rom;
    size_t rom_size;
    size_t table_offset;
    unsigned count;
} GcIplResourceTable;

typedef struct {
    uint8_t *bytes;
    size_t byte_count;
} GcIplResource;

typedef struct {
    uint8_t format;
    uint8_t wrap_s;
    uint8_t wrap_t;
    uint8_t min_filter;
    uint8_t mag_filter;
} GcTextureInfo;

typedef struct {
    GcIplImage *images;
    GcTextureInfo *information;
    size_t count;
} GcTextureCollection;

typedef struct {
    GcTextureCollection collection;
    GcIplImage digits[10];
    GcIplImage weekdays[7][7]; /* Language, Sunday=0 through Saturday=6. */
    GcIplImage sound[7][2];    /* Language, gc_sound. */
    GcIplImage card_numbers;   /* Eleven cells; native card counts use cells 0..9. */
    GcIplImage grid;
    bool europe;
} GcMenuTextures;

enum { GC_MENU_GRID_COLUMNS = 19, GC_MENU_GRID_ROWS = 14 };

bool gc_ipl_resource_table_decode(const uint8_t *rom, size_t rom_size,
                                  size_t table_offset, GcIplResourceTable *table);
bool gc_ipl_resource_bytes(const GcIplResourceTable *table, unsigned index,
                           const uint8_t **bytes, size_t *byte_count);
bool gc_ipl_resource_unpack(const GcIplResourceTable *table, unsigned index,
                            GcIplResource *resource);
void gc_ipl_resource_destroy(GcIplResource *resource);
/* TXH0 contains a 32-byte header followed by row-relative 32-byte BTIs. */
bool gc_texture_collection_decode(const uint8_t *bytes, size_t byte_count,
                                  GcTextureCollection *collection);
void gc_texture_collection_destroy(GcTextureCollection *collection);
const GcIplImage *gc_texture_collection_image(const GcTextureCollection *collection,
                                              unsigned index);
/* Uses the already descrambled GcText ROM; decoded images own their pixels. */
bool gc_menu_textures_decode(const GcText *text, GcMenuTextures *textures);
void gc_menu_textures_destroy(GcMenuTextures *textures);
/* Resolves native calendar/option digit and weekday overrides, sound labels,
 * direction visibility, and ordinary GLH0 pool textures. */
const GcIplImage *gc_menu_textures_pane(const GcMenuTextures *textures,
                                        const gc_menu *menu, GcLayoutGroup group,
                                        const GcLayoutPane *pane);
/* Card count draws bypass the 8x8 GLH placeholder. Each cell uses its original
 * image dimensions with two-pixel spacing; index runs left to right. */
bool gc_menu_card_number_quad(const GcMenuTextures *textures, unsigned number,
                              float center_x, float center_y, unsigned index,
                              GcLayoutVertex vertices[4]);
/* Native grid emits nineteen by fourteen 32-pixel tiles. Its repeating 8x8
 * texture runs from UV0..2 across each tile; the rightmost tile is clipped. */
bool gc_menu_grid_quad(unsigned index, GcLayoutVertex vertices[4]);

#endif
