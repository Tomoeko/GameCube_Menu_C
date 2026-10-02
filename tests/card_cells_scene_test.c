#include "gamecube/frame_history.h"
#include "render_internal.h"
#include "software.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool delayed_samples(void *context, uint32_t *value) {
    unsigned *counter = context;
    *value = ((*counter)++ & 1) ? 15 : 0;
    return true;
}

static void draw_tick(GcScene *scene, gc_menu *menu, unsigned tick) {
    menu->page_elapsed = (double)tick / scene->startup.frame_rate;
    gc_scene_draw(scene, menu);
}

static void card_entrance(GcScene *scene, gc_menu *menu) {
    unsigned samples = 0;
    gc_scene_set_card_random(scene, delayed_samples, &samples);
    gc_card cards[2] = {{.status = GC_CARD_READY, .capacity_blocks = 59},
                        {.status = GC_CARD_READY, .capacity_blocks = 251}};
    assert(gc_menu_set_card(menu, 0, &cards[0]));
    assert(gc_menu_set_card(menu, 1, &cards[1]));
    gc_menu_skip_startup(menu);
    draw_tick(scene, menu, 0);
    gc_menu_press(menu, GC_BUTTON_DOWN);
    draw_tick(scene, menu, 100);
    assert(gc_scene_can_press(scene, menu, GC_BUTTON_DOWN));
    gc_menu_press(menu, GC_BUTTON_DOWN);
    assert(menu->page == GC_PAGE_CARDS);
    draw_tick(scene, menu, 0);
    assert(samples == 508 && scene->card_cells.entrance);
    assert(!gc_scene_can_press(scene, menu, GC_BUTTON_CANCEL));
    draw_tick(scene, menu, 5);
    assert(scene->card_cells.cells[0][0].counter == 0);
    assert(scene->card_cells.cells[0][0].drawn_alpha == 0);

    /* The native memory icon's XYZ/alpha collapse releases the cell updater
     * on tick20 in this deterministic trace. Every invisible READY record
     * still participates in its delay+duration gate. */
    draw_tick(scene, menu, 20);
    assert(gc_face_geometry_editor_ready(&scene->face_geometry_state,
                                         GC_FACE_MEMORY_CARD));
    assert(scene->card_cells.cells[0][0].counter == 1);
    unsigned ready_tick = scene->startup.frame_rate == 50 ? 84 : 94;
    draw_tick(scene, menu, ready_tick - 1);
    assert(!gc_scene_can_press(scene, menu, GC_BUTTON_CANCEL));
    draw_tick(scene, menu, ready_tick);
    assert(gc_scene_can_press(scene, menu, GC_BUTTON_CANCEL));
    assert(!scene->card_cells.entrance);
    const bool ready[2] = {true, true};
    assert(gc_card_cells_ready(&scene->card_cells, ready));
    GcCardCells state = scene->card_cells;
    uint64_t paused_pixels = gc_software_frame_hash(scene->platform);
    draw_tick(scene, menu, ready_tick);
    assert(!memcmp(&state, &scene->card_cells, sizeof(state)));
    assert(paused_pixels == gc_software_frame_hash(scene->platform));

    /* An out-of-view cell must reject input just like a visible one. */
    scene->card_cells.cells[1][126].counter--;
    assert(!gc_scene_can_press(scene, menu, GC_BUTTON_CANCEL));
    assert(!gc_scene_card_operation_ready(scene));
    scene->card_cells = state;
    assert(gc_scene_card_operation_ready(scene));

    GcFrameHistory *history = gc_frame_history_create(2);
    GcBootControl boot = {0};
    GcFrameRuntime runtime = {0};
    assert(history);
    assert(gc_frame_history_save(history, 0, menu, &boot, scene, &runtime));
    draw_tick(scene, menu, ready_tick + 1);
    GcCardCells replay = scene->card_cells;
    uint64_t replay_pixels = gc_software_frame_hash(scene->platform);
    assert(gc_frame_history_save(history, 1, menu, &boot, scene, &runtime));
    uint64_t counter;
    assert(gc_frame_history_seek(history, -1, &counter, menu, &boot, scene, &runtime));
    assert(counter == 0 && !memcmp(&state, &scene->card_cells, sizeof(state)));
    draw_tick(scene, menu, ready_tick + 1);
    assert(!memcmp(&replay, &scene->card_cells, sizeof(replay)));
    assert(replay_pixels == gc_software_frame_hash(scene->platform));
    gc_frame_history_destroy(history);

    /* The first B edge after the exact gate returns to the face immediately. */
    assert(gc_scene_can_press(scene, menu, GC_BUTTON_CANCEL));
    gc_menu_press(menu, GC_BUTTON_CANCEL);
    assert(menu->page == GC_PAGE_FACE && menu->face == GC_FACE_MEMORY_CARD);
    gc_scene_set_card_random(scene, NULL, NULL);
}

