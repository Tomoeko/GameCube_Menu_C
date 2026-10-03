#include "gamecube/config.h"
#include "cards/card_checksum.h"
#include "console_common/support/endian.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    DUMMY_ICON_WIDTH = 32,
    DUMMY_ICON_BYTES = DUMMY_ICON_WIDTH * DUMMY_ICON_WIDTH * 2,
    DUMMY_ANIMATED_FRAMES = 3,
    DUMMY_COMMENT_BYTES = 64
};

_Static_assert((DUMMY_ANIMATED_FRAMES * DUMMY_ICON_BYTES + DUMMY_COMMENT_BYTES) <=
                   GC_CARD_BLOCK_BYTES,
               "Dummy artwork and comments must fit in one save block");

void gc_config_init(GcConfig *config) {
    if (config)
        *config = (GcConfig){.slot_present = {true, true}, .dummy_count = {3, 3}};
}

bool gc_config_noinsert_mask(const char *slots, unsigned *mask) {
    if (!slots || !mask)
        return false;
    unsigned parsed = !strcmp(slots, "a")    ? 1u
                      : !strcmp(slots, "b")  ? 2u
                      : !strcmp(slots, "ab") ? 3u
                                             : 0;
    if (!parsed)
        return false;
    *mask = parsed;
    return true;
}

static bool valid(const GcConfig *config) {
    return config && config->dummy_count[0] <= GC_CONFIG_DUMMY_LIMIT &&
           config->dummy_count[1] <= GC_CONFIG_DUMMY_LIMIT;
}

static char *trim(char *text) {
    while (isspace((unsigned char)*text))
        ++text;
    size_t length = strlen(text);
    while (length && isspace((unsigned char)text[length - 1]))
        text[--length] = 0;
    return text;
}

static void lowercase(char *text) {
    for (; *text; ++text)
        *text = (char)tolower((unsigned char)*text);
}

static bool presence(const char *value, bool *result) {
    if (!strcmp(value, "present") || !strcmp(value, "available"))
        *result = true;
    else if (!strcmp(value, "absent") || !strcmp(value, "unavailable"))
        *result = false;
    else
        return false;
    return true;
}

static bool boolean(const char *value, bool *result) {
    if (!strcmp(value, "true"))
        *result = true;
    else if (!strcmp(value, "false"))
        *result = false;
    else
        return false;
    return true;
}

static bool count(const char *value, unsigned *result) {
    if (!*value)
        return false;
    unsigned number = 0;
    for (; *value; ++value) {
        if (*value < '0' || *value > '9')
            return false;
        number = number * 10 + (unsigned)(*value - '0');
        if (number > GC_CONFIG_DUMMY_LIMIT)
            return false;
    }
    *result = number;
    return true;
}

static bool assignment(GcConfig *config, const char *key, const char *value) {
    if (!strcmp(key, "dummy_count")) {
        unsigned number;
        if (!count(value, &number))
            return false;
        config->dummy_count[0] = config->dummy_count[1] = number;
        return true;
    }
    for (unsigned slot = 0; slot < 2; ++slot) {
        if (!strcmp(key, slot ? "slot_b" : "slot_a"))
            return presence(value, &config->slot_present[slot]);
        if (!strcmp(key, slot ? "dummy_b" : "dummy_a"))
            return boolean(value, &config->dummy_data[slot]);
        if (!strcmp(key, slot ? "dummy_count_b" : "dummy_count_a"))
            return count(value, &config->dummy_count[slot]);
    }
    return false;
}

