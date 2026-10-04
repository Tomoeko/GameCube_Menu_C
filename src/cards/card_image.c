#include "console_common/support/host.h"
#include "gamecube/card_image.h"
#include "card_checksum.h"
#include "console_common/support/endian.h"
#include "console_common/support/atomic_file.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Independently authored from the native card layout described in YAGCD chapter 12.
 * Its directory checksum offsets have a typo: 127 * 64 byte entries occupy 0x1fc0. */
enum {
    HEADER_CHECKSUM_OFFSET = 0x1fc,
    DIRECTORY_COUNTER_OFFSET = 0x1ffa,
    DIRECTORY_CHECKSUM_OFFSET = 0x1ffc,
    DIRECTORY_ENTRY_BYTES = 64,
    BAT_MAP_OFFSET = 10,
    PERMISSION_NO_COPY = 8,
    PERMISSION_NO_MOVE = 16
};

static uint8_t *directory_bytes(const gc_card_image *image, unsigned copy) {
    return image->bytes + (size_t)(1 + copy) * GC_CARD_BLOCK_BYTES;
}

static uint8_t *bat_bytes(const gc_card_image *image, unsigned copy) {
    return image->bytes + (size_t)(3 + copy) * GC_CARD_BLOCK_BYTES;
}

static uint16_t bat_next(const uint8_t *bat, unsigned block) {
    return cc_read_be16(bat + BAT_MAP_OFFSET +
                        (size_t)(block - GC_CARD_SYSTEM_BLOCKS) * 2);
}

static void bat_set(uint8_t *bat, unsigned block, uint16_t next) {
    cc_write_be16(bat + BAT_MAP_OFFSET + (size_t)(block - GC_CARD_SYSTEM_BLOCKS) * 2,
                  next);
}

static bool empty_entry(const uint8_t *entry) {
    return cc_read_be32(entry) == UINT32_MAX;
}

static bool newer_counter(uint16_t left, uint16_t right) {
    return left != right && (uint16_t)(left - right) < 0x8000u;
}

static gc_card_image_result select_copies(gc_card_image *image) {
    bool directory_valid[2];
    bool bat_valid[2];
    unsigned copy;

    for (copy = 0; copy < 2; ++copy) {
        const uint8_t *directory = directory_bytes(image, copy);
        const uint8_t *bat = bat_bytes(image, copy);
        directory_valid[copy] =
            gc_card_checksum_valid(directory, DIRECTORY_CHECKSUM_OFFSET,
                                   directory + DIRECTORY_CHECKSUM_OFFSET);
        bat_valid[copy] = gc_card_checksum_valid(bat + 4, GC_CARD_BLOCK_BYTES - 4, bat);
    }
    if ((!directory_valid[0] && !directory_valid[1]) ||
        (!bat_valid[0] && !bat_valid[1]))
        return GC_CARD_IMAGE_CHECKSUM;
    image->active_directory = directory_valid[0] ? 0 : 1;
    image->active_bat = bat_valid[0] ? 0 : 1;
    if (directory_valid[0] && directory_valid[1] &&
        newer_counter(
            cc_read_be16(directory_bytes(image, 1) + DIRECTORY_COUNTER_OFFSET),
            cc_read_be16(directory_bytes(image, 0) + DIRECTORY_COUNTER_OFFSET)))
        image->active_directory = 1;
    if (bat_valid[0] && bat_valid[1] &&
        newer_counter(cc_read_be16(bat_bytes(image, 1) + 4),
                      cc_read_be16(bat_bytes(image, 0) + 4)))
        image->active_bat = 1;
    return GC_CARD_IMAGE_OK;
}

