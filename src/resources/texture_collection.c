#include "gamecube/texture_collection.h"
#include "console_common/support/endian.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

enum {
    RESOURCE_LIMIT = 512,
    RESOURCE_BYTES_LIMIT = 16 * 1024 * 1024,
    TEXTURE_LIMIT = 512,
    PIXEL_BYTES_LIMIT = 64 * 1024 * 1024
};

static bool resource_offset(const GcIplResourceTable *table, unsigned index,
                            size_t *offset) {
    size_t relative;

    if (!table || !table->rom || !offset || !table->count ||
        table->count > RESOURCE_LIMIT || index >= table->count ||
        table->table_offset > table->rom_size ||
        table->rom_size - table->table_offset < 4 + (size_t)table->count * 4)
        return false;
    relative = cc_read_be32(table->rom + table->table_offset + 4 + (size_t)index * 4);
    if (relative < 4 + (size_t)table->count * 4 ||
        relative >= table->rom_size - table->table_offset)
        return false;
    *offset = table->table_offset + relative;
    return true;
}

bool gc_ipl_resource_table_decode(const uint8_t *rom, size_t rom_size,
                                  size_t table_offset, GcIplResourceTable *table) {
    GcIplResourceTable candidate;
    size_t previous = 0;

    if (!rom || !table || table_offset > rom_size || rom_size - table_offset < 4)
        return false;
    candidate = (GcIplResourceTable){rom, rom_size, table_offset,
                                     cc_read_be32(rom + table_offset)};
    if (!candidate.count || candidate.count > RESOURCE_LIMIT)
        return false;
    for (unsigned index = 0; index < candidate.count; ++index) {
        size_t offset;
        if (!resource_offset(&candidate, index, &offset) || offset <= previous)
            return false;
        previous = offset;
    }
    *table = candidate;
    return true;
}

bool gc_ipl_resource_bytes(const GcIplResourceTable *table, unsigned index,
                           const uint8_t **bytes, size_t *byte_count) {
    size_t start;
    size_t end;

    if (!bytes || !byte_count || !resource_offset(table, index, &start))
        return false;
    end = table->rom_size;
    if (index + 1 < table->count && !resource_offset(table, index + 1, &end))
        return false;
    if (end <= start)
        return false;
    *bytes = table->rom + start;
    *byte_count = end - start;
    return true;
}

bool gc_ipl_resource_unpack(const GcIplResourceTable *table, unsigned index,
                            GcIplResource *resource) {
    const uint8_t *bytes;
    size_t size;
    size_t decoded_size;
    GcIplResource candidate = {0};
    bool compressed;

    if (!resource || resource->bytes ||
        !gc_ipl_resource_bytes(table, index, &bytes, &size))
        return false;
    compressed = size >= 4 && memcmp(bytes, "Yay0", 4) == 0;
    decoded_size = size;
    if (compressed && !gc_ipl_yay0_size(bytes, size, &decoded_size))
        return false;
    if (!decoded_size || decoded_size > RESOURCE_BYTES_LIMIT)
        return false;
    candidate.bytes = malloc(decoded_size);
    if (!candidate.bytes)
        return false;
    candidate.byte_count = decoded_size;
    if (compressed) {
        size_t actual_size;
        size_t consumed;
        if (!gc_ipl_yay0_decode(bytes, size, candidate.bytes, decoded_size,
                                &actual_size, &consumed) ||
            actual_size != decoded_size) {
            gc_ipl_resource_destroy(&candidate);
            return false;
        }
    } else {
        memcpy(candidate.bytes, bytes, decoded_size);
    }
    *resource = candidate;
    return true;
}

void gc_ipl_resource_destroy(GcIplResource *resource) {
    if (!resource)
        return;
    free(resource->bytes);
    memset(resource, 0, sizeof(*resource));
}

