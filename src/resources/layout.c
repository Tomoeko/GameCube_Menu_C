#include "gamecube/layout.h"
#include "console_common/support/endian.h"

#include "native_constants.h"

#include <math.h>
#include <string.h>

enum {
    HEADER_BYTES = 24,
    PANE_BYTES = 16,
    TEXT_BYTES = 36,
    FRAME_BYTES = 48,
    MAX_RECORDS = 1024,
    MAX_TEXT_BYTES = 2048
};

static bool valid_span(size_t offset, unsigned count, size_t stride, size_t size) {
    return count <= MAX_RECORDS && offset >= HEADER_BYTES && offset <= size &&
           (size_t)count <= (size - offset) / stride;
}

static bool valid_table(const GcLayoutTable *table) {
    return table && table->bytes &&
           valid_span(table->pane_offset, table->pane_count, PANE_BYTES,
                      table->byte_count) &&
           valid_span(table->text_offset, table->text_count, TEXT_BYTES,
                      table->byte_count) &&
           valid_span(table->frame_offset, table->frame_count, FRAME_BYTES,
                      table->byte_count);
}

static size_t records_end(const GcLayoutTable *table) {
    size_t end = table->pane_offset + (size_t)table->pane_count * PANE_BYTES;
    size_t text_end = table->text_offset + (size_t)table->text_count * TEXT_BYTES;
    size_t frame_end = table->frame_offset + (size_t)table->frame_count * FRAME_BYTES;

    if (end < text_end)
        end = text_end;
    if (end < frame_end)
        end = frame_end;
    return end;
}

static GcLayoutBox read_box(const uint8_t *record) {
    /* GLH0 positions are centers in unsigned 12.4 fixed point. Native pane
     * drawing (USA BS2 0x8130a030) subtracts half the width and height. */
    return (GcLayoutBox){
        cc_read_be16(record + 4) / 16.0f, cc_read_be16(record + 6) / 16.0f,
        cc_read_be16(record + 8) / 16.0f, cc_read_be16(record + 10) / 16.0f};
}

static void read_name(const uint8_t *record, char name[5]) {
    memcpy(name, record, 4);
    name[4] = 0;
}

bool gc_layout_pane(const GcLayoutTable *table, unsigned index, GcLayoutPane *pane) {
    const uint8_t *record;
    GcLayoutPane candidate;

    if (!pane || !valid_table(table) || index >= table->pane_count)
        return false;
    record = table->bytes + table->pane_offset + (size_t)index * PANE_BYTES;
    read_name(record, candidate.name);
    candidate.box = read_box(record);
    candidate.texture = cc_read_be16(record + 12);
    candidate.alignment = record[14];
    candidate.flags = record[15];
    *pane = candidate;
    return true;
}

bool gc_layout_text(const GcLayoutTable *table, unsigned index, GcLayoutText *text) {
    const uint8_t *record;
    size_t record_offset;
    size_t string_offset;
    size_t relative_offset;
    size_t string_length;
    GcLayoutText candidate;

    if (!text || !valid_table(table) || index >= table->text_count)
        return false;
    record_offset = table->text_offset + (size_t)index * TEXT_BYTES;
    record = table->bytes + record_offset;
    relative_offset = cc_read_be32(record + 32);
    if (relative_offset > table->byte_count - record_offset)
        return false;
    string_offset = record_offset + relative_offset;
    string_length = cc_read_be16(record + 30);
    if (string_offset < records_end(table) || string_offset >= table->byte_count ||
        string_length > MAX_TEXT_BYTES ||
        string_length >= table->byte_count - string_offset ||
        table->bytes[string_offset + string_length] != 0 ||
        memchr(table->bytes + string_offset, 0, string_length) != NULL)
        return false;
    if ((record[20] & 0x7f) > 2 || record[21] > 2)
        return false;
    candidate.frame_index = cc_read_be16(record + 28);
    if (candidate.frame_index != UINT16_MAX &&
        candidate.frame_index >= table->frame_count)
        return false;
    read_name(record, candidate.name);
    candidate.box = read_box(record);
    candidate.color_first = cc_read_be32(record + 12);
    candidate.color_second = cc_read_be32(record + 16);
    candidate.horizontal_alignment = record[20];
    candidate.vertical_alignment = record[21];
    candidate.letter_spacing = (int16_t)cc_read_be16(record + 22);
    candidate.line_spacing = cc_read_be16(record + 24);
    candidate.font_height = cc_read_be16(record + 26);
    candidate.embedded_text = (const char *)(table->bytes + string_offset);
    candidate.embedded_length = string_length;
    *text = candidate;
    return true;
}

