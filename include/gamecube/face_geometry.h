#ifndef GAMECUBE_FACE_GEOMETRY_H
#define GAMECUBE_FACE_GEOMETRY_H

#include "gamecube/menu_animation.h"

#define GC_FACE_GEOMETRY_POINTS 61
#define GC_FACE_MEMORY_POINTS 36
#define GC_FACE_OPTIONS_PERIOD UINT64_C(530)
#define GC_FACE_MEMORY_PERIOD UINT64_C(281)
#define GC_FACE_GAMEPLAY_PERIOD UINT64_C(6549504)

typedef struct {
    uint32_t values[4];
} GcFaceRandom;

/* Native random initialization reads four low time-base words and discards
 * one sample. Every subsequent call selects a stream using time-base bit8.
 * Explicit clock samples preserve the algorithm without inventing a native
 * timer seed or tying recovered geometry to a host clock implementation.
 */
bool gc_face_random_init(const uint32_t time_base_samples[5], GcFaceRandom *random);
bool gc_face_random_next(GcFaceRandom *random, uint32_t time_base, uint32_t *value);

typedef enum {
    GC_FACE_MODEL_MENU_CUBE,
    GC_FACE_MODEL_CARD_BASE,
    GC_FACE_MODEL_CARD_COVER
} GcFaceGeometryModel;

typedef struct {
    float reference_distance;
    float reference_brightness;
    float position[3];
    float cutoff_degrees;
} GcMenuGridLighting;

typedef struct {
    unsigned frame_rate;
    uint8_t options_mask[72];
    uint8_t options_corners[4];
    uint8_t options_corner_order[4];
    uint8_t options_corner_start[4];
    float options_edges[4];
    float options_wave_height;
    float options_wave_duration;
    float memory_scale;
    float memory_inner_scale;
    float memory_turns[3];
    int16_t options_color[4];
    int16_t memory_color[4];
    unsigned options_exit_step;
    unsigned options_exit_fade;
    unsigned memory_exit_step;
    unsigned memory_exit_fade;
    int16_t gameplay_color[4];
    int16_t calendar_colors[2][4];
    unsigned gameplay_radius;
    float gameplay_scale;
    unsigned gameplay_exit_step;
    unsigned gameplay_exit_fade;
    float calendar_scale;
    float calendar_hand_scale;
    unsigned calendar_exit_step;
    unsigned calendar_exit_fade;
    float card_min_scale;
    float card_max_scale;
    float card_hover_offset[2];
    int16_t card_colors[6][4][4];
    float card_particle_radius;
    float card_particle_start;
    float card_particle_growth;
    float card_particle_scale[3];
    unsigned card_arrow_ticks;
    float card_arrow_amplitude;
    GcMenuGridLighting grid_lighting;
} GcFaceGeometry;

typedef struct {
    float position[3];
    int16_t angles[3];
    uint8_t alpha;
} GcFaceGeometryCell;

typedef struct {
    GcFaceGeometryCell options[GC_FACE_GEOMETRY_POINTS];
    GcFaceGeometryCell memory[GC_FACE_MEMORY_POINTS];
    GcFaceGeometryCell gameplay[16];
    GcFaceGeometryCell calendar[13];
    uint8_t memory_arrival_delay[GC_FACE_MEMORY_POINTS];
    unsigned options_tick;
    unsigned memory_tick;
    int16_t memory_turn;
    bool options_visible;
    bool memory_visible;
    unsigned gameplay_tick;
    uint16_t gameplay_phase;
    int16_t gameplay_angles[3];
    int16_t gameplay_steps[2];
    unsigned calendar_tick;
    bool gameplay_visible;
    bool calendar_visible;
    gc_date_time clock;
} GcFaceGeometryState;

typedef struct {
    GcFaceGeometryModel model;
    float matrix[12];
    float scale[3];
    int16_t registers[2][4];
    unsigned register_mask;
    uint8_t alpha;
} GcFaceGeometryPoint;

/* Private ROM data remains in memory. The native options uses 30 frame
 * cubes and 31 mask-selected cubes; the memory icon uses a 6 by 6 grid.
 */
bool gc_face_geometry_load(const char *ipl_path, GcFaceGeometry *geometry);

/* The original randomizes each memory cube's arrival by 0 through 31 ticks.
 * A caller may provide those samples. NULL gives simultaneous arrivals for
 * deterministic inspection without inventing an original random seed.
 */
