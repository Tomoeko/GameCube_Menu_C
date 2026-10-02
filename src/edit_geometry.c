#include "gamecube/edit_geometry.h"
#include "console_common/support/endian.h"
#include "gamecube/angle.h"
#include "native_constants.h"
#include "gamecube/layout.h"

#include <math.h>
#include <string.h>

enum { MOTION_HEADER_BYTES = 16, MOTION_POINT_BYTES = 36, MAP_HEADER_LIMIT = 1024 };

static bool motion_group(const GcEditMotion *motion, unsigned group,
                         const uint8_t **records, unsigned *count, unsigned *times) {
    size_t offset;
    size_t relative;
    const uint8_t *row;

    if (!motion || !motion->bytes || !records || !count || !times ||
        group >= motion->group_count || motion->group_count > 64 ||
        motion->group_offset > motion->byte_count ||
        (size_t)motion->group_count > (motion->byte_count - motion->group_offset) / 8)
        return false;
    offset = motion->group_offset + (size_t)group * 8;
    row = motion->bytes + offset;
    *count = cc_read_be16(row);
    *times = cc_read_be16(row + 2);
    relative = cc_read_be32(row + 4);
    if (!*count || *count > GC_EDIT_POINT_LIMIT || *times < 2 || *times > 64 ||
        relative > motion->byte_count - offset ||
        relative < (size_t)(motion->group_count - group) * 8 ||
        (size_t)*count > (motion->byte_count - offset - relative) / 8)
        return false;
    *records = row + relative;
    return true;
}

static bool motion_track(const GcEditMotion *motion, const uint8_t *record,
                         unsigned times, const uint8_t **indices) {
    size_t offset = (size_t)(record - motion->bytes);
    size_t relative = cc_read_be32(record + 4);
    if (offset > motion->byte_count || relative < 8 ||
        relative > motion->byte_count - offset ||
        (size_t)times > (motion->byte_count - offset - relative) / 2)
        return false;
    *indices = record + relative;
    for (unsigned index = 0; index < times; ++index)
        if (cc_read_be16(*indices + index * 2) >= motion->point_count)
            return false;
    return true;
}

bool gc_edit_motion_decode(const uint8_t *bytes, size_t byte_count,
                           GcEditMotion *motion) {
    GcEditMotion candidate;

    if (!bytes || !motion || byte_count < MOTION_HEADER_BYTES ||
        memcmp(bytes, "CAFD", 4) != 0)
        return false;
    candidate = (GcEditMotion){bytes,
                               byte_count,
                               cc_read_be32(bytes + 8),
                               0,
                               cc_read_be32(bytes + 12),
                               cc_read_be32(bytes + 4)};
    if (!candidate.group_count || candidate.group_count > 64 ||
        candidate.point_offset < MOTION_HEADER_BYTES ||
        candidate.point_offset >= candidate.group_offset ||
        candidate.group_offset > byte_count ||
        (candidate.group_offset - candidate.point_offset) % MOTION_POINT_BYTES)
        return false;
    candidate.point_count =
        (candidate.group_offset - candidate.point_offset) / MOTION_POINT_BYTES;
    if (!candidate.point_count || candidate.point_count > UINT16_MAX)
        return false;
    for (size_t index = 0; index < candidate.point_count; ++index)
        for (unsigned component = 0; component < 9; ++component) {
            float value = cc_read_be_float(bytes + candidate.point_offset +
                                           index * MOTION_POINT_BYTES + component * 4);
            if (!isfinite(value) || fabsf(value) > 1000000)
                return false;
        }
    for (unsigned group = 0; group < candidate.group_count; ++group) {
        const uint8_t *records;
        unsigned count;
        unsigned times;
        if (!motion_group(&candidate, group, &records, &count, &times))
            return false;
        for (unsigned index = 0; index < count; ++index) {
            const uint8_t *indices;
            if (!motion_track(&candidate, records + index * 8, times, &indices))
                return false;
        }
    }
    *motion = candidate;
    return true;
}

bool gc_edit_motion_point(const GcEditMotion *motion, unsigned group, unsigned index,
                          float progress, float position[3]) {
    const uint8_t *records;
    const uint8_t *indices;
    const uint8_t *record;
    unsigned count;
    unsigned times;
    float delay;
    float time;
    unsigned segment;
    float duration;
    float local;
    const uint8_t *first;
    const uint8_t *second;

    if (!position || !isfinite(progress) ||
        !motion_group(motion, group, &records, &count, &times) || index >= count)
        return false;
    record = records + index * 8;
    if (!motion_track(motion, record, times, &indices))
        return false;
    /* Native sampler USA BS2 0x81328114: stagger delay 0.4*(1-byte/255),
     * followed by a 0.6 interval and evenly spaced Hermite knots. */
    delay = 0.4f * (1 - record[1] / 255.0f);
    time = (progress - delay) / 0.6f;
    if (time < 0)
        time = 0;
    if (time > 1)
        time = 1;
    duration = 1.0f / (float)(times - 1);
    segment = (unsigned)floorf(time / duration);
    if (segment >= times - 1)
        segment = times - 2;
    local = (time - (float)segment * duration) / duration;
    first = motion->bytes + motion->point_offset +
            (size_t)cc_read_be16(indices + segment * 2) * MOTION_POINT_BYTES;
    second = motion->bytes + motion->point_offset +
             (size_t)cc_read_be16(indices + (segment + 1) * 2) * MOTION_POINT_BYTES;
    if (time == 0 || time == 1) {
        const uint8_t *endpoint = time == 0 ? first : second;
        for (unsigned component = 0; component < 3; ++component)
            position[component] = cc_read_be_float(endpoint + component * 4);
        return true;
    }
    for (unsigned component = 0; component < 3; ++component) {
        float square = local * local;
        float cube = square * local;
        float start = cc_read_be_float(first + component * 4);
        float end = cc_read_be_float(second + component * 4);
        float outgoing = cc_read_be_float(first + 24 + component * 4);
        float incoming = cc_read_be_float(second + 12 + component * 4);
        position[component] = (2 * cube - 3 * square + 1) * start +
                              (cube - 2 * square + local) * duration * outgoing +
                              (-2 * cube + 3 * square) * end +
                              (cube - square) * duration * incoming;
    }
    return true;
}

static bool map_list(const GcIplResource *map, size_t field, const uint8_t **indices,
                     unsigned *count) {
    size_t header;
    size_t relative;
    if (!map || !map->bytes || map->byte_count < 4 || !indices || !count)
        return false;
    header = cc_read_be16(map->bytes + 2);
    if (header < 0xd4 || header > MAP_HEADER_LIMIT || header % 4 ||
        header > map->byte_count || field >= header || field % 4)
        return false;
    *count = cc_read_be16(map->bytes + field);
    relative = cc_read_be16(map->bytes + field + 2);
    if (!*count || *count > GC_EDIT_POINT_LIMIT || relative < header - field ||
        relative > map->byte_count - field ||
        (size_t)*count > (map->byte_count - field - relative) / 2)
        return false;
    *indices = map->bytes + field + relative;
    return true;
}

static bool map_valid(const GcIplResource *map) {
    size_t header;
    if (!map || !map->bytes || map->byte_count < 4)
        return false;
    header = cc_read_be16(map->bytes + 2);
    if (header < 0xd4 || header > MAP_HEADER_LIMIT || header % 4)
        return false;
    for (size_t field = 0; field < header; field += 4) {
        const uint8_t *indices;
        unsigned count;
        if (!map_list(map, field, &indices, &count))
            return false;
        for (unsigned index = 0; index < count; ++index)
            if (cc_read_be16(indices + index * 2) >= GC_EDIT_POINT_LIMIT)
                return false;
    }
    return true;
}

static bool decode_palettes(const GcText *text, GcEditGeometry *geometry) {
    uint32_t registers[32] = {0};
    bool known[32] = {false};
    bool found[2][3][2][3] = {false};
    bool disc_found[2][3] = {false};
    size_t begin = text->europe ? 0x11c84 : 0x111cc;
    size_t end = text->europe ? 0x12078 : 0x11588;
    unsigned color_begin = text->europe ? 0x136 : 0xfe;
    unsigned disc_begin = text->europe ? 0x12a : 0xf2;

    if (end > text->rom_size)
        return false;
    /* Original initializer writes the palette with li/sth pairs. Read those
     * bounded instructions rather than embed its color-table bytes. */
    for (size_t offset = begin; offset + 4 <= end; offset += 4) {
        uint32_t word = cc_read_be32(text->rom + offset);
        unsigned opcode = word >> 26;
        unsigned target = (word >> 21) & 31;
        unsigned base = (word >> 16) & 31;
        int32_t immediate = (int16_t)(word & 0xffff);
        if (opcode == 14 || opcode == 15) {
            if (!base || known[base]) {
                uint32_t addend =
                    opcode == 15 ? (uint32_t)immediate << 16 : (uint32_t)immediate;
                registers[target] = (base ? registers[base] : 0) + addend;
                known[target] = true;
            } else {
                known[target] = false;
            }
        } else if (opcode == 18 && (word & 1)) {
            known[0] = false;
            for (unsigned index = 3; index <= 12; ++index)
                known[index] = false;
        } else if (opcode == 44 && base == 31 && known[target] &&
                   immediate >= (int32_t)disc_begin &&
                   immediate < (int32_t)disc_begin + 12) {
            unsigned index = (unsigned)(immediate - (int32_t)disc_begin) / 2;
            if (immediate % 2 || registers[target] > 255)
                return false;
            geometry->disc_colors[index % 2][index / 2] = (uint8_t)registers[target];
            disc_found[index % 2][index / 2] = true;
        } else if (opcode == 44 && base == 31 && known[target] &&
                   immediate >= (int32_t)color_begin &&
                   immediate < (int32_t)color_begin + 72) {
            unsigned index = (unsigned)(immediate - (int32_t)color_begin) / 2;
            unsigned panel = index / 18;
            unsigned palette = index / 6 % 3;
            unsigned channel = index % 6 / 2;
            unsigned reg = index % 2;
            if (immediate % 2 || registers[target] > 255)
                return false;
            geometry->palettes[panel][palette][reg][channel] =
                (uint8_t)registers[target];
            found[panel][palette][reg][channel] = true;
        }
    }
    for (unsigned reg = 0; reg < 2; ++reg) {
        for (unsigned channel = 0; channel < 3; ++channel)
            if (!disc_found[reg][channel])
                return false;
        geometry->disc_colors[reg][3] = 255;
    }
    for (unsigned panel = 0; panel < 2; ++panel)
        for (unsigned palette = 0; palette < 3; ++palette)
            for (unsigned reg = 0; reg < 2; ++reg) {
                for (unsigned channel = 0; channel < 3; ++channel)
                    if (!found[panel][palette][reg][channel])
                        return false;
                geometry->palettes[panel][palette][reg][3] = 255;
            }
    return true;
}