bool gc_texture_collection_decode(const uint8_t *bytes, size_t byte_count,
                                  GcTextureCollection *collection) {
    GcTextureCollection candidate = {0};
    size_t header_end;
    size_t pixel_bytes = 0;

    if (!bytes || byte_count < 32 || !collection || collection->images ||
        collection->information || memcmp(bytes, "TXH0", 4) != 0)
        return false;
    candidate.count = cc_read_be16(bytes + 4);
    if (!candidate.count || candidate.count > TEXTURE_LIMIT ||
        candidate.count > (byte_count - 32) / 32)
        return false;
    header_end = 32 + candidate.count * 32;
    candidate.images = calloc(candidate.count, sizeof(*candidate.images));
    candidate.information = calloc(candidate.count, sizeof(*candidate.information));
    if (!candidate.images || !candidate.information)
        goto fail;
    for (size_t index = 0; index < candidate.count; ++index) {
        size_t offset = 32 + index * 32;
        const uint8_t *row = bytes + offset;
        size_t image_offset = cc_read_be32(row + 28);
        size_t decoded_bytes =
            (size_t)cc_read_be16(row + 2) * cc_read_be16(row + 4) * 4;

        /* Every BTI pointer is relative to its own row, not the TXH0 header.
         * Native pool setter and draw: USA BS2 0x813097fc/0x8130a334. */
        if (image_offset < header_end - offset || image_offset >= byte_count - offset ||
            decoded_bytes > PIXEL_BYTES_LIMIT - pixel_bytes || row[6] > 2 ||
            row[7] > 2 || row[20] > 5 || row[21] > 1 ||
            !gc_ipl_texture_decode(row, byte_count - offset, &candidate.images[index]))
            goto fail;
        pixel_bytes += decoded_bytes;
        candidate.information[index] =
            (GcTextureInfo){row[0], row[6], row[7], row[20], row[21]};
    }
    *collection = candidate;
    return true;
fail:
    gc_texture_collection_destroy(&candidate);
    return false;
}

void gc_texture_collection_destroy(GcTextureCollection *collection) {
    if (!collection)
        return;
    if (collection->images)
        for (size_t index = 0; index < collection->count; ++index)
            gc_ipl_image_destroy(&collection->images[index]);
    free(collection->images);
    free(collection->information);
    memset(collection, 0, sizeof(*collection));
}

const GcIplImage *gc_texture_collection_image(const GcTextureCollection *collection,
                                              unsigned index) {
    if (!collection || !collection->images || index >= collection->count)
        return NULL;
    return &collection->images[index];
}

static bool decode_resource_image(const GcIplResourceTable *table, unsigned index,
                                  GcIplImage *image) {
    GcIplResource resource = {0};
    bool result;

    if (!gc_ipl_resource_unpack(table, index, &resource))
        return false;
    result = gc_ipl_texture_decode(resource.bytes, resource.byte_count, image);
    gc_ipl_resource_destroy(&resource);
    return result;
}

bool gc_menu_textures_decode(const GcText *text, GcMenuTextures *textures) {
    GcMenuTextures candidate = {0};
    GcIplResourceTable table;
    GcIplResource pool = {0};
    unsigned digit_base;
    unsigned pool_index;
    unsigned grid_index;
    /* Resource indices recovered from USA 0x8130b3ec, EUR 0x8130b518.
     * Only metadata is compiled; original image data is read from the IPL. */
    static const struct {
        unsigned weekday_base;
        unsigned sound_base;
    } europe_resources[6] = {
        [GC_LANGUAGE_ENGLISH] = {49, 47}, [GC_LANGUAGE_GERMAN] = {71, 69},
        [GC_LANGUAGE_FRENCH] = {62, 60},  [GC_LANGUAGE_SPANISH] = {171, 169},
        [GC_LANGUAGE_ITALIAN] = {80, 78}, [GC_LANGUAGE_DUTCH] = {40, 38}};

    if (!text || !text->rom || !textures || textures->collection.images ||
        textures->grid.rgba || textures->digits[0].rgba)
        return false;
    candidate.europe = text->europe;
    if (!gc_ipl_resource_table_decode(text->rom, text->rom_size,
                                      text->europe ? 0x82040 : 0x5f240, &table))
        return false;
    digit_base = text->europe ? 91 : 51;
    pool_index = text->europe ? 167 : 77;
    grid_index = text->europe ? 89 : 49;
    if (!gc_ipl_resource_unpack(&table, pool_index, &pool) ||
        !gc_texture_collection_decode(pool.bytes, pool.byte_count,
                                      &candidate.collection))
        goto fail;
    gc_ipl_resource_destroy(&pool);
    if (!decode_resource_image(&table, grid_index, &candidate.grid))
        goto fail;
    if (!decode_resource_image(&table, grid_index + 1, &candidate.card_numbers))
        goto fail;
    for (unsigned index = 0; index < 10; ++index)
        if (!decode_resource_image(&table, digit_base + index,
                                   &candidate.digits[index]))
            goto fail;
    for (unsigned language = 0; language < 7; ++language) {
        unsigned weekday_base;
        unsigned sound_base;
        if (text->europe) {
            if (language == GC_LANGUAGE_JAPANESE)
                continue;
            weekday_base = europe_resources[language].weekday_base;
            sound_base = europe_resources[language].sound_base;
        } else {
            if (language != GC_LANGUAGE_ENGLISH && language != GC_LANGUAGE_JAPANESE)
                continue;
            weekday_base = language == GC_LANGUAGE_JAPANESE ? 63 : 31;
            sound_base = language == GC_LANGUAGE_JAPANESE ? 61 : 29;
        }
        for (unsigned index = 0; index < 7; ++index)
            if (!decode_resource_image(&table, weekday_base + index,
                                       &candidate.weekdays[language][index]))
                goto fail;
        for (unsigned mode = 0; mode < 2; ++mode)
            if (!decode_resource_image(&table, sound_base + mode,
                                       &candidate.sound[language][mode]))
                goto fail;
    }
    *textures = candidate;
    return true;
fail:
    gc_ipl_resource_destroy(&pool);
    gc_menu_textures_destroy(&candidate);
    return false;
}

