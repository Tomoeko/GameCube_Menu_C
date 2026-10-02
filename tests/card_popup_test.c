#include "gamecube/card_popup.h"
#include "gamecube/layout.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static void test_native_counter_trace(void) {
    GcCardPopupStyle style = {20, 30, 10, 20, 40, 20, 5};
    GcCardPopups state = {0};
    GcCardPopupPose pose;
    gc_menu menu;
    gc_menu_init(&menu, GC_REGION_USA);
    menu.page = GC_PAGE_CARD_ACTION;
    menu.card_action = GC_CARD_ACTION_ERASE;
    menu.card_index = 9;
    assert(gc_card_popups_advance(&style, &state, &menu, 0));
    assert(gc_card_popup_pose(&style, &state, 0, &pose));
    assert(pose.body_alpha == 0 && pose.open_offset == 40);
    assert(gc_card_popup_can_press(&style, &state, &menu));
    assert(gc_card_popups_advance(&style, &state, &menu, 10));
    assert(gc_card_popup_pose(&style, &state, 0, &pose));
    assert(pose.body_alpha == 127 && pose.open_offset == 20);
    assert(pose.other_rows_alpha == 127 && pose.row_spacing == 1);
    assert(gc_card_popups_advance(&style, &state, &menu, 10));
    menu.page = GC_PAGE_CARD_CONFIRM;
    assert(gc_card_popups_advance(&style, &state, &menu, 10));
    assert(gc_card_popup_pose(&style, &state, 0, &pose));
    assert(pose.body_alpha == 255 && pose.other_rows_alpha == 0);
    assert(pose.row_spacing == 1 && pose.choices_alpha == 0);
    assert(gc_card_popup_can_press(&style, &state, &menu));
    assert(gc_card_popups_advance(&style, &state, &menu, 5));
    assert(gc_card_popup_pose(&style, &state, 0, &pose));
    assert(pose.row_spacing == 0.5f && pose.choices_alpha == 0);
    assert(gc_card_popups_advance(&style, &state, &menu, 5));
    assert(gc_card_popup_pose(&style, &state, 0, &pose));
    assert(pose.row_spacing == 0 && pose.choices_alpha == 0);
    assert(pose.choices_offset == 20);
    assert(gc_card_popups_advance(&style, &state, &menu, 1));
    assert(gc_card_popup_pose(&style, &state, 0, &pose));
    assert(pose.choices_alpha == 25 && pose.choices_offset == 18);
    menu.confirm_yes = true;
    assert(gc_card_popups_advance(&style, &state, &menu, UINT64_MAX));
    assert(gc_card_popup_pose(&style, &state, 0, &pose));
    assert(pose.choices_alpha == 255 && pose.choices_offset == 0);
    assert(state.actions[0].confirm_yes);
    menu.page = GC_PAGE_CARDS;
    menu.card_index = 2;
    assert(gc_card_popups_advance(&style, &state, &menu, 1));
    assert(gc_card_popup_pose(&style, &state, 0, &pose));
    assert(pose.body_alpha == 242 && pose.choices_alpha == 229);
    assert(state.actions[0].file_index == 9);
    GcCardPopups saved = state;
    assert(gc_card_popups_advance(&style, &state, &menu, 0));
    assert(memcmp(&saved, &state, sizeof(state)) == 0);
    assert(gc_card_popups_advance(&style, &state, &menu, UINT64_MAX));
    assert(gc_card_popup_pose(&style, &state, 0, &pose));
    assert(pose.body_alpha == 0 && pose.choices_alpha == 0);
}