/* Native initializer lfs/stfs pairs; only bounded original constant loads are accepted.
 */
static bool native_float_stores(const GcText *text, size_t begin, size_t end, size_t r2,
                                unsigned base_register, float values[128],
                                bool found[128]) {
    float registers[32] = {0};
    bool known[32] = {false};
    if (begin > end || end > text->rom_size)
        return false;
    for (size_t offset = begin; offset + 4 <= end; offset += 4) {
        uint32_t word = cc_read_be32(text->rom + offset);
        unsigned opcode = word >> 26, target = word >> 21 & 31, base = word >> 16 & 31;
        int32_t immediate = (int16_t)word;
        if (opcode == 48 && base == 2) {
            int64_t address = (int64_t)r2 + immediate;
            if (address < 0 || (uint64_t)address + 4 > text->rom_size)
                return false;
            registers[target] = cc_read_be_float(text->rom + (size_t)address);
            known[target] = isfinite(registers[target]);
        } else if (opcode == 52 && base == base_register && known[target] &&
                   immediate >= 0 && immediate < 512 && immediate % 4 == 0) {
            values[(unsigned)immediate / 4] = registers[target];
            found[(unsigned)immediate / 4] = true;
        }
    }
    return true;
}

static bool decode_options_parts(const GcText *text, GcEditGeometry *geometry) {
    float values[128] = {0};
    bool found[128] = {false};
    size_t r2 = text->europe ? 0x1b6340 : 0x1664e0;
    size_t begin = text->europe ? 0x11c84 : 0x111cc;
    size_t end = text->europe ? 0x11ff4 : 0x113ac;
    if (!native_float_stores(text, begin, end, r2, 31, values, found))
        return false;
    /* PAL always calls this final layout adjustment, after its constructor. */
    if (text->europe &&
        !native_float_stores(text, 0x11c0c, 0x11c84, r2, 3, values, found))
        return false;
    for (unsigned group = 0; group < (text->europe ? 3u : 2u); ++group) {
        unsigned field = 0x40 + group * 16;
        if (!found[field / 4] || values[field / 4] < 0 || values[field / 4] > 1)
            return false;
        geometry->option_growth[group] = values[field / 4];
    }
    unsigned first = text->europe ? 0xf4 : 0xcc;
    unsigned amplitude = first - 4;
    if (!found[amplitude / 4])
        return false;
    geometry->arrow_amplitude = values[amplitude / 4];
    for (unsigned arrow = 0; arrow < (text->europe ? 6u : 4u); ++arrow)
        for (unsigned axis = 0; axis < 2; ++axis) {
            unsigned field = first + arrow * 8 + axis * 4;
            if (!found[field / 4] || fabsf(values[field / 4]) > 1000)
                return false;
            geometry->arrow_offsets[arrow][axis] = values[field / 4];
        }
    if (!gc_native_halfwords(text, begin, end, 31, text->europe ? 0xec : 0xc4, 1,
                             &geometry->arrow_ticks) ||
        !geometry->arrow_ticks || geometry->arrow_ticks > 1000 ||
        !gc_native_halfwords(text, begin, end, 31, text->europe ? 0x124 : 0xec, 2,
                             geometry->screen_bar_ticks) ||
        !geometry->screen_bar_ticks[0] || !geometry->screen_bar_ticks[1])
        return false;
    uint16_t channels[3];
    size_t caption_begin = text->europe ? 0x21f98 : 0x20ec0;
    size_t caption_end = text->europe ? 0x22008 : 0x20f10;
    if (!gc_native_halfwords(text, caption_begin, caption_end, 3, 0x10, 3, channels))
        return false;
    for (unsigned i = 0; i < 3; ++i)
        if (channels[i] > 255)
            return false;
    geometry->option_heading_color = (uint32_t)channels[0] << 24 |
                                     (uint32_t)channels[1] << 16 |
                                     (uint32_t)channels[2] << 8 | 255;
    if (text->europe) {
        memset(found, 0, sizeof(found));
        if (!native_float_stores(text, caption_begin, caption_end, r2, 3, values,
                                 found))
            return false;
        for (unsigned i = 0; i < 4; ++i) {
            if (!found[(0x24 + 4 * i) / 4])
                return false;
            geometry->option_caption_offsets[i] = values[(0x24 + 4 * i) / 4];
        }
        /* Original 27f08 reads the six lan pane x centers for its underline. */
        GcLayoutTable table;
        if (!gc_layout_table_decode(text->rom + 0xd24a0, text->rom_size - 0xd24a0,
                                    &table))
            return false;
        for (unsigned i = 0; i < 6; ++i) {
            char name[5] = {'l', 'a', 'n', (char)('1' + i), 0};
            GcLayoutPane pane;
            if (!gc_layout_find_pane(&table, name, 0, &pane))
                return false;
            geometry->language_centers[i] = truncf(pane.box.center_x) - (i < 4 ? 2 : 1);
        }
        geometry->language_transform[0] = 0;
        geometry->language_transform[1] = cc_read_be_float(text->rom + r2 - 0x7d68);
        geometry->language_transform[2] = cc_read_be_float(text->rom + r2 - 0x7d4c);
        geometry->language_origin[0] = cc_read_be_float(text->rom + r2 - 0x7a4c);
        geometry->language_origin[1] = cc_read_be_float(text->rom + r2 - 0x7a48);
        for (unsigned i = 0; i < 2; ++i) {
            uint64_t bits = cc_read_be64(text->rom + r2 - (i ? 0x7a38 : 0x7a40));
            memcpy(&geometry->language_shift[i], &bits, sizeof(bits));
            if (!isfinite(geometry->language_shift[i]))
                return false;
        }
    }
    return true;
}

static bool decode_disc_launch(const GcText *text, GcEditGeometry *geometry,
                               size_t r2) {
    static const unsigned offsets[2][8] = {
        {0x7aa4, 0x7a88, 0x7aa0, 0x7a9c, 0x7a98, 0x7a94, 0x7a90, 0x7a8c},
        {0x7a0c, 0x7a30, 0x7a08, 0x7a04, 0x7a00, 0x79fc, 0x79f8, 0x79f4}};
    float values[8];
    for (unsigned index = 0; index < 8; ++index) {
        size_t offset = r2 - offsets[text->europe][index];
        if (offset > text->rom_size || text->rom_size - offset < 4)
            return false;
        values[index] = cc_read_be_float(text->rom + offset);
        if (!isfinite(values[index]) || fabsf(values[index]) > 1000)
            return false;
    }
    if (values[0] <= 0 || values[0] >= 1 || values[1] <= 0 || values[1] > 100 ||
        values[2] < 0 || values[2] >= 1 || values[3] <= 0 || values[3] > 10 ||
        values[4] < 0 || values[4] > 1)
        return false;
    /* USA 2d504 / PAL 301ac loads the per-block delay table with lis/addi.
     * Recover the address from the instructions, retaining no extracted table. */
    size_t code = text->europe ? 0x309cc : 0x2dd24;
    if (code > text->rom_size || text->rom_size - code < 8)
        return false;
    uint32_t high = cc_read_be32(text->rom + code);
    uint32_t low = cc_read_be32(text->rom + code + 4);
    if (high >> 26 != 15 || (high >> 16 & 31) != 0 || low >> 26 != 14 ||
        (low >> 16 & 31) != (high >> 21 & 31))
        return false;
    int64_t address = (int64_t)(uint32_t)((high & 0xffff) << 16) + (int16_t)low;
    int64_t file = address - (int64_t)GC_IPL_BS2_ADDRESS + (int64_t)GC_IPL_BS2_OFFSET;
    const uint8_t *records;
    unsigned count, times;
    if (!motion_group(&geometry->motion, 1, &records, &count, &times) || file < 0 ||
        (uint64_t)file > text->rom_size || count > text->rom_size - (size_t)file)
        return false;
    geometry->disc_launch =
        (GcEditDiscLaunch){.compression_end = values[0],
                           .compression_time = values[1],
                           .compression_amount = values[2],
                           .fade_speed = values[3],
                           .delay_span = values[4],
                           .translation = {values[5], values[6], -values[7]},
                           .delays = text->rom + (size_t)file,
                           .point_count = count};
    return true;
}

