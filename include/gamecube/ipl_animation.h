#ifndef GAMECUBE_IPL_ANIMATION_H
#define GAMECUBE_IPL_ANIMATION_H

#include "gamecube/ipl_model.h"

typedef enum { GC_IPL_ANIMATION_JOINT, GC_IPL_ANIMATION_COLOR } GcIplAnimationKind;

typedef struct {
    uint8_t *data;
    size_t size;
    GcIplAnimationKind kind;
    unsigned duration_frames;
    unsigned track_count;
    unsigned rotation_shift;
    unsigned loop_mode;
    size_t descriptors;
    size_t arrays[4];
    size_t array_counts[4];
    size_t remap;
} GcIplAnimation;

/* Owns a validated copy of the native BCK or IPK1 animation. The supplied
 * regional ROMs each contain one 65-frame logotype animation of each kind.
 */
bool gc_ipl_animation_decode(const uint8_t *data, size_t size, GcIplAnimationKind kind,
                             GcIplAnimation *animation);
bool gc_ipl_animation_load(const char *ipl_path, GcIplAnimationKind kind,
                           GcIplAnimation *animation);
void gc_ipl_animation_destroy(GcIplAnimation *animation);

/* Frame time is clamped at the first/last key. This applies native joint TRS
 * or material RGBA tracks; the startup state machine controls when time starts.
 */
bool gc_ipl_animation_apply(const GcIplAnimation *animation, float frame,
                            GcIplModel *model);

#endif
