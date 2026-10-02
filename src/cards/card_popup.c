#include "gamecube/card_popup.h"
#include "resources/native_constants.h"

#include <math.h>

static bool valid_style(const GcCardPopupStyle *style) {
    return style && style->duration && style->duration <= 1000 &&
           style->collapse_begin && style->collapse_begin < style->choices_begin &&
           style->choices_begin < style->confirmation_duration &&
           style->confirmation_duration <= 1000 && style->open_offset <= 1000 &&
           style->choices_offset <= 1000 && style->border_step &&
           style->border_step <= 255;
}

bool gc_card_popup_style_decode(const GcText *text, GcCardPopupStyle *style) {
    uint16_t fields[5], offsets[2], duration, border_step;
    if (!text || !style ||
        !gc_native_halfwords(text, text->europe ? 0x1ae64 : 0x19eac,
                             text->europe ? 0x1aea8 : 0x19ef0, 3, 2, 5, fields) ||
        !gc_native_halfwords(text, text->europe ? 0x1ae64 : 0x19eac,
                             text->europe ? 0x1aea8 : 0x19ef0, 3, 16, 2, offsets) ||
        !gc_native_halfwords(text, text->europe ? 0xb30c : 0xb4e8,
                             text->europe ? 0xb334 : 0xb510, 3, 2, 1, &duration) ||
        !gc_native_halfwords(text, text->europe ? 0x16020 : 0x15324,
                             text->europe ? 0x16154 : 0x15458, 3, 0x14, 1,
                             &border_step) ||
        fields[1] || duration != fields[0])
        return false;
    GcCardPopupStyle candidate = {duration,   fields[2],  fields[3],  fields[4],
                                  offsets[0], offsets[1], border_step};
    if (!valid_style(&candidate))
        return false;
    *style = candidate;
    return true;
}

static uint16_t approach(uint16_t value, unsigned target, uint64_t ticks) {
    if (value < target)
        return ticks >= target - value ? (uint16_t)target : (uint16_t)(value + ticks);
    return ticks >= value - target ? (uint16_t)target : (uint16_t)(value - ticks);
}

static uint8_t alpha(unsigned counter, unsigned duration) {
    return counter <= duration ? (uint8_t)(counter * 255 / duration) : 0;
}

bool gc_card_popups_advance(const GcCardPopupStyle *style, GcCardPopups *state,
                            const gc_menu *menu, uint64_t ticks) {
    if (!valid_style(style) || !state || !menu || menu->card_slot > 1)
        return false;
    for (unsigned slot = 0; slot < 2; ++slot) {
        if (state->availability_ticks[slot] > style->duration ||
            state->action_ticks[slot] > style->duration ||
            state->confirmation_ticks[slot] > style->confirmation_duration)
            return false;
        for (unsigned stage = 0; stage < 2; ++stage)
            if (state->format_ticks[slot][stage] > style->duration)
                return false;
    }
    for (unsigned index = 0; index < GC_CARD_POPUP_MESSAGE_COUNT; ++index)
        if (state->message_ticks[index] > style->duration)
            return false;
    bool action_page = menu->page == GC_PAGE_CARD_ACTION ||
                       (menu->page == GC_PAGE_CARD_CONFIRM &&
                        menu->card_action != GC_CARD_ACTION_FORMAT);
    bool format_page = menu->page == GC_PAGE_CARD_CONFIRM &&
                       menu->card_action == GC_CARD_ACTION_FORMAT;
    bool card_controller =
        menu->page == GC_PAGE_CARDS || action_page || format_page ||
        (menu->page == GC_PAGE_MESSAGE && menu->message_return_page == GC_PAGE_CARDS);
    for (unsigned slot = 0; slot < 2; ++slot) {
        /* USA 171e4 / PAL 17f40: the header is independent of the
         * selected-slot spotlight and appears only for READY metadata. */
        state->availability_ticks[slot] =
            approach(state->availability_ticks[slot],
                     card_controller && menu->cards[slot].status == GC_CARD_READY
                         ? style->duration
                         : 0,
                     ticks);
        /* USA 196d0 / PAL 1a688 advances the phase even for a closed popup.
         * Its draw routine submits only the counter's low byte. */
        if (card_controller)
            state->border_phase[slot] = (uint8_t)(state->border_phase[slot] +
                                                  (ticks % 256) * style->border_step);
        bool shown = action_page && slot == menu->card_slot;
        if (shown)
            state->actions[slot] =
                (GcCardPopupRecord){menu->card_index, menu->card_first_row,
                                    menu->card_action, menu->confirm_yes, true};
        /* USA 0x81318124 dispatches 0x813196d0: mode0 opens the action body,
         * mode2 also collapses its rows and opens Yes/No, mode1 closes both. */
        state->action_ticks[slot] =
            approach(state->action_ticks[slot], shown ? style->duration : 0, ticks);
        state->confirmation_ticks[slot] = approach(
            state->confirmation_ticks[slot],
            shown && menu->page == GC_PAGE_CARD_CONFIRM ? style->confirmation_duration
                                                        : 0,
            ticks);
        for (unsigned stage = 0; stage < 2; ++stage) {
            bool format = format_page && slot == menu->card_slot &&
                          stage == (unsigned)menu->format_second_confirmation;
            if (format)
                state->format_yes[slot][stage] = menu->confirm_yes;
            state->format_ticks[slot][stage] = approach(
                state->format_ticks[slot][stage], format ? style->duration : 0, ticks);
        }
    }
    GcTextGroup group;
    unsigned active_entry = GC_CARD_POPUP_MESSAGE_COUNT;
    if (menu->page == GC_PAGE_MESSAGE && menu->message_return_page == GC_PAGE_CARDS) {
        if (!gc_text_message_index(menu, &group, &active_entry) ||
            group != GC_TEXT_CARD)
            active_entry = GC_CARD_POPUP_MESSAGE_COUNT - 1;
    }
    for (unsigned index = 0; index < GC_CARD_POPUP_MESSAGE_COUNT; ++index) {
        bool shown = index == active_entry;
        if (card_controller && index >= 33 && index <= 36) {
            unsigned slot = (index - 33) & 1u;
            gc_card_status status = index < 35 ? GC_CARD_ABSENT : GC_CARD_DAMAGED;
            /* USA 171e4 / PAL 17f40 closes both status notices during
             * operation messages. Formatting retains only the other slot. */
            bool notice =
                menu->page != GC_PAGE_MESSAGE ||
                (menu->message == GC_MESSAGE_CARD_FORMATTED && slot != menu->card_slot);
            shown |= notice && menu->cards[slot].status == status;
        }
        state->message_ticks[index] =
            approach(state->message_ticks[index], shown ? style->duration : 0, ticks);
    }
    return true;
}

