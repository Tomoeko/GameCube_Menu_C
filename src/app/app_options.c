#include "app_options.h"
#include "support/option_values.h"
#include "gamecube/config.h"

#include <limits.h>
#include <string.h>

static bool parse_boot_phase(const char *name, GcBootPhase *phase) {
    if (!strcmp(name, "normal"))
        *phase = GC_BOOT_NORMAL;
    else if (!strcmp(name, "notice"))
        *phase = GC_BOOT_SETTINGS_NOTICE;
    else if (!strcmp(name, "lost"))
        *phase = GC_BOOT_RESET_PROMPT;
    else
        return false;
    return true;
}

static bool parse_value(GcAppOptions *options, const char *flag, const char *value,
                        bool *explicit_ipl) {
    if (!strcmp(flag, "--ipl")) {
        options->ipl_path = value;
        *explicit_ipl = true;
    } else if (!strcmp(flag, "--region")) {
        return gc_option_region(value, &options->region);
    } else if (!strcmp(flag, "--config")) {
        options->config_path = value;
    } else if (!strcmp(flag, "--boot-state")) {
        return parse_boot_phase(value, &options->boot_phase);
    } else if (!strcmp(flag, "--card-a")) {
        options->card_inputs[0] = value;
    } else if (!strcmp(flag, "--card-b")) {
        options->card_inputs[1] = value;
    } else if (!strcmp(flag, "--noinsert")) {
        unsigned mask;
        if (!gc_config_noinsert_mask(value, &mask))
            return false;
        options->absent_cards |= mask;
    } else if (!strcmp(flag, "--disc")) {
        options->disc_path = value;
    } else if (!strcmp(flag, "--startup-sound")) {
        if (strlen(value) != 1 || *value < '0' || *value > '2')
            return false;
        options->startup_sound = (unsigned)(*value - '0');
    } else if (!strcmp(flag, "--frames")) {
        return gc_option_unsigned(value, 10, 1, ULONG_MAX, &options->frame_limit);
    } else if (!strcmp(flag, "--audio")) {
        if (strcmp(value, "web"))
            return false;
        options->record_audio = CC_CAPTURE_AUDIO_WEB;
    } else {
        return false;
    }
    return true;
}

GcAppOptionsResult gc_app_options_parse(GcAppOptions *options, int argc,
                                        char *const argv[]) {
    if (!options || argc < 1 || !argv)
        return GC_APP_OPTIONS_INVALID;
    GcAppOptions candidate = {.region = GC_REGION_USA,
                              .config_path = "Files/config.ini",
                              .boot_phase = GC_BOOT_NORMAL};
    bool explicit_ipl = false;
    bool explicit_audio = false;
    for (int index = 1; index < argc; ++index) {
        const char *flag = argv[index];
        if (!flag)
            return GC_APP_OPTIONS_INVALID;
        if (!strcmp(flag, "--help"))
            return GC_APP_OPTIONS_HELP;
        if (!strcmp(flag, "--step")) {
            candidate.inspect_frames = true;
        } else if (!strcmp(flag, "--aa")) {
            candidate.antialiasing = true;
        } else if (!strcmp(flag, "--record")) {
            candidate.record = true;
            candidate.record_half =
                index + 1 < argc && argv[index + 1] && !strcmp(argv[index + 1], "half");
            if (candidate.record_half)
                ++index;
        } else if (!strcmp(flag, "--skip-startup")) {
            candidate.skip_startup = true;
        } else if (!strcmp(flag, "--delaystart")) {
            candidate.delay_start = true;
            candidate.timed_start = false;
            if (index + 1 < argc) {
                const char *next = argv[index + 1];
                if (!next)
                    return GC_APP_OPTIONS_INVALID;
                if (strncmp(next, "--", 2)) {
                    if (!gc_option_seconds(next, &candidate.startup_delay_seconds))
                        return GC_APP_OPTIONS_INVALID;
                    ++index;
                    candidate.timed_start = true;
                }
            }
        } else {
            if (index + 1 >= argc || !argv[index + 1] ||
                !parse_value(&candidate, flag, argv[index + 1], &explicit_ipl))
                return GC_APP_OPTIONS_INVALID;
            explicit_audio |= !strcmp(flag, "--audio");
            ++index;
        }
    }
    if (explicit_audio && !candidate.record)
        return GC_APP_OPTIONS_INVALID;
    static const char *const region_states[] = {
        "Files/state-jap.dat", "Files/state-usa.dat", "Files/state-eur.dat"};
    if (!explicit_ipl)
        candidate.ipl_path = gc_option_default_ipl(candidate.region);
    candidate.state_path = region_states[candidate.region];
    *options = candidate;
    return GC_APP_OPTIONS_OK;
}

void gc_app_options_usage(FILE *output) {
    if (!output)
        return;
    fprintf(output,
            "Usage: gamecube-menu [--ipl Files/path/IPL.bin]\n"
            "       [--region USA|EUR|JAP] [--skip-startup] [--frames count]\n"
            "       [--card-a Files/input.raw] [--card-b Files/input.raw]\n"
            "       [--noinsert a|b|ab] (override card presence)\n"
            "       [--startup-sound 0|1|2] (normal, four Z, one Z)\n"
            "       [--disc Files/game.iso]\n"
            "       [--config Files/config.ini] [--step]\n"
            "       [--boot-state normal|notice|lost]\n"
            "       [--delaystart [seconds]] (wait for input or a timed delay)\n"
            "       [--record [half]] (compressed MP4 to Movies until exit)\n"
            "       [--audio web] (AAC recording audio for web playback)\n"
            "       Audio stays unchanged without --audio.\n"
            "       half records half width and height.\n"
            "       [--aa] (smooth edges; uses extra GPU resources)\n"
            "Arrow keys select, A/Enter confirm, B/Escape cancel, S starts.\n"
            "F toggles fullscreen; Escape at the home cube exits fullscreen.\n"
            "R restarts startup from frame 0 (preserving the --step pause state).\n"
            "- lowers menu music; = or + raises it (0 to 600 percent).\n"
            "D inserts/ejects local test media (a dummy disc without --disc).\n"
            "E toggles the local fatal-error presentation at any time.\n"
            "--step starts paused: comma back, period forward, Space play/pause.\n");
}