static gc_card_image_result validate_chains(const gc_card_image *image) {
    bool used[GC_CARD_MAX_BLOCKS] = {false};
    const uint8_t *directory = directory_bytes(image, image->active_directory);
    const uint8_t *bat = bat_bytes(image, image->active_bat);
    unsigned slot;
    unsigned free_blocks = 0;
    unsigned block;

    for (slot = 0; slot < GC_CARD_FILE_LIMIT; ++slot) {
        const uint8_t *entry = directory + slot * DIRECTORY_ENTRY_BYTES;
        unsigned remaining;

        if (empty_entry(entry))
            continue;
        remaining = cc_read_be16(entry + 0x38);
        block = cc_read_be16(entry + 0x36);
        if (!remaining || remaining > image->block_count - GC_CARD_SYSTEM_BLOCKS)
            return GC_CARD_IMAGE_CHAIN;
        while (remaining) {
            uint16_t next;

            if (block < GC_CARD_SYSTEM_BLOCKS || block >= image->block_count ||
                used[block])
                return GC_CARD_IMAGE_CHAIN;
            used[block] = true;
            next = bat_next(bat, block);
            --remaining;
            if (!remaining && next != UINT16_MAX)
                return GC_CARD_IMAGE_CHAIN;
            block = next;
        }
    }
    for (block = GC_CARD_SYSTEM_BLOCKS; block < image->block_count; ++block) {
        uint16_t next = bat_next(bat, block);
        if ((next != 0) != used[block])
            return GC_CARD_IMAGE_CHAIN;
        if (!used[block])
            ++free_blocks;
    }
    if (cc_read_be16(bat + 6) != free_blocks)
        return GC_CARD_IMAGE_CHAIN;
    return GC_CARD_IMAGE_OK;
}

gc_card_image_result gc_card_image_read_file(const gc_card_image *image,
                                             size_t file_index, size_t offset,
                                             void *output, size_t length) {
    const uint8_t *directory;
    const uint8_t *entry;
    const uint8_t *bat;
    uint8_t *destination = output;
    size_t file_bytes;
    unsigned block;
    size_t skip_blocks;

    if (!image || !image->bytes || file_index >= image->card.file_count ||
        (length && !output))
        return GC_CARD_IMAGE_ARGUMENT;
    file_bytes = (size_t)image->card.files[file_index].blocks * GC_CARD_BLOCK_BYTES;
    if (offset > file_bytes || length > file_bytes - offset)
        return GC_CARD_IMAGE_ARGUMENT;
    directory = directory_bytes(image, image->active_directory);
    entry = directory + image->files[file_index].directory_slot * DIRECTORY_ENTRY_BYTES;
    bat = bat_bytes(image, image->active_bat);
    block = cc_read_be16(entry + 0x36);
    skip_blocks = offset / GC_CARD_BLOCK_BYTES;
    while (skip_blocks--) {
        if (block < GC_CARD_SYSTEM_BLOCKS || block >= image->block_count)
            return GC_CARD_IMAGE_CHAIN;
        block = bat_next(bat, block);
    }
    offset %= GC_CARD_BLOCK_BYTES;
    while (length) {
        size_t span = GC_CARD_BLOCK_BYTES - offset;

        if (block < GC_CARD_SYSTEM_BLOCKS || block >= image->block_count)
            return GC_CARD_IMAGE_CHAIN;
        if (span > length)
            span = length;
        memcpy(destination, image->bytes + (size_t)block * GC_CARD_BLOCK_BYTES + offset,
               span);
        destination += span;
        length -= span;
        offset = 0;
        block = bat_next(bat, block);
    }
    return GC_CARD_IMAGE_OK;
}

static void display_comment(char *destination, const uint8_t *source, size_t length) {
    size_t index;

    for (index = 0; index < length && source[index]; ++index) {
        uint8_t value = source[index];
        /* Preserve source bytes in the image; the ASCII preview avoids controls. */
        destination[index] = value >= 0x20 && value < 0x7f ? (char)value : '?';
    }
    destination[index] = '\0';
}

