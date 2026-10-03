#include "render_internal.h"

#include <math.h>
#include <string.h>

const char *gc_render_native_text(const GcScene *scene, GcTextGroup group,
                                  unsigned index, const char *fallback) {
    const char *value = gc_text_get(&scene->native_text, scene->language, group, index);
    return value ? value : fallback;
}

CcColor gc_render_color_rgba(uint32_t rgba) {
    return (CcColor){(float)(rgba >> 24) / 255, (float)((rgba >> 16) & 255) / 255,
                     (float)((rgba >> 8) & 255) / 255, (float)(rgba & 255) / 255};
}

static int layout_face(GcLayoutGroup group) {
    switch (group) {
        case GC_LAYOUT_MENU:
            return GC_MENU_ANIMATION_HOME_PANE;
        case GC_LAYOUT_CALENDAR_FACE:
            return GC_FACE_CALENDAR;
        case GC_LAYOUT_DISC_FACE:
            return GC_FACE_GAME_PLAY;
        case GC_LAYOUT_CARD_FACE:
            return GC_FACE_MEMORY_CARD;
        case GC_LAYOUT_OPTIONS_FACE:
            return GC_FACE_OPTIONS;
        default:
            return -1;
    }
}

static void layout_vertices_sampled(GcScene *scene, GcLayoutGroup group,
                                    CcDrawVertex vertices[4], uint32_t texture,
                                    bool squared_alpha, bool nearest) {
    int face = layout_face(group);
    float alpha = face < 0
                      ? 1
                      : (float)(squared_alpha ? scene->menu_pose.frame_alpha[face]
                                              : scene->menu_pose.pane_alpha[face]) /
                            255;
    if (group == GC_LAYOUT_CARD || group == GC_LAYOUT_DISC ||
        group == GC_LAYOUT_CALENDAR || group == GC_LAYOUT_OPTIONS ||
        group == GC_LAYOUT_ERROR)
        alpha *= scene->text_alpha;
    if (alpha <= 0)
        return;
    float distance = 224.0f / tanf(3.14159265358979323846f / 18);
    CcMaterialQuad material = gc_render_raster_material(texture);
    material.nearest[0] = nearest;
    /* HELP's 0x81309818 submitter selects raster RGB with texture alpha.
     * Sampling the I4 RGB too darkens the native controller glow twice. */
    if (group == GC_LAYOUT_HELP)
        material.tev_stages[0][4] = 0xcf;
    for (unsigned index = 0; index < 4; index++) {
        CcDrawVertex *vertex = &vertices[index];
        if (face < 0) {
            vertex->x = gc_render_projection_x(scene->perspective, vertex->x - 292);
            vertex->y = gc_render_projection_center_y(scene->startup.frame_rate == 50) +
                        (vertex->y - 224) * scene->pixel_scale_y;
        } else {
            float local[3] = {vertex->x, vertex->y, 0}, point[3];
            gc_startup_transform(scene->menu_pose.pane_matrices[face], local, point);
            float factor = distance / (distance - point[2]);
            vertex->x = gc_render_projection_x(true, point[0] * factor);
            vertex->y = gc_render_projection_center_y(scene->startup.frame_rate == 50) -
                        (point[1] - scene->camera_y) * factor * scene->pixel_scale_y;
            material.vertices[index].depth =
                10000.0f / 9950 - 500000.0f / (9950 * (distance - point[2]));
            material.vertices[index].clip_w = (distance - point[2]) / distance;
        }
        vertex->color.a *= alpha;
        vertex->x += scene->display_offset_x;
        material.vertices[index].x = vertex->x;
        material.vertices[index].y = vertex->y;
        material.vertices[index].color = vertex->color;
        material.vertices[index].uv[0][0] = vertex->u;
        material.vertices[index].uv[0][1] = vertex->v;
    }
    if (face < 0 && group != GC_LAYOUT_HELP && !nearest)
        cc_platform_draw_vertices(scene->platform, vertices, texture);
    else
        cc_platform_draw_material_quad(scene->platform, &material);
}

void gc_render_layout_vertices(GcScene *scene, GcLayoutGroup group,
                               CcDrawVertex vertices[4], uint32_t texture,
                               bool squared_alpha) {
    layout_vertices_sampled(scene, group, vertices, texture, squared_alpha, false);
}