bool gc_edit_geometry_decode(const GcText *text, GcEditGeometry *geometry) {
    GcEditGeometry candidate = {0};
    GcIplResourceTable table;
    static const unsigned europe_maps[6] = {182, 184, 181, 179, 183, 180};
    static const unsigned ntsc_float_offsets[5][3] = {{0x7d88, 0x7d58, 0x7d54},
                                                      {0x7d60, 0x7d4c, 0x7d54},
                                                      {0x7d60, 0x7d48, 0x7d54},
                                                      {0x7d60, 0x7d44, 0x7d54},
                                                      {0x7d40, 0x7d40, 0x7d3c}};
    /* EUR initializer 0x81311464 finishes with 0x813113ec. The final
     * three-row layout overrides the earlier two-row defaults. */
    static const unsigned europe_float_offsets[5][3] = {{0x7d68, 0x7d44, 0x7d40},
                                                        {0x7d18, 0x7d3c, 0x7d38},
                                                        {0x7d18, 0x7cec, 0x7d48},
                                                        {0x7d18, 0x7d00, 0x7d48},
                                                        {0x7ce8, 0x7ce8, 0x7ce4}};
    size_t r2_offset;

    if (!text || !text->rom || !geometry || geometry->motion_owner.bytes)
        return false;
    for (unsigned language = 0; language < 7; ++language)
        if (geometry->maps[language].bytes)
            return false;
    candidate.europe = text->europe;
    if (!gc_ipl_resource_table_decode(text->rom, text->rom_size,
                                      text->europe ? 0x82040 : 0x5f240, &table) ||
        !gc_ipl_resource_unpack(&table, text->europe ? 37 : 28,
                                &candidate.motion_owner) ||
        !gc_edit_motion_decode(candidate.motion_owner.bytes,
                               candidate.motion_owner.byte_count, &candidate.motion))
        goto fail;
    for (unsigned language = 0; language < 7; ++language) {
        unsigned resource;
        if (text->europe) {
            if (language == GC_LANGUAGE_JAPANESE)
                continue;
            resource = europe_maps[language];
        } else {
            if (language != GC_LANGUAGE_ENGLISH && language != GC_LANGUAGE_JAPANESE)
                continue;
            resource = language == GC_LANGUAGE_JAPANESE ? 81 : 80;
        }
        if (!gc_ipl_resource_unpack(&table, resource, &candidate.maps[language]) ||
            !map_valid(&candidate.maps[language]))
            goto fail;
    }
    r2_offset = text->europe ? 0x1b6340 : 0x1664e0;
    /* USA 0x8132f9a0 / EUR equivalent reads the original constructor's
     * six motion constants. Rotation conversion uses 65535 native units. */
    static const unsigned motion_offsets[2][6] = {
        {0x7d34, 0x7d5c, 0x7d30, 0x7d2c, 0x7d28, 0x7d20},
        {0x7ce0, 0x7d14, 0x7cdc, 0x7cd8, 0x7cd4, 0x7ccc}};
    for (unsigned index = 0; index < 6; ++index) {
        size_t offset = r2_offset - motion_offsets[text->europe][index];
        if (offset > text->rom_size || text->rom_size - offset < 4)
            goto fail;
        candidate.motion_profile[index] = cc_read_be_float(text->rom + offset);
        if (!isfinite(candidate.motion_profile[index]) ||
            candidate.motion_profile[index] < 0 || candidate.motion_profile[index] > 90)
            goto fail;
    }
    if (candidate.motion_profile[4] <= 0 || candidate.motion_profile[4] >= 1 ||
        candidate.motion_profile[5] >= 1)
        goto fail;
    size_t angle_offset = r2_offset - (text->europe ? 0x7a60 : 0x7ac0);
    if (angle_offset > text->rom_size || text->rom_size - angle_offset < 4)
        goto fail;
    candidate.angle_units = cc_read_be_float(text->rom + angle_offset);
    if (!isfinite(candidate.angle_units) || candidate.angle_units < 65534 ||
        candidate.angle_units > 65536)
        goto fail;
    if (!decode_disc_launch(text, &candidate, r2_offset))
        goto fail;
    for (unsigned group = 0; group < 5; ++group)
        for (unsigned axis = 0; axis < 3; ++axis) {
            unsigned relative = text->europe ? europe_float_offsets[group][axis]
                                             : ntsc_float_offsets[group][axis];
            size_t offset = r2_offset - relative;
            if (offset > text->rom_size || text->rom_size - offset < 4)
                goto fail;
            candidate.transforms[group][axis] = cc_read_be_float(text->rom + offset);
            if (!isfinite(candidate.transforms[group][axis]) ||
                fabsf(candidate.transforms[group][axis]) > 1000 ||
                (axis == 2 && candidate.transforms[group][axis] <= 0))
                goto fail;
        }
    if (!decode_palettes(text, &candidate) || !decode_options_parts(text, &candidate))
        goto fail;
    uint16_t limits[2];
    size_t init_begin = text->europe ? 0x11c84 : 0x111cc;
    size_t init_end = text->europe ? 0x12078 : 0x11588;
    if (!gc_native_halfwords(text, init_begin, init_end, 31, text->europe ? 0xa8 : 0x80,
                             2, limits) ||
        !limits[0] || limits[0] > 255 || limits[1] != limits[0] ||
        !gc_native_halfwords(text, init_begin, init_end, 31, text->europe ? 0xd6 : 0xae,
                             1, &candidate.entrance_ticks) ||
        !candidate.entrance_ticks || candidate.entrance_ticks > 1000)
        goto fail;
    candidate.selection_ticks = limits[0];
    if (!gc_native_halfwords(text, init_begin, init_end, 31, text->europe ? 0xc8 : 0xa0,
                             1, &candidate.motion_ticks) ||
        !candidate.motion_ticks || candidate.motion_ticks > 1000)
        goto fail;
    if (!gc_native_halfwords(text, text->europe ? 0xb30c : 0xb4e8,
                             text->europe ? 0xb334 : 0xb510, 3, 2, 1,
                             &candidate.language_fade_ticks) ||
        !candidate.language_fade_ticks || candidate.language_fade_ticks > 255)
        goto fail;
    *geometry = candidate;
    return true;
fail:
    gc_edit_geometry_destroy(&candidate);
    return false;
}

void gc_edit_geometry_destroy(GcEditGeometry *geometry) {
    if (!geometry)
        return;
    gc_ipl_resource_destroy(&geometry->motion_owner);
    for (unsigned language = 0; language < 7; ++language)
        gc_ipl_resource_destroy(&geometry->maps[language]);
    memset(geometry, 0, sizeof(*geometry));
}

gc_language gc_edit_geometry_map_language(const gc_menu *menu) {
    if (!menu)
        return GC_LANGUAGE_ENGLISH;
    /* PAL MAP accessors, including 31008, query the committed global locale.
     * The six preview captions use their independent candidate faders. */
    return menu->page == GC_PAGE_OPTIONS && menu->editing && menu->editor_index == 2
               ? menu->settings_before_edit.language
               : menu->settings.language;
}

bool gc_edit_geometry_colors(const GcEditGeometry *geometry, bool calendar,
                             float selection_weight, float base_weight,
                             uint8_t colors[2][4]) {
    uint8_t result[2][4];
    unsigned panel = calendar ? 1 : 0;
    if (!geometry || !colors || !isfinite(selection_weight) || !isfinite(base_weight) ||
        selection_weight < 0 || selection_weight > 1 || base_weight < 0 ||
        base_weight > 1)
        return false;
    /* USA 0x8132d870 / EUR 0x81330518 performs a single-precision blend,
     * then truncates each component toward zero. */
    for (unsigned reg = 0; reg < 2; ++reg)
        for (unsigned channel = 0; channel < 4; ++channel) {
            float selected = geometry->palettes[panel][1][reg][channel];
            float dim = geometry->palettes[panel][2][reg][channel];
            float base = geometry->palettes[panel][0][reg][channel];
            float value =
                base * base_weight + (1 - base_weight) * (selected * selection_weight +
                                                          dim * (1 - selection_weight));
            result[reg][channel] = (uint8_t)value;
        }
    memcpy(colors, result, sizeof(result));
    return true;
}

static bool mark_list(const GcIplResource *map, size_t field, bool *marked,
                      unsigned point_count) {
    const uint8_t *indices;
    unsigned count;
    if (!map_list(map, field, &indices, &count))
        return false;
    for (unsigned index = 0; index < count; ++index) {
        unsigned point = cc_read_be16(indices + index * 2);
        if (point >= point_count)
            return false;
        marked[point] = true;
    }
    return true;
}

