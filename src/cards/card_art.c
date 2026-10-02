#include "gamecube/card_art.h"
#include "console_common/support/endian.h"
#include "console_common/support/bounds.h"
#include "console_common/resources/resource_tpl.h"

#include <stdlib.h>
#include <string.h>

enum {
    BANNER_WIDTH = 96,
    BANNER_HEIGHT = 32,
    ICON_WIDTH = 32,
    ICON_HEIGHT = 32,
    PALETTE_BYTES = 512,
    TPL_WRAPPER_BYTES = 64,
    MAX_ENCODED_BYTES = BANNER_WIDTH * BANNER_HEIGHT * 2
};

typedef struct {
    unsigned format;
    size_t pixels;
    size_t palette;
} IconLayout;

static bool advance_offset(size_t *offset, size_t length, size_t file_size) {
    if (!cc_bounds_contains(file_size, *offset, length))
        return false;
    *offset += length;
    return true;
}

static gc_card_image_result decode_texture(const gc_card_image *image,
                                           size_t file_index, size_t pixels,
                                           size_t palette, unsigned width,
                                           unsigned height, bool indexed,
                                           GcIplImage *output) {
    uint8_t wrapper[TPL_WRAPPER_BYTES + MAX_ENCODED_BYTES + PALETTE_BYTES] = {0};
    size_t encoded_bytes = (size_t)width * height * (indexed ? 1u : 2u);
    size_t wrapper_size = TPL_WRAPPER_BYTES + encoded_bytes;
    CcTpl decoded = {0};
    gc_card_image_result result;

    /* Native card artwork uses GX textures. Route tiled pixels through the shared
     * first-party decoder instead of keeping another RGB5A3/CI8 implementation. */
    cc_write_be32(wrapper, 0x0020af30);
    cc_write_be32(wrapper + 4, 1);
    cc_write_be32(wrapper + 8, 12);
    cc_write_be32(wrapper + 12, 20);
    cc_write_be32(wrapper + 16, indexed ? 32 : 0);
    cc_write_be16(wrapper + 20, (uint16_t)height);
    cc_write_be16(wrapper + 22, (uint16_t)width);
    cc_write_be32(wrapper + 24, indexed ? 9 : 5);
    cc_write_be32(wrapper + 28, TPL_WRAPPER_BYTES);
    result = gc_card_image_read_file(image, file_index, pixels,
                                     wrapper + TPL_WRAPPER_BYTES, encoded_bytes);
    if (result != GC_CARD_IMAGE_OK)
        return result;
    if (indexed) {
        cc_write_be16(wrapper + 32, 256);
        cc_write_be32(wrapper + 36, 2);
        cc_write_be32(wrapper + 40, (uint32_t)wrapper_size);
        result = gc_card_image_read_file(image, file_index, palette,
                                         wrapper + wrapper_size, PALETTE_BYTES);
        if (result != GC_CARD_IMAGE_OK)
            return result;
        wrapper_size += PALETTE_BYTES;
    }
    if (!cc_tpl_decode(wrapper, wrapper_size, &decoded, NULL, 0))
        return GC_CARD_IMAGE_FORMAT;
    output->width = width;
    output->height = height;
    output->rgba = decoded.images[0].rgba;
    decoded.images[0].rgba = NULL;
    cc_tpl_free(&decoded);
    return GC_CARD_IMAGE_OK;
}

void gc_card_art_destroy(GcCardArt *art) {
    unsigned frame;

    if (!art)
        return;
    free(art->banner.rgba);
    for (frame = 0; frame < GC_CARD_ICON_FRAMES; ++frame)
        free(art->icons[frame].rgba);
    memset(art, 0, sizeof(*art));
}

static gc_card_image_result decode_banner(const gc_card_image *image, size_t file_index,
                                          unsigned format, size_t *offset,
                                          size_t file_size, GcIplImage *banner) {
    size_t pixels = *offset;
    size_t palette = 0;
    bool indexed = format == 1;

    if (!format)
        return GC_CARD_IMAGE_OK;
    if (format > 2)
        return GC_CARD_IMAGE_FORMAT;
    if (!advance_offset(offset, BANNER_WIDTH * BANNER_HEIGHT * (indexed ? 1u : 2u),
                        file_size))
        return GC_CARD_IMAGE_FORMAT;
    if (indexed) {
        palette = *offset;
        if (!advance_offset(offset, PALETTE_BYTES, file_size))
            return GC_CARD_IMAGE_FORMAT;
    }
    return decode_texture(image, file_index, pixels, palette, BANNER_WIDTH,
                          BANNER_HEIGHT, indexed, banner);
}