bool gc_face_geometry_init(const GcFaceGeometry *geometry,
                           const uint8_t memory_arrival_delay[GC_FACE_MEMORY_POINTS],
                           GcFaceGeometryState *state);
bool gc_face_geometry_set_clock(GcFaceGeometryState *state, const gc_date_time *clock);
bool gc_face_geometry_update(const GcFaceGeometry *geometry, GcFaceGeometryState *state,
                             bool selected, gc_face face, bool editing);
bool gc_face_geometry_advance(const GcFaceGeometry *geometry,
                              GcFaceGeometryState *state, uint64_t ticks, bool selected,
                              gc_face face, bool editing);

/* Selected, unedited face motion repeats after its entrance has settled.
 * Gameplay combines 624 / 656 tick rocking and a 4096 tick ring phase.
 * Calendar has no repeating geometry motion while its supplied clock is
 * fixed. The original face state is settled after 1200 continuous ticks;
 * preserve that entrance before reducing an offline time by this period.
 */
uint64_t gc_face_geometry_period(gc_face face);

/* Native Gameplay, Calendar and Options editors begin once every face cell's
 * X/Y position has collapsed to zero. Query after the editing tick update;
 * alpha and camera focus are independent. Memory Card has a separate gate.
 */
bool gc_face_geometry_editor_ready(const GcFaceGeometryState *state, gc_face face);

/* Advance once per native video tick, then iterate all cells. Zero-alpha
 * cells are returned as well. Apply the native model's bind transform,
 * scale, and matrix before camera projection; the matrix already includes
 * menu_pose->cube_matrix. Registers are signed TEV C0/C1 overrides.
 */
size_t gc_face_geometry_count(gc_face face);
bool gc_face_geometry_get(const GcFaceGeometry *geometry,
                          const GcFaceGeometryState *state,
                          const GcMenuAnimationPose *menu_pose, gc_face face,
                          size_t index, GcFaceGeometryPoint *point);

/* Native full-card frames use i_cube0 for an empty cell and i_cube1 for a
 * populated cell. Coordinates are logical GUI pixel centers, not the native
 * motion record's fixed-point positions. selection_tick is 0 through 6;
 * phase is the native unsigned oscillator, advanced by 7 each video tick.
 * Sector sizes are the native 0x2000 through 0x40000 powers of two.
 * Project the returned world matrix with the editor camera at Y zero.
 */
bool gc_card_geometry_sample(const GcFaceGeometry *geometry, const GcStartup *startup,
                             float center_x, float center_y, bool populated,
                             bool selected, unsigned sector_size,
                             unsigned selection_tick, uint16_t phase, uint8_t alpha,
                             GcFaceGeometryPoint *point);

/* Native erase uses scale0..selected maximum, with hover/rotation amplitude
 * proportional to that scale instead of the ordinary min..max selection.
 */
bool gc_card_geometry_erase(const GcFaceGeometry *geometry, const GcStartup *startup,
                            float center_x, float center_y, float fraction,
                            unsigned sector_size, uint16_t phase, uint8_t alpha,
                            GcFaceGeometryPoint *point);

/* The native erase effect emits 12 i_cube1 pieces. Its random delay (0..3)
 * and signed angle are caller-owned samples; duration is 10 video ticks.
 * The parent matrix is the selected card's world matrix at erase completion.
 */
bool gc_card_geometry_particle(const GcFaceGeometry *geometry,
                               const float parent_matrix[12], unsigned sector_size,
                               unsigned tick, unsigned delay, int16_t angle,
                               uint8_t page_alpha, GcFaceGeometryPoint *point);

/* Native GLH arau/arbu move by +offset and arad/arbd by -offset in GUI Y.
 * ticks counts video updates since card initialization; both extrema repeat
 * once. The offset retains native fixed12.4 truncation. Arrow visibility and
 * its independent 20-tick fader remain with the card controller.
 */
bool gc_card_arrow_offset(const GcFaceGeometry *geometry, uint64_t ticks,
                          float *offset);

/* The stationary native grid lights the alpha channel, using COS2 spotlight
 * and gentle distance attenuation. Sample logical GUI vertex coordinates;
 * interpolate the returned raster color across each original tile. Its TEV
 * color is ONE * RASC and alpha is TEXA * RASA.
 */
bool gc_menu_grid_color(const GcFaceGeometry *geometry, float logical_x,
                        float logical_y, uint8_t page_alpha, float rgba[4]);
bool gc_menu_grid_light_color(const GcMenuGridLighting *light, float logical_x,
                              float logical_y, uint8_t light_alpha, float rgba[4]);

#endif
