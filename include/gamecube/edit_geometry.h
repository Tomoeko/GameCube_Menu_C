#ifndef GAMECUBE_EDIT_GEOMETRY_H
#define GAMECUBE_EDIT_GEOMETRY_H

#include "gamecube/texture_collection.h"

enum { GC_EDIT_POINT_LIMIT = 2048 };

typedef enum {
    GC_EDIT_PUNCTUATION,
    GC_EDIT_DAY,
    GC_EDIT_MONTH,
    GC_EDIT_YEAR,
    GC_EDIT_HOUR,
    GC_EDIT_MINUTE,
    GC_EDIT_SECOND,
    GC_EDIT_SOUND,
    GC_EDIT_SCREEN_POSITION,
    GC_EDIT_LANGUAGE,
    GC_EDIT_DISC,
    GC_EDIT_ARROW,
    GC_EDIT_SCREEN_BAR
} GcEditField;

typedef struct {
    float position[3];
    float scale;
    uint8_t alpha;
    int16_t angles[3];
    uint8_t colors[2][4]; /* Native material registers C0 and C1. */
    unsigned source_index;
    GcEditField field;
    bool selected;
} GcEditPoint;

typedef struct {
    const uint8_t *bytes;
    size_t byte_count;
    size_t point_offset;
    size_t point_count;
    size_t group_offset;
    unsigned group_count;
} GcEditMotion;

typedef struct {
    float compression_end;
    float compression_time;
    float compression_amount;
    float fade_speed;
    float delay_span;
    float translation[3];
    const uint8_t *delays; /* Borrowed from the original ROM owned by GcText. */
    unsigned point_count;
} GcEditDiscLaunch;

typedef struct {
    GcIplResource motion_owner;
    GcIplResource maps[7];
    GcEditMotion motion;
    float transforms[5][3];       /* Sound, position, date, time, disc: x/y/scale. */
    uint8_t palettes[2][3][2][4]; /* Options/calendar, base/selected/dim, C0/C1. */
    uint8_t disc_colors[2][4];
    uint16_t entrance_ticks;
    uint16_t selection_ticks;
    uint16_t language_fade_ticks;
    uint16_t motion_ticks;
    float motion_profile[6]; /* X/Y shift, Y/Z degrees, split, entrance gate. */
    float angle_units;
    GcEditDiscLaunch disc_launch;
    float language_transform[3];
    float option_growth[3];
    float arrow_offsets[6][2];
    float arrow_amplitude;
    float language_centers[6];
    float language_origin[2];
    double language_shift[2];
    float option_caption_offsets[4];
    uint32_t option_heading_color;
    uint16_t arrow_ticks;
    uint16_t screen_bar_ticks[2];
    bool europe;
} GcEditGeometry;

typedef struct {
    uint16_t entrance_counter;
    uint16_t sampled_tick;
    uint16_t selection[GC_EDIT_POINT_LIMIT];
    uint16_t next_selection[GC_EDIT_POINT_LIMIT];
    uint16_t movement[GC_EDIT_POINT_LIMIT];
    uint16_t next_movement[GC_EDIT_POINT_LIMIT];
    uint16_t motion_counter;
    uint16_t sampled_motion;
    uint16_t disc_launch_motion;
    bool disc_launch_motion_active;
    bool disc_launching;
    uint16_t language_labels[6];
    uint16_t language_captions[6];
    uint16_t arrows[6];
    uint16_t next_arrows[6];
    uint16_t growth[3];
    uint16_t next_growth[3];
    uint16_t arrow_counter;
    uint16_t sampled_arrow;
    uint16_t screen_bar_counter;
    uint8_t screen_bar_state;
    bool screen_editing;
    gc_language sampled_language;
    float language_from;
    float language_to;
    uint16_t language_movement;
    uint16_t next_language_movement;
    gc_page page;
    bool active;
    bool ready;
} GcEditState;

bool gc_edit_motion_decode(const uint8_t *bytes, size_t byte_count,
                           GcEditMotion *motion);