static void test_format_gate_and_messages(void) {
    GcCardPopupStyle style = {20, 30, 10, 20, 40, 20, 5};
    GcCardPopups state = {0};
    gc_menu menu;
    gc_menu_init(&menu, GC_REGION_EUROPE);
    menu.page = GC_PAGE_CARD_CONFIRM;
    menu.card_action = GC_CARD_ACTION_FORMAT;
    menu.card_slot = 1;
    assert(!gc_card_popup_can_press(&style, &state, &menu));
    assert(gc_card_popups_advance(&style, &state, &menu, 10));
    assert(gc_card_popup_format_alpha(&style, &state, 1, 0, false) == 127);
    assert(gc_card_popup_format_alpha(&style, &state, 1, 0, true) == 0);
    assert(gc_card_popups_advance(&style, &state, &menu, 3));
    assert(gc_card_popup_format_alpha(&style, &state, 1, 0, false) == 165);
    assert(!gc_card_popup_can_press(&style, &state, &menu));
    assert(gc_card_popups_advance(&style, &state, &menu, 1));
    assert(gc_card_popup_format_alpha(&style, &state, 1, 0, false) == 178);
    assert(gc_card_popup_format_alpha(&style, &state, 1, 0, true) == 102);
    assert(gc_card_popup_can_press(&style, &state, &menu));
    assert(gc_card_popups_advance(&style, &state, &menu, 6));
    menu.format_second_confirmation = true;
    assert(gc_card_popups_advance(&style, &state, &menu, 1));
    assert(gc_card_popup_format_alpha(&style, &state, 1, 0, false) == 242);
    assert(gc_card_popup_format_alpha(&style, &state, 1, 1, false) == 12);
    assert(gc_card_popup_can_press(&style, &state, &menu));
    assert(gc_card_popups_advance(&style, &state, &menu, 19));
    assert(gc_card_popup_format_alpha(&style, &state, 1, 0, false) == 0);
    assert(gc_card_popup_format_alpha(&style, &state, 1, 1, false) == 255);
    menu.page = GC_PAGE_MESSAGE;
    menu.message_return_page = GC_PAGE_CARDS;
    menu.message = GC_MESSAGE_CARD_FORMATTED;
    assert(gc_card_popups_advance(&style, &state, &menu, 10));
    assert(gc_card_popup_message_alpha(&style, &state, 44) == 127);
    assert(gc_card_popup_format_alpha(&style, &state, 1, 1, false) == 127);
    menu.message = GC_MESSAGE_CARD_SAVE_FAILED;
    assert(gc_card_popups_advance(&style, &state, &menu, 10));
    assert(gc_card_popup_message_alpha(&style, &state, 44) == 0);
    assert(gc_card_popup_message_alpha(&style, &state, 47) == 127);
    GcCardPopups saved = state;
    style.duration = 0;
    assert(!gc_card_popups_advance(&style, &state, &menu, 1));
    assert(memcmp(&saved, &state, sizeof(state)) == 0);
}

static void test_expanding_border(void) {
    GcCardPopupStyle style = {20, 30, 10, 20, 40, 20, 5};
    GcCardPopups state = {0};
    GcCardPopupBorder border;
    gc_menu menu;
    gc_menu_init(&menu, GC_REGION_USA);
    menu.page = GC_PAGE_CARDS;
    assert(gc_card_popup_border(&style, &state, 0, 100, 200, 80, 90, 255, &border));
    assert(border.left == 99 && border.top == 199);
    assert(border.width == 82 && border.height == 92);
    assert(border.line_width == 3 && border.color == UINT32_C(0xc0c0c0ff));
    assert(gc_card_popups_advance(&style, &state, &menu, 16));
    assert(state.border_phase[0] == 80 && state.border_phase[1] == 80);
    assert(gc_card_popup_border(&style, &state, 1, 100, 200, 80, 90, 127, &border));
    assert(border.left == 94 && border.top == 194);
    assert(border.width == 92 && border.height == 102);
    assert(border.color == UINT32_C(0xc0c0c057));
    GcCardPopups sampled = state;
    assert(gc_card_popups_advance(&style, &state, &menu, 256));
    assert(memcmp(sampled.border_phase, state.border_phase,
                  sizeof(state.border_phase)) == 0);
    assert(gc_card_popups_advance(&style, &state, &menu, UINT64_MAX));
    assert(state.border_phase[0] == 75);
    menu.page = GC_PAGE_CUBE;
    sampled = state;
    assert(gc_card_popups_advance(&style, &state, &menu, UINT64_MAX));
    assert(memcmp(sampled.border_phase, state.border_phase,
                  sizeof(state.border_phase)) == 0);
    state.border_phase[0] = 255;
    assert(gc_card_popup_border(&style, &state, 0, 100, 200, 80, 90, 255, &border));
    assert(border.left == 83.0625f && !(border.color & 255));
    GcCardPopupBorder saved = border;
    assert(!gc_card_popup_border(&style, &state, 2, 100, 200, 80, 90, 255, &border));
    assert(!gc_card_popup_border(&style, &state, 0, NAN, 200, 80, 90, 255, &border));
    assert(!gc_card_popup_border(&style, &state, 0, 100, 200, -1, 90, 255, &border));
    assert(memcmp(&saved, &border, sizeof(border)) == 0);
}

