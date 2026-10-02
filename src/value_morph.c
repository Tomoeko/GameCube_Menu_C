#include "gamecube/value_morph.h"
#include "console_common/support/endian.h"
#include "native_constants.h"

#include <math.h>
#include <string.h>

static bool valid_style(const GcValueMorphStyle *style) {
    if (!style)
        return false;
    for (unsigned axis = 0; axis < 3; ++axis)
        if (!isfinite(style->disc_direction[axis]) ||
            fabsf(style->disc_direction[axis]) > 1)
            return false;
    return isfinite(style->spread) && style->spread > 0 && style->spread <= 10 &&
           isfinite(style->tens_multiplier) && style->tens_multiplier > 0 &&
           style->tens_multiplier <= 10 && style->duration && style->duration <= 255 &&
           style->sound_offsets && style->sound_offsets <= GC_VALUE_MORPH_OFFSET_LIMIT;
}

static bool constructor_floats(const GcText *text, size_t begin, size_t end, size_t r2,
                               unsigned field, float values[2]) {
    float registers[32] = {0};
    bool known[32] = {false};
    bool found[2] = {false};
    if (begin > end || end > text->rom_size)
        return false;
    for (size_t offset = begin; offset + 4 <= end; offset += 4) {
        uint32_t word = cc_read_be32(text->rom + offset);
        unsigned opcode = word >> 26;
        unsigned target = word >> 21 & 31;
        unsigned base = word >> 16 & 31;
        int32_t immediate = (int16_t)word;
        if (opcode == 48 && base == 2) {
            int64_t address = (int64_t)r2 + immediate;
            if (address < 0 || (uint64_t)address + 4 > text->rom_size)
                return false;
            registers[target] = cc_read_be_float(text->rom + (size_t)address);
            known[target] = isfinite(registers[target]);
        } else if (opcode == 52 && base == 31 && known[target] &&
                   immediate >= (int32_t)field && immediate < (int32_t)(field + 8) &&
                   immediate % 4 == 0) {
            unsigned index = ((unsigned)immediate - field) / 4;
            values[index] = registers[target];
            found[index] = true;
        }
    }
    return found[0] && found[1];
}

bool gc_value_morph_style_decode(const GcText *text, GcValueMorphStyle *style) {
    GcValueMorphStyle result = {0};
    float values[2];
    if (!text || !text->rom || !style)
        return false;
    size_t begin = text->europe ? 0x11c84 : 0x111cc;
    size_t end = text->europe ? 0x12078 : 0x11588;
    unsigned field = text->europe ? 0xcc : 0xa4;
    size_t r2 = text->europe ? 0x1b6340 : 0x1664e0;
    if (!constructor_floats(text, begin, end, r2, field, values) ||
        !gc_native_halfwords(text, begin, end, 31, field + 8, 1, &result.duration))
        return false;
    result.spread = values[0];
    result.tens_multiplier = values[1];
    /* USA 32588 / EUR 37488 passes this ROM vector to the word-change
     * controller after sampling outgoing and incoming Disc blocks. */
    size_t direction = text->europe ? 0x7f868 : 0x5eba8;
    if (direction > text->rom_size || text->rom_size - direction < 12)
        return false;
    for (unsigned axis = 0; axis < 3; ++axis)
        result.disc_direction[axis] =
            cc_read_be_float(text->rom + direction + axis * 4);
    /* USA 32678 / EUR 3758c: the loop consumes every random slot, including
     * unused word blocks. Localized PAL words require the larger pool. */
    begin = text->europe ? 0x37dac : 0x32e98;
    end = text->europe ? 0x37e90 : 0x32f90;
    if (end > text->rom_size)
        return false;
    for (size_t offset = begin; offset + 4 <= end; offset += 4) {
        uint32_t word = cc_read_be32(text->rom + offset);
        if (word >> 26 == 11 && (word & 0xffff) >= 768 &&
            (word & 0xffff) <= GC_VALUE_MORPH_OFFSET_LIMIT) {
            if (result.sound_offsets)
                return false;
            result.sound_offsets = (uint16_t)word;
        }
    }
    if (!valid_style(&result))
        return false;
    *style = result;
    return true;
}

