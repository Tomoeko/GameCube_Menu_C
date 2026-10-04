#include "gamecube/layout.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static void put_u16(uint8_t *bytes, unsigned value) {
    bytes[0] = (uint8_t)(value >> 8);
    bytes[1] = (uint8_t)value;
}

static void put_u32(uint8_t *bytes, uint32_t value) {
    bytes[0] = (uint8_t)(value >> 24);
    bytes[1] = (uint8_t)(value >> 16);
    bytes[2] = (uint8_t)(value >> 8);
    bytes[3] = (uint8_t)value;
}

static void build_table(uint8_t bytes[224]) {
    memset(bytes, 0, 224);
    memcpy(bytes, "GLH0", 4);
    put_u32(bytes + 4, 24);
    put_u32(bytes + 8, 56);
    put_u32(bytes + 12, 128);
    put_u16(bytes + 16, 2);
    put_u16(bytes + 18, 2);
    put_u16(bytes + 20, 1);
    for (unsigned index = 0; index < 2; ++index) {
        uint8_t *pane = bytes + 24 + index * 16;
        uint8_t *text = bytes + 56 + index * 36;
        memcpy(pane, "same", 4);
        put_u16(pane + 4, 1600 + 16 * index);
        put_u16(pane + 6, 3200);
        put_u16(pane + 8, 640);
        put_u16(pane + 10, 960);
        put_u16(pane + 12, 38 + index);
        pane[14] = 15;
        pane[15] = (uint8_t)index;
        memcpy(text, "text", 4);
        memcpy(text + 4, pane + 4, 8);
        put_u32(text + 12, 0x12345678);
        put_u32(text + 16, 0x90abcdef);
        text[20] = 0x82;
        text[21] = 2;
        put_u16(text + 22, 0xfffe);
        put_u16(text + 24, 26);
        put_u16(text + 26, 22);
        put_u16(text + 28, index ? UINT16_MAX : 0);
        put_u16(text + 30, 3);
        put_u32(text + 32, 120 - 32 * index);
        memcpy(bytes + 176 + index * 4, index ? "two" : "one", 4);
    }
    memcpy(bytes + 128, "frme", 4);
    memcpy(bytes + 132, bytes + 28, 8);
    for (unsigned index = 0; index < 10; ++index)
        put_u16(bytes + 140 + index * 2, 100 + index);
    for (unsigned index = 0; index < 4; ++index)
        put_u32(bytes + 160 + index * 4, 0xaabbcc00 + index);
}

static void test_records_and_duplicate_names(void) {
    uint8_t bytes[224];
    GcLayoutTable table = {0};
    GcLayoutPane pane;
    GcLayoutText text;
    GcLayoutFrame frame;

    build_table(bytes);
    assert(gc_layout_table_decode(bytes, sizeof(bytes), &table));
    assert(table.pane_count == 2 && table.text_count == 2 && table.frame_count == 1);
    assert(gc_layout_find_pane(&table, "same", 1, &pane));
    assert(pane.box.center_x == 101 && pane.box.center_y == 200);
    assert(pane.box.width == 40 && pane.box.height == 60);
    assert(pane.texture == 39 && pane.alignment == 15 && pane.flags == 1);
    assert(strcmp(pane.name, "same") == 0);
    assert(!gc_layout_find_pane(&table, "same", 2, &pane));
    assert(!gc_layout_find_pane(&table, "none", 0, &pane));
    assert(gc_layout_find_text(&table, "text", 0, &text));
    assert(text.color_first == 0x12345678 && text.color_second == 0x90abcdef);
    assert(text.horizontal_alignment == 0x82 && text.vertical_alignment == 2);
    assert(text.letter_spacing == -2 && text.line_spacing == 26);
    assert(text.font_height == 22 && text.frame_index == 0);
    assert(text.embedded_text == (const char *)bytes + 176);
    assert(text.embedded_length == 3 && strcmp(text.embedded_text, "one") == 0);
    assert(gc_layout_find_text(&table, "text", 1, &text));
    assert(text.frame_index == UINT16_MAX && strcmp(text.embedded_text, "two") == 0);
    assert(!gc_layout_text(&table, 2, &text));
    assert(gc_layout_find_frame(&table, "frme", 0, &frame));
    assert(frame.box.width == 40 && frame.box.height == 60);
    assert(frame.parameters[0] == 100 && frame.parameters[9] == 109);
    assert(frame.colors[0] == 0xaabbcc00 && frame.colors[3] == 0xaabbcc03);
    assert(!gc_layout_frame(&table, 1, &frame));
    assert(!gc_layout_find_frame(&table, "frme", 1, &frame));
}

