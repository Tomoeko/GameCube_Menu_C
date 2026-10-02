#include "gamecube/card_lighting.h"
#include "console_common/support/endian.h"
#include "resources/native_constants.h"

#include <math.h>
#include <string.h>

static bool pool_float(const GcText *text, size_t offset, float *value) {
    if (offset > text->rom_size || text->rom_size - offset < 4)
        return false;
    *value = cc_read_be_float(text->rom + offset);
    return isfinite(*value);
}

static bool valid_style(const GcCardLightingStyle *style) {
    return style &&
           (!style->selection_lights ||
            (style->focus_ticks && style->focus_ticks <= 1000 && style->blink_ticks &&
             style->blink_ticks <= 1000 && style->header_flags <= 3 &&
             style->header_strength <= 255 && isfinite(style->slot_height) &&
             style->slot_height > 0 && isfinite(style->cutoff_degrees) &&
             style->cutoff_degrees > 0 && style->cutoff_degrees < 90 &&
             isfinite(style->reference_brightness) && style->reference_brightness > 0 &&
             style->reference_brightness < 1));
}

bool gc_card_lighting_style_decode(const GcText *text, GcCardLightingStyle *style) {
    if (!text || !text->rom || !style || text->rom_size != GC_IPL_ROM_SIZE)
        return false;
    /* USA/Japan 1.0 draw 1a994 uses only the fixed center light at b124.
     * PAL 1.0 adds independent slot lights and header attenuation. */
    GcCardLightingStyle candidate = {.selection_lights = text->europe};
    if (!text->europe) {
        *style = candidate;
        return true;
    }
    /* PAL 0x81315800 initializes header flags/strength and slot height;
     * 0x81315bcc sets the three independent focus faders to ten ticks. */
    uint16_t fields[2];
    if (!gc_native_halfwords(text, 0x16020, 0x16154, 3, 0x6c, 2, fields) ||
        !gc_native_halfwords(text, 0x163ec, 0x167fc, 31, 0xe3e, 1,
                             &candidate.focus_ticks))
        return false;
    candidate.header_flags = fields[0];
    candidate.header_strength = fields[1];
    /* Recover the immediate stored to the signed r13 blink-maximum field. */
    bool blink_found = false;
    uint32_t registers[32] = {0};
    bool known[32] = {false};
    for (size_t offset = 0x163ec; offset < 0x167fc; offset += 4) {
        uint32_t instruction = cc_read_be32(text->rom + offset);
        unsigned opcode = instruction >> 26;
        unsigned target = instruction >> 21 & 31;
        unsigned base = instruction >> 16 & 31;
        int immediate = (int16_t)instruction;
        if (opcode == 14 && !base) {
            registers[target] = (uint32_t)immediate;
            known[target] = true;
        } else if (opcode == 18 && (instruction & 1)) {
            for (unsigned index = 0; index <= 12; ++index)
                known[index] = false;
        } else if (opcode == 44 && base == 13 && immediate == -0x7b12 &&
                   known[target] && registers[target] <= UINT16_MAX) {
            candidate.blink_ticks = (uint16_t)registers[target];
            blink_found = true;
        }
    }
    const size_t pool = 0x1b5b20 + GC_IPL_BS2_OFFSET;
    if (!blink_found || !pool_float(text, pool - 0x7c2c, &candidate.slot_height) ||
        !pool_float(text, pool - 0x7eb4, &candidate.cutoff_degrees) ||
        !pool_float(text, pool - 0x7eb0, &candidate.reference_brightness) ||
        !valid_style(&candidate))
        return false;
    *style = candidate;
    return true;
}

static uint16_t approach(uint16_t value, unsigned target, uint64_t ticks) {
    if (value < target)
        return ticks >= target - value ? (uint16_t)target : (uint16_t)(value + ticks);
    return ticks >= value - target ? (uint16_t)target : (uint16_t)(value - ticks);
}

static uint8_t focus_alpha(const GcCardLightingStyle *style, unsigned counter) {
    return (uint8_t)(counter * 255 / style->focus_ticks);
}

