#define _POSIX_C_SOURCE 200809L

#include "gamecube/ipl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static unsigned failures;

static void check(bool condition, const char *name) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", name);
        ++failures;
    }
}

static void put_be16(uint8_t *bytes, unsigned value) {
    bytes[0] = (uint8_t)(value >> 8);
    bytes[1] = (uint8_t)value;
}

static void test_scrambler(void) {
    static const uint8_t first_mask[16] = {0x89, 0x7e, 0x47, 0x7f, 0xf4, 0x42,
                                           0x3f, 0xe2, 0xa1, 0x44, 0x32, 0xa6,
                                           0x30, 0x13, 0xbc, 0xd1};
    uint8_t *rom = calloc(GC_IPL_ROM_SIZE, 1);
    check(rom != NULL, "allocate synthetic ROM");
    if (!rom)
        return;
    rom[0] = 0x42;
    rom[GC_IPL_SCRAMBLED_END] = 0x53;
    check(!gc_ipl_descramble(NULL, GC_IPL_ROM_SIZE), "reject null ROM");
    check(!gc_ipl_descramble(rom, GC_IPL_ROM_SIZE - 1), "reject short ROM");
    check(gc_ipl_descramble(rom, GC_IPL_ROM_SIZE), "descramble valid ROM size");
    check(memcmp(rom + GC_IPL_SCRAMBLED_START, first_mask, sizeof(first_mask)) == 0,
          "hardware recurrence known vector");
    check(rom[0] == 0x42 && rom[GC_IPL_SCRAMBLED_END] == 0x53,
          "preserve clear copyright and font ranges");
    check(gc_ipl_descramble(rom, GC_IPL_ROM_SIZE), "second transform");
    for (size_t offset = GC_IPL_SCRAMBLED_START; offset < GC_IPL_SCRAMBLED_END;
         ++offset) {
        if (rom[offset]) {
            check(false, "XOR transform is involutive");
            break;
        }
    }
    free(rom);
}

static void test_raw_rom(void) {
    (void)mkdir("Files", 0700);
    char path[] = "Files/ipl-rom-test-XXXXXX";
    int descriptor = mkstemp(path);
    check(descriptor >= 0, "create raw ROM fixture");
    if (descriptor < 0)
        return;
    FILE *file = fdopen(descriptor, "wb");
    check(file != NULL, "open raw ROM fixture");
    if (!file) {
        close(descriptor);
        unlink(path);
        return;
    }
    uint8_t *expected = calloc(GC_IPL_ROM_SIZE, 1);
    check(expected != NULL, "allocate raw ROM fixture");
    if (!expected) {
        fclose(file);
        unlink(path);
        return;
    }
    expected[GC_IPL_SCRAMBLED_START] = 0x42;
    expected[GC_IPL_ROM_SIZE - 1] = 0x53;
    bool written = fwrite(expected, 1, GC_IPL_ROM_SIZE, file) == GC_IPL_ROM_SIZE;
    check(fclose(file) == 0 && written, "write exact raw ROM fixture");
    uint8_t *rom = NULL;
    check(gc_ipl_rom_read(path, &rom), "read exact regular ROM");
    check(rom && !memcmp(rom, expected, GC_IPL_ROM_SIZE),
          "raw reader preserves scrambled bytes");
    free(rom);
    rom = NULL;
    file = fopen(path, "ab");
    check(file != NULL, "open oversized ROM fixture");
    if (file) {
        written = fputc(1, file) != EOF;
        check(fclose(file) == 0 && written, "append oversized ROM byte");
    }
    check(!gc_ipl_rom_read(path, &rom) && !rom, "reject oversized ROM");
    file = fopen(path, "wb");
    check(file != NULL, "open truncated ROM fixture");
    if (file) {
        written = fwrite(expected, 1, GC_IPL_ROM_SIZE - 1, file) == GC_IPL_ROM_SIZE - 1;
        check(fclose(file) == 0 && written, "write truncated ROM");
    }
    check(!gc_ipl_rom_read(path, &rom) && !rom, "reject truncated ROM");
    check(!gc_ipl_rom_read(NULL, &rom) && !rom, "reject null ROM path");
    check(!gc_ipl_rom_read(path, NULL), "reject null ROM output");
    check(!gc_ipl_rom_read("Files", &rom) && !rom, "reject ROM directory");
    check(unlink(path) == 0, "remove raw ROM fixture");
    free(expected);
}