float gc_render_layout_line_width(const GcScene *scene, const GcLayoutText *layout,
                                  const char *bytes, size_t length) {
    float width = 0;
    size_t offset = 0;
    uint32_t code;
    bool any = false;
    while (gc_font_next(bytes, length, &offset, scene->encoding, &code)) {
        GcFontGlyph glyph;
        if (!gc_font_glyph(&scene->font, scene->encoding, code, &glyph))
            continue;
        if (any)
            width += layout->letter_spacing;
        width += (float)glyph.cell.advance * (float)layout->font_height /
                 (float)glyph.cell.height;
        any = true;
    }
    return width;
}

static void layout_glyph_line(GcScene *scene, GcLayoutGroup group,
                              const GcLayoutText *layout, const char *bytes,
                              size_t length, float x, float y) {
    CcColor top = gc_render_color_rgba(layout->color_first);
    CcColor bottom = gc_render_color_rgba(layout->color_second);
    size_t offset = 0;
    uint32_t code;
    while (gc_font_next(bytes, length, &offset, scene->encoding, &code)) {
        GcFontGlyph glyph;
        if (!gc_font_glyph(&scene->font, scene->encoding, code, &glyph))
            continue;
        float scale = (float)layout->font_height / (float)glyph.cell.height;
        float left = x, right = left + (float)glyph.cell.width * scale;
        float upper = y, lower = y + (float)layout->font_height;
        float u0 = (float)glyph.cell.x / (float)glyph.atlas_width;
        float v0 = (float)glyph.cell.y / (float)glyph.atlas_height;
        float u1 = (float)(glyph.cell.x + glyph.cell.width) / (float)glyph.atlas_width;
        float v1 =
            (float)(glyph.cell.y + glyph.cell.height) / (float)glyph.atlas_height;
        CcDrawVertex vertices[4] = {{left, upper, u0, v0, top},
                                    {right, upper, u1, v0, top},
                                    {left, lower, u0, v1, bottom},
                                    {right, lower, u1, v1, bottom}};
        uint32_t texture = glyph.encoding == GC_TEXT_SHIFT_JIS ? scene->sjis_texture
                                                               : scene->font_texture;
        /* USA 1258c banner labels use the linear face alpha. */
        gc_render_layout_vertices(scene, group, vertices, texture,
                                  group != GC_LAYOUT_DISC_FACE);
        x += (float)glyph.cell.advance * scale + layout->letter_spacing;
    }
}

void gc_render_layout_text_value(GcScene *scene, GcLayoutGroup group,
                                 const GcLayoutText *source, const char *value) {
    if (!value || !source)
        return;
    GcLayoutText layout = *source;
    unsigned lines = 1;
    for (const char *cursor = value; *cursor; cursor++)
        if (*cursor == '\n')
            lines++;
    float height = (float)layout.font_height + (float)(lines - 1) * layout.line_spacing;
    float first_y = 0, ignored_x = 0;
    if (!gc_layout_text_origin(&layout, 0, height, &ignored_x, &first_y))
        return;
    while (*value) {
        const char *end = strchr(value, '\n');
        size_t length = end ? (size_t)(end - value) : strlen(value);
        float width = gc_render_layout_line_width(scene, &layout, value, length);
        float x = 0, ignored_y = 0;
        if (!gc_layout_text_origin(&layout, width, height, &x, &ignored_y))
            return;
        layout_glyph_line(scene, group, &layout, value, length, x, first_y);
        if (!end)
            break;
        value = end + 1;
        first_y += layout.line_spacing;
    }
}

void gc_render_layout_text_measure(const GcScene *scene, const GcLayoutText *layout,
                                   const char *value, float *width, float *height) {
    unsigned lines = 1;
    *width = 0;
    for (;;) {
        const char *end = strchr(value, '\n');
        size_t length = end ? (size_t)(end - value) : strlen(value);
        *width =
            fmaxf(*width, gc_render_layout_line_width(scene, layout, value, length));
        if (!end)
            break;
        lines++;
        value = end + 1;
    }
    *height = (float)layout->font_height + (float)(lines - 1) * layout->line_spacing;
}