void gc_value_morph_init(GcValueMorphState *state) {
    if (state)
        memset(state, 0, sizeof(*state));
}

static GcEditField menu_field(const gc_menu *menu) {
    if (menu->page == GC_PAGE_DISC)
        return GC_EDIT_DISC;
    if (!menu->editing)
        return GC_EDIT_PUNCTUATION;
    if (menu->page == GC_PAGE_OPTIONS) {
        if (menu->editor_index == 0)
            return GC_EDIT_SOUND;
        if (menu->editor_index == 1)
            return GC_EDIT_SCREEN_POSITION;
    } else if (menu->page == GC_PAGE_CALENDAR) {
        return (GcEditField)(GC_EDIT_DAY + gc_menu_calendar_field(menu));
    }
    return GC_EDIT_PUNCTUATION;
}

static int field_value(const gc_menu *menu, GcEditField field) {
    switch (field) {
        case GC_EDIT_SOUND:
            return (int)menu->settings.sound;
        case GC_EDIT_SCREEN_POSITION:
            return menu->settings.screen_position;
        case GC_EDIT_DAY:
            return menu->clock.day;
        case GC_EDIT_MONTH:
            return menu->clock.month;
        case GC_EDIT_YEAR:
            return menu->clock.year;
        case GC_EDIT_HOUR:
            return menu->clock.hour;
        case GC_EDIT_MINUTE:
            return menu->clock.minute;
        case GC_EDIT_SECOND:
            return menu->clock.second;
        case GC_EDIT_DISC:
            return menu->disc_status == GC_DISC_READY    ? 0
                   : menu->disc_status == GC_DISC_ABSENT ? 1
                                                         : 2;
        default:
            return 0;
    }
}

static bool random_offsets(GcValueMorphSample *sample, const GcValueMorphStyle *style,
                           unsigned count, GcValueMorphRandom random, void *context) {
    for (unsigned index = 0; index < count; ++index) {
        uint32_t value = 0;
        if (random && !random(context, &value))
            return false;
        float distance = style->spread * (float)(200 + value % 600) / 10;
        sample->offsets[index] = (int16_t)truncf(distance);
    }
    return true;
}

static bool wrapped_value(GcEditField field, int previous, int next) {
    int minimum = 0;
    int maximum = 59;
    switch (field) {
        case GC_EDIT_DAY:
            return (previous == 1 && next >= 28 && next <= 31) ||
                   (next == 1 && previous >= 28 && previous <= 31);
        case GC_EDIT_MONTH:
            minimum = 1;
            maximum = 12;
            break;
        case GC_EDIT_YEAR:
            minimum = 2000;
            maximum = 2099;
            break;
        case GC_EDIT_HOUR:
            maximum = 23;
            break;
        case GC_EDIT_MINUTE:
        case GC_EDIT_SECOND:
            break;
        default:
            return false;
    }
    return (previous == minimum && next == maximum) ||
           (next == minimum && previous == maximum);
}

static uint8_t digit(int value, unsigned column) {
    unsigned absolute = (unsigned)(value < 0 ? -value : value);
    return (uint8_t)(column ? absolute % 10 : absolute / 10 % 10);
}

static void initialize_sample(GcValueMorphSample *sample, GcEditField field, int value,
                              uint16_t duration) {
    memset(sample, 0, sizeof(*sample));
    sample->kind = field == GC_EDIT_SOUND         ? GC_VALUE_MORPH_SOUND
                   : field == GC_EDIT_DISC        ? GC_VALUE_MORPH_DISC
                   : field == GC_EDIT_PUNCTUATION ? GC_VALUE_MORPH_NONE
                                                  : GC_VALUE_MORPH_NUMBER;
    sample->field = field;
    for (unsigned column = 0; column < 2; ++column) {
        sample->previous[column] = sample->next[column] =
            field == GC_EDIT_SOUND || field == GC_EDIT_DISC ? (uint8_t)value
                                                            : digit(value, column);
        sample->ticks[column] = duration;
    }
}

