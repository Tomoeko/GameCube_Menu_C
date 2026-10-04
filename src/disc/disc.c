#include "console_common/support/host.h"
#include "gamecube/disc.h"
#include "console_common/support/endian.h"
#include "console_common/support/bounds.h"
#include "console_common/resources/resource_tpl.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    DISC_HEADER_BYTES = 0x460,
    DISC_FST_ENTRY_BYTES = 12,
    DISC_MAX_FST_BYTES = 16 * 1024 * 1024,
    BANNER_HEADER_BYTES = 0x20,
    BANNER_PIXEL_BYTES = 0x1800,
    BANNER_METADATA_OFFSET = 0x1820,
    BANNER_METADATA_BYTES = 0x140,
    BANNER_MAX_BYTES = BANNER_METADATA_OFFSET + BANNER_METADATA_BYTES * 6,
    TPL_HEADER_BYTES = 64
};

#define DISC_MAX_IMAGE_BYTES ((size_t)1459978240)

typedef struct {
    const uint8_t *bytes;
    size_t byte_count;
    FILE *file;
} DiscReader;

typedef struct {
    uint32_t index;
    uint32_t end;
} DirectoryFrame;

typedef struct {
    size_t offset;
    size_t byte_count;
    bool found;
} BannerFile;

static void copy_text(char *target, const uint8_t *source, size_t field_bytes) {
    memcpy(target, source, field_bytes);
    target[field_bytes] = 0;
}

static bool read_span(const DiscReader *reader, size_t offset, void *target,
                      size_t byte_count) {
    if (!cc_bounds_contains(reader->byte_count, offset, byte_count))
        return false;
    if (reader->bytes) {
        memcpy(target, reader->bytes + offset, byte_count);
        return true;
    }
    if (offset > LONG_MAX || fseek(reader->file, (long)offset, SEEK_SET))
        return false;
    return fread(target, 1, byte_count, reader->file) == byte_count;
}

static GcDiscResult decode_banner_pixels(const uint8_t *pixels, GcIplImage *image) {
    uint8_t wrapper[TPL_HEADER_BYTES + BANNER_PIXEL_BYTES] = {0};
    CcTpl decoded = {0};

    cc_write_be32(wrapper, 0x0020af30);
    cc_write_be32(wrapper + 4, 1);
    cc_write_be32(wrapper + 8, 12);
    cc_write_be32(wrapper + 12, 20);
    cc_write_be16(wrapper + 20, 32);
    cc_write_be16(wrapper + 22, 96);
    /* USA IPL 0x8131258c sets GX format 5, width 96, height 32, and pixels +0x20.
     * Some early format notes call this RGB5A1; the native format is RGB5A3. */
    cc_write_be32(wrapper + 24, 5);
    cc_write_be32(wrapper + 28, TPL_HEADER_BYTES);
    memcpy(wrapper + TPL_HEADER_BYTES, pixels, BANNER_PIXEL_BYTES);
    if (!cc_tpl_decode(wrapper, sizeof(wrapper), &decoded, NULL, 0))
        return GC_DISC_IMAGE_BANNER;
    image->width = 96;
    image->height = 32;
    image->rgba = decoded.images[0].rgba;
    decoded.images[0].rgba = NULL;
    cc_tpl_free(&decoded);
    return GC_DISC_IMAGE_OK;
}

static GcDiscResult decode_banner(const uint8_t *bytes, size_t byte_count,
                                  GcDisc *disc) {
    unsigned language_count;
    GcDiscResult result;

    if (byte_count < 4)
        return GC_DISC_IMAGE_BANNER;
    if (memcmp(bytes, "BNR1", 4) == 0)
        language_count = 1;
    else if (memcmp(bytes, "BNR2", 4) == 0)
        language_count = 6;
    else
        return GC_DISC_IMAGE_BANNER;
    if (byte_count !=
        BANNER_METADATA_OFFSET + (size_t)language_count * BANNER_METADATA_BYTES)
        return GC_DISC_IMAGE_BANNER;
    for (unsigned index = 0; index < language_count; ++index) {
        const uint8_t *metadata =
            bytes + BANNER_METADATA_OFFSET + (size_t)index * BANNER_METADATA_BYTES;
        GcDiscMetadata *output = &disc->languages[index];
        copy_text(output->game_name, metadata, 32);
        copy_text(output->company, metadata + 32, 32);
        copy_text(output->full_title, metadata + 64, 64);
        copy_text(output->full_company, metadata + 128, 64);
        copy_text(output->description, metadata + 192, 128);
    }
    result = decode_banner_pixels(bytes + BANNER_HEADER_BYTES, &disc->banner);
    if (result == GC_DISC_IMAGE_OK) {
        disc->language_count = language_count;
        disc->encoding =
            disc->region == GC_REGION_JAPAN ? GC_TEXT_SHIFT_JIS : GC_TEXT_LATIN1;
    }
    return result;
}

