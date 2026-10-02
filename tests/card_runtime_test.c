#include "gamecube/card_runtime.h"
#include "software.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *inputs[2] = {"runtime-input-a.raw", "runtime-input-b.raw"};
static const char *states[2] = {"runtime-state-a.raw", "runtime-state-b.raw"};

static void put16(uint8_t *bytes, unsigned value) {
    bytes[0] = (uint8_t)(value >> 8);
    bytes[1] = (uint8_t)value;
}

static void put32(uint8_t *bytes, uint32_t value) {
    bytes[0] = (uint8_t)(value >> 24);
    bytes[1] = (uint8_t)(value >> 16);
    bytes[2] = (uint8_t)(value >> 8);
    bytes[3] = (uint8_t)value;
}

static void checksum(uint8_t *bytes, size_t length, uint8_t *stored) {
    uint16_t sum = 0, inverse = 0;
    for (size_t index = 0; index < length; index += 2) {
        uint16_t value = (uint16_t)((uint16_t)bytes[index] << 8 | bytes[index + 1]);
        sum = (uint16_t)(sum + value);
        inverse = (uint16_t)(inverse + (uint16_t)~value);
    }
    put16(stored, sum == UINT16_MAX ? 0 : sum);
    put16(stored + 2, inverse == UINT16_MAX ? 0 : inverse);
}

static void populate_image(gc_card_image *image, unsigned file_count,
                           const char *prefix) {
    assert(file_count <= 59);
    if (!file_count)
        return;
    for (unsigned copy = 0; copy < 2; ++copy) {
        uint8_t *directory = image->bytes + (1u + copy) * GC_CARD_BLOCK_BYTES;
        uint8_t *bat = image->bytes + (3u + copy) * GC_CARD_BLOCK_BYTES;
        for (unsigned index = 0; index < file_count; ++index) {
            uint8_t *entry = directory + index * 64;
            memset(entry, 0, 64);
            memcpy(entry, "TEST00", 6);
            snprintf((char *)entry + 8, 32, "%s%u", prefix, index);
            put32(entry + 0x28, index);
            put32(entry + 0x2c, 0);
            put16(entry + 0x30, 2);
            put16(entry + 0x32, 1);
            put16(entry + 0x36, 5 + index);
            put16(entry + 0x38, 1);
            put32(entry + 0x3c, 2048);
            put16(bat + 10 + index * 2, UINT16_MAX);
        }
        put16(bat + 6, 59 - file_count);
        put16(bat + 8, 4 + file_count);
        checksum(directory, 0x1ffc, directory + 0x1ffc);
        checksum(bat + 4, GC_CARD_BLOCK_BYTES - 4, bat);
    }
    for (unsigned index = 0; index < file_count; ++index) {
        uint8_t *payload = image->bytes + (5u + index) * GC_CARD_BLOCK_BYTES;
        for (unsigned pixel = 0; pixel < 1024; ++pixel)
            put16(payload + pixel * 2,
                  0x8000u | ((index + 8u) & 31u) << 10 | 12u << 5 | 20u);
        snprintf((char *)payload + 2048, 32, "Runtime fixture %u", index);
        memcpy(payload + 2080, "First-party test payload", 24);
    }
}

static void make_pair(unsigned source_count, unsigned destination_count) {
    gc_card_image source = {0}, target = {0};
    assert(gc_card_image_create(&source, 64, 0, 0) == GC_CARD_IMAGE_OK);
    assert(gc_card_image_create(&target, 64, 0, 0) == GC_CARD_IMAGE_OK);
    populate_image(&source, source_count, "runtime_fixture_");
    populate_image(&target, destination_count, "destination_fixture_");
    assert(gc_card_image_write(&source, inputs[0]) == GC_CARD_IMAGE_OK);
    assert(gc_card_image_write(&target, inputs[1]) == GC_CARD_IMAGE_OK);
    gc_card_image_free(&source);
    gc_card_image_free(&target);
}

static void make_images(unsigned file_count) {
    make_pair(file_count, 0);
}