static void recover_metadata(gc_card_image *image) {
    const uint8_t *directory = directory_bytes(image, image->active_directory);
    unsigned slot;

    memset(&image->card, 0, sizeof(image->card));
    memset(image->files, 0, sizeof(image->files));
    image->card.status = GC_CARD_READY;
    image->card.capacity_blocks =
        (uint16_t)(image->block_count - GC_CARD_SYSTEM_BLOCKS);
    for (slot = 0; slot < GC_CARD_FILE_LIMIT; ++slot) {
        const uint8_t *entry = directory + slot * DIRECTORY_ENTRY_BYTES;
        gc_card_file *file;
        gc_card_image_file_info *info;
        size_t file_index;
        uint8_t comments[64];

        if (empty_entry(entry))
            continue;
        file_index = image->card.file_count++;
        file = &image->card.files[file_index];
        info = &image->files[file_index];
        memcpy(file->game_code, entry, 4);
        memcpy(file->maker_code, entry + 4, 2);
        memcpy(file->filename, entry + 8, 32);
        file->blocks = cc_read_be16(entry + 0x38);
        file->allow_copy = (entry[0x34] & PERMISSION_NO_COPY) == 0;
        file->allow_move = (entry[0x34] & PERMISSION_NO_MOVE) == 0;
        info->directory_slot = slot;
        info->image_flags = entry[7];
        info->modified_seconds = cc_read_be32(entry + 0x28);
        info->image_offset = cc_read_be32(entry + 0x2c);
        info->icon_formats = cc_read_be16(entry + 0x30);
        info->icon_speeds = cc_read_be16(entry + 0x32);
        info->permissions = entry[0x34];
        info->copy_count = entry[0x35];
        info->comment_offset = cc_read_be32(entry + 0x3c);
        if (info->comment_offset != UINT32_MAX &&
            info->comment_offset % GC_CARD_BLOCK_BYTES <= GC_CARD_BLOCK_BYTES - 64 &&
            gc_card_image_read_file(image, file_index, info->comment_offset, comments,
                                    sizeof(comments)) == GC_CARD_IMAGE_OK) {
            display_comment(file->title, comments, 32);
            display_comment(file->comment, comments + 32, 32);
        }
        if (!file->title[0])
            display_comment(file->title, entry + 8, 32);
    }
}

static gc_card_image_result validate_image(gc_card_image *image) {
    gc_card_image_result result;
    uint16_t size_mbits;

    if (image->byte_count < 64u * GC_CARD_BLOCK_BYTES ||
        image->byte_count > (size_t)GC_CARD_MAX_BLOCKS * GC_CARD_BLOCK_BYTES ||
        image->byte_count % GC_CARD_BLOCK_BYTES)
        return GC_CARD_IMAGE_FORMAT;
    image->block_count = (unsigned)(image->byte_count / GC_CARD_BLOCK_BYTES);
    size_mbits = cc_read_be16(image->bytes + 0x22);
    image->encoding = cc_read_be16(image->bytes + 0x24);
    if ((unsigned)size_mbits * 16 != image->block_count || image->encoding > 1 ||
        (image->block_count & (image->block_count - 1)) != 0)
        return GC_CARD_IMAGE_FORMAT;
    if (!gc_card_checksum_valid(image->bytes, HEADER_CHECKSUM_OFFSET,
                                image->bytes + HEADER_CHECKSUM_OFFSET))
        return GC_CARD_IMAGE_CHECKSUM;
    result = select_copies(image);
    if (result != GC_CARD_IMAGE_OK)
        return result;
    result = validate_chains(image);
    if (result != GC_CARD_IMAGE_OK)
        return result;
    recover_metadata(image);
    return GC_CARD_IMAGE_OK;
}

static bool physical_size_valid(size_t byte_count) {
    size_t block_count;

    if (byte_count < 64u * GC_CARD_BLOCK_BYTES ||
        byte_count > (size_t)GC_CARD_MAX_BLOCKS * GC_CARD_BLOCK_BYTES ||
        byte_count % GC_CARD_BLOCK_BYTES)
        return false;
    block_count = byte_count / GC_CARD_BLOCK_BYTES;
    return (block_count & (block_count - 1)) == 0;
}