uint32_t gc_render_layout_color_alpha(uint32_t color, uint8_t alpha) {
    return (color & UINT32_C(0xffffff00)) | ((color & 255) * alpha / 255);
}

void gc_render_layout_text_alpha(GcLayoutText *layout, uint8_t alpha) {
    layout->color_first = gc_render_layout_color_alpha(layout->color_first, alpha);
    layout->color_second = gc_render_layout_color_alpha(layout->color_second, alpha);
}

void gc_render_layout_highlight_value(GcScene *scene, GcLayoutGroup group,
                                      const GcLayoutText *layout, const char *value,
                                      uint32_t foreground, uint32_t halo,
                                      uint8_t alpha) {
    for (unsigned index = 0; index < 26; index++) {
        GcLayoutTextPass pass;
        if (!gc_layout_glow_pass(&scene->text_glow, foreground, halo, scene->ui_ticks,
                                 alpha, index, &pass) ||
            !(pass.color & 255))
            continue;
        GcLayoutText copy = *layout;
        copy.box.center_x += pass.offset_x;
        copy.box.center_y += pass.offset_y;
        copy.color_first = copy.color_second = pass.color;
        gc_render_layout_text_value(scene, group, &copy, value);
    }
}

void gc_render_layout_string(GcScene *scene, GcLayoutGroup group, const char name[4],
                             const char *value) {
    GcLayoutText layout;
    const GcLayoutTable *table =
        gc_layout_table(&scene->layouts, scene->language, group);
    if (gc_layout_find_text(table, name, 0, &layout))
        gc_render_layout_text_value(scene, group, &layout, value);
}

void gc_render_layout_string_alpha(GcScene *scene, GcLayoutGroup group,
                                   const char name[4], const char *value,
                                   uint8_t alpha) {
    GcLayoutText layout;
    const GcLayoutTable *table =
        gc_layout_table(&scene->layouts, scene->language, group);
    if (gc_layout_find_text(table, name, 0, &layout)) {
        gc_render_layout_text_alpha(&layout, alpha);
        gc_render_layout_text_value(scene, group, &layout, value);
    }
}

void gc_render_layout_image(GcScene *scene, GcLayoutGroup group,
                            const GcLayoutPane *pane, uint32_t texture, CcColor tint) {
    GcLayoutVertex native[4];
    if (!texture || !gc_layout_pane_quad(pane, native))
        return;
    static const unsigned order[4] = {0, 1, 3, 2};
    CcDrawVertex vertices[4];
    for (unsigned index = 0; index < 4; index++) {
        const GcLayoutVertex *point = &native[order[index]];
        vertices[index] = (CcDrawVertex){point->x, point->y, point->u, point->v, tint};
    }
    /* USA 1272c squares only the disc foreground before applying its
     * status fader. The offset shadows and banner retain linear alpha. */
    bool squared_alpha =
        group == GC_LAYOUT_DISC_FACE &&
        (!memcmp(pane->name, "titl", 4) || !memcmp(pane->name, "nodi", 4) ||
         !memcmp(pane->name, "qust", 4));
    gc_render_layout_vertices(scene, group, vertices, texture, squared_alpha);
}

void gc_render_layout_pane(GcScene *scene, const gc_menu *menu, GcLayoutGroup group,
                           const char name[4], CcColor tint) {
    const GcLayoutTable *table =
        gc_layout_table(&scene->layouts, scene->language, group);
    GcLayoutPane pane;
    if (!scene->ui || !gc_layout_find_pane(table, name, 0, &pane))
        return;
    const GcIplImage *image =
        gc_menu_textures_pane(&scene->ui->native, menu, group, &pane);
    gc_render_layout_image(scene, group, &pane,
                           gc_render_ui_image_texture(scene->ui, image), tint);
}

