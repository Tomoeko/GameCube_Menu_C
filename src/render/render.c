#include "render_internal.h"
#include "gamecube/disc_control.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define SCENE_LOOP_TICKS UINT64_C(3628553586278400)
#define UI_LOOP_TICKS UINT64_C(589824)

_Static_assert(SCENE_LOOP_TICKS % GC_FACE_OPTIONS_PERIOD == 0 &&
                   SCENE_LOOP_TICKS % GC_FACE_MEMORY_PERIOD == 0 &&
                   SCENE_LOOP_TICKS % GC_FACE_GAMEPLAY_PERIOD == 0 &&
                   SCENE_LOOP_TICKS % GC_STARTUP_MENU_PERIOD == 0 &&
                   SCENE_LOOP_TICKS % UI_LOOP_TICKS == 0 &&
                   SCENE_LOOP_TICKS % 25 == 0 && SCENE_LOOP_TICKS % 62 == 0,
               "Offline time reduction must preserve every native phase");

static const CcColor white = {1, 1, 1, 1};

bool gc_scene_card_erase(GcScene *scene, unsigned slot, unsigned visible_cell,
                         const uint8_t delays[12], const int16_t angles[12]) {
    if (!scene || slot > 1 || visible_cell >= 16 || !delays || !angles)
        return false;
    for (unsigned piece = 0; piece < 12; piece++)
        if (delays[piece] > 3)
            return false;
    const GcLayoutTable *layout =
        gc_layout_table(&scene->layouts, scene->language, GC_LAYOUT_CARD);
    char key[5];
    snprintf(key, sizeof(key), "i%c%02u", slot ? 'b' : 'a', visible_cell);
    GcLayoutPane pane;
    GcFaceGeometryPoint point;
    if (!gc_layout_find_pane(layout, key, 0, &pane) ||
        !gc_card_geometry_sample(&scene->face_geometry, &scene->startup,
                                 pane.box.center_x, pane.box.center_y, true, true,
                                 GC_CARD_BLOCK_BYTES,
                                 scene->card_selection_ticks[slot][visible_cell],
                                 scene->menu_animation.oscillator_phase, 255, &point))
        return false;
    memcpy(scene->card_erase_matrix, point.matrix, sizeof(point.matrix));
    memcpy(scene->card_erase_delays, delays, sizeof(scene->card_erase_delays));
    memcpy(scene->card_erase_angles, angles, sizeof(scene->card_erase_angles));
    scene->card_erase_tick = 0;
    scene->card_erasing = true;
    return true;
}

void gc_scene_card_operation_end(GcScene *scene) {
    if (!scene)
        return;
    for (unsigned slot = 0; slot < 2; ++slot) {
        if (!scene->card_art[slot])
            scene->card_art[slot] = scene->card_operation_art[slot];
        else
            gc_render_card_textures_destroy(scene, scene->card_operation_art[slot]);
        scene->card_operation_art[slot] = NULL;
    }
    scene->card_operation_active = false;
}

bool gc_scene_card_operation_begin(GcScene *scene, const gc_menu *menu,
                                   const GcCardOperation *operation,
                                   const int16_t piece_angles[12]) {
    if (!scene || !menu || !operation || operation->source_slot > 1)
        return false;
    const GcLayoutTable *layout =
        gc_layout_table(&scene->layouts, scene->language, GC_LAYOUT_CARD);
    float centers[2][16][2];
    for (unsigned slot = 0; slot < 2; ++slot) {
        for (unsigned cell = 0; cell < 16; ++cell) {
            char key[5];
            snprintf(key, sizeof(key), "i%c%02u", slot ? 'b' : 'a', cell);
            GcLayoutPane pane;
            if (!gc_layout_find_pane(layout, key, 0, &pane))
                return false;
            centers[slot][cell][0] = pane.box.center_x;
            centers[slot][cell][1] = pane.box.center_y;
        }
    }
    size_t first =
        gc_menu_card_first_row(menu, operation->source_slot) * GC_CARD_COLUMNS;
    if (operation->action != GC_CARD_ACTION_FORMAT &&
        (operation->source_file_index < first ||
         operation->source_file_index - first >= 16))
        return false;
    gc_scene_card_operation_end(scene);
    scene->card_operation = *operation;
    memcpy(scene->card_operation_cards, menu->cards,
           sizeof(scene->card_operation_cards));
    memcpy(scene->card_operation_centers, centers, sizeof(centers));
    for (unsigned slot = 0; slot < 2; ++slot) {
        scene->card_operation_first_rows[slot] = gc_menu_card_first_row(menu, slot);
        scene->card_operation_art[slot] = scene->card_art[slot];
        scene->card_art[slot] = NULL;
    }
    if (piece_angles)
        memcpy(scene->card_erase_angles, piece_angles,
               sizeof(scene->card_erase_angles));
    else
        memset(scene->card_erase_angles, 0, sizeof(scene->card_erase_angles));
    scene->card_operation_active = true;
    scene->card_erasing = false;
    if (operation->action != GC_CARD_ACTION_FORMAT) {
        unsigned cell = (unsigned)(operation->source_file_index - first);
        const float *center = centers[operation->source_slot][cell];
        gc_card_geometry_sample(
            &scene->face_geometry, &scene->startup, center[0], center[1], true, true,
            GC_CARD_BLOCK_BYTES,
            scene->card_selection_ticks[operation->source_slot][cell],
            scene->menu_animation.oscillator_phase, 255, &scene->card_operation_point);
    }
    return true;
}

void gc_scene_card_operation_update(GcScene *scene, const GcCardOperation *operation) {
    if (scene && operation)
        scene->card_operation = *operation;
}

bool gc_scene_card_operation_ready(const GcScene *scene) {
    if (!scene)
        return false;
    for (unsigned channel = 0; channel < GC_TRANSITION_CHANNEL_COUNT; ++channel) {
        uint8_t alpha =
            gc_page_transition_alpha(&scene->page_style, &scene->page_transitions,
                                     GC_TRANSITION_CARD, (GcTransitionChannel)channel);
        if (alpha <= 172)
            return false;
    }
    if (!scene->page_snapshots || !scene->page_snapshots->saved[GC_TRANSITION_CARD])
        return false;
    const gc_menu *menu = &scene->page_snapshots->menus[GC_TRANSITION_CARD];
    const bool ready[2] = {menu->cards[0].status == GC_CARD_READY,
                           menu->cards[1].status == GC_CARD_READY};
    /* USA 1f9e4 counts all127 records of each READY slot, including cells
     * outside the current scroll window, and rejects the global erase flag. */
    if (!gc_card_cells_ready(&scene->card_cells, ready) || scene->card_erasing ||
        (scene->card_operation_active && scene->card_operation.erase_active))
        return false;
    return true;
}

static bool card_page_ready(const GcScene *scene) {
    for (unsigned channel = 0; channel < GC_TRANSITION_CHANNEL_COUNT; ++channel)
        if (gc_page_transition_alpha(&scene->page_style, &scene->page_transitions,
                                     GC_TRANSITION_CARD,
                                     (GcTransitionChannel)channel) <= 172)
            return false;
    return true;
}

static bool card_cell_layout(const GcScene *scene, float centers[2][16][2]) {
    const GcLayoutTable *table =
        gc_layout_table(&scene->layouts, scene->language, GC_LAYOUT_CARD);
    for (unsigned slot = 0; slot < 2; ++slot)
        for (unsigned cell = 0; cell < 16; ++cell) {
            char name[5];
            GcLayoutPane pane;
            snprintf(name, sizeof(name), "i%c%02u", slot ? 'b' : 'a', cell);
            if (!gc_layout_find_pane(table, name, 0, &pane))
                return false;
            centers[slot][cell][0] = pane.box.center_x;
            centers[slot][cell][1] = pane.box.center_y;
        }
    return true;
}

static void card_cells_sync(GcScene *scene, const gc_menu *menu, uint64_t ticks) {
    if (gc_page_transition_group(menu) != GC_TRANSITION_CARD ||
        !scene->card_cells.initialized)
        return;
    const bool ready[2] = {menu->cards[0].status == GC_CARD_READY,
                           menu->cards[1].status == GC_CARD_READY};
    bool erasing = scene->card_operation_active && scene->card_operation.erase_active;
    gc_card_cells_advance(
        &scene->card_cell_style, &scene->card_cells, ready,
        gc_face_geometry_editor_ready(&scene->face_geometry_state, GC_FACE_MEMORY_CARD),
        card_page_ready(scene), erasing, menu->card_slot,
        gc_menu_card_cursor_index(menu, menu->card_slot), ticks);
}

static void disc_face_sync(GcScene *scene, const gc_menu *menu, uint64_t ticks) {
    /* USA 1155c selects three independent twenty-tick face faders. A valid
     * banner wins; absent media uses nodi, other pending/error states qust. */
    if (menu->page != GC_PAGE_CUBE && menu->page != GC_PAGE_FACE)
        return;
    unsigned active = menu->disc_status == GC_DISC_READY && scene->disc_banner ? 0u
                      : menu->disc_status == GC_DISC_ABSENT                    ? 1u
                                                                               : 2u;
    for (unsigned index = 0; index < 3; ++index) {
        unsigned value = scene->disc_face_ticks[index];
        scene->disc_face_ticks[index] =
            index == active ? (uint8_t)(ticks >= 20 - value ? 20 : value + ticks)
                            : (uint8_t)(ticks >= value ? 0 : value - ticks);
    }
}

static void editor_sync(GcScene *scene, const gc_menu *menu, bool ready,
                        uint64_t ticks) {
    bool starting = ticks && ready && menu->page == GC_PAGE_DISC &&
                    menu->launch_requested && !scene->edit_state.disc_launching;
    if (!gc_edit_state_advance(&scene->edit_state, &scene->edit_geometry, menu, ready,
                               ticks))
        return;
    if (starting && scene->card_random) {
        /* USA 28c6c / PAL equivalent consumes one sample when Start
         * restarts the original Disc block timer, even for local media. */
        uint32_t value;
        scene->card_random(scene->card_random_context, &value);
    }
}