static void setup(GcServices *services, gc_menu *menu) {
    make_images(4);
    gc_menu_init(menu, GC_REGION_EUROPE);
    assert(gc_services_init(services, menu, inputs, states));
    assert(menu->cards[0].file_count == 4);
    gc_menu_skip_startup(menu);
    menu->face = GC_FACE_MEMORY_CARD;
    menu->page = GC_PAGE_CARDS;
}

static void confirm(gc_menu *menu, gc_card_action action) {
    menu->card_slot = 0;
    menu->card_index = 1;
    menu->card_action = action;
    menu->page = GC_PAGE_CARD_CONFIRM;
    menu->confirm_yes = true;
    menu->format_second_confirmation = true;
}

static void teardown(GcServices *services) {
    gc_services_destroy(services);
    for (unsigned slot = 0; slot < 2; ++slot) {
        assert(remove(inputs[slot]) == 0);
        assert(remove(states[slot]) == 0);
    }
}

static void test_copy_and_error(void) {
    gc_menu *menu = calloc(1, sizeof(*menu));
    GcServices *services = calloc(1, sizeof(*services));
    assert(menu && services);
    setup(services, menu);
    GcCardRuntime runtime;
    GcCardRuntimeEvents events;
    assert(gc_card_runtime_init(&runtime, services, menu, NULL));
    gc_page original = menu->page;
    assert(gc_card_runtime_press(&runtime, GC_BUTTON_CANCEL, NULL, NULL, &events));
    assert(!events.handled && menu->page == original);
    confirm(menu, GC_CARD_ACTION_COPY);
    assert(gc_card_runtime_press(&runtime, GC_BUTTON_CONFIRM, NULL, NULL, &events));
    assert(events.handled && runtime.active && events.operation.sound_event_count == 1);
    assert(events.operation.sound_events[0] == 8);
    assert(menu->message == GC_MESSAGE_CARD_COPIED && menu->cards[1].file_count == 1);
    gc_card_image source = {0};
    assert(gc_card_image_load(&source, inputs[0]) == GC_CARD_IMAGE_OK);
    assert(source.card.file_count == 4);
    gc_card_image_free(&source);
    assert(gc_card_runtime_press(&runtime, GC_BUTTON_CONFIRM, NULL, NULL, &events));
    assert(events.handled && menu->page == GC_PAGE_MESSAGE &&
           !events.operation.dismissed);
    assert(gc_card_runtime_tick(&runtime, 1, &events));
    assert(events.operation.metadata_completed &&
           events.operation.sound_events[0] == 19);
    /* SlotA polls OK before slotB publishes its changed directory. */
    assert(runtime.operation.progress_poll_count == 1);
    assert(gc_card_runtime_tick(&runtime, 38, &events));
    assert(!runtime.operation.input_ready);
    assert(gc_card_runtime_tick(&runtime, 1, &events));
    assert(events.operation.presentation_ready && runtime.operation.input_ready);
    assert(events.operation.sound_event_count == 1 &&
           events.operation.sound_events[0] == 16);
    assert(gc_card_runtime_press(&runtime, GC_BUTTON_CANCEL, NULL, NULL, &events));
    assert(events.operation.dismissed && !runtime.active &&
           menu->page == GC_PAGE_CARDS);
    assert(events.operation.sound_events[0] == 7);
    confirm(menu, GC_CARD_ACTION_MOVE);
    menu->card_index = 0;
    services->card_paths[1] = "runtime-missing-directory/card.raw";
    assert(gc_card_runtime_press(&runtime, GC_BUTTON_CONFIRM, NULL, NULL, &events));
    assert(menu->message == GC_MESSAGE_CARD_SAVE_FAILED);
    assert(menu->cards[0].file_count == 4 && menu->cards[1].file_count == 1);
    assert(gc_card_runtime_tick(&runtime, 1, &events));
    assert(events.operation.sound_events[0] == 13 && events.operation.geometry_aborted);
    assert(gc_card_runtime_tick(&runtime, 100, &events));
    assert(runtime.operation.input_ready);
    assert(gc_card_runtime_press(&runtime, GC_BUTTON_CONFIRM, NULL, NULL, &events));
    assert(events.operation.dismissed);
    gc_card_runtime_destroy(&runtime);
    teardown(services);
    free(services);
    free(menu);
}