static void test_yay0(void) {
    uint8_t compressed[24] = {'Y', 'a', 'y', '0', 0,    0, 0, 20, 0, 0, 0,   20,
                              0,   0,   0,   22,  0x80, 0, 0, 0,  0, 0, 'A', 1};
    uint8_t output[20];
    size_t decoded = 0;
    size_t consumed = 0;
    check(gc_ipl_yay0_decode(compressed, sizeof(compressed), output, sizeof(output),
                             &decoded, &consumed),
          "decode overlapping extended match");
    check(decoded == sizeof(output) && consumed == sizeof(compressed),
          "report decoded and consumed sizes");
    for (size_t offset = 0; offset < sizeof(output); ++offset)
        check(output[offset] == 'A', "overlapping match reproduces literal");
    check(!gc_ipl_yay0_decode(compressed, sizeof(compressed), output, 19, NULL, NULL),
          "reject inadequate destination capacity");
    for (size_t size = 0; size < sizeof(compressed); ++size)
        check(!gc_ipl_yay0_decode(compressed, size, output, sizeof(output), NULL, NULL),
              "reject truncated mask, link, or chunk stream");
    compressed[20] = 0x10;
    compressed[21] = 1;
    check(!gc_ipl_yay0_decode(compressed, sizeof(compressed), output, sizeof(output),
                              NULL, NULL),
          "reject backward reference before output");
    compressed[20] = 0;
    compressed[21] = 0;
    compressed[23] = 2;
    check(!gc_ipl_yay0_decode(compressed, sizeof(compressed), output, sizeof(output),
                              NULL, NULL),
          "reject match extending past decoded length");
}

static void test_font(void) {
    uint8_t source[65] = {0};
    GcIplFont font = {0};
    GcIplGlyph glyph = {0};
    put_be16(source + 2, 32);
    put_be16(source + 4, 32);
    put_be16(source + 6, 32);
    put_be16(source + 8, 8);
    put_be16(source + 16, 8);
    put_be16(source + 18, 8);
    put_be16(source + 26, 1);
    put_be16(source + 28, 1);
    put_be16(source + 30, 8);
    put_be16(source + 32, 8);
    put_be16(source + 34, 48);
    source[39] = 49;
    source[48] = 5;
    source[49] = 0x1b;
    check(!gc_ipl_font_decode(source, 64, &font), "reject truncated packed font");
    check(gc_ipl_font_decode(source, sizeof(source), &font), "decode synthetic font");
    if (!font.rgba)
        return;
    for (unsigned x = 0; x < 4; ++x) {
        check(font.rgba[x * 4 + 3] == x * 85, "expand native intensity levels");
        check(font.rgba[x * 4] == 255, "font RGB supports tinting");
    }
    check(gc_ipl_font_glyph(&font, 32, &glyph), "font glyph lookup");
    check(glyph.x == 0 && glyph.y == 0 && glyph.width == 8 && glyph.height == 8 &&
              glyph.advance == 5,
          "glyph coordinates and proportional advance");
    check(gc_ipl_font_glyph(&font, 1000, &glyph), "substitute unsupported character");
    check(!gc_ipl_font_decode(source, sizeof(source), &font),
          "reject owned font overwrite");
    gc_ipl_font_destroy(&font);
    check(font.rgba == NULL && font.width == 0, "release and reset font");
}

int main(void) {
    test_raw_rom();
    test_scrambler();
    test_yay0();
    test_font();
    if (failures)
        return 1;
    puts("IPL recovery tests passed.");
    return 0;
}