static void card_scroll_retention(GcScene *scene, gc_menu *menu) {
    gc_card cards[2] = {
        {.status = GC_CARD_READY, .capacity_blocks = 59, .file_count = 24},
        {.status = GC_CARD_READY, .capacity_blocks = 251, .file_count = 1}};
    for (unsigned slot = 0; slot < 2; ++slot)
        for (size_t index = 0; index < cards[slot].file_count; ++index)
            cards[slot].files[index].blocks = 1;
    assert(gc_menu_set_card(menu, 0, &cards[0]));
    assert(gc_menu_set_card(menu, 1, &cards[1]));
    menu->card_slot = 0;
    menu->card_index = 23;
    menu->card_first_row = 2;
    menu->card_cursors[1] = (gc_card_cursor){0, 0};
    draw_tick(scene, menu, 100);
    assert(gc_scene_can_press(scene, menu, GC_BUTTON_CONFIRM));
    gc_menu_press(menu, GC_BUTTON_CONFIRM);
    assert(menu->page == GC_PAGE_CARDS);
    draw_tick(scene, menu, 0);
    draw_tick(scene, menu, 200);
    assert(gc_scene_can_press(scene, menu, GC_BUTTON_RIGHT));
    assert(scene->card_cells.cells[0][23].selection == 6);
    assert(scene->card_cells.first_rows[0] == 2);

    /* Crossing into an empty visible cell of B preserves A's scroll row,
     * cell positions, up arrow and eventual return cursor. */
    gc_menu_press(menu, GC_BUTTON_RIGHT);
    assert(menu->card_slot == 1 && menu->card_index == 12);
    assert(!gc_menu_card_selected(menu));
    draw_tick(scene, menu, 250);
    assert(gc_menu_card_first_row(menu, 0) == 2);
    assert(scene->card_cells.first_rows[0] == 2);
    assert(scene->card_cells.first_rows[1] == 0);
    assert(scene->card_arrow_alpha[0][0] == 20);
    assert(scene->card_cells.cells[0][23].selection == 0);
    assert(scene->card_cells.cells[1][12].selection == 6);
    assert(scene->card_cells.cells[0][8].drawn_alpha == 255);
    assert(scene->card_cells.cells[0][0].drawn_alpha == 0);
    GcCardCells paused = scene->card_cells;
    uint64_t pixels = gc_software_frame_hash(scene->platform);
    draw_tick(scene, menu, 250);
    assert(!memcmp(&paused, &scene->card_cells, sizeof(paused)));
    assert(pixels == gc_software_frame_hash(scene->platform));

    gc_menu_press(menu, GC_BUTTON_LEFT);
    assert(menu->card_slot == 0 && menu->card_first_row == 2);
    assert(menu->card_index == 23 && gc_menu_card_selected(menu));
    draw_tick(scene, menu, 300);
    assert(scene->card_cells.first_rows[0] == 2);
    assert(scene->card_cells.cells[0][23].selection == 6);
    assert(scene->card_cells.cells[1][12].selection == 0);
}

