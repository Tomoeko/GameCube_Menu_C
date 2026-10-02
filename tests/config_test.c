#include "gamecube/config.h"
#include "gamecube/card_art.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *config_path = "config-test.ini";
static const char *state_paths[2] = {"config-state-a.raw", "config-state-b.raw"};

static void write_text(const char *text) {
    FILE *file = fopen(config_path, "w");
    assert(file && fputs(text, file) >= 0 && !fclose(file));
}

static void test_configuration(void) {
    GcConfig config;
    gc_config_init(&config);
    assert(config.slot_present[0] && config.slot_present[1]);
    assert(!config.dummy_data[0] && !config.dummy_data[1]);
    assert(config.dummy_count[0] == 3 && config.dummy_count[1] == 3);
    assert(gc_config_load(&config, "config-test-missing.ini", NULL) ==
           GC_CONFIG_MISSING);
    assert(gc_config_write(&config, config_path) == GC_CONFIG_OK);
    GcConfig restored = {0};
    assert(gc_config_load(&restored, config_path, NULL) == GC_CONFIG_OK);
    assert(!memcmp(&restored, &config, sizeof(config)));
    write_text(" # comment\r\n[CaRdS]\r\n SLOT_A = unavailable ; A off\n"
               "slot_b=AVAILABLE\ndummy_A=TRUE\ndummy_b=false\n"
               "dummy_count=12\ndummy_count_b=4\n");
    assert(gc_config_load(&config, config_path, NULL) == GC_CONFIG_OK);
    assert(!config.slot_present[0] && config.slot_present[1]);
    assert(config.dummy_data[0] && !config.dummy_data[1]);
    assert(config.dummy_count[0] == 12 && config.dummy_count[1] == 4);
    GcConfig saved = config;
    size_t line = 0;
    write_text("slot_a=present\ndummy_count=60\n");
    assert(gc_config_load(&config, config_path, &line) == GC_CONFIG_INVALID &&
           line == 2);
    assert(!memcmp(&saved, &config, sizeof(config)));
    const char *invalid[] = {"dummy_a=yes\n",
                             "unknown=true\n",
                             "[unknown]\n",
                             "dummy_count=-1\n",
                             "dummy_count=999999999999999\n",
                             "slot_a=true\n",
                             "slot_a present\n"};
    for (size_t index = 0; index < sizeof(invalid) / sizeof(invalid[0]); ++index) {
        write_text(invalid[index]);
        assert(gc_config_load(&config, config_path, &line) == GC_CONFIG_INVALID &&
               line == 1);
        assert(!memcmp(&saved, &config, sizeof(config)));
    }
    char long_line[300];
    memset(long_line, 'x', sizeof(long_line));
    long_line[sizeof(long_line) - 1] = 0;
    write_text(long_line);
    assert(gc_config_load(&config, config_path, &line) == GC_CONFIG_INVALID);
    assert(!memcmp(&saved, &config, sizeof(config)));
    assert(remove(config_path) == 0);
}

static void test_noinsert_parser(void) {
    unsigned mask = 7;
    assert(gc_config_noinsert_mask("a", &mask) && mask == 1);
    assert(gc_config_noinsert_mask("b", &mask) && mask == 2);
    assert(gc_config_noinsert_mask("ab", &mask) && mask == 3);
    const char *invalid[] = {NULL, "", "ba", "c", "a,b", "both"};
    for (size_t index = 0; index < sizeof(invalid) / sizeof(invalid[0]); ++index) {
        assert(!gc_config_noinsert_mask(invalid[index], &mask));
        assert(mask == 3);
    }
    assert(!gc_config_noinsert_mask("a", NULL));
}