static bool random_sample(void *context, uint32_t *value) {
    unsigned *samples = context;
    *value = (*samples)++;
    return true;
}

static void test_transfer_random_capture(void) {
    gc_menu *menu = calloc(1, sizeof(*menu));
    GcServices *services = calloc(1, sizeof(*services));
    assert(menu && services);
    setup(services, menu);
    GcCardRuntime runtime;
    GcCardRuntimeEvents events;
    unsigned samples = 0;
    assert(gc_card_runtime_init(&runtime, services, menu, NULL));
    gc_card_runtime_set_random(&runtime, random_sample, &samples);
    confirm(menu, GC_CARD_ACTION_COPY);
    assert(gc_card_runtime_begin(&runtime, NULL, NULL, &events));
    assert(samples == 1);
    assert(gc_card_runtime_tick(&runtime, 100, &events));
    assert(samples == 1 && runtime.operation.input_ready);
    assert(gc_card_runtime_press(&runtime, GC_BUTTON_CANCEL, NULL, NULL, &events));
    confirm(menu, GC_CARD_ACTION_MOVE);
    menu->card_index = 0;
    assert(gc_card_runtime_begin(&runtime, NULL, NULL, &events));
    assert(samples == 2);
    assert(gc_card_runtime_tick(&runtime, 100, &events));
    assert(samples == 2 && runtime.operation.input_ready);
    gc_card_runtime_destroy(&runtime);
    teardown(services);
    free(services);
    free(menu);
}

static void test_erase_and_random_timing(void) {
    gc_menu *menu = calloc(1, sizeof(*menu));
    GcServices *services = calloc(1, sizeof(*services));
    assert(menu && services);
    setup(services, menu);
    GcCardRuntime runtime;
    GcCardRuntimeEvents events;
    unsigned samples = 0;
    assert(gc_card_runtime_init(&runtime, services, menu, NULL));
    gc_card_runtime_set_random(&runtime, random_sample, &samples);
    confirm(menu, GC_CARD_ACTION_ERASE);
    assert(gc_card_runtime_begin(&runtime, NULL, NULL, &events));
    assert(menu->cards[0].file_count == 3 && samples == 0);
    assert(gc_card_runtime_advance(&runtime, 0.1, &events));
    assert(runtime.operation.shrink_remaining == 15 && samples == 0);
    assert(gc_card_runtime_tick(&runtime, 15, &events));
    assert(runtime.operation.shrink_remaining == 0 && !runtime.operation.pieces_active);
    assert(gc_card_runtime_tick(&runtime, 1, &events));
    assert(events.operation.pieces_begin && samples == 24 &&
           runtime.operation.pieces_active);
    assert(runtime.operation.piece_delays[0] == 0 &&
           runtime.operation.piece_delays[1] == 2);
    assert(gc_card_runtime_tick(&runtime, 12, &events));
    assert(events.operation.pieces_finished && runtime.operation.layout_tick == 0);
    assert(gc_card_runtime_tick(&runtime, 39, &events));
    assert(!runtime.operation.input_ready && runtime.operation.layout_tick == 39);
    assert(gc_card_runtime_tick(&runtime, 1, &events));
    assert(runtime.operation.input_ready && !events.operation.sound_event_count);
    assert(gc_card_runtime_press(&runtime, GC_BUTTON_CONFIRM, NULL, NULL, &events));
    assert(menu->page == GC_PAGE_CARDS && events.operation.sound_events[0] == 7);
    gc_card_runtime_destroy(&runtime);
    teardown(services);
    free(services);
    free(menu);
}

