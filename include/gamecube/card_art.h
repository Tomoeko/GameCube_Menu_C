#ifndef GAMECUBE_CARD_ART_H
#define GAMECUBE_CARD_ART_H

#include "gamecube/card_image.h"
#include "gamecube/ipl.h"

#define GC_CARD_ICON_FRAMES 8

typedef struct {
    GcIplImage banner;
    GcIplImage icons[GC_CARD_ICON_FRAMES];
    unsigned frame_count;
    unsigned durations[GC_CARD_ICON_FRAMES];
    bool ping_pong;
} GcCardArt;

/* Initialize to zero. Decoded RGBA is owned, with no allocation during sampling. */
gc_card_image_result gc_card_art_load(const gc_card_image *image, size_t file_index,
                                      GcCardArt *art);
unsigned gc_card_art_frame(const GcCardArt *art, uint64_t elapsed_video_frames);
const GcIplImage *gc_card_art_icon(const GcCardArt *art, uint64_t elapsed_video_frames);
void gc_card_art_destroy(GcCardArt *art);

#endif
