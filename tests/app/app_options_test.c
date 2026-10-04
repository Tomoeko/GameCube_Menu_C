#include "app/app_options.h"

#include <assert.h>
#include <string.h>

static void test_defaults_and_region_paths(void) {
    GcAppOptions options;
    char *defaults[] = {"gamecube-menu"};
    assert(gc_app_options_parse(&options, 1, defaults) == GC_APP_OPTIONS_OK);
    assert(options.region == GC_REGION_USA && !options.inspect_frames &&
           !options.record);
    assert(options.antialiasing && !options.antialiasing_override &&
           options.record_audio == CC_CAPTURE_AUDIO_NORMAL);
    assert(!strcmp(options.ipl_path, "Files/GameCube_BIOS/USA/IPL.bin"));
    assert(!strcmp(options.state_path, "Files/state-usa.dat"));
    const char *regions[] = {"JAP", "USA", "EUR"};
    for (unsigned region = 0; region < 3; ++region) {
        char *arguments[] = {"gamecube-menu", "--region", (char *)regions[region]};
        assert(gc_app_options_parse(&options, 3, arguments) == GC_APP_OPTIONS_OK);
        assert((unsigned)options.region == region);
        assert(strstr(options.ipl_path, regions[region]));
        char *explicit_path[] = {"gamecube-menu", "--ipl", "Files/input.bin",
                                 "--region", (char *)regions[region]};
        assert(gc_app_options_parse(&options, 5, explicit_path) == GC_APP_OPTIONS_OK);
        assert(options.ipl_path == explicit_path[2]);
    }
}

static void test_inspection_and_overrides(void) {
    GcAppOptions options;
    char *manual[] = {"gamecube-menu", "--delaystart", "--step"};
    assert(gc_app_options_parse(&options, 3, manual) == GC_APP_OPTIONS_OK);
    assert(options.delay_start && options.inspect_frames && !options.timed_start);
    char *timed[] = {"gamecube-menu", "--step", "--delaystart", "5.5"};
    assert(gc_app_options_parse(&options, 4, timed) == GC_APP_OPTIONS_OK);
    assert(options.delay_start && options.inspect_frames && options.timed_start);
    assert(options.startup_delay_seconds == 5.5);
    char *cards[] = {"gamecube-menu", "--card-a", "Files/a.raw", "--noinsert", "a",
                     "--noinsert",    "b",        "--card-b",    "Files/b.raw"};
    assert(gc_app_options_parse(&options, 9, cards) == GC_APP_OPTIONS_OK);
    assert(options.absent_cards == 3);
    assert(options.card_inputs[0] == cards[2] && options.card_inputs[1] == cards[8]);
    char *remaining[] = {"gamecube-menu",   "--skip-startup",
                         "--boot-state",    "notice",
                         "--startup-sound", "2",
                         "--frames",        "15",
                         "--config",        "Files/custom.ini",
                         "--disc",          "Files/local.iso"};
    assert(gc_app_options_parse(&options, 12, remaining) == GC_APP_OPTIONS_OK);
    assert(options.skip_startup && options.boot_phase == GC_BOOT_SETTINGS_NOTICE);
    assert(options.startup_sound == 2 && options.frame_limit == 15);
    assert(options.config_path == remaining[9] && options.disc_path == remaining[11]);
}

static void test_recording_arguments(void) {
    GcAppOptions options;
    char *record[] = {"gamecube-menu", "--record"};
    assert(gc_app_options_parse(&options, 2, record) == GC_APP_OPTIONS_OK);
    assert(options.record && !options.record_half && options.region == GC_REGION_USA);
    assert(!options.delay_start && !options.inspect_frames && !options.frame_limit);

    char *manual[] = {"gamecube-menu", "--delaystart", "--record", "--step",
                      "--region",      "EUR",          "--frames", "15"};
    assert(gc_app_options_parse(&options, 8, manual) == GC_APP_OPTIONS_OK);
    assert(options.record && options.delay_start && !options.timed_start);
    assert(options.inspect_frames && options.region == GC_REGION_EUROPE);
    assert(options.frame_limit == 15);
    assert(!strcmp(options.state_path, "Files/state-eur.dat"));

    char *half[] = {"gamecube-menu", "--delaystart", "--record", "half", "--step"};
    assert(gc_app_options_parse(&options, 5, half) == GC_APP_OPTIONS_OK);
    assert(options.record && options.record_half && options.inspect_frames);
    assert(options.delay_start && !options.timed_start);

    char *full_after_half[] = {"gamecube-menu", "--record", "half", "--record"};
    assert(gc_app_options_parse(&options, 4, full_after_half) == GC_APP_OPTIONS_OK);
    assert(options.record && !options.record_half);

    char *half_after_full[] = {"gamecube-menu", "--record", "--record", "half"};
    assert(gc_app_options_parse(&options, 4, half_after_full) == GC_APP_OPTIONS_OK);
    assert(options.record && options.record_half);

    char *timed[] = {"gamecube-menu", "--record", "--delaystart", "5.5", "--step",
                     "--frames",      "240",      "--region",     "JAP"};
    assert(gc_app_options_parse(&options, 9, timed) == GC_APP_OPTIONS_OK);
    assert(options.record && options.delay_start && options.timed_start);
    assert(options.startup_delay_seconds == 5.5 && options.inspect_frames);
    assert(options.frame_limit == 240 && options.region == GC_REGION_JAPAN);

    char *repeated[] = {"gamecube-menu", "--record", "--delaystart", "--record"};
    assert(gc_app_options_parse(&options, 4, repeated) == GC_APP_OPTIONS_OK);
    assert(options.record && options.delay_start && !options.timed_start);

    GcAppOptions original = options;
    char *invalid_value[] = {"gamecube-menu", "--record", "capture.mp4"};
    assert(gc_app_options_parse(&options, 3, invalid_value) == GC_APP_OPTIONS_INVALID);
    assert(!memcmp(&options, &original, sizeof(options)));
    char *invalid_count[] = {"gamecube-menu", "--record", "--frames", "0"};
    assert(gc_app_options_parse(&options, 4, invalid_count) == GC_APP_OPTIONS_INVALID);
    assert(!memcmp(&options, &original, sizeof(options)));
    char *help[] = {"gamecube-menu", "--record", "--help"};
    assert(gc_app_options_parse(&options, 3, help) == GC_APP_OPTIONS_HELP);
    assert(!memcmp(&options, &original, sizeof(options)));
}