GcConfigResult gc_config_load(GcConfig *config, const char *path, size_t *error_line) {
    if (error_line)
        *error_line = 0;
    if (!config || !path)
        return GC_CONFIG_INVALID;
    FILE *file = fopen(path, "r");
    if (!file)
        return errno == ENOENT ? GC_CONFIG_MISSING : GC_CONFIG_IO;
    GcConfig pending;
    gc_config_init(&pending);
    char buffer[256];
    size_t line = 0;
    GcConfigResult result = GC_CONFIG_OK;
    while (fgets(buffer, sizeof(buffer), file)) {
        ++line;
        size_t length = strlen(buffer);
        if (length == sizeof(buffer) - 1 && buffer[length - 1] != '\n') {
            result = GC_CONFIG_INVALID;
            break;
        }
        char *comment = strpbrk(buffer, "#;");
        if (comment)
            *comment = 0;
        char *key = trim(buffer);
        if (!*key)
            continue;
        lowercase(key);
        if (!strcmp(key, "[cards]"))
            continue;
        char *equals = strchr(key, '=');
        if (!equals) {
            result = GC_CONFIG_INVALID;
            break;
        }
        *equals = 0;
        if (!assignment(&pending, trim(key), trim(equals + 1))) {
            result = GC_CONFIG_INVALID;
            break;
        }
    }
    if (ferror(file))
        result = GC_CONFIG_IO;
    if (fclose(file) && result == GC_CONFIG_OK)
        result = GC_CONFIG_IO;
    if (result == GC_CONFIG_OK)
        *config = pending;
    else if (error_line && result == GC_CONFIG_INVALID)
        *error_line = line;
    return result;
}

GcConfigResult gc_config_write(const GcConfig *config, const char *path) {
    if (!valid(config) || !path)
        return GC_CONFIG_INVALID;
    FILE *file = fopen(path, "w");
    if (!file)
        return GC_CONFIG_IO;
    int written = fprintf(file,
                          "[cards]\n"
                          "slot_a=%s\nslot_b=%s\n"
                          "dummy_a=%s\ndummy_b=%s\n"
                          "dummy_count_a=%u\ndummy_count_b=%u\n",
                          config->slot_present[0] ? "present" : "absent",
                          config->slot_present[1] ? "present" : "absent",
                          config->dummy_data[0] ? "true" : "false",
                          config->dummy_data[1] ? "true" : "false",
                          config->dummy_count[0], config->dummy_count[1]);
    bool failed = written < 0 || ferror(file);
    if (fclose(file))
        failed = true;
    return failed ? GC_CONFIG_IO : GC_CONFIG_OK;
}

static unsigned dummy_icon_frames(unsigned file) {
    return file % 3 ? DUMMY_ANIMATED_FRAMES : 1;
}

static void dummy_icon(uint8_t *pixels, unsigned slot, unsigned file, unsigned frame) {
    /* Independently authored QA graphics in native 4x4 texture tiles. */
    bool animated = dummy_icon_frames(file) > 1;
    unsigned marker_left = 6 + frame * 6;
    for (unsigned y = 0; y < DUMMY_ICON_WIDTH; ++y) {
        for (unsigned x = 0; x < DUMMY_ICON_WIDTH; ++x) {
            bool border = x < 3 || y < 3 || x >= 29 || y >= 29;
            bool mark =
                animated ? x >= marker_left && x < marker_left + 8 && y >= 12 && y < 20
                         : x >= 8 && x <= 23 && y >= 8 && y <= 23 &&
                               (((x / 4 + y / 4 + file) & 1) == 0);
            unsigned red = border ? 3 : mark ? 31 : (8 + file * 5) & 31;
            unsigned green = border ? 3 : mark ? 31 : (slot ? 12 : 24);
            unsigned blue = border ? 3 : mark ? 31 : (slot ? 24 : 12);
            unsigned value = 0x8000u | red << 10 | green << 5 | blue;
            unsigned tile = (y / 4 * 8 + x / 4) * 32;
            unsigned offset = tile + (y % 4 * 4 + x % 4) * 2;
            cc_write_be16(pixels + offset, (uint16_t)value);
        }
    }
}