static bool begin_change(GcValueMorphSample *sample, const GcValueMorphStyle *style,
                         int previous, int next, GcValueMorphRandom random,
                         void *context) {
    if (previous == next)
        return true;
    bool word =
        sample->kind == GC_VALUE_MORPH_SOUND || sample->kind == GC_VALUE_MORPH_DISC;
    unsigned count = word ? style->sound_offsets : GC_VALUE_MORPH_NUMBER_OFFSETS;
    if (!random_offsets(sample, style, count, random, context))
        return false;
    memset(sample->direction, 0, sizeof(sample->direction));
    if (word) {
        if (sample->kind == GC_VALUE_MORPH_DISC)
            memcpy(sample->direction, style->disc_direction, sizeof(sample->direction));
        else
            sample->direction[0] = next == GC_SOUND_STEREO ? -1 : 1;
        sample->previous[0] = (uint8_t)previous;
        sample->next[0] = (uint8_t)next;
        sample->ticks[0] = 0;
    } else {
        unsigned axis = sample->field == GC_EDIT_SCREEN_POSITION ? 0u : 1u;
        float sign = next > previous ? -1.0f : 1.0f;
        sample->direction[axis] =
            wrapped_value(sample->field, previous, next) ? -sign : sign;
        for (unsigned column = 0; column < 2; ++column) {
            uint8_t from = digit(previous, column);
            uint8_t to = digit(next, column);
            if (from != to) {
                sample->previous[column] = from;
                sample->next[column] = to;
                sample->ticks[column] = 0;
            }
        }
    }
    return true;
}

static bool advance_tick(GcValueMorphState *state, const GcValueMorphStyle *style,
                         const gc_menu *menu, GcValueMorphRandom random,
                         void *context) {
    GcEditField field = menu_field(menu);
    int value = field_value(menu, field);
    bool changed_mode = !state->initialized || state->page != menu->page ||
                        state->observed_field != field;
    if (changed_mode) {
        initialize_sample(&state->current, field, value, style->duration);
        state->drawn = state->current;
        state->drawn.kind = GC_VALUE_MORPH_NONE;
    } else if (field != GC_EDIT_PUNCTUATION) {
        /* USA 319c4/3201c sample first, then 32678 resets and increments. USA
         * 305f4 calls 31664 before sampling its two independent counters. */
        bool word = field == GC_EDIT_SOUND || field == GC_EDIT_DISC;
        if (word)
            state->drawn = state->current;
        if (!begin_change(&state->current, style, state->observed_value, value, random,
                          context))
            return false;
        if (!word)
            state->drawn = state->current;
        for (unsigned column = 0; column < 2; ++column)
            if (state->current.ticks[column] < style->duration)
                ++state->current.ticks[column];
    } else {
        state->drawn.kind = GC_VALUE_MORPH_NONE;
    }
    state->page = menu->page;
    state->observed_field = field;
    state->observed_value = value;
    state->initialized = true;
    return true;
}

static bool valid_sample(const GcValueMorphSample *sample,
                         const GcValueMorphStyle *style) {
    if ((unsigned)sample->kind > GC_VALUE_MORPH_DISC ||
        (unsigned)sample->field > GC_EDIT_SCREEN_BAR ||
        (sample->kind == GC_VALUE_MORPH_SOUND && sample->field != GC_EDIT_SOUND) ||
        (sample->kind == GC_VALUE_MORPH_DISC && sample->field != GC_EDIT_DISC) ||
        (sample->kind == GC_VALUE_MORPH_NUMBER &&
         !((sample->field >= GC_EDIT_DAY && sample->field <= GC_EDIT_SECOND) ||
           sample->field == GC_EDIT_SCREEN_POSITION)))
        return false;
    for (unsigned index = 0; index < 2; ++index) {
        if (sample->ticks[index] > style->duration ||
            (sample->kind == GC_VALUE_MORPH_SOUND &&
             (sample->previous[index] > GC_SOUND_STEREO ||
              sample->next[index] > GC_SOUND_STEREO)) ||
            (sample->kind == GC_VALUE_MORPH_DISC &&
             (sample->previous[index] > 2 || sample->next[index] > 2)) ||
            (sample->kind == GC_VALUE_MORPH_NUMBER &&
             (sample->previous[index] > 9 || sample->next[index] > 9)))
            return false;
    }
    for (unsigned axis = 0; axis < 3; ++axis)
        if (!isfinite(sample->direction[axis]) || fabsf(sample->direction[axis]) > 1)
            return false;
    return true;
}