static bool mark_digit(const GcIplResource *map, size_t field, unsigned digit,
                       bool *marked, unsigned point_count) {
    const uint8_t *positions;
    const uint8_t *pattern;
    unsigned position_count;
    unsigned pattern_count;
    if (digit > 9 || !map_list(map, field, &positions, &position_count) ||
        !map_list(map, 0x78 + (size_t)digit * 4, &pattern, &pattern_count))
        return false;
    for (unsigned index = 0; index < pattern_count; ++index) {
        unsigned cell = cc_read_be16(pattern + index * 2);
        unsigned point;
        if (cell >= position_count)
            return false;
        point = cc_read_be16(positions + cell * 2);
        if (point >= point_count)
            return false;
        marked[point] = true;
    }
    return true;
}

static bool transform_group(const GcIplResource *map, size_t field, GcEditPoint *points,
                            unsigned count, const float transform[3],
                            GcEditField identity, bool *members) {
    const uint8_t *indices;
    unsigned size;
    float center[3] = {0};
    if (!map_list(map, field, &indices, &size))
        return false;
    for (unsigned index = 0; index < size; ++index) {
        unsigned point = cc_read_be16(indices + index * 2);
        if (point >= count)
            return false;
        for (unsigned axis = 0; axis < 3; ++axis)
            center[axis] += points[point].position[axis] / (float)size;
    }
    for (unsigned index = 0; index < size; ++index) {
        unsigned point = cc_read_be16(indices + index * 2);
        if (members && members[point])
            continue;
        if (members)
            members[point] = true;
        for (unsigned axis = 0; axis < 3; ++axis)
            points[point].position[axis] =
                center[axis] + (points[point].position[axis] - center[axis] +
                                (axis < 2 ? transform[axis] : 0)) *
                                   transform[2];
        points[point].scale *= transform[2];
        points[point].field = identity;
    }
    return true;
}

bool gc_edit_point_matrix(const GcEditPoint *point, float matrix[12]) {
    if (!point || !matrix)
        return false;
    for (unsigned axis = 0; axis < 3; ++axis)
        if (!isfinite(point->position[axis]))
            return false;
    float sx = gc_angle_sine(point->angles[0]), cx = gc_angle_cosine(point->angles[0]);
    float sy = gc_angle_sine(point->angles[1]), cy = gc_angle_cosine(point->angles[1]);
    float sz = gc_angle_sine(point->angles[2]), cz = gc_angle_cosine(point->angles[2]);
    float result[12] = {cz * cy,
                        -sz * cx + sx * cz * sy,
                        sz * sx + cx * cz * sy,
                        point->position[0],
                        sz * cy,
                        cz * cx + sx * sz * sy,
                        -cz * sx + cx * sz * sy,
                        point->position[1],
                        -sy,
                        cy * sx,
                        cy * cx,
                        point->position[2]};
    memcpy(matrix, result, sizeof(result));
    return true;
}

/* Native 0x8132fb54 traverses two zero-tangent Hermite intervals. */
static float oscillate(float split, float phase, float amplitude) {
    float time = phase < split ? phase / split : (phase - split) / (1 - split);
    float blend = time * time * (3 - 2 * time);
    return phase < split ? amplitude * (2 * blend - 1) : amplitude * (1 - 2 * blend);
}

static bool sample_motion(const GcEditGeometry *geometry, const GcEditState *state,
                          float shift[2], int16_t angles[3]) {
    if (!geometry->motion_ticks || state->sampled_motion >= geometry->motion_ticks)
        return false;
    float phase = (float)state->sampled_motion / geometry->motion_ticks;
    float first = phase + geometry->motion_profile[4] * 0.5f;
    first -= floorf(first);
    float second = 0.25f + 2 * phase;
    second -= floorf(second);
    float third = phase + 0.25f;
    third -= floorf(third);
    shift[0] =
        oscillate(geometry->motion_profile[4], first, geometry->motion_profile[0]);
    shift[1] = oscillate(0.5f, second, geometry->motion_profile[1]);
    angles[0] = 0;
    angles[1] =
        (int16_t)truncf(geometry->angle_units *
                        oscillate(0.5f, third, geometry->motion_profile[2]) / 360);
    angles[2] =
        (int16_t)truncf(geometry->angle_units *
                        oscillate(0.5f, third, geometry->motion_profile[3]) / 360);
    return true;
}

static bool move_group(const GcIplResource *map, size_t field, GcEditPoint *points,
                       unsigned count, const GcEditGeometry *geometry,
                       const GcEditState *state, const float shift[2],
                       const int16_t angles[3]) {
    const uint8_t *indices;
    unsigned size;
    GcEditPoint rotation = {.angles = {angles[0], angles[1], angles[2]}};
    float matrix[12];
    float center[3] = {0};
    if (!map_list(map, field, &indices, &size) ||
        !gc_edit_point_matrix(&rotation, matrix))
        return false;
    for (unsigned index = 0; index < size; ++index) {
        unsigned point = cc_read_be16(indices + index * 2);
        if (point >= count)
            return false;
        for (unsigned axis = 0; axis < 3; ++axis)
            center[axis] += points[point].position[axis] / (float)size;
    }
    for (unsigned index = 0; index < size; ++index) {
        unsigned point = cc_read_be16(indices + index * 2);
        float weight = (float)state->movement[point] / geometry->selection_ticks;
        if (weight < 0 || weight > 1)
            return false;
        float relative[3];
        for (unsigned axis = 0; axis < 3; ++axis)
            relative[axis] = points[point].position[axis] - center[axis];
        for (unsigned axis = 0; axis < 3; ++axis) {
            float rotated = matrix[axis * 4] * relative[0] +
                            matrix[axis * 4 + 1] * relative[1] +
                            matrix[axis * 4 + 2] * relative[2];
            points[point].position[axis] +=
                weight * (rotated - relative[axis] + (axis < 2 ? shift[axis] : 0));
            points[point].angles[axis] = (int16_t)truncf((float)angles[axis] * weight);
        }
    }
    return true;
}

static bool move_values(const GcIplResource *map, GcEditPoint *points, unsigned count,
                        const GcEditGeometry *geometry, const gc_menu *menu,
                        const GcEditState *state, float progress) {
    if (!state || !state->active || state->page != menu->page ||
        progress <= geometry->motion_profile[5])
        return true;
    float shift[2];
    int16_t angles[3];
    if (!sample_motion(geometry, state, shift, angles))
        return false;
    static const size_t calendar_fields[18] = {8,    12,   16,   20,   0x5c, 0x60,
                                               0x64, 0x68, 0x44, 0x48, 0x20, 0x24,
                                               0x2c, 0x30, 0x38, 0x3c, 0x50, 0x54};
    if (menu->page == GC_PAGE_CALENDAR) {
        for (unsigned group = 0; group < 18; ++group)
            if (!move_group(map, calendar_fields[group], points, count, geometry, state,
                            shift, angles))
                return false;
    } else {
        size_t fields[5] = {0xa0, geometry->europe ? 0xb8 : 0xb4,
                            geometry->europe ? 0xbc : 0xb8,
                            geometry->europe ? 0xd8 : 0xcc, 0xac};
        for (unsigned group = 0; group < (geometry->europe ? 5u : 4u); ++group)
            if (!move_group(map, fields[group], points, count, geometry, state, shift,
                            angles))
                return false;
    }
    return true;
}

static bool option_arrow_targets(const GcEditGeometry *geometry, const gc_menu *menu,
                                 bool targets[6]) {
    memset(targets, 0, 6 * sizeof(*targets));
    if (menu->page != GC_PAGE_OPTIONS || !menu->editing)
        return true;
    if (menu->editor_index == 0) {
        targets[0] = menu->settings.sound == GC_SOUND_STEREO;
        targets[1] = menu->settings.sound == GC_SOUND_MONO;
    } else if (menu->editor_index == 1) {
        targets[2] = menu->settings.screen_position > GC_SCREEN_POSITION_MIN;
        targets[3] = menu->settings.screen_position < GC_SCREEN_POSITION_MAX;
    } else if (geometry->europe && menu->editor_index == 2) {
        targets[4] = menu->settings.language > GC_LANGUAGE_ENGLISH;
        targets[5] = menu->settings.language < GC_LANGUAGE_DUTCH;
    }
    return true;
}

static bool group_center(const GcIplResource *map, size_t field, GcEditPoint *points,
                         unsigned count, float center[3]) {
    const uint8_t *ids;
    unsigned size;
    memset(center, 0, 3 * sizeof(*center));
    if (!map_list(map, field, &ids, &size))
        return false;
    for (unsigned i = 0; i < size; ++i) {
        unsigned point = cc_read_be16(ids + i * 2);
        if (point >= count)
            return false;
        for (unsigned axis = 0; axis < 3; ++axis)
            center[axis] += points[point].position[axis] / (float)size;
    }
    return true;
}

static bool grow_option_group(const GcIplResource *map, size_t field,
                              GcEditPoint *points, unsigned count,
                              const GcEditGeometry *geometry, const GcEditState *state,
                              unsigned group, bool selected) {
    const uint8_t *ids;
    unsigned size;
    float center[3];
    if (!map_list(map, field, &ids, &size) ||
        !group_center(map, field, points, count, center))
        return false;
    for (unsigned i = 0; i < size; ++i) {
        unsigned point = cc_read_be16(ids + i * 2);
        float weight = state ? (float)state->growth[group] / geometry->selection_ticks
                             : (selected ? 1 : 0);
        float scale = 1 + geometry->option_growth[group] * weight;
        for (unsigned axis = 0; axis < 3; ++axis)
            points[point].position[axis] =
                center[axis] + (points[point].position[axis] - center[axis]) * scale;
        points[point].scale *= scale;
    }
    return true;
}