uint8_t gc_card_popup_header_alpha(const GcCardPopupStyle *style,
                                   const GcCardPopups *state, unsigned slot) {
    if (!valid_style(style) || !state || slot > 1)
        return 0;
    return alpha(state->availability_ticks[slot], style->duration);
}

bool gc_card_popup_border(const GcCardPopupStyle *style, const GcCardPopups *state,
                          unsigned slot, float left, float top, float width,
                          float height, uint8_t opacity, GcCardPopupBorder *border) {
    if (!valid_style(style) || !state || !border || slot > 1 || !isfinite(left) ||
        !isfinite(top) || !isfinite(width) || !isfinite(height) || width < 0 ||
        height < 0 || width > 4096 || height > 4096)
        return false;
    unsigned phase = state->border_phase[slot];
    float expansion = (float)(phase + 16) / 16;
    unsigned fade = (256 - phase) * 255 / 256;
    /* USA 197fc / PAL 1a7b4: GX_LINES, 18 sixths of a pixel, RGB192. */
    GcCardPopupBorder candidate = {.left = left - expansion,
                                   .top = top - expansion,
                                   .width = width + 2 * expansion,
                                   .height = height + 2 * expansion,
                                   .line_width = 3,
                                   .color =
                                       UINT32_C(0xc0c0c000) | (fade * opacity / 255)};
    *border = candidate;
    return true;
}

bool gc_card_popup_pose(const GcCardPopupStyle *style, const GcCardPopups *state,
                        unsigned slot, GcCardPopupPose *pose) {
    if (!valid_style(style) || !state || !pose || slot > 1 ||
        state->action_ticks[slot] > style->duration ||
        state->confirmation_ticks[slot] > style->confirmation_duration)
        return false;
    unsigned open = state->action_ticks[slot];
    unsigned confirmation = state->confirmation_ticks[slot];
    unsigned spacing_duration = style->choices_begin - style->collapse_begin;
    unsigned choices_duration = style->confirmation_duration - style->choices_begin;
    unsigned spacing = confirmation < style->collapse_begin ? spacing_duration
                       : confirmation < style->choices_begin
                           ? style->choices_begin - confirmation
                           : 0;
    GcCardPopupPose candidate = {
        .body_alpha = alpha(open, style->duration),
        .other_rows_alpha = confirmation < style->collapse_begin
                                ? (uint8_t)(alpha(open, style->duration) *
                                            (style->collapse_begin - confirmation) /
                                            style->collapse_begin)
                                : 0,
        .choices_alpha =
            confirmation > style->choices_begin
                ? alpha(confirmation - style->choices_begin, choices_duration)
                : 0,
        .row_spacing = (float)spacing / (float)spacing_duration,
        .open_offset = (float)(style->open_offset * (style->duration - open)) /
                       (float)style->duration,
        .choices_offset = confirmation >= style->choices_begin
                              ? (float)(style->choices_offset *
                                        (style->confirmation_duration - confirmation)) /
                                    (float)choices_duration
                              : 0};
    *pose = candidate;
    return true;
}

uint8_t gc_card_popup_format_alpha(const GcCardPopupStyle *style,
                                   const GcCardPopups *state, unsigned slot,
                                   unsigned stage, bool choices) {
    if (!valid_style(style) || !state || slot > 1 || stage > 1 ||
        state->format_ticks[slot][stage] > style->duration)
        return 0;
    unsigned counter = state->format_ticks[slot][stage];
    unsigned half = style->duration / 2;
    if (!choices)
        return alpha(counter, style->duration);
    /* Original 0x81319184 uses 2*(counter-duration/2)/duration. */
    return counter > half ? (uint8_t)(2 * (counter - half) * 255 / style->duration) : 0;
}

uint8_t gc_card_popup_message_alpha(const GcCardPopupStyle *style,
                                    const GcCardPopups *state, unsigned entry) {
    if (!valid_style(style) || !state || entry >= GC_CARD_POPUP_MESSAGE_COUNT)
        return 0;
    return alpha(state->message_ticks[entry], style->duration);
}

bool gc_card_popup_can_press(const GcCardPopupStyle *style, const GcCardPopups *state,
                             const gc_menu *menu) {
    if (!valid_style(style) || !state || !menu || menu->card_slot > 1)
        return false;
    if (menu->page != GC_PAGE_CARD_CONFIRM ||
        menu->card_action != GC_CARD_ACTION_FORMAT)
        return true;
    /* USA 0x813163f4 accepts either format stage's alpha strictly above 172. */
    return gc_card_popup_format_alpha(style, state, menu->card_slot, 0, false) > 172 ||
           gc_card_popup_format_alpha(style, state, menu->card_slot, 1, false) > 172;
}