static void test_untrusted_records(void) {
    uint8_t bytes[224];
    GcLayoutTable table = {0};
    GcLayoutTable saved;
    GcLayoutPane pane;

    build_table(bytes);
    assert(gc_layout_table_decode(bytes, sizeof(bytes), &table));
    saved = table;
    assert(!gc_layout_table_decode(bytes, 23, &table));
    assert(!gc_layout_table_decode(bytes, 175, &table));
    assert(memcmp(&saved, &table, sizeof(table)) == 0);
    put_u32(bytes + 4, UINT32_MAX);
    assert(!gc_layout_table_decode(bytes, sizeof(bytes), &table));
    build_table(bytes);
    put_u32(bytes + 8, 40);
    assert(!gc_layout_table_decode(bytes, sizeof(bytes), &table));
    build_table(bytes);
    put_u32(bytes + 12, 100);
    assert(!gc_layout_table_decode(bytes, sizeof(bytes), &table));
    build_table(bytes);
    put_u16(bytes + 16, 1025);
    assert(!gc_layout_table_decode(bytes, sizeof(bytes), &table));
    build_table(bytes);
    put_u32(bytes + 88, UINT32_MAX);
    assert(!gc_layout_table_decode(bytes, sizeof(bytes), &table));
    build_table(bytes);
    put_u32(bytes + 88, 72);
    assert(!gc_layout_table_decode(bytes, sizeof(bytes), &table));
    build_table(bytes);
    put_u16(bytes + 86, 2);
    assert(!gc_layout_table_decode(bytes, sizeof(bytes), &table));
    build_table(bytes);
    bytes[177] = 0;
    assert(!gc_layout_table_decode(bytes, sizeof(bytes), &table));
    build_table(bytes);
    bytes[76] = 0x83;
    assert(!gc_layout_table_decode(bytes, sizeof(bytes), &table));
    build_table(bytes);
    bytes[77] = 3;
    assert(!gc_layout_table_decode(bytes, sizeof(bytes), &table));
    build_table(bytes);
    put_u16(bytes + 84, 1);
    assert(!gc_layout_table_decode(bytes, sizeof(bytes), &table));
    saved.pane_count = UINT32_MAX;
    assert(!gc_layout_pane(&saved, 0, &pane));
    assert(!gc_layout_pane(NULL, 0, &pane));
}

