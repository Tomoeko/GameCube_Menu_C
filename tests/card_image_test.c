#include "gamecube/card_image.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
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

static void fixture_rechecksum(gc_card_image *image) {
    unsigned copy;

    fixture_checksum(image->bytes, 0x1fc, image->bytes + 0x1fc);
    for (copy = 0; copy < 2; ++copy) {
        uint8_t *directory = image->bytes + (size_t)(1 + copy) * GC_CARD_BLOCK_BYTES;
        uint8_t *bat = image->bytes + (size_t)(3 + copy) * GC_CARD_BLOCK_BYTES;
        fixture_checksum(directory, 0x1ffc, directory + 0x1ffc);
        fixture_checksum(bat + 4, GC_CARD_BLOCK_BYTES - 4, bat);
    }
}

static void fixture_file(gc_card_image *image) {
    unsigned copy;
    gc_card_image parsed = {0};

    assert(gc_card_image_create(image, 64, 0, 0) == GC_CARD_IMAGE_OK);
    for (copy = 0; copy < 2; ++copy) {
        uint8_t *entry = image->bytes + (size_t)(1 + copy) * GC_CARD_BLOCK_BYTES;
        uint8_t *bat = image->bytes + (size_t)(3 + copy) * GC_CARD_BLOCK_BYTES;

        memset(entry, 0, 64);
        memcpy(entry, "TEST00", 6);
        entry[6] = 0xff;
        memcpy(entry + 8, "local_fixture", 13);
        put_u32(entry + 0x28, 1234);
        put_u32(entry + 0x2c, 0x100);
        put_u16(entry + 0x30, 2);
        put_u16(entry + 0x32, 1);
        put_u16(entry + 0x36, 5);
        put_u16(entry + 0x38, 3);
        put_u16(entry + 0x3a, UINT16_MAX);
        put_u32(entry + 0x3c, 32);
        put_u16(bat + 6, 56);
        put_u16(bat + 8, 9);
        put_u16(bat + 10, 9);
        put_u16(bat + 12, UINT16_MAX);
        put_u16(bat + 18, 6);
    }
    memset(image->bytes + 5u * GC_CARD_BLOCK_BYTES, 0x11, GC_CARD_BLOCK_BYTES);
    memset(image->bytes + 9u * GC_CARD_BLOCK_BYTES, 0x22, GC_CARD_BLOCK_BYTES);
    memset(image->bytes + 6u * GC_CARD_BLOCK_BYTES, 0x33, GC_CARD_BLOCK_BYTES);
    memset(image->bytes + 5u * GC_CARD_BLOCK_BYTES + 32, 0, 64);
    memcpy(image->bytes + 5u * GC_CARD_BLOCK_BYTES + 32, "Local fixture title", 19);
    memcpy(image->bytes + 5u * GC_CARD_BLOCK_BYTES + 64, "Local fixture comment", 21);
    fixture_rechecksum(image);
    assert(gc_card_image_read(&parsed, image->bytes, image->byte_count) ==
           GC_CARD_IMAGE_OK);
    gc_card_image_free(image);
    *image = parsed;
}

static void test_sizes_and_empty_checksums(void) {
    gc_card_image image = {0};
    gc_card_image readback = {0};
    unsigned block_count;

    for (block_count = 64; block_count <= GC_CARD_MAX_BLOCKS; block_count *= 2) {
        assert(gc_card_image_create(&image, block_count, 0, 0) == GC_CARD_IMAGE_OK);
        assert(image.card.file_count == 0);
        assert(image.card.capacity_blocks == block_count - 5);
        assert(image.bytes[GC_CARD_BLOCK_BYTES + 0x1ffc] == 0xf0);
        assert(image.bytes[GC_CARD_BLOCK_BYTES + 0x1ffd] == 0x03);
        assert(gc_card_image_read(&readback, image.bytes, image.byte_count) ==
               GC_CARD_IMAGE_OK);
        assert(gc_card_free_blocks(&readback.card) == block_count - 5);
    }
    assert(gc_card_image_create(&image, 63, 0, 0) == GC_CARD_IMAGE_ARGUMENT);
    assert(gc_card_image_create(&image, 128, 2, 0) == GC_CARD_IMAGE_ARGUMENT);
    gc_card_image_free(&image);
    gc_card_image_free(&readback);
}

