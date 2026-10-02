#include "gamecube/disc.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FIXTURE_BYTES ((size_t)0x4000)
#define FIXTURE_FST ((size_t)0x500)
#define FIXTURE_BANNER ((size_t)0x1000)

typedef struct {
    uint8_t *bytes;
    size_t banner_size;
} DiscFixture;

static void put_u16(uint8_t *bytes, unsigned value) {
    bytes[0] = (uint8_t)(value >> 8);
    bytes[1] = (uint8_t)value;
}

static void put_u32(uint8_t *bytes, uint32_t value) {
    bytes[0] = (uint8_t)(value >> 24);
    bytes[1] = (uint8_t)(value >> 16);
    bytes[2] = (uint8_t)(value >> 8);
    bytes[3] = (uint8_t)value;
}

static void fst_entry(uint8_t *entry, bool directory, unsigned name, unsigned value,
                      unsigned length) {
    put_u32(entry, (directory ? UINT32_C(0x1000000) : 0) | name);
    put_u32(entry + 4, value);
    put_u32(entry + 8, length);
}

static DiscFixture make_fixture(bool multilingual, gc_region region) {
    DiscFixture fixture = {calloc(FIXTURE_BYTES, 1), multilingual ? 0x1fa0 : 0x1960};
    uint8_t *fst;
    uint8_t *banner;
    unsigned languages = multilingual ? 6 : 1;

    assert(fixture.bytes);
    memcpy(fixture.bytes, "GTST01", 6);
    fixture.bytes[3] = region == GC_REGION_JAPAN ? 'J'
                       : region == GC_REGION_USA ? 'E'
                                                 : 'P';
    fixture.bytes[6] = 1;
    fixture.bytes[7] = 2;
    fixture.bytes[8] = 1;
    put_u32(fixture.bytes + 0x1c, UINT32_C(0xc2339f3d));
    memcpy(fixture.bytes + 0x20, "Header title", 13);
    put_u32(fixture.bytes + 0x424, (uint32_t)FIXTURE_FST);
    put_u32(fixture.bytes + 0x428, 100);
    put_u32(fixture.bytes + 0x42c, 100);
    put_u32(fixture.bytes + 0x458, (uint32_t)region);
    fst = fixture.bytes + FIXTURE_FST;
    fst_entry(fst, true, 0, 0, 5);
    fst_entry(fst + 12, true, 1, 0, 3);
    fst_entry(fst + 24, false, 8, 0x600, 10);
    fst_entry(fst + 36, false, 20, (unsigned)FIXTURE_BANNER,
              (unsigned)fixture.banner_size);
    fst_entry(fst + 48, false, 32, 0x700, 4);
    memcpy(fst + 60, "\0assets\0opening.bnr\0opening.bnr\0tail\0", 37);
    banner = fixture.bytes + FIXTURE_BANNER;
    memcpy(banner, multilingual ? "BNR2" : "BNR1", 4);
    put_u16(banner + 0x20, 0xfc00);
    put_u16(banner + 0x20 + 2, 0x03f0);
    put_u16(banner + 0x20 + 32, 0x801f);
    put_u16(banner + 0x20 + 768, 0xffff);
    for (unsigned index = 0; index < languages; ++index) {
        uint8_t *metadata = banner + 0x1820 + index * 0x140;
        int written = snprintf((char *)metadata, 32, "Short title %u", index);
        assert(written > 0);
        memcpy(metadata + 32, "Developer", 10);
        written = snprintf((char *)metadata + 64, 64, "Full title %u", index);
        assert(written > 0);
        memset(metadata + 128, 'C', 64);
        memcpy(metadata + 192, "Description\nSecond line", 24);
    }
    if (region == GC_REGION_JAPAN) {
        banner[0x1820] = 0x82;
        banner[0x1821] = 0xa0;
        banner[0x1822] = 0;
    }
    return fixture;
}