static gc_card_image_result decode_icons(const gc_card_image *image, size_t file_index,
                                         size_t offset, size_t file_size,
                                         GcCardArt *art) {
    const gc_card_image_file_info *info = &image->files[file_index];
    IconLayout layouts[GC_CARD_ICON_FRAMES] = {0};
    bool shared_palette = false;
    size_t shared_palette_offset = 0;
    unsigned frame;

    for (frame = 0; frame < GC_CARD_ICON_FRAMES; ++frame) {
        unsigned speed = ((unsigned)info->icon_speeds >> (frame * 2)) & 3;
        IconLayout *layout = &layouts[frame];

        if (!speed)
            break;
        ++art->frame_count;
        art->durations[frame] = speed * 4;
        layout->format = ((unsigned)info->icon_formats >> (frame * 2)) & 3;
        layout->pixels = offset;
        if (!layout->format)
            continue;
        if (!advance_offset(&offset,
                            ICON_WIDTH * ICON_HEIGHT * (layout->format == 2 ? 2u : 1u),
                            file_size))
            return GC_CARD_IMAGE_FORMAT;
        if (layout->format == 1)
            shared_palette = true;
        if (layout->format == 3) {
            layout->palette = offset;
            if (!advance_offset(&offset, PALETTE_BYTES, file_size))
                return GC_CARD_IMAGE_FORMAT;
        }
    }
    if (shared_palette) {
        shared_palette_offset = offset;
        if (!advance_offset(&offset, PALETTE_BYTES, file_size))
            return GC_CARD_IMAGE_FORMAT;
    }
    for (frame = 0; frame < art->frame_count; ++frame) {
        IconLayout *layout = &layouts[frame];
        gc_card_image_result result;

        if (!layout->format)
            continue;
        if (layout->format == 1)
            layout->palette = shared_palette_offset;
        result = decode_texture(image, file_index, layout->pixels, layout->palette,
                                ICON_WIDTH, ICON_HEIGHT, layout->format != 2,
                                &art->icons[frame]);
        if (result != GC_CARD_IMAGE_OK)
            return result;
    }
    return GC_CARD_IMAGE_OK;
}

gc_card_image_result gc_card_art_load(const gc_card_image *image, size_t file_index,
                                      GcCardArt *art) {
    GcCardArt candidate = {0};
    const gc_card_image_file_info *info;
    size_t offset;
    size_t file_size;
    gc_card_image_result result;

    if (!image || !image->bytes || !art || file_index >= image->card.file_count)
        return GC_CARD_IMAGE_ARGUMENT;
    info = &image->files[file_index];
    offset = info->image_offset;
    file_size = (size_t)image->card.files[file_index].blocks * GC_CARD_BLOCK_BYTES;
    if (offset == UINT32_MAX) {
        gc_card_art_destroy(art);
        return GC_CARD_IMAGE_OK;
    }
    if (offset > file_size)
        return GC_CARD_IMAGE_FORMAT;
    candidate.ping_pong = (info->image_flags & 4) != 0;
    /* Original format research records 01/05 as CI8 and 02/06 as RGB5A3.
     * YAGCD's individual banner-presence bit description is inconsistent. */
    result = decode_banner(image, file_index, info->image_flags & 3, &offset, file_size,
                           &candidate.banner);
    if (result == GC_CARD_IMAGE_OK)
        result = decode_icons(image, file_index, offset, file_size, &candidate);
    if (result == GC_CARD_IMAGE_OK) {
        gc_card_art_destroy(art);
        *art = candidate;
        memset(&candidate, 0, sizeof(candidate));
    }
    gc_card_art_destroy(&candidate);
    return result;
}

unsigned gc_card_art_frame(const GcCardArt *art, uint64_t elapsed_video_frames) {
    unsigned frame;
    unsigned cycle_frames = 0;
    unsigned tick;

    if (!art || !art->frame_count || art->frame_count > GC_CARD_ICON_FRAMES)
        return 0;
    for (frame = 0; frame < art->frame_count; ++frame)
        cycle_frames += art->durations[frame];
    if (art->ping_pong && art->frame_count > 2) {
        for (frame = 1; frame + 1 < art->frame_count; ++frame)
            cycle_frames += art->durations[frame];
    }
    if (!cycle_frames)
        return 0;
    tick = (unsigned)(elapsed_video_frames % cycle_frames);
    for (frame = 0; frame < art->frame_count; ++frame) {
        if (tick < art->durations[frame])
            return frame;
        tick -= art->durations[frame];
    }
    for (frame = art->frame_count - 2; frame > 0; --frame) {
        if (tick < art->durations[frame])
            return frame;
        tick -= art->durations[frame];
    }
    return 0;
}

const GcIplImage *gc_card_art_icon(const GcCardArt *art,
                                   uint64_t elapsed_video_frames) {
    unsigned frame;

    if (!art || !art->frame_count)
        return NULL;
    frame = gc_card_art_frame(art, elapsed_video_frames);
    return art->icons[frame].rgba ? &art->icons[frame] : NULL;
}
