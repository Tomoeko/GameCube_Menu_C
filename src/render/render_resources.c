#include "render_internal.h"

#include <stdlib.h>
#include <string.h>

void gc_render_card_textures_destroy(GcScene *scene, GcCardTextures *textures) {
    if (!textures)
        return;
    for (size_t file = 0; file < GC_CARD_FILE_LIMIT; file++) {
        cc_platform_destroy_texture(scene->platform, textures->banners[file]);
        for (unsigned frame = 0; frame < GC_CARD_ICON_FRAMES; frame++)
            cc_platform_destroy_texture(scene->platform, textures->icons[file][frame]);
    }
    free(textures);
}

static uint32_t upload_image(GcScene *scene, const GcIplImage *image) {
    return image->rgba ? cc_platform_create_texture(scene->platform, (int)image->width,
                                                    (int)image->height, image->rgba)
                       : 0;
}

static uint32_t upload_startup_trail(GcScene *scene) {
    const GcIplImage *image = &scene->startup.trail_texture;
    enum { QUARTER_EDGE = 64, TILE_EDGE = QUARTER_EDGE * 2 };
    if (!image->rgba || image->width != QUARTER_EDGE || image->height != QUARTER_EDGE ||
        image->wrap_s != 2 || image->wrap_t != 2)
        return 0;
    uint8_t *pixels = malloc((size_t)TILE_EDGE * TILE_EDGE * 4);
    if (!pixels)
        return 0;
    /* USA/JAP 0x8131054c and EUR 0x81310e84 set TEX0 to S16/13:
     * emitted 0x4000 means 2.0. Bake its mirrored quarter-mask period
     * once so the basic GLES2/Metal path can use normalized clamp UVs. */
    for (unsigned y = 0; y < TILE_EDGE; ++y) {
        unsigned source_y = y < QUARTER_EDGE ? y : TILE_EDGE - 1 - y;
        for (unsigned x = 0; x < TILE_EDGE; ++x) {
            unsigned source_x = x < QUARTER_EDGE ? x : TILE_EDGE - 1 - x;
            size_t destination = ((size_t)y * TILE_EDGE + x) * 4;
            size_t source = ((size_t)source_y * QUARTER_EDGE + source_x) * 4;
            memcpy(pixels + destination, image->rgba + source, 4);
        }
    }
    uint32_t texture =
        cc_platform_create_texture(scene->platform, TILE_EDGE, TILE_EDGE, pixels);
    free(pixels);
    return texture;
}

static void ui_textures_destroy(GcScene *scene, GcUiTextures *ui) {
    if (!ui)
        return;
    for (size_t index = 0; ui->collection && index < ui->native.collection.count;
         index++)
        cc_platform_destroy_texture(scene->platform, ui->collection[index]);
    for (unsigned digit = 0; digit < 10; digit++)
        cc_platform_destroy_texture(scene->platform, ui->digits[digit]);
    for (unsigned language = 0; language < 7; language++) {
        for (unsigned day = 0; day < 7; day++)
            cc_platform_destroy_texture(scene->platform, ui->weekdays[language][day]);
        for (unsigned sound = 0; sound < 2; sound++)
            cc_platform_destroy_texture(scene->platform, ui->sound[language][sound]);
    }
    cc_platform_destroy_texture(scene->platform, ui->grid);
    cc_platform_destroy_texture(scene->platform, ui->card_numbers);
    gc_menu_textures_destroy(&ui->native);
    free(ui->collection);
    free(ui);
}

static bool upload_optional(GcScene *scene, const GcIplImage *image,
                            uint32_t *texture) {
    *texture = upload_image(scene, image);
    return !image->rgba || *texture != 0;
}

static bool ui_textures_load(GcScene *scene) {
    GcUiTextures *ui = calloc(1, sizeof(*ui));
    if (!ui)
        return false;
    if (!gc_menu_textures_decode(&scene->native_text, &ui->native))
        goto failed;
    ui->collection = calloc(ui->native.collection.count, sizeof(*ui->collection));
    if (!ui->collection)
        goto failed;
    for (size_t index = 0; index < ui->native.collection.count; index++)
        if (!upload_optional(scene, &ui->native.collection.images[index],
                             &ui->collection[index]))
            goto failed;
    for (unsigned digit = 0; digit < 10; digit++)
        if (!upload_optional(scene, &ui->native.digits[digit], &ui->digits[digit]))
            goto failed;
    for (unsigned language = 0; language < 7; language++) {
        for (unsigned day = 0; day < 7; day++)
            if (!upload_optional(scene, &ui->native.weekdays[language][day],
                                 &ui->weekdays[language][day]))
                goto failed;
        for (unsigned sound = 0; sound < 2; sound++)
            if (!upload_optional(scene, &ui->native.sound[language][sound],
                                 &ui->sound[language][sound]))
                goto failed;
    }
    if (!upload_optional(scene, &ui->native.grid, &ui->grid))
        goto failed;
    if (!upload_optional(scene, &ui->native.card_numbers, &ui->card_numbers))
        goto failed;
    scene->ui = ui;
    CcMaterialQuad material = gc_render_raster_material(ui->grid);
    material.nearest[0] = true;
    cc_platform_prepare_material(scene->platform, &material);
    material.tev_stages[0][4] = 0xcf;
    cc_platform_prepare_material(scene->platform, &material);
    return true;

failed:
    ui_textures_destroy(scene, ui);
    return false;
}

