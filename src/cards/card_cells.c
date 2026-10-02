#include "gamecube/card_cells.h"
#include "console_common/support/endian.h"
#include "gamecube/angle.h"

#include <math.h>
#include <string.h>

static bool valid_style(const GcCardCellStyle *style) {
    if (!style || !style->entrance_ticks || style->entrance_ticks > 1000 ||
        !style->relayout_ticks || style->relayout_ticks > 1000 ||
        style->delay_mask > 31 || !style->entrance_alpha_step || !style->alpha_step ||
        !isfinite(style->tangent_radius) || style->tangent_radius < 0 ||
        style->tangent_radius > 10000 || !isfinite(style->offscreen_distance) ||
        style->offscreen_distance <= 0 || style->offscreen_distance > 10000)
        return false;
    for (unsigned axis = 0; axis < 3; ++axis)
        if (!isfinite(style->origin[axis]) || fabsf(style->origin[axis]) > 100000)
            return false;
    return true;
}

bool gc_card_cell_style_decode(const GcText *text, GcCardCellStyle *style) {
    if (!text || !text->rom || !style)
        return false;
    /* USA 1f6f0 / PAL 207c8 initialize the same 254 records. Their motion
     * duration differs. The following instruction sites are bounded stores
     * in those routines and the normal (non-erase) updater. */
    size_t origin = text->europe ? 0x7f604 : 0x5e944;
    size_t radius = text->europe ? 0x1ae7a4 : 0x15e8fc;
    size_t duration = text->europe ? 0x210fc : 0x20024;
    size_t relayout = text->europe ? 0x208a0 : 0x1f7c8;
    size_t alpha = text->europe ? 0x203dc : 0x1f304;
    if (origin > text->rom_size || text->rom_size - origin < 12 || radius < 4 ||
        radius > text->rom_size || text->rom_size - radius < 4 ||
        duration > text->rom_size || text->rom_size - duration < 4 ||
        relayout > text->rom_size || text->rom_size - relayout < 4 ||
        alpha > text->rom_size || text->rom_size - alpha < 12)
        return false;
    uint32_t duration_word = cc_read_be32(text->rom + duration);
    uint32_t layout_word = cc_read_be32(text->rom + relayout);
    uint32_t alpha_word = cc_read_be32(text->rom + alpha);
    uint32_t ordinary_word = cc_read_be32(text->rom + alpha + 8);
    if ((duration_word & UINT32_C(0xffff0000)) != UINT32_C(0x38030000) ||
        (layout_word & UINT32_C(0xffff0000)) != UINT32_C(0x38000000) ||
        (alpha_word & UINT32_C(0xffff0000)) != UINT32_C(0x38800000) ||
        (ordinary_word & UINT32_C(0xffff0000)) != UINT32_C(0x38800000))
        return false;
    GcCardCellStyle candidate = {.origin = {cc_read_be_float(text->rom + origin),
                                            cc_read_be_float(text->rom + origin + 4),
                                            cc_read_be_float(text->rom + origin + 8)},
                                 .tangent_radius = cc_read_be_float(text->rom + radius),
                                 .offscreen_distance =
                                     cc_read_be_float(text->rom + radius - 4),
                                 .entrance_ticks = (uint16_t)duration_word,
                                 .relayout_ticks = (uint16_t)layout_word,
                                 .delay_mask = 15,
                                 .entrance_alpha_step = (uint8_t)alpha_word,
                                 .alpha_step = (uint8_t)ordinary_word};
    if (!valid_style(&candidate))
        return false;
    *style = candidate;
    return true;
}

static bool valid_layout(const float centers[2][16][2], const size_t rows[2]) {
    if (!centers || !rows || rows[0] > 28 || rows[1] > 28)
        return false;
    for (unsigned slot = 0; slot < 2; ++slot)
        for (unsigned cell = 0; cell < 16; ++cell)
            for (unsigned axis = 0; axis < 2; ++axis)
                if (!isfinite(centers[slot][cell][axis]) ||
                    fabsf(centers[slot][cell][axis]) > 10000)
                    return false;
    return true;
}

