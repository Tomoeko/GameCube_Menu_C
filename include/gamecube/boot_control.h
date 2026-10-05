#ifndef GAMECUBE_BOOT_CONTROL_H
#define GAMECUBE_BOOT_CONTROL_H

#include "gamecube/startup.h"

#define GC_BOOT_CONTROLLER_COUNT 4
#define GC_BOOT_PAD_Z UINT16_C(0x0010)
#define GC_BOOT_PAD_A UINT16_C(0x0100)
#define GC_BOOT_PAD_B UINT16_C(0x0200)
#define GC_BOOT_PAD_LEFT UINT16_C(0x0001)
#define GC_BOOT_PAD_RIGHT UINT16_C(0x0002)
#define GC_BOOT_PAD_DOWN UINT16_C(0x0004)
#define GC_BOOT_PAD_UP UINT16_C(0x0008)

typedef enum {
    GC_BOOT_DRIVE_PENDING = 0,
    GC_BOOT_DRIVE_READY = 0x10,
    GC_BOOT_DRIVE_ABSENT = 0x12,
    GC_BOOT_DRIVE_LID_OPEN = 0x13,
    GC_BOOT_DRIVE_UNRECOGNIZED = 0x16,
    GC_BOOT_DRIVE_FATAL = 0x17,
    GC_BOOT_DRIVE_RETRY = 0x18
} GcBootDrive;

typedef enum {
    GC_BOOT_SETTINGS_NOTICE = 0,
    GC_BOOT_NORMAL = 1,
    GC_BOOT_RESET_PROMPT = 2,
    GC_BOOT_LID_DELAY = 3,
    GC_BOOT_TRANSITION = 4,
    GC_BOOT_MENU = 5,
    GC_BOOT_DISC_HANDOFF = 6
} GcBootPhase;

typedef struct {
    bool valid;
    uint16_t held;
} GcBootPad;

typedef struct {
    GcBootPad controllers[GC_BOOT_CONTROLLER_COUNT];
    uint8_t drive_state; /* Native status codes; unknown codes keep waiting. */
} GcBootInput;

typedef struct {
    const GcStartup *startup;     /* Borrowed, must outlive the controller. */
    unsigned sound_thresholds[3]; /* Event order: default, four Z, first Z. */
    unsigned return_ticks;
    unsigned lid_wait_ticks;
    /* PAL popup/SRAM order: English, German, French, Spanish, Italian,
     * Dutch, matching the local menu language enumeration. */
    unsigned initial_language;
    unsigned repeat_delay_ticks;
    unsigned repeat_period_ticks;
} GcBootConfig;

typedef struct {
    GcBootPhase phase;
    GcBootPhase next_phase;
    unsigned video_tick;
    unsigned drawing_tick;
    unsigned spin_ticks;
    unsigned transition_tick;
    unsigned menu_tick;
    unsigned sound_counter;
    unsigned return_tick;
    unsigned lid_wait_remaining;
    unsigned notice_stage;
    unsigned notice_counter;
    unsigned reset_stage;
    unsigned reset_delay;
    unsigned reset_body_counter;
    unsigned reset_choice_counter;
    unsigned reset_choice; /* Native order: Yes=0, No=1. */
    unsigned language_selection;
    unsigned repeat_tick;
    uint16_t previous_buttons;
    uint16_t previous_directions;
    uint16_t pressed_buttons;
    uint16_t navigation_buttons;
    unsigned waves[2];
    unsigned wave_phase;
    unsigned kinetic;
    unsigned kinetic_phase;
    unsigned error_ticks;
    unsigned unrecognized_ticks;
    float acceleration;
    float velocity;
    float return_velocity;
    int16_t angle;
    int16_t return_angle;
    int16_t transition_saved_angle;
    uint8_t fader;
    uint8_t language_alpha;
    bool drawing_complete;
    bool drawing_started;
    bool drawing_active;
    bool absence_latched;
    bool sound_pending;
    bool fatal_error;
    bool has_frame;
    bool language_pending;
    bool menu_requested; /* Local keyboard request, retained after key release. */
} GcBootControl;

typedef struct {
    int sound_event; /* Native audio event; startup uses 0..2, -1 means none. */
    bool menu_begin;
    bool disc_handoff;
    bool drawing_fast_forwarded;
    bool cube_motion;
    unsigned cube_direction;
    float cube_fraction;
    bool language_applied;
    unsigned language;        /* PAL popup/SRAM index, matching initial_language. */
    unsigned sound_events[4]; /* Preserves simultaneous native input cues. */
    unsigned sound_event_count;
} GcBootEvents;

typedef struct {
    uint8_t notice_alpha;
    uint8_t reset_alpha;
    uint8_t choices_alpha;
    uint8_t language_alpha;
    uint8_t error_alpha;
    uint8_t unrecognized_alpha;
    unsigned reset_choice;
    unsigned language_selection;
} GcBootUi;

bool gc_boot_config_init(GcBootConfig *config, const GcStartup *startup);

/* A lid already open at initialization selects the native silent delay path.
 * A lid opened later latches the normal menu route instead. Settings notice
 * mode corresponds to the valid-SRAM configuration-required flag.
 */
bool gc_boot_control_init(const GcBootConfig *config, GcBootControl *control,
                          GcBootPhase initial_phase);
GcBootPhase gc_boot_initial_phase(uint8_t initial_drive_state,
                                  bool configuration_required, bool force_menu);

/* A local confirm tap drives the recovered acceleration and menu transition.
 * Mandatory startup dialogs and an already-started disc handoff keep ownership.
 * Controller held levels retain their separate native behavior.
 */
void gc_boot_control_request_menu(GcBootControl *control);

/* One native video update, with held buttons sampled before the update.
 * The changing-state tick retains the old draw phase, matching the dispatcher.
 * This reports a disc handoff; the caller owns its local disc service.
 */
bool gc_boot_control_step(const GcBootConfig *config, GcBootControl *control,
                          const GcBootInput *input, GcBootEvents *events);
bool gc_boot_control_sample_pose(const GcBootConfig *config,
                                 const GcBootControl *control, GcStartupPose *pose);
void gc_boot_control_ui(const GcBootControl *control, GcBootUi *ui);

#endif