bool gc_value_morph_advance(GcValueMorphState *state, const GcValueMorphStyle *style,
                            const gc_menu *menu, bool ready, uint64_t ticks,
                            GcValueMorphRandom random, void *context) {
    if (!state || !valid_style(style) || !menu ||
        !valid_sample(&state->current, style) || !valid_sample(&state->drawn, style) ||
        state->observed_value < -10000 || state->observed_value > 10000 ||
        (menu->page == GC_PAGE_OPTIONS &&
         (menu->settings.sound > GC_SOUND_STEREO ||
          menu->settings.screen_position < GC_SCREEN_POSITION_MIN ||
          menu->settings.screen_position > GC_SCREEN_POSITION_MAX)) ||
        (menu->page == GC_PAGE_CALENDAR && !gc_date_time_valid(&menu->clock)) ||
        (menu->page == GC_PAGE_DISC && (unsigned)menu->disc_status > GC_DISC_FATAL))
        return false;
    if (!ticks)
        return true;
    GcValueMorphState candidate = *state;
    if (menu->page != GC_PAGE_CALENDAR && menu->page != GC_PAGE_OPTIONS &&
        menu->page != GC_PAGE_DISC) {
        gc_value_morph_init(&candidate);
    } else {
        candidate.ready = ready;
        if (ready) {
            uint64_t updates = ticks > (uint64_t)style->duration + 2
                                   ? (uint64_t)style->duration + 2
                                   : ticks;
            for (uint64_t tick = 0; tick < updates; ++tick)
                if (!advance_tick(&candidate, style, menu, random, context))
                    return false;
        }
    }
    *state = candidate;
    return true;
}

typedef struct {
    const uint8_t *indices;
    unsigned count;
} MapList;

typedef struct {
    const GcEditPoint *source;
    size_t source_count;
    const GcValueMorphSample *sample;
    const GcValueMorphStyle *style;
    const GcEditGeometry *geometry;
    GcEditPoint *points;
    size_t count;
} PointWriter;

static bool map_list(const GcIplResource *map, size_t field, MapList *list) {
    if (!map || !map->bytes || map->byte_count < 4 || !list)
        return false;
    unsigned header = cc_read_be16(map->bytes + 2);
    if (header < 0xd4 || header > 1024 || header > map->byte_count || header % 4 ||
        field >= header || field % 4)
        return false;
    unsigned count = cc_read_be16(map->bytes + field);
    size_t relative = cc_read_be16(map->bytes + field + 2);
    if (!count || count > GC_EDIT_POINT_LIMIT || relative < header - field ||
        relative > map->byte_count - field ||
        count > (map->byte_count - field - relative) / 2)
        return false;
    *list = (MapList){map->bytes + field + relative, count};
    return true;
}

static bool number_fields(bool europe, GcEditField field, size_t fields[2]) {
    switch (field) {
        case GC_EDIT_SCREEN_POSITION:
            fields[0] = europe ? 0xb8 : 0xb4;
            fields[1] = europe ? 0xbc : 0xb8;
            break;
        case GC_EDIT_DAY:
            fields[0] = 0x20;
            fields[1] = 0x24;
            break;
        case GC_EDIT_MONTH:
            fields[0] = 0x44;
            fields[1] = 0x48;
            break;
        case GC_EDIT_YEAR:
            fields[0] = 0x64;
            fields[1] = 0x68;
            break;
        case GC_EDIT_HOUR:
            fields[0] = 0x2c;
            fields[1] = 0x30;
            break;
        case GC_EDIT_MINUTE:
            fields[0] = 0x38;
            fields[1] = 0x3c;
            break;
        case GC_EDIT_SECOND:
            fields[0] = 0x50;
            fields[1] = 0x54;
            break;
        default:
            return false;
    }
    return true;
}