void gc_render_layout_frame_value(GcScene *scene, GcLayoutGroup group,
                                  const GcLayoutFrame *frame, uint32_t tint) {
    if (!scene->ui || !frame)
        return;
    for (unsigned index = 0; index < 9; index++) {
        GcLayoutFrameQuad native;
        if (!gc_layout_frame_quad(frame, scene->ui->native.collection.images,
                                  scene->ui->native.collection.count, index, tint,
                                  &native))
            continue;
        static const unsigned order[4] = {0, 1, 3, 2};
        CcDrawVertex vertices[4];
        for (unsigned corner = 0; corner < 4; corner++) {
            unsigned source = order[corner];
            vertices[corner] =
                (CcDrawVertex){native.vertices[source].x, native.vertices[source].y,
                               native.vertices[source].u, native.vertices[source].v,
                               gc_render_color_rgba(native.colors[source])};
        }
        uint32_t texture =
            native.textured && native.texture < scene->ui->native.collection.count
                ? scene->ui->collection[native.texture]
                : 0;
        const GcTextureInfo *information =
            texture && scene->ui->native.collection.information
                ? &scene->ui->native.collection.information[native.texture]
                : NULL;
        bool nearest =
            information && information->min_filter == 0 && information->mag_filter == 0;
        layout_vertices_sampled(scene, group, vertices, texture, true, nearest);
    }
}

void gc_render_layout_frame(GcScene *scene, GcLayoutGroup group, const char name[4],
                            unsigned occurrence, uint32_t tint) {
    GcLayoutFrame frame;
    const GcLayoutTable *table =
        gc_layout_table(&scene->layouts, scene->language, group);
    if (gc_layout_find_frame(table, name, occurrence, &frame))
        gc_render_layout_frame_value(scene, group, &frame, tint);
}

void gc_render_layout_native_entry(GcScene *scene, GcTextGroup text_group,
                                   GcLayoutGroup layout_group, unsigned index,
                                   uint8_t alpha) {
    GcTextEntry entry;
    GcLayoutText layout;
    const GcTextTable *texts =
        gc_text_table(&scene->native_text, scene->language, text_group);
    const GcLayoutTable *layouts =
        gc_layout_table(&scene->layouts, scene->language, layout_group);
    if (!alpha || !gc_text_table_entry(texts, index, &entry) ||
        !gc_layout_text(layouts, entry.flags, &layout))
        return;
    GcLayoutFrame frame;
    if (gc_layout_frame(layouts, layout.frame_index, &frame)) {
        float width, height;
        gc_render_layout_text_measure(scene, &layout, entry.bytes, &width, &height);
        if (width <= 4000 && height <= 4000) {
            frame.parameters[2] = (uint16_t)((width + 16) * 16);
            frame.parameters[3] = (uint16_t)((height + 16) * 16);
            gc_render_layout_frame_value(scene, layout_group, &frame,
                                         UINT32_C(0xffffff00) | alpha);
        }
    }
    gc_render_layout_text_alpha(&layout, alpha);
    gc_render_layout_text_value(scene, layout_group, &layout, entry.bytes);
}

void gc_render_card_number(GcScene *scene, unsigned number, const char name[4]) {
    const GcLayoutTable *table =
        gc_layout_table(&scene->layouts, scene->language, GC_LAYOUT_CARD);
    GcLayoutPane anchor;
    if (!scene->ui || !gc_layout_find_pane(table, name, 0, &anchor))
        return;
    for (unsigned digit = 0; digit < 4; digit++) {
        GcLayoutVertex native[4];
        if (!gc_menu_card_number_quad(&scene->ui->native, number, anchor.box.center_x,
                                      anchor.box.center_y, digit, native))
            break;
        CcDrawVertex vertices[4];
        static const unsigned order[4] = {0, 1, 3, 2};
        for (unsigned corner = 0; corner < 4; corner++) {
            const GcLayoutVertex *point = &native[order[corner]];
            vertices[corner] = (CcDrawVertex){
                gc_render_projection_x(scene->perspective, point->x - 292) +
                    scene->display_offset_x,
                gc_render_projection_center_y(scene->startup.frame_rate == 50) +
                    (point->y - 224) * scene->pixel_scale_y,
                point->u,
                point->v,
                {1, 1, 1, scene->text_alpha}};
        }
        cc_platform_draw_vertices(scene->platform, vertices, scene->ui->card_numbers);
    }
}

