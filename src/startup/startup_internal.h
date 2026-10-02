#ifndef GAMECUBE_STARTUP_INTERNAL_H
#define GAMECUBE_STARTUP_INTERNAL_H

#include "gamecube/startup.h"

/* Resource validation and pure sampling share this bounded recovery window. */
enum { GC_STARTUP_SAMPLE_TICK_LIMIT = 2048 };

/* The sampler and live controller retain different update histories;
 * their native affine construction consumes the same instantaneous state.
 */
typedef struct {
    float velocity;
    unsigned kinetic;
    unsigned kinetic_phase;
    unsigned menu_phase;
    unsigned waves[2];
    unsigned wave_phase;
    int16_t spin_angle;
} GcStartupMotion;

void gc_startup_motion_scene(const GcStartup *startup, const GcStartupMotion *motion,
                             float matrix[12]);
void gc_startup_motion_glass(const GcStartup *startup, const GcStartupMotion *motion,
                             float matrix[12]);

#endif