static bool header_valid(const gc_card_image *image) {
    return (unsigned)cc_read_be16(image->bytes + 0x22) * 16 == image->block_count &&
           cc_read_be16(image->bytes + 0x24) <= 1 &&
           gc_card_checksum_valid(image->bytes, HEADER_CHECKSUM_OFFSET,
                                  image->bytes + HEADER_CHECKSUM_OFFSET);
}

static gc_card_image_result classify_present(gc_card_image *image,
                                             uint16_t fallback_encoding) {
    gc_card_image_result result;
    bool erased = true;

    if (!physical_size_valid(image->byte_count))
        return GC_CARD_IMAGE_FORMAT;
    result = validate_image(image);
    image->integrity = result;
    if (result == GC_CARD_IMAGE_OK)
        return GC_CARD_IMAGE_OK;
    image->block_count = (unsigned)(image->byte_count / GC_CARD_BLOCK_BYTES);
    image->encoding =
        header_valid(image) ? cc_read_be16(image->bytes + 0x24) : fallback_encoding;
    for (size_t index = 0; index < image->byte_count; ++index) {
        if (image->bytes[index] != 0xff) {
            erased = false;
            break;
        }
    }
    memset(&image->card, 0, sizeof(image->card));
    memset(image->files, 0, sizeof(image->files));
    image->active_directory = 0;
    image->active_bat = 0;
    image->card.status = erased ? GC_CARD_UNFORMATTED : GC_CARD_DAMAGED;
    image->card.capacity_blocks =
        (uint16_t)(image->block_count - GC_CARD_SYSTEM_BLOCKS);
    return GC_CARD_IMAGE_OK;
}

void gc_card_image_free(gc_card_image *image) {
    if (!image)
        return;
    free(image->bytes);
    memset(image, 0, sizeof(*image));
}

static void replace_image(gc_card_image *destination, gc_card_image *source) {
    gc_card_image_free(destination);
    *destination = *source;
    memset(source, 0, sizeof(*source));
}

gc_card_image_result gc_card_image_clone(gc_card_image *destination,
                                         const gc_card_image *source) {
    gc_card_image candidate;

    if (!destination || !source || !source->bytes ||
        !physical_size_valid(source->byte_count))
        return GC_CARD_IMAGE_ARGUMENT;
    candidate = *source;
    candidate.bytes = malloc(source->byte_count);
    if (!candidate.bytes)
        return GC_CARD_IMAGE_MEMORY;
    memcpy(candidate.bytes, source->bytes, source->byte_count);
    replace_image(destination, &candidate);
    return GC_CARD_IMAGE_OK;
}

gc_card_image_result gc_card_image_attach(gc_card_image *image, const uint8_t *bytes,
                                          size_t byte_count,
                                          uint16_t fallback_encoding) {
    gc_card_image candidate = {0};
    gc_card_image_result result;

    if (!image || !bytes || fallback_encoding > 1)
        return GC_CARD_IMAGE_ARGUMENT;
    if (!physical_size_valid(byte_count))
        return GC_CARD_IMAGE_FORMAT;
    candidate.bytes = malloc(byte_count);
    if (!candidate.bytes)
        return GC_CARD_IMAGE_MEMORY;
    candidate.byte_count = byte_count;
    memcpy(candidate.bytes, bytes, byte_count);
    result = classify_present(&candidate, fallback_encoding);
    if (result == GC_CARD_IMAGE_OK)
        replace_image(image, &candidate);
    gc_card_image_free(&candidate);
    return result;
}