static void menu_animation_sync(GcScene *scene, const gc_menu *menu) {
    bool selected_face = menu->page != GC_PAGE_CUBE;
    bool focused = selected_face && menu->page != GC_PAGE_FACE;
    double elapsed =
        isfinite(menu->page_elapsed) && menu->page_elapsed > 0 ? menu->page_elapsed : 0;
    double delta = scene->animation_started && scene->animation_page == menu->page
                       ? fmax(0, elapsed - scene->animation_elapsed)
                       : elapsed;
    double rate = scene->startup.frame_rate;
    /* Keep enormous offline capture times bounded without losing the native
     * HELP, glow, face-loop and unsigned animation phases. */
    double total =
        delta > 1000000000000.0 / rate
            ? 1200 + fmod(delta - 1200 / rate, (double)SCENE_LOOP_TICKS / rate) * rate +
                  scene->animation_fraction
            : delta * rate + scene->animation_fraction;
    /* Repeated 1/rate additions can land just below an integral video
     * update. Keep real fractional intervals while preserving exact steps. */
    double nearest_tick = round(total);
    if (fabs(total - nearest_tick) < 1e-9)
        total = nearest_tick;
    uint64_t video_ticks = (uint64_t)floor(total);
    unsigned first_ticks = video_ticks > 128 ? 128 : (unsigned)video_ticks;
    if (!scene->animation_started) {
        /* Native 0x81312068 increments the sway phase before the first
         * menu draw. The transition leaves it at 145 * 7. */
        gc_menu_animation_update(&scene->startup, &scene->menu_animation, false,
                                 GC_FACE_GAME_PLAY, false);
        gc_card_usage_advance(&scene->card_usage, menu->cards, 1);
        disc_face_sync(scene, menu, 1);
        scene->ui_ticks = scene->boot_control ? scene->boot_control->video_tick
                                              : scene->startup.menu_ticks;
    }
    scene->ui_ticks = (scene->ui_ticks + video_ticks % UI_LOOP_TICKS) % UI_LOOP_TICKS;
    bool card_page = gc_page_transition_group(menu) == GC_TRANSITION_CARD;
    bool was_card_page =
        scene->animation_started &&
        (scene->animation_page == GC_PAGE_CARDS ||
         scene->animation_page == GC_PAGE_CARD_ACTION ||
         scene->animation_page == GC_PAGE_CARD_CONFIRM ||
         (scene->animation_page == GC_PAGE_MESSAGE && scene->card_cells.initialized));
    if (card_page) {
        const size_t rows[2] = {gc_menu_card_first_row(menu, 0),
                                gc_menu_card_first_row(menu, 1)};
        float centers[2][16][2];
        if ((!was_card_page || !scene->card_cells.initialized) &&
            card_cell_layout(scene, centers))
            gc_card_cells_begin(&scene->card_cell_style, &scene->card_cells, centers,
                                rows, scene->card_random, scene->card_random_context);
        else if (!scene->card_cells.entrance &&
                 !(scene->card_operation_active &&
                   scene->card_operation.erase_active) &&
                 (rows[0] != scene->card_cells.first_rows[0] ||
                  rows[1] != scene->card_cells.first_rows[1]) &&
                 card_cell_layout(scene, centers))
            gc_card_cells_relayout(&scene->card_cell_style, &scene->card_cells, centers,
                                   rows);
    }
    gc_face_geometry_set_clock(&scene->face_geometry_state, &menu->clock);
    /* The editor's CAFD counter begins only after every native face point has
     * collapsed. Advance that gate and its builder together, one video tick
     * at a time, rather than starting from the page's elapsed host time. */
    for (unsigned tick = 0; tick < first_ticks; tick++) {
        gc_face_geometry_advance(&scene->face_geometry, &scene->face_geometry_state, 1,
                                 selected_face, menu->face, focused);
        editor_sync(
            scene, menu,
            gc_face_geometry_editor_ready(&scene->face_geometry_state, menu->face), 1);
        gc_value_morph_advance(&scene->value_morph_state, &scene->value_morph_style,
                               menu, scene->edit_state.ready, 1, scene->card_random,
                               scene->card_random_context);
        gc_menu_animation_update(&scene->startup, &scene->menu_animation, selected_face,
                                 menu->face, focused);
        gc_page_transitions_advance(&scene->page_style, &scene->page_transitions, menu,
                                    1);
        card_cells_sync(scene, menu, 1);
    }
    uint64_t remaining = video_ticks - first_ticks;
    if (remaining) {
        gc_face_geometry_advance(&scene->face_geometry, &scene->face_geometry_state,
                                 remaining, selected_face, menu->face, focused);
        editor_sync(
            scene, menu,
            gc_face_geometry_editor_ready(&scene->face_geometry_state, menu->face),
            remaining);
        gc_value_morph_advance(&scene->value_morph_state, &scene->value_morph_style,
                               menu, scene->edit_state.ready, remaining,
                               scene->card_random, scene->card_random_context);
        scene->menu_animation.oscillator_phase =
            (uint16_t)(scene->menu_animation.oscillator_phase +
                       (unsigned)(remaining % 65536) * 7);
        gc_page_transitions_advance(&scene->page_style, &scene->page_transitions, menu,
                                    remaining);
        card_cells_sync(scene, menu, remaining);
    } else if (!video_ticks) {
        editor_sync(
            scene, menu,
            gc_face_geometry_editor_ready(&scene->face_geometry_state, menu->face), 0);
    }
    gc_help_advance(&scene->help_state, menu, video_ticks);
    if (!video_ticks)
        gc_page_transitions_advance(&scene->page_style, &scene->page_transitions, menu,
                                    0);
    gc_card_popups_advance(&scene->popup_style, &scene->card_popups, menu, video_ticks);
    gc_card_usage_advance(&scene->card_usage, menu->cards, video_ticks);
    gc_card_lighting_advance(&scene->card_lighting_style, &scene->card_lighting, menu,
                             video_ticks);
    disc_face_sync(scene, menu, video_ticks);
    scene->fatal_error_latched |= menu->disc_status == GC_DISC_FATAL;
    if (scene->fatal_error_latched) {
        unsigned remaining = 20u - scene->fatal_error_ticks;
        scene->fatal_error_ticks =
            (uint8_t)(video_ticks >= remaining
                          ? 20
                          : scene->fatal_error_ticks + video_ticks);
    }
    int full_page = gc_page_transition_group(menu);
    if (scene->page_snapshots && full_page >= 0) {
        scene->page_snapshots->menus[full_page] = *menu;
        scene->page_snapshots->editors[full_page] = scene->edit_state;
        scene->page_snapshots->morphs[full_page] = scene->value_morph_state;
        scene->page_snapshots->saved[full_page] = true;
    }
    unsigned disc_entry = gc_disc_control_text_index(menu->disc_status);
    for (unsigned index = 0; index < 5; index++) {
        unsigned value = scene->disc_text_ticks[index];
        if (menu->page != GC_PAGE_DISC)
            continue;
        bool shown = index == disc_entry;
        scene->disc_text_ticks[index] =
            shown ? (uint8_t)(video_ticks >= 20 - value ? 20 : value + video_ticks)
                  : (uint8_t)(video_ticks >= value ? 0 : value - video_ticks);
    }
    if (menu->page == GC_PAGE_DISC) {
        unsigned value = scene->disc_metadata_ticks;
        bool shown = menu->disc_status == GC_DISC_READY && scene->disc_banner;
        scene->disc_metadata_ticks =
            shown ? (uint8_t)(video_ticks >= 20 - value ? 20 : value + video_ticks)
                  : (uint8_t)(video_ticks >= value ? 0 : value - video_ticks);
    }
    unsigned selection_ticks = video_ticks > 6 ? 6 : (unsigned)video_ticks;
    for (unsigned slot = 0; slot < 2; slot++) {
        size_t first = gc_menu_card_first_row(menu, slot) * GC_CARD_COLUMNS;
        for (unsigned cell = 0; cell < 16; cell++) {
            bool selected = focused && menu->face == GC_FACE_MEMORY_CARD &&
                            !scene->card_cells.entrance && slot == menu->card_slot &&
                            first + cell == gc_menu_card_cursor_index(menu, slot);
            unsigned value = scene->card_selection_ticks[slot][cell];
            value = selected
                        ? (value + selection_ticks > 6 ? 6 : value + selection_ticks)
                        : (value > selection_ticks ? value - selection_ticks : 0);
            scene->card_selection_ticks[slot][cell] = (uint8_t)value;
        }
    }
    if (!card_page || !was_card_page)
        scene->card_ticks = 0;
    if (card_page)
        scene->card_ticks += video_ticks;
    for (unsigned slot = 0; slot < 2; slot++) {
        size_t rows =
            (menu->cards[slot].file_count + GC_CARD_COLUMNS - 1) / GC_CARD_COLUMNS;
        size_t maximum = rows > GC_CARD_VISIBLE_ROWS ? rows - GC_CARD_VISIBLE_ROWS : 0;
        size_t first = gc_menu_card_first_row(menu, slot);
        const bool visible[2] = {menu->page == GC_PAGE_CARDS && first > 0,
                                 menu->page == GC_PAGE_CARDS && first < maximum};
        for (unsigned direction = 0; direction < 2; direction++) {
            unsigned value = scene->card_arrow_alpha[slot][direction];
            scene->card_arrow_alpha[slot][direction] =
                visible[direction]
                    ? (uint8_t)(video_ticks >= 20 - value ? 20 : value + video_ticks)
                    : (uint8_t)(video_ticks >= value ? 0 : value - video_ticks);
        }
    }
    if (scene->card_erasing) {
        if (!card_page || video_ticks >= 14 - scene->card_erase_tick)
            scene->card_erasing = false;
        else
            scene->card_erase_tick = (uint8_t)(scene->card_erase_tick + video_ticks);
    }
    scene->animation_fraction = total - floor(total);
    scene->animation_page = menu->page;
    scene->animation_elapsed = elapsed;
    scene->animation_started = true;
    gc_menu_animation_sample(&scene->startup, &scene->menu_animation, selected_face,
                             menu->face, focused, &scene->menu_pose);
    scene->perspective = true;
}

static void glass_cube(GcScene *scene, bool back) {
    float scale[12] = {0};
    for (unsigned axis = 0; axis < 3; axis++)
        scale[axis * 5] = scene->menu_pose.cube_scale;
    const unsigned materials[2] = {back ? 3u : 1u, back ? 2u : 0u};
    for (unsigned pass = 0; pass < 2; pass++) {
        scene->menu_cube->material_mask = 1u << materials[pass];
        gc_render_mesh_draw(scene, scene->menu_cube, scene->menu_pose.cube_matrix,
                            scale, 320, 240, (float)scene->menu_pose.glass_alpha / 255);
    }
    scene->menu_cube->material_mask = 0;
}