static void test_noncontiguous_payload_metadata_and_backup(void) {
    gc_card_image image = {0};
    gc_card_image readback = {0};
    uint8_t data[16];

    fixture_file(&image);
    assert(image.card.file_count == 1);
    assert(strcmp(image.card.files[0].title, "Local fixture title") == 0);
    assert(strcmp(image.card.files[0].comment, "Local fixture comment") == 0);
    assert(image.files[0].modified_seconds == 1234);
    assert(image.files[0].image_offset == 0x100);
    assert(image.files[0].icon_formats == 2);
    assert(gc_card_image_read_file(&image, 0, GC_CARD_BLOCK_BYTES - 8, data, 16) ==
           GC_CARD_IMAGE_OK);
    assert(data[0] == 0x11 && data[7] == 0x11 && data[8] == 0x22 && data[15] == 0x22);
    assert(gc_card_image_read_file(&image, 0, 2u * GC_CARD_BLOCK_BYTES, data, 16) ==
           GC_CARD_IMAGE_OK);
    assert(data[0] == 0x33);
    assert(gc_card_image_read_file(&image, 0, 3u * GC_CARD_BLOCK_BYTES, data, 1) ==
           GC_CARD_IMAGE_ARGUMENT);
    image.bytes[GC_CARD_BLOCK_BYTES + 7] ^= 1;
    image.bytes[3u * GC_CARD_BLOCK_BYTES + 7] ^= 1;
    assert(gc_card_image_read(&readback, image.bytes, image.byte_count) ==
           GC_CARD_IMAGE_OK);
    assert(readback.active_directory == 1 && readback.active_bat == 1);
    gc_card_image_free(&image);
    gc_card_image_free(&readback);
}

static void test_corruption_and_preserved_destination(void) {
    gc_card_image image = {0};
    gc_card_image readback = {0};
    unsigned copy;

    fixture_file(&image);
    assert(gc_card_image_create(&readback, 128, 0, 0) == GC_CARD_IMAGE_OK);
    image.bytes[0] ^= 1;
    assert(gc_card_image_read(&readback, image.bytes, image.byte_count) ==
           GC_CARD_IMAGE_CHECKSUM);
    assert(readback.block_count == 128 && readback.card.file_count == 0);
    image.bytes[0] ^= 1;
    for (copy = 0; copy < 2; ++copy) {
        uint8_t *bat = image.bytes + (size_t)(3 + copy) * GC_CARD_BLOCK_BYTES;
        put_u16(bat + 10, 5);
    }
    fixture_rechecksum(&image);
    assert(gc_card_image_read(&readback, image.bytes, image.byte_count) ==
           GC_CARD_IMAGE_CHAIN);
    for (copy = 0; copy < 2; ++copy) {
        uint8_t *bat = image.bytes + (size_t)(3 + copy) * GC_CARD_BLOCK_BYTES;
        put_u16(bat + 10, 9);
        put_u16(bat + 14, UINT16_MAX);
    }
    fixture_rechecksum(&image);
    assert(gc_card_image_read(&readback, image.bytes, image.byte_count) ==
           GC_CARD_IMAGE_CHAIN);
    gc_card_image_free(&image);
    gc_card_image_free(&readback);
}

