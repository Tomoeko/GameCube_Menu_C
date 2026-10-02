#include "gamecube/help.h"
#include "console_common/support/endian.h"

#include "resources/native_constants.h"

#include <string.h>

bool gc_help_style_decode(const GcText *text, GcHelpStyle *style) {
    GcHelpStyle candidate = {0};
    uint16_t channels[6];
    size_t r2_offset;
    unsigned first_color;
    if (!text || !style || !text->rom)
        return false;
    if (!gc_native_halfwords(text, text->europe ? 0x11c84 : 0x111cc,
                             text->europe ? 0x12078 : 0x11588, 31,
                             text->europe ? 0x1a4 : 0x16c, 6, channels))
        return false;
    for (unsigned index = 0; index < 6; ++index)
        if (channels[index] > 255)
            return false;
    r2_offset = text->europe ? 0x1b6340 : 0x1664e0;
    first_color = text->europe ? 0x7c54 : 0x7c9c;
    if (r2_offset < first_color + 24 || r2_offset > text->rom_size)
        return false;
    /* USA 0x813142ec / EUR 0x8131508c: three repeating background
     * tints, white control textures and a separate dot/label tint. */
    candidate.backgrounds[0] = cc_read_be32(text->rom + r2_offset - first_color);
    candidate.backgrounds[1] = (uint32_t)channels[3] << 24 |
                               (uint32_t)channels[4] << 16 |
                               (uint32_t)channels[5] << 8 | 255;
    candidate.backgrounds[2] = (uint32_t)channels[0] << 24 |
                               (uint32_t)channels[1] << 16 |
                               (uint32_t)channels[2] << 8 | 255;
    candidate.controls[0] = cc_read_be32(text->rom + r2_offset - first_color - 16);
    candidate.controls[1] = cc_read_be32(text->rom + r2_offset - first_color - 8);
    candidate.controls[2] = cc_read_be32(text->rom + r2_offset - first_color - 4);
    candidate.dots = cc_read_be32(text->rom + r2_offset - first_color - 12);
    candidate.text = cc_read_be32(text->rom + r2_offset - first_color - 24);
    *style = candidate;
    return true;
}

void gc_help_state_init(GcHelpState *state) {
    if (state)
        memset(state, 0, sizeof(*state));
}

static uint8_t advance_fader(uint8_t current, bool visible, uint64_t ticks) {
    unsigned step = ticks > 20 ? 20 : (unsigned)ticks;
    if (visible)
        return (uint8_t)(current + step > 20 ? 20 : current + step);
    return (uint8_t)(step > current ? 0 : current - step);
}

bool gc_help_advance(GcHelpState *state, const gc_menu *menu, uint64_t ticks) {
    bool labels[GC_HELP_ENTRY_LIMIT] = {false};
    bool icons[6] = {false};
    unsigned entries[3];
    if (!state || !menu || menu->page < GC_PAGE_STARTUP ||
        menu->page > GC_PAGE_GAME_STARTED)
        return false;
    for (unsigned index = 0; index < GC_HELP_ENTRY_LIMIT; ++index)
        if (state->labels[index] > 20)
            return false;
    for (unsigned index = 0; index < 6; ++index)
        if (state->icons[index] > 20)
            return false;
    size_t count = gc_text_help_entries(menu, entries, 3);
    for (size_t index = 0; index < count; ++index)
        labels[entries[index]] = true;
    switch (menu->page) {
        case GC_PAGE_CUBE:
            icons[3] = true;
            break;
        case GC_PAGE_FACE:
            icons[4] = icons[5] = true;
            break;
        case GC_PAGE_DISC:
            icons[1] = true;
            break;
        case GC_PAGE_CALENDAR:
        case GC_PAGE_OPTIONS:
        case GC_PAGE_CARDS:
        case GC_PAGE_CARD_ACTION:
        case GC_PAGE_CARD_CONFIRM:
            icons[0] = icons[1] = icons[2] = true;
            break;
        default:
            break;
    }
    for (unsigned index = 0; index < GC_HELP_ENTRY_LIMIT; ++index)
        state->labels[index] =
            advance_fader(state->labels[index], labels[index], ticks);
    for (unsigned index = 0; index < 6; ++index)
        state->icons[index] = advance_fader(state->icons[index], icons[index], ticks);
    state->phase = (uint16_t)(state->phase + (uint16_t)ticks);
    unsigned intro_step = ticks > 64 ? 256 : (unsigned)ticks * 4;
    state->control_intro = (uint8_t)(state->control_intro + intro_step > 255
                                         ? 255
                                         : state->control_intro + intro_step);
    return true;
}

uint8_t gc_help_entry_alpha(const GcHelpState *state, unsigned entry) {
    if (!state || entry >= GC_HELP_ENTRY_LIMIT || state->labels[entry] > 20)
        return 0;
    return (uint8_t)(state->labels[entry] * 255 / 20);
}

static uint32_t opacity(uint32_t color, unsigned alpha) {
    return (color & 0xffffff00) | ((color & 255) * alpha / 255);
}

bool gc_help_pane(const GcHelpStyle *style, const GcHelpState *state,
                  const GcLayoutTable *table, unsigned index, uint8_t alpha,
                  GcHelpPane *pane) {
    GcHelpPane candidate;
    char name[5] = {0};
    unsigned slot;
    unsigned value;
    uint32_t color;
    if (!style || !state || !table || !pane || index >= GC_HELP_PANE_DRAWS)
        return false;
    for (unsigned item = 0; item < 6; ++item)
        if (state->icons[item] > 20)
            return false;
    unsigned tick = state->phase;
    if (index < 6) {
        slot = index;
        memcpy(name, "bac1", 5);
        name[3] = (char)('1' + slot);
        unsigned pulse = tick % 256 > 127 ? 100 + tick % 128 * 155 / 127
                                          : 255 - tick % 128 * 155 / 127;
        value = pulse * alpha / 255;
        color = style->backgrounds[slot % 3];
    } else if (index < 10) {
        unsigned stick = (index - 6) / 2;
        slot = stick * 3;
        unsigned segment = tick / 128 % 8;
        unsigned normal = tick % 128 * 255 / 127;
        if (segment % 2)
            normal = 255 - normal;
        normal = normal * alpha / 255;
        bool center = index % 2 != 0;
        unsigned direction = center ? 0 : ((segment + 1) / 2 + 3) % 4 + 1;
        memcpy(name, "stk0", 5);
        name[3] = (char)('0' + stick * 5 + direction);
        value = center ? normal : 255 - normal;
        color = style->controls[0];
    } else {
        static const char names[10][5] = {"butb", "dot2", "butc", "dot5", "buta",
                                          "dot3", "butd", "dot6", "dot1", "dot4"};
        static const unsigned slots[10] = {1, 1, 4, 4, 2, 2, 5, 5, 0, 3};
        unsigned item = index - 10;
        memcpy(name, names[item], sizeof(name));
        slot = slots[item];
        value = state->control_intro * alpha / 255;
        color = item % 2 || item >= 8 ? style->dots : style->controls[slot % 3];
    }
    value = value * (state->icons[slot] * 255 / 20) / 255;
    candidate.tint = opacity(color, value);
    if (!gc_layout_find_pane(table, name, 0, &candidate.pane) ||
        !gc_layout_pane_quad(&candidate.pane, candidate.vertices))
        return false;
    *pane = candidate;
    return true;
}