static void test_native_uv_and_text_alignment(void) {
    GcLayoutPane pane = {0};
    GcLayoutVertex vertices[4];
    GcLayoutVertex saved[4];
    GcLayoutText text = {0};
    float x = 0;
    float y = 0;

    pane.box = (GcLayoutBox){100, 200, 40, 60};
    pane.alignment = 15;
    assert(gc_layout_pane_quad(&pane, vertices));
    assert(vertices[0].x == 80 && vertices[0].y == 170);
    assert(vertices[2].x == 120 && vertices[2].y == 230);
    assert(vertices[0].u == 0 && vertices[0].v == 0);
    assert(vertices[2].u == 1 && vertices[2].v == 1);
    pane.flags = GC_LAYOUT_FLIP_V | GC_LAYOUT_FLIP_U;
    assert(gc_layout_pane_quad(&pane, vertices));
    assert(vertices[0].u == 1 && vertices[0].v == 1);
    assert(vertices[2].u == 0 && vertices[2].v == 0);
    pane.flags = GC_LAYOUT_ROTATE_UV;
    assert(gc_layout_pane_quad(&pane, vertices));
    assert(vertices[0].u == 0 && vertices[0].v == 1);
    assert(vertices[1].u == 0 && vertices[1].v == 0);
    assert(vertices[2].u == 1 && vertices[2].v == 0);
    assert(vertices[3].u == 1 && vertices[3].v == 1);
    pane.flags = GC_LAYOUT_ROTATE_UV | GC_LAYOUT_FLIP_U;
    assert(gc_layout_pane_quad(&pane, vertices));
    assert(vertices[0].u == 1 && vertices[0].v == 1);
    assert(vertices[2].u == 0 && vertices[2].v == 0);
    memcpy(saved, vertices, sizeof(saved));
    pane.alignment = 0;
    assert(!gc_layout_pane_quad(&pane, vertices));
    assert(memcmp(saved, vertices, sizeof(saved)) == 0);
    pane.alignment = 15;
    pane.flags = 8;
    assert(!gc_layout_pane_quad(&pane, vertices));
    text.box = pane.box;
    assert(gc_layout_text_origin(&text, 20, 10, &x, &y));
    assert(x == 90 && y == 195);
    text.horizontal_alignment = 0x81;
    text.vertical_alignment = 1;
    assert(gc_layout_text_origin(&text, 20, 10, &x, &y));
    assert(x == 100 && y == 220);
    text.horizontal_alignment = 0x82;
    text.vertical_alignment = 2;
    assert(gc_layout_text_origin(&text, 20, 10, &x, &y));
    assert(x == 80 && y == 170);
    assert(!gc_layout_text_origin(&text, NAN, 10, &x, &y));
    assert(x == 80 && y == 170);
}

static void test_native_frame_geometry(void) {
    GcLayoutFrame frame = {0};
    GcIplImage textures[4] = {0};
    uint8_t texel[4] = {0};
    GcLayoutFrameQuad quad;
    GcLayoutFrameQuad saved;

    frame.box = (GcLayoutBox){100, 200, 56, 26};
    frame.parameters[2] = 40 * 16;
    frame.parameters[3] = 20 * 16;
    frame.parameters[8] = 0x1b00; /* TL=0, TR=1, BL=2, BR=3. */
    for (unsigned corner = 0; corner < 4; ++corner) {
        frame.parameters[4 + corner] = (uint16_t)corner;
        frame.colors[corner] = 0x11223380 + corner;
        textures[corner] = (GcIplImage){.width = 8, .height = 3, .rgba = texel};
    }
    assert(gc_layout_frame_quad(&frame, NULL, 0, 0, 0xaabbcc80, &quad));
    assert(!quad.textured);
    assert(quad.vertices[0].x == 80 && quad.vertices[0].y == 190);
    assert(quad.vertices[2].x == 120 && quad.vertices[2].y == 210);
    assert(quad.colors[0] == 0x11223340);
    assert(quad.colors[2] == 0x11223341);
    assert(quad.colors[3] == 0x11223341);
    assert(gc_layout_frame_quad(&frame, textures, 4, 1, 0xaabbcc80, &quad));
    assert(quad.textured && quad.texture == 0 && quad.colors[0] == 0xaabbcc80);
    assert(quad.vertices[0].x == 72 && quad.vertices[0].y == 187);
    assert(quad.vertices[2].x == 80 && quad.vertices[2].y == 190);
    assert(gc_layout_frame_quad(&frame, textures, 4, 4, 0xffffffff, &quad));
    assert(quad.texture == 3);
    assert(quad.vertices[0].x == 120 && quad.vertices[0].y == 210);
    assert(quad.vertices[2].x == 128 && quad.vertices[2].y == 213);
    assert(quad.vertices[0].u == 1 && quad.vertices[0].v == 1);
    assert(gc_layout_frame_quad(&frame, textures, 4, 5, 0xffffffff, &quad));
    assert(quad.texture == 1);
    assert(quad.vertices[0].x == 80 && quad.vertices[0].y == 187);
    assert(quad.vertices[2].x == 120 && quad.vertices[2].y == 190);
    assert(quad.vertices[0].u == 0 && quad.vertices[2].u == 0);
    assert(quad.vertices[0].v == 1 && quad.vertices[2].v == 0);
    assert(gc_layout_frame_quad(&frame, textures, 4, 6, 0xffffffff, &quad));
    assert(quad.texture == 3 && quad.vertices[0].u == 1 && quad.vertices[2].u == 1);
    assert(quad.vertices[0].y == 210 && quad.vertices[2].y == 213);
    assert(gc_layout_frame_quad(&frame, textures, 4, 7, 0xffffffff, &quad));
    assert(quad.texture == 2 && quad.vertices[0].v == 0 && quad.vertices[2].v == 0);
    assert(quad.vertices[0].x == 72 && quad.vertices[2].x == 80);
    assert(quad.vertices[0].u == 1 && quad.vertices[2].u == 0);
    assert(gc_layout_frame_quad(&frame, textures, 4, 8, 0xffffffff, &quad));
    assert(quad.texture == 3 && quad.vertices[0].v == 1 && quad.vertices[2].v == 1);
    assert(quad.vertices[0].x == 120 && quad.vertices[2].x == 128);
    saved = quad;
    assert(!gc_layout_frame_quad(&frame, textures, 4, 9, 0, &quad));
    frame.parameters[4] = 4;
    assert(!gc_layout_frame_quad(&frame, textures, 4, 1, 0, &quad));
    assert(memcmp(&saved, &quad, sizeof(quad)) == 0);
}