static void test_scene_retained_art(const char *ipl_path) {
    CcPlatform *platform = cc_platform_create("Card operation verification", 640, 480);
    GcScene *scene = calloc(1, sizeof(*scene));
    gc_menu *menu = calloc(1, sizeof(*menu));
    GcServices *services = calloc(1, sizeof(*services));
    assert(platform && scene && menu && services &&
           gc_scene_init(scene, platform, ipl_path));
    setup(services, menu);
    menu->region = scene->startup.frame_rate == 50 ? GC_REGION_EUROPE : GC_REGION_USA;
    assert(gc_scene_set_cards(scene, services->cards));
    gc_scene_draw(scene, menu);
    menu->page_elapsed = 2;
    gc_scene_draw(scene, menu);
    assert(gc_scene_card_operation_ready(scene));
    GcCardRuntime runtime;
    GcCardRuntimeEvents events;
    assert(gc_card_runtime_init(&runtime, services, menu, scene));
    /* A failed write has no destination file to seed. Its native abort
     * relayout restores the captured source and keeps its original art. */
    GcCardTextures *failed_art = scene->card_art[0];
    services->card_paths[1] = "runtime-missing-directory/card.raw";
    confirm(menu, GC_CARD_ACTION_COPY);
    assert(gc_card_runtime_begin(&runtime, NULL, NULL, &events));
    assert(menu->message == GC_MESSAGE_CARD_SAVE_FAILED);
    assert(gc_card_runtime_tick(&runtime, 1, &events));
    assert(events.operation.geometry_aborted &&
           scene->card_cells.cells[0][1].duration == 40);
    menu->page_elapsed = 1;
    gc_scene_draw(scene, menu);
    assert(gc_card_runtime_tick(&runtime, 100, &events));
    assert(runtime.operation.input_ready);
    assert(gc_card_runtime_press(&runtime, GC_BUTTON_CANCEL, NULL, NULL, &events));
    assert(scene->card_art[0] == failed_art && !scene->card_operation_art[0]);
    services->card_paths[1] = states[1];
    menu->page_elapsed = 2;
    gc_scene_draw(scene, menu);
    uint8_t delays[12] = {3};
    int16_t angles[12] = {0};
    GcCardTextures *old_art = scene->card_art[0];
    assert(old_art);
    confirm(menu, GC_CARD_ACTION_ERASE);
    assert(gc_card_runtime_begin(&runtime, delays, angles, &events));
    assert(scene->card_operation_art[0] == old_art && !scene->card_art[0]);
    assert(gc_scene_set_cards(scene, services->cards));
    assert(scene->card_art[0] && scene->card_art[0] != old_art);
    assert(scene->card_operation_art[0] == old_art);
    for (unsigned tick = 0; tick < 74; ++tick) {
        assert(gc_card_runtime_tick(&runtime, 1, &events));
        menu->page_elapsed = (double)(tick + 1) / runtime.frame_rate;
        gc_scene_draw(scene, menu);
        assert(scene->card_operation_active && scene->card_operation_art[0] == old_art);
    }
    assert(runtime.operation.input_ready);
    assert(gc_card_runtime_press(&runtime, GC_BUTTON_CANCEL, NULL, NULL, &events));
    assert(events.operation.dismissed && !scene->card_operation_active &&
           !scene->card_operation_art[0]);
    gc_card_runtime_destroy(&runtime);
    gc_scene_destroy(scene);
    cc_platform_destroy(platform);
    teardown(services);
    free(services);
    free(menu);
    free(scene);
}

static void scene_steps(GcCardRuntime *runtime, unsigned ticks) {
    GcCardRuntimeEvents events;
    assert(gc_card_runtime_tick(runtime, ticks, &events));
    runtime->menu->page_elapsed += (double)ticks / runtime->frame_rate;
    gc_scene_draw(runtime->scene, runtime->menu);
}

static void scene_press(GcCardRuntime *runtime, gc_button button) {
    assert(runtime->active ||
           gc_scene_can_press(runtime->scene, runtime->menu, button));
    gc_page page = runtime->menu->page;
    uint64_t revision = runtime->services->revision;
    GcCardRuntimeEvents events;
    assert(gc_card_runtime_press(runtime, button, NULL, NULL, &events));
    if (!events.handled)
        gc_services_press(runtime->services, runtime->menu, button);
    if (revision != runtime->services->revision)
        assert(gc_scene_set_cards(runtime->scene, runtime->services->cards));
    if (runtime->menu->page != page)
        runtime->menu->page_elapsed = 0;
    gc_scene_draw(runtime->scene, runtime->menu);
}