bool gc_layout_frame(const GcLayoutTable *table, unsigned index, GcLayoutFrame *frame) {
    const uint8_t *record;
    GcLayoutFrame candidate;

    if (!frame || !valid_table(table) || index >= table->frame_count)
        return false;
    record = table->bytes + table->frame_offset + (size_t)index * FRAME_BYTES;
    read_name(record, candidate.name);
    candidate.box = read_box(record);
    for (unsigned parameter = 0; parameter < 10; ++parameter)
        candidate.parameters[parameter] = cc_read_be16(record + 12 + parameter * 2);
    for (unsigned color = 0; color < 4; ++color)
        candidate.colors[color] = cc_read_be32(record + 32 + color * 4);
    *frame = candidate;
    return true;
}

bool gc_layout_table_decode(const uint8_t *bytes, size_t byte_count,
                            GcLayoutTable *table) {
    GcLayoutTable candidate;

    if (!bytes || !table || byte_count < HEADER_BYTES || memcmp(bytes, "GLH0", 4) != 0)
        return false;
    candidate = (GcLayoutTable){bytes,
                                byte_count,
                                cc_read_be32(bytes + 4),
                                cc_read_be32(bytes + 8),
                                cc_read_be32(bytes + 12),
                                cc_read_be16(bytes + 16),
                                cc_read_be16(bytes + 18),
                                cc_read_be16(bytes + 20)};
    if (!valid_table(&candidate) ||
        candidate.text_offset <
            candidate.pane_offset + (size_t)candidate.pane_count * PANE_BYTES ||
        candidate.frame_offset <
            candidate.text_offset + (size_t)candidate.text_count * TEXT_BYTES)
        return false;
    for (unsigned index = 0; index < candidate.text_count; ++index) {
        GcLayoutText text;
        if (!gc_layout_text(&candidate, index, &text))
            return false;
    }
    *table = candidate;
    return true;
}

bool gc_layout_find_pane(const GcLayoutTable *table, const char name[4],
                         unsigned occurrence, GcLayoutPane *pane) {
    if (!name || !pane || !valid_table(table))
        return false;
    for (unsigned index = 0; index < table->pane_count; ++index) {
        const uint8_t *record =
            table->bytes + table->pane_offset + (size_t)index * PANE_BYTES;
        if (memcmp(record, name, 4) == 0 && occurrence-- == 0)
            return gc_layout_pane(table, index, pane);
    }
    return false;
}

bool gc_layout_find_text(const GcLayoutTable *table, const char name[4],
                         unsigned occurrence, GcLayoutText *text) {
    if (!name || !text || !valid_table(table))
        return false;
    for (unsigned index = 0; index < table->text_count; ++index) {
        const uint8_t *record =
            table->bytes + table->text_offset + (size_t)index * TEXT_BYTES;
        if (memcmp(record, name, 4) == 0 && occurrence-- == 0)
            return gc_layout_text(table, index, text);
    }
    return false;
}

bool gc_layout_find_frame(const GcLayoutTable *table, const char name[4],
                          unsigned occurrence, GcLayoutFrame *frame) {
    if (!name || !frame || !valid_table(table))
        return false;
    for (unsigned index = 0; index < table->frame_count; ++index) {
        const uint8_t *record =
            table->bytes + table->frame_offset + (size_t)index * FRAME_BYTES;
        if (memcmp(record, name, 4) == 0 && occurrence-- == 0)
            return gc_layout_frame(table, index, frame);
    }
    return false;
}