static void test_copy_move_erase_roundtrip(void) {
    gc_card_image source = {0};
    gc_card_image target = {0};
    gc_card_image moved = {0};
    gc_card_image readback = {0};
    uint8_t source_payload[3 * GC_CARD_BLOCK_BYTES];
    uint8_t target_payload[3 * GC_CARD_BLOCK_BYTES];

    fixture_file(&source);
    assert(gc_card_image_create(&target, 64, 0, 0) == GC_CARD_IMAGE_OK);
    assert(gc_card_image_copy(&source, &target, 0, false) == GC_CARD_IMAGE_OK);
    assert(source.card.file_count == 1 && target.card.file_count == 1);
    assert(target.files[0].copy_count == 1);
    assert(target.active_directory == 1 && target.active_bat == 1);
    assert(gc_card_image_read_file(&source, 0, 0, source_payload,
                                   sizeof(source_payload)) == GC_CARD_IMAGE_OK);
    assert(gc_card_image_read_file(&target, 0, 0, target_payload,
                                   sizeof(target_payload)) == GC_CARD_IMAGE_OK);
    assert(memcmp(source_payload, target_payload, sizeof(source_payload)) == 0);
    assert(gc_card_image_read(&readback, target.bytes, target.byte_count) ==
           GC_CARD_IMAGE_OK);
    assert(readback.card.file_count == 1 && readback.active_directory == 1);
    assert(gc_card_image_copy(&source, &target, 0, false) == GC_CARD_IMAGE_DUPLICATE);
    assert(gc_card_image_create(&moved, 64, 0, 0) == GC_CARD_IMAGE_OK);
    assert(gc_card_image_copy(&source, &moved, 0, true) == GC_CARD_IMAGE_OK);
    assert(source.card.file_count == 0 && moved.card.file_count == 1);
    assert(gc_card_free_blocks(&source.card) == 59);
    assert(gc_card_image_read(&readback, source.bytes, source.byte_count) ==
           GC_CARD_IMAGE_OK);
    assert(readback.card.file_count == 0);
    assert(gc_card_image_erase(&moved, 0) == GC_CARD_IMAGE_OK);
    assert(moved.card.file_count == 0 && gc_card_free_blocks(&moved.card) == 59);
    assert(gc_card_image_read(&readback, moved.bytes, moved.byte_count) ==
           GC_CARD_IMAGE_OK);
    assert(gc_card_image_format(&target) == GC_CARD_IMAGE_OK);
    assert(target.card.file_count == 0 && target.active_directory == 0);
    gc_card_image_free(&source);
    gc_card_image_free(&target);
    gc_card_image_free(&moved);
    gc_card_image_free(&readback);
}

static void test_copy_permissions_encoding_and_capacity(void) {
    gc_card_image source = {0};
    gc_card_image target = {0};
    unsigned copy;

    fixture_file(&source);
    assert(gc_card_image_create(&target, 64, 1, 0) == GC_CARD_IMAGE_OK);
    assert(gc_card_image_copy(&source, &target, 0, false) == GC_CARD_IMAGE_ENCODING);
    for (copy = 0; copy < 2; ++copy)
        source.bytes[(size_t)(1 + copy) * GC_CARD_BLOCK_BYTES + 0x34] = 24;
    fixture_rechecksum(&source);
    assert(gc_card_image_read(&source, source.bytes, source.byte_count) ==
           GC_CARD_IMAGE_OK);
    assert(gc_card_image_copy(&source, &target, 0, false) == GC_CARD_IMAGE_PERMISSION);
    assert(gc_card_image_copy(&source, &target, 0, true) == GC_CARD_IMAGE_PERMISSION);
    assert(gc_card_image_copy(&source, &source, 0, false) == GC_CARD_IMAGE_ARGUMENT);
    assert(gc_card_image_copy(&source, &target, 1, false) == GC_CARD_IMAGE_ARGUMENT);
    assert(gc_card_image_erase(&source, 0) == GC_CARD_IMAGE_OK);
    gc_card_image_free(&source);
    gc_card_image_free(&target);
}

static void test_disk_roundtrip(const char *path) {
    gc_card_image image = {0};
    gc_card_image readback = {0};

    fixture_file(&image);
    assert(gc_card_image_write(&image, path) == GC_CARD_IMAGE_OK);
    assert(gc_card_image_load(&readback, path) == GC_CARD_IMAGE_OK);
    assert(readback.byte_count == image.byte_count);
    assert(memcmp(readback.bytes, image.bytes, image.byte_count) == 0);
    assert(gc_card_image_erase(&image, 0) == GC_CARD_IMAGE_OK);
    assert(gc_card_image_write(&image, path) == GC_CARD_IMAGE_OK);
    assert(gc_card_image_load(&readback, path) == GC_CARD_IMAGE_OK);
    assert(readback.card.file_count == 0);
    assert(remove(path) == 0);
    gc_card_image_free(&image);
    gc_card_image_free(&readback);
}