static void set_targets(const GcCardCellStyle *style, GcCardCells *state,
                        const float centers[2][16][2], const size_t rows[2]) {
    for (unsigned slot = 0; slot < 2; ++slot) {
        size_t first = rows[slot] * GC_CARD_COLUMNS;
        state->first_rows[slot] = rows[slot];
        for (unsigned index = 0; index < GC_CARD_FILE_LIMIT; ++index) {
            GcCardCell *cell = &state->cells[slot][index];
            unsigned column = index % GC_CARD_COLUMNS;
            bool visible = index >= first && index < first + 16;
            unsigned pane =
                visible ? (unsigned)(index - first) : column + (index < first ? 0 : 12);
            cell->target_alpha = visible ? 255 : 0;
            cell->target[0] = centers[slot][pane][0] * 16;
            cell->target[1] = centers[slot][pane][1] * 16;
            cell->target[2] = 0;
            if (!visible)
                cell->target[1] += index < first ? -style->offscreen_distance
                                                 : style->offscreen_distance;
        }
    }
}

bool gc_card_cells_begin(const GcCardCellStyle *style, GcCardCells *state,
                         const float centers[2][16][2], const size_t rows[2],
                         GcCardCellsRandom random, void *context) {
    if (!valid_style(style) || !state || !valid_layout(centers, rows))
        return false;
    /* Obtain all samples before touching live state: adapter failure cannot
     * publish a partially initialized entrance. */
    uint32_t samples[2][GC_CARD_FILE_LIMIT][2] = {{{0}}};
    if (random)
        for (unsigned slot = 0; slot < 2; ++slot)
            for (unsigned index = 0; index < GC_CARD_FILE_LIMIT; ++index)
                for (unsigned sample = 0; sample < 2; ++sample)
                    if (!random(context, &samples[slot][index][sample]))
                        return false;
    uint16_t phase = state->initialized ? state->phase : 0;
    memset(state, 0, sizeof(*state));
    state->phase = state->drawn_phase = phase;
    set_targets(style, state, centers, rows);
    for (unsigned slot = 0; slot < 2; ++slot)
        for (unsigned index = 0; index < GC_CARD_FILE_LIMIT; ++index) {
            GcCardCell *cell = &state->cells[slot][index];
            int16_t angle = (int16_t)(uint16_t)samples[slot][index][0];
            cell->delay = (uint8_t)(samples[slot][index][1] & style->delay_mask);
            cell->duration = style->entrance_ticks + cell->delay;
            memcpy(cell->position, style->origin, sizeof(cell->position));
            memcpy(cell->start, style->origin, sizeof(cell->start));
            memcpy(cell->drawn_position, style->origin, sizeof(cell->drawn_position));
            cell->tangent[0] = style->tangent_radius * gc_angle_sine(angle);
            cell->tangent[1] = style->tangent_radius * gc_angle_cosine(angle);
            memcpy(cell->velocity, cell->tangent, sizeof(cell->velocity));
        }
    state->initialized = true;
    state->entrance = true;
    return true;
}

bool gc_card_cells_relayout(const GcCardCellStyle *style, GcCardCells *state,
                            const float centers[2][16][2], const size_t rows[2]) {
    if (!valid_style(style) || !state || !state->initialized || state->entrance ||
        !valid_layout(centers, rows))
        return false;
    set_targets(style, state, centers, rows);
    for (unsigned slot = 0; slot < 2; ++slot)
        for (unsigned index = 0; index < GC_CARD_FILE_LIMIT; ++index) {
            GcCardCell *cell = &state->cells[slot][index];
            memcpy(cell->start, cell->position, sizeof(cell->start));
            memcpy(cell->tangent, cell->velocity, sizeof(cell->tangent));
            cell->delay = 0;
            cell->duration = style->relayout_ticks;
            cell->counter = 0;
        }
    return true;
}

