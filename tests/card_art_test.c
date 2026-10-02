#include "gamecube/card_art.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

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

static void fixture_image(gc_card_image *image, uint8_t flags, uint16_t formats,
                          uint16_t speeds) {
    unsigned copy;

    assert(gc_card_image_create(image, 64, 0, 0) == GC_CARD_IMAGE_OK);
    for (copy = 0; copy < 2; ++copy) {
        uint8_t *entry = image->bytes + (size_t)(1 + copy) * GC_CARD_BLOCK_BYTES;
        uint8_t *bat = image->bytes + (size_t)(3 + copy) * GC_CARD_BLOCK_BYTES;

        memset(entry, 0, 64);
        memcpy(entry, "TEST00", 6);
        entry[6] = 0xff;
        entry[7] = flags;
        memcpy(entry + 8, "art_fixture", 11);
        put_u32(entry + 0x2c, 0);
        put_u16(entry + 0x30, formats);
        put_u16(entry + 0x32, speeds);
        put_u16(entry + 0x36, 5);
        put_u16(entry + 0x38, 4);
        put_u32(entry + 0x3c, UINT32_MAX);
        put_u16(bat + 6, 55);
        put_u16(bat + 8, 8);
        put_u16(bat + 10, 6);
        put_u16(bat + 12, 7);
        put_u16(bat + 14, 8);
        put_u16(bat + 16, UINT16_MAX);
        fixture_checksum(entry, 0x1ffc, entry + 0x1ffc);
        fixture_checksum(bat + 4, GC_CARD_BLOCK_BYTES - 4, bat);
    }
    memset(image->bytes + 5u * GC_CARD_BLOCK_BYTES, 0, 4u * GC_CARD_BLOCK_BYTES);
    assert(gc_card_image_read(image, image->bytes, image->byte_count) ==
           GC_CARD_IMAGE_OK);
}

static void fill_rgb5a3(uint8_t *pixels, size_t byte_count, uint16_t color) {
    size_t offset;

    for (offset = 0; offset < byte_count; offset += 2)
        put_u16(pixels + offset, color);
}

static void assert_color(const GcIplImage *image, unsigned x, unsigned y, unsigned red,
                         unsigned green, unsigned blue, unsigned alpha) {
    size_t offset = ((size_t)y * image->width + x) * 4;

    assert(image->rgba);
    assert(image->rgba[offset] == red);
    assert(image->rgba[offset + 1] == green);
    assert(image->rgba[offset + 2] == blue);
    assert(image->rgba[offset + 3] == alpha);
}

static void test_native_rgb5a3_tiles(void) {
    gc_card_image image = {0};
    GcCardArt art = {0};
    uint8_t *payload;

    fixture_image(&image, 2, 2, 1);
    payload = image.bytes + 5u * GC_CARD_BLOCK_BYTES;
    fill_rgb5a3(payload, 6144, 0xfc00);
    put_u16(payload + 32, 0x83e0);
    fill_rgb5a3(payload + 6144, 2048, 0x001f);
    assert(gc_card_art_load(&image, 0, &art) == GC_CARD_IMAGE_OK);
    assert(art.banner.width == 96 && art.banner.height == 32);
    assert_color(&art.banner, 0, 0, 255, 0, 0, 255);
    assert_color(&art.banner, 4, 0, 0, 255, 0, 255);
    assert_color(&art.banner, 0, 1, 255, 0, 0, 255);
    assert(art.frame_count == 1 && art.durations[0] == 4);
    assert_color(&art.icons[0], 0, 0, 0, 17, 255, 0);
    assert(gc_card_art_icon(&art, UINT64_MAX) == &art.icons[0]);
    gc_card_art_destroy(&art);
    gc_card_image_free(&image);
}

static void test_ci8_shared_private_palettes_and_animation(void) {
    gc_card_image image = {0};
    GcCardArt art = {0};
    uint8_t *payload;
    size_t offset;

    fixture_image(&image, 5, 29, 57);
    payload = image.bytes + 5u * GC_CARD_BLOCK_BYTES;
    memset(payload, 7, 3072);
    payload[32] = 8;
    put_u16(payload + 3072 + 7 * 2, 0xfc00);
    put_u16(payload + 3072 + 8 * 2, 0x801f);
    offset = 3584;
    memset(payload + offset, 3, 1024);
    offset += 1024;
    memset(payload + offset, 3, 1024);
    offset += 1024;
    put_u16(payload + offset + 3 * 2, 0x801f);
    offset += 512;
    memset(payload + offset, 3, 1024);
    offset += 1024;
    put_u16(payload + offset + 3 * 2, 0x83e0);
    assert(gc_card_art_load(&image, 0, &art) == GC_CARD_IMAGE_OK);
    assert_color(&art.banner, 0, 0, 255, 0, 0, 255);
    assert_color(&art.banner, 8, 0, 0, 0, 255, 255);
    assert_color(&art.icons[0], 0, 0, 0, 255, 0, 255);
    assert_color(&art.icons[1], 0, 0, 0, 0, 255, 255);
    assert_color(&art.icons[2], 0, 0, 0, 255, 0, 255);
    assert(art.ping_pong && art.frame_count == 3);
    assert(art.durations[0] == 4 && art.durations[1] == 8 && art.durations[2] == 12);
    assert(gc_card_art_frame(&art, 3) == 0);
    assert(gc_card_art_frame(&art, 4) == 1);
    assert(gc_card_art_frame(&art, 12) == 2);
    assert(gc_card_art_frame(&art, 24) == 1);
    assert(gc_card_art_frame(&art, 31) == 1);
    assert(gc_card_art_frame(&art, 32) == 0);
    art.ping_pong = false;
    assert(gc_card_art_frame(&art, 24) == 0);
    gc_card_art_destroy(&art);
    gc_card_image_free(&image);
}

static void test_bounds_and_unchanged_art_on_error(void) {
    gc_card_image image = {0};
    GcCardArt art = {0};
    uint8_t *previous;

    fixture_image(&image, 2, 2, 1);
    assert(gc_card_art_load(&image, 0, &art) == GC_CARD_IMAGE_OK);
    previous = art.banner.rgba;
    image.files[0].image_offset = 4u * GC_CARD_BLOCK_BYTES - 64;
    assert(gc_card_art_load(&image, 0, &art) == GC_CARD_IMAGE_FORMAT);
    assert(art.banner.rgba == previous);
    image.files[0].image_flags = 0;
    image.files[0].icon_formats = 1;
    image.files[0].image_offset = 4u * GC_CARD_BLOCK_BYTES - 1024;
    assert(gc_card_art_load(&image, 0, &art) == GC_CARD_IMAGE_FORMAT);
    assert(art.banner.rgba == previous);
    image.files[0].image_offset = UINT32_MAX;
    assert(gc_card_art_load(&image, 0, &art) == GC_CARD_IMAGE_OK);
    assert(!art.banner.rgba && !gc_card_art_icon(&art, 0));
    assert(gc_card_art_load(&image, 1, &art) == GC_CARD_IMAGE_ARGUMENT);
    gc_card_art_destroy(&art);
    gc_card_image_free(&image);
}

int main(void) {
    test_native_rgb5a3_tiles();
    test_ci8_shared_private_palettes_and_animation();
    test_bounds_and_unchanged_art_on_error();
    puts("card artwork tests passed");
    return 0;
}