gc_card_image_result gc_card_image_read(gc_card_image *image, const uint8_t *bytes,
                                        size_t byte_count) {
    gc_card_image candidate = {0};
    gc_card_image_result result;

    if (!image || !bytes || byte_count < 64u * GC_CARD_BLOCK_BYTES ||
        byte_count > (size_t)GC_CARD_MAX_BLOCKS * GC_CARD_BLOCK_BYTES)
        return GC_CARD_IMAGE_ARGUMENT;
    candidate.bytes = malloc(byte_count);
    if (!candidate.bytes)
        return GC_CARD_IMAGE_MEMORY;
    candidate.byte_count = byte_count;
    memcpy(candidate.bytes, bytes, byte_count);
    result = validate_image(&candidate);
    if (result == GC_CARD_IMAGE_OK)
        replace_image(image, &candidate);
    gc_card_image_free(&candidate);
    return result;
}

static gc_card_image_result load_image(gc_card_image *image, const char *path,
                                       bool allow_damaged, uint16_t fallback_encoding) {
    gc_card_image candidate = {0};
    gc_card_image_result result = GC_CARD_IMAGE_IO;
    FILE *file;
    long length;

    if (!image || !path || fallback_encoding > 1)
        return GC_CARD_IMAGE_ARGUMENT;
    file = cc_host_fopen(path, "rb");
    if (!file)
        return GC_CARD_IMAGE_IO;
    if (fseek(file, 0, SEEK_END) != 0)
        goto close_file;
    length = ftell(file);
    if (length < 64L * GC_CARD_BLOCK_BYTES ||
        length > (long)GC_CARD_MAX_BLOCKS * GC_CARD_BLOCK_BYTES) {
        result = GC_CARD_IMAGE_FORMAT;
        goto close_file;
    }
    if (fseek(file, 0, SEEK_SET) != 0)
        goto close_file;
    candidate.byte_count = (size_t)length;
    candidate.bytes = malloc(candidate.byte_count);
    if (!candidate.bytes) {
        result = GC_CARD_IMAGE_MEMORY;
        goto close_file;
    }
    if (fread(candidate.bytes, 1, candidate.byte_count, file) != candidate.byte_count)
        goto close_file;
    result = allow_damaged ? classify_present(&candidate, fallback_encoding)
                           : validate_image(&candidate);
    if (result == GC_CARD_IMAGE_OK)
        replace_image(image, &candidate);

close_file:
    fclose(file);
    gc_card_image_free(&candidate);
    return result;
}

gc_card_image_result gc_card_image_load(gc_card_image *image, const char *path) {
    return load_image(image, path, false, 0);
}

gc_card_image_result gc_card_image_open_present(gc_card_image *image, const char *path,
                                                uint16_t fallback_encoding) {
    return load_image(image, path, true, fallback_encoding);
}

static void initialize_filesystem(gc_card_image *image) {
    unsigned copy;

    for (copy = 0; copy < 2; ++copy) {
        uint8_t *directory = directory_bytes(image, copy);
        uint8_t *bat = bat_bytes(image, copy);

        memset(directory, 0xff, GC_CARD_BLOCK_BYTES);
        cc_write_be16(directory + DIRECTORY_COUNTER_OFFSET, 0);
        gc_card_checksum_write(directory, DIRECTORY_CHECKSUM_OFFSET,
                               directory + DIRECTORY_CHECKSUM_OFFSET);
        memset(bat, 0, GC_CARD_BLOCK_BYTES);
        cc_write_be16(bat + 6, (uint16_t)(image->block_count - GC_CARD_SYSTEM_BLOCKS));
        cc_write_be16(bat + 8, GC_CARD_SYSTEM_BLOCKS - 1);
        gc_card_checksum_write(bat + 4, GC_CARD_BLOCK_BYTES - 4, bat);
    }
}