gc_card_image_result gc_config_create_card(const GcConfig *config, unsigned slot,
                                           uint16_t encoding, gc_card_image *image) {
    if (!valid(config) || slot > 1 || !image)
        return GC_CARD_IMAGE_ARGUMENT;
    gc_card_image pending = {0};
    gc_card_image_result result = gc_card_image_create(
        &pending, 64, encoding, (slot + UINT64_C(1)) * UINT64_C(0x12345678));
    if (result != GC_CARD_IMAGE_OK)
        return result;
    unsigned files = config->dummy_data[slot] ? config->dummy_count[slot] : 0;
    for (unsigned copy = 0; copy < 2; ++copy) {
        uint8_t *directory = pending.bytes + (1u + copy) * GC_CARD_BLOCK_BYTES;
        uint8_t *bat = pending.bytes + (3u + copy) * GC_CARD_BLOCK_BYTES;
        for (unsigned index = 0; index < files; ++index) {
            uint8_t *entry = directory + index * 64;
            memset(entry, 0, 64);
            memcpy(entry, "GCQA00", 6);
            snprintf((char *)entry + 8, 32, "test_%c_%02u", slot ? 'b' : 'a',
                     index + 1);
            cc_write_be32(entry + 0x28, index);
            cc_write_be32(entry + 0x2c, 0);
            unsigned frames = dummy_icon_frames(index);
            unsigned formats = 0;
            unsigned speeds = 0;
            for (unsigned frame = 0; frame < frames; ++frame) {
                formats |= 2u << (frame * 2); /* RGB5A3, no banner. */
                speeds |= (frames > 1 ? 3u : 1u) << (frame * 2);
            }
            entry[7] = index % 3 == 2 ? 4 : 0; /* Native back-and-forth flag. */
            cc_write_be16(entry + 0x30, (uint16_t)formats);
            cc_write_be16(entry + 0x32, (uint16_t)speeds);
            cc_write_be16(entry + 0x36, (uint16_t)(5 + index));
            cc_write_be16(entry + 0x38, 1);
            cc_write_be32(entry + 0x3c, frames * DUMMY_ICON_BYTES);
            cc_write_be16(bat + 10 + index * 2, UINT16_MAX);
        }
        cc_write_be16(bat + 6, (uint16_t)(59 - files));
        cc_write_be16(bat + 8, (uint16_t)(files ? files + 4 : 4));
        gc_card_checksum_write(directory, 0x1ffc, directory + 0x1ffc);
        gc_card_checksum_write(bat + 4, GC_CARD_BLOCK_BYTES - 4, bat);
    }
    for (unsigned index = 0; index < files; ++index) {
        uint8_t *payload = pending.bytes + (5u + index) * GC_CARD_BLOCK_BYTES;
        unsigned frames = dummy_icon_frames(index);
        for (unsigned frame = 0; frame < frames; ++frame)
            dummy_icon(payload + frame * DUMMY_ICON_BYTES, slot, index, frame);
        size_t comment_offset = frames * DUMMY_ICON_BYTES;
        snprintf((char *)payload + comment_offset, 32, "Test Save %02u", index + 1);
        snprintf((char *)payload + comment_offset + 32, 32, "Local QA data - Card %c",
                 slot ? 'B' : 'A');
    }
    gc_card_image verified = {0};
    result = gc_card_image_read(&verified, pending.bytes, pending.byte_count);
    gc_card_image_free(&pending);
    if (result == GC_CARD_IMAGE_OK) {
        gc_card_image_free(image);
        *image = verified;
    } else {
        gc_card_image_free(&verified);
    }
    return result;
}

bool gc_config_prepare_services(const GcConfig *config, GcServices *services,
                                gc_menu *menu, const char *explicit_inputs[2],
                                const char *state_paths[2]) {
    if (!valid(config) || !services || !menu || !explicit_inputs || !state_paths)
        return false;
    const char *inputs[2] = {explicit_inputs[0], explicit_inputs[1]};
    for (unsigned slot = 0; slot < 2; ++slot) {
        if (inputs[slot] || !config->slot_present[slot])
            continue;
        if (!state_paths[slot])
            return false;
        gc_card_image image = {0};
        gc_card_image_result result = gc_config_create_card(
            config, slot, menu->region == GC_REGION_JAPAN ? 1 : 0, &image);
        if (result == GC_CARD_IMAGE_OK)
            result = gc_card_image_write(&image, state_paths[slot]);
        gc_card_image_free(&image);
        if (result != GC_CARD_IMAGE_OK)
            return false;
        inputs[slot] = state_paths[slot];
    }
    return gc_services_init(services, menu, inputs, state_paths);
}