static void test_image_and_multilingual_banner(void) {
    DiscFixture fixture = make_fixture(true, GC_REGION_EUROPE);
    GcDisc disc = {0};
    const uint8_t *rgba;

    assert(gc_disc_decode(fixture.bytes, FIXTURE_BYTES, &disc) == GC_DISC_IMAGE_OK);
    assert(disc.region == GC_REGION_EUROPE && disc.encoding == GC_TEXT_LATIN1);
    assert(strcmp(disc.game_code, "GTSP") == 0);
    assert(strcmp(disc.maker_code, "01") == 0);
    assert(disc.disc_id == 1 && disc.version == 2 && disc.audio_streaming);
    assert(strcmp(disc.header_title, "Header title") == 0);
    assert(disc.language_count == 6 && disc.banner.width == 96 &&
           disc.banner.height == 32);
    for (unsigned language = 0; language < 6; ++language) {
        const GcDiscMetadata *metadata = gc_disc_metadata(&disc, (gc_language)language);
        char expected[32];
        assert(snprintf(expected, sizeof(expected), "Full title %u", language) > 0);
        assert(strcmp(metadata->full_title, expected) == 0);
        assert(strlen(metadata->full_company) == 64);
        assert(metadata->full_company[64] == 0);
    }
    rgba = disc.banner.rgba;
    assert(rgba[0] == 255 && rgba[1] == 0 && rgba[2] == 0 && rgba[3] == 255);
    assert(rgba[4] == 51 && rgba[5] == 255 && rgba[6] == 0 && rgba[7] == 0);
    assert(rgba[4 * 4 + 2] == 255);
    assert(rgba[(4 * 96) * 4] == 255 && rgba[(4 * 96) * 4 + 3] == 255);
    assert(gc_disc_metadata(&disc, GC_LANGUAGE_JAPANESE) == &disc.languages[0]);
    gc_disc_destroy(&disc);
    free(fixture.bytes);
}

static void test_japanese_bytes_and_optional_banner(void) {
    DiscFixture fixture = make_fixture(false, GC_REGION_JAPAN);
    GcDisc disc = {0};
    const GcDiscMetadata *metadata;

    assert(gc_disc_decode(fixture.bytes, FIXTURE_BYTES, &disc) == GC_DISC_IMAGE_OK);
    assert(disc.encoding == GC_TEXT_SHIFT_JIS && disc.language_count == 1);
    metadata = gc_disc_metadata(&disc, GC_LANGUAGE_JAPANESE);
    assert((unsigned char)metadata->game_name[0] == 0x82);
    assert((unsigned char)metadata->game_name[1] == 0xa0);
    fst_entry(fixture.bytes + FIXTURE_FST + 36, false, 32, 0x700, 4);
    assert(gc_disc_decode(fixture.bytes, FIXTURE_BYTES, &disc) == GC_DISC_IMAGE_OK);
    assert(!disc.banner.rgba && !disc.language_count);
    assert(strcmp(disc.header_title, "Header title") == 0);
    assert(!gc_disc_metadata(&disc, GC_LANGUAGE_ENGLISH));
    gc_disc_destroy(&disc);
    free(fixture.bytes);
}

static void test_invalid_filesystems_preserve_owner(void) {
    DiscFixture fixture = make_fixture(false, GC_REGION_USA);
    GcDisc disc = {0};
    uint8_t *saved;
    uint8_t *fst = fixture.bytes + FIXTURE_FST;

    assert(gc_disc_decode(fixture.bytes, FIXTURE_BYTES, &disc) == GC_DISC_IMAGE_OK);
    saved = disc.banner.rgba;
    put_u32(fixture.bytes + 0x428, UINT32_MAX);
    assert(gc_disc_decode(fixture.bytes, FIXTURE_BYTES, &disc) ==
           GC_DISC_IMAGE_FILESYSTEM);
    assert(disc.banner.rgba == saved);
    put_u32(fixture.bytes + 0x428, 100);
    put_u32(fst + 8, UINT32_MAX);
    assert(gc_disc_decode(fixture.bytes, FIXTURE_BYTES, &disc) ==
           GC_DISC_IMAGE_FILESYSTEM);
    put_u32(fst + 8, 5);
    put_u32(fst + 16, 2);
    assert(gc_disc_decode(fixture.bytes, FIXTURE_BYTES, &disc) ==
           GC_DISC_IMAGE_FILESYSTEM);
    put_u32(fst + 16, 0);
    put_u32(fst + 20, 6);
    assert(gc_disc_decode(fixture.bytes, FIXTURE_BYTES, &disc) ==
           GC_DISC_IMAGE_FILESYSTEM);
    put_u32(fst + 20, 3);
    put_u32(fst + 40, (uint32_t)FIXTURE_BYTES - 1);
    assert(gc_disc_decode(fixture.bytes, FIXTURE_BYTES, &disc) ==
           GC_DISC_IMAGE_FILESYSTEM);
    put_u32(fst + 40, (uint32_t)FIXTURE_BANNER);
    put_u32(fst + 36, 0x00ffffff);
    assert(gc_disc_decode(fixture.bytes, FIXTURE_BYTES, &disc) ==
           GC_DISC_IMAGE_FILESYSTEM);
    put_u32(fst + 36, 20);
    fst_entry(fst + 48, false, 20, (unsigned)FIXTURE_BANNER,
              (unsigned)fixture.banner_size);
    assert(gc_disc_decode(fixture.bytes, FIXTURE_BYTES, &disc) ==
           GC_DISC_IMAGE_FILESYSTEM);
    fst_entry(fst + 48, false, 32, 0x700, 4);
    fixture.bytes[FIXTURE_BANNER] = 'X';
    assert(gc_disc_decode(fixture.bytes, FIXTURE_BYTES, &disc) == GC_DISC_IMAGE_BANNER);
    assert(disc.banner.rgba == saved);
    assert(gc_disc_banner_decode(fixture.bytes + FIXTURE_BANNER,
                                 fixture.banner_size - 1, GC_REGION_USA,
                                 &disc) == GC_DISC_IMAGE_BANNER);
    assert(disc.banner.rgba == saved);
    fixture.bytes[0x1c] = 0;
    assert(gc_disc_decode(fixture.bytes, FIXTURE_BYTES, &disc) == GC_DISC_IMAGE_HEADER);
    assert(gc_disc_decode(fixture.bytes, 0x400, &disc) == GC_DISC_IMAGE_HEADER);
    gc_disc_destroy(&disc);
    free(fixture.bytes);
}