static float language_position(const GcEditGeometry *geometry, const gc_menu *menu,
                               const GcEditState *state) {
    if (!state)
        return geometry->language_centers[menu->settings.language];
    float t = (float)state->language_movement / geometry->selection_ticks;
    float blend = t * t * (3 - 2 * t);
    return state->language_from + (state->language_to - state->language_from) * blend;
}

static bool option_extras(const GcEditGeometry *geometry, const gc_menu *menu,
                          const GcEditState *state, const GcIplResource *map,
                          GcEditPoint *points, unsigned count, bool visible[],
                          bool selected[], float progress) {
    size_t sound = geometry->europe ? 0xd8 : 0xcc;
    size_t position = geometry->europe ? 0xb4 : 0xb0;
    float centers[3][3];
    if (!group_center(map, sound, points, count, centers[0]) ||
        !group_center(map, position, points, count, centers[1]))
        return false;
    for (unsigned group = 0; group < 2; ++group)
        for (unsigned axis = 0; axis < 2; ++axis)
            centers[group][axis] -=
                geometry->transforms[group][axis] * geometry->transforms[group][2];
    float language_x = 0;
    if (geometry->europe) {
        const uint8_t *ids;
        unsigned size;
        if (!map_list(map, 0xac, &ids, &size) || !mark_list(map, 0xac, visible, count))
            return false;
        if (menu->editor_index == 2 && !mark_list(map, 0xac, selected, count))
            return false;
        if (!group_center(map, 0xac, points, count, centers[2]))
            return false;
        float origin[3] = {geometry->language_origin[0], geometry->language_origin[1],
                           centers[2][2]};
        /* Native 2d234 scales the underline about a fixed x/y origin. */
        float entrance = (progress - geometry->motion_profile[5]) /
                         (1 - geometry->motion_profile[5]);
        if (entrance < 0)
            entrance = 0;
        if (entrance > 1)
            entrance = 1;
        language_x = (float)(geometry->language_shift[0] +
                             geometry->language_shift[1] *
                                 language_position(geometry, menu, state)) *
                     entrance;
        for (unsigned i = 0; i < size; ++i) {
            unsigned point = cc_read_be16(ids + i * 2);
            if (point >= count)
                return false;
            for (unsigned axis = 0; axis < 3; ++axis)
                points[point].position[axis] =
                    origin[axis] +
                    (points[point].position[axis] - origin[axis] +
                     (axis < 2 ? geometry->language_transform[axis] : 0)) *
                        geometry->language_transform[2];
            points[point].field = GC_EDIT_LANGUAGE;
            points[point].scale = geometry->language_transform[2];
            points[point].position[0] += language_x;
        }
    }
    bool targets[6];
    if (!option_arrow_targets(geometry, menu, targets))
        return false;
    unsigned phase_tick = state ? state->sampled_arrow : 0;
    float phase = geometry->arrow_ticks ? (float)phase_tick / geometry->arrow_ticks : 0;
    float triangle = phase < .5f ? 2 * phase : 2 * (1 - phase);
    float motion = triangle * triangle * (3 - 2 * triangle);
    for (unsigned arrow = 0; arrow < (geometry->europe ? 6u : 4u); ++arrow) {
        const uint8_t *ids;
        unsigned size;
        size_t field = (geometry->europe ? 0xc0 : 0xbc) + arrow * 4;
        if (!map_list(map, field, &ids, &size))
            return false;
        unsigned group = arrow / 2;
        const float *transform =
            group == 2 ? geometry->language_transform : geometry->transforms[group];
        float scale = transform[2] * (1 + geometry->option_growth[group]);
        float opacity = state ? (float)state->arrows[arrow] / geometry->selection_ticks
                        : targets[arrow] ? 1
                                         : 0;
        for (unsigned i = 0; i < size; ++i) {
            unsigned point = cc_read_be16(ids + i * 2);
            if (point >= count)
                return false;
            visible[point] = opacity > 0;
            points[point].field = GC_EDIT_ARROW;
            points[point].scale = scale;
            points[point].alpha = (uint8_t)(255 * opacity * progress);
            for (unsigned axis = 0; axis < 3; ++axis) {
                float center = centers[group][axis];
                points[point].position[axis] =
                    center + (axis < 2 ? transform[axis] : 0) +
                    (points[point].position[axis] - center) * scale;
            }
            points[point].position[0] +=
                geometry->arrow_offsets[arrow][0] +
                (arrow % 2 ? 1 : -1) * geometry->arrow_amplitude * motion;
            points[point].position[1] += geometry->arrow_offsets[arrow][1];
        }
    }
    if (!grow_option_group(map, sound, points, count, geometry, state, 0,
                           menu->editing && menu->editor_index == 0) ||
        !grow_option_group(map, position, points, count, geometry, state, 1,
                           menu->editing && menu->editor_index == 1) ||
        (geometry->europe &&
         !grow_option_group(map, 0xac, points, count, geometry, state, 2,
                            menu->editing && menu->editor_index == 2)))
        return false;
    return true;
}