bool gc_layout_pane_quad(const GcLayoutPane *pane, GcLayoutVertex vertices[4]) {
    static const unsigned column[] = {0, 1, 1, 0};
    static const unsigned row[] = {0, 0, 1, 1};
    float u[2];
    float v[2];
    float left;
    float top;

    if (!pane || !vertices || (pane->alignment & 15) != 15 ||
        (pane->flags & ~7u) != 0 || !isfinite(pane->box.center_x) ||
        !isfinite(pane->box.center_y) || !isfinite(pane->box.width) ||
        !isfinite(pane->box.height) || pane->box.width < 0 || pane->box.height < 0)
        return false;
    left = pane->box.center_x - pane->box.width * 0.5f;
    top = pane->box.center_y - pane->box.height * 0.5f;
    u[0] = (pane->flags & GC_LAYOUT_FLIP_U) ? 1.0f : 0.0f;
    u[1] = 1.0f - u[0];
    v[0] = (pane->flags & GC_LAYOUT_FLIP_V) ? 1.0f : 0.0f;
    v[1] = 1.0f - v[0];
    /* Native vertex emission at 0x8130a1f8 rotates after axis flips. */
    for (unsigned index = 0; index < 4; ++index) {
        vertices[index].x = left + column[index] * pane->box.width;
        vertices[index].y = top + row[index] * pane->box.height;
        vertices[index].u =
            u[(pane->flags & GC_LAYOUT_ROTATE_UV) ? row[index] : column[index]];
        vertices[index].v =
            v[(pane->flags & GC_LAYOUT_ROTATE_UV) ? 1 - column[index] : row[index]];
    }
    return true;
}

bool gc_layout_text_origin(const GcLayoutText *text, float measured_width,
                           float measured_height, float *x, float *y) {
    unsigned horizontal;
    float result_x;
    float result_y;

    if (!text || !x || !y || !isfinite(measured_width) || !isfinite(measured_height) ||
        measured_width < 0 || measured_height < 0 || !isfinite(text->box.center_x) ||
        !isfinite(text->box.center_y) || !isfinite(text->box.width) ||
        !isfinite(text->box.height) || text->box.width < 0 || text->box.height < 0)
        return false;
    horizontal = text->horizontal_alignment & 0x7f;
    if (horizontal > 2 || text->vertical_alignment > 2)
        return false;
    /* Native text placement: USA BS2 0x8130862c. */
    result_x = text->box.center_x - measured_width * 0.5f;
    if (horizontal == 1)
        result_x = text->box.center_x + text->box.width * 0.5f - measured_width;
    if (horizontal == 2)
        result_x = text->box.center_x - text->box.width * 0.5f;
    result_y = text->box.center_y - measured_height * 0.5f;
    if (text->vertical_alignment == 1)
        result_y = text->box.center_y + text->box.height * 0.5f - measured_height;
    if (text->vertical_alignment == 2)
        result_y = text->box.center_y - text->box.height * 0.5f;
    *x = result_x;
    *y = result_y;
    return true;
}

