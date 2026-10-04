#include "console_common/support/host.h"
#include "gamecube/state.h"
#include "console_common/support/endian.h"
#include "console_common/support/atomic_file.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

enum { STATE_BYTES = 48, STATE_CHECKSUM_OFFSET = 44, STATE_VERSION = 1 };

static uint32_t state_checksum(const uint8_t *bytes) {
    uint32_t crc = UINT32_MAX;
    size_t offset;

    for (offset = 0; offset < STATE_CHECKSUM_OFFSET; ++offset) {
        unsigned bit;
        crc ^= bytes[offset];
        for (bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0);
    }
    return ~crc;
}

static bool language_matches_region(gc_region region, gc_language language) {
    if (region == GC_REGION_JAPAN)
        return language == GC_LANGUAGE_JAPANESE;
    if (region == GC_REGION_USA)
        return language == GC_LANGUAGE_ENGLISH;
    if (region == GC_REGION_EUROPE)
        return language >= GC_LANGUAGE_ENGLISH && language <= GC_LANGUAGE_DUTCH;
    return false;
}

static gc_state_result decode_record(gc_menu *menu, const uint8_t *bytes,
                                     int64_t now_unix_seconds) {
    gc_menu candidate = *menu;
    uint64_t saved_seconds;
    uint32_t fraction;
    unsigned index;

    if (memcmp(bytes, "GCMSTATE", 8) != 0 || cc_read_be16(bytes + 8) != STATE_VERSION ||
        cc_read_be16(bytes + 10) != STATE_BYTES ||
        cc_read_be32(bytes + STATE_CHECKSUM_OFFSET) != state_checksum(bytes))
        return GC_STATE_FORMAT;
    if (bytes[12] != (uint8_t)menu->region)
        return GC_STATE_REGION;
    if (bytes[13] > GC_SOUND_STEREO || bytes[14] > 64 ||
        bytes[15] > GC_LANGUAGE_JAPANESE || bytes[23] != 0)
        return GC_STATE_FORMAT;
    for (index = 36; index < STATE_CHECKSUM_OFFSET; ++index) {
        if (bytes[index] != 0)
            return GC_STATE_FORMAT;
    }
    candidate.settings.sound = (gc_sound)bytes[13];
    candidate.settings.screen_position = (int)bytes[14] + GC_SCREEN_POSITION_MIN;
    candidate.settings.language = (gc_language)bytes[15];
    candidate.clock.year = cc_read_be16(bytes + 16);
    candidate.clock.month = bytes[18];
    candidate.clock.day = bytes[19];
    candidate.clock.hour = bytes[20];
    candidate.clock.minute = bytes[21];
    candidate.clock.second = bytes[22];
    saved_seconds = cc_read_be64(bytes + 24);
    fraction = cc_read_be32(bytes + 32);
    if (saved_seconds > INT64_MAX || fraction > 999999 ||
        !gc_date_time_valid(&candidate.clock) ||
        !language_matches_region(candidate.region, candidate.settings.language))
        return GC_STATE_FORMAT;
    candidate.clock_fraction = (double)fraction / 1000000.0;
    candidate.page = GC_PAGE_CUBE;
    if ((uint64_t)now_unix_seconds > saved_seconds)
        gc_menu_tick(&candidate, (double)((uint64_t)now_unix_seconds - saved_seconds));
    menu->clock = candidate.clock;
    menu->clock_fraction = candidate.clock_fraction;
    menu->settings = candidate.settings;
    menu->clock_changed = false;
    menu->settings_changed = false;
    return GC_STATE_OK;
}

gc_state_result gc_state_load(gc_menu *menu, const char *path,
                              int64_t now_unix_seconds) {
    uint8_t bytes[STATE_BYTES];
    FILE *file;
    size_t length;
    int trailing;
    bool io_error;

    if (!menu || !path || now_unix_seconds < 0)
        return GC_STATE_ARGUMENT;
    file = cc_host_fopen(path, "rb");
    if (!file)
        return errno == ENOENT ? GC_STATE_MISSING : GC_STATE_IO;
    length = fread(bytes, 1, sizeof(bytes), file);
    trailing = fgetc(file);
    io_error = ferror(file) != 0;
    if (fclose(file) != 0)
        io_error = true;
    if (io_error)
        return GC_STATE_IO;
    if (length != sizeof(bytes) || trailing != EOF)
        return GC_STATE_FORMAT;
    return decode_record(menu, bytes, now_unix_seconds);
}

gc_state_result gc_state_save(gc_menu *menu, const char *path,
                              int64_t now_unix_seconds) {
    uint8_t bytes[STATE_BYTES] = {0};
    gc_settings settings;
    gc_date_time clock;

    if (!menu || !path || now_unix_seconds < 0 || !isfinite(menu->clock_fraction) ||
        menu->clock_fraction < 0.0 || menu->clock_fraction >= 1.0)
        return GC_STATE_ARGUMENT;
    settings = menu->page == GC_PAGE_OPTIONS && menu->editing
                   ? menu->settings_before_edit
                   : menu->settings;
    clock = menu->page == GC_PAGE_CALENDAR && menu->editing ? menu->clock_before_edit
                                                            : menu->clock;
    if (!gc_date_time_valid(&clock) || settings.sound < GC_SOUND_MONO ||
        settings.sound > GC_SOUND_STEREO ||
        settings.screen_position < GC_SCREEN_POSITION_MIN ||
        settings.screen_position > GC_SCREEN_POSITION_MAX ||
        !language_matches_region(menu->region, settings.language))
        return GC_STATE_ARGUMENT;
    memcpy(bytes, "GCMSTATE", 8);
    cc_write_be16(bytes + 8, STATE_VERSION);
    cc_write_be16(bytes + 10, STATE_BYTES);
    bytes[12] = (uint8_t)menu->region;
    bytes[13] = (uint8_t)settings.sound;
    bytes[14] = (uint8_t)(settings.screen_position - GC_SCREEN_POSITION_MIN);
    bytes[15] = (uint8_t)settings.language;
    cc_write_be16(bytes + 16, (uint16_t)clock.year);
    bytes[18] = (uint8_t)clock.month;
    bytes[19] = (uint8_t)clock.day;
    bytes[20] = (uint8_t)clock.hour;
    bytes[21] = (uint8_t)clock.minute;
    bytes[22] = (uint8_t)clock.second;
    cc_write_be64(bytes + 24, (uint64_t)now_unix_seconds);
    cc_write_be32(bytes + 32, (uint32_t)(menu->clock_fraction * 1000000.0));
    cc_write_be32(bytes + STATE_CHECKSUM_OFFSET, state_checksum(bytes));
    if (!cc_atomic_file_replace(path, bytes, sizeof(bytes)))
        return GC_STATE_IO;
    menu->settings_changed = false;
    menu->clock_changed = false;
    return GC_STATE_OK;
}

const char *gc_state_result_text(gc_state_result result) {
    switch (result) {
        case GC_STATE_OK:
            return "okay";
        case GC_STATE_MISSING:
            return "no local settings have been saved";
        case GC_STATE_ARGUMENT:
            return "local settings contain invalid values";
        case GC_STATE_IO:
            return "local settings could not be read or saved";
        case GC_STATE_FORMAT:
            return "local settings are damaged or unsupported";
        case GC_STATE_REGION:
            return "local settings belong to another IPL region";
    }
    return "unknown state error";
}