static float interpolate(const GcCardCell *cell, unsigned axis) {
    if (cell->counter < cell->delay)
        return cell->start[axis];
    float duration = (float)(cell->duration - cell->delay);
    float elapsed = (float)(cell->counter - cell->delay);
    float inverse = 1 / duration;
    float square_time = inverse * elapsed * elapsed;
    float square = square_time * inverse;
    float cubic_time = elapsed * square;
    float cube = cubic_time * inverse;
    /* Native 07e20 retains tangents as derivatives per video tick. */
    return cell->tangent[axis] * (elapsed + cubic_time - 2 * square_time) +
           cell->start[axis] * (1 + 2 * cube - 3 * square) +
           cell->target[axis] * (-2 * cube + 3 * square);
}

bool gc_card_cells_ready(const GcCardCells *state, const bool ready_cards[2]) {
    if (!state || !state->initialized || !ready_cards)
        return false;
    for (unsigned slot = 0; slot < 2; ++slot)
        if (ready_cards[slot])
            for (unsigned index = 0; index < GC_CARD_FILE_LIMIT; ++index)
                if (state->cells[slot][index].counter !=
                    state->cells[slot][index].duration)
                    return false;
    return true;
}

static void advance_tick(const GcCardCellStyle *style, GcCardCells *state,
                         unsigned selected_slot, size_t selected_index) {
    unsigned step = state->entrance ? style->entrance_alpha_step : style->alpha_step;
    for (unsigned slot = 0; slot < 2; ++slot)
        for (unsigned index = 0; index < GC_CARD_FILE_LIMIT; ++index) {
            GcCardCell *cell = &state->cells[slot][index];
            memcpy(cell->drawn_position, cell->position, sizeof(cell->drawn_position));
            cell->drawn_alpha = cell->alpha;
            cell->drawn_selection = cell->selection;
            if (cell->counter >= cell->delay)
                cell->alpha =
                    cell->target_alpha
                        ? (uint8_t)(255 - cell->alpha < step ? 255 : cell->alpha + step)
                        : (uint8_t)(cell->alpha < step ? 0 : cell->alpha - step);
            for (unsigned axis = 0; axis < 3; ++axis) {
                float position = interpolate(cell, axis);
                cell->velocity[axis] = position - cell->position[axis];
                cell->position[axis] = position;
            }
            if (cell->counter < cell->duration)
                ++cell->counter;
            bool selected =
                !state->entrance && selected_slot == slot && selected_index == index;
            if (selected && cell->selection < 6)
                ++cell->selection;
            else if (!selected && cell->selection)
                --cell->selection;
        }
}

bool gc_card_cells_advance(const GcCardCellStyle *style, GcCardCells *state,
                           const bool ready_cards[2], bool face_ready, bool page_ready,
                           bool erasing, unsigned selected_slot, size_t selected_index,
                           uint64_t ticks) {
    if (!valid_style(style) || !state || !state->initialized || !ready_cards ||
        selected_slot > 1 || selected_index >= GC_CARD_FILE_LIMIT)
        return false;
    if (!face_ready || !ticks)
        return true;
    unsigned limit =
        style->entrance_ticks + style->delay_mask + style->relayout_ticks + 34;
    uint64_t steps = ticks < limit ? ticks : limit;
    for (uint64_t tick = 0; tick < steps; ++tick) {
        if (!erasing)
            advance_tick(style, state, selected_slot, selected_index);
        if (state->entrance && !erasing && page_ready &&
            gc_card_cells_ready(state, ready_cards)) {
            state->entrance = false;
            for (unsigned slot = 0; slot < 2; ++slot)
                for (unsigned index = 0; index < GC_CARD_FILE_LIMIT; ++index) {
                    GcCardCell *cell = &state->cells[slot][index];
                    cell->delay = 0;
                    cell->duration = style->relayout_ticks;
                    cell->counter = cell->duration;
                }
        }
    }
    state->drawn_phase = (uint16_t)(state->phase + (unsigned)((ticks - 1) % 65536) * 7);
    state->phase = (uint16_t)(state->phase + (unsigned)(ticks % 65536) * 7);
    return true;
}
