#ifndef GAMECUBE_DISC_H
#define GAMECUBE_DISC_H

#include "gamecube/ipl.h"
#include "gamecube/menu.h"
#include "gamecube/text.h"

typedef enum {
    GC_DISC_IMAGE_OK,
    GC_DISC_IMAGE_ARGUMENT,
    GC_DISC_IMAGE_MEMORY,
    GC_DISC_IMAGE_IO,
    GC_DISC_IMAGE_HEADER,
    GC_DISC_IMAGE_FILESYSTEM,
    GC_DISC_IMAGE_BANNER
} GcDiscResult;

typedef struct {
    char game_name[33];
    char company[33];
    char full_title[65];
    char full_company[65];
    char description[129];
} GcDiscMetadata;

typedef struct {
    char game_code[5];
    char maker_code[3];
    char header_title[993];
    uint8_t disc_id;
    uint8_t version;
    bool audio_streaming;
    bool simulated; /* Authored local test media, rather than an image input. */
    gc_region region;
    GcTextEncoding encoding;
    unsigned language_count;
    GcDiscMetadata languages[6];
    GcIplImage banner; /* Owned RGBA, released by gc_disc_destroy. */
} GcDisc;

/* Inputs are raw, unencrypted GCM/ISO images. Filesystem and banner data are
 * bounded before reads; no executable code is run. Initialize the owner to zero. */
GcDiscResult gc_disc_load(const char *path, GcDisc *disc);
GcDiscResult gc_disc_decode(const uint8_t *bytes, size_t byte_count, GcDisc *disc);
/* Create immutable local test metadata and authored 96x32 banner pixels.
 * The owner is replaced only after allocation succeeds. No image is read. */
GcDiscResult gc_disc_create_dummy(gc_region region, GcDisc *disc);
GcDiscResult gc_disc_banner_decode(const uint8_t *bytes, size_t byte_count,
                                   gc_region region, GcDisc *disc);
const GcDiscMetadata *gc_disc_metadata(const GcDisc *disc, gc_language language);
void gc_disc_destroy(GcDisc *disc);
const char *gc_disc_result_text(GcDiscResult result);

#endif