uint32_t gc_render_ui_image_texture(const GcUiTextures *ui, const GcIplImage *image) {
    if (!ui || !image)
        return 0;
    for (size_t index = 0; index < ui->native.collection.count; index++)
        if (image == &ui->native.collection.images[index])
            return ui->collection[index];
    for (unsigned digit = 0; digit < 10; digit++)
        if (image == &ui->native.digits[digit])
            return ui->digits[digit];
    for (unsigned language = 0; language < 7; language++) {
        for (unsigned day = 0; day < 7; day++)
            if (image == &ui->native.weekdays[language][day])
                return ui->weekdays[language][day];
        for (unsigned sound = 0; sound < 2; sound++)
            if (image == &ui->native.sound[language][sound])
                return ui->sound[language][sound];
    }
    return 0;
}

bool gc_scene_set_disc(GcScene *scene, const GcDisc *disc) {
    if (!scene)
        return false;
    uint32_t texture = disc ? upload_image(scene, &disc->banner) : 0;
    if (disc && disc->banner.rgba && !texture)
        return false;
    cc_platform_destroy_texture(scene->platform, scene->disc_banner);
    scene->disc_banner = texture;
    scene->disc = disc;
    return true;
}

bool gc_scene_set_cards(GcScene *scene, const gc_card_image cards[2]) {
    if (!scene || !cards)
        return false;
    GcCardTextures *pending[2] = {NULL, NULL};
    for (unsigned slot = 0; slot < 2; slot++) {
        if (!cards[slot].bytes)
            continue;
        pending[slot] = calloc(1, sizeof(*pending[slot]));
        if (!pending[slot])
            goto release_pending;
        for (size_t file = 0; file < cards[slot].card.file_count; file++) {
            GcCardArt art = {0};
            if (gc_card_art_load(&cards[slot], file, &art) != GC_CARD_IMAGE_OK)
                continue;
            pending[slot]->banners[file] = upload_image(scene, &art.banner);
            pending[slot]->timing[file].frame_count = art.frame_count;
            pending[slot]->timing[file].ping_pong = art.ping_pong;
            memcpy(pending[slot]->timing[file].durations, art.durations,
                   sizeof(art.durations));
            bool okay = !art.banner.rgba || pending[slot]->banners[file];
            for (unsigned frame = 0; frame < art.frame_count; frame++) {
                pending[slot]->icons[file][frame] =
                    upload_image(scene, &art.icons[frame]);
                if (art.icons[frame].rgba && !pending[slot]->icons[file][frame])
                    okay = false;
            }
            gc_card_art_destroy(&art);
            if (!okay)
                goto release_pending;
        }
    }
    for (unsigned slot = 0; slot < 2; slot++) {
        gc_render_card_textures_destroy(scene, scene->card_art[slot]);
        scene->card_art[slot] = pending[slot];
    }
    return true;

release_pending:
    for (unsigned slot = 0; slot < 2; slot++)
        gc_render_card_textures_destroy(scene, pending[slot]);
    return false;
}

