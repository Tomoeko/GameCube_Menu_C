#include "console_common/support/affine.h"
#include "gamecube/menu_animation.h"
#include "gamecube/angle.h"

#include <math.h>
#include <string.h>

#define MENU_QUARTER_TURN 16384
#define MENU_PANE_THRESHOLD 11834
#define MENU_PANE_FADE_ANGLE 5000
#define MENU_FOCUS_MAX 32767

static bool valid_config(const GcStartup *startup) {
    return startup && startup->menu_rotation_step > 0 &&
           startup->menu_rotation_step < MENU_QUARTER_TURN &&
           startup->menu_focus_enter_step > 0 &&
           startup->menu_focus_enter_step < MENU_FOCUS_MAX &&
           startup->menu_focus_exit_step > 0 &&
           startup->menu_focus_exit_step < MENU_FOCUS_MAX &&
           startup->menu_glass_min_alpha <= 255 &&
           isfinite(startup->menu_focus_distance) &&
           isfinite(startup->menu_focus_twist);
}

static bool target_angles(bool selected, gc_face face, int angles[2]) {
    angles[0] = angles[1] = 0;
    if (!selected)
        return true;
    switch (face) {
        case GC_FACE_GAME_PLAY:
            angles[0] = MENU_QUARTER_TURN;
            break;
        case GC_FACE_MEMORY_CARD:
            angles[0] = -MENU_QUARTER_TURN;
            break;
        case GC_FACE_CALENDAR:
            angles[1] = -MENU_QUARTER_TURN;
            break;
        case GC_FACE_OPTIONS:
            angles[1] = MENU_QUARTER_TURN;
            break;
        default:
            return false;
    }
    return true;
}

/* Native USA 0x81311608 rounds a returning angle to a step multiple before
 * subtracting its last step. The selected axis advances directly and clamps
 * at the quarter turn. Division of negative values truncates toward zero.
 */
static int16_t advance_angle(int16_t value, int target, unsigned increment) {
    int step = (int)increment;
    int result = value;
    if (!target) {
        if (result > 0)
            result = step * (result / step) - step;
        if (result < 0)
            result = step * (result / step) + step;
    } else if (target > 0) {
        result += step;
        if (result > target)
            result = target;
    } else {
        result -= step;
        if (result < target)
            result = target;
    }
    return (int16_t)result;
}

bool gc_menu_animation_init(const GcStartup *startup, GcMenuAnimation *animation) {
    if (!animation || !valid_config(startup))
        return false;
    memset(animation, 0, sizeof(*animation));
    unsigned transition = startup->transition_ticks[0] + startup->transition_ticks[1] +
                          startup->transition_ticks[2];
    animation->oscillator_phase = (uint16_t)(transition * 7);
    return true;
}

bool gc_menu_animation_update(const GcStartup *startup, GcMenuAnimation *animation,
                              bool face_selected, gc_face face, bool editing) {
    int target[2];
    if (!animation || !valid_config(startup) ||
        !target_angles(face_selected, face, target))
        return false;
    for (unsigned axis = 0; axis < 2; ++axis)
        animation->angles[axis] = advance_angle(animation->angles[axis], target[axis],
                                                startup->menu_rotation_step);
    unsigned focus = animation->focus_angle;
    if (face_selected && editing) {
        unsigned end = MENU_FOCUS_MAX - startup->menu_focus_enter_step;
        if (focus < end)
            focus += startup->menu_focus_enter_step;
        if (focus >= end)
            focus = MENU_FOCUS_MAX;
    } else {
        if (focus > startup->menu_focus_exit_step)
            focus -= startup->menu_focus_exit_step;
        if (focus <= startup->menu_focus_exit_step)
            focus = 0;
    }
    animation->focus_angle = (uint16_t)focus;
    animation->oscillator_phase = (uint16_t)(animation->oscillator_phase + 7);
    return true;
}

/* Native 0x813074cc constructs Rz * Ry * Rx, then assigns translation. */
static void native_matrix(const int angles[3], const float translation[3],
                          float matrix[12]) {
    float sx = gc_angle_sine(angles[0]);
    float cx = gc_angle_cosine(angles[0]);
    float sy = gc_angle_sine(angles[1]);
    float cy = gc_angle_cosine(angles[1]);
    float sz = gc_angle_sine(angles[2]);
    float cz = gc_angle_cosine(angles[2]);
    matrix[0] = cz * cy;
    matrix[1] = -sz * cx + sx * cz * sy;
    matrix[2] = sz * sx + cx * cz * sy;
    matrix[3] = translation[0];
    matrix[4] = sz * cy;
    matrix[5] = cz * cx + sx * sz * sy;
    matrix[6] = -cz * sx + cx * sz * sy;
    matrix[7] = translation[1];
    matrix[8] = -sy;
    matrix[9] = cy * sx;
    matrix[10] = cy * cx;
    matrix[11] = translation[2];
}