static size_t geometry_points(const GcEditGeometry *geometry, const gc_menu *menu,
                              float progress, const GcEditState *state,
                              GcEditPoint *points, size_t capacity,
                              bool *selection_targets, unsigned *selection_count,
                              bool include_hidden) {
    GcEditPoint candidate[GC_EDIT_POINT_LIMIT] = {0};
    bool visible[GC_EDIT_POINT_LIMIT] = {false};
    bool selected[GC_EDIT_POINT_LIMIT] = {false};
    bool transformed[GC_EDIT_POINT_LIMIT] = {false};
    const GcIplResource *map;
    const uint8_t *records;
    unsigned count;
    unsigned times;
    unsigned group;
    size_t output_count = 0;

    if (!geometry || !menu || !isfinite(progress) ||
        menu->settings.language < GC_LANGUAGE_ENGLISH ||
        menu->settings.language > GC_LANGUAGE_JAPANESE ||
        (unsigned)gc_edit_geometry_map_language(menu) > GC_LANGUAGE_JAPANESE ||
        (menu->page != GC_PAGE_CALENDAR && menu->page != GC_PAGE_OPTIONS))
        return 0;
    map = &geometry->maps[gc_edit_geometry_map_language(menu)];
    group = menu->page == GC_PAGE_CALENDAR ? 0 : 2;
    if (!motion_group(&geometry->motion, group, &records, &count, &times))
        return 0;
    for (unsigned index = 0; index < count; ++index) {
        candidate[index].source_index = index;
        candidate[index].scale = 1;
        candidate[index].alpha = (uint8_t)(255 * fmaxf(0, fminf(progress, 1)));
        if (!gc_edit_motion_point(&geometry->motion, group, index, progress,
                                  candidate[index].position))
            return 0;
    }
    if (menu->page == GC_PAGE_CALENDAR) {
        const gc_date_time *clock = &menu->clock;
        static const size_t digit_fields[14] = {0x5c, 0x60, 0x64, 0x68, 0x44,
                                                0x48, 0x20, 0x24, 0x2c, 0x30,
                                                0x38, 0x3c, 0x50, 0x54};
        unsigned values[14];
        static const size_t selected_fields[6] = {0x1c, 0x40, 0x58, 0x28, 0x34, 0x4c};
        if (!gc_date_time_valid(clock))
            return 0;
        values[0] = (unsigned)clock->year / 1000;
        values[1] = (unsigned)clock->year / 100 % 10;
        values[2] = (unsigned)clock->year / 10 % 10;
        values[3] = (unsigned)clock->year % 10;
        values[4] = (unsigned)clock->month / 10;
        values[5] = (unsigned)clock->month % 10;
        values[6] = (unsigned)clock->day / 10;
        values[7] = (unsigned)clock->day % 10;
        values[8] = (unsigned)clock->hour / 10;
        values[9] = (unsigned)clock->hour % 10;
        values[10] = (unsigned)clock->minute / 10;
        values[11] = (unsigned)clock->minute % 10;
        values[12] = (unsigned)clock->second / 10;
        values[13] = (unsigned)clock->second % 10;
        for (unsigned digit = 0; digit < 14; ++digit)
            if (!mark_digit(map, digit_fields[digit], values[digit], visible, count))
                return 0;
        if (!mark_list(map, 4, visible, count) ||
            !transform_group(map, 0x18, candidate, count, geometry->transforms[2],
                             GC_EDIT_PUNCTUATION, transformed) ||
            !transform_group(map, 0, candidate, count, geometry->transforms[3],
                             GC_EDIT_PUNCTUATION, transformed))
            return 0;
        for (unsigned field = 0; field < 6; ++field) {
            const uint8_t *ids;
            unsigned size;
            if (!map_list(map, selected_fields[field], &ids, &size))
                return 0;
            for (unsigned index = 0; index < size; ++index) {
                unsigned point = cc_read_be16(ids + index * 2);
                if (point >= count)
                    return 0;
                candidate[point].field = (GcEditField)(GC_EDIT_DAY + field);
            }
        }
        size_t selection = menu->editing ? selected_fields[gc_menu_calendar_field(menu)]
                           : menu->editor_index < 3 ? 0x18
                                                    : 0;
        if (!mark_list(map, selection, selected, count))
            return 0;
    } else {
        int position = menu->settings.screen_position;
        unsigned absolute;
        size_t position_field = geometry->europe ? 0xb4 : 0xb0;
        size_t tens_field = geometry->europe ? 0xb8 : 0xb4;
        size_t ones_field = geometry->europe ? 0xbc : 0xb8;
        size_t sound_field = geometry->europe ? 0xd8 : 0xcc;
        size_t mono_field = geometry->europe ? 0xb0 : 0xac;
        size_t stereo_field = geometry->europe ? 0xdc : 0xd0;
        if (position < GC_SCREEN_POSITION_MIN || position > GC_SCREEN_POSITION_MAX ||
            menu->settings.sound < GC_SOUND_MONO ||
            menu->settings.sound > GC_SOUND_STEREO)
            return 0;
        absolute = (unsigned)(position < 0 ? -position : position);
        if (!mark_digit(map, tens_field, absolute / 10, visible, count) ||
            !mark_digit(map, ones_field, absolute % 10, visible, count) ||
            !mark_list(map, position < 0 ? 0xa4 : 0xa8, visible, count) ||
            !mark_list(map,
                       menu->settings.sound == GC_SOUND_STEREO ? stereo_field
                                                               : mono_field,
                       visible, count) ||
            !transform_group(map, sound_field, candidate, count,
                             geometry->transforms[0], GC_EDIT_SOUND, transformed) ||
            !transform_group(map, position_field, candidate, count,
                             geometry->transforms[1], GC_EDIT_SCREEN_POSITION,
                             transformed))
            return 0;
        if (menu->editor_index < 2 &&
            !mark_list(map, menu->editor_index == 0 ? sound_field : position_field,
                       selected, count))
            return 0;
    }
    if (menu->page == GC_PAGE_OPTIONS &&
        !option_extras(geometry, menu, state, map, candidate, count, visible, selected,
                       progress))
        return 0;
    if (selection_targets && selection_count) {
        memcpy(selection_targets, selected, count * sizeof(selected[0]));
        *selection_count = count;
    }
    if (!move_values(map, candidate, count, geometry, menu, state, progress))
        return 0;
    for (unsigned index = 0; index < count; ++index) {
        if (!visible[index] && !include_hidden)
            continue;
        candidate[index].selected = selected[index];
        float selection_weight = selected[index] ? 1 : 0;
        if (state && geometry->selection_ticks && state->active &&
            state->page == menu->page)
            selection_weight =
                (float)state->selection[index] / geometry->selection_ticks;
        if (!gc_edit_geometry_colors(geometry, menu->page == GC_PAGE_CALENDAR,
                                     selection_weight,
                                     candidate[index].field == GC_EDIT_ARROW ||
                                             (selected[index] && menu->editing)
                                         ? 1
                                         : 0,
                                     candidate[index].colors))
            return 0;
        if (include_hidden && !visible[index])
            candidate[index].alpha = 0;
        ++output_count;
    }
    if (menu->page == GC_PAGE_OPTIONS && state && state->screen_bar_state) {
        unsigned bar_group = state->screen_bar_state == 1 ? 3 : 4;
        unsigned duration = geometry->screen_bar_ticks[state->screen_bar_state - 1];
        float time = (float)state->screen_bar_counter / duration;
        float opacity = state->screen_bar_state == 1 ? time : 1 - time;
        const uint8_t *bar_records;
        unsigned bar_count, bar_times;
        if (!motion_group(&geometry->motion, bar_group, &bar_records, &bar_count,
                          &bar_times) ||
            bar_count > GC_EDIT_POINT_LIMIT - count)
            return 0;
        for (unsigned i = 0; i < bar_count; ++i) {
            unsigned point = count + i;
            if (!gc_edit_motion_point(&geometry->motion, bar_group, i,
                                      state->screen_bar_state == 1 ? time : 1 - time,
                                      candidate[point].position))
                return 0;
            candidate[point].scale = 1;
            candidate[point].alpha = (uint8_t)(255 * opacity);
            candidate[point].source_index = GC_EDIT_POINT_LIMIT + i;
            candidate[point].field = GC_EDIT_SCREEN_BAR;
            memcpy(candidate[point].colors, geometry->palettes[0][0],
                   sizeof(candidate[point].colors));
            visible[point] = true;
        }
        count += bar_count;
        output_count += bar_count;
    }
    if (!points)
        return output_count;
    if (capacity < output_count)
        return 0;
    output_count = 0;
    for (unsigned index = 0; index < count; ++index)
        if (visible[index] || include_hidden)
            points[output_count++] = candidate[index];
    return output_count;
}

size_t gc_edit_geometry_points(const GcEditGeometry *geometry, const gc_menu *menu,
                               float progress, GcEditPoint *points, size_t capacity) {
    return geometry_points(geometry, menu, progress, NULL, points, capacity, NULL, NULL,
                           false);
}

void gc_edit_state_init(GcEditState *state) {
    if (state)
        memset(state, 0, sizeof(*state));
}

static uint16_t approach_counter(uint16_t current, uint16_t target, uint64_t ticks) {
    if (current < target) {
        uint16_t difference = (uint16_t)(target - current);
        return ticks >= difference ? target : (uint16_t)(current + ticks);
    }
    uint16_t difference = (uint16_t)(current - target);
    return ticks >= difference ? target : (uint16_t)(current - ticks);
}

static bool valid_edit_state(const GcEditState *state, const GcEditGeometry *geometry) {
    if (state->entrance_counter > geometry->entrance_ticks ||
        state->sampled_tick > geometry->entrance_ticks ||
        state->motion_counter >= geometry->motion_ticks ||
        state->sampled_motion >= geometry->motion_ticks ||
        state->disc_launch_motion >= geometry->motion_ticks)
        return false;
    if (state->screen_bar_state > 2 ||
        (state->screen_bar_state &&
         state->screen_bar_counter >
             geometry->screen_bar_ticks[state->screen_bar_state - 1]) ||
        state->language_movement > geometry->selection_ticks ||
        state->next_language_movement > geometry->selection_ticks ||
        (geometry->arrow_ticks && (state->arrow_counter >= geometry->arrow_ticks ||
                                   state->sampled_arrow >= geometry->arrow_ticks)))
        return false;
    for (unsigned arrow = 0; arrow < 6; ++arrow)
        if (state->arrows[arrow] > geometry->selection_ticks ||
            state->next_arrows[arrow] > geometry->selection_ticks)
            return false;
    for (unsigned row = 0; row < 3; ++row)
        if (state->growth[row] > geometry->selection_ticks ||
            state->next_growth[row] > geometry->selection_ticks)
            return false;
    for (unsigned index = 0; index < GC_EDIT_POINT_LIMIT; ++index)
        if (state->selection[index] > geometry->selection_ticks ||
            state->next_selection[index] > geometry->selection_ticks ||
            state->movement[index] > geometry->selection_ticks ||
            state->next_movement[index] > geometry->selection_ticks)
            return false;
    for (unsigned language = 0; language < 6; ++language)
        if (state->language_labels[language] > geometry->language_fade_ticks ||
            state->language_captions[language] > geometry->language_fade_ticks)
            return false;
    return true;
}

static bool advance_options_state(GcEditState *state, const GcEditGeometry *geometry,
                                  const gc_menu *menu, uint64_t ticks) {
    bool targets[6];
    if (!option_arrow_targets(geometry, menu, targets) || !geometry->arrow_ticks ||
        !geometry->screen_bar_ticks[0] || !geometry->screen_bar_ticks[1])
        return false;
    state->sampled_arrow =
        (uint16_t)((state->arrow_counter + (ticks - 1) % geometry->arrow_ticks) %
                   geometry->arrow_ticks);
    state->arrow_counter =
        (uint16_t)((state->arrow_counter + ticks % geometry->arrow_ticks) %
                   geometry->arrow_ticks);
    for (unsigned arrow = 0; arrow < 6; ++arrow) {
        uint16_t target = targets[arrow] ? geometry->selection_ticks : 0;
        state->arrows[arrow] =
            approach_counter(state->next_arrows[arrow], target, ticks - 1);
        state->next_arrows[arrow] =
            approach_counter(state->next_arrows[arrow], target, ticks);
    }
    for (unsigned row = 0; row < 3; ++row) {
        uint16_t target =
            menu->editing && menu->editor_index == row ? geometry->selection_ticks : 0;
        state->growth[row] =
            approach_counter(state->next_growth[row], target, ticks - 1);
        state->next_growth[row] =
            approach_counter(state->next_growth[row], target, ticks);
    }
    bool screen = menu->editing && menu->editor_index == 1;
    if (screen != state->screen_editing) {
        state->screen_bar_state = screen ? 1 : 2;
        state->screen_bar_counter = 0;
        state->screen_editing = screen;
    } else if (state->screen_bar_state) {
        unsigned duration = geometry->screen_bar_ticks[state->screen_bar_state - 1];
        state->screen_bar_counter =
            approach_counter(state->screen_bar_counter, (uint16_t)duration, ticks);
        if (state->screen_bar_state == 2 && state->screen_bar_counter == duration) {
            state->screen_bar_state = 0;
            state->screen_bar_counter = 0;
        }
    }
    if (geometry->europe) {
        if (state->sampled_language != menu->settings.language) {
            state->language_from = language_position(geometry, menu, state);
            state->language_to = geometry->language_centers[menu->settings.language];
            state->language_movement = state->next_language_movement = 0;
            state->sampled_language = menu->settings.language;
        }
        state->language_movement = approach_counter(
            state->next_language_movement, geometry->selection_ticks, ticks - 1);
        state->next_language_movement = approach_counter(
            state->next_language_movement, geometry->selection_ticks, ticks);
    }
    return true;
}

