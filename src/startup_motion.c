#include "startup_internal.h"
#include "console_common/support/affine.h"
#include "gamecube/angle.h"

#include <math.h>
#include <string.h>

static void rotate(float matrix[12], unsigned axis, int units) {
    float rotation[12];
    cc_affine_identity(rotation);
    unsigned first = (axis + 1) % 3;
    unsigned second = (axis + 2) % 3;
    float sine = gc_angle_sine(units);
    float cosine = gc_angle_cosine(units);
    rotation[first * 4 + first] = rotation[second * 4 + second] = cosine;
    rotation[first * 4 + second] = -sine;
    rotation[second * 4 + first] = sine;
    cc_affine_multiply(matrix, matrix, rotation);
}

/* USA/JAP 0x8130c200 and EUR 0x8130c694. The startup reset clears
 * scene+0x24, so the constructor's unused .7 weight does not blend this
 * wobble. Native Euler order is Rz*Ry*Rx; the global spin is positive Y.
 */
void gc_startup_motion_scene(const GcStartup *startup, const GcStartupMotion *motion,
                             float matrix[12]) {
    cc_affine_identity(matrix);
    for (unsigned axis = 3; axis-- > 0;)
        rotate(matrix, axis, startup->scene_base_angles[axis]);
    float translation[12];
    cc_affine_identity(translation);
    translation[3] = translation[7] = translation[11] = startup->scene_origin_offset;
    cc_affine_multiply(matrix, matrix, translation);
    if (motion->kinetic) {
        const unsigned rates[3] = {90, 60, 70};
        float wobble[12];
        cc_affine_identity(wobble);
        for (unsigned axis = 3; axis-- > 0;) {
            int phase = (int)(uint16_t)(motion->kinetic_phase * rates[axis]);
            rotate(wobble, axis,
                   gc_angle_wrap((float)motion->kinetic * gc_angle_sine(phase)));
        }
        cc_affine_multiply(wobble, wobble, matrix);
        memcpy(matrix, wobble, sizeof(wobble));
    }
    float sine = gc_angle_sine((int)motion->wave_phase);
    float shake[12];
    cc_affine_identity(shake);
    shake[7] = sine * (startup->wave_translation[0] * (float)motion->waves[0] +
                       startup->wave_translation[1] * (float)motion->waves[1]);
    rotate(shake, 2, gc_angle_wrap((float)motion->waves[0] * sine));
    rotate(shake, 0, gc_angle_wrap(-(float)motion->waves[1] * sine));
    cc_affine_multiply(shake, shake, matrix);
    float spin[12];
    cc_affine_identity(spin);
    rotate(spin, 1, motion->spin_angle);
    cc_affine_multiply(spin, spin, shake);
    memcpy(matrix, spin, sizeof(spin));
}

/* USA/JAP 0x8130e120 and EUR 0x8130ea34, followed by the Y spin
 * submitted in e41c/ed30. Reset weight zero leaves velocity/target as the
 * profile blend; geometry squash uses its own separate calculation.
 */
void gc_startup_motion_glass(const GcStartup *startup, const GcStartupMotion *motion,
                             float matrix[12]) {
    float blend = motion->velocity / startup->spin_target;
    int16_t rotations[3];
    for (unsigned axis = 0; axis < 3; ++axis) {
        int phase = (int)(uint16_t)motion->menu_phase * startup->menu_profile[axis];
        if (axis == 1)
            phase += startup->menu_profile[10];
        float idle = startup->menu_profile[axis + 3] * gc_angle_cosine(phase);
        float active_phase =
            (float)motion->kinetic_phase * startup->kinetic_phase_rates[axis];
        float active =
            startup->transition_base_angles[axis] +
            (float)motion->kinetic * gc_angle_sine((int)truncf(active_phase));
        rotations[axis] = gc_angle_wrap((1 - blend) * idle + blend * active);
    }
    cc_affine_identity(matrix);
    matrix[3] =
        startup->menu_profile[8] *
        gc_angle_sine((int)(uint16_t)motion->menu_phase * startup->menu_profile[6] +
                      startup->menu_profile[11]);
    matrix[7] =
        startup->menu_profile[9] *
        gc_angle_sine((int)(uint16_t)motion->menu_phase * startup->menu_profile[7]);
    for (unsigned axis = 3; axis-- > 0;)
        rotate(matrix, axis, rotations[axis]);
    float spin[12];
    cc_affine_identity(spin);
    rotate(spin, 1, motion->spin_angle);
    cc_affine_multiply(spin, spin, matrix);
    memcpy(matrix, spin, sizeof(spin));
}
