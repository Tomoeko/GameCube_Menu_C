#ifndef GAMECUBE_CARD_IMAGE_H
#define GAMECUBE_CARD_IMAGE_H

#include "gamecube/menu.h"

#define GC_CARD_BLOCK_BYTES 8192
#define GC_CARD_SYSTEM_BLOCKS 5
#define GC_CARD_MAX_BLOCKS 2048

typedef enum {
    GC_CARD_IMAGE_OK,
    GC_CARD_IMAGE_ARGUMENT,
    GC_CARD_IMAGE_MEMORY,
    GC_CARD_IMAGE_IO,
    GC_CARD_IMAGE_FORMAT,
    GC_CARD_IMAGE_CHECKSUM,
    GC_CARD_IMAGE_CHAIN,
    GC_CARD_IMAGE_PERMISSION,
    GC_CARD_IMAGE_NO_SPACE,
    GC_CARD_IMAGE_DUPLICATE,
    GC_CARD_IMAGE_ENCODING
} gc_card_image_result;

typedef struct {
    unsigned directory_slot;
    uint32_t image_offset;
    uint32_t comment_offset;
    uint32_t modified_seconds;
    uint16_t icon_formats;
    uint16_t icon_speeds;
    uint8_t image_flags;
    uint8_t permissions;
    uint8_t copy_count;
} gc_card_image_file_info;

/* Initialize to zero. The image owns bytes until gc_card_image_free(). */
typedef struct {
    uint8_t *bytes;
    size_t byte_count;
    unsigned block_count;
    unsigned active_directory;
    unsigned active_bat;
    uint16_t encoding;
    gc_card_image_result integrity;
    gc_card card;
    gc_card_image_file_info files[GC_CARD_FILE_LIMIT];
} gc_card_image;

gc_card_image_result gc_card_image_load(gc_card_image *image, const char *path);
/* A physically valid card image remains present even when its filesystem is
 * damaged. All-erased images are unformatted. The fallback encoding is used
 * only when no valid original header can identify the native encoding. */
gc_card_image_result gc_card_image_open_present(gc_card_image *image, const char *path,
                                                uint16_t fallback_encoding);
gc_card_image_result gc_card_image_attach(gc_card_image *image, const uint8_t *bytes,
                                          size_t byte_count,
                                          uint16_t fallback_encoding);
gc_card_image_result gc_card_image_clone(gc_card_image *destination,
                                         const gc_card_image *source);
gc_card_image_result gc_card_image_read(gc_card_image *image, const uint8_t *bytes,
                                        size_t byte_count);
gc_card_image_result gc_card_image_create(gc_card_image *image, unsigned block_count,
                                          uint16_t encoding, uint64_t format_ticks);
gc_card_image_result gc_card_image_write(const gc_card_image *image, const char *path);
gc_card_image_result gc_card_image_format(gc_card_image *image);
gc_card_image_result gc_card_image_erase(gc_card_image *image, size_t file_index);
gc_card_image_result gc_card_image_copy(gc_card_image *source, gc_card_image *target,
                                        size_t file_index, bool move);
gc_card_image_result gc_card_image_read_file(const gc_card_image *image,
                                             size_t file_index, size_t offset,
                                             void *output, size_t length);
void gc_card_image_free(gc_card_image *image);
const char *gc_card_image_result_text(gc_card_image_result result);

#endif