static void test_present_unformatted_and_damaged_recovery(const char *path) {
    const size_t size = 64u * GC_CARD_BLOCK_BYTES;
    uint8_t *erased = malloc(size);
    uint8_t header[GC_CARD_BLOCK_BYTES];
    gc_card_image image = {0};
    gc_card_image clone = {0};
    gc_card_image strict = {0};

    assert(erased);
    memset(erased, 0xff, size);
    assert(gc_card_image_read(&strict, erased, size) == GC_CARD_IMAGE_FORMAT);
    assert(gc_card_image_attach(&image, erased, size, 1) == GC_CARD_IMAGE_OK);
    assert(image.card.status == GC_CARD_UNFORMATTED && image.encoding == 1);
    assert(image.card.file_count == 0 && image.card.capacity_blocks == 59);
    assert(image.integrity == GC_CARD_IMAGE_FORMAT);
    assert(gc_card_image_clone(&clone, &image) == GC_CARD_IMAGE_OK);
    assert(clone.bytes != image.bytes && memcmp(clone.bytes, erased, size) == 0);
    assert(gc_card_image_write(&image, path) == GC_CARD_IMAGE_OK);
    assert(gc_card_image_load(&strict, path) == GC_CARD_IMAGE_FORMAT);
    assert(gc_card_image_open_present(&clone, path, 1) == GC_CARD_IMAGE_OK);
    assert(clone.card.status == GC_CARD_UNFORMATTED);
    assert(gc_card_image_format(&image) == GC_CARD_IMAGE_OK);
    assert(image.card.status == GC_CARD_READY && image.encoding == 1);
    assert(image.integrity == GC_CARD_IMAGE_OK);
    assert(gc_card_image_read(&strict, image.bytes, image.byte_count) ==
           GC_CARD_IMAGE_OK);
    for (size_t index = 0; index < size; ++index)
        assert(erased[index] == 0xff);
    fixture_file(&image);
    memcpy(header, image.bytes, sizeof(header));
    image.bytes[GC_CARD_BLOCK_BYTES + 20] ^= 1;
    image.bytes[2u * GC_CARD_BLOCK_BYTES + 20] ^= 1;
    assert(gc_card_image_attach(&image, image.bytes, image.byte_count, 1) ==
           GC_CARD_IMAGE_OK);
    assert(image.card.status == GC_CARD_DAMAGED && image.card.file_count == 0);
    assert(image.integrity == GC_CARD_IMAGE_CHECKSUM && image.encoding == 0);
    assert(gc_card_image_clone(&clone, &image) == GC_CARD_IMAGE_OK);
    assert(clone.card.status == GC_CARD_DAMAGED);
    assert(clone.integrity == GC_CARD_IMAGE_CHECKSUM);
    assert(gc_card_image_format(&image) == GC_CARD_IMAGE_OK);
    assert(memcmp(image.bytes, header, sizeof(header)) == 0);
    assert(gc_card_image_read(&strict, image.bytes, image.byte_count) ==
           GC_CARD_IMAGE_OK);
    assert(strict.card.file_count == 0);
    assert(gc_card_image_attach(&image, erased, size - 1, 0) == GC_CARD_IMAGE_FORMAT);
    assert(image.card.status == GC_CARD_READY);
    assert(gc_card_image_attach(&image, erased, size, 2) == GC_CARD_IMAGE_ARGUMENT);
    assert(remove(path) == 0);
    gc_card_image_free(&image);
    gc_card_image_free(&clone);
    gc_card_image_free(&strict);
    free(erased);
}

int main(int argc, char **argv) {
    test_sizes_and_empty_checksums();
    test_noncontiguous_payload_metadata_and_backup();
    test_corruption_and_preserved_destination();
    test_copy_move_erase_roundtrip();
    test_copy_permissions_encoding_and_capacity();
    test_disk_roundtrip(argc > 1 ? argv[1] : "card_image_test.raw");
    test_present_unformatted_and_damaged_recovery(argc > 1 ? argv[1]
                                                           : "card_image_test.raw");
    puts("card image tests passed");
    return 0;
}