static void test_native_dummy_images(void) {
    GcConfig config;
    gc_config_init(&config);
    for (unsigned slot = 0; slot < 2; ++slot) {
        for (unsigned encoding = 0; encoding < 2; ++encoding) {
            gc_card_image image = {0};
            assert(gc_config_create_card(&config, slot, (uint16_t)encoding, &image) ==
                   GC_CARD_IMAGE_OK);
            assert(image.card.status == GC_CARD_READY && image.card.file_count == 0);
            assert(image.block_count == 64 && gc_card_free_blocks(&image.card) == 59);
            config.dummy_data[slot] = true;
            assert(gc_config_create_card(&config, slot, (uint16_t)encoding, &image) ==
                   GC_CARD_IMAGE_OK);
            assert(image.card.file_count == 3 &&
                   gc_card_free_blocks(&image.card) == 56);
            assert(image.encoding == encoding && image.card.files[0].blocks == 1);
            assert(!strcmp(image.card.files[0].title, "Test Save 01"));
            GcCardArt art = {0};
            assert(gc_card_art_load(&image, 0, &art) == GC_CARD_IMAGE_OK);
            assert(art.frame_count == 1 && art.durations[0] == 4);
            assert(art.icons[0].rgba && art.icons[0].width == 32 &&
                   art.icons[0].height == 32);
            assert(!art.banner.rgba);
            assert(art.icons[0].rgba[0] == 24 && art.icons[0].rgba[3] == 255);
            gc_card_art_destroy(&art);
            gc_card_image snapshot = {0};
            assert(gc_card_image_clone(&snapshot, &image) == GC_CARD_IMAGE_OK);
            assert(gc_config_create_card(&config, slot, (uint16_t)encoding, &image) ==
                   GC_CARD_IMAGE_OK);
            assert(image.byte_count == snapshot.byte_count &&
                   !memcmp(image.bytes, snapshot.bytes, image.byte_count));
            assert(gc_card_image_erase(&image, 1) == GC_CARD_IMAGE_OK);
            assert(image.card.file_count == 2 &&
                   gc_card_free_blocks(&image.card) == 57);
            gc_card_image_free(&snapshot);
            gc_card_image_free(&image);
            config.dummy_data[slot] = false;
        }
    }
    config.dummy_data[0] = true;
    config.dummy_count[0] = GC_CONFIG_DUMMY_LIMIT;
    gc_card_image full = {0};
    assert(gc_config_create_card(&config, 0, 0, &full) == GC_CARD_IMAGE_OK);
    assert(full.card.file_count == 59 && gc_card_free_blocks(&full.card) == 0);
    gc_card_image_free(&full);
    config.dummy_count[0] = GC_CONFIG_DUMMY_LIMIT + 1;
    assert(gc_config_create_card(&config, 0, 0, &full) == GC_CARD_IMAGE_ARGUMENT);
}