static bool mark_replaced(const GcIplResource *map, const GcValueMorphSample *sample,
                          bool europe, size_t source_count,
                          bool replaced[GC_EDIT_POINT_LIMIT]) {
    size_t fields[2];
    unsigned count = 1;
    if (sample->kind == GC_VALUE_MORPH_SOUND) {
        fields[0] = europe ? 0xd8 : 0xcc;
    } else if (sample->kind == GC_VALUE_MORPH_NUMBER) {
        if (!number_fields(europe, sample->field, fields))
            return false;
        count = 2;
    } else {
        return true;
    }
    for (unsigned field = 0; field < count; ++field) {
        MapList list;
        if (!map_list(map, fields[field], &list))
            return false;
        for (unsigned index = 0; index < list.count; ++index) {
            unsigned source = cc_read_be16(list.indices + index * 2);
            if (source >= source_count || source >= GC_EDIT_POINT_LIMIT)
                return false;
            replaced[source] = true;
        }
    }
    return true;
}

static bool append_point(PointWriter *writer, unsigned source, unsigned slot,
                         unsigned column, bool incoming) {
    if (source >= writer->source_count ||
        writer->source[source].source_index != source ||
        slot >= GC_VALUE_MORPH_OFFSET_LIMIT)
        return false;
    float progress = (float)writer->sample->ticks[column] / writer->style->duration;
    uint8_t alpha = (uint8_t)(255 * (incoming ? progress : 1 - progress));
    if (!alpha)
        return true;
    if (writer->count >= GC_EDIT_POINT_LIMIT)
        return false;
    if (writer->points) {
        GcEditPoint point = writer->source[source];
        float weight = incoming ? 1 - progress : -progress;
        float distance = writer->sample->offsets[slot] * weight;
        if (writer->sample->kind == GC_VALUE_MORPH_NUMBER && column == 0)
            distance *= writer->style->tens_multiplier;
        for (unsigned axis = 0; axis < 3; ++axis)
            point.position[axis] += distance * writer->sample->direction[axis];
        point.alpha = alpha;
        point.field = writer->sample->field;
        point.selected = writer->sample->kind != GC_VALUE_MORPH_DISC;
        unsigned palette = writer->sample->field >= GC_EDIT_DAY &&
                                   writer->sample->field <= GC_EDIT_SECOND
                               ? 1u
                               : 0u;
        if (writer->sample->kind != GC_VALUE_MORPH_DISC)
            memcpy(point.colors, writer->geometry->palettes[palette][0],
                   sizeof(point.colors));
        writer->points[writer->count] = point;
    }
    ++writer->count;
    return true;
}

static bool append_sound(PointWriter *writer, const GcIplResource *map) {
    unsigned old_count = 0;
    for (unsigned pass = 0; pass < 2; ++pass) {
        MapList list;
        uint8_t sound = pass ? writer->sample->next[0] : writer->sample->previous[0];
        size_t field = sound == GC_SOUND_MONO
                           ? (writer->geometry->europe ? 0xb0 : 0xac)
                           : (writer->geometry->europe ? 0xdc : 0xd0);
        if (!map_list(map, field, &list) ||
            list.count > writer->style->sound_offsets - old_count)
            return false;
        for (unsigned index = 0; index < list.count; ++index)
            if (!append_point(writer, cc_read_be16(list.indices + index * 2),
                              old_count + index, 0, pass != 0))
                return false;
        if (!pass)
            old_count = list.count;
    }
    return true;
}

static bool append_number(PointWriter *writer, const GcIplResource *map) {
    size_t fields[2];
    if (!number_fields(writer->geometry->europe, writer->sample->field, fields))
        return false;
    for (unsigned column = 0; column < 2; ++column) {
        MapList positions;
        if (!map_list(map, fields[column], &positions) || positions.count != 28)
            return false;
        for (unsigned pass = 0; pass < 2; ++pass) {
            MapList pattern;
            bool visible[28] = {false};
            uint8_t value =
                pass ? writer->sample->next[column] : writer->sample->previous[column];
            if (!map_list(map, 0x78 + (size_t)value * 4, &pattern) ||
                pattern.count != positions.count)
                return false;
            /* The native pattern pads its 28 entries by repeating a visible
             * cell. It sets that cell's alpha, rather than drawing it twice. */
            for (unsigned index = 0; index < pattern.count; ++index) {
                unsigned cell = cc_read_be16(pattern.indices + index * 2);
                if (cell >= positions.count)
                    return false;
                visible[cell] = true;
            }
            for (unsigned cell = 0; cell < positions.count; ++cell)
                if (visible[cell] &&
                    !append_point(writer, cc_read_be16(positions.indices + cell * 2),
                                  column * 56 + pass * 28 + cell, column, pass != 0))
                    return false;
        }
    }
    return true;
}