bool gc_layout_frame_quad(const GcLayoutFrame *frame, const GcIplImage *textures,
                          size_t texture_count, unsigned index, uint32_t tint,
                          GcLayoutFrameQuad *quad) {
    static const unsigned color_order[4] = {0, 1, 3, 2};
    static const unsigned column[4] = {0, 1, 1, 0};
    static const unsigned row[4] = {0, 0, 1, 1};
    GcLayoutFrameQuad candidate = {0};
    float width;
    float height;
    float left;
    float top;
    unsigned flags = 0;
    bool horizontal_edge = false;
    bool vertical_edge = false;

    if (!frame || !quad || index > 8 || !isfinite(frame->box.center_x) ||
        !isfinite(frame->box.center_y))
        return false;
    /* Native frame draw: USA BS2 0x81309b24. The outer box is descriptive;
     * the body uses +0x10/+0x12 and corner BTI dimensions define its borders. */
    width = frame->parameters[2] / 16.0f;
    height = frame->parameters[3] / 16.0f;
    left = frame->box.center_x - width * 0.5f;
    top = frame->box.center_y - height * 0.5f;
    if (index) {
        static const unsigned corner_for_quad[8] = {0, 1, 2, 3, 1, 3, 2, 3};
        unsigned corner = corner_for_quad[index - 1];
        unsigned first_texture = frame->parameters[4];
        float corner_width;
        float corner_height;
        float right = left + width;
        float bottom = top + height;

        candidate.texture = frame->parameters[4 + corner];
        if (!textures || first_texture >= texture_count ||
            candidate.texture >= texture_count || !textures[first_texture].rgba ||
            !textures[candidate.texture].rgba || !textures[first_texture].width ||
            !textures[first_texture].height)
            return false;
        corner_width = (float)textures[first_texture].width;
        corner_height = (float)textures[first_texture].height;
        flags = (frame->parameters[8] >> (14 - corner * 2)) & 3;
        candidate.textured = true;
        switch (index) {
            case 1:
                left -= corner_width;
                top -= corner_height;
                width = corner_width;
                height = corner_height;
                break;
            case 2:
                left = right;
                top -= corner_height;
                width = corner_width;
                height = corner_height;
                break;
            case 3:
                left -= corner_width;
                top = bottom;
                width = corner_width;
                height = corner_height;
                break;
            case 4:
                left = right;
                top = bottom;
                width = corner_width;
                height = corner_height;
                break;
            case 5:
                top -= corner_height;
                height = corner_height;
                horizontal_edge = true;
                break;
            case 6:
                top = bottom;
                height = corner_height;
                horizontal_edge = true;
                break;
            case 7:
                left -= corner_width;
                width = corner_width;
                vertical_edge = true;
                break;
            case 8:
                left = right;
                width = corner_width;
                vertical_edge = true;
                break;
        }
    }
    for (unsigned vertex = 0; vertex < 4; ++vertex) {
        unsigned u = horizontal_edge ? 0 : column[vertex];
        unsigned v = vertical_edge ? 0 : row[vertex];
        candidate.vertices[vertex] =
            (GcLayoutVertex){left + column[vertex] * width, top + row[vertex] * height,
                             (float)((flags & GC_LAYOUT_FLIP_U) ? 1 - u : u),
                             (float)((flags & GC_LAYOUT_FLIP_V) ? 1 - v : v)};
        if (index) {
            candidate.colors[vertex] = tint;
        } else {
            uint32_t color = frame->colors[color_order[vertex]];
            uint32_t alpha = (color & 255) * (tint & 255) / 255;
            candidate.colors[vertex] = (color & UINT32_C(0xffffff00)) | alpha;
        }
    }
    *quad = candidate;
    return true;
}

static bool popup_frame(GcLayoutFrame *frame, float left, float top, float width,
                        float height) {
    if (width * 16 > UINT16_MAX || height * 16 > UINT16_MAX)
        return false;
    frame->box.center_x = left + width * 0.5f;
    frame->box.center_y = top + height * 0.5f;
    frame->parameters[2] = (uint16_t)(width * 16);
    frame->parameters[3] = (uint16_t)(height * 16);
    return true;
}

bool gc_layout_card_popup(const GcLayoutTable *table, unsigned slot,
                          unsigned visible_index, bool confirmation,
                          gc_card_action action, float measured_width,
                          float measured_line_height, GcLayoutCardPopup *popup) {
    GcLayoutCardPopup candidate = {0};
    GcLayoutPane tile;
    GcLayoutFrame frame;
    GcLayoutText text;
    char name[4] = {'i', slot ? 'b' : 'a', 0, 0};
    float left;
    float top;
    float expanded;
    float action_height;
    float width;

    if (!popup || slot > 1 || visible_index >= 16 || action < GC_CARD_ACTION_MOVE ||
        action > GC_CARD_ACTION_ERASE || !isfinite(measured_width) ||
        !isfinite(measured_line_height) || measured_width < 0 ||
        measured_width > 2048 || measured_line_height <= 0 ||
        measured_line_height > 2048)
        return false;
    name[2] = (char)('0' + visible_index / 10);
    name[3] = (char)('0' + visible_index % 10);
    if (!gc_layout_find_pane(table, name, 0, &tile) ||
        !gc_layout_find_frame(table, "mes0", 0, &frame) ||
        !gc_layout_find_text(table, "txt0", 0, &text) || !text.line_spacing)
        return false;
    /* USA 0x81314c40 copies iaNN/ibNN centers; 0x8131a060 positions
     * the dynamic body with the native eight-pixel padding. */
    measured_width = floorf(measured_width * 16) / 16;
    measured_line_height = floorf(measured_line_height * 16) / 16;
    width = measured_width + 16;
    expanded = measured_line_height + 2 * text.line_spacing;
    left = tile.box.center_x + (visible_index % 4 < 2 ? 40 : -measured_width - 56);
    /* The bottom-row adjustment adds 8*8 raw 12.4 units, four pixels. */
    top = tile.box.center_y - (visible_index / 4 < 2 ? 0 : expanded + 4);
    action_height = (confirmation ? measured_line_height : expanded) + 16;
    candidate.frames[0] = frame;
    if (!popup_frame(&candidate.frames[0], left, top, width, action_height))
        return false;
    candidate.frame_count = confirmation ? 2 : 1;
    candidate.row_count = 3;
    for (unsigned index = 0; index < 3; ++index) {
        candidate.rows[index] = text;
        candidate.rows[index].box =
            (GcLayoutBox){left + 8 + measured_width * 0.5f,
                          top + 8 + measured_line_height * 0.5f +
                              (confirmation ? 0 : index * text.line_spacing),
                          measured_width, measured_line_height};
        candidate.rows[index].frame_index = UINT16_MAX;
        candidate.text_entries[index] = 2 + index;
    }
    if (confirmation) {
        candidate.rows[0] = candidate.rows[(unsigned)action];
        candidate.text_entries[0] = 2 + (unsigned)action;
        candidate.frames[1] = frame;
        float confirmation_top = top + action_height + 8;
        if (!popup_frame(&candidate.frames[1], left, confirmation_top, width,
                         measured_line_height + text.line_spacing + 16))
            return false;
        for (unsigned index = 0; index < 2; ++index) {
            candidate.rows[1 + index] = text;
            candidate.rows[1 + index].box =
                (GcLayoutBox){left + 8 + measured_width * 0.5f,
                              confirmation_top + 8 + measured_line_height * 0.5f +
                                  index * text.line_spacing,
                              measured_width, measured_line_height};
            candidate.rows[1 + index].frame_index = UINT16_MAX;
            candidate.text_entries[1 + index] = 5 + index;
        }
    }
    *popup = candidate;
    return true;
}