GcDiscResult gc_disc_banner_decode(const uint8_t *bytes, size_t byte_count,
                                   gc_region region, GcDisc *disc) {
    GcDisc candidate;
    GcDiscResult result;

    if (!bytes || !disc || region < GC_REGION_JAPAN || region > GC_REGION_EUROPE)
        return GC_DISC_IMAGE_ARGUMENT;
    candidate = *disc;
    candidate.banner = (GcIplImage){0};
    candidate.region = region;
    candidate.language_count = 0;
    memset(candidate.languages, 0, sizeof(candidate.languages));
    result = decode_banner(bytes, byte_count, &candidate);
    if (result != GC_DISC_IMAGE_OK) {
        gc_ipl_image_destroy(&candidate.banner);
        return result;
    }
    gc_ipl_image_destroy(&disc->banner);
    *disc = candidate;
    return GC_DISC_IMAGE_OK;
}

static GcDiscResult find_banner(const uint8_t *fst, size_t fst_bytes, size_t disc_bytes,
                                BannerFile *banner) {
    uint32_t count;
    size_t names;
    DirectoryFrame *stack;
    size_t depth = 1;
    GcDiscResult result = GC_DISC_IMAGE_FILESYSTEM;

    if (fst_bytes < DISC_FST_ENTRY_BYTES || fst[0] != 1 ||
        (cc_read_be32(fst) & 0xffffff) != 0 || cc_read_be32(fst + 4) != 0)
        return GC_DISC_IMAGE_FILESYSTEM;
    count = cc_read_be32(fst + 8);
    if (!count || count > fst_bytes / DISC_FST_ENTRY_BYTES)
        return GC_DISC_IMAGE_FILESYSTEM;
    names = (size_t)count * DISC_FST_ENTRY_BYTES;
    stack = calloc(count, sizeof(*stack));
    if (!stack)
        return GC_DISC_IMAGE_MEMORY;
    stack[0] = (DirectoryFrame){0, count};
    for (uint32_t index = 1; index < count; ++index) {
        const uint8_t *entry = fst + (size_t)index * DISC_FST_ENTRY_BYTES;
        size_t name = cc_read_be32(entry) & 0xffffff;
        uint32_t value = cc_read_be32(entry + 4);
        uint32_t length = cc_read_be32(entry + 8);
        const char *filename;

        while (depth > 1 && index == stack[depth - 1].end)
            --depth;
        if (index >= stack[depth - 1].end || name >= fst_bytes - names)
            goto release_stack;
        filename = (const char *)fst + names + name;
        if (!filename[0] || !memchr(filename, 0, fst_bytes - names - name))
            goto release_stack;
        if (entry[0] == 1) {
            if (value != stack[depth - 1].index || length <= index ||
                length > stack[depth - 1].end)
                goto release_stack;
            stack[depth++] = (DirectoryFrame){index, length};
        } else if (entry[0] == 0) {
            if ((size_t)value > disc_bytes || (size_t)length > disc_bytes - value)
                goto release_stack;
            if (depth == 1 && strcmp(filename, "opening.bnr") == 0) {
                if (banner->found)
                    goto release_stack;
                *banner = (BannerFile){value, length, true};
            }
        } else {
            goto release_stack;
        }
    }
    result = GC_DISC_IMAGE_OK;

release_stack:
    free(stack);
    return result;
}

