#include "gamecube/services.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static const char *input_paths[2] = {"services-input-a.raw", "services-input-b.raw"};
static const char *state_paths[2] = {"services-state-a.raw", "services-state-b.raw"};
static const char *failed_path = "services-test-missing-directory/card.raw";

static void put_u16(uint8_t *bytes, uint16_t value) {
    bytes[0] = (uint8_t)(value >> 8);
    bytes[1] = (uint8_t)value;
}

static void put_u32(uint8_t *bytes, uint32_t value) {
    bytes[0] = (uint8_t)(value >> 24);
    bytes[1] = (uint8_t)(value >> 16);
    bytes[2] = (uint8_t)(value >> 8);
    bytes[3] = (uint8_t)value;
}

static void fixture_checksum(uint8_t *bytes, size_t length, uint8_t *stored) {
    uint16_t sum = 0;
    uint16_t inverse = 0;
    size_t index;

    for (index = 0; index < length; index += 2) {
        uint16_t value = (uint16_t)((uint16_t)bytes[index] << 8 | bytes[index + 1]);
        sum = (uint16_t)(sum + value);
        inverse = (uint16_t)(inverse + (uint16_t)~value);
    }
    put_u16(stored, sum == UINT16_MAX ? 0 : sum);
    put_u16(stored + 2, inverse == UINT16_MAX ? 0 : inverse);
}

static void make_inputs(void) {
    gc_card_image source = {0};
    gc_card_image target = {0};
    unsigned copy;

    assert(gc_card_image_create(&source, 64, 0, 0) == GC_CARD_IMAGE_OK);
    assert(gc_card_image_create(&target, 64, 0, 0) == GC_CARD_IMAGE_OK);
    for (copy = 0; copy < 2; ++copy) {
        uint8_t *entry = source.bytes + (size_t)(1 + copy) * GC_CARD_BLOCK_BYTES;
        uint8_t *bat = source.bytes + (size_t)(3 + copy) * GC_CARD_BLOCK_BYTES;
        memset(entry, 0, 64);
        memcpy(entry, "TEST00", 6);
        memcpy(entry + 8, "services_fixture", 16);
        put_u32(entry + 0x2c, UINT32_MAX);
        put_u16(entry + 0x36, 5);
        put_u16(entry + 0x38, 1);
        put_u32(entry + 0x3c, UINT32_MAX);
        put_u16(bat + 6, 58);
        put_u16(bat + 8, 5);
        put_u16(bat + 10, UINT16_MAX);
        fixture_checksum(entry, 0x1ffc, entry + 0x1ffc);
        fixture_checksum(bat + 4, GC_CARD_BLOCK_BYTES - 4, bat);
    }
    memset(source.bytes + 5u * GC_CARD_BLOCK_BYTES, 0x46, GC_CARD_BLOCK_BYTES);
    assert(gc_card_image_write(&source, input_paths[0]) == GC_CARD_IMAGE_OK);
    assert(gc_card_image_write(&target, input_paths[1]) == GC_CARD_IMAGE_OK);
    gc_card_image_free(&source);
    gc_card_image_free(&target);
}

static void begin_services(GcServices *services, gc_menu *menu) {
    make_inputs();
    gc_menu_init(menu, GC_REGION_EUROPE);
    assert(gc_services_init(services, menu, input_paths, state_paths));
    gc_menu_skip_startup(menu);
    gc_services_press(services, menu, GC_BUTTON_DOWN);
    gc_services_press(services, menu, GC_BUTTON_CONFIRM);
    assert(menu->page == GC_PAGE_CARDS && menu->cards[0].file_count == 1);
}

static void perform_action(GcServices *services, gc_menu *menu, gc_card_action action) {
    gc_services_press(services, menu, GC_BUTTON_CONFIRM);
    assert(menu->page == GC_PAGE_CARD_ACTION);
    while (menu->card_action != action)
        gc_services_press(services, menu, GC_BUTTON_DOWN);
    assert(menu->card_action == action);
    gc_services_press(services, menu, GC_BUTTON_CONFIRM);
    assert(menu->page == GC_PAGE_CARD_CONFIRM);
    gc_services_press(services, menu, GC_BUTTON_UP);
    gc_services_press(services, menu, GC_BUTTON_CONFIRM);
    assert(menu->page == GC_PAGE_MESSAGE);
}

static void check_disk(const char *path, size_t count) {
    gc_card_image image = {0};
    uint8_t payload[16];

    assert(gc_card_image_load(&image, path) == GC_CARD_IMAGE_OK);
    assert(image.card.file_count == count);
    if (count) {
        assert(gc_card_image_read_file(&image, 0, 0, payload, sizeof(payload)) ==
               GC_CARD_IMAGE_OK);
        for (size_t index = 0; index < sizeof(payload); ++index)
            assert(payload[index] == 0x46);
    }
    gc_card_image_free(&image);
}