bool gc_edit_motion_point(const GcEditMotion *motion, unsigned group, unsigned index,
                          float progress, float position[3]);
bool gc_edit_geometry_decode(const GcText *text, GcEditGeometry *geometry);
void gc_edit_geometry_destroy(GcEditGeometry *geometry);
/* PAL caption preview changes before the original cube MAP language is
 * committed. Only the language editor has a separate candidate locale. */
gc_language gc_edit_geometry_map_language(const gc_menu *menu);
/* Native palette blend: base*base_weight + (1-base_weight)*
 * (selected*selection_weight + dim*(1-selection_weight)). */
bool gc_edit_geometry_colors(const GcEditGeometry *geometry, bool calendar,
                             float selection_weight, float base_weight,
                             uint8_t colors[2][4]);
/* Original signed-angle matrix used for the independently moving cube blocks. */
bool gc_edit_point_matrix(const GcEditPoint *point, float matrix[12]);
/* Samples the original CAFD block motion and native visibility/digit maps.
 * progress is the native normalized entrance time. Points use the IPL cube1
 * model; no character geometry or font bitmaps are synthesized. */
size_t gc_edit_geometry_points(const GcEditGeometry *geometry, const gc_menu *menu,
                               float progress, GcEditPoint *points, size_t capacity);
/* Advance once per recovered video tick, after the native face collapse gate.
 * Original builders sample first and increment afterward; the first ready
 * tick therefore samples entrance zero and initial palette counters zero. */
void gc_edit_state_init(GcEditState *state);
bool gc_edit_state_advance(GcEditState *state, const GcEditGeometry *geometry,
                           const gc_menu *menu, bool editor_ready, uint64_t ticks);
float gc_edit_state_progress(const GcEditState *state, const GcEditGeometry *geometry);
/* PAL labels approach half opacity when unselected; translated captions
 * fade fully out. Native constructor 0x8130aaec supplies the duration. */
uint8_t gc_edit_language_alpha(const GcEditState *state, const GcEditGeometry *geometry,
                               gc_language language, bool caption);
size_t gc_edit_geometry_points_with_state(const GcEditGeometry *geometry,
                                          const gc_menu *menu, const GcEditState *state,
                                          GcEditPoint *points, size_t capacity);
/* Includes the native blocks hidden by the current digit/word pattern. This
 * is the source transform array copied by the original value-change effects.
 * The source_index remains the original CAFD group index. */
size_t gc_edit_geometry_source_points(const GcEditGeometry *geometry,
                                      const gc_menu *menu, const GcEditState *state,
                                      GcEditPoint *points, size_t capacity);
/* Original group1 word patterns: native NO DISC or PRESS START. Uses the same
 * zero-Y perspective editor camera as calendar/options. */
size_t gc_edit_disc_points(const GcEditGeometry *geometry, gc_language language,
                           bool ready, float progress, GcEditPoint *points,
                           size_t capacity);
size_t gc_edit_disc_status_points(const GcEditGeometry *geometry, gc_language language,
                                  gc_disc_status status, float progress,
                                  GcEditPoint *points, size_t capacity);
/* Original Disc CAFD group before a status MAP selects visible word blocks. */
size_t gc_edit_disc_source_points(const GcEditGeometry *geometry, gc_language language,
                                  float progress, GcEditPoint *points, size_t capacity);
/* Stateful Disc samples retain the 120-tick whole-word translation and signed
 * rotation after entrance. Sample the complete group before selecting a MAP. */
size_t gc_edit_disc_status_points_with_state(const GcEditGeometry *geometry,
                                             gc_language language,
                                             gc_disc_status status,
                                             const GcEditState *state,
                                             GcEditPoint *points, size_t capacity);
size_t gc_edit_disc_source_points_with_state(const GcEditGeometry *geometry,
                                             gc_language language,
                                             const GcEditState *state,
                                             GcEditPoint *points, size_t capacity);

#endif