static void face_panes(GcScene *scene, const gc_menu *menu, GcLayoutGroup group) {
    const GcLayoutTable *table =
        gc_layout_table(&scene->layouts, scene->language, group);
    if (!table || !scene->ui)
        return;
    const char *disc_order[6] = {"titl", "zodi", "nodi", "zust", "qust", "bana"};
    unsigned pane_count = group == GC_LAYOUT_DISC_FACE ? 6 : table->pane_count;
    for (unsigned index = 0; index < pane_count; index++) {
        GcLayoutPane pane;
        bool found = group == GC_LAYOUT_DISC_FACE
                         ? gc_layout_find_pane(table, disc_order[index], 0, &pane)
                         : gc_layout_pane(table, index, &pane);
        if (!found)
            continue;
        if (group == GC_LAYOUT_DISC_FACE) {
            if (!memcmp(pane.name, "bana", 4)) {
                CcColor tint = white;
                tint.a = (float)(scene->disc_face_ticks[0] * 255 / 20) / 255;
                gc_render_layout_image(scene, group, &pane, scene->disc_banner, tint);
                continue;
            }
        }
        const GcIplImage *image =
            gc_menu_textures_pane(&scene->ui->native, menu, group, &pane);
        CcColor tint = pane.name[0] == 'z'
                           ? (CcColor){40.0f / 255, 40.0f / 255, 40.0f / 255, 1}
                           : white;
        if (group == GC_LAYOUT_DISC_FACE) {
            if (!memcmp(pane.name, "nodi", 4) || !memcmp(pane.name, "zodi", 4))
                tint.a = (float)(scene->disc_face_ticks[1] * 255 / 20) / 255;
            else if (!memcmp(pane.name, "qust", 4) || !memcmp(pane.name, "zust", 4))
                tint.a = (float)(scene->disc_face_ticks[2] * 255 / 20) / 255;
        }
        if (group == GC_LAYOUT_CARD_FACE &&
            (!memcmp(pane.name, "mca1", 4) || !memcmp(pane.name, "mcb1", 4))) {
            unsigned slot = pane.name[2] == 'b';
            tint.a = (float)gc_card_usage_alpha(&scene->card_usage, slot, 255) / 255;
        }
        if (group == GC_LAYOUT_OPTIONS && menu->region == GC_REGION_EUROPE) {
            pane.box.center_y += scene->edit_geometry.option_caption_offsets[3];
            if (!memcmp(pane.name, "lan", 3) && pane.name[3] >= '1' &&
                pane.name[3] <= '6')
                tint.a = (float)gc_edit_language_alpha(
                             &scene->edit_state, &scene->edit_geometry,
                             (gc_language)(pane.name[3] - '1'), false) /
                         255;
            else if (!memcmp(pane.name, "sla", 3))
                tint.a = 128.0f / 255;
        }
        gc_render_layout_image(scene, group, &pane,
                               gc_render_ui_image_texture(scene->ui, image), tint);
    }
}

static void face_frames(GcScene *scene, GcLayoutGroup group) {
    const GcLayoutTable *table =
        gc_layout_table(&scene->layouts, scene->language, group);
    if (!table)
        return;
    for (unsigned index = 0; index < table->frame_count; index++) {
        GcLayoutFrame frame;
        if (!gc_layout_frame(table, index, &frame))
            continue;
        if (group == GC_LAYOUT_CARD_FACE &&
            (!memcmp(frame.name, "mca2", 4) || !memcmp(frame.name, "mcb2", 4))) {
            unsigned slot = frame.name[2] == 'b';
            unsigned height = (unsigned)gc_card_usage_fill(&scene->card_usage, slot) *
                              frame.parameters[3] / 255;
            if (!height)
                continue;
            float bottom = frame.box.center_y + (float)frame.parameters[3] / 32;
            frame.parameters[3] = (uint16_t)height;
            frame.box.center_y = bottom - (float)height / 32;
            gc_render_layout_frame_value(scene, group, &frame, UINT32_MAX);
            continue;
        }
        unsigned occurrence = 0;
        for (unsigned previous = 0; previous < index; previous++) {
            GcLayoutFrame other;
            if (gc_layout_frame(table, previous, &other) &&
                !memcmp(frame.name, other.name, 4))
                occurrence++;
        }
        gc_render_layout_frame(scene, group, frame.name, occurrence, UINT32_MAX);
    }
}

static void face_geometry(GcScene *scene) {
    GcMesh *models[3] = {scene->face_cube, scene->card_base, scene->card_cover};
    for (unsigned face = 0; face < 4; face++)
        for (size_t index = 0; index < gc_face_geometry_count((gc_face)face); index++) {
            GcFaceGeometryPoint point;
            if (!gc_face_geometry_get(&scene->face_geometry,
                                      &scene->face_geometry_state, &scene->menu_pose,
                                      (gc_face)face, index, &point) ||
                !point.alpha || point.model > GC_FACE_MODEL_CARD_COVER)
                continue;
            gc_render_mesh_piece(scene, models[point.model], point.matrix, point.scale,
                                 point.registers, point.register_mask,
                                 (float)point.alpha / 255);
        }
}

static void edit_points_draw(GcScene *scene, size_t count) {
    bool perspective = scene->perspective;
    float camera_y = scene->camera_y;
    scene->perspective = true;
    scene->camera_y = 0;
    for (size_t index = 0; index < count; index++) {
        const GcEditPoint *point = &scene->ui->edit_points[index];
        float matrix[12];
        if (!gc_edit_point_matrix(point, matrix))
            continue;
        float scale[3] = {point->scale, point->scale, point->scale};
        int16_t registers[2][4];
        for (unsigned color = 0; color < 2; color++)
            for (unsigned channel = 0; channel < 4; channel++)
                registers[color][channel] = point->colors[color][channel];
        /* Native 2e0f0 submits RGB with register alpha255, folding the
         * palette alpha into the model opacity rather than the TEV input. */
        registers[0][3] = registers[1][3] = 255;
        gc_render_mesh_piece(scene, scene->glyph_cube, matrix, scale, registers, 3,
                             (float)(point->alpha * point->colors[0][3]) / 65025);
    }
    scene->perspective = perspective;
    scene->camera_y = camera_y;
}

static void edit_values(GcScene *scene, const gc_menu *menu) {
    if (!scene->ui)
        return;
    size_t count = gc_value_morph_points(
        &scene->edit_geometry, menu, &scene->edit_state, &scene->value_morph_style,
        &scene->value_morph_state, scene->ui->edit_points, GC_EDIT_POINT_LIMIT);
    edit_points_draw(scene, count);
}

static void draw_disc(GcScene *scene, const gc_menu *menu) {
    gc_render_grid(scene, menu->page_elapsed);
    edit_values(scene, menu);
    /* USA 26adc omits the fatal Disc record. The menu submits its global
     * ERROR4 overlay after dimming the complete page instead. */
    for (unsigned index = 0; index < 5; index++)
        if (index != 1)
            gc_render_layout_native_entry(
                scene, GC_TEXT_DISC, GC_LAYOUT_DISC, index,
                (uint8_t)(scene->disc_text_ticks[index] * 255 / 20));
    const GcDiscMetadata *metadata = gc_disc_metadata(scene->disc, scene->language);
    if (scene->disc_metadata_ticks) {
        uint8_t alpha = (uint8_t)(scene->disc_metadata_ticks * 255 / 20);
        const GcLayoutTable *table =
            gc_layout_table(&scene->layouts, scene->language, GC_LAYOUT_DISC);
        GcLayoutPane banner;
        if (gc_layout_find_pane(table, "bana", 0, &banner))
            gc_render_layout_image(scene, GC_LAYOUT_DISC, &banner, scene->disc_banner,
                                   (CcColor){1, 1, 1, (float)alpha / 255});
        gc_render_layout_string_alpha(
            scene, GC_LAYOUT_DISC, "titl",
            metadata ? metadata->full_title : menu->disc_title, alpha);
        gc_render_layout_string_alpha(
            scene, GC_LAYOUT_DISC, "makr",
            metadata ? metadata->full_company : menu->disc_company, alpha);
        if (metadata)
            gc_render_layout_string_alpha(scene, GC_LAYOUT_DISC, "info",
                                          metadata->description, alpha);
    }
    gc_render_prompts(scene, menu);
}

static void draw_cube(GcScene *scene, const gc_menu *menu) {
    glass_cube(scene, true);
    face_geometry(scene);
    glass_cube(scene, false);
    const GcLayoutGroup groups[5] = {GC_LAYOUT_DISC_FACE, GC_LAYOUT_CALENDAR_FACE,
                                     GC_LAYOUT_CARD_FACE, GC_LAYOUT_OPTIONS_FACE,
                                     GC_LAYOUT_MENU};
    for (unsigned index = 0; index < 5; index++) {
        GcLayoutGroup group = groups[index];
        if (!scene->menu_pose.pane_alpha[index])
            continue;
        face_frames(scene, group);
        face_panes(scene, menu, group);
        if (group == GC_LAYOUT_DISC_FACE && scene->disc_face_ticks[0]) {
            uint8_t alpha = (uint8_t)(scene->disc_face_ticks[0] * 255 / 20);
            const GcDiscMetadata *metadata =
                gc_disc_metadata(scene->disc, scene->language);
            const char *title = metadata ? metadata->game_name : menu->disc_title;
            const char *company = metadata ? metadata->company : menu->disc_company;
            gc_render_layout_string_alpha(scene, group, "zame", title, alpha);
            gc_render_layout_string_alpha(scene, group, "game", title, alpha);
            gc_render_layout_string_alpha(scene, group, "zakr", company, alpha);
            gc_render_layout_string_alpha(scene, group, "makr", company, alpha);
        }
    }
    gc_render_prompts(scene, menu);
}

static void draw_calendar(GcScene *scene, const gc_menu *menu) {
    gc_render_grid(scene, menu->page_elapsed);
    /* USA 27f00 / PAL 2969c draws the three native captions and glyphs.
     * The GLH's bar1/bar2 records share the heading centers, but this page
     * never submits those frames. */
    gc_render_layout_string(
        scene, GC_LAYOUT_CALENDAR, "mesg",
        gc_render_native_text(scene, GC_TEXT_CALENDAR, 0, "Set the calendar."));
    gc_render_layout_string(scene, GC_LAYOUT_CALENDAR, "txt1",
                            gc_render_native_text(scene, GC_TEXT_CALENDAR, 1, "Date"));
    gc_render_layout_string(scene, GC_LAYOUT_CALENDAR, "txt2",
                            gc_render_native_text(scene, GC_TEXT_CALENDAR, 2, "Time"));
    edit_values(scene, menu);
    gc_render_prompts(scene, menu);
}

static void options_captions(GcScene *scene, gc_language language, uint8_t alpha) {
    const GcTextTable *texts =
        gc_text_table(&scene->native_text, language, GC_TEXT_OPTIONS);
    const GcLayoutTable *layouts =
        gc_layout_table(&scene->layouts, language, GC_LAYOUT_OPTIONS);
    if (!alpha)
        return;
    for (unsigned row = 0; row < (scene->edit_geometry.europe ? 4u : 3u); row++) {
        unsigned index = !scene->edit_geometry.europe && row == 2 ? 4 : row;
        GcTextEntry entry;
        GcLayoutText layout;
        if (!gc_text_table_entry(texts, index, &entry) ||
            !gc_layout_text(layouts, entry.flags, &layout))
            continue;
        if (index) {
            layout.color_first = layout.color_second =
                scene->edit_geometry.option_heading_color;
            layout.box.center_y += scene->edit_geometry.option_caption_offsets[row - 1];
        }
        gc_render_layout_text_alpha(&layout, alpha);
        gc_render_layout_text_value(scene, GC_LAYOUT_OPTIONS, &layout, entry.bytes);
    }
}