void gc_menu_textures_destroy(GcMenuTextures *textures) {
    if (!textures)
        return;
    gc_texture_collection_destroy(&textures->collection);
    gc_ipl_image_destroy(&textures->grid);
    gc_ipl_image_destroy(&textures->card_numbers);
    for (unsigned index = 0; index < 10; ++index)
        gc_ipl_image_destroy(&textures->digits[index]);
    for (unsigned language = 0; language < 7; ++language) {
        for (unsigned index = 0; index < 7; ++index)
            gc_ipl_image_destroy(&textures->weekdays[language][index]);
        for (unsigned mode = 0; mode < 2; ++mode)
            gc_ipl_image_destroy(&textures->sound[language][mode]);
    }
    memset(textures, 0, sizeof(*textures));
}

static const GcIplImage *available_image(const GcIplImage *image) {
    return image && image->rgba ? image : NULL;
}

static bool pane_key(const GcLayoutPane *pane, const char key[4]) {
    return memcmp(pane->name, key, 4) == 0;
}

const GcIplImage *gc_menu_textures_pane(const GcMenuTextures *textures,
                                        const gc_menu *menu, GcLayoutGroup group,
                                        const GcLayoutPane *pane) {
    gc_language language;

    if (!textures || !menu || !pane || group < GC_LAYOUT_MENU ||
        group >= GC_LAYOUT_GROUP_COUNT)
        return NULL;
    language = menu->settings.language;
    if (language < GC_LANGUAGE_ENGLISH || language > GC_LANGUAGE_JAPANESE)
        return NULL;
    if (group == GC_LAYOUT_CALENDAR_FACE) {
        unsigned digits[8];
        unsigned time[6];
        const gc_date_time *clock = &menu->clock;

        if (!gc_date_time_valid(clock))
            return NULL;
        digits[0] = (unsigned)clock->year / 1000;
        digits[1] = (unsigned)clock->year / 100 % 10;
        digits[2] = (unsigned)clock->year / 10 % 10;
        digits[3] = (unsigned)clock->year % 10;
        digits[4] = (unsigned)clock->month / 10;
        digits[5] = (unsigned)clock->month % 10;
        digits[6] = (unsigned)clock->day / 10;
        digits[7] = (unsigned)clock->day % 10;
        time[0] = (unsigned)clock->hour / 10;
        time[1] = (unsigned)clock->hour % 10;
        time[2] = (unsigned)clock->minute / 10;
        time[3] = (unsigned)clock->minute % 10;
        time[4] = (unsigned)clock->second / 10;
        time[5] = (unsigned)clock->second % 10;
        if ((!memcmp(pane->name, "cal", 3) || !memcmp(pane->name, "zal", 3)) &&
            pane->name[3] >= '1' && pane->name[3] <= '8')
            return available_image(
                &textures->digits[digits[(unsigned)pane->name[3] - '1']]);
        if ((!memcmp(pane->name, "tim", 3) || !memcmp(pane->name, "zim", 3)) &&
            pane->name[3] >= '1' && pane->name[3] <= '6')
            return available_image(
                &textures->digits[time[(unsigned)pane->name[3] - '1']]);
        if (pane_key(pane, "week") || pane_key(pane, "zeek"))
            return available_image(
                &textures->weekdays[language][gc_date_time_weekday(clock)]);
    }
    if (group == GC_LAYOUT_OPTIONS_FACE) {
        int position = menu->settings.screen_position;
        unsigned absolute;

        if (position < GC_SCREEN_POSITION_MIN || position > GC_SCREEN_POSITION_MAX)
            return NULL;
        absolute = (unsigned)(position < 0 ? -position : position);
        if (pane_key(pane, "num1") || pane_key(pane, "zum1"))
            return available_image(&textures->digits[absolute / 10]);
        if (pane_key(pane, "num2") || pane_key(pane, "zum2"))
            return available_image(&textures->digits[absolute % 10]);
        if ((pane_key(pane, "arwl") || pane_key(pane, "zrwl")) && position >= 0)
            return NULL;
        if ((pane_key(pane, "arwr") || pane_key(pane, "zrwr")) && position <= 0)
            return NULL;
        if (pane_key(pane, "sond") || pane_key(pane, "zond")) {
            if (menu->settings.sound < GC_SOUND_MONO ||
                menu->settings.sound > GC_SOUND_STEREO)
                return NULL;
            return available_image(&textures->sound[language][menu->settings.sound]);
        }
    }
    return gc_texture_collection_image(&textures->collection, pane->texture);
}