static void initialize_header(gc_card_image *image, uint16_t encoding,
                              uint64_t format_ticks) {
    memset(image->bytes, 0xff, GC_CARD_BLOCK_BYTES);
    memset(image->bytes, 0, 0x26);
    /* A local serial has no account, host or hardware identifier embedded in it. */
    memcpy(image->bytes, "GAMECUBE_C", 10);
    for (unsigned index = 0; index < 8; ++index)
        image->bytes[0x0c + index] = (uint8_t)(format_ticks >> (56 - index * 8));
    cc_write_be16(image->bytes + 0x22, (uint16_t)(image->block_count / 16));
    cc_write_be16(image->bytes + 0x24, encoding);
    cc_write_be16(image->bytes + 0x1fa, 0);
    gc_card_checksum_write(image->bytes, HEADER_CHECKSUM_OFFSET,
                           image->bytes + HEADER_CHECKSUM_OFFSET);
}

gc_card_image_result gc_card_image_create(gc_card_image *image, unsigned block_count,
                                          uint16_t encoding, uint64_t format_ticks) {
    gc_card_image candidate = {0};

    if (!image || block_count < 64 || block_count > GC_CARD_MAX_BLOCKS ||
        encoding > 1 || (block_count & (block_count - 1)) != 0)
        return GC_CARD_IMAGE_ARGUMENT;
    candidate.byte_count = (size_t)block_count * GC_CARD_BLOCK_BYTES;
    candidate.block_count = block_count;
    candidate.bytes = malloc(candidate.byte_count);
    if (!candidate.bytes)
        return GC_CARD_IMAGE_MEMORY;
    memset(candidate.bytes, 0xff, candidate.byte_count);
    initialize_header(&candidate, encoding, format_ticks);
    initialize_filesystem(&candidate);
    candidate.encoding = encoding;
    recover_metadata(&candidate);
    replace_image(image, &candidate);
    return GC_CARD_IMAGE_OK;
}

gc_card_image_result gc_card_image_write(const gc_card_image *image, const char *path) {
    if (!image || !image->bytes || !path)
        return GC_CARD_IMAGE_ARGUMENT;
    return cc_atomic_file_replace(path, image->bytes, image->byte_count)
               ? GC_CARD_IMAGE_OK
               : GC_CARD_IMAGE_IO;
}

static gc_card_image_result prepare_edit(const gc_card_image *image,
                                         gc_card_image *candidate) {
    gc_card_image_result result;
    unsigned next_directory;
    unsigned next_bat;

    result = gc_card_image_read(candidate, image->bytes, image->byte_count);
    if (result != GC_CARD_IMAGE_OK)
        return result;
    next_directory = 1 - candidate->active_directory;
    next_bat = 1 - candidate->active_bat;
    memcpy(directory_bytes(candidate, next_directory),
           directory_bytes(candidate, candidate->active_directory),
           GC_CARD_BLOCK_BYTES);
    memcpy(bat_bytes(candidate, next_bat), bat_bytes(candidate, candidate->active_bat),
           GC_CARD_BLOCK_BYTES);
    candidate->active_directory = next_directory;
    candidate->active_bat = next_bat;
    return GC_CARD_IMAGE_OK;
}

static void finish_edit(gc_card_image *image) {
    uint8_t *directory = directory_bytes(image, image->active_directory);
    uint8_t *bat = bat_bytes(image, image->active_bat);

    cc_write_be16(directory + DIRECTORY_COUNTER_OFFSET,
                  (uint16_t)(cc_read_be16(directory + DIRECTORY_COUNTER_OFFSET) + 1));
    cc_write_be16(bat + 4, (uint16_t)(cc_read_be16(bat + 4) + 1));
    gc_card_checksum_write(directory, DIRECTORY_CHECKSUM_OFFSET,
                           directory + DIRECTORY_CHECKSUM_OFFSET);
    gc_card_checksum_write(bat + 4, GC_CARD_BLOCK_BYTES - 4, bat);
    recover_metadata(image);
}