static void draw_options(GcScene *scene, const gc_menu *menu) {
    gc_render_grid(scene, menu->page_elapsed);
    if (menu->region == GC_REGION_EUROPE) {
        for (unsigned language = 0; language < 6; language++)
            options_captions(scene, (gc_language)language,
                             gc_edit_language_alpha(&scene->edit_state,
                                                    &scene->edit_geometry,
                                                    (gc_language)language, true));
    } else {
        options_captions(scene, scene->language, 255);
    }
    edit_values(scene, menu);
    if (menu->region == GC_REGION_EUROPE) {
        face_panes(scene, menu, GC_LAYOUT_OPTIONS);
    }
    gc_render_prompts(scene, menu);
}

static bool card_same_file(const gc_card_file *first, const gc_card_file *second) {
    return !memcmp(first->game_code, second->game_code, sizeof(first->game_code)) &&
           !memcmp(first->maker_code, second->maker_code, sizeof(first->maker_code)) &&
           !strcmp(first->filename, second->filename);
}

bool gc_scene_card_relayout(GcScene *scene, const gc_menu *menu,
                            const GcCardOperation *operation) {
    if (!scene || !menu || !operation || !scene->card_operation_active ||
        !scene->card_cells.initialized || scene->card_cells.entrance ||
        operation->source_slot > 1 ||
        operation->source_file_index >= GC_CARD_FILE_LIMIT)
        return false;
    float centers[2][16][2];
    if (!card_cell_layout(scene, centers))
        return false;
    const size_t rows[2] = {gc_menu_card_first_row(menu, 0),
                            gc_menu_card_first_row(menu, 1)};
    if (operation->result)
        return gc_card_cells_relayout(&scene->card_cell_style, &scene->card_cells,
                                      centers, rows);
    unsigned source_slot = operation->source_slot;
    bool transfer = operation->action == GC_CARD_ACTION_COPY ||
                    operation->action == GC_CARD_ACTION_MOVE;
    for (unsigned slot = 0; slot < 2; ++slot)
        if (scene->card_operation_cards[slot].file_count > GC_CARD_FILE_LIMIT ||
            menu->cards[slot].file_count > GC_CARD_FILE_LIMIT)
            return false;
    if (transfer &&
        operation->destination_file_index >= menu->cards[source_slot ^ 1u].file_count)
        return false;
    GcCardCell previous[2][GC_CARD_FILE_LIMIT];
    memcpy(previous, scene->card_cells.cells, sizeof(previous));
    /* Native 1f234 moves metadata first, then 1f494 captures every current
     * position and velocity for its forty-tick Hermite relayout. */
    for (unsigned slot = 0; slot < 2; ++slot) {
        const gc_card *old = &scene->card_operation_cards[slot];
        const gc_card *current = &menu->cards[slot];
        for (size_t index = 0; index < current->file_count; ++index) {
            for (size_t source = 0; source < old->file_count; ++source) {
                if (card_same_file(&current->files[index], &old->files[source])) {
                    scene->card_cells.cells[slot][index] = previous[slot][source];
                    break;
                }
            }
        }
    }
    if (transfer) {
        unsigned target_slot = source_slot ^ 1u;
        size_t target_index = operation->destination_file_index;
        GcCardCell *target = &scene->card_cells.cells[target_slot][target_index];
        *target = previous[source_slot][operation->source_file_index];
        size_t first = scene->card_operation_first_rows[source_slot] * GC_CARD_COLUMNS;
        size_t visible = operation->source_file_index - first;
        target->velocity[0] = target->velocity[2] = 0;
        target->velocity[1] = visible < 8 ? 150 : -150;
        target->alpha = target->drawn_alpha = 255;
        /* Native 1f234 seeds max scale/+a=+10 before ordinary deselection. */
        target->selection = target->drawn_selection = 6;
    }
    if (operation->action == GC_CARD_ACTION_MOVE ||
        operation->action == GC_CARD_ACTION_ERASE) {
        size_t empty = menu->cards[source_slot].file_count;
        if (empty < GC_CARD_FILE_LIMIT) {
            GcCardCell *cell = &scene->card_cells.cells[source_slot][empty];
            cell->alpha = cell->drawn_alpha = 0;
            cell->selection = cell->drawn_selection = 0;
        }
    }
    return gc_card_cells_relayout(&scene->card_cell_style, &scene->card_cells, centers,
                                  rows);
}

static void draw_card_point(GcScene *scene, const GcFaceGeometryPoint *point,
                            bool populated, const GcCardTextures *art, size_t file) {
    float camera_y = scene->camera_y;
    scene->camera_y = 0;
    GcMesh *mesh = populated ? scene->card_cover : scene->card_base;
    if (!mesh) {
        scene->camera_y = camera_y;
        return;
    }
    if (populated) {
        mesh->material_mask = (1u << 0) | (1u << 2);
        gc_render_mesh_piece(scene, mesh, point->matrix, point->scale, point->registers,
                             point->register_mask, (float)point->alpha / 255);
        unsigned frame =
            art ? gc_card_art_frame(&art->timing[file], scene->card_ticks) : 0;
        mesh->icon_texture = art ? art->icons[file][frame] : 0;
        mesh->material_mask = 1u << 1;
    }
    /* USA 18a04/1ff00 submits the center plane only for a valid icon. */
    if (!populated || mesh->icon_texture)
        gc_render_mesh_piece(scene, mesh, point->matrix, point->scale, point->registers,
                             point->register_mask, (float)point->alpha / 255);
    mesh->material_mask = 0;
    mesh->icon_texture = 0;
    scene->camera_y = camera_y;
}

static void draw_card_header(GcScene *scene, const gc_menu *menu, unsigned slot) {
    bool erasing = scene->card_operation_active && scene->card_operation.erase_active;
    bool retained = erasing || (scene->card_operation_active &&
                                scene->card_operation.selected_special);
    const gc_card *card =
        retained ? &scene->card_operation_cards[slot] : &menu->cards[slot];
    float text_alpha = scene->text_alpha;
    uint8_t available =
        gc_card_popup_header_alpha(&scene->popup_style, &scene->card_popups, slot);
    uint8_t alpha = gc_card_lighting_header_alpha(&scene->card_lighting_style,
                                                  &scene->card_lighting, slot,
                                                  menu->card_slot, available);
    scene->text_alpha *= (float)alpha / 255;
    gc_render_layout_pane(scene, menu, GC_LAYOUT_CARD, slot ? "texb" : "texa", white);
    gc_render_layout_string(scene, GC_LAYOUT_CARD, slot ? "ftxb" : "ftxa",
                            gc_render_native_text(scene, GC_TEXT_CARD, slot, "Open"));
    if (card->status == GC_CARD_READY) {
        gc_render_layout_frame(scene, GC_LAYOUT_CARD, slot ? "ffrb" : "ffra", 0,
                               UINT32_MAX);
        gc_render_card_number(scene, gc_card_free_blocks(card), slot ? "fnmb" : "fnma");
    }
    scene->text_alpha = text_alpha;
    /* Absence/damage messages use their independent STH faders below.
     * Their original GLH rows distinguish plain txt4/5 from red txt2/3. */
    if (card->status == GC_CARD_UNFORMATTED)
        gc_render_layout_native_entry(scene, GC_TEXT_CARD, GC_LAYOUT_CARD, 39 + slot,
                                      255);
}

static void draw_card_slot(GcScene *scene, const gc_menu *menu, unsigned slot,
                           bool selected_only) {
    bool erasing = scene->card_operation_active && scene->card_operation.erase_active;
    bool retained = erasing || (scene->card_operation_active &&
                                scene->card_operation.selected_special);
    const gc_card *card =
        retained ? &scene->card_operation_cards[slot] : &menu->cards[slot];
    if (card->status != GC_CARD_READY)
        return;
    const GcLayoutTable *layout =
        gc_layout_table(&scene->layouts, scene->language, GC_LAYOUT_CARD);
    unsigned count = erasing ? 16 : GC_CARD_FILE_LIMIT;
    for (unsigned cell = 0; cell < count; cell++) {
        size_t index = gc_menu_card_first_row(menu, slot) * GC_CARD_COLUMNS + cell;
        float center_x, center_y;
        uint8_t alpha = 255;
        unsigned selection_tick;
        uint16_t phase = scene->menu_animation.oscillator_phase;
        if (erasing) {
            char key[5];
            snprintf(key, sizeof(key), "i%c%02u", slot ? 'b' : 'a', cell);
            GcLayoutPane pane;
            if (!gc_layout_find_pane(layout, key, 0, &pane))
                continue;
            index = scene->card_operation_first_rows[slot] * GC_CARD_COLUMNS + cell;
            center_x = pane.box.center_x;
            center_y = pane.box.center_y;
            selection_tick = scene->card_selection_ticks[slot][cell];
        } else {
            index = cell;
            const GcCardCell *motion = &scene->card_cells.cells[slot][index];
            if (!scene->card_cells.initialized || !motion->drawn_alpha)
                continue;
            center_x = motion->drawn_position[0] / 16;
            center_y = motion->drawn_position[1] / 16;
            alpha = motion->drawn_alpha;
            selection_tick = motion->drawn_selection;
            phase = scene->card_cells.drawn_phase;
        }
        if (scene->card_operation_active && slot == scene->card_operation.source_slot &&
            index == scene->card_operation.source_file_index &&
            (scene->card_operation.selected_special ||
             scene->card_operation.erase_active))
            continue;
        bool populated = index < card->file_count;
        bool selected = !scene->card_cells.entrance && menu->card_slot == slot &&
                        gc_menu_card_cursor_index(menu, slot) == index;
        /* Native 202a0 submits the populated selection after ordinary cells.
         * Empty selections remain in the ordinary traversal. */
        if ((populated && selected) != selected_only)
            continue;
        GcFaceGeometryPoint point;
        if (!gc_card_geometry_sample(&scene->face_geometry, &scene->startup, center_x,
                                     center_y, populated, selected, 8192,
                                     selection_tick, phase, alpha, &point))
            continue;
        GcCardTextures *art =
            retained ? scene->card_operation_art[slot] : scene->card_art[slot];
        if (!art)
            art = scene->card_operation_art[slot];
        draw_card_point(scene, &point, populated, art, index);
    }
    if (selected_only)
        return;
    float offset;
    if (!gc_card_arrow_offset(&scene->face_geometry, scene->card_ticks, &offset))
        return;
    for (unsigned direction = 0; direction < 2; direction++) {
        unsigned alpha = scene->card_arrow_alpha[slot][direction];
        if (!alpha)
            continue;
        char name[5] = {'a', 'r', slot ? 'b' : 'a', direction ? 'd' : 'u', 0};
        GcLayoutPane pane;
        if (!gc_layout_find_pane(layout, name, 0, &pane))
            continue;
        pane.box.center_y += direction ? -offset : offset;
        const GcIplImage *image =
            gc_menu_textures_pane(&scene->ui->native, menu, GC_LAYOUT_CARD, &pane);
        gc_render_layout_image(scene, GC_LAYOUT_CARD, &pane,
                               gc_render_ui_image_texture(scene->ui, image),
                               (CcColor){1, 1, 1, (float)(alpha * 255 / 20) / 255});
    }
}