static void cleanup(void) {
    for (unsigned slot = 0; slot < 2; ++slot) {
        assert(remove(input_paths[slot]) == 0);
        assert(remove(state_paths[slot]) == 0);
    }
}

static void test_copy_and_erase_persist_payload(void) {
    GcServices services;
    gc_menu menu;

    begin_services(&services, &menu);
    perform_action(&services, &menu, GC_CARD_ACTION_COPY);
    assert(menu.message == GC_MESSAGE_CARD_COPIED);
    assert(menu.cards[0].file_count == 1 && menu.cards[1].file_count == 1);
    assert(!menu.cards_changed[0] && !menu.cards_changed[1]);
    check_disk(state_paths[0], 1);
    check_disk(state_paths[1], 1);
    gc_services_press(&services, &menu, GC_BUTTON_CONFIRM);
    perform_action(&services, &menu, GC_CARD_ACTION_ERASE);
    assert(menu.message == GC_MESSAGE_CARD_ERASED);
    check_disk(state_paths[0], 0);
    check_disk(state_paths[1], 1);
    check_disk(input_paths[0], 1);
    check_disk(input_paths[1], 0);
    gc_services_destroy(&services);
    cleanup();
}

static void test_decline_retains_action_without_writing(void) {
    GcServices services;
    gc_menu menu;

    begin_services(&services, &menu);
    uint64_t revision = services.revision;
    gc_services_press(&services, &menu, GC_BUTTON_CONFIRM);
    assert(menu.card_action == GC_CARD_ACTION_ERASE);
    gc_services_press(&services, &menu, GC_BUTTON_UP);
    assert(menu.card_action == GC_CARD_ACTION_COPY);
    gc_services_press(&services, &menu, GC_BUTTON_CONFIRM);
    assert(menu.page == GC_PAGE_CARD_CONFIRM && !menu.confirm_yes);
    gc_services_press(&services, &menu, GC_BUTTON_LEFT);
    gc_services_press(&services, &menu, GC_BUTTON_RIGHT);
    assert(!menu.confirm_yes);
    gc_services_press(&services, &menu, GC_BUTTON_CONFIRM);
    assert(menu.page == GC_PAGE_CARD_ACTION && services.revision == revision);
    gc_services_press(&services, &menu, GC_BUTTON_CANCEL);
    gc_services_press(&services, &menu, GC_BUTTON_CONFIRM);
    assert(menu.page == GC_PAGE_CARD_ACTION && menu.card_action == GC_CARD_ACTION_COPY);
    assert(services.revision == revision);
    check_disk(state_paths[0], 1);
    check_disk(state_paths[1], 0);
    gc_services_destroy(&services);
    cleanup();
}

static void test_destination_write_failure_restores_memory(void) {
    GcServices services;
    gc_menu menu;

    begin_services(&services, &menu);
    services.card_paths[1] = failed_path;
    perform_action(&services, &menu, GC_CARD_ACTION_MOVE);
    assert(menu.message == GC_MESSAGE_CARD_SAVE_FAILED);
    assert(menu.cards[0].file_count == 1 && menu.cards[1].file_count == 0);
    assert(services.cards[0].card.file_count == 1 &&
           services.cards[1].card.file_count == 0);
    check_disk(state_paths[0], 1);
    check_disk(state_paths[1], 0);
    gc_services_destroy(&services);
    cleanup();
}

static void test_second_move_write_failure_keeps_both_copies(void) {
    GcServices services;
    gc_menu menu;

    begin_services(&services, &menu);
    services.card_paths[0] = failed_path;
    perform_action(&services, &menu, GC_CARD_ACTION_MOVE);
    assert(menu.message == GC_MESSAGE_CARD_SAVE_FAILED);
    assert(menu.cards[0].file_count == 1 && menu.cards[1].file_count == 1);
    check_disk(state_paths[0], 1);
    check_disk(state_paths[1], 1);
    gc_services_destroy(&services);
    cleanup();
}

static void test_restart_from_local_images_preserves_edits(void) {
    GcServices services;
    gc_menu menu;

    begin_services(&services, &menu);
    perform_action(&services, &menu, GC_CARD_ACTION_MOVE);
    assert(menu.message == GC_MESSAGE_CARD_MOVED);
    gc_services_destroy(&services);
    gc_menu_init(&menu, GC_REGION_EUROPE);
    assert(gc_services_init(&services, &menu, state_paths, state_paths));
    assert(menu.cards[0].file_count == 0 && menu.cards[1].file_count == 1);
    check_disk(state_paths[0], 0);
    check_disk(state_paths[1], 1);
    check_disk(input_paths[0], 1);
    gc_services_destroy(&services);
    cleanup();
}