gc_card_image_result gc_card_image_format(gc_card_image *image) {
    gc_card_image candidate = {0};
    gc_card_image_result result;

    if (!image || !image->bytes || image->encoding > 1 ||
        !physical_size_valid(image->byte_count))
        return GC_CARD_IMAGE_ARGUMENT;
    result = gc_card_image_clone(&candidate, image);
    if (result != GC_CARD_IMAGE_OK)
        return result;
    candidate.block_count = (unsigned)(candidate.byte_count / GC_CARD_BLOCK_BYTES);
    if (!header_valid(&candidate))
        initialize_header(&candidate, candidate.encoding, 0);
    initialize_filesystem(&candidate);
    candidate.active_directory = 0;
    candidate.active_bat = 0;
    candidate.integrity = GC_CARD_IMAGE_OK;
    recover_metadata(&candidate);
    replace_image(image, &candidate);
    return GC_CARD_IMAGE_OK;
}

static void erase_in_candidate(gc_card_image *image, size_t file_index) {
    uint8_t *directory = directory_bytes(image, image->active_directory);
    uint8_t *bat = bat_bytes(image, image->active_bat);
    uint8_t *entry =
        directory + image->files[file_index].directory_slot * DIRECTORY_ENTRY_BYTES;
    unsigned block = cc_read_be16(entry + 0x36);
    unsigned remaining = cc_read_be16(entry + 0x38);
    uint16_t free_blocks = cc_read_be16(bat + 6);

    while (remaining--) {
        uint16_t next = bat_next(bat, block);
        bat_set(bat, block, 0);
        ++free_blocks;
        block = next;
    }
    cc_write_be16(bat + 6, free_blocks);
    memset(entry, 0xff, DIRECTORY_ENTRY_BYTES);
}

gc_card_image_result gc_card_image_erase(gc_card_image *image, size_t file_index) {
    gc_card_image candidate = {0};
    gc_card_image_result result;

    if (!image || !image->bytes || file_index >= image->card.file_count)
        return GC_CARD_IMAGE_ARGUMENT;
    result = prepare_edit(image, &candidate);
    if (result != GC_CARD_IMAGE_OK)
        return result;
    erase_in_candidate(&candidate, file_index);
    finish_edit(&candidate);
    replace_image(image, &candidate);
    return GC_CARD_IMAGE_OK;
}

static bool duplicate_entry(const gc_card_image *source, const gc_card_image *target,
                            size_t file_index) {
    const gc_card_file *file = &source->card.files[file_index];
    size_t index;

    for (index = 0; index < target->card.file_count; ++index) {
        const gc_card_file *other = &target->card.files[index];
        if (memcmp(file->game_code, other->game_code, 4) == 0 &&
            memcmp(file->maker_code, other->maker_code, 2) == 0 &&
            memcmp(file->filename, other->filename, 32) == 0)
            return true;
    }
    return false;
}

static void copy_to_candidate(const gc_card_image *source, gc_card_image *target,
                              size_t file_index) {
    const uint8_t *source_directory = directory_bytes(source, source->active_directory);
    const uint8_t *source_entry =
        source_directory +
        source->files[file_index].directory_slot * DIRECTORY_ENTRY_BYTES;
    const uint8_t *source_bat = bat_bytes(source, source->active_bat);
    uint8_t *directory = directory_bytes(target, target->active_directory);
    uint8_t *bat = bat_bytes(target, target->active_bat);
    unsigned slot = 0;
    uint8_t *entry;
    unsigned source_block = cc_read_be16(source_entry + 0x36);
    unsigned previous = 0;
    unsigned first_block = 0;
    unsigned target_block;
    unsigned remaining = cc_read_be16(source_entry + 0x38);

    while (!empty_entry(directory + slot * DIRECTORY_ENTRY_BYTES))
        ++slot;
    entry = directory + slot * DIRECTORY_ENTRY_BYTES;
    memcpy(entry, source_entry, DIRECTORY_ENTRY_BYTES);
    entry[0x35] = (uint8_t)(entry[0x35] + 1);
    for (target_block = GC_CARD_SYSTEM_BLOCKS; remaining; ++target_block) {
        if (bat_next(bat, target_block))
            continue;
        memcpy(target->bytes + (size_t)target_block * GC_CARD_BLOCK_BYTES,
               source->bytes + (size_t)source_block * GC_CARD_BLOCK_BYTES,
               GC_CARD_BLOCK_BYTES);
        if (!first_block)
            first_block = target_block;
        if (previous)
            bat_set(bat, previous, (uint16_t)target_block);
        bat_set(bat, target_block, UINT16_MAX);
        previous = target_block;
        source_block = bat_next(source_bat, source_block);
        --remaining;
    }
    cc_write_be16(entry + 0x36, (uint16_t)first_block);
    cc_write_be16(bat + 6,
                  (uint16_t)(cc_read_be16(bat + 6) - cc_read_be16(entry + 0x38)));
    cc_write_be16(bat + 8, (uint16_t)previous);
}