bool gc_card_lighting_advance(const GcCardLightingStyle *style, GcCardLighting *state,
                              const gc_menu *menu, uint64_t ticks) {
    if (!valid_style(style) || !state || !menu || menu->card_slot > 1)
        return false;
    if (!style->selection_lights)
        return true;
    if (state->focus[0] > style->focus_ticks || state->focus[1] > style->focus_ticks ||
        state->center > style->focus_ticks ||
        state->blink_phase >= 2u * (style->blink_ticks + 1u))
        return false;
    bool active =
        menu->page == GC_PAGE_CARDS || menu->page == GC_PAGE_CARD_ACTION ||
        menu->page == GC_PAGE_CARD_CONFIRM ||
        (menu->page == GC_PAGE_MESSAGE && menu->message_return_page == GC_PAGE_CARDS);
    if (!active) {
        state->active = false;
        return true;
    }
    GcCardLighting candidate = state->active ? *state : (GcCardLighting){0};
    candidate.active = true;
    bool selected_ready = menu->cards[menu->card_slot].status == GC_CARD_READY;
    for (unsigned slot = 0; slot < 2; ++slot) {
        bool selected = selected_ready && menu->card_slot == slot;
        candidate.focus[slot] =
            approach(candidate.focus[slot], selected ? style->focus_ticks : 0, ticks);
    }
    candidate.center =
        approach(candidate.center, selected_ready ? 0 : style->focus_ticks, ticks);
    unsigned period = 2u * (style->blink_ticks + 1u);
    candidate.blink_phase =
        (uint16_t)((candidate.blink_phase + ticks % period) % period);
    *state = candidate;
    return true;
}

uint8_t gc_card_lighting_header_alpha(const GcCardLightingStyle *style,
                                      const GcCardLighting *state, unsigned slot,
                                      unsigned selected_slot, uint8_t alpha) {
    if (!valid_style(style) || !state || slot > 1 || selected_slot > 1)
        return 0;
    if (!style->selection_lights)
        return alpha;
    unsigned strength = style->header_strength;
    unsigned result = alpha;
    if (style->header_flags & 1) {
        if (state->focus[slot] > style->focus_ticks)
            return 0;
        unsigned factor =
            255 - strength + strength * focus_alpha(style, state->focus[slot]) / 255;
        result = result * factor / 255;
    }
    if ((style->header_flags & 2) && slot == selected_slot) {
        unsigned half_period = style->blink_ticks + 1u;
        if (state->blink_phase >= half_period * 2)
            return 0;
        unsigned counter = state->blink_phase < half_period
                               ? state->blink_phase
                               : half_period * 2 - 1 - state->blink_phase;
        unsigned factor = 255 - strength + strength * counter / style->blink_ticks;
        result = result * factor / 255;
    }
    return (uint8_t)result;
}

static bool slot_light(const GcCardLightingStyle *style, const GcLayoutTable *layout,
                       const GcFaceGeometry *geometry, unsigned slot,
                       GcMenuGridLighting *output) {
    char names[3][5] = {{'i', slot ? 'b' : 'a', '0', '5', 0},
                        {'i', slot ? 'b' : 'a', '0', '6', 0},
                        {'i', slot ? 'b' : 'a', '0', '9', 0}};
    GcLayoutPane panes[3];
    for (unsigned index = 0; index < 3; ++index)
        if (!gc_layout_find_pane(layout, names[index], 0, &panes[index]))
            return false;
    *output = geometry->grid_lighting;
    output->position[0] = (panes[0].box.center_x + panes[1].box.center_x) * 0.5f;
    output->position[1] = (panes[0].box.center_y + panes[2].box.center_y) * 0.5f;
    output->position[2] = style->slot_height;
    output->cutoff_degrees = style->cutoff_degrees;
    output->reference_brightness = style->reference_brightness;
    return true;
}

bool gc_card_lighting_grid(const GcCardLightingStyle *style,
                           const GcCardLighting *state, const GcFaceGeometry *geometry,
                           const GcLayoutTable *layout, GcCardGridLighting *lighting) {
    if (!valid_style(style) || !state || !geometry || !lighting)
        return false;
    GcCardGridLighting candidate = {.lights = {geometry->grid_lighting},
                                    .alpha = {255}};
    if (!style->selection_lights) {
        *lighting = candidate;
        return true;
    }
    if (state->center > style->focus_ticks || state->focus[0] > style->focus_ticks ||
        state->focus[1] > style->focus_ticks)
        return false;
    candidate.alpha[0] = focus_alpha(style, state->center);
    for (unsigned slot = 0; slot < 2; ++slot) {
        if (!slot_light(style, layout, geometry, slot, &candidate.lights[slot + 1]))
            return false;
        candidate.alpha[slot + 1] = focus_alpha(style, state->focus[slot]);
    }
    *lighting = candidate;
    return true;
}

bool gc_card_lighting_grid_color(const GcCardGridLighting *lighting, float x, float y,
                                 uint8_t page_alpha, float rgba[4]) {
    if (!lighting || !rgba)
        return false;
    float result[4] = {1, 1, 1, 0};
    for (unsigned index = 0; index < 3; ++index) {
        if (!lighting->alpha[index])
            continue;
        float color[4];
        if (!gc_menu_grid_light_color(&lighting->lights[index], x, y,
                                      lighting->alpha[index], color))
            return false;
        result[3] += color[3];
    }
    result[3] = fminf(result[3], 1) * (float)page_alpha / 255;
    memcpy(rgba, result, sizeof(result));
    return true;
}