static GcDiscResult read_disc(const DiscReader *reader, GcDisc *disc) {
    uint8_t header[DISC_HEADER_BYTES];
    uint8_t banner_bytes[BANNER_MAX_BYTES];
    uint8_t *fst = NULL;
    GcDisc candidate = {0};
    BannerFile banner = {0};
    GcDiscResult result = GC_DISC_IMAGE_HEADER;
    size_t fst_offset;
    size_t fst_bytes;
    uint32_t region;

    if (reader->byte_count < sizeof(header) ||
        reader->byte_count > DISC_MAX_IMAGE_BYTES)
        return GC_DISC_IMAGE_HEADER;
    if (!read_span(reader, 0, header, sizeof(header)))
        return GC_DISC_IMAGE_IO;
    if (cc_read_be32(header + 0x1c) != UINT32_C(0xc2339f3d))
        return GC_DISC_IMAGE_HEADER;
    fst_offset = cc_read_be32(header + 0x424);
    fst_bytes = cc_read_be32(header + 0x428);
    if (fst_offset < DISC_HEADER_BYTES || fst_offset > reader->byte_count ||
        fst_bytes < DISC_FST_ENTRY_BYTES || fst_bytes > DISC_MAX_FST_BYTES ||
        fst_bytes > reader->byte_count - fst_offset ||
        cc_read_be32(header + 0x42c) < fst_bytes)
        return GC_DISC_IMAGE_FILESYSTEM;
    copy_text(candidate.game_code, header, 4);
    copy_text(candidate.maker_code, header + 4, 2);
    copy_text(candidate.header_title, header + 0x20, 992);
    candidate.disc_id = header[6];
    candidate.version = header[7];
    candidate.audio_streaming = header[8] != 0;
    region = cc_read_be32(header + 0x458);
    if (region <= 2)
        candidate.region = (gc_region)region;
    else
        candidate.region = header[3] == 'J'   ? GC_REGION_JAPAN
                           : header[3] == 'E' ? GC_REGION_USA
                                              : GC_REGION_EUROPE;
    candidate.encoding =
        candidate.region == GC_REGION_JAPAN ? GC_TEXT_SHIFT_JIS : GC_TEXT_LATIN1;
    fst = malloc(fst_bytes);
    if (!fst)
        return GC_DISC_IMAGE_MEMORY;
    if (!read_span(reader, fst_offset, fst, fst_bytes)) {
        result = GC_DISC_IMAGE_IO;
        goto release_disc;
    }
    result = find_banner(fst, fst_bytes, reader->byte_count, &banner);
    if (result != GC_DISC_IMAGE_OK)
        goto release_disc;
    if (banner.found) {
        if (banner.byte_count > sizeof(banner_bytes)) {
            result = GC_DISC_IMAGE_BANNER;
            goto release_disc;
        }
        if (!read_span(reader, banner.offset, banner_bytes, banner.byte_count)) {
            result = GC_DISC_IMAGE_IO;
            goto release_disc;
        }
        result = decode_banner(banner_bytes, banner.byte_count, &candidate);
        if (result != GC_DISC_IMAGE_OK)
            goto release_disc;
    }
    gc_disc_destroy(disc);
    *disc = candidate;
    memset(&candidate, 0, sizeof(candidate));

release_disc:
    gc_disc_destroy(&candidate);
    free(fst);
    return result;
}

GcDiscResult gc_disc_decode(const uint8_t *bytes, size_t byte_count, GcDisc *disc) {
    DiscReader reader = {bytes, byte_count, NULL};

    if (!bytes || !disc)
        return GC_DISC_IMAGE_ARGUMENT;
    return read_disc(&reader, disc);
}

static void dummy_banner_pixel(unsigned x, unsigned y, uint8_t rgba[4]) {
    bool alternate = ((x / 8) + (y / 8)) % 2 != 0;
    rgba[0] = alternate ? 32 : 24;
    rgba[1] = alternate ? 40 : 32;
    rgba[2] = alternate ? 72 : 56;
    rgba[3] = 255;
    int dx = (int)x - 16;
    int dy = (int)y - 16;
    int radius_squared = dx * dx + dy * dy;
    if (radius_squared >= 16 && radius_squared <= 144) {
        rgba[0] = 192;
        rgba[1] = 200;
        rgba[2] = 224;
    }
    if (radius_squared >= 64 && radius_squared <= 81) {
        rgba[0] = 112;
        rgba[1] = 120;
        rgba[2] = 176;
    }
    if (x >= 36 && x < 88 && y >= 8 && y < 24 && (y - 8) % 6 < 4) {
        bool bright = (x - 36) % 10 < 8;
        if (bright) {
            rgba[0] = y < 12 ? 224 : 152;
            rgba[1] = y < 12 ? 232 : 176;
            rgba[2] = 248;
        }
    }
}