static void test_slot_configuration_and_import_override(void) {
    gc_menu *menu = calloc(1, sizeof(*menu));
    GcServices *services = calloc(1, sizeof(*services));
    assert(menu && services);
    GcConfig config;
    const char *no_inputs[2] = {NULL, NULL};
    for (unsigned present = 0; present < 4; ++present) {
        for (unsigned dummy = 0; dummy < 4; ++dummy) {
            gc_config_init(&config);
            for (unsigned slot = 0; slot < 2; ++slot) {
                config.slot_present[slot] = (present & (1u << slot)) != 0;
                config.dummy_data[slot] = (dummy & (1u << slot)) != 0;
            }
            gc_menu_init(menu, GC_REGION_EUROPE);
            assert(gc_config_prepare_services(&config, services, menu, no_inputs,
                                              state_paths));
            for (unsigned slot = 0; slot < 2; ++slot) {
                assert(menu->cards[slot].status ==
                       (config.slot_present[slot] ? GC_CARD_READY : GC_CARD_ABSENT));
                assert(
                    menu->cards[slot].file_count ==
                    (config.slot_present[slot] && config.dummy_data[slot] ? 3u : 0u));
            }
            gc_services_destroy(services);
        }
    }
    gc_config_init(&config);
    config.dummy_data[0] = true;
    gc_card_image imported = {0};
    assert(gc_config_create_card(&config, 0, 0, &imported) == GC_CARD_IMAGE_OK);
    const char *original_path = "config-original.raw";
    assert(gc_card_image_write(&imported, original_path) == GC_CARD_IMAGE_OK);
    config.slot_present[0] = false;
    config.dummy_data[0] = false;
    config.dummy_data[1] = true;
    const char *explicit_inputs[2] = {original_path, NULL};
    gc_menu_init(menu, GC_REGION_EUROPE);
    assert(gc_config_prepare_services(&config, services, menu, explicit_inputs,
                                      state_paths));
    assert(menu->cards[0].status == GC_CARD_READY && menu->cards[0].file_count == 3);
    assert(menu->cards[1].status == GC_CARD_READY && menu->cards[1].file_count == 3);
    gc_card_image original = {0};
    assert(gc_card_image_load(&original, original_path) == GC_CARD_IMAGE_OK);
    assert(original.byte_count == imported.byte_count &&
           !memcmp(original.bytes, imported.bytes, original.byte_count));
    gc_card_image_free(&original);
    gc_card_image_free(&imported);
    gc_services_destroy(services);
    assert(remove(original_path) == 0);
    for (unsigned slot = 0; slot < 2; ++slot)
        assert(remove(state_paths[slot]) == 0);
    free(services);
    free(menu);
}

static void perform_dummy_action(GcServices *services, gc_menu *menu,
                                 gc_card_action action) {
    gc_services_press(services, menu, GC_BUTTON_CONFIRM);
    assert(menu->page == GC_PAGE_CARD_ACTION);
    while (menu->card_action != action)
        gc_services_press(services, menu, GC_BUTTON_DOWN);
    gc_services_press(services, menu, GC_BUTTON_CONFIRM);
    assert(menu->page == GC_PAGE_CARD_CONFIRM && !menu->confirm_yes);
    gc_services_press(services, menu, GC_BUTTON_UP);
    gc_services_press(services, menu, GC_BUTTON_CONFIRM);
    assert(menu->page == GC_PAGE_MESSAGE);
}

static void check_dummy_card_persistence(const GcServices *services) {
    for (unsigned slot = 0; slot < 2; ++slot) {
        gc_card_image saved = {0};
        assert(gc_card_image_load(&saved, state_paths[slot]) == GC_CARD_IMAGE_OK);
        assert(saved.byte_count == services->cards[slot].byte_count);
        assert(!memcmp(saved.bytes, services->cards[slot].bytes, saved.byte_count));
        gc_card_image_free(&saved);
    }
}