static void draw_card_particles(GcScene *scene) {
    bool operation = scene->card_operation_active && scene->card_operation.erase_active;
    if (!scene->card_erasing && !operation)
        return;
    if (operation) {
        const GcCardOperation *state = &scene->card_operation;
        size_t first =
            scene->card_operation_first_rows[state->source_slot] * GC_CARD_COLUMNS;
        size_t cell = state->source_file_index - first;
        if (cell >= 16)
            return;
        const float *center = scene->card_operation_centers[state->source_slot][cell];
        GcFaceGeometryPoint point;
        if (!gc_card_geometry_erase(
                &scene->face_geometry, &scene->startup, center[0], center[1],
                state->pieces_active ? 0 : state->shrink_fraction, GC_CARD_BLOCK_BYTES,
                scene->menu_animation.oscillator_phase, 255, &point))
            return;
        if (!state->pieces_active) {
            if (state->shrink_fraction > 0)
                draw_card_point(scene, &point, true,
                                scene->card_operation_art[state->source_slot],
                                state->source_file_index);
            return;
        }
        memcpy(scene->card_erase_matrix, point.matrix, sizeof(point.matrix));
    }
    float camera_y = scene->camera_y;
    scene->camera_y = 0;
    for (unsigned piece = 0; piece < 12; piece++) {
        GcFaceGeometryPoint point;
        unsigned tick = operation ? scene->card_operation.piece_ticks[piece]
                                  : scene->card_erase_tick;
        unsigned delay = operation ? scene->card_operation.piece_delays[piece]
                                   : scene->card_erase_delays[piece];
        if (!gc_card_geometry_particle(&scene->face_geometry, scene->card_erase_matrix,
                                       GC_CARD_BLOCK_BYTES, tick, delay,
                                       scene->card_erase_angles[piece], 255, &point) ||
            !point.alpha)
            continue;
        const unsigned materials[2] = {2, 0};
        for (unsigned pass = 0; pass < 2; pass++) {
            scene->card_cover->material_mask = 1u << materials[pass];
            gc_render_mesh_piece(scene, scene->card_cover, point.matrix, point.scale,
                                 point.registers, point.register_mask,
                                 (float)point.alpha / 255);
        }
    }
    scene->card_cover->material_mask = 0;
    scene->camera_y = camera_y;
}

static void card_popup_frame_height(GcLayoutFrame *frame, float height) {
    float top = frame->box.center_y - (float)frame->parameters[3] / 32;
    frame->box.center_y = top + height * 0.5f;
    frame->parameters[3] = (uint16_t)(height * 16);
}

static void draw_card_popup_border(GcScene *scene, unsigned slot,
                                   const GcLayoutFrame *frame, uint8_t alpha) {
    float width = (float)frame->parameters[2] / 16;
    float height = (float)frame->parameters[3] / 16;
    GcCardPopupBorder border;
    if (!gc_card_popup_border(&scene->popup_style, &scene->card_popups, slot,
                              frame->box.center_x - width * 0.5f,
                              frame->box.center_y - height * 0.5f, width, height, alpha,
                              &border) ||
        !(border.color & 255))
        return;
    float half = border.line_width * 0.5f;
    /* GX line width is measured in framebuffer pixels. PAL's 520-line
     * viewport stretches the logical 448-line canvas before display fitting. */
    float line_height = scene->startup.frame_rate == 50 ? border.line_width * 448 / 520
                                                        : border.line_width;
    float half_height = line_height * 0.5f;
    float edges[4][4] = {{border.left - half, border.top - half_height,
                          border.width + border.line_width, line_height},
                         {border.left - half, border.top - half_height,
                          border.line_width, border.height + line_height},
                         {border.left - half, border.top + border.height - half_height,
                          border.width + border.line_width, line_height},
                         {border.left + border.width - half, border.top - half_height,
                          border.line_width, border.height + line_height}};
    CcColor color = gc_render_color_rgba(border.color);
    for (unsigned edge = 0; edge < 4; ++edge) {
        float left = edges[edge][0];
        float top = edges[edge][1];
        float right = left + edges[edge][2];
        float bottom = top + edges[edge][3];
        CcDrawVertex vertices[4] = {{left, top, 0, 0, color},
                                    {right, top, 0, 0, color},
                                    {left, bottom, 0, 0, color},
                                    {right, bottom, 0, 0, color}};
        gc_render_layout_vertices(scene, GC_LAYOUT_CARD, vertices, 0, true);
    }
}

static void draw_card_popup(GcScene *scene, const gc_menu *menu, unsigned slot) {
    const GcCardPopupRecord *record = &scene->card_popups.actions[slot];
    GcCardPopupPose pose;
    if (!record->valid ||
        !gc_card_popup_pose(&scene->popup_style, &scene->card_popups, slot, &pose) ||
        !pose.body_alpha)
        return;
    const GcLayoutTable *table =
        gc_layout_table(&scene->layouts, scene->language, GC_LAYOUT_CARD);
    GcLayoutText text_layout;
    if (!gc_layout_find_text(table, "txt0", 0, &text_layout))
        return;
    float width = 0;
    for (unsigned action = 0; action < 3; action++) {
        const char *label = gc_render_native_text(scene, GC_TEXT_CARD, 2 + action, "");
        width = fmaxf(width, gc_render_layout_line_width(scene, &text_layout, label,
                                                         strlen(label)));
    }
    size_t first = record->first_row * GC_CARD_COLUMNS;
    if (record->file_index < first || record->file_index - first >= 16)
        return;
    unsigned cell = (unsigned)(record->file_index - first);
    GcLayoutCardPopup popup, confirmation;
    if (!gc_layout_card_popup(table, slot, cell, false, record->action, width,
                              text_layout.font_height, &popup) ||
        !gc_layout_card_popup(table, slot, cell, true, record->action, width,
                              text_layout.font_height, &confirmation))
        return;
    float shift = cell / 4 < 2 ? pose.open_offset : -pose.open_offset;
    float body_height = (float)text_layout.font_height +
                        2 * text_layout.line_spacing * pose.row_spacing + 16;
    card_popup_frame_height(&popup.frames[0], body_height);
    popup.frames[0].box.center_y += shift;
    gc_render_layout_frame_value(scene, GC_LAYOUT_CARD, &popup.frames[0],
                                 UINT32_C(0xffffff00) | pose.body_alpha);
    float first_y = popup.rows[0].box.center_y + shift;
    for (unsigned index = 0; index < 3; index++) {
        GcLayoutText row = popup.rows[index];
        row.box.center_y =
            first_y + index * text_layout.line_spacing * pose.row_spacing;
        unsigned entry = popup.text_entries[index];
        bool selected = index == (unsigned)record->action;
        uint8_t opacity = selected ? pose.body_alpha : pose.other_rows_alpha;
        const gc_card *card = &menu->cards[slot];
        bool disabled = entry < 4 && (record->file_index >= card->file_count ||
                                      menu->cards[slot ^ 1].status != GC_CARD_READY);
        uint32_t foreground = scene->popup_colors[0];
        if (disabled) {
            /* Original action rows use (70+availability/2)/(70+127). */
            foreground = (((foreground >> 24) * 70 / 197) << 24) |
                         ((((foreground >> 16) & 255) * 70 / 197) << 16) |
                         ((((foreground >> 8) & 255) * 70 / 197) << 8) | 255;
        }
        const char *label = gc_render_native_text(scene, GC_TEXT_CARD, entry, "");
        if (selected && !scene->card_popups.confirmation_ticks[slot])
            gc_render_layout_highlight_value(scene, GC_LAYOUT_CARD, &row, label,
                                             foreground, scene->popup_colors[1],
                                             opacity);
        else {
            row.color_first = row.color_second =
                gc_render_layout_color_alpha(foreground, opacity);
            gc_render_layout_text_value(scene, GC_LAYOUT_CARD, &row, label);
        }
    }
    unsigned remaining = scene->popup_style.confirmation_duration -
                         scene->card_popups.confirmation_ticks[slot];
    uint8_t border_alpha = (uint8_t)(pose.body_alpha * remaining /
                                     scene->popup_style.confirmation_duration);
    draw_card_popup_border(scene, slot, &popup.frames[0], border_alpha);
    if (!pose.choices_alpha)
        return;
    float choice_shift =
        body_height - ((float)text_layout.font_height + 16) + pose.choices_offset;
    confirmation.frames[1].box.center_y += choice_shift;
    gc_render_layout_frame_value(scene, GC_LAYOUT_CARD, &confirmation.frames[1],
                                 UINT32_C(0xffffff00) | pose.choices_alpha);
    draw_card_popup_border(scene, slot, &confirmation.frames[1], pose.choices_alpha);
    for (unsigned index = 0; index < 2; index++) {
        GcLayoutText row = confirmation.rows[1 + index];
        row.box.center_y += choice_shift;
        const char *label = gc_render_native_text(scene, GC_TEXT_CARD, 5 + index, "");
        if (index == (record->confirm_yes ? 0u : 1u))
            gc_render_layout_highlight_value(
                scene, GC_LAYOUT_CARD, &row, label, scene->popup_colors[0],
                scene->popup_colors[1], pose.choices_alpha);
        else {
            row.color_first = row.color_second = gc_render_layout_color_alpha(
                scene->popup_colors[0], pose.choices_alpha);
            gc_render_layout_text_value(scene, GC_LAYOUT_CARD, &row, label);
        }
    }
}