gc_card_image_result gc_card_image_copy(gc_card_image *source, gc_card_image *target,
                                        size_t file_index, bool move) {
    gc_card_image source_candidate = {0};
    gc_card_image target_candidate = {0};
    gc_card_image_result result;
    const gc_card_file *file;

    if (!source || !target || source == target || !source->bytes || !target->bytes ||
        file_index >= source->card.file_count)
        return GC_CARD_IMAGE_ARGUMENT;
    file = &source->card.files[file_index];
    if ((move && !file->allow_move) || (!move && !file->allow_copy))
        return GC_CARD_IMAGE_PERMISSION;
    if (source->encoding != target->encoding)
        return GC_CARD_IMAGE_ENCODING;
    if (target->card.file_count >= GC_CARD_FILE_LIMIT ||
        file->blocks > gc_card_free_blocks(&target->card))
        return GC_CARD_IMAGE_NO_SPACE;
    if (duplicate_entry(source, target, file_index))
        return GC_CARD_IMAGE_DUPLICATE;
    result = prepare_edit(target, &target_candidate);
    if (result != GC_CARD_IMAGE_OK)
        return result;
    if (move) {
        result = prepare_edit(source, &source_candidate);
        if (result != GC_CARD_IMAGE_OK)
            goto release_candidates;
    }
    copy_to_candidate(source, &target_candidate, file_index);
    finish_edit(&target_candidate);
    if (move) {
        erase_in_candidate(&source_candidate, file_index);
        finish_edit(&source_candidate);
        replace_image(source, &source_candidate);
    }
    replace_image(target, &target_candidate);

release_candidates:
    gc_card_image_free(&source_candidate);
    gc_card_image_free(&target_candidate);
    return result;
}

const char *gc_card_image_result_text(gc_card_image_result result) {
    switch (result) {
        case GC_CARD_IMAGE_OK:
            return "okay";
        case GC_CARD_IMAGE_ARGUMENT:
            return "invalid card operation";
        case GC_CARD_IMAGE_MEMORY:
            return "memory allocation failed";
        case GC_CARD_IMAGE_IO:
            return "card image could not be saved or opened";
        case GC_CARD_IMAGE_FORMAT:
            return "card image has an unsupported layout";
        case GC_CARD_IMAGE_CHECKSUM:
            return "card filesystem checksums are damaged";
        case GC_CARD_IMAGE_CHAIN:
            return "card allocation chains are damaged";
        case GC_CARD_IMAGE_PERMISSION:
            return "the save does not allow this operation";
        case GC_CARD_IMAGE_NO_SPACE:
            return "the target card has insufficient blocks or file slots";
        case GC_CARD_IMAGE_DUPLICATE:
            return "the target card already contains this save";
        case GC_CARD_IMAGE_ENCODING:
            return "the cards use different text encodings";
    }
    return "unknown card error";
}