static uint64_t arrow_pixels(const GcScene *scene, const char name[4]) {
    const GcLayoutTable *table =
        gc_layout_table(&scene->layouts, GC_LANGUAGE_ENGLISH, GC_LAYOUT_CARD);
    GcLayoutPane pane;
    GcLayoutVertex vertices[4];
    assert(gc_layout_find_pane(table, name, 0, &pane));
    assert(gc_layout_pane_quad(&pane, vertices));
    float left = vertices[0].x, right = left;
    float top = vertices[0].y, bottom = top;
    for (unsigned index = 1; index < 4; ++index) {
        left = fminf(left, vertices[index].x);
        right = fmaxf(right, vertices[index].x);
        top = fminf(top, vertices[index].y);
        bottom = fmaxf(bottom, vertices[index].y);
    }
    unsigned min_x = (unsigned)floorf(left + 27);
    unsigned max_x = (unsigned)ceilf(right + 29);
    unsigned min_y = (unsigned)floorf(240 + (top - 233) * scene->pixel_scale_y);
    unsigned max_y = (unsigned)ceilf(240 + (bottom - 215) * scene->pixel_scale_y);
    uint64_t hash = UINT64_C(14695981039346656037);
    for (unsigned y = min_y; y <= max_y; ++y)
        for (unsigned x = min_x; x <= max_x; ++x) {
            uint8_t rgba[4];
            assert(gc_software_read_pixel(scene->platform, x, y, rgba));
            for (unsigned channel = 0; channel < 4; ++channel) {
                hash ^= rgba[channel];
                hash *= UINT64_C(1099511628211);
            }
        }
    return hash;
}

static void verify_transfer_payload(unsigned slot, size_t index,
                                    unsigned original_index) {
    gc_card_image image = {0};
    assert(gc_card_image_load(&image, states[slot]) == GC_CARD_IMAGE_OK);
    uint8_t bytes[2];
    assert(gc_card_image_read_file(&image, index, 0, bytes, sizeof(bytes)) ==
           GC_CARD_IMAGE_OK);
    unsigned expected = 0x8000u | ((original_index + 8u) & 31u) << 10 | 12u << 5 | 20u;
    assert(bytes[0] == (uint8_t)(expected >> 8) && bytes[1] == (uint8_t)expected);
    gc_card_image_free(&image);
}

