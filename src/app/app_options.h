#ifndef GAMECUBE_APP_OPTIONS_H
#define GAMECUBE_APP_OPTIONS_H

#include "gamecube/boot_control.h"
#include "gamecube/menu.h"
#include "console_common/capture/capture_writer.h"

#include <stdio.h>

typedef struct {
    const char *ipl_path;
    const char *state_path;
    const char *config_path;
    const char *disc_path;
    const char *card_inputs[2];
    gc_region region;
    GcBootPhase boot_phase;
    CcCaptureAudioMode record_audio;
    unsigned long frame_limit;
    unsigned startup_sound;
    unsigned absent_cards;
    double startup_delay_seconds;
    bool skip_startup;
    bool inspect_frames;
    bool delay_start;
    bool timed_start;
    bool record;
    bool record_half;
    bool antialiasing;
    bool antialiasing_override;
} GcAppOptions;

typedef enum {
    GC_APP_OPTIONS_OK,
    GC_APP_OPTIONS_HELP,
    GC_APP_OPTIONS_INVALID
} GcAppOptionsResult;

/* Paths borrow argv storage or static defaults. Invalid input leaves output
 * unchanged; parsing does not open resources or change the working directory.
 */
GcAppOptionsResult gc_app_options_parse(GcAppOptions *options, int argc,
                                        char *const argv[]);
void gc_app_options_usage(FILE *output);

#endif