static void draw_card_format(GcScene *scene, unsigned slot, unsigned stage) {
    uint8_t body_alpha = gc_card_popup_format_alpha(
        &scene->popup_style, &scene->card_popups, slot, stage, false);
    if (!body_alpha)
        return;
    uint8_t choices_alpha = gc_card_popup_format_alpha(
        &scene->popup_style, &scene->card_popups, slot, stage, true);
    const GcLayoutTable *layouts =
        gc_layout_table(&scene->layouts, scene->language, GC_LAYOUT_CARD);
    const GcTextTable *texts =
        gc_text_table(&scene->native_text, scene->language, GC_TEXT_CARD);
    GcTextEntry body, choices[2];
    GcLayoutText source;
    GcLayoutDialog dialog;
    if (!gc_text_table_entry(texts, 39 + 2 * stage + slot, &body) ||
        !gc_text_table_entry(texts, 5, &choices[0]) ||
        !gc_text_table_entry(texts, 6, &choices[1]) ||
        !gc_layout_text(layouts, body.flags, &source))
        return;
    float body_width, body_height, choice_width = 0, choice_height = 0;
    gc_render_layout_text_measure(scene, &source, body.bytes, &body_width,
                                  &body_height);
    for (unsigned index = 0; index < 2; index++) {
        float width, height;
        gc_render_layout_text_measure(scene, &source, choices[index].bytes, &width,
                                      &height);
        choice_width = fmaxf(choice_width, width);
        choice_height = fmaxf(choice_height, height);
    }
    if (!gc_layout_confirmation_dialog(layouts, &source, body_width, body_height,
                                       choice_width, choice_height, &dialog))
        return;
    for (unsigned index = 0; index < 2; index++)
        gc_render_layout_frame_value(scene, GC_LAYOUT_CARD, &dialog.frames[index],
                                     UINT32_C(0xffffff00) |
                                         (index ? choices_alpha : body_alpha));
    GcLayoutText row = dialog.rows[0];
    gc_render_layout_text_alpha(&row, body_alpha);
    gc_render_layout_text_value(scene, GC_LAYOUT_CARD, &row, body.bytes);
    for (unsigned index = 0; index < 2; index++) {
        row = dialog.rows[1 + index];
        if (index == (scene->card_popups.format_yes[slot][stage] ? 0u : 1u))
            gc_render_layout_highlight_value(
                scene, GC_LAYOUT_CARD, &row, choices[index].bytes,
                scene->popup_colors[0], scene->popup_colors[1], choices_alpha);
        else {
            row.color_first = row.color_second =
                gc_render_layout_color_alpha(scene->popup_colors[0], choices_alpha);
            gc_render_layout_text_value(scene, GC_LAYOUT_CARD, &row,
                                        choices[index].bytes);
        }
    }
}

static void draw_card_local_error(GcScene *scene, uint8_t alpha) {
    if (!alpha)
        return;
    /* Local file-write failures have no original console message. Retain the
     * native card page and its central message geometry for this host error. */
    GcLayoutText layout;
    GcLayoutFrame frame;
    const GcLayoutTable *table =
        gc_layout_table(&scene->layouts, scene->language, GC_LAYOUT_CARD);
    const char *message = gc_menu_message_text(GC_MESSAGE_CARD_SAVE_FAILED);
    if (gc_layout_find_text(table, "txt1", 0, &layout)) {
        if (gc_layout_frame(table, layout.frame_index, &frame)) {
            float width, height;
            gc_render_layout_text_measure(scene, &layout, message, &width, &height);
            if (width <= 4000 && height <= 4000) {
                frame.parameters[2] = (uint16_t)((width + 16) * 16);
                frame.parameters[3] = (uint16_t)((height + 16) * 16);
                gc_render_layout_frame_value(scene, GC_LAYOUT_CARD, &frame,
                                             UINT32_C(0xffffff00) | alpha);
            }
        }
        gc_render_layout_text_alpha(&layout, alpha);
        gc_render_layout_text_value(scene, GC_LAYOUT_CARD, &layout, message);
    }
}

static void draw_card_overlays(GcScene *scene, const gc_menu *menu) {
    /* USA 1a994 / PAL 1b94c submit slot status notices before the action
     * popups and the final operation-message pass. Array index is not Z order. */
    for (unsigned pass = 0; pass < 2; pass++) {
        unsigned slot = menu->card_slot ^ (pass ^ 1u);
        for (unsigned kind = 0; kind < 2; kind++) {
            unsigned entry = 33 + slot + 2 * kind;
            uint8_t alpha = gc_card_popup_message_alpha(&scene->popup_style,
                                                        &scene->card_popups, entry);
            gc_render_layout_native_entry(scene, GC_TEXT_CARD, GC_LAYOUT_CARD, entry,
                                          alpha);
        }
        for (unsigned stage = 0; stage < 2; stage++)
            draw_card_format(scene, slot, stage);
    }
    for (unsigned slot = 0; slot < 2; slot++)
        draw_card_popup(scene, menu, slot);
    for (unsigned entry = 0; entry < GC_CARD_POPUP_MESSAGE_COUNT - 1; entry++) {
        if (entry >= 33 && entry <= 36)
            continue;
        uint8_t alpha = gc_card_popup_message_alpha(&scene->popup_style,
                                                    &scene->card_popups, entry);
        if (alpha)
            gc_render_layout_native_entry(scene, GC_TEXT_CARD, GC_LAYOUT_CARD, entry,
                                          alpha);
    }
    draw_card_local_error(
        scene, gc_card_popup_message_alpha(&scene->popup_style, &scene->card_popups,
                                           GC_CARD_POPUP_MESSAGE_COUNT - 1));
}

static void draw_cards(GcScene *scene, const gc_menu *menu) {
    const GcLayoutTable *layout =
        gc_layout_table(&scene->layouts, scene->language, GC_LAYOUT_CARD);
    GcCardGridLighting lighting;
    if (gc_card_lighting_grid(&scene->card_lighting_style, &scene->card_lighting,
                              &scene->face_geometry, layout, &lighting))
        gc_render_grid_lights(scene, &lighting);
    /* USA 1a994 / PAL 1b94c always submit the blue detail footer, even
     * with no READY slot or when the cursor selects an empty block. */
    gc_render_layout_frame(scene, GC_LAYOUT_CARD, "mes6", 0, UINT32_MAX);
    /* PAL 19bc4 visits inactive headers first. Native 202a0 postpones the
     * populated selection until both ordinary cell passes have finished. */
    draw_card_header(scene, menu, menu->card_slot ^ 1u);
    draw_card_header(scene, menu, menu->card_slot);
    draw_card_slot(scene, menu, menu->card_slot, false);
    draw_card_slot(scene, menu, menu->card_slot ^ 1u, false);
    draw_card_slot(scene, menu, menu->card_slot, true);
    if (scene->card_operation_active && scene->card_operation.selected_special &&
        !scene->card_operation.erase_active) {
        unsigned slot = scene->card_operation.source_slot;
        draw_card_point(scene, &scene->card_operation_point, true,
                        scene->card_operation_art[slot],
                        scene->card_operation.source_file_index);
    }
    const gc_card_file *file = gc_menu_card_selected(menu);
    unsigned file_slot = menu->card_slot;
    size_t file_index = gc_menu_card_cursor_index(menu, file_slot);
    bool operation = scene->card_operation_active &&
                     scene->card_operation.action != GC_CARD_ACTION_FORMAT;
    if (operation) {
        file_slot = scene->card_operation.source_slot;
        file_index = scene->card_operation.source_file_index;
        const gc_card *old = &scene->card_operation_cards[file_slot];
        file = file_index < old->file_count ? &old->files[file_index] : NULL;
    }
    if (file) {
        gc_render_layout_frame(scene, GC_LAYOUT_CARD, "frmc", 0, UINT32_MAX);
        GcCardTextures *art = operation ? scene->card_operation_art[file_slot]
                                        : scene->card_art[file_slot];
        uint32_t banner_texture = art ? art->banners[file_index] : 0;
        if (banner_texture) {
            GcLayoutPane pane;
            const GcLayoutTable *table =
                gc_layout_table(&scene->layouts, scene->language, GC_LAYOUT_CARD);
            if (gc_layout_find_pane(table, "bana", 0, &pane))
                gc_render_layout_image(scene, GC_LAYOUT_CARD, &pane, banner_texture,
                                       white);
        }
        gc_render_layout_string(scene, GC_LAYOUT_CARD, "titl", file->title);
        gc_render_layout_string(scene, GC_LAYOUT_CARD, "info", file->comment);
        gc_render_card_number(scene, file->blocks, "nmc1");
        GcLayoutPane icon;
        const GcLayoutTable *layout =
            gc_layout_table(&scene->layouts, scene->language, GC_LAYOUT_CARD);
        if (art && gc_layout_find_pane(layout, "ic00", 0, &icon)) {
            unsigned frame =
                gc_card_art_frame(&art->timing[file_index], scene->card_ticks);
            gc_render_layout_image(scene, GC_LAYOUT_CARD, &icon,
                                   art->icons[file_index][frame], white);
        }
    }
    draw_card_particles(scene);
    gc_render_prompts(scene, menu);
    draw_card_overlays(scene, menu);
}

static void draw_message(GcScene *scene, const gc_menu *menu) {
    if (menu->message_return_page == GC_PAGE_CARDS)
        draw_cards(scene, menu);
    else
        gc_render_grid(scene, menu->page_elapsed);
    GcTextGroup group;
    unsigned index;
    if (gc_text_message_index(menu, &group, &index)) {
        if (group == GC_TEXT_CARD)
            return;
        gc_render_layout_native_entry(
            scene, group, group == GC_TEXT_CARD ? GC_LAYOUT_CARD : GC_LAYOUT_DISC,
            index, 255);
        return;
    }
}

static void full_page_layers(GcScene *scene) {
    if (!scene->page_snapshots)
        return;
    GcEditState current_editor = scene->edit_state;
    GcValueMorphState current_morph = scene->value_morph_state;
    for (unsigned group = 0; group < GC_TRANSITION_GROUP_COUNT; group++) {
        if (!scene->page_snapshots->saved[group] ||
            !gc_page_transition_visible(&scene->page_style, &scene->page_transitions,
                                        (GcTransitionGroup)group))
            continue;
        const gc_menu *menu = &scene->page_snapshots->menus[group];
        scene->edit_state = scene->page_snapshots->editors[group];
        scene->value_morph_state = scene->page_snapshots->morphs[group];
        scene->value_alpha = (float)gc_page_transition_alpha(
                                 &scene->page_style, &scene->page_transitions,
                                 (GcTransitionGroup)group, GC_TRANSITION_VALUES) /
                             255;
        scene->grid_alpha = (float)gc_page_transition_alpha(
                                &scene->page_style, &scene->page_transitions,
                                (GcTransitionGroup)group, GC_TRANSITION_GRID) /
                            255;
        scene->text_alpha = (float)gc_page_transition_alpha(
                                &scene->page_style, &scene->page_transitions,
                                (GcTransitionGroup)group, GC_TRANSITION_TEXT) /
                            255;
        switch (menu->page) {
            case GC_PAGE_CALENDAR:
                draw_calendar(scene, menu);
                break;
            case GC_PAGE_OPTIONS:
                draw_options(scene, menu);
                break;
            case GC_PAGE_DISC:
                draw_disc(scene, menu);
                break;
            case GC_PAGE_MESSAGE:
                draw_message(scene, menu);
                break;
            default:
                draw_cards(scene, menu);
                break;
        }
    }
    scene->edit_state = current_editor;
    scene->value_morph_state = current_morph;
    scene->value_alpha = scene->grid_alpha = scene->text_alpha = 1;
}