static void scene_operation(GcCardRuntime *runtime, gc_card_action action) {
    uint64_t grid = gc_software_frame_hash(runtime->scene->platform);
    scene_press(runtime, GC_BUTTON_CONFIRM);
    assert(runtime->menu->page == GC_PAGE_CARD_ACTION);
    scene_steps(runtime, 20);
    assert(grid != gc_software_frame_hash(runtime->scene->platform));
    for (unsigned attempt = 0; runtime->menu->card_action != action; ++attempt) {
        assert(attempt < 3);
        scene_press(runtime, GC_BUTTON_UP);
    }
    uint64_t popup = gc_software_frame_hash(runtime->scene->platform);
    scene_press(runtime, GC_BUTTON_CONFIRM);
    assert(runtime->menu->page == GC_PAGE_CARD_CONFIRM && !runtime->menu->confirm_yes);
    scene_steps(runtime, 30);
    assert(popup != gc_software_frame_hash(runtime->scene->platform));
    scene_press(runtime, GC_BUTTON_UP);
    assert(runtime->menu->confirm_yes);
    scene_press(runtime, GC_BUTTON_CONFIRM);
    assert(runtime->active && runtime->menu->page == GC_PAGE_MESSAGE);
    bool transfer = action == GC_CARD_ACTION_COPY || action == GC_CARD_ACTION_MOVE;
    unsigned source_slot = runtime->operation.source_slot;
    unsigned source_index = runtime->operation.source_file_index;
    float source_position[3];
    memcpy(source_position,
           runtime->scene->card_cells.cells[source_slot][source_index].position,
           sizeof(source_position));
    GcCardRuntimeEvents events;
    assert(gc_card_runtime_press(runtime, GC_BUTTON_CANCEL, NULL, NULL, &events));
    assert(events.handled && !events.operation.dismissed);
    unsigned ticks = 0;
    uint64_t first_transfer = 0;
    while (!runtime->operation.input_ready) {
        assert(ticks++ < 120);
        scene_steps(runtime, 1);
        if (transfer) {
            const GcCardCell *target =
                &runtime->scene->card_cells
                     .cells[source_slot ^ 1u]
                           [runtime->operation.destination_file_index];
            if (ticks == 1) {
                size_t first = runtime->scene->card_operation_first_rows[source_slot] *
                               GC_CARD_COLUMNS;
                assert(target->duration == 40 && target->counter == 1);
                assert(target->drawn_selection == 6 && target->selection == 5);
                assert(
                    !memcmp(target->start, source_position, sizeof(source_position)));
                assert(target->tangent[0] == 0 && target->tangent[2] == 0);
                assert(target->tangent[1] == (source_index - first < 8 ? 150 : -150));
                assert(!gc_scene_card_operation_ready(runtime->scene));
                first_transfer = gc_software_frame_hash(runtime->scene->platform);
            } else if (ticks == 20) {
                assert(first_transfer !=
                       gc_software_frame_hash(runtime->scene->platform));
                assert(target->counter == 20 &&
                       fabsf(target->position[0] - source_position[0]) > 1);
                if (target->target_alpha) {
                    unsigned x = (unsigned)floorf(target->drawn_position[0] / 16 + 28);
                    unsigned y =
                        (unsigned)floorf(240 + (target->drawn_position[1] / 16 - 224) *
                                                   runtime->scene->pixel_scale_y);
                    uint8_t pixel[4];
                    assert(
                        gc_software_read_pixel(runtime->scene->platform, x, y, pixel));
                    unsigned red = (source_index + 8) & 31;
                    assert(pixel[0] == ((red << 3) | (red >> 2)) && pixel[1] == 99 &&
                           pixel[2] == 165 && pixel[3] == 255);
                }
            }
        }
    }
    assert(gc_scene_card_operation_ready(runtime->scene));
    uint64_t message = gc_software_frame_hash(runtime->scene->platform);
    scene_steps(runtime, 0);
    assert(message == gc_software_frame_hash(runtime->scene->platform));
    scene_press(runtime, GC_BUTTON_CANCEL);
    assert(!runtime->active && runtime->menu->page == GC_PAGE_CARDS);
    scene_steps(runtime, 40);
}

