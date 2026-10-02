#ifndef GAMECUBE_ANGLE_H
#define GAMECUBE_ANGLE_H

#include <math.h>
#include <stdint.h>

/* Native angle arithmetic truncates, then wraps to signed 16-bit turns.
 * Callers supply finite values representable by int32_t after truncation. */
static inline int16_t gc_angle_wrap(float angle) {
    uint16_t bits = (uint16_t)(int32_t)truncf(angle);
    int value = bits <= INT16_MAX ? (int)bits : (int)bits - 65536;
    return (int16_t)value;
}

/* USA 01244 initializes the original 4096-entry sine table at 07c24.
 * 07de8 / 07e04 index an unsigned 16-bit angle after discarding four bits;
 * cosine uses the same table displaced by one quarter turn. Generate each
 * requested value with the original float phase and double sine operation.
 */
static inline float gc_angle_sine(int angle) {
    unsigned index = (uint16_t)angle >> 4;
    float phase = (6.28318530717958647692f / 4096) * (float)index;
    return (float)sin((double)phase);
}

static inline float gc_angle_cosine(int angle) {
    unsigned index = ((uint16_t)angle >> 4) + 1024;
    float phase = (6.28318530717958647692f / 4096) * (float)index;
    return (float)sin((double)phase);
}

#endif