static void startup_trails(GcScene *scene, const GcStartupPose *pose) {
    static const unsigned order[4] = {0, 1, 3, 2};
    /* Native USA/JAP 0x81310198 and EUR 0x81310ad0 pair cell X with V
     * and cell Y with U. The uploaded tile contains the full mirrored
     * 0..2 mask period, so its coordinates are normalized here. */
    const float uv[4][2] = {{0, 0}, {0, 1}, {1, 0}, {1, 1}};
    for (size_t index = 0; index < pose->trail_count; index++) {
        const GcStartupTrail *trail = &pose->trails[index];
        CcColor color = {(float)scene->startup.trail_color[0] / 255,
                         (float)scene->startup.trail_color[1] / 255,
                         (float)scene->startup.trail_color[2] / 255,
                         (float)trail->alpha / 255};
        CcDrawVertex vertices[4];
        for (unsigned vertex = 0; vertex < 4; vertex++) {
            float point[3];
            gc_startup_transform(pose->scene_matrix, trail->positions[order[vertex]],
                                 point);
            vertices[vertex] = (CcDrawVertex){
                322.18f + point[0] * (592.0f / 588.0f) + scene->display_offset_x,
                240 - (point[1] - scene->camera_y) * scene->pixel_scale_y,
                uv[vertex][0], uv[vertex][1], color};
        }
        cc_platform_draw_vertices(scene->platform, vertices, scene->trail_texture);
    }
}

static void boot_ui(GcScene *scene) {
    if (!scene->boot_control)
        return;
    GcBootUi ui;
    gc_boot_control_ui(scene->boot_control, &ui);
    const GcLayoutTable *layouts =
        gc_layout_table(&scene->layouts, scene->language, GC_LAYOUT_ERROR);
    const GcTextTable *texts =
        gc_text_table(&scene->native_text, scene->language, GC_TEXT_ERROR);
    if (ui.reset_alpha || ui.choices_alpha) {
        GcLayoutText source;
        GcTextEntry body, choices[2];
        GcLayoutDialog dialog;
        if (gc_text_table_entry(texts, 0, &body) &&
            gc_text_table_entry(texts, 1, &choices[0]) &&
            gc_text_table_entry(texts, 2, &choices[1]) &&
            gc_layout_text(layouts, body.flags, &source)) {
            float body_width, body_height, choice_width = 0, choice_height = 0;
            gc_render_layout_text_measure(scene, &source, body.bytes, &body_width,
                                          &body_height);
            for (unsigned index = 0; index < 2; index++) {
                float width, height;
                gc_render_layout_text_measure(scene, &source, choices[index].bytes,
                                              &width, &height);
                choice_width = fmaxf(choice_width, width);
                choice_height = fmaxf(choice_height, height);
            }
            if (gc_layout_boot_dialog(layouts, body_width, body_height, choice_width,
                                      choice_height, &dialog)) {
                gc_render_layout_frame_value(scene, GC_LAYOUT_ERROR, &dialog.frames[0],
                                             UINT32_C(0xffffff00) | ui.reset_alpha);
                gc_render_layout_frame_value(scene, GC_LAYOUT_ERROR, &dialog.frames[1],
                                             UINT32_C(0xffffff00) | ui.choices_alpha);
                GcLayoutText row = dialog.rows[0];
                gc_render_layout_text_alpha(&row, ui.reset_alpha);
                gc_render_layout_text_value(scene, GC_LAYOUT_ERROR, &row, body.bytes);
                for (unsigned index = 0; index < 2; index++) {
                    row = dialog.rows[index + 1];
                    if (index == ui.reset_choice)
                        gc_render_layout_highlight_value(
                            scene, GC_LAYOUT_ERROR, &row, choices[index].bytes,
                            scene->popup_colors[0], scene->popup_colors[1],
                            ui.choices_alpha);
                    else {
                        row.color_first = row.color_second =
                            gc_render_layout_color_alpha(scene->popup_colors[0],
                                                         ui.choices_alpha);
                        gc_render_layout_text_value(scene, GC_LAYOUT_ERROR, &row,
                                                    choices[index].bytes);
                    }
                }
            }
        }
    }
    if (ui.language_alpha && ui.language_selection < 6) {
        GcLayoutText source;
        GcLayoutDialog dialog;
        if (gc_layout_find_text(layouts, "lan1", 0, &source)) {
            float maximum_width = 0, maximum_height = 0;
            for (unsigned index = 0; index < 6; index++) {
                GcTextEntry entry;
                if (!gc_text_table_entry(texts, index + 12, &entry))
                    continue;
                float width, height;
                gc_render_layout_text_measure(scene, &source, entry.bytes, &width,
                                              &height);
                maximum_width = fmaxf(maximum_width, width);
                maximum_height = fmaxf(maximum_height, height);
            }
            if (gc_layout_language_dialog(layouts, maximum_width, maximum_height,
                                          &dialog)) {
                gc_render_layout_frame_value(scene, GC_LAYOUT_ERROR, &dialog.frames[0],
                                             UINT32_C(0xffffff00) | ui.language_alpha);
                for (unsigned index = 0; index < 6; index++) {
                    GcTextEntry entry;
                    if (!gc_text_table_entry(texts, index + 12, &entry))
                        continue;
                    GcLayoutText row = dialog.rows[index];
                    if (index == ui.language_selection)
                        gc_render_layout_highlight_value(
                            scene, GC_LAYOUT_ERROR, &row, entry.bytes,
                            scene->popup_colors[0], scene->popup_colors[1],
                            ui.language_alpha);
                    else {
                        row.color_first = row.color_second =
                            gc_render_layout_color_alpha(scene->popup_colors[0],
                                                         ui.language_alpha);
                        gc_render_layout_text_value(scene, GC_LAYOUT_ERROR, &row,
                                                    entry.bytes);
                    }
                }
            }
        }
        gc_render_layout_native_entry(scene, GC_TEXT_ERROR, GC_LAYOUT_ERROR,
                                      6 + ui.language_selection, ui.language_alpha);
    }
    gc_render_layout_native_entry(scene, GC_TEXT_ERROR, GC_LAYOUT_ERROR, 3,
                                  ui.notice_alpha);
    gc_render_layout_native_entry(scene, GC_TEXT_ERROR, GC_LAYOUT_ERROR, 4,
                                  ui.error_alpha);
    gc_render_layout_native_entry(scene, GC_TEXT_ERROR, GC_LAYOUT_ERROR, 5,
                                  ui.unrecognized_alpha);
}

static void startup_menu_labels(GcScene *scene, const gc_menu *menu,
                                const GcStartupPose *pose) {
    if (!pose->menu_labels_alpha)
        return;
    float *pane = scene->menu_pose.pane_matrices[GC_MENU_ANIMATION_HOME_PANE];
    const float *cube = pose->glass_matrix;
    for (unsigned row = 0; row < 3; row++) {
        pane[row * 4] = cube[row * 4];
        pane[row * 4 + 1] = -cube[row * 4 + 1];
        pane[row * 4 + 2] = cube[row * 4 + 2];
        pane[row * 4 + 3] = cube[row * 4 + 3] - 144 * cube[row * 4] +
                            144 * cube[row * 4 + 1] + 144 * cube[row * 4 + 2];
    }
    /* Native 0x81312448 fades the four home labels over the final 100
     * transition ticks. This is the MENU pane, not a wordmark sprite. */
    scene->menu_pose.pane_alpha[GC_MENU_ANIMATION_HOME_PANE] = pose->menu_labels_alpha;
    scene->menu_pose.frame_alpha[GC_MENU_ANIMATION_HOME_PANE] = pose->menu_labels_alpha;
    face_panes(scene, menu, GC_LAYOUT_MENU);
}