static void card_availability_pixels(GcScene *scene, gc_menu *menu) {
    gc_menu_init(menu,
                 scene->startup.frame_rate == 50 ? GC_REGION_EUROPE : GC_REGION_USA);
    menu->page = GC_PAGE_CARDS;
    menu->face = GC_FACE_MEMORY_CARD;
    const GcLayoutTable *table =
        gc_layout_table(&scene->layouts, menu->settings.language, GC_LAYOUT_CARD);
    GcLayoutFrame footer;
    assert(gc_layout_find_frame(table, "mes6", 0, &footer));
    unsigned footer_x = (unsigned)lroundf(footer.box.center_x + 28);
    unsigned footer_y =
        (unsigned)lroundf(240 + (footer.box.center_y - 224) * scene->pixel_scale_y);
    for (unsigned ready_mask = 0; ready_mask < 3; ++ready_mask) {
        for (unsigned slot = 0; slot < 2; ++slot) {
            gc_card card = {.status = (ready_mask & (1u << slot)) ? GC_CARD_READY
                                                                  : GC_CARD_ABSENT,
                            .capacity_blocks = 59};
            assert(gc_menu_set_card(menu, slot, &card));
        }
        draw_tick(scene, menu, 480 + ready_mask * 180);
        for (unsigned slot = 0; slot < 2; ++slot) {
            bool ready = (ready_mask & (1u << slot)) != 0;
            assert(gc_card_popup_header_alpha(&scene->popup_style, &scene->card_popups,
                                              slot) == (ready ? 255 : 0));
            assert(gc_card_popup_message_alpha(&scene->popup_style, &scene->card_popups,
                                               33 + slot) == (ready ? 0 : 255));
        }
        uint8_t pixel[4];
        assert(gc_software_read_pixel(scene->platform, footer_x, footer_y, pixel));
        assert(pixel[2] > pixel[0] + 40 && pixel[2] > pixel[1] + 40);
        if (!ready_mask) {
            assert(scene->card_lighting.center ==
                   (scene->card_lighting_style.selection_lights ? 10 : 0));
            assert(!scene->card_lighting.focus[0] && !scene->card_lighting.focus[1]);
            /* Empty slots have neither a colored Open header nor a red
             * message body. Their text and the grid are neutral RGB. */
            for (unsigned y = 48; y < 290; ++y)
                for (unsigned x = 48; x < 592; ++x) {
                    assert(gc_software_read_pixel(scene->platform, x, y, pixel));
                    assert(pixel[0] == pixel[1] && pixel[1] == pixel[2]);
                }
            /* The native 8x8 I4 grid's first texel is the line. At its two
             * pixel magnification, nearest filtering leaves no shoulder
             * on the adjacent transparent texel. Both slots share the
             * same center light while absent. */
            unsigned grid_y =
                (unsigned)lroundf(240 + (260 - 224) * scene->pixel_scale_y);
            uint8_t lines[2];
            const unsigned grid_x[2] = {128, 464};
            for (unsigned slot = 0; slot < 2; ++slot) {
                unsigned x = grid_x[slot] + 28;
                assert(gc_software_read_pixel(scene->platform, x, grid_y, pixel));
                lines[slot] = pixel[0];
                assert(lines[slot] > 20);
                assert(gc_software_read_pixel(scene->platform, x + 2, grid_y, pixel));
                assert(!pixel[0] && !pixel[1] && !pixel[2]);
            }
            assert(abs((int)lines[0] - lines[1]) <= 1);
        }
        uint64_t pixels = gc_software_frame_hash(scene->platform);
        GcCardPopups paused = scene->card_popups;
        draw_tick(scene, menu, 480 + ready_mask * 180);
        assert(!memcmp(&paused, &scene->card_popups, sizeof(paused)));
        assert(pixels == gc_software_frame_hash(scene->platform));
    }
}

static bool card_icon_visible(const GcScene *scene) {
    const GcMesh *cover = scene->card_cover;
    for (size_t index = 0; index < cover->model.triangle_count; ++index)
        if (cover->faces[index].material == 1 && cover->faces[index].visible)
            return true;
    return false;
}

static void card_icon_pass(GcScene *scene, gc_menu *menu) {
    gc_menu_init(menu,
                 scene->startup.frame_rate == 50 ? GC_REGION_EUROPE : GC_REGION_USA);
    menu->page = GC_PAGE_CARDS;
    menu->face = GC_FACE_MEMORY_CARD;
    gc_card card = {.status = GC_CARD_READY, .capacity_blocks = 59, .file_count = 1};
    card.files[0].blocks = 1;
    assert(gc_menu_set_card(menu, 0, &card));
    draw_tick(scene, menu, 200);
    assert(!card_icon_visible(scene));

    GcCardTextures *art = calloc(1, sizeof(*art));
    const uint8_t pixels[16] = {255, 0, 255, 255, 255, 0, 255, 255,
                                255, 0, 255, 255, 255, 0, 255, 255};
    assert(art);
    art->icons[0][0] = cc_platform_create_texture(scene->platform, 2, 2, pixels);
    assert(art->icons[0][0]);
    art->timing[0].frame_count = 2;
    art->timing[0].durations[0] = art->timing[0].durations[1] = 1;
    scene->card_art[0] = art;
    scene->card_ticks = 0;
    draw_tick(scene, menu, 200);
    assert(card_icon_visible(scene));
    uint64_t icon_pixels = gc_software_frame_hash(scene->platform);

    /* A native blank animation frame skips the icon shape, rather than
     * falling back to the model's embedded texture. */
    scene->card_ticks = 1;
    draw_tick(scene, menu, 200);
    assert(!card_icon_visible(scene));
    assert(icon_pixels != gc_software_frame_hash(scene->platform));
    gc_render_card_textures_destroy(scene, scene->card_art[0]);
    scene->card_art[0] = NULL;
}