bool gc_menu_card_number_quad(const GcMenuTextures *textures, unsigned number,
                              float center_x, float center_y, unsigned index,
                              GcLayoutVertex vertices[4]) {
    unsigned divisor = 1;
    unsigned count = 1;
    unsigned digit;
    float width;
    float height;
    float left;
    float top;
    float u0;
    float u1;
    GcLayoutVertex candidate[4];

    if (!textures || !vertices || !textures->card_numbers.rgba || number > 9999 ||
        !textures->card_numbers.width || textures->card_numbers.width % 11 ||
        !textures->card_numbers.height || !isfinite(center_x) || !isfinite(center_y))
        return false;
    while (number / divisor >= 10) {
        divisor *= 10;
        ++count;
    }
    if (index >= count)
        return false;
    for (unsigned position = 0; position < index; ++position)
        divisor /= 10;
    digit = number / divisor % 10;
    width = (float)(textures->card_numbers.width / 11);
    height = (float)textures->card_numbers.height;
    /* USA BS2 0x8131873c centers the digit centers, then 0x813185e0
     * subtracts half a cell and submits UVs into the original eleven-cell strip. */
    left = center_x - (width + 2) * (float)(count - 1) * 0.5f - width * 0.5f +
           (width + 2) * (float)index;
    top = center_y - height * 0.5f;
    u0 = (float)digit / 11;
    u1 = (float)(digit + 1) / 11;
    candidate[0] = (GcLayoutVertex){left, top, u0, 0};
    candidate[1] = (GcLayoutVertex){left + width, top, u1, 0};
    candidate[2] = (GcLayoutVertex){left + width, top + height, u1, 1};
    candidate[3] = (GcLayoutVertex){left, top + height, u0, 1};
    memcpy(vertices, candidate, sizeof(candidate));
    return true;
}

bool gc_menu_grid_quad(unsigned index, GcLayoutVertex vertices[4]) {
    float left;
    float top;

    if (!vertices || index >= GC_MENU_GRID_COLUMNS * GC_MENU_GRID_ROWS)
        return false;
    /* Native USA BS2 0x8130b124, EUR 0x8130af54: column outer loop. */
    left = (float)(index / GC_MENU_GRID_ROWS) * 32;
    top = (float)(index % GC_MENU_GRID_ROWS) * 32;
    vertices[0] = (GcLayoutVertex){left, top, 0, 0};
    vertices[1] = (GcLayoutVertex){left + 32, top, 2, 0};
    vertices[2] = (GcLayoutVertex){left + 32, top + 32, 2, 2};
    vertices[3] = (GcLayoutVertex){left, top + 32, 0, 2};
    return true;
}