static void test_streaming_file(const char *path) {
    DiscFixture fixture = make_fixture(true, GC_REGION_EUROPE);
    FILE *file = fopen(path, "wb");
    GcDisc disc = {0};

    assert(file);
    assert(fwrite(fixture.bytes, 1, FIXTURE_BYTES, file) == FIXTURE_BYTES);
    assert(fclose(file) == 0);
    assert(gc_disc_load(path, &disc) == GC_DISC_IMAGE_OK);
    assert(strcmp(gc_disc_metadata(&disc, GC_LANGUAGE_FRENCH)->full_title,
                  "Full title 2") == 0);
    assert(gc_disc_load("", &disc) == GC_DISC_IMAGE_IO);
    assert(disc.banner.rgba);
    assert(remove(path) == 0);
    gc_disc_destroy(&disc);
    free(fixture.bytes);
}

static void test_dummy_disc_owner_and_metadata(void) {
    GcDisc disc = {0};
    for (unsigned region = GC_REGION_JAPAN; region <= GC_REGION_EUROPE; ++region) {
        assert(gc_disc_create_dummy((gc_region)region, &disc) == GC_DISC_IMAGE_OK);
        assert(disc.simulated && disc.region == (gc_region)region);
        assert(!disc.audio_streaming && !disc.disc_id && !disc.version);
        assert(strcmp(disc.header_title, "Dummy Disc") == 0);
        assert(disc.language_count == (region == GC_REGION_EUROPE ? 6u : 1u));
        assert(disc.banner.width == 96 && disc.banner.height == 32 && disc.banner.rgba);
        const GcDiscMetadata *metadata = gc_disc_metadata(&disc, GC_LANGUAGE_ENGLISH);
        assert(metadata && strcmp(metadata->game_name, "Dummy Disc") == 0);
        assert(strcmp(metadata->full_title, "Dummy Disc") == 0);
        assert(strstr(metadata->full_company, "simulated"));
        assert(strstr(metadata->description, "Simulated"));
        for (unsigned language = GC_LANGUAGE_ENGLISH; language <= GC_LANGUAGE_JAPANESE;
             ++language) {
            metadata = gc_disc_metadata(&disc, (gc_language)language);
            assert(metadata && strcmp(metadata->full_title, "Dummy Disc") == 0);
        }
        for (size_t pixel = 0; pixel < 96u * 32u; ++pixel)
            assert(disc.banner.rgba[pixel * 4 + 3] == 255);
        assert(memcmp(disc.banner.rgba, disc.banner.rgba + (16u * 96u + 5u) * 4, 3));
        uint8_t *pixels = disc.banner.rgba;
        assert(gc_disc_create_dummy((gc_region)-1, &disc) == GC_DISC_IMAGE_ARGUMENT);
        assert(disc.banner.rgba == pixels && disc.simulated);
        assert(gc_disc_create_dummy(GC_REGION_USA, NULL) == GC_DISC_IMAGE_ARGUMENT);
    }
    DiscFixture fixture = make_fixture(false, GC_REGION_USA);
    assert(gc_disc_decode(fixture.bytes, FIXTURE_BYTES, &disc) == GC_DISC_IMAGE_OK);
    assert(!disc.simulated && strcmp(disc.header_title, "Header title") == 0);
    free(fixture.bytes);
    gc_disc_destroy(&disc);
    assert(!disc.banner.rgba && !disc.simulated);
}

int main(int argc, char **argv) {
    test_dummy_disc_owner_and_metadata();
    test_image_and_multilingual_banner();
    test_japanese_bytes_and_optional_banner();
    test_invalid_filesystems_preserve_owner();
    test_streaming_file(argc > 1 ? argv[1] : "disc_test.iso");
    puts(
        "Disc filesystem, native banner pixels, metadata, and streaming tests passed.");
    return 0;
}