static void card_transfer_prompt_layer(GcScene *scene, gc_menu *menu) {
    unsigned white_pixels[4096];
    unsigned covered_pixels[4096];
    for (unsigned source = 0; source < 2; ++source) {
        for (unsigned moving = 0; moving < 2; ++moving) {
            gc_menu_init(menu, scene->startup.frame_rate == 50 ? GC_REGION_EUROPE
                                                               : GC_REGION_USA);
            menu->page = GC_PAGE_CARDS;
            menu->face = GC_FACE_MEMORY_CARD;
            gc_card card = {
                .status = GC_CARD_READY, .capacity_blocks = 59, .file_count = 1};
            card.files[0].blocks = 1;
            card.files[0].allow_copy = card.files[0].allow_move = true;
            assert(gc_menu_set_card(menu, source, &card));
            menu->card_slot = source;
            draw_tick(scene, menu, 1000 + source * 200 + moving * 100);
            const GcLayoutTable *table = gc_layout_table(
                &scene->layouts, menu->settings.language, GC_LAYOUT_CARD);
            GcLayoutText absence;
            assert(gc_layout_find_text(table, source ? "txt4" : "txt5", 0, &absence));
            unsigned center_x = (unsigned)lroundf(absence.box.center_x + 28);
            unsigned center_y = (unsigned)lroundf(240 + (absence.box.center_y - 224) *
                                                            scene->pixel_scale_y);
            unsigned white_count = 0;
            uint8_t rgba[4];
            for (unsigned y = center_y - 40; y < center_y + 40; ++y)
                for (unsigned x = center_x - 112; x < center_x + 112; ++x) {
                    assert(gc_software_read_pixel(scene->platform, x, y, rgba));
                    if (rgba[0] > 240 && rgba[1] > 240 && rgba[2] > 240) {
                        assert(white_count < 4096);
                        white_pixels[white_count++] = y * 640 + x;
                    }
                }
            assert(white_count > 0);
            menu->card_action = moving ? GC_CARD_ACTION_MOVE : GC_CARD_ACTION_COPY;
            menu->message = GC_MESSAGE_CARD_ABSENT;
            menu->message_return_page = GC_PAGE_CARDS;
            menu->page = GC_PAGE_MESSAGE;
            draw_tick(scene, menu, 20);
            assert(scene->card_popups.message_ticks[33 + (source ^ 1)] == 0);
            GcCardPopups saved = scene->card_popups;
            /* Retained outgoing notice states must also respect the native
             * layering while their twenty-tick close is still in flight. */
            saved.message_ticks[33 + (source ^ 1)] = 20;
            scene->card_popups.message_ticks[33 + (source ^ 1)] = 0;
            draw_tick(scene, menu, 20);
            unsigned covered_count = 0;
            for (unsigned index = 0; index < white_count; ++index) {
                unsigned pixel = white_pixels[index];
                assert(gc_software_read_pixel(scene->platform, pixel % 640, pixel / 640,
                                              rgba));
                if (rgba[0] > rgba[1] + 40 && rgba[1] < 100)
                    covered_pixels[covered_count++] = pixel;
            }
            assert(covered_count > 0);
            scene->card_popups = saved;
            draw_tick(scene, menu, 20);
            for (unsigned index = 0; index < covered_count; ++index) {
                unsigned pixel = covered_pixels[index];
                assert(gc_software_read_pixel(scene->platform, pixel % 640, pixel / 640,
                                              rgba));
                /* The native translucent dialog can retain dim background
                 * text, but the absence glyph must never sit above its body. */
                assert(rgba[1] < 210 && rgba[2] < 210);
            }
        }
    }
}

int main(int argc, char **argv) {
    if (argc < 2)
        return EXIT_SUCCESS;
    CcPlatform *platform = cc_platform_create("Card scene verification", 640, 480);
    GcScene scene = {0};
    gc_menu *menu = calloc(1, sizeof(*menu));
    assert(platform && menu && gc_scene_init(&scene, platform, argv[1]));
    gc_menu_init(menu,
                 scene.startup.frame_rate == 50 ? GC_REGION_EUROPE : GC_REGION_USA);
    card_entrance(&scene, menu);
    card_scroll_retention(&scene, menu);
    card_availability_pixels(&scene, menu);
    card_icon_pass(&scene, menu);
    card_transfer_prompt_layer(&scene, menu);
    gc_scene_destroy(&scene);
    cc_platform_destroy(platform);
    free(menu);
    puts("Native card entrance, earliest B gate, pause and replay passed.");
    return EXIT_SUCCESS;
}
