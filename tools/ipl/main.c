#include "gamecube/ipl.h"
#include "console_common/support/endian.h"
#include "output.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t crc32(const uint8_t *bytes, size_t size) {
    uint32_t crc = UINT32_MAX;
    for (size_t offset = 0; offset < size; ++offset) {
        crc ^= bytes[offset];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (crc & 1 ? UINT32_C(0xedb88320) : 0);
    }
    return ~crc;
}

static bool write_blob(const char *directory, const char *name, const uint8_t *bytes,
                       size_t size) {
    char path[640];
    int length = snprintf(path, sizeof(path), "%s/%s", directory, name);
    if (length < 0 || (size_t)length >= sizeof(path))
        return false;
    FILE *file = gc_tool_output_open(path);
    if (!file)
        return false;
    bool okay = fwrite(bytes, 1, size, file) == size;
    if (fclose(file))
        okay = false;
    return okay;
}

static const char *resource_kind(const uint8_t *bytes, size_t size, size_t offset) {
    if (size >= 8 && !memcmp(bytes, "J3D1bmd1", 8))
        return "J3D1_bmd1";
    if (size >= 4 && !memcmp(bytes, "CAFD", 4))
        return "CAFD_camera_animation";
    if (size >= 4 && !memcmp(bytes, "TXH0", 4))
        return "TXH0_texture_collection";
    if (offset == GC_IPL_FONT_ANSI_OFFSET)
        return "ANSI_system_font";
    if (offset == GC_IPL_FONT_SJIS_OFFSET)
        return "SJIS_system_font";
    return "unidentified";
}

static void inspect_j3d(const uint8_t *bytes, size_t size) {
    if (size < 32)
        return;
    size_t offset = 32;
    unsigned count = cc_read_be32(bytes + 12);
    for (unsigned index = 0; index < count && offset <= size && size - offset >= 8;
         ++index) {
        uint32_t section_size = cc_read_be32(bytes + offset + 4);
        if (section_size < 8 || section_size > size - offset)
            break;
        printf("  section %.4s offset=0x%zx size=%u\n", bytes + offset, offset,
               section_size);
        offset += section_size;
    }
}

static bool recover_resources(const uint8_t *rom, const char *directory) {
    for (size_t offset = 0; offset < GC_IPL_ROM_SIZE - 16; ++offset) {
        if (memcmp(rom + offset, "Yay0", 4))
            continue;
        size_t size;
        if (!gc_ipl_yay0_size(rom + offset, GC_IPL_ROM_SIZE - offset, &size) ||
            size == 0 || size > 16 * 1024 * 1024)
            continue;
        uint8_t *decoded = malloc(size);
        size_t consumed;
        if (!decoded)
            return false;
        bool okay = gc_ipl_yay0_decode(rom + offset, GC_IPL_ROM_SIZE - offset, decoded,
                                       size, NULL, &consumed);
        if (!okay) {
            fprintf(stderr, "Invalid Yay0 resource at ROM offset 0x%zx.\n", offset);
            free(decoded);
            return false;
        }
        const char *kind = resource_kind(decoded, size, offset);
        printf("Yay0 ROM=0x%06zx packed=%zu decoded=%zu kind=%s crc32=%08x\n", offset,
               consumed, size, kind, crc32(decoded, size));
        if (!strcmp(kind, "J3D1_bmd1"))
            inspect_j3d(decoded, size);
        if (directory) {
            char name[64];
            snprintf(name, sizeof(name), "yay0_%06zx.bin", offset);
            okay = write_blob(directory, name, decoded, size);
        }
        free(decoded);
        if (!okay)
            return false;
        offset += consumed - 1;
    }
    return true;
}

