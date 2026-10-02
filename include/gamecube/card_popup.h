#ifndef GAMECUBE_CARD_POPUP_H
#define GAMECUBE_CARD_POPUP_H

#include "gamecube/text.h"

enum { GC_CARD_POPUP_MESSAGE_COUNT = 48 };

typedef struct {
    uint16_t duration;
    uint16_t confirmation_duration;
    uint16_t collapse_begin;
    uint16_t choices_begin;
    uint16_t open_offset;
    uint16_t choices_offset;
    uint16_t border_step;
} GcCardPopupStyle;

/* Retain dialog coordinates without retaining the card metadata or its art. */
typedef struct {
    size_t file_index;
    size_t first_row;
    gc_card_action action;
    bool confirm_yes;
    bool valid;
} GcCardPopupRecord;

typedef struct {
    uint16_t availability_ticks[2];
    uint16_t action_ticks[2];
    uint16_t confirmation_ticks[2];
    uint16_t format_ticks[2][2];
    uint16_t message_ticks[GC_CARD_POPUP_MESSAGE_COUNT];
    uint8_t border_phase[2];
    GcCardPopupRecord actions[2];
    bool format_yes[2][2];
} GcCardPopups;

typedef struct {
    uint8_t body_alpha;
    uint8_t other_rows_alpha;
    uint8_t choices_alpha;
    float row_spacing;
    float open_offset;
    float choices_offset;
} GcCardPopupPose;

typedef struct {
    float left;
    float top;
    float width;
    float height;
    float line_width;
    uint32_t color;
} GcCardPopupBorder;

bool gc_card_popup_style_decode(const GcText *text, GcCardPopupStyle *style);
bool gc_card_popups_advance(const GcCardPopupStyle *style, GcCardPopups *state,
                            const gc_menu *menu, uint64_t ticks);
bool gc_card_popup_pose(const GcCardPopupStyle *style, const GcCardPopups *state,
                        unsigned slot, GcCardPopupPose *pose);
/* The native border expands by its low-byte phase and fades before wrapping. */
bool gc_card_popup_border(const GcCardPopupStyle *style, const GcCardPopups *state,
                          unsigned slot, float left, float top, float width,
                          float height, uint8_t alpha, GcCardPopupBorder *border);
/* USA 18d58 / PAL 19d70 submit slot headers through the READY fader. */
uint8_t gc_card_popup_header_alpha(const GcCardPopupStyle *style,
                                   const GcCardPopups *state, unsigned slot);
uint8_t gc_card_popup_format_alpha(const GcCardPopupStyle *style,
                                   const GcCardPopups *state, unsigned slot,
                                   unsigned stage, bool choices);
uint8_t gc_card_popup_message_alpha(const GcCardPopupStyle *style,
                                    const GcCardPopups *state, unsigned entry);
/* Only the native format controller applies this dialog-specific alpha gate.
 * Action and transfer-confirmation controllers accept input while animating. */
bool gc_card_popup_can_press(const GcCardPopupStyle *style, const GcCardPopups *state,
                             const gc_menu *menu);

#endif