static void test_scroll_and_operations(const char *ipl_path) {
    CcPlatform *platform = cc_platform_create("Scrolling card verification", 640, 480);
    GcScene *scene = calloc(1, sizeof(*scene));
    gc_menu *menu = calloc(1, sizeof(*menu));
    GcServices *services = calloc(1, sizeof(*services));
    assert(platform && scene && menu && services &&
           gc_scene_init(scene, platform, ipl_path));
    make_images(25);
    gc_menu_init(menu,
                 scene->startup.frame_rate == 50 ? GC_REGION_EUROPE : GC_REGION_USA);
    assert(gc_services_init(services, menu, inputs, states));
    assert(menu->cards[0].file_count == 25 &&
           gc_card_free_blocks(&menu->cards[0]) == 34);
    gc_menu_skip_startup(menu);
    menu->face = GC_FACE_MEMORY_CARD;
    menu->page = GC_PAGE_CARDS;
    assert(gc_scene_set_cards(scene, services->cards));
    GcCardRuntime runtime;
    assert(gc_card_runtime_init(&runtime, services, menu, scene));
    scene_steps(&runtime, 120);
    assert(scene->card_arrow_alpha[0][0] == 0 && scene->card_arrow_alpha[0][1] == 20);
    for (unsigned step = 0; step < 6; ++step) {
        if (step == 3) {
            gc_services_press(services, menu, GC_BUTTON_DOWN);
            /* Native scrolling resets the motion records during input.
             * Our pending row comparison also closes that gate before draw. */
            assert(menu->card_first_row == 1);
            assert(gc_scene_can_press(scene, menu, GC_BUTTON_UP));
            assert(!gc_scene_can_press(scene, menu, GC_BUTTON_CONFIRM));
            gc_scene_draw(scene, menu);
        } else {
            scene_press(&runtime, GC_BUTTON_DOWN);
        }
        bool scrolling = step >= 3;
        if (scrolling) {
            assert(gc_scene_can_press(scene, menu, GC_BUTTON_UP));
            assert(!gc_scene_can_press(scene, menu, GC_BUTTON_CONFIRM));
            scene_steps(&runtime, 39);
            assert(gc_scene_can_press(scene, menu, GC_BUTTON_UP));
            assert(!gc_scene_can_press(scene, menu, GC_BUTTON_CANCEL));
            scene_steps(&runtime, 1);
        } else {
            scene_steps(&runtime, 6);
        }
    }
    assert(menu->card_index == 24 && menu->card_first_row == 3);
    assert(scene->card_arrow_alpha[0][0] == 20 && !scene->card_arrow_alpha[0][1]);
    scene_press(&runtime, GC_BUTTON_RIGHT);
    assert(menu->card_index == 25 && !gc_menu_card_selected(menu));
    scene_press(&runtime, GC_BUTTON_CONFIRM);
    assert(menu->page == GC_PAGE_CARDS);
    scene_press(&runtime, GC_BUTTON_LEFT);
    for (unsigned step = 0; step < 6; ++step) {
        scene_press(&runtime, GC_BUTTON_UP);
        scene_steps(&runtime, 40);
    }
    assert(menu->card_index == 0 && menu->card_first_row == 0);
    /* Direction repeats retarget an unfinished Hermite path. Native A/B
     * retain their stricter all-cell gate; scrolling itself does not wait. */
    for (unsigned step = 0; step < 6; ++step) {
        GcCardCell previous = scene->card_cells.cells[0][16];
        scene_press(&runtime, GC_BUTTON_DOWN);
        if (step >= 3) {
            const GcCardCell *cell = &scene->card_cells.cells[0][16];
            assert(!memcmp(cell->start, previous.position, sizeof(cell->start)));
            assert(!memcmp(cell->tangent, previous.velocity, sizeof(cell->tangent)));
            assert(cell->counter == 0 && cell->duration == 40);
            assert(gc_scene_can_press(scene, menu, GC_BUTTON_DOWN));
            assert(!gc_scene_can_press(scene, menu, GC_BUTTON_CONFIRM));
        }
        scene_steps(&runtime, 3);
    }
    assert(menu->card_index == 24 && menu->card_first_row == 3);
    scene_steps(&runtime, 37);
    assert(gc_scene_can_press(scene, menu, GC_BUTTON_CONFIRM));
    for (unsigned step = 0; step < 6; ++step) {
        scene_press(&runtime, GC_BUTTON_UP);
        scene_steps(&runtime, 40);
    }
    assert(menu->card_index == 0 && menu->card_first_row == 0);
    for (unsigned step = 0; step < 4; ++step) {
        scene_press(&runtime, GC_BUTTON_DOWN);
        scene_steps(&runtime, 40);
    }
    assert(menu->card_first_row == 1);
    assert(scene->card_arrow_alpha[0][0] == 20 && scene->card_arrow_alpha[0][1] == 20);
    scene_steps(&runtime, (unsigned)((32 - scene->card_ticks % 32) % 32));
    uint64_t first_arrow = arrow_pixels(scene, "arau");
    scene_steps(&runtime, 15);
    uint64_t last_arrow = arrow_pixels(scene, "arau");
    assert(first_arrow != last_arrow);
    scene_steps(&runtime, 1);
    assert(last_arrow == arrow_pixels(scene, "arau"));
    for (unsigned step = 0; step < 2; ++step) {
        scene_press(&runtime, GC_BUTTON_DOWN);
        scene_steps(&runtime, 40);
    }
    assert(menu->card_index == 24 && menu->card_first_row == 3);
    scene_operation(&runtime, GC_CARD_ACTION_COPY);
    assert(menu->cards[0].file_count == 25 && menu->cards[1].file_count == 1);
    verify_transfer_payload(1, 0, 24);
    scene_operation(&runtime, GC_CARD_ACTION_ERASE);
    assert(menu->cards[0].file_count == 24 && menu->cards[1].file_count == 1);
    assert(menu->card_first_row == 2 && scene->card_cells.first_rows[0] == 2);
    for (unsigned step = 0; step < 3; ++step)
        scene_press(&runtime, GC_BUTTON_RIGHT);
    assert(menu->card_index == 23);
    scene_operation(&runtime, GC_CARD_ACTION_MOVE);
    assert(menu->cards[0].file_count == 23 && menu->cards[1].file_count == 2);
    verify_transfer_payload(1, 1, 23);
    gc_card_image persisted = {0};
    assert(gc_card_image_load(&persisted, states[0]) == GC_CARD_IMAGE_OK);
    assert(persisted.card.file_count == 23 &&
           gc_card_free_blocks(&persisted.card) == 36);
    gc_card_image_free(&persisted);
    assert(gc_card_image_load(&persisted, inputs[0]) == GC_CARD_IMAGE_OK);
    assert(persisted.card.file_count == 25);
    gc_card_image_free(&persisted);
    gc_card_runtime_destroy(&runtime);
    gc_scene_destroy(scene);
    cc_platform_destroy(platform);
    teardown(services);
    free(services);
    free(menu);
    free(scene);
}