static void test_card_availability_messages(void) {
    GcCardPopupStyle style = {20, 30, 10, 20, 40, 20, 5};
    GcCardPopups state = {0};
    gc_menu menu;
    gc_menu_init(&menu, GC_REGION_USA);
    menu.page = GC_PAGE_CARDS;
    assert(gc_card_popups_advance(&style, &state, &menu, 10));
    assert(gc_card_popup_header_alpha(&style, &state, 0) == 0);
    assert(gc_card_popup_header_alpha(&style, &state, 1) == 0);
    assert(gc_card_popup_message_alpha(&style, &state, 33) == 127);
    assert(gc_card_popup_message_alpha(&style, &state, 34) == 127);
    assert(gc_card_popups_advance(&style, &state, &menu, 10));
    assert(gc_card_popup_message_alpha(&style, &state, 33) == 255);
    menu.cards[0].status = GC_CARD_READY;
    menu.cards[1].status = GC_CARD_DAMAGED;
    assert(gc_card_popups_advance(&style, &state, &menu, 10));
    assert(gc_card_popup_header_alpha(&style, &state, 0) == 127);
    assert(gc_card_popup_header_alpha(&style, &state, 1) == 0);
    assert(gc_card_popup_message_alpha(&style, &state, 33) == 127);
    assert(gc_card_popup_message_alpha(&style, &state, 34) == 127);
    assert(gc_card_popup_message_alpha(&style, &state, 36) == 127);
    assert(gc_card_popups_advance(&style, &state, &menu, 10));
    assert(gc_card_popup_header_alpha(&style, &state, 0) == 255);
    assert(gc_card_popup_message_alpha(&style, &state, 33) == 0);
    assert(gc_card_popup_message_alpha(&style, &state, 34) == 0);
    assert(gc_card_popup_message_alpha(&style, &state, 36) == 255);
    GcCardPopups paused = state;
    assert(gc_card_popups_advance(&style, &state, &menu, 0));
    assert(!memcmp(&paused, &state, sizeof(state)));
    menu.cards[0].status = GC_CARD_ABSENT;
    assert(gc_card_popups_advance(&style, &state, &menu, 20));
    assert(gc_card_popup_header_alpha(&style, &state, 0) == 0);
    assert(gc_card_popup_message_alpha(&style, &state, 33) == 255);
    menu.cards[0].status = GC_CARD_READY;
    menu.cards[0].file_count = 1;
    menu.cards[0].files[0].blocks = 1;
    menu.cards[1].status = GC_CARD_ABSENT;
    assert(gc_card_popups_advance(&style, &state, &menu, 20));
    assert(gc_card_popup_message_alpha(&style, &state, 34) == 255);
    menu.card_action = GC_CARD_ACTION_COPY;
    menu.page = GC_PAGE_MESSAGE;
    menu.message_return_page = GC_PAGE_CARDS;
    menu.message = GC_MESSAGE_CARD_ABSENT;
    assert(gc_card_popups_advance(&style, &state, &menu, 10));
    assert(gc_card_popup_message_alpha(&style, &state, 34) == 127);
    assert(gc_card_popup_message_alpha(&style, &state, 22) == 127);
    assert(gc_card_popups_advance(&style, &state, &menu, 10));
    assert(gc_card_popup_message_alpha(&style, &state, 34) == 0);
    assert(gc_card_popup_message_alpha(&style, &state, 22) == 255);
    menu.message = GC_MESSAGE_CARD_FORMATTED;
    assert(gc_card_popups_advance(&style, &state, &menu, 20));
    assert(gc_card_popup_message_alpha(&style, &state, 34) == 255);
    assert(gc_card_popup_message_alpha(&style, &state, 43) == 255);
    state.availability_ticks[0] = 21;
    paused = state;
    assert(!gc_card_popups_advance(&style, &state, &menu, 1));
    assert(!memcmp(&paused, &state, sizeof(state)));
}

static void test_rom(const char *path) {
    GcText text = {0};
    GcCardPopupStyle style = {0};
    assert(gc_text_load(path, &text));
    assert(gc_card_popup_style_decode(&text, &style));
    assert(style.duration == 20 && style.confirmation_duration == 30);
    assert(style.collapse_begin == 10 && style.choices_begin == 20);
    assert(style.open_offset == 40 && style.choices_offset == 20);
    assert(style.border_step == 5);
    GcLayouts layouts;
    assert(gc_layouts_index(&text, &layouts));
    for (unsigned language = 0; language < 7; ++language) {
        const GcTextTable *messages =
            gc_text_table(&text, (gc_language)language, GC_TEXT_CARD);
        const GcLayoutTable *table =
            gc_layout_table(&layouts, (gc_language)language, GC_LAYOUT_CARD);
        if (!messages || !table)
            continue;
        for (unsigned slot = 0; slot < 2; ++slot) {
            GcTextEntry message;
            GcLayoutText row;
            GcLayoutFrame frame;
            assert(gc_text_table_entry(messages, 33 + slot, &message));
            assert(gc_layout_text(table, message.flags, &row));
            assert(!strcmp(row.name, slot ? "txt5" : "txt4"));
            assert(row.frame_index == UINT16_MAX);
            assert(gc_text_table_entry(messages, 35 + slot, &message));
            assert(gc_layout_text(table, message.flags, &row));
            assert(gc_layout_frame(table, row.frame_index, &frame));
            assert(!strcmp(frame.name, slot ? "mes3" : "mes2"));
            assert(frame.colors[0] == UINT32_C(0xa00000d2));
        }
    }
    GcCardPopupStyle saved = style;
    text.rom_size = 128;
    assert(!gc_card_popup_style_decode(&text, &style));
    assert(memcmp(&saved, &style, sizeof(style)) == 0);
    gc_text_destroy(&text);
}

int main(int argc, char **argv) {
    test_native_counter_trace();
    test_format_gate_and_messages();
    test_expanding_border();
    test_card_availability_messages();
    for (int index = 1; index < argc; ++index)
        test_rom(argv[index]);
    puts("Native card popup counter traces, format gates and retained fade tests "
         "passed.");
    return 0;
}