static void advance_editor_points(GcEditState *state, const GcEditGeometry *geometry,
                                  const bool *selected, const bool *movement,
                                  unsigned count, uint64_t ticks) {
    uint16_t duration = geometry->entrance_ticks;
    uint64_t remaining = duration - state->entrance_counter;
    unsigned first_motion =
        (unsigned)floorf((float)duration * geometry->motion_profile[5]) + 1;
    uint64_t wait_motion = state->entrance_counter < first_motion
                               ? first_motion - state->entrance_counter
                               : 0;
    if (ticks > wait_motion) {
        uint64_t updates = ticks - wait_motion;
        state->sampled_motion = (uint16_t)((state->motion_counter +
                                            (updates - 1) % geometry->motion_ticks) %
                                           geometry->motion_ticks);
        state->motion_counter =
            (uint16_t)((state->motion_counter + updates % geometry->motion_ticks) %
                       geometry->motion_ticks);
    }
    state->sampled_tick = ticks - 1 >= remaining
                              ? duration
                              : (uint16_t)(state->entrance_counter + ticks - 1);
    state->entrance_counter =
        ticks >= remaining ? duration : (uint16_t)(state->entrance_counter + ticks);
    for (unsigned index = 0; index < GC_EDIT_POINT_LIMIT; ++index) {
        uint16_t target =
            index < count && selected[index] ? geometry->selection_ticks : 0;
        state->selection[index] =
            approach_counter(state->next_selection[index], target, ticks - 1);
        state->next_selection[index] =
            approach_counter(state->next_selection[index], target, ticks);
        target = index < count && movement[index] ? geometry->selection_ticks : 0;
        state->movement[index] =
            approach_counter(state->next_movement[index], target, ticks - 1);
        state->next_movement[index] =
            approach_counter(state->next_movement[index], target, ticks);
    }
}

bool gc_edit_state_advance(GcEditState *state, const GcEditGeometry *geometry,
                           const gc_menu *menu, bool editor_ready, uint64_t ticks) {
    bool selected[GC_EDIT_POINT_LIMIT] = {false};
    bool movement[GC_EDIT_POINT_LIMIT] = {false};
    unsigned count = 0;
    GcEditState candidate;
    if (!state || !geometry || !menu || !geometry->selection_ticks ||
        !geometry->entrance_ticks || geometry->entrance_ticks > 1000 ||
        geometry->selection_ticks > 255 || !geometry->motion_ticks ||
        geometry->motion_ticks > 1000 || !isfinite(geometry->motion_profile[5]) ||
        geometry->motion_profile[5] < 0 || geometry->motion_profile[5] >= 1)
        return false;
    if (!valid_edit_state(state, geometry))
        return false;
    if (menu->page != GC_PAGE_CALENDAR && menu->page != GC_PAGE_OPTIONS &&
        menu->page != GC_PAGE_DISC) {
        gc_edit_state_init(state);
        return true;
    }
    if (menu->page != GC_PAGE_DISC &&
        !geometry_points(geometry, menu, 0, NULL, NULL, 0, selected, &count, false))
        return false;
    if (menu->page == GC_PAGE_CALENDAR) {
        const GcIplResource *map = &geometry->maps[menu->settings.language];
        if (!mark_list(map, menu->editor_index < 3 ? 0x18 : 0, movement, count))
            return false;
        if (menu->editing)
            for (unsigned index = 0; index < count; ++index)
                if (selected[index])
                    movement[index] = false;
    } else if (menu->page == GC_PAGE_OPTIONS && !menu->editing) {
        memcpy(movement, selected, count * sizeof(movement[0]));
    }
    candidate = *state;
    if (!candidate.active || candidate.page != menu->page) {
        gc_edit_state_init(&candidate);
        candidate.page = menu->page;
        candidate.active = true;
        candidate.sampled_language = menu->settings.language;
        if (geometry->europe)
            candidate.language_from = candidate.language_to =
                geometry->language_centers[menu->settings.language];
        candidate.language_movement = candidate.next_language_movement =
            geometry->selection_ticks;
    }
    candidate.ready = editor_ready;
    if (geometry->europe && menu->page == GC_PAGE_OPTIONS && ticks) {
        if (!geometry->language_fade_ticks ||
            menu->settings.language > GC_LANGUAGE_DUTCH)
            return false;
        /* PAL 0x81328648 updates the language rows even while the native
         * three-dimensional values are waiting for their collapse gate. */
        for (unsigned language = 0; language < 6; ++language) {
            bool selected_language = (unsigned)menu->settings.language == language;
            candidate.language_labels[language] =
                approach_counter(candidate.language_labels[language],
                                 selected_language ? geometry->language_fade_ticks
                                                   : geometry->language_fade_ticks / 2,
                                 ticks);
            candidate.language_captions[language] = approach_counter(
                candidate.language_captions[language],
                selected_language ? geometry->language_fade_ticks : 0, ticks);
        }
    }
    if (editor_ready && ticks) {
        if (menu->page == GC_PAGE_DISC && menu->launch_requested &&
            !candidate.disc_launching) {
            /* USA 28ad0 / PAL 2a3d4 resets the sampled-before-update
             * timer while retaining the previous whole-word motion offsets. */
            candidate.disc_launching = true;
            candidate.disc_launch_motion = candidate.sampled_motion;
            candidate.disc_launch_motion_active =
                (float)candidate.sampled_tick / geometry->entrance_ticks >
                geometry->motion_profile[5];
            candidate.entrance_counter = candidate.sampled_tick = 0;
        }
        if (menu->page == GC_PAGE_OPTIONS &&
            !advance_options_state(&candidate, geometry, menu, ticks))
            return false;
        advance_editor_points(&candidate, geometry, selected, movement, count, ticks);
    }
    *state = candidate;
    return true;
}

float gc_edit_state_progress(const GcEditState *state, const GcEditGeometry *geometry) {
    if (!state || !geometry || !geometry->entrance_ticks ||
        state->sampled_tick > geometry->entrance_ticks)
        return 0;
    return (float)state->sampled_tick / geometry->entrance_ticks;
}

uint8_t gc_edit_language_alpha(const GcEditState *state, const GcEditGeometry *geometry,
                               gc_language language, bool caption) {
    if (!state || !geometry || !geometry->language_fade_ticks || !geometry->europe ||
        !state->active || state->page != GC_PAGE_OPTIONS ||
        language < GC_LANGUAGE_ENGLISH || language > GC_LANGUAGE_DUTCH)
        return 0;
    unsigned counter =
        caption ? state->language_captions[language] : state->language_labels[language];
    if (counter > geometry->language_fade_ticks)
        return 0;
    return (uint8_t)(counter * 255 / geometry->language_fade_ticks);
}

size_t gc_edit_geometry_points_with_state(const GcEditGeometry *geometry,
                                          const gc_menu *menu, const GcEditState *state,
                                          GcEditPoint *points, size_t capacity) {
    if (!state || !menu || !state->active || !state->ready || state->page != menu->page)
        return 0;
    return geometry_points(geometry, menu, gc_edit_state_progress(state, geometry),
                           state, points, capacity, NULL, NULL, false);
}

size_t gc_edit_geometry_source_points(const GcEditGeometry *geometry,
                                      const gc_menu *menu, const GcEditState *state,
                                      GcEditPoint *points, size_t capacity) {
    if (!state || !menu || !state->active || !state->ready || state->page != menu->page)
        return 0;
    return geometry_points(geometry, menu, gc_edit_state_progress(state, geometry),
                           state, points, capacity, NULL, NULL, true);
}

static bool move_disc_points(const GcEditGeometry *geometry, const GcEditState *state,
                             GcEditPoint *points, unsigned count, float progress) {
    if (!state || progress <= geometry->motion_profile[5])
        return true;
    float shift[2], center[3] = {0};
    GcEditPoint rotation = {0};
    float matrix[12];
    if (!sample_motion(geometry, state, shift, rotation.angles) ||
        !gc_edit_point_matrix(&rotation, matrix))
        return false;
    for (unsigned index = 0; index < count; ++index)
        for (unsigned axis = 0; axis < 3; ++axis)
            center[axis] += points[index].position[axis];
    for (unsigned axis = 0; axis < 3; ++axis)
        center[axis] /= (float)count;
    /* USA 0x8132e80c / PAL 0x8133374c translate every sampled block before
     * rotating about the complete group's original centroid. The subsequent
     * status MAP cannot change that centroid or the native bobbing phase. */
    for (unsigned index = 0; index < count; ++index) {
        float relative[3];
        for (unsigned axis = 0; axis < 3; ++axis)
            relative[axis] = points[index].position[axis] - center[axis] +
                             (axis < 2 ? shift[axis] : 0);
        for (unsigned axis = 0; axis < 3; ++axis) {
            points[index].position[axis] =
                center[axis] + matrix[axis * 4] * relative[0] +
                matrix[axis * 4 + 1] * relative[1] + matrix[axis * 4 + 2] * relative[2];
            points[index].angles[axis] = rotation.angles[axis];
        }
    }
    return true;
}