static void test_offscreen_destination(const char *ipl_path) {
    CcPlatform *platform =
        cc_platform_create("Card destination verification", 640, 480);
    GcScene *scene = calloc(1, sizeof(*scene));
    gc_menu *menu = calloc(1, sizeof(*menu));
    GcServices *services = calloc(1, sizeof(*services));
    assert(platform && scene && menu && services &&
           gc_scene_init(scene, platform, ipl_path));
    make_pair(25, 25);
    gc_menu_init(menu,
                 scene->startup.frame_rate == 50 ? GC_REGION_EUROPE : GC_REGION_USA);
    assert(gc_services_init(services, menu, inputs, states));
    gc_menu_skip_startup(menu);
    menu->face = GC_FACE_MEMORY_CARD;
    menu->page = GC_PAGE_CARDS;
    assert(gc_scene_set_cards(scene, services->cards));
    GcCardRuntime runtime;
    assert(gc_card_runtime_init(&runtime, services, menu, scene));
    scene_steps(&runtime, 120);
    scene_operation(&runtime, GC_CARD_ACTION_COPY);
    assert(menu->cards[0].file_count == 25 && menu->cards[1].file_count == 26);
    assert(menu->card_slot == 0 && gc_menu_card_cursor_index(menu, 0) == 0);
    assert(gc_menu_card_first_row(menu, 0) == 0 &&
           gc_menu_card_first_row(menu, 1) == 0);
    /* Native16808 clamps rows but does not follow the new destination file.
     * Its positive tangent still starts at the captured top-row source. */
    assert(scene->card_cells.cells[1][25].tangent[1] == 150);
    assert(scene->card_cells.cells[1][25].target_alpha == 0 &&
           scene->card_cells.cells[1][25].drawn_alpha == 0);
    verify_transfer_payload(1, 25, 0);
    gc_card_runtime_destroy(&runtime);
    gc_scene_destroy(scene);
    cc_platform_destroy(platform);
    teardown(services);
    free(services);
    free(menu);
    free(scene);
}

int main(int argc, char **argv) {
    test_copy_and_error();
    test_transfer_random_capture();
    test_erase_and_random_timing();
    if (argc > 1) {
        test_scene_retained_art(argv[1]);
        test_scroll_and_operations(argv[1]);
        test_offscreen_destination(argv[1]);
    }
    puts("Local card runtime tests passed.");
    return 0;
}