static void test_original_rom(const char *path) {
    GcText owner = {0};
    GcLayouts layouts = {0};
    GcLayouts saved;
    GcLayoutPane pane;
    GcLayoutText text;
    const unsigned panes[] = {4, 0, 0, 1, 43, 26, 0, 39, 6, 3, 15};
    const unsigned texts[] = {0, 3, 3, 5, 10, 6, 2, 1, 5, 1, 1};
    const unsigned frames[] = {0, 2, 2, 1, 8, 0, 1, 2, 1, 4, 2};

    assert(gc_text_load(path, &owner));
    assert(gc_layouts_index(&owner, &layouts));
    for (unsigned language = 0; language < 7; ++language) {
        bool available = owner.europe ? language != GC_LANGUAGE_JAPANESE
                                      : language == GC_LANGUAGE_ENGLISH ||
                                            language == GC_LANGUAGE_JAPANESE;
        for (unsigned group = 0; group < GC_LAYOUT_GROUP_COUNT; ++group) {
            const GcLayoutTable *table =
                gc_layout_table(&layouts, (gc_language)language, (GcLayoutGroup)group);
            unsigned pane_count = panes[group];
            unsigned text_count = texts[group];
            unsigned frame_count = frames[group];
            if (!available) {
                assert(!table);
                continue;
            }
            if (owner.europe && group == GC_LAYOUT_OPTIONS) {
                pane_count = 11;
                text_count = 4;
                frame_count = 0;
            }
            if (owner.europe && group == GC_LAYOUT_ERROR) {
                text_count = 14;
                frame_count = 2;
            }
            if (owner.europe && group == GC_LAYOUT_OPTIONS_FACE) {
                pane_count = 19;
                frame_count = 3;
            }
            if (!owner.europe && language == GC_LANGUAGE_JAPANESE &&
                group == GC_LAYOUT_CALENDAR_FACE)
                pane_count = 43;
            assert(table && table->pane_count == pane_count);
            assert(table->text_count == text_count &&
                   table->frame_count == frame_count);
            for (unsigned index = 0; index < table->pane_count; ++index) {
                GcLayoutVertex quad[4];
                assert(gc_layout_pane(table, index, &pane));
                assert(gc_layout_pane_quad(&pane, quad));
                assert(quad[2].x - quad[0].x == pane.box.width);
                assert(quad[2].y - quad[0].y == pane.box.height);
            }
            for (unsigned index = 0; index < table->text_count; ++index) {
                assert(gc_layout_text(table, index, &text));
                assert(text.embedded_length == strlen(text.embedded_text));
                assert(text.embedded_text >= (const char *)owner.rom);
                assert(text.embedded_text < (const char *)owner.rom + owner.rom_size);
            }
            for (unsigned index = 0; index < table->frame_count; ++index) {
                GcLayoutFrame frame;
                assert(gc_layout_frame(table, index, &frame));
            }
        }
    }
    const GcLayoutTable *card =
        gc_layout_table(&layouts, GC_LANGUAGE_ENGLISH, GC_LAYOUT_CARD);
    for (unsigned slot = 0; slot < 2; ++slot) {
        for (unsigned index = 0; index < 16; ++index) {
            char key[5];
            snprintf(key, sizeof(key), "i%c%02u", slot ? 'b' : 'a', index);
            assert(gc_layout_find_pane(card, key, 0, &pane));
            assert(pane.box.center_x == (float)((slot ? 332 : 84) + 56 * (index % 4)));
            assert(pane.box.center_y == (float)(106 + 56 * (index / 4)));
            assert(pane.box.width == 46 && pane.box.height == 46);
            GcLayoutCardPopup positioned;
            assert(gc_layout_card_popup(card, slot, index, false, GC_CARD_ACTION_COPY,
                                        70, 22, &positioned));
            float left = positioned.frames[0].box.center_x - 43;
            float top = positioned.frames[0].box.center_y - 47;
            assert(left == pane.box.center_x + (index % 4 < 2 ? 40 : -126));
            assert(top == pane.box.center_y - (index / 4 < 2 ? 0 : 82));
        }
    }
    assert(gc_layout_find_pane(card, "bana", 0, &pane));
    assert(pane.box.center_x == 124 && pane.box.center_y == 357);
    assert(pane.box.width == 156 && pane.box.height == 52);
    assert(gc_layout_find_pane(card, "arbd", 0, &pane));
    assert(pane.flags == GC_LAYOUT_FLIP_V);
    assert(gc_layout_find_text(card, "titl", 0, &text));
    assert(text.box.center_x == 371 && text.box.center_y == 342);
    assert(text.letter_spacing == -1 && text.line_spacing == 22);
    assert(text.font_height == 22 && text.color_first == 0xdcdcdcff);
    GcLayoutCardPopup popup;
    assert(
        gc_layout_card_popup(card, 0, 0, false, GC_CARD_ACTION_MOVE, 70, 22, &popup));
    assert(popup.frame_count == 1 && popup.row_count == 3);
    assert(popup.frames[0].box.center_x == 167);
    assert(popup.frames[0].box.center_y == 153);
    assert(popup.frames[0].parameters[2] == 86 * 16);
    assert(popup.frames[0].parameters[3] == 94 * 16);
    assert(popup.rows[0].box.center_y == 125);
    assert(popup.rows[2].box.center_y == 181);
    assert(popup.text_entries[0] == 2 && popup.text_entries[2] == 4);
    assert(
        gc_layout_card_popup(card, 1, 15, true, GC_CARD_ACTION_ERASE, 70, 22, &popup));
    assert(popup.frame_count == 2 && popup.row_count == 3);
    assert(popup.frames[0].box.center_x == 417);
    assert(popup.frames[0].box.center_y == 211);
    assert(popup.frames[1].box.center_y == 271);
    assert(popup.text_entries[0] == 4 && popup.text_entries[1] == 5 &&
           popup.text_entries[2] == 6);
    GcLayoutCardPopup popup_saved = popup;
    assert(
        !gc_layout_card_popup(card, 2, 0, false, GC_CARD_ACTION_MOVE, 70, 22, &popup));
    assert(
        !gc_layout_card_popup(card, 0, 16, false, GC_CARD_ACTION_MOVE, 70, 22, &popup));
    assert(!gc_layout_card_popup(card, 0, 0, false, GC_CARD_ACTION_FORMAT, 70, 22,
                                 &popup));
    assert(
        !gc_layout_card_popup(card, 0, 0, false, GC_CARD_ACTION_MOVE, NAN, 22, &popup));
    assert(memcmp(&popup, &popup_saved, sizeof(popup)) == 0);
    uint32_t popup_colors[2] = {0};
    assert(gc_layout_card_popup_colors(&owner, popup_colors));
    assert(popup_colors[0] == 0xffffffff && popup_colors[1] == 0xf0e600ff);
    GcLayoutGlow glow;
    GcLayoutTextPass pass;
    assert(gc_layout_glow_decode(&owner, &glow));
    assert(glow.minimum == 70 && glow.maximum == 255);
    assert(glow.half_cycle == 35);
    for (unsigned index = 0; index < 25; ++index) {
        assert(gc_layout_glow_pass(&glow, popup_colors[0], popup_colors[1], 0, 255,
                                   index, &pass));
        assert(pass.offset_x == (float)(index / 5) * 2 - 4);
        assert(pass.offset_y == (float)(index % 5) * 2 - 4);
        assert(pass.color == 0xf0e6000b);
    }
    assert(gc_layout_glow_pass(&glow, popup_colors[0], popup_colors[1], glow.half_cycle,
                               255, 12, &pass));
    assert(pass.offset_x == 0 && pass.offset_y == 0 && pass.color == 0xf0e6002a);
    assert(gc_layout_glow_pass(&glow, popup_colors[0], popup_colors[1],
                               glow.half_cycle + 1u, 255, 12, &pass));
    assert(pass.color == 0xf0e6002a); /* Endpoint is drawn twice. */
    assert(gc_layout_glow_pass(&glow, popup_colors[0], popup_colors[1],
                               2u * glow.half_cycle + 1u, 255, 12, &pass));
    assert(pass.color == 0xf0e6000b);
    assert(gc_layout_glow_pass(&glow, popup_colors[0], popup_colors[1],
                               2u * glow.half_cycle + 2u, 255, 12, &pass));
    assert(pass.color == 0xf0e6000b);
    assert(gc_layout_glow_pass(&glow, popup_colors[0], popup_colors[1], UINT64_MAX, 127,
                               25, &pass));
    assert(pass.offset_x == 0 && pass.offset_y == 0 && pass.color == 0xffffff7f);
    GcLayoutTextPass pass_saved = pass;
    assert(!gc_layout_glow_pass(&glow, popup_colors[0], popup_colors[1], 0, 255, 26,
                                &pass));
    assert(memcmp(&pass_saved, &pass, sizeof(pass)) == 0);
    GcLayoutGlow glow_saved = glow;
    const GcLayoutTable *error =
        gc_layout_table(&layouts, GC_LANGUAGE_ENGLISH, GC_LAYOUT_ERROR);
    GcLayoutText body;
    GcLayoutDialog dialog;
    assert(gc_layout_find_text(error, "txt0", 0, &body));
    assert(gc_layout_boot_dialog(error, 200.09f, 44.09f, 60, 22, &dialog));
    assert(dialog.frame_count == 2 && dialog.row_count == 3);
    assert(dialog.frames[0].parameters[2] == 216 * 16 + 1);
    assert(dialog.frames[0].parameters[3] == 60 * 16 + 1);
    assert(dialog.frames[1].parameters[2] == 76 * 16);
    assert(dialog.frames[1].parameters[3] == (38 + body.line_spacing) * 16);
    assert(
        dialog.frames[1].box.center_y - dialog.frames[1].parameters[3] / 32.0f -
            (dialog.frames[0].box.center_y + dialog.frames[0].parameters[3] / 32.0f) ==
        8);
    assert((dialog.frames[0].box.center_y - dialog.frames[0].parameters[3] / 32.0f +
            dialog.frames[1].box.center_y + dialog.frames[1].parameters[3] / 32.0f) *
               0.5f ==
           body.box.center_y);
    for (unsigned index = 0; index < 3; ++index) {
        assert(dialog.text_entries[index] == index);
        assert(dialog.rows[index].frame_index == UINT16_MAX);
        assert(dialog.rows[index].box.center_x == body.box.center_x);
    }
    assert(dialog.rows[2].box.center_y - dialog.rows[1].box.center_y ==
           body.line_spacing);
    GcLayoutDialog dialog_saved = dialog;
    assert(!gc_layout_boot_dialog(error, NAN, 44, 60, 22, &dialog));
    assert(!gc_layout_boot_dialog(error, 200, 0, 60, 22, &dialog));
    assert(!gc_layout_boot_dialog(error, 200, 44, -1, 22, &dialog));
    assert(!gc_layout_boot_dialog(card, 200, 44, 60, 22, &dialog));
    assert(memcmp(&dialog, &dialog_saved, sizeof(dialog)) == 0);
    if (owner.europe) {
        GcLayoutFrame language_frame;
        GcLayoutText language;
        assert(gc_layout_find_text(error, "lan1", 0, &language));
        assert(gc_layout_frame(error, language.frame_index, &language_frame));
        assert(gc_layout_language_dialog(error, 100, 20, &dialog));
        assert(dialog.frame_count == 1 && dialog.row_count == 6);
        assert(dialog.frames[0].box.center_x == language_frame.box.center_x);
        assert(dialog.frames[0].box.center_y == language_frame.box.center_y);
        assert(dialog.frames[0].parameters[2] == 116 * 16);
        assert(dialog.frames[0].parameters[3] == (36 + 5 * language.line_spacing) * 16);
        assert(dialog.rows[5].box.center_y - dialog.rows[0].box.center_y ==
               5 * language.line_spacing);
        for (unsigned index = 0; index < 6; ++index) {
            assert(dialog.text_entries[index] == 12 + index);
            assert(dialog.rows[index].frame_index == UINT16_MAX);
            assert(dialog.rows[index].box.center_x == language_frame.box.center_x);
        }
        dialog_saved = dialog;
        assert(!gc_layout_language_dialog(error, 100, INFINITY, &dialog));
        assert(!gc_layout_language_dialog(error, -1, 20, &dialog));
        assert(memcmp(&dialog, &dialog_saved, sizeof(dialog)) == 0);
    } else {
        assert(!gc_layout_language_dialog(error, 100, 20, &dialog));
    }
    assert(gc_layout_find_text(
        gc_layout_table(&layouts, GC_LANGUAGE_ENGLISH, GC_LAYOUT_OPTIONS), "txt1", 0,
        &text));
    assert(text.box.center_y == (owner.europe ? 102 : 111));
    assert(text.color_first == 0x00ff78ff && text.font_height == 24);
    saved = layouts;
    owner.rom_size = 128;
    assert(!gc_layout_glow_decode(&owner, &glow));
    assert(memcmp(&glow_saved, &glow, sizeof(glow)) == 0);
    assert(!gc_layout_card_popup_colors(&owner, popup_colors));
    assert(popup_colors[0] == 0xffffffff && popup_colors[1] == 0xf0e600ff);
    assert(!gc_layouts_index(&owner, &layouts));
    assert(memcmp(&saved, &layouts, sizeof(layouts)) == 0);
    assert(!gc_layout_table(&layouts, (gc_language)-1, GC_LAYOUT_CARD));
    assert(!gc_layout_table(&layouts, GC_LANGUAGE_ENGLISH, GC_LAYOUT_GROUP_COUNT));
    gc_text_destroy(&owner);
}

int main(int argc, char **argv) {
    test_records_and_duplicate_names();
    test_untrusted_records();
    test_native_uv_and_text_alignment();
    test_native_frame_geometry();
    for (int index = 1; index < argc; ++index)
        test_original_rom(argv[index]);
    puts("Native GLH0 layout parsing, placement and region tests passed.");
    return 0;
}