bool gc_scene_init(GcScene *scene, CcPlatform *platform, const char *ipl_path) {
    if (!scene || !platform || !ipl_path)
        return false;
    *scene = (GcScene){.platform = platform};
    if (!gc_font_load(ipl_path, &scene->font))
        return false;
    scene->font_texture =
        cc_platform_create_texture(platform, (int)scene->font.ansi.width,
                                   (int)scene->font.ansi.height, scene->font.ansi.rgba);
    if (!scene->font_texture) {
        gc_font_destroy(&scene->font);
        return false;
    }
    if (scene->font.sjis.atlas.rgba) {
        scene->sjis_texture = upload_image(scene, &scene->font.sjis.atlas);
        if (!scene->sjis_texture) {
            gc_scene_destroy(scene);
            return false;
        }
    }
    if (!gc_text_load(ipl_path, &scene->native_text)) {
        gc_scene_destroy(scene);
        return false;
    }
    if (!gc_layouts_index(&scene->native_text, &scene->layouts)) {
        gc_scene_destroy(scene);
        return false;
    }
    if (!ui_textures_load(scene)) {
        gc_scene_destroy(scene);
        return false;
    }
    scene->menu_cube = gc_render_mesh_load(scene, ipl_path, "basecube");
    scene->boot_mark = gc_render_mesh_load(scene, ipl_path, "boot_demo_mark");
    scene->boot_base = gc_render_mesh_load(scene, ipl_path, "boot_demo_base_cube");
    scene->boot_cover = gc_render_mesh_load(scene, ipl_path, "boot_demo_cover_cube");
    scene->moving_cube = gc_render_mesh_load(scene, ipl_path, "s_cube");
    scene->logotype = gc_render_mesh_load(scene, ipl_path, "logotype_null");
    scene->face_cube = gc_render_mesh_load(scene, ipl_path, "cube_in_manu");
    scene->glyph_cube = gc_render_mesh_load(scene, ipl_path, "cube1");
    scene->card_base = gc_render_mesh_load(scene, ipl_path, "i_cube0");
    scene->card_cover = gc_render_mesh_load(scene, ipl_path, "i_cube1");
    if (!scene->menu_cube || !scene->boot_mark || !scene->boot_base ||
        !scene->boot_cover || !scene->moving_cube || !scene->logotype ||
        !scene->face_cube || !scene->glyph_cube || !scene->card_base ||
        !scene->card_cover || !gc_face_geometry_load(ipl_path, &scene->face_geometry) ||
        !gc_face_geometry_init(&scene->face_geometry, NULL,
                               &scene->face_geometry_state) ||
        !gc_edit_geometry_decode(&scene->native_text, &scene->edit_geometry) ||
        !gc_value_morph_style_decode(&scene->native_text, &scene->value_morph_style) ||
        !gc_help_style_decode(&scene->native_text, &scene->help_style) ||
        !gc_layout_glow_decode(&scene->native_text, &scene->text_glow) ||
        !gc_layout_card_popup_colors(&scene->native_text, scene->popup_colors) ||
        !gc_card_popup_style_decode(&scene->native_text, &scene->popup_style) ||
        !gc_card_lighting_style_decode(&scene->native_text,
                                       &scene->card_lighting_style) ||
        !gc_card_cell_style_decode(&scene->native_text, &scene->card_cell_style) ||
        !gc_page_transition_style_decode(&scene->native_text, &scene->page_style) ||
        !gc_startup_load(ipl_path, &scene->startup) ||
        !gc_menu_animation_init(&scene->startup, &scene->menu_animation) ||
        !gc_ipl_animation_load(ipl_path, GC_IPL_ANIMATION_JOINT,
                               &scene->logotype_joints) ||
        !gc_ipl_animation_load(ipl_path, GC_IPL_ANIMATION_COLOR,
                               &scene->logotype_colors)) {
        gc_scene_destroy(scene);
        return false;
    }
    scene->trail_texture = upload_startup_trail(scene);
    if (!scene->trail_texture) {
        gc_scene_destroy(scene);
        return false;
    }
    scene->page_snapshots = calloc(1, sizeof(*scene->page_snapshots));
    if (!scene->page_snapshots) {
        gc_scene_destroy(scene);
        return false;
    }
    return true;
}

void gc_scene_destroy(GcScene *scene) {
    if (!scene)
        return;
    cc_platform_destroy_texture(scene->platform, scene->font_texture);
    cc_platform_destroy_texture(scene->platform, scene->sjis_texture);
    cc_platform_destroy_texture(scene->platform, scene->disc_banner);
    cc_platform_destroy_texture(scene->platform, scene->trail_texture);
    gc_render_mesh_destroy(scene, scene->menu_cube);
    gc_render_mesh_destroy(scene, scene->boot_mark);
    gc_render_mesh_destroy(scene, scene->boot_base);
    gc_render_mesh_destroy(scene, scene->boot_cover);
    gc_render_mesh_destroy(scene, scene->moving_cube);
    gc_render_mesh_destroy(scene, scene->logotype);
    gc_render_mesh_destroy(scene, scene->face_cube);
    gc_render_mesh_destroy(scene, scene->glyph_cube);
    gc_render_mesh_destroy(scene, scene->card_base);
    gc_render_mesh_destroy(scene, scene->card_cover);
    gc_edit_geometry_destroy(&scene->edit_geometry);
    gc_startup_destroy(&scene->startup);
    gc_ipl_animation_destroy(&scene->logotype_joints);
    gc_ipl_animation_destroy(&scene->logotype_colors);
    for (unsigned slot = 0; slot < 2; slot++) {
        gc_render_card_textures_destroy(scene, scene->card_art[slot]);
        gc_render_card_textures_destroy(scene, scene->card_operation_art[slot]);
    }
    ui_textures_destroy(scene, scene->ui);
    gc_font_destroy(&scene->font);
    gc_text_destroy(&scene->native_text);
    free(scene->page_snapshots);
    *scene = (GcScene){0};
}