static void test_invalid_arguments_are_atomic(void) {
    const char *flags[] = {"--ipl",           "--region", "--config",   "--boot-state",
                           "--card-a",        "--card-b", "--noinsert", "--disc",
                           "--startup-sound", "--frames", "--frame",    "--audio"};
    GcAppOptions original;
    memset(&original, 0, sizeof(original));
    original.frame_limit = 17;
    for (unsigned index = 0; index < sizeof(flags) / sizeof(flags[0]); ++index) {
        GcAppOptions options;
        memcpy(&options, &original, sizeof(options));
        char *arguments[] = {"gamecube-menu", (char *)flags[index]};
        assert(gc_app_options_parse(&options, 2, arguments) == GC_APP_OPTIONS_INVALID);
        assert(!memcmp(&options, &original, sizeof(options)));
    }
    const char *invalid_delays[] = {"", "-1", "nan", "inf", "1e400", "5suffix"};
    for (unsigned index = 0; index < sizeof(invalid_delays) / sizeof(invalid_delays[0]);
         ++index) {
        GcAppOptions options;
        memcpy(&options, &original, sizeof(options));
        char *arguments[] = {"gamecube-menu", "--delaystart",
                             (char *)invalid_delays[index]};
        assert(gc_app_options_parse(&options, 3, arguments) == GC_APP_OPTIONS_INVALID);
        assert(!memcmp(&options, &original, sizeof(options)));
    }
    const char *invalid_counts[] = {
        "", "0", "-1", " -1", "1suffix", "9999999999999999999999999999999999"};
    for (unsigned index = 0; index < sizeof(invalid_counts) / sizeof(invalid_counts[0]);
         ++index) {
        GcAppOptions options;
        memcpy(&options, &original, sizeof(options));
        char *arguments[] = {"gamecube-menu", "--frames",
                             (char *)invalid_counts[index]};
        assert(gc_app_options_parse(&options, 3, arguments) == GC_APP_OPTIONS_INVALID);
        assert(!memcmp(&options, &original, sizeof(options)));
    }
    char *help[] = {"gamecube-menu", "--delaystart", "--step", "--help"};
    assert(gc_app_options_parse(&original, 4, help) == GC_APP_OPTIONS_HELP);
    assert(original.frame_limit == 17);
    assert(gc_app_options_parse(NULL, 4, help) == GC_APP_OPTIONS_INVALID);
}

static void test_recording_audio_and_antialiasing(void) {
    GcAppOptions options;
    char *web[] = {"gamecube-menu", "--record", "half", "--audio", "web", "--aa"};
    assert(gc_app_options_parse(&options, 6, web) == GC_APP_OPTIONS_OK);
    assert(options.record && options.record_half && options.antialiasing);
    assert(options.record_audio == CC_CAPTURE_AUDIO_WEB);
    char *reordered[] = {"gamecube-menu", "--audio", "web", "--record", "--step"};
    assert(gc_app_options_parse(&options, 5, reordered) == GC_APP_OPTIONS_OK);
    assert(options.record && !options.record_half && options.inspect_frames);
    assert(options.record_audio == CC_CAPTURE_AUDIO_WEB);
    char *aa[] = {"gamecube-menu", "--aa"};
    assert(gc_app_options_parse(&options, 2, aa) == GC_APP_OPTIONS_OK);
    assert(options.antialiasing && options.antialiasing_override && !options.record);
    char *disabled[] = {"gamecube-menu", "--no-aa"};
    assert(gc_app_options_parse(&options, 2, disabled) == GC_APP_OPTIONS_OK);
    assert(!options.antialiasing && options.antialiasing_override);
    assert(options.record_audio == CC_CAPTURE_AUDIO_NORMAL);
    GcAppOptions original = options;
    char *without_record[] = {"gamecube-menu", "--audio", "web"};
    assert(gc_app_options_parse(&options, 3, without_record) == GC_APP_OPTIONS_INVALID);
    assert(!memcmp(&options, &original, sizeof(options)));
    const char *invalid_modes[] = {"normal", "aac", "", "--aa"};
    for (unsigned index = 0; index < sizeof(invalid_modes) / sizeof(invalid_modes[0]);
         ++index) {
        char *invalid[] = {"gamecube-menu", "--record", "--audio",
                           (char *)invalid_modes[index]};
        assert(gc_app_options_parse(&options, 4, invalid) == GC_APP_OPTIONS_INVALID);
        assert(!memcmp(&options, &original, sizeof(options)));
    }
}

int main(void) {
    test_defaults_and_region_paths();
    test_inspection_and_overrides();
    test_recording_arguments();
    test_invalid_arguments_are_atomic();
    test_recording_audio_and_antialiasing();
    return 0;
}