static void draw_startup(GcScene *scene, const gc_menu *menu) {
    const float identity[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    GcStartupPose pose;
    double ticks = menu->startup_elapsed * scene->startup.frame_rate;
    if (!isfinite(ticks) || ticks < 0)
        return;
    /* After menu entry, native idle motion repeats its 16-bit angle phase.
     * Preserve that cycle for captures without narrowing an unbounded time. */
    unsigned tick =
        ticks <= scene->startup.menu_ticks
            ? (unsigned)ticks
            : scene->startup.menu_ticks +
                  (unsigned)fmod(floor(ticks) - scene->startup.menu_ticks, 65536);
    scene->ui_ticks = scene->boot_control ? scene->boot_control->video_tick : tick;
    bool sampled = scene->boot_config && scene->boot_control
                       ? gc_boot_control_sample_pose(scene->boot_config,
                                                     scene->boot_control, &pose)
                       : gc_startup_sample_menu(&scene->startup, tick, &pose);
    if (!sampled)
        return;
    scene->perspective = pose.perspective;
    scene->camera_y = -55;
    if (pose.perspective) {
        /* Native 0x81301154 computes the camera before e41c. On entry
         * 02f08 still sees the drawing phase and uses -55. Later frames
         * see the prior transition count, initialized to 1 by bcd8. */
        unsigned transition = scene->boot_control
                                  ? scene->boot_control->transition_tick
                                  : tick - (scene->startup.menu_ticks -
                                            scene->startup.transition_ticks[0] -
                                            scene->startup.transition_ticks[1] -
                                            scene->startup.transition_ticks[2]);
        if (scene->boot_control) {
            if (transition <= 1)
                transition = 0;
        } else if (transition) {
            ++transition;
        }
        float fraction =
            (float)transition / (float)scene->startup.transition_fade_ticks;
        scene->camera_y += 45 * fminf(1, fraction);
    }
    float center_y = 240 + scene->camera_y * scene->pixel_scale_y;
    float model_scale[12] = {0}, glass_scale[12] = {0}, cover_scale[12] = {0};
    for (unsigned axis = 0; axis < 3; axis++) {
        model_scale[axis * 5] = pose.model_scale[axis];
        glass_scale[axis * 5] = pose.glass_scale[axis];
        cover_scale[axis * 5] = pose.model_scale[axis] * 1.01f;
    }
    /* Native 0x8130d3b8 draws the logotype first, then splits the cover
     * behind and in front of the trails, moving cube and completed mark. */
    if (scene->logotype) {
        gc_ipl_animation_apply(&scene->logotype_joints, pose.logotype_frame,
                               &scene->logotype->model);
        gc_ipl_animation_apply(&scene->logotype_colors, pose.logotype_frame,
                               &scene->logotype->model);
        gc_render_mesh_draw(scene, scene->logotype, identity, identity, 322.18f,
                            center_y, (float)pose.logotype_alpha / 255);
    }
    if (pose.base_cube_alpha)
        gc_render_mesh_draw(scene, scene->boot_base, pose.scene_matrix, model_scale,
                            322.18f, center_y, (float)pose.base_cube_alpha / 255);
    if (pose.cover_cube_alpha) {
        scene->boot_cover->material_mask = 1u << 1;
        gc_render_mesh_draw(scene, scene->boot_cover, pose.scene_matrix, cover_scale,
                            322.18f, center_y, (float)pose.cover_cube_alpha / 255);
    }
    /* Native 0x813104ec draws every trail before submitting the opaque
     * moving cube. Its material disables depth, so reversing this order
     * lets trails overwrite the cube and falsely appear through it. */
    startup_trails(scene, &pose);
    if (pose.moving_cube_alpha) {
        float cube_scene[12];
        memcpy(cube_scene, pose.scene_matrix, sizeof(cube_scene));
        cube_scene[7] += pose.moving_cube_world_y;
        gc_render_mesh_draw(scene, scene->moving_cube, cube_scene, pose.cube_matrix,
                            322.18f, center_y, (float)pose.moving_cube_alpha / 255);
    }
    if (pose.boot_mark_alpha)
        gc_render_mesh_draw(scene, scene->boot_mark, pose.scene_matrix, model_scale,
                            322.18f, center_y, (float)pose.boot_mark_alpha / 255);
    if (pose.cover_cube_alpha) {
        scene->boot_cover->material_mask = 1u << 0;
        gc_render_mesh_draw(scene, scene->boot_cover, pose.scene_matrix, cover_scale,
                            322.18f, center_y, (float)pose.cover_cube_alpha / 255);
        scene->boot_cover->material_mask = 0;
    }
    if (pose.glass_cube_alpha)
        gc_render_mesh_draw(scene, scene->menu_cube, pose.glass_matrix, glass_scale,
                            322.18f, center_y, (float)pose.glass_cube_alpha / 255);
    startup_menu_labels(scene, menu, &pose);
    boot_ui(scene);
}

void gc_scene_set_boot(GcScene *scene, const GcBootConfig *config,
                       const GcBootControl *control) {
    if (!scene)
        return;
    scene->boot_config = config;
    scene->boot_control = control;
}

void gc_scene_set_card_random(GcScene *scene, GcCardCellsRandom adapter,
                              void *context) {
    if (!scene)
        return;
    scene->card_random = adapter;
    scene->card_random_context = context;
}

bool gc_scene_can_press(const GcScene *scene, const gc_menu *menu, gc_button button) {
    GcMenuAnimationPose pose;
    if (!scene || !menu || button < GC_BUTTON_UP || button > GC_BUTTON_START)
        return false;
    if (scene->fatal_error_latched || menu->disc_status == GC_DISC_FATAL)
        return false;
    if (menu->page == GC_PAGE_STARTUP || menu->page == GC_PAGE_GAME_STARTED)
        return true;
    bool face = menu->page != GC_PAGE_CUBE;
    bool focused = face && menu->page != GC_PAGE_FACE;
    if (!gc_menu_animation_sample(&scene->startup, &scene->menu_animation, face,
                                  menu->face, focused, &pose))
        return false;
    /* Original 0x81310d80, with the native +0x32 debug override left zero. */
    if (menu->page == GC_PAGE_CUBE)
        return pose.rotation_complete;
    if (menu->page == GC_PAGE_FACE)
        return pose.rotation_complete && pose.focus_complete;
    if (menu->page == GC_PAGE_CALENDAR || menu->page == GC_PAGE_OPTIONS ||
        menu->page == GC_PAGE_DISC) {
        /* 0x813283d0(0) compares the CAFD counter with its configured duration.
         * The controller updates precede drawing; retain the post-update counter. */
        if (!menu->editing &&
            (!scene->edit_state.active || !scene->edit_state.ready ||
             scene->edit_state.page != menu->page ||
             scene->edit_state.entrance_counter < scene->edit_geometry.entrance_ticks))
            return false;
        return button != GC_BUTTON_CANCEL || menu->editing || pose.focus_complete;
    }
    int group = gc_page_transition_group(menu);
    if (group == GC_TRANSITION_CARD) {
        /* USA 157a0 permits direction repeats after the initial entrance,
         * retargeting a moving scroll. Only A/B use the all-cell gate. */
        if (menu->page == GC_PAGE_CARDS && button >= GC_BUTTON_UP &&
            button <= GC_BUTTON_RIGHT)
            return scene->card_cells.initialized && !scene->card_cells.entrance;
        /* Native 0x81314070 rejects every channel at alpha <=172. */
        for (unsigned channel = 0; channel < GC_TRANSITION_CHANNEL_COUNT; ++channel)
            if (gc_page_transition_alpha(&scene->page_style, &scene->page_transitions,
                                         GC_TRANSITION_CARD,
                                         (GcTransitionChannel)channel) <= 172)
                return false;
        const bool ready[2] = {menu->cards[0].status == GC_CARD_READY,
                               menu->cards[1].status == GC_CARD_READY};
        for (unsigned slot = 0; slot < 2; ++slot)
            if (gc_menu_card_first_row(menu, slot) !=
                scene->card_cells.first_rows[slot])
                return false;
        if (scene->card_cells.entrance ||
            !gc_card_cells_ready(&scene->card_cells, ready))
            return false;
        if (scene->card_operation_active && !gc_scene_card_operation_ready(scene))
            return false;
        if (!gc_card_popup_can_press(&scene->popup_style, &scene->card_popups, menu))
            return false;
        return button != GC_BUTTON_CANCEL || pose.focus_complete;
    }
    return true;
}

void gc_scene_frame_counter(GcScene *scene, uint64_t counter) {
    if (!scene)
        return;
    scene->frame_counter_enabled = true;
    scene->frame_counter = counter;
}

static void presentation_overlay(GcScene *scene) {
    if ((scene->frame_counter_enabled || scene->test_error_alpha) &&
        scene->inspection_fade_alpha) {
        CcQuad fade = {.width = 640,
                       .height = 480,
                       .color = {0, 0, 0, (float)scene->inspection_fade_alpha / 255}};
        cc_platform_draw_quad(scene->platform, &fade);
    }
    uint8_t error_alpha = scene->test_error_alpha;
    uint8_t fatal_alpha = (uint8_t)(scene->fatal_error_ticks * 255 / 20);
    if (fatal_alpha > error_alpha)
        error_alpha = fatal_alpha;
    if (error_alpha) {
        CcQuad dim = {.width = 640,
                      .height = 480,
                      .color = {0, 0, 0, (float)(error_alpha / 2) / 255}};
        cc_platform_draw_quad(scene->platform, &dim);
        gc_render_layout_native_entry(scene, GC_TEXT_ERROR, GC_LAYOUT_ERROR, 4,
                                      error_alpha);
    }
    if (!scene->frame_counter_enabled)
        return;
    char label[40];
    int written =
        snprintf(label, sizeof(label), "Frame %" PRIu64, scene->frame_counter);
    if (written <= 0 || (size_t)written >= sizeof(label))
        return;
    float x = 8, y = 8;
    for (int index = 0; index < written; ++index) {
        GcFontGlyph glyph;
        if (!gc_font_glyph(&scene->font, GC_TEXT_LATIN1,
                           (uint32_t)(unsigned char)label[index], &glyph))
            continue;
        float scale = 14.0f / (float)glyph.cell.height;
        float width = (float)glyph.cell.width * scale;
        float u0 = (float)glyph.cell.x / (float)glyph.atlas_width;
        float v0 = (float)glyph.cell.y / (float)glyph.atlas_height;
        float u1 = (float)(glyph.cell.x + glyph.cell.width) / (float)glyph.atlas_width;
        float v1 =
            (float)(glyph.cell.y + glyph.cell.height) / (float)glyph.atlas_height;
        for (unsigned pass = 0; pass < 2; ++pass) {
            float offset = pass ? 0 : 1;
            CcColor color = pass ? white : (CcColor){0, 0, 0, 1};
            CcDrawVertex vertices[4] = {
                {x + offset, y + offset, u0, v0, color},
                {x + width + offset, y + offset, u1, v0, color},
                {x + offset, y + 14 + offset, u0, v1, color},
                {x + width + offset, y + 14 + offset, u1, v1, color}};
            cc_platform_draw_vertices(scene->platform, vertices, scene->font_texture);
        }
        x += (float)glyph.cell.advance * scale;
    }
}

void gc_scene_draw_wait(GcScene *scene) {
    if (!scene || !scene->platform)
        return;
    cc_platform_begin(scene->platform, (CcColor){0, 0, 0, 1});
    presentation_overlay(scene);
    cc_platform_end(scene->platform);
}

void gc_scene_draw(GcScene *scene, const gc_menu *menu) {
    if (!scene || !menu || !scene->font_texture)
        return;
    /* PAL changes the global resource language only on confirmation. The
     * Options caption below has its own candidate-language crossfade. */
    bool language_preview =
        menu->page == GC_PAGE_OPTIONS && menu->editing && menu->editor_index == 2;
    scene->language = language_preview ? menu->settings_before_edit.language
                                       : menu->settings.language;
    scene->encoding =
        scene->language == GC_LANGUAGE_JAPANESE ? GC_TEXT_SHIFT_JIS : GC_TEXT_LATIN1;
    /* Active native viewports are 592x448 in a 480-line NTSC frame and
     * 592x520 in a 576-line PAL frame. Preserve overscan borders while
     * fitting either signal to the common 640x480 presentation canvas. */
    scene->pixel_scale_y =
        menu->region == GC_REGION_EUROPE ? 520.0f / 448 * (480.0f / 576) : 1;
    scene->perspective = false;
    scene->camera_y = -10;
    scene->display_offset_x = (float)menu->settings.screen_position;
    scene->value_alpha = scene->grid_alpha = scene->text_alpha = 1;
    scene->help_drawn = true;
    if (menu->page != GC_PAGE_STARTUP)
        menu_animation_sync(scene, menu);
    cc_platform_begin(scene->platform, (CcColor){0, 0, 0, 1});
    if (menu->page == GC_PAGE_STARTUP) {
        draw_startup(scene, menu);
    } else if (menu->page != GC_PAGE_GAME_STARTED) {
        draw_cube(scene, menu);
        full_page_layers(scene);
        if (menu->page == GC_PAGE_MESSAGE && gc_page_transition_group(menu) < 0)
            draw_message(scene, menu);
        scene->help_drawn = false;
        gc_render_prompts(scene, menu);
    }
    presentation_overlay(scene);
    cc_platform_end(scene->platform);
}