bool gc_layout_card_popup_colors(const GcText *text, uint32_t colors[2]) {
    uint16_t channels[6];
    uint32_t result[2];
    if (!text || !colors ||
        !gc_native_halfwords(text, text->europe ? 0x16020 : 0x15324,
                             text->europe ? 0x16154 : 0x15458, 3, 0x50, 6, channels))
        return false;
    for (unsigned index = 0; index < 6; ++index)
        if (channels[index] > 255)
            return false;
    for (unsigned index = 0; index < 2; ++index)
        result[index] = (uint32_t)channels[index * 3] << 24 |
                        (uint32_t)channels[index * 3 + 1] << 16 |
                        (uint32_t)channels[index * 3 + 2] << 8 | 255;
    memcpy(colors, result, sizeof(result));
    return true;
}

bool gc_layout_glow_decode(const GcText *text, GcLayoutGlow *glow) {
    uint16_t fields[5];
    if (!text || !glow ||
        !gc_native_halfwords(text, text->europe ? 0x1770 : 0x18d0,
                             text->europe ? 0x1814 : 0x1974, 3, 0x32, 5, fields) ||
        fields[0] > 255 || fields[1] > fields[0] || fields[2] != 0 || !fields[3] ||
        fields[3] > 1000 || fields[4] != 1)
        return false;
    *glow = (GcLayoutGlow){fields[1], fields[0], fields[3]};
    return true;
}

bool gc_layout_glow_pass(const GcLayoutGlow *glow, uint32_t foreground, uint32_t halo,
                         uint64_t ticks, uint8_t alpha, unsigned index,
                         GcLayoutTextPass *pass) {
    GcLayoutTextPass result = {0};
    if (!glow || !pass || index > 25 || !glow->half_cycle || glow->half_cycle > 1000 ||
        glow->maximum > 255 || glow->minimum > glow->maximum)
        return false;
    if (index == 25) {
        result.color = (foreground & 0xffffff00) | ((foreground & 255) * alpha / 255);
    } else {
        unsigned phase = (unsigned)(ticks % (2u * (glow->half_cycle + 1u)));
        unsigned counter =
            phase <= glow->half_cycle ? phase : 2u * glow->half_cycle + 1u - phase;
        unsigned pulse = glow->minimum +
                         counter * (glow->maximum - glow->minimum) / glow->half_cycle;
        unsigned opacity = (halo & 255) * alpha / 255;
        result.offset_x = (float)(index / 5) * 2 - 4;
        result.offset_y = (float)(index % 5) * 2 - 4;
        result.color = (halo & 0xffffff00) | (opacity * pulse / 255 / 6);
    }
    *pass = result;
    return true;
}