static float disc_compression(const GcEditDiscLaunch *launch, float progress) {
    float time = progress * launch->compression_time;
    bool first = time < launch->compression_end;
    float local =
        first ? time / launch->compression_end
              : (time - launch->compression_end) / (1 - launch->compression_end);
    float curve = local * local * (3 - 2 * local);
    return 1 - launch->compression_amount * (first ? curve : 1 - curve);
}

static bool launch_disc_points(const GcEditGeometry *geometry, GcEditPoint *points,
                               unsigned count, const float center[3], float progress) {
    const GcEditDiscLaunch *launch = &geometry->disc_launch;
    if (!launch->delays || launch->point_count != count)
        return false;
    if (progress < launch->compression_end) {
        float factor = disc_compression(launch, progress);
        for (unsigned index = 0; index < count; ++index) {
            for (unsigned axis = 0; axis < 3; ++axis)
                points[index].position[axis] =
                    center[axis] +
                    factor * (points[index].position[axis] - center[axis]);
            /* Native 2d6cc multiplies each scale component twice. */
            points[index].scale *= factor * factor;
        }
        return true;
    }
    float elapsed = progress - launch->compression_end;
    uint8_t alpha = (uint8_t)fmaxf(0, truncf(255 * (1 - launch->fade_speed * elapsed)));
    for (unsigned index = 0; index < count; ++index) {
        float delay = launch->delay_span * (1 - launch->delays[index] / 255.0f);
        float time = fmaxf(0, elapsed - delay);
        uint16_t angle = (uint16_t)(uint32_t)truncf(geometry->angle_units * time);
        GcEditPoint rotation = {.angles = {(int16_t)angle, 0, (int16_t)angle}};
        float matrix[12], translated[3];
        if (!gc_edit_point_matrix(&rotation, matrix))
            return false;
        for (unsigned axis = 0; axis < 3; ++axis)
            translated[axis] =
                points[index].position[axis] + launch->translation[axis] * time;
        /* USA 2d480 / PAL 30128 rotates the translated point around the
         * origin. The later ordinary builder supplies the cube's angles. */
        for (unsigned axis = 0; axis < 3; ++axis)
            points[index].position[axis] = matrix[axis * 4] * translated[0] +
                                           matrix[axis * 4 + 1] * translated[1] +
                                           matrix[axis * 4 + 2] * translated[2];
        points[index].colors[0][3] = alpha;
    }
    return true;
}

static bool retained_disc_motion(const GcEditGeometry *geometry,
                                 const GcEditState *state, GcEditPoint *points,
                                 unsigned count, const float original_center[3]) {
    if (!state->disc_launch_motion_active)
        return true;
    GcEditState previous = *state;
    previous.sampled_motion = state->disc_launch_motion;
    float shift[2];
    GcEditPoint rotation = {0};
    float matrix[12];
    if (!sample_motion(geometry, &previous, shift, rotation.angles) ||
        !gc_edit_point_matrix(&rotation, matrix))
        return false;
    for (unsigned index = 0; index < count; ++index) {
        float endpoint[3], relative[3];
        if (!gc_edit_motion_point(&geometry->motion, 1, index, 1, endpoint))
            return false;
        for (unsigned axis = 0; axis < 3; ++axis)
            relative[axis] =
                (endpoint[axis] - original_center[axis]) * geometry->transforms[4][2] +
                (axis < 2 ? shift[axis] : 0);
        for (unsigned axis = 0; axis < 3; ++axis) {
            float rotated = matrix[axis * 4] * relative[0] +
                            matrix[axis * 4 + 1] * relative[1] +
                            matrix[axis * 4 + 2] * relative[2];
            /* Native 2a0e4 retains 2e80c's rotation displacement. The
             * translation was already applied to that earlier input. */
            points[index].position[axis] += rotated - relative[axis];
            points[index].angles[axis] = rotation.angles[axis];
        }
    }
    return true;
}

static size_t disc_points(const GcEditGeometry *geometry, gc_language language,
                          size_t field, float progress, GcEditPoint *points,
                          size_t capacity, bool source_points,
                          const GcEditState *state) {
    GcEditPoint candidate[GC_EDIT_POINT_LIMIT] = {0};
    bool visible[GC_EDIT_POINT_LIMIT] = {false};
    const uint8_t *records;
    unsigned count;
    unsigned times;
    float center[3] = {0};
    float original_center[3];
    size_t output_count = 0;

    if (!geometry || language < GC_LANGUAGE_ENGLISH ||
        language > GC_LANGUAGE_JAPANESE || !isfinite(progress) ||
        !motion_group(&geometry->motion, 1, &records, &count, &times) ||
        (!source_points &&
         !mark_list(&geometry->maps[language], field, visible, count)))
        return 0;
    /* USA 0x8132c65c / EUR 0x8132ec54 transforms the complete sampled
     * group around its mean, before the visibility map selects a word. */
    bool launching = state && state->disc_launching;
    for (unsigned index = 0; index < count; ++index) {
        if (!gc_edit_motion_point(&geometry->motion, 1, index, launching ? 1 : progress,
                                  candidate[index].position))
            return 0;
        candidate[index].scale = 1;
        memcpy(candidate[index].colors, geometry->disc_colors,
               sizeof(candidate[index].colors));
        for (unsigned axis = 0; axis < 3; ++axis)
            center[axis] += candidate[index].position[axis];
    }
    for (unsigned axis = 0; axis < 3; ++axis)
        center[axis] /= (float)count;
    memcpy(original_center, center, sizeof(original_center));
    if (launching) {
        if (!launch_disc_points(geometry, candidate, count, center, progress))
            return 0;
        memset(center, 0, sizeof(center));
        for (unsigned index = 0; index < count; ++index)
            for (unsigned axis = 0; axis < 3; ++axis)
                center[axis] += candidate[index].position[axis];
        for (unsigned axis = 0; axis < 3; ++axis)
            center[axis] /= (float)count;
    }
    for (unsigned index = 0; index < count; ++index) {
        candidate[index].field = GC_EDIT_DISC;
        candidate[index].alpha = (uint8_t)(255 * fmaxf(0, fminf(progress, 1)));
        candidate[index].source_index = index;
        candidate[index].scale *= geometry->transforms[4][2];
        for (unsigned axis = 0; axis < 3; ++axis)
            candidate[index].position[axis] =
                center[axis] + (candidate[index].position[axis] - center[axis] +
                                (axis < 2 ? geometry->transforms[4][axis] : 0)) *
                                   geometry->transforms[4][2];
        if (source_points || visible[index])
            ++output_count;
    }
    if (launching && progress <= geometry->motion_profile[5] &&
        !retained_disc_motion(geometry, state, candidate, count, original_center))
        return 0;
    if (!move_disc_points(geometry, state, candidate, count, progress))
        return 0;
    if (!points)
        return output_count;
    if (capacity < output_count)
        return 0;
    output_count = 0;
    for (unsigned index = 0; index < count; ++index)
        if (source_points || visible[index])
            points[output_count++] = candidate[index];
    return output_count;
}

size_t gc_edit_disc_points(const GcEditGeometry *geometry, gc_language language,
                           bool ready, float progress, GcEditPoint *points,
                           size_t capacity) {
    return disc_points(geometry, language, ready ? 0x70 : 0x6c, progress, points,
                       capacity, false, NULL);
}

size_t gc_edit_disc_status_points(const GcEditGeometry *geometry, gc_language language,
                                  gc_disc_status status, float progress,
                                  GcEditPoint *points, size_t capacity) {
    if ((unsigned)status > GC_DISC_FATAL)
        return 0;
    return disc_points(geometry, language,
                       status == GC_DISC_READY    ? 0x70u
                       : status == GC_DISC_ABSENT ? 0x6cu
                                                  : 0x74u,
                       progress, points, capacity, false, NULL);
}

size_t gc_edit_disc_source_points(const GcEditGeometry *geometry, gc_language language,
                                  float progress, GcEditPoint *points,
                                  size_t capacity) {
    return disc_points(geometry, language, 0, progress, points, capacity, true, NULL);
}

static bool valid_disc_state(const GcEditGeometry *geometry, const GcEditState *state) {
    return geometry && state && state->active && state->ready &&
           state->page == GC_PAGE_DISC && geometry->entrance_ticks &&
           state->sampled_tick <= geometry->entrance_ticks && geometry->motion_ticks &&
           state->sampled_motion < geometry->motion_ticks;
}

size_t gc_edit_disc_status_points_with_state(const GcEditGeometry *geometry,
                                             gc_language language,
                                             gc_disc_status status,
                                             const GcEditState *state,
                                             GcEditPoint *points, size_t capacity) {
    if (!valid_disc_state(geometry, state) || (unsigned)status > GC_DISC_FATAL)
        return 0;
    return disc_points(geometry, language,
                       status == GC_DISC_READY    ? 0x70u
                       : status == GC_DISC_ABSENT ? 0x6cu
                                                  : 0x74u,
                       gc_edit_state_progress(state, geometry), points, capacity, false,
                       state);
}

size_t gc_edit_disc_source_points_with_state(const GcEditGeometry *geometry,
                                             gc_language language,
                                             const GcEditState *state,
                                             GcEditPoint *points, size_t capacity) {
    if (!valid_disc_state(geometry, state))
        return 0;
    return disc_points(geometry, language, 0, gc_edit_state_progress(state, geometry),
                       points, capacity, true, state);
}
