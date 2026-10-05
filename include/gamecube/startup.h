#ifndef GAMECUBE_STARTUP_H
#define GAMECUBE_STARTUP_H

#include "gamecube/ipl.h"

#define GC_STARTUP_MAX_STEPS 64
#define GC_STARTUP_MAX_TRAILS 64
#define GC_STARTUP_MENU_PERIOD UINT64_C(65536)

typedef enum {
    GC_STARTUP_DROP,
    GC_STARTUP_ROLL,
    GC_STARTUP_BOUNCE,
    GC_STARTUP_RISE,
    GC_STARTUP_REVEAL,
    GC_STARTUP_COMPLETE,
    GC_STARTUP_PHASE_COUNT
} GcStartupPhase;

typedef enum {
    GC_STARTUP_SCENE_DRAWING,
    GC_STARTUP_SCENE_SPINNING,
    GC_STARTUP_SCENE_TRANSITION,
    GC_STARTUP_SCENE_MENU
} GcStartupScenePhase;

typedef struct {
    uint8_t direction;
    uint8_t trail_kind;
    uint8_t trail_lifetime; /* Completed rolls, not video frames. */
} GcStartupStep;

typedef struct {
    unsigned frame_rate;
    unsigned roll_ticks;
    unsigned corner_ticks;
    unsigned drop_ticks;
    unsigned drop_wait_ticks;
    unsigned bounce_ticks;
    unsigned bounce_wait_ticks;
    unsigned rise_ticks;
    unsigned rise_wait_ticks;
    unsigned reveal_ticks;
    unsigned logotype_ticks;
    unsigned logotype_delay_ticks;
    unsigned bounce_quarter_turns;
    unsigned cube_edge;
    float drop_height;
    float bounce_height;
    float rise_velocity;
    float wave_decay[2];
    float wave_translation[2];
    uint8_t trail_color[3];
    GcStartupStep steps[GC_STARTUP_MAX_STEPS];
    size_t step_count;
    unsigned phase_start_ticks[GC_STARTUP_PHASE_COUNT];
    unsigned sequence_ticks;
    unsigned spin_ticks;
    unsigned transition_ticks[3];
    unsigned transition_fade_ticks;
    unsigned menu_ticks;
    unsigned transition_turns;
    int16_t transition_angle;
    float spin_target;
    float spin_acceleration;
    float scene_origin_offset;
    int16_t scene_base_angles[3];
    float kinetic_increment;
    float kinetic_decay;
    float spin_squash[2];
    float transition_base_angles[3];
    float kinetic_phase_rates[3];
    int16_t menu_profile[13];
    unsigned menu_rotation_step;
    unsigned menu_focus_enter_step;
    unsigned menu_focus_exit_step;
    unsigned menu_glass_min_alpha;
    float menu_focus_distance;
    float menu_focus_twist;
    GcIplImage trail_texture; /* Native I8 coverage, white RGB for raster tint. */
} GcStartup;

typedef struct {
    float positions[4][3];
    uint8_t alpha;
    unsigned face;
} GcStartupTrail;

typedef struct {
    GcStartupPhase phase;
    GcStartupScenePhase scene_phase;
    unsigned step_index;
    unsigned waves[2];
    unsigned wave_phase;
    float scene_matrix[12];
    float glass_matrix[12];
    float cube_matrix[12];
    float moving_cube_world_y;
    float model_scale[3];
    float glass_scale[3];
    bool perspective;
    GcStartupTrail trails[GC_STARTUP_MAX_TRAILS];
    size_t trail_count;
    uint8_t cover_alpha;
    uint8_t base_cube_alpha;
    uint8_t boot_mark_alpha;
    uint8_t cover_cube_alpha;
    uint8_t glass_cube_alpha;
    uint8_t logotype_alpha;
    uint8_t moving_cube_alpha;
    uint8_t menu_labels_alpha; /* All four home GLH panes, already squared. */
    float logotype_frame;
    bool drawing_ready; /* Native +0x1f flag, before the synthetic COMPLETE phase. */
    bool complete;
} GcStartupPose;

/* Recovers the native route, regional video tick constants, and I8 trail
 * texture from the supplied private ROM. USA/JAP are 60 Hz; EUR is 50 Hz.
 * This covers the initial cube drawing sequence. Drive/button-dependent
 * spinning and the later menu transition are controlled by separate states.
 */
bool gc_startup_load(const char *ipl_path, GcStartup *startup);
void gc_startup_destroy(GcStartup *startup);

/* Samples the native integer tick state. Frame zero is the first video update.
 * Matrices are row-major 3x4 affines: apply cube_matrix, then model_scale,
 * then scene_matrix, then add moving_cube_world_y to the moving cube's world Y.
 * Trails and the large startup models share model_scale and scene_matrix.
 */
bool gc_startup_sample(const GcStartup *startup, unsigned tick, GcStartupPose *pose);

/* Normal boot with no disc: the drive has reported its ready/absent state
 * before drawing completes. Samples the native acceleration and menu
 * transition as well. Other drive and controller branches can wait longer.
 */
bool gc_startup_sample_menu(const GcStartup *startup, unsigned tick,
                            GcStartupPose *pose);
void gc_startup_transform(const float matrix[12], const float input[3],
                          float output[3]);

#endif