static bool recover_font(const char *path, const char *directory) {
    GcIplFont font = {0};
    if (!gc_ipl_font_load(path, &font))
        return false;
    printf("ANSI font: %ux%u, cell=%ux%u grid=%ux%u characters=%u..%u\n", font.width,
           font.height, font.cell_width, font.cell_height, font.columns, font.rows,
           font.first_character, font.last_character);
    bool okay = !directory || write_blob(directory, "font_ansi.rgba", font.rgba,
                                         (size_t)font.width * font.height * 4);
    gc_ipl_font_destroy(&font);
    return okay;
}

static bool recover_wordmark(const char *path, const char *directory) {
    GcIplImage image = {0};
    if (!gc_ipl_wordmark_load(path, &image))
        return false;
    printf("Native GAMECUBE wordmark: %ux%u RGBA\n", image.width, image.height);
    bool okay = !directory || write_blob(directory, "wordmark.rgba", image.rgba,
                                         (size_t)image.width * image.height * 4);
    gc_ipl_image_destroy(&image);
    return okay;
}

static void inspect_sections(const uint8_t *rom) {
    /* BS2 startup at +0xb4 walks these source/destination/length triples.
     * In the supplied NTSC 1.0 IPL the initialized sections are in place.
     * Print the actual ROM table instead of pretending a synthetic flat
     * file is a complete runtime memory image.
     */
    size_t table = GC_IPL_BS2_OFFSET + 0x30c;
    for (unsigned index = 0; index < 9; ++index) {
        const uint8_t *entry = rom + table + index * 12;
        uint32_t size = cc_read_be32(entry + 8);
        if (!size)
            break;
        printf("BS2 section source=0x%08x destination=0x%08x size=0x%x\n",
               cc_read_be32(entry), cc_read_be32(entry + 4), size);
    }
}

int main(int argc, char **argv) {
    bool prepare = argc >= 2 && !strcmp(argv[1], "prepare");
    bool inspect = argc >= 2 && !strcmp(argv[1], "inspect");
    if ((!prepare && !inspect) || (prepare && argc != 4) || (inspect && argc != 3)) {
        fprintf(stderr, "Usage: gc-ipl-tool inspect INPUT_IPL\n"
                        "       gc-ipl-tool prepare INPUT_IPL Files/Recovery/REGION\n");
        return 2;
    }
    const char *directory = prepare ? argv[3] : NULL;
    if (directory && !gc_tool_output_directory(directory)) {
        fprintf(stderr,
                "Recovery output must be a writable relative directory under Files.\n");
        return 2;
    }
    uint8_t *rom = NULL;
    if (!gc_ipl_rom_read(argv[2], &rom)) {
        fprintf(stderr, "Input must be a readable 2 MiB IPL ROM.\n");
        free(rom);
        return 1;
    }
    printf("IPL bytes=%zu crc32=%08x\n", GC_IPL_ROM_SIZE, crc32(rom, GC_IPL_ROM_SIZE));
    bool descrambled =
        cc_read_be32(rom + GC_IPL_SCRAMBLED_START) == UINT32_C(0x3c800011);
    bool okay = descrambled || gc_ipl_descramble(rom, GC_IPL_ROM_SIZE);
    okay = okay && cc_read_be32(rom + GC_IPL_SCRAMBLED_START) == UINT32_C(0x3c800011);
    if (!okay) {
        fprintf(stderr,
                "Decoded IPL boot instruction does not match the supported ROMs.\n");
        free(rom);
        return 1;
    }
    inspect_sections(rom);
    if (directory) {
        okay = write_blob(directory, "IPL.descrambled.bin", rom, GC_IPL_ROM_SIZE) &&
               write_blob(directory, "IPL.bs2.bin", rom + GC_IPL_BS2_OFFSET,
                          GC_IPL_SCRAMBLED_END - GC_IPL_BS2_OFFSET);
    }
    if (okay)
        okay = recover_resources(rom, directory) && recover_font(argv[2], directory) &&
               recover_wordmark(argv[2], directory);
    free(rom);
    if (!okay)
        fprintf(stderr, "IPL recovery failed.\n");
    return okay ? 0 : 1;
}
