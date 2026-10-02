#include "audio_internal.h"

#include <math.h>
#include <string.h>

static float float_from_bits(uint32_t bits) {
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

/* Finite route angles only. Every ordinary operation rounds separately;
 * explicit fmaf corresponds to the recovered single precision FMADDS.
 */
static float route_angle_sine(float radians) {
    static const uint32_t reduction_bits[4] = {
        0x3e800000,
        0x3cbe6080,
        0x34372200,
        0x2da44152,
    };
    static const uint32_t polynomial_bits[10] = {
        0x366ccfaa, 0x34a5e129, 0xb9aae275, 0xb8196543, 0x3c81e0ed,
        0x3b2335dd, 0xbe9de9e6, 0xbda55de7, 0x3f800000, 0x3f490fdb,
    };
    static const float quadrant_values[4][2] = {
        {0, 1},
        {1, 0},
        {0, -1},
        {-1, 0},
    };
    float estimate = float_from_bits(0x3f22f983) * radians;
    int index = (int)(estimate + copysignf(0.5f, radians));
    unsigned quadrant = (unsigned)index & 3;
    float reduced = radians - (float)(index * 2);
    for (unsigned i = 0; i < 4; ++i)
        reduced = fmaf(float_from_bits(reduction_bits[i]), radians, reduced);
    if (fabsf(reduced) < float_from_bits(0x39b504f3)) {
        float slope = reduced * quadrant_values[quadrant][1];
        return fmaf(float_from_bits(polynomial_bits[9]), slope,
                    quadrant_values[quadrant][0]);
    }
    float square = reduced * reduced;
    unsigned coefficient = quadrant & 1 ? 0 : 1;
    float value = fmaf(float_from_bits(polynomial_bits[coefficient]), square,
                       float_from_bits(polynomial_bits[coefficient + 2]));
    for (unsigned i = coefficient + 4; i < 10; i += 2)
        value = fmaf(square, value, float_from_bits(polynomial_bits[i]));
    if (!(quadrant & 1))
        value = reduced * value;
    return value * quadrant_values[quadrant][quadrant & 1 ? 0 : 1];
}

void gc_audio_route_table_init(GcAudio *audio) {
    if (!audio || !audio->sequence_revision)
        return;
    for (unsigned index = 0; index <= 256; ++index) {
        float radians = float_from_bits(0x3fc90fdb) * (float)index;
        radians *= 1.0f / 256.0f;
        audio->route_sine_table[index] = route_angle_sine(radians);
    }
}

float gc_audio_route_sine(const GcAudio *audio, float value) {
    if (!audio || !isfinite(value))
        return 0;
    value = fmaxf(0, fminf(1, value));
    if (audio->sequence_revision) {
        unsigned index = (unsigned)(value * 256.0f);
        return audio->route_sine_table[index];
    }
    float radians = float_from_bits(0x40490e2c) * value;
    radians *= 0.5f;
    return route_angle_sine(radians);
}