static bool measured_popup_size(float value, bool height) {
    return isfinite(value) && value >= (height ? 1 : 0) && value <= 2048;
}

bool gc_layout_confirmation_dialog(const GcLayoutTable *table,
                                   const GcLayoutText *source, float body_width,
                                   float body_height, float choices_width,
                                   float choice_line_height, GcLayoutDialog *dialog) {
    GcLayoutDialog candidate = {0};
    GcLayoutText text;
    GcLayoutFrame frame;
    if (!dialog || !measured_popup_size(body_width, false) ||
        !measured_popup_size(body_height, true) ||
        !measured_popup_size(choices_width, false) ||
        !measured_popup_size(choice_line_height, true) || !source ||
        !source->line_spacing)
        return false;
    text = *source;
    if (!gc_layout_frame(table, text.frame_index, &frame))
        return false;
    body_width = floorf(body_width * 16) / 16;
    body_height = floorf(body_height * 16) / 16;
    choices_width = floorf(choices_width * 16) / 16;
    choice_line_height = floorf(choice_line_height * 16) / 16;
    float body_frame_height = body_height + 16;
    float choice_frame_height = choice_line_height + text.line_spacing + 16;
    float total_height = body_frame_height + choice_frame_height + 8;
    float body_top = text.box.center_y - total_height * 0.5f;
    float choices_top = text.box.center_y + total_height * 0.5f - choice_frame_height;
    candidate.frames[0] = candidate.frames[1] = frame;
    if (!popup_frame(&candidate.frames[0], text.box.center_x - body_width * 0.5f - 8,
                     body_top, body_width + 16, body_frame_height) ||
        !popup_frame(&candidate.frames[1], text.box.center_x - choices_width * 0.5f - 8,
                     choices_top, choices_width + 16, choice_frame_height))
        return false;
    candidate.frame_count = 2;
    candidate.row_count = 3;
    candidate.rows[0] = text;
    candidate.rows[0].box =
        (GcLayoutBox){text.box.center_x, body_top + body_frame_height * 0.5f,
                      body_width, body_height};
    candidate.rows[0].frame_index = UINT16_MAX;
    candidate.text_entries[0] = 0;
    /* USA 0x8130d668: the combined body/choices stack is centered on txt0;
     * eight-pixel padding surrounds each body with an eight-pixel gap. */
    for (unsigned index = 0; index < 2; ++index) {
        candidate.rows[index + 1] = text;
        candidate.rows[index + 1].box = (GcLayoutBox){
            text.box.center_x,
            choices_top + 8 + choice_line_height * 0.5f + index * text.line_spacing,
            choices_width, choice_line_height};
        candidate.rows[index + 1].frame_index = UINT16_MAX;
        candidate.text_entries[index + 1] = index + 1;
    }
    *dialog = candidate;
    return true;
}

bool gc_layout_boot_dialog(const GcLayoutTable *table, float body_width,
                           float body_height, float choices_width,
                           float choice_line_height, GcLayoutDialog *dialog) {
    GcLayoutText text;
    return gc_layout_find_text(table, "txt0", 0, &text) &&
           gc_layout_confirmation_dialog(table, &text, body_width, body_height,
                                         choices_width, choice_line_height, dialog);
}

bool gc_layout_language_dialog(const GcLayoutTable *table, float measured_width,
                               float measured_line_height, GcLayoutDialog *dialog) {
    GcLayoutDialog candidate = {0};
    GcLayoutText text;
    GcLayoutFrame frame;
    if (!dialog || !measured_popup_size(measured_width, false) ||
        !measured_popup_size(measured_line_height, true) ||
        !gc_layout_find_text(table, "lan1", 0, &text) || !text.line_spacing ||
        !gc_layout_frame(table, text.frame_index, &frame))
        return false;
    measured_width = floorf(measured_width * 16) / 16;
    measured_line_height = floorf(measured_line_height * 16) / 16;
    float height = measured_line_height + 5 * text.line_spacing + 16;
    float top = frame.box.center_y - height * 0.5f;
    float left = frame.box.center_x - measured_width * 0.5f - 8;
    candidate.frames[0] = frame;
    if (!popup_frame(&candidate.frames[0], left, top, measured_width + 16, height))
        return false;
    candidate.frame_count = 1;
    candidate.row_count = 6;
    /* EUR 0x8130db58 centers the six rows on lan1's owning mes1 frame,
     * rather than using the first label's original center. */
    for (unsigned index = 0; index < 6; ++index) {
        candidate.rows[index] = text;
        candidate.rows[index].box = (GcLayoutBox){
            frame.box.center_x,
            top + 8 + measured_line_height * 0.5f + index * text.line_spacing,
            measured_width, measured_line_height};
        candidate.rows[index].frame_index = UINT16_MAX;
        candidate.text_entries[index] = index + 12;
    }
    *dialog = candidate;
    return true;
}