static void cube_pose(const GcStartup *startup, const GcMenuAnimation *animation,
                      float matrix[12]) {
    int angles[3] = {animation->angles[0], animation->angles[1], 0};
    float translation[3] = {0};
    float face[12], idle[12];
    native_matrix(angles, translation, face);
    int phase = animation->oscillator_phase;
    const int16_t *profile = startup->menu_profile;
    for (unsigned axis = 0; axis < 3; ++axis) {
        int argument = phase * profile[axis];
        if (axis == 1)
            argument += profile[10];
        angles[axis] = (int)truncf(profile[axis + 3] * gc_angle_cosine(argument));
    }
    translation[0] = profile[8] * gc_angle_sine(phase * profile[6] + profile[11]);
    translation[1] = profile[9] * gc_angle_sine(phase * profile[7]);
    native_matrix(angles, translation, idle);
    cc_affine_multiply(matrix, idle, face);
    if (animation->focus_angle) {
        angles[0] = angles[1] = 0;
        angles[2] = (int)truncf(animation->focus_angle * startup->menu_focus_twist);
        translation[0] = translation[1] = 0;
        /* USA 11468 loads the native constant 1.0, so the last exit
         * update approaches the unfocused position without a depth jump.
         */
        translation[2] = startup->menu_focus_distance *
                         (1 - gc_angle_cosine(animation->focus_angle));
        native_matrix(angles, translation, idle);
        cc_affine_multiply(matrix, idle, matrix);
    }
}

/* USA 0x813136ac uses five 288-unit panes on the native cube. The local
 * records put the pane origin at its upper left and invert its Y axis.
 */
static void pane_pose(unsigned pane, const float cube[12], float matrix[12]) {
    int angles[3] = {0};
    float translation[3] = {-144, 144, 144};
    if (pane == GC_FACE_GAME_PLAY) {
        angles[0] = -MENU_QUARTER_TURN;
        translation[1] = 144;
        translation[2] = -144;
    } else if (pane == GC_FACE_MEMORY_CARD) {
        angles[0] = MENU_QUARTER_TURN;
        translation[1] = -144;
    } else if (pane == GC_FACE_CALENDAR) {
        angles[1] = MENU_QUARTER_TURN;
        translation[0] = 144;
    } else if (pane == GC_FACE_OPTIONS) {
        angles[1] = -MENU_QUARTER_TURN;
        translation[0] = -144;
        translation[2] = -144;
    }
    float local[12];
    native_matrix(angles, translation, local);
    local[1] = -local[1];
    local[5] = -local[5];
    local[9] = -local[9];
    cc_affine_multiply(matrix, cube, local);
}

static unsigned absolute_angle(int16_t angle) {
    int value = angle;
    return (unsigned)(value < 0 ? -value : value);
}

bool gc_menu_animation_sample(const GcStartup *startup,
                              const GcMenuAnimation *animation, bool face_selected,
                              gc_face face, bool editing, GcMenuAnimationPose *pose) {
    int target[2];
    if (!animation || !pose || !valid_config(startup) ||
        !target_angles(face_selected, face, target) ||
        animation->focus_angle > MENU_FOCUS_MAX)
        return false;
    memset(pose, 0, sizeof(*pose));
    cube_pose(startup, animation, pose->cube_matrix);
    pose->cube_scale = 1.1f;
    /* USA 0x8130dcac supplies this same alpha to both glass passes and
     * to 136ac's selected-side overlay. The home labels are independent.
     */
    unsigned minimum = startup->menu_glass_min_alpha;
    pose->glass_alpha =
        (uint8_t)(minimum + (255 - minimum) *
                                (MENU_FOCUS_MAX - animation->focus_angle) /
                                MENU_FOCUS_MAX);
    for (unsigned pane = 0; pane < GC_MENU_ANIMATION_PANES; ++pane)
        pane_pose(pane, pose->cube_matrix, pose->pane_matrices[pane]);
    unsigned x = absolute_angle(animation->angles[0]);
    unsigned y = absolute_angle(animation->angles[1]);
    unsigned maximum = x > y ? x : y;
    if (maximum > MENU_PANE_FADE_ANGLE)
        maximum = MENU_PANE_FADE_ANGLE;
    pose->pane_alpha[GC_MENU_ANIMATION_HOME_PANE] =
        (uint8_t)(255 - maximum * 255 / MENU_PANE_FADE_ANGLE);
    if (x == y && x != 0)
        pose->pane_alpha[GC_MENU_ANIMATION_HOME_PANE] = 0;
    const unsigned axes[4] = {0, 1, 0, 1};
    const int signs[4] = {1, -1, -1, 1};
    for (unsigned pane = 0; pane < 4; ++pane) {
        int angle = animation->angles[axes[pane]] * signs[pane];
        if (angle >= MENU_PANE_THRESHOLD) {
            unsigned difference = MENU_QUARTER_TURN - (unsigned)angle;
            pose->pane_alpha[pane] =
                (uint8_t)(pose->glass_alpha *
                          (255 - difference * 255 / MENU_PANE_FADE_ANGLE) / 255);
        }
    }
    for (unsigned pane = 0; pane < GC_MENU_ANIMATION_PANES; ++pane) {
        unsigned alpha = pose->pane_alpha[pane];
        pose->frame_alpha[pane] = (uint8_t)(alpha * alpha / 255);
    }
    pose->rotation_complete =
        animation->angles[0] == target[0] && animation->angles[1] == target[1];
    pose->focus_complete =
        animation->focus_angle == (face_selected && editing ? MENU_FOCUS_MAX : 0);
    return true;
}