static void make_unformatted_input(unsigned slot) {
    gc_card_image image = {0};

    assert(gc_card_image_create(&image, 64, 0, 0) == GC_CARD_IMAGE_OK);
    memset(image.bytes, 0xff, image.byte_count);
    assert(gc_card_image_write(&image, input_paths[slot]) == GC_CARD_IMAGE_OK);
    gc_card_image_free(&image);
}

static void confirm_format(GcServices *services, gc_menu *menu) {
    gc_menu_skip_startup(menu);
    gc_services_press(services, menu, GC_BUTTON_DOWN);
    gc_services_press(services, menu, GC_BUTTON_CONFIRM);
    gc_services_press(services, menu, GC_BUTTON_CONFIRM);
    assert(menu->page == GC_PAGE_CARD_CONFIRM &&
           menu->card_action == GC_CARD_ACTION_FORMAT);
    gc_services_press(services, menu, GC_BUTTON_UP);
    gc_services_press(services, menu, GC_BUTTON_CONFIRM);
    assert(menu->format_second_confirmation && !menu->confirm_yes);
    gc_services_press(services, menu, GC_BUTTON_UP);
    gc_services_press(services, menu, GC_BUTTON_CONFIRM);
    assert(menu->page == GC_PAGE_MESSAGE);
}

static void check_unformatted_disk(const char *path) {
    gc_card_image image = {0};

    assert(gc_card_image_open_present(&image, path, 1) == GC_CARD_IMAGE_OK);
    assert(image.card.status == GC_CARD_UNFORMATTED);
    for (size_t index = 0; index < image.byte_count; ++index)
        assert(image.bytes[index] == 0xff);
    gc_card_image_free(&image);
}

static void test_unformatted_card_format_and_failed_save(void) {
    GcServices services;
    gc_menu menu;

    make_inputs();
    make_unformatted_input(0);
    gc_menu_init(&menu, GC_REGION_JAPAN);
    assert(gc_services_init(&services, &menu, input_paths, state_paths));
    assert(menu.cards[0].status == GC_CARD_UNFORMATTED);
    assert(services.cards[0].encoding == 1);
    confirm_format(&services, &menu);
    assert(menu.message == GC_MESSAGE_CARD_FORMATTED);
    assert(menu.cards[0].status == GC_CARD_READY);
    check_disk(state_paths[0], 0);
    check_unformatted_disk(input_paths[0]);
    gc_services_destroy(&services);
    make_unformatted_input(0);
    gc_menu_init(&menu, GC_REGION_JAPAN);
    assert(gc_services_init(&services, &menu, input_paths, state_paths));
    services.card_paths[0] = failed_path;
    confirm_format(&services, &menu);
    assert(menu.message == GC_MESSAGE_CARD_SAVE_FAILED);
    assert(menu.cards[0].status == GC_CARD_UNFORMATTED);
    assert(services.cards[0].card.status == GC_CARD_UNFORMATTED);
    check_unformatted_disk(state_paths[0]);
    check_unformatted_disk(input_paths[0]);
    gc_services_destroy(&services);
    cleanup();
}

static void test_damaged_other_card_does_not_block_erase(void) {
    gc_card_image image = {0};
    GcServices services;
    gc_menu menu;

    make_inputs();
    assert(gc_card_image_load(&image, input_paths[1]) == GC_CARD_IMAGE_OK);
    image.bytes[0] ^= 1;
    assert(gc_card_image_write(&image, input_paths[1]) == GC_CARD_IMAGE_OK);
    gc_card_image_free(&image);
    gc_menu_init(&menu, GC_REGION_EUROPE);
    assert(gc_services_init(&services, &menu, input_paths, state_paths));
    assert(menu.cards[1].status == GC_CARD_DAMAGED);
    gc_menu_skip_startup(&menu);
    gc_services_press(&services, &menu, GC_BUTTON_DOWN);
    gc_services_press(&services, &menu, GC_BUTTON_CONFIRM);
    perform_action(&services, &menu, GC_CARD_ACTION_ERASE);
    assert(menu.message == GC_MESSAGE_CARD_ERASED && menu.cards[0].file_count == 0);
    assert(menu.cards[1].status == GC_CARD_DAMAGED);
    check_disk(state_paths[0], 0);
    assert(gc_card_image_open_present(&image, state_paths[1], 0) == GC_CARD_IMAGE_OK);
    assert(image.card.status == GC_CARD_DAMAGED);
    gc_card_image_free(&image);
    gc_services_destroy(&services);
    cleanup();
}

int main(void) {
    test_copy_and_erase_persist_payload();
    test_decline_retains_action_without_writing();
    test_destination_write_failure_restores_memory();
    test_second_move_write_failure_keeps_both_copies();
    test_restart_from_local_images_preserves_edits();
    test_unformatted_card_format_and_failed_save();
    test_damaged_other_card_does_not_block_erase();
    puts("services tests passed");
    return 0;
}