/* Recovered GLH0 metadata only; layout assets remain in the local IPL. */
static const uint32_t ntsc_offsets[2][GC_LAYOUT_GROUP_COUNT] = {
    {0x77600, 0x5f3a0, 0x61ec0, 0x5fa60, 0x60040, 0x76c60, 0x5f580, 0x770c0, 0x77440,
     0x776c0, 0x77800},
    {0x62760, 0x5f3a0, 0x61ec0, 0x5fa60, 0x60040, 0x61ac0, 0x5f580, 0x621e0, 0x625a0,
     0x62820, 0x62960}};

static const uint32_t europe_offsets[6][GC_LAYOUT_GROUP_COUNT] = {
    {0xadc40, 0x92d40, 0xd24a0, 0x930a0, 0x93bc0, 0xd1840, 0xd0140, 0xd32c0, 0xada80,
     0xadd00, 0xd3640},
    {0xd4700, 0x92d40, 0xd24a0, 0x930a0, 0x93bc0, 0xd1840, 0xd0140, 0xd41c0, 0xd4540,
     0xd47e0, 0xd4920},
    {0xd3da0, 0x92d40, 0xd24a0, 0x930a0, 0x93bc0, 0xd1840, 0xd0140, 0xd3860, 0xd3be0,
     0xd3e60, 0xd3fa0},
    {0xd5a00, 0x92d40, 0xd24a0, 0x930a0, 0x93bc0, 0xd1840, 0xd0140, 0xd54a0, 0xd5840,
     0xd5ae0, 0xd5c20},
    {0xd5080, 0x92d40, 0xd24a0, 0x930a0, 0x93bc0, 0xd1840, 0xd0140, 0xd4b40, 0xd4ec0,
     0xd5140, 0xd5280},
    {0xd2ea0, 0x92d40, 0xd24a0, 0x930a0, 0x93bc0, 0xd1840, 0xd0140, 0xd2960, 0xd2ce0,
     0xd2f60, 0xd30a0}};

bool gc_layouts_index(const GcText *text, GcLayouts *layouts) {
    GcLayouts candidate = {0};

    if (!text || !text->rom || !layouts)
        return false;
    candidate.europe = text->europe;
    for (unsigned language = 0; language < 7; ++language) {
        const uint32_t *offsets;
        if (text->europe) {
            if (language == GC_LANGUAGE_JAPANESE)
                continue;
            offsets = europe_offsets[language];
        } else {
            if (language != GC_LANGUAGE_ENGLISH && language != GC_LANGUAGE_JAPANESE)
                continue;
            offsets = ntsc_offsets[language == GC_LANGUAGE_JAPANESE ? 1 : 0];
        }
        for (unsigned group = 0; group < GC_LAYOUT_GROUP_COUNT; ++group) {
            size_t offset = offsets[group];
            if (offset >= text->rom_size ||
                !gc_layout_table_decode(text->rom + offset, text->rom_size - offset,
                                        &candidate.tables[language][group]))
                return false;
        }
    }
    *layouts = candidate;
    return true;
}

const GcLayoutTable *gc_layout_table(const GcLayouts *layouts, gc_language language,
                                     GcLayoutGroup group) {
    const GcLayoutTable *table;

    if (!layouts || language < GC_LANGUAGE_ENGLISH || language > GC_LANGUAGE_JAPANESE ||
        group < GC_LAYOUT_MENU || group >= GC_LAYOUT_GROUP_COUNT)
        return NULL;
    table = &layouts->tables[language][group];
    return table->bytes ? table : NULL;
}