static void test_scrollable_dummy_operations(void) {
    GcConfig config;
    gc_config_init(&config);
    write_text("[cards]\nslot_a=present\nslot_b=present\n"
               "dummy_a=true\ndummy_b=true\ndummy_count=25\n");
    assert(gc_config_load(&config, config_path, NULL) == GC_CONFIG_OK);
    assert(remove(config_path) == 0);
    gc_menu *menu = calloc(1, sizeof(*menu));
    GcServices *services = calloc(1, sizeof(*services));
    assert(menu && services);
    const char *no_inputs[2] = {NULL, NULL};
    uint8_t original[GC_CARD_BLOCK_BYTES];
    uint8_t transferred[GC_CARD_BLOCK_BYTES];
    const gc_region regions[] = {GC_REGION_USA, GC_REGION_EUROPE, GC_REGION_JAPAN};

    for (size_t region = 0; region < sizeof(regions) / sizeof(regions[0]); ++region) {
        gc_menu_init(menu, regions[region]);
        assert(gc_config_prepare_services(&config, services, menu, no_inputs,
                                          state_paths));
        for (unsigned slot = 0; slot < 2; ++slot) {
            assert(menu->cards[slot].file_count == 25);
            assert(gc_card_free_blocks(&menu->cards[slot]) == 34);
            for (size_t index = 0; index < 25; ++index)
                assert(strcmp(menu->cards[slot].files[index].filename,
                              menu->cards[slot ^ 1].files[index].filename));
        }
        gc_menu_skip_startup(menu);
        gc_services_press(services, menu, GC_BUTTON_DOWN);
        gc_services_press(services, menu, GC_BUTTON_CONFIRM);
        assert(menu->page == GC_PAGE_CARDS && menu->card_slot == 0);
        for (unsigned row = 0; row < 6; ++row)
            gc_services_press(services, menu, GC_BUTTON_DOWN);
        assert(menu->card_index == 24 && menu->card_first_row == 3);
        assert(gc_menu_card_selected(menu));
        assert(gc_card_image_read_file(&services->cards[0], 24, 0, original,
                                       sizeof(original)) == GC_CARD_IMAGE_OK);

        perform_dummy_action(services, menu, GC_CARD_ACTION_COPY);
        assert(menu->message == GC_MESSAGE_CARD_COPIED);
        assert(menu->cards[0].file_count == 25 && menu->cards[1].file_count == 26);
        assert(gc_card_free_blocks(&menu->cards[1]) == 33);
        assert(!strcmp(menu->cards[0].files[24].filename,
                       menu->cards[1].files[25].filename));
        assert(gc_card_image_read_file(&services->cards[1], 25, 0, transferred,
                                       sizeof(transferred)) == GC_CARD_IMAGE_OK);
        assert(!memcmp(original, transferred, sizeof(original)));
        check_dummy_card_persistence(services);

        gc_services_press(services, menu, GC_BUTTON_CONFIRM);
        gc_services_press(services, menu, GC_BUTTON_UP);
        assert(menu->page == GC_PAGE_CARDS && menu->card_index == 20);
        assert(gc_card_image_read_file(&services->cards[0], 20, 0, original,
                                       sizeof(original)) == GC_CARD_IMAGE_OK);
        char moved_name[sizeof(menu->cards[0].files[20].filename)];
        memcpy(moved_name, menu->cards[0].files[20].filename, sizeof(moved_name));
        perform_dummy_action(services, menu, GC_CARD_ACTION_MOVE);
        assert(menu->message == GC_MESSAGE_CARD_MOVED);
        assert(menu->cards[0].file_count == 24 && menu->cards[1].file_count == 27);
        assert(gc_card_free_blocks(&menu->cards[0]) == 35);
        assert(gc_card_free_blocks(&menu->cards[1]) == 32);
        assert(!strcmp(moved_name, menu->cards[1].files[26].filename));
        for (size_t index = 0; index < menu->cards[0].file_count; ++index)
            assert(strcmp(moved_name, menu->cards[0].files[index].filename));
        assert(gc_card_image_read_file(&services->cards[1], 26, 0, transferred,
                                       sizeof(transferred)) == GC_CARD_IMAGE_OK);
        assert(!memcmp(original, transferred, sizeof(original)));
        check_dummy_card_persistence(services);

        gc_services_press(services, menu, GC_BUTTON_CONFIRM);
        perform_dummy_action(services, menu, GC_CARD_ACTION_ERASE);
        assert(menu->message == GC_MESSAGE_CARD_ERASED);
        assert(menu->cards[0].file_count == 23 && menu->cards[1].file_count == 27);
        assert(gc_card_free_blocks(&menu->cards[0]) == 36);
        assert(gc_card_free_blocks(&menu->cards[1]) == 32);
        assert(services->revision == 3);
        check_dummy_card_persistence(services);
        gc_services_destroy(services);
        for (unsigned slot = 0; slot < 2; ++slot)
            assert(remove(state_paths[slot]) == 0);
    }
    free(services);
    free(menu);
}

int main(void) {
    test_configuration();
    test_noinsert_parser();
    test_native_dummy_images();
    test_slot_configuration_and_import_override();
    test_scrollable_dummy_operations();
    puts("Config and native QA card tests passed.");
    return 0;
}