GcDiscResult gc_disc_create_dummy(gc_region region, GcDisc *disc) {
    enum { WIDTH = 96, HEIGHT = 32, PIXEL_BYTES = WIDTH * HEIGHT * 4 };
    if (!disc || region < GC_REGION_JAPAN || region > GC_REGION_EUROPE)
        return GC_DISC_IMAGE_ARGUMENT;
    GcDisc candidate = {0};
    candidate.banner.rgba = malloc(PIXEL_BYTES);
    if (!candidate.banner.rgba)
        return GC_DISC_IMAGE_MEMORY;
    candidate.banner.width = WIDTH;
    candidate.banner.height = HEIGHT;
    for (unsigned y = 0; y < HEIGHT; ++y)
        for (unsigned x = 0; x < WIDTH; ++x)
            dummy_banner_pixel(x, y,
                               candidate.banner.rgba + ((size_t)y * WIDTH + x) * 4);
    memcpy(candidate.game_code, "DMY", 3);
    candidate.game_code[3] = region == GC_REGION_JAPAN ? 'J'
                             : region == GC_REGION_USA ? 'E'
                                                       : 'P';
    memcpy(candidate.maker_code, "00", 2);
    memcpy(candidate.header_title, "Dummy Disc", sizeof("Dummy Disc"));
    candidate.region = region;
    candidate.encoding = region == GC_REGION_JAPAN ? GC_TEXT_SHIFT_JIS : GC_TEXT_LATIN1;
    candidate.simulated = true;
    candidate.language_count = region == GC_REGION_EUROPE ? 6 : 1;
    for (unsigned index = 0; index < candidate.language_count; ++index) {
        GcDiscMetadata *metadata = &candidate.languages[index];
        memcpy(metadata->game_name, "Dummy Disc", sizeof("Dummy Disc"));
        memcpy(metadata->full_title, "Dummy Disc", sizeof("Dummy Disc"));
        memcpy(metadata->company, "Local test media (simulated)",
               sizeof("Local test media (simulated)"));
        memcpy(metadata->full_company, "Local test media (simulated)",
               sizeof("Local test media (simulated)"));
        memcpy(metadata->description, "Simulated disc for menu animation testing.",
               sizeof("Simulated disc for menu animation testing."));
    }
    gc_disc_destroy(disc);
    *disc = candidate;
    return GC_DISC_IMAGE_OK;
}

GcDiscResult gc_disc_load(const char *path, GcDisc *disc) {
    DiscReader reader = {0};
    GcDiscResult result;
    long length;

    if (!path || !disc)
        return GC_DISC_IMAGE_ARGUMENT;
    reader.file = cc_host_fopen(path, "rb");
    if (!reader.file)
        return GC_DISC_IMAGE_IO;
    if (fseek(reader.file, 0, SEEK_END) || (length = ftell(reader.file)) < 0) {
        fclose(reader.file);
        return GC_DISC_IMAGE_IO;
    }
    reader.byte_count = (size_t)length;
    result = read_disc(&reader, disc);
    fclose(reader.file);
    return result;
}

const GcDiscMetadata *gc_disc_metadata(const GcDisc *disc, gc_language language) {
    if (!disc || !disc->language_count)
        return NULL;
    if (disc->language_count == 1 || language < GC_LANGUAGE_ENGLISH ||
        language > GC_LANGUAGE_DUTCH)
        return &disc->languages[0];
    return &disc->languages[language];
}

void gc_disc_destroy(GcDisc *disc) {
    if (!disc)
        return;
    gc_ipl_image_destroy(&disc->banner);
    memset(disc, 0, sizeof(*disc));
}

const char *gc_disc_result_text(GcDiscResult result) {
    static const char *const descriptions[] = {"Disc image ready.",
                                               "Invalid disc image argument.",
                                               "Not enough memory for the disc image.",
                                               "Disc image could not be read.",
                                               "Disc image header is invalid.",
                                               "Disc image filesystem is invalid.",
                                               "Disc image banner is invalid."};
    if (result < GC_DISC_IMAGE_OK || result > GC_DISC_IMAGE_BANNER)
        return "Unknown disc image error.";
    return descriptions[result];
}