void gc_render_prompts(GcScene *scene, const gc_menu *menu) {
    if (!scene->ui || scene->help_drawn)
        return;
    scene->help_drawn = true;
    const GcLayoutTable *layouts =
        gc_layout_table(&scene->layouts, scene->language, GC_LAYOUT_HELP);
    for (unsigned index = 0; index < GC_HELP_PANE_DRAWS; index++) {
        GcHelpPane pane;
        if (!gc_help_pane(&scene->help_style, &scene->help_state, layouts, index, 255,
                          &pane) ||
            !(pane.tint & 255))
            continue;
        const GcIplImage *image =
            gc_menu_textures_pane(&scene->ui->native, menu, GC_LAYOUT_HELP, &pane.pane);
        gc_render_layout_image(scene, GC_LAYOUT_HELP, &pane.pane,
                               gc_render_ui_image_texture(scene->ui, image),
                               gc_render_color_rgba(pane.tint));
    }
    const GcTextTable *table =
        gc_text_table(&scene->native_text, scene->language, GC_TEXT_HELP);
    for (unsigned index = 0; table && index < table->count; index++) {
        GcTextEntry entry;
        GcLayoutText layout;
        uint8_t alpha = gc_help_entry_alpha(&scene->help_state, index);
        if (!alpha || !gc_text_table_entry(table, index, &entry) ||
            !gc_layout_text(layouts, entry.flags, &layout))
            continue;
        layout.color_first = layout.color_second =
            gc_render_layout_color_alpha(scene->help_style.text, alpha);
        gc_render_layout_text_value(scene, GC_LAYOUT_HELP, &layout, entry.bytes);
    }
}

void gc_render_grid_lights(GcScene *scene, const GcCardGridLighting *lighting) {
    if (!scene->ui || !scene->ui->grid)
        return;
    CcMaterialQuad tile = gc_render_raster_material(scene->ui->grid);
    tile.tev_stages[0][4] =
        0xcf; /* Native RGB is white raster; alpha samples the grid. */
    /* USA 06070 submits the grid BTI's zero min/mag filters (GX_NEAR). */
    tile.nearest[0] = true;
    tile.wrap_s[0] = tile.wrap_t[0] = 1;
    /* The plane reaches logical x608, beyond the 592-pixel active framebuffer.
     * Clip there independently of the fractional viewport transform. */
    const CcClipRect clip = {
        24 + scene->display_offset_x,
        scene->startup.frame_rate == 50 ? 28.0f * 480 / 576 : 16, 592,
        scene->startup.frame_rate == 50 ? 520.0f * 480 / 576 : 448};
    cc_platform_set_clip(scene->platform, &clip);
    for (unsigned index = 0; index < GC_MENU_GRID_COLUMNS * GC_MENU_GRID_ROWS;
         index++) {
        GcLayoutVertex native[4];
        if (!gc_menu_grid_quad(index, native))
            break;
        static const unsigned order[4] = {0, 1, 3, 2};
        for (unsigned corner = 0; corner < 4; corner++) {
            const GcLayoutVertex *point = &native[order[corner]];
            float rgba[4] = {0};
            uint8_t alpha = (uint8_t)(scene->grid_alpha * 255);
            bool valid = lighting ? gc_card_lighting_grid_color(lighting, point->x,
                                                                point->y, alpha, rgba)
                                  : gc_menu_grid_color(&scene->face_geometry, point->x,
                                                       point->y, alpha, rgba);
            if (!valid) {
                cc_platform_set_clip(scene->platform, NULL);
                return;
            }
            CcColor color = {rgba[0], rgba[1], rgba[2], rgba[3]};
            tile.vertices[corner] = (CcMaterialVertex){
                .x = gc_render_projection_x(scene->perspective, point->x - 292) +
                     scene->display_offset_x,
                .y = gc_render_projection_center_y(scene->startup.frame_rate == 50) +
                     (point->y - 224) * scene->pixel_scale_y,
                .color = color,
                .uv = {{point->u, point->v}}};
        }
        cc_platform_draw_material_quad(scene->platform, &tile);
    }
    cc_platform_set_clip(scene->platform, NULL);
}

void gc_render_grid(GcScene *scene, double time) {
    (void)time;
    gc_render_grid_lights(scene, NULL);
}