static bool append_disc(PointWriter *writer, const GcIplResource *map) {
    static const size_t fields[3] = {0x70, 0x6c, 0x74};
    unsigned old_count = 0;
    for (unsigned pass = 0; pass < 2; ++pass) {
        uint8_t word = pass ? writer->sample->next[0] : writer->sample->previous[0];
        MapList list;
        if (word > 2 || !map_list(map, fields[word], &list) ||
            list.count > writer->style->sound_offsets - old_count)
            return false;
        for (unsigned index = 0; index < list.count; ++index)
            if (!append_point(writer, cc_read_be16(list.indices + index * 2),
                              old_count + index, 0, pass != 0))
                return false;
        if (!pass)
            old_count = list.count;
    }
    return true;
}

static bool append_effect(PointWriter *writer, const GcIplResource *map) {
    switch (writer->sample->kind) {
        case GC_VALUE_MORPH_SOUND:
            return append_sound(writer, map);
        case GC_VALUE_MORPH_NUMBER:
            return append_number(writer, map);
        case GC_VALUE_MORPH_DISC:
            return append_disc(writer, map);
        case GC_VALUE_MORPH_NONE:
            return true;
        default:
            return false;
    }
}

size_t gc_value_morph_points(const GcEditGeometry *geometry, const gc_menu *menu,
                             const GcEditState *editor, const GcValueMorphStyle *style,
                             const GcValueMorphState *state, GcEditPoint *points,
                             size_t capacity) {
    if (!geometry || !menu || !editor || !valid_style(style) || !state ||
        !valid_sample(&state->drawn, style))
        return 0;
    if (!state->ready || !state->initialized || state->page != menu->page ||
        state->drawn.kind == GC_VALUE_MORPH_NONE) {
        if (menu->page == GC_PAGE_DISC)
            return state->ready ? gc_edit_disc_status_points_with_state(
                                      geometry, menu->settings.language,
                                      menu->disc_status, editor, points, capacity)
                                : 0;
        return gc_edit_geometry_points_with_state(geometry, menu, editor, points,
                                                  capacity);
    }
    GcEditPoint source[GC_EDIT_POINT_LIMIT];
    bool replaced[GC_EDIT_POINT_LIMIT] = {false};
    gc_language language = gc_edit_geometry_map_language(menu);
    bool disc = state->drawn.kind == GC_VALUE_MORPH_DISC;
    size_t source_count =
        disc ? gc_edit_disc_source_points_with_state(geometry, language, editor, source,
                                                     GC_EDIT_POINT_LIMIT)
             : gc_edit_geometry_source_points(geometry, menu, editor, source,
                                              GC_EDIT_POINT_LIMIT);
    if (!source_count || (unsigned)language >= 7)
        return 0;
    const GcIplResource *map = &geometry->maps[language];
    if (disc)
        memset(replaced, 1, source_count * sizeof(*replaced));
    else if (!mark_replaced(map, &state->drawn, geometry->europe, source_count,
                            replaced))
        return 0;
    size_t ordinary_count = 0;
    for (size_t index = 0; index < source_count; ++index) {
        unsigned id = source[index].source_index;
        if (source[index].alpha && (id >= GC_EDIT_POINT_LIMIT || !replaced[id]))
            ++ordinary_count;
    }
    PointWriter writer = {source,   source_count, &state->drawn, style,
                          geometry, NULL,         ordinary_count};
    if (!append_effect(&writer, map))
        return 0;
    size_t output_count = writer.count;
    if (!points)
        return output_count;
    if (output_count > capacity)
        return 0;
    size_t output = 0;
    for (size_t index = 0; index < source_count; ++index) {
        unsigned id = source[index].source_index;
        if (source[index].alpha && (id >= GC_EDIT_POINT_LIMIT || !replaced[id]))
            points[output++] = source[index];
    }
    writer.points = points;
    writer.count = ordinary_count;
    if (!append_effect(&writer, map))
        return 0;
    return output_count;
}
