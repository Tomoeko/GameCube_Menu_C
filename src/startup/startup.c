#include "console_common/support/affine.h"
#include "startup_internal.h"
#include "gamecube/angle.h"
#include "console_common/support/interpolation.h"

#include <math.h>
#include <string.h>

typedef struct {
    unsigned x;
    unsigned y;
    unsigned face;
    unsigned lifetime;
    unsigned kind;
    unsigned change;
} TrailCell;

typedef struct {
    unsigned phase;
    unsigned next_phase;
    unsigned counter;
    unsigned roll_counter;
    unsigned step;
    unsigned x;
    unsigned y;
    unsigned face;
    unsigned reveal_counter;
    unsigned logotype_counter;
    unsigned bounce_counter;
    float height;
    float velocity;
    float acceleration;
    unsigned waves[2];
    unsigned wave_phase;
    TrailCell cells[GC_STARTUP_MAX_TRAILS];
    size_t cell_count;
} StartupState;

static void translate(float matrix[12], float x, float y, float z) {
    float translation[12];
    cc_affine_identity(translation);
    translation[3] = x;
    translation[7] = y;
    translation[11] = z;
    cc_affine_multiply(matrix, matrix, translation);
}

static void rotate(float matrix[12], unsigned axis, float angle) {
    float rotation[12];
    cc_affine_identity(rotation);
    unsigned a = (axis + 1) % 3;
    unsigned b = (axis + 2) % 3;
    int units = (int)lrintf(angle * (32768 / 3.14159265358979323846f));
    float sine = gc_angle_sine(units);
    float cosine = gc_angle_cosine(units);
    rotation[a * 4 + a] = rotation[b * 4 + b] = cosine;
    rotation[a * 4 + b] = -sine;
    rotation[b * 4 + a] = sine;
    cc_affine_multiply(matrix, matrix, rotation);
}

void gc_startup_transform(const float matrix[12], const float input[3],
                          float output[3]) {
    float result[3];
    for (unsigned row = 0; row < 3; ++row)
        result[row] = matrix[row * 4] * input[0] + matrix[row * 4 + 1] * input[1] +
                      matrix[row * 4 + 2] * input[2] + matrix[row * 4 + 3];
    memcpy(output, result, sizeof(result));
}

static void decay_waves(const GcStartup *startup, StartupState *state) {
    for (unsigned wave = 0; wave < 2; ++wave) {
        float value = (float)state->waves[wave] * startup->wave_decay[wave];
        state->waves[wave] = value < (wave ? 80 : 10) ? 0 : (unsigned)value;
    }
    if (state->waves[0] || state->waves[1])
        state->wave_phase = (state->wave_phase + 1000) & 65535;
}

static void advance_trails(const GcStartup *startup, StartupState *state) {
    for (size_t index = 0; index < state->cell_count; ++index) {
        TrailCell *cell = &state->cells[index];
        if (cell->change == 2)
            cell->kind = cell->lifetime = cell->change = 0;
        else if (cell->change == 1)
            cell->change = 0;
        else if (cell->kind && cell->lifetime != 255) {
            if (cell->lifetime)
                --cell->lifetime;
            if (!cell->lifetime)
                cell->change = 2;
        }
    }
    for (size_t index = 0; index < state->cell_count; ++index) {
        TrailCell *cell = &state->cells[index];
        if (cell->x == state->x && cell->y == state->y && cell->face == state->face) {
            cell->kind = startup->steps[state->step].trail_kind;
            cell->lifetime = startup->steps[state->step].trail_lifetime;
            cell->change = 1;
            return;
        }
    }
    if (state->cell_count < GC_STARTUP_MAX_TRAILS) {
        TrailCell *cell = &state->cells[state->cell_count++];
        cell->x = state->x;
        cell->y = state->y;
        cell->face = state->face;
        cell->kind = startup->steps[state->step].trail_kind;
        cell->lifetime = startup->steps[state->step].trail_lifetime;
        cell->change = 1;
    }
}

static void advance_step(const GcStartup *startup, StartupState *state) {
    unsigned direction = startup->steps[state->step].direction;
    if (direction == 0)
        ++state->x;
    else if (direction == 1)
        ++state->y;
    else if (direction == 2)
        --state->x;
    else if (direction == 3)
        --state->y;
    else if (direction < 7) {
        unsigned face = direction - 4;
        unsigned difference = (face + 3 - state->face) % 3;
        if (difference == 1 && state->y == 16) {
            state->y = state->x;
            state->x = 16;
            state->face = face;
        } else if (difference == 2 && state->x == 16) {
            state->x = state->y;
            state->y = 16;
            state->face = face;
        }
    }
    ++state->step;
    state->roll_counter = 0;
    advance_trails(startup, state);
}

/* Native USA 0x8130f1dc, 0x8130f134, and 0x8130fc4c: the drawing sequence
 * advances independently of the drive state. Tick comparisons use >, while
 * the logotype counter clamps on >=. These differences affect phase lengths.
 */
static void update(const GcStartup *startup, StartupState *state) {
    state->phase = state->next_phase;
    decay_waves(startup, state);
    if (state->phase == GC_STARTUP_DROP) {
        state->height -= state->velocity;
        if (state->height < 0) {
            state->height = 0;
            if (!state->waves[0]) {
                state->waves[0] = 4000;
                state->wave_phase = 32767;
            }
        }
        state->velocity += state->acceleration;
        if (state->counter <= startup->drop_ticks + startup->drop_wait_ticks)
            ++state->counter;
        else {
            state->next_phase = GC_STARTUP_ROLL;
            state->counter = 0;
        }
    } else if (state->phase == GC_STARTUP_ROLL) {
        unsigned direction = startup->steps[state->step].direction;
        unsigned duration = direction < 4 ? startup->roll_ticks : startup->corner_ticks;
        if (state->roll_counter > duration && state->step + 1 < startup->step_count)
            advance_step(startup, state);
        if (startup->steps[state->step].direction == 7) {
            /* USA/JAP 0x8130fd38 and EUR 0x81310670 enter the jump
             * without advancing the reset fade counter. The final tile
             * therefore stays transparent throughout the jump and reveal. */
            state->next_phase = GC_STARTUP_BOUNCE;
            state->counter = 0;
        } else
            ++state->roll_counter;
    } else if (state->phase == GC_STARTUP_BOUNCE) {
        ++state->counter;
        state->bounce_counter = state->counter;
        if (state->counter > startup->bounce_ticks + startup->bounce_wait_ticks) {
            state->next_phase = GC_STARTUP_RISE;
            state->counter = 0;
            state->height = startup->bounce_height;
            state->velocity = startup->rise_velocity;
            state->acceleration =
                fmaxf(0, 2 * startup->bounce_height /
                                 (float)(startup->rise_ticks * startup->rise_ticks) -
                             startup->rise_velocity / (float)startup->rise_ticks);
        }
    } else if (state->phase == GC_STARTUP_RISE) {
        state->height = fmaxf(0, state->height - state->velocity);
        state->velocity += state->acceleration;
        if (state->height == 0 && !state->waves[1]) {
            state->waves[1] = 12000;
            state->wave_phase = 32767;
        }
        if (state->counter <= startup->rise_ticks + startup->rise_wait_ticks)
            ++state->counter;
        else {
            state->next_phase = GC_STARTUP_REVEAL;
            state->counter = 0;
        }
        if (state->logotype_counter <
            startup->logotype_ticks + startup->logotype_delay_ticks)
            ++state->logotype_counter;
    } else if (state->phase == GC_STARTUP_REVEAL) {
        if (state->reveal_counter <= startup->reveal_ticks)
            ++state->reveal_counter;
        if (state->logotype_counter <
            startup->logotype_ticks + startup->logotype_delay_ticks)
            ++state->logotype_counter;
        if (state->reveal_counter > startup->reveal_ticks &&
            state->logotype_counter >=
                startup->logotype_ticks + startup->logotype_delay_ticks &&
            !state->waves[1])
            state->next_phase = GC_STARTUP_COMPLETE;
    }
}

static void cube_matrix(const GcStartup *startup, const StartupState *state,
                        float matrix[12]) {
    const float pi = 3.14159265358979323846f;
    float half = (float)startup->cube_edge * 0.5f;
    cc_affine_identity(matrix);
    /* Native USA 0x8130eda4/0x8130eedc/0x8130f2b0 concatenate each new
     * affine on the left. Assemble the resulting product in reverse call
     * order here. The height translation is applied after the scene matrix.
     */
    translate(matrix, half * 3, half * 3, half * 3);
    if (state->phase == GC_STARTUP_DROP || state->phase == GC_STARTUP_ROLL) {
        unsigned direction = startup->steps[state->step].direction;
        unsigned duration = direction < 4 ? startup->roll_ticks : startup->corner_ticks;
        unsigned limit = direction < 4 ? 16384 : 32768;
        unsigned angle_units = limit * state->roll_counter / duration;
        if (angle_units > limit)
            angle_units = limit;
        float angle = (float)angle_units * (pi / 32768);
        if (state->face == 1) {
            rotate(matrix, 2, pi * 0.5f);
            rotate(matrix, 0, pi * 0.5f);
        } else if (state->face == 2) {
            rotate(matrix, 2, -pi * 0.5f);
            rotate(matrix, 1, -pi * 0.5f);
        }
        translate(matrix, -((float)state->x - 16) * (float)startup->cube_edge, 0,
                  -((float)state->y - 16) * (float)startup->cube_edge);
        translate(matrix, -half, 0, -half);
        const int directions[4] = {-16384, 32767, 16384, 0};
        rotate(matrix, 1,
               direction < 4 ? (float)directions[direction] * (pi / 32768) : 0);
        translate(matrix, half, 0, half);
        rotate(matrix, 0, angle);
        translate(matrix, -half, half, -half);
    } else {
        float progress =
            fminf(1, (float)state->bounce_counter / (float)startup->bounce_ticks);
        float scale = fminf(1.5f, 1 + progress * 0.5f);
        float shift =
            fmaxf(-scale * (float)startup->cube_edge * 0.5f,
                  half - (float)startup->cube_edge * (1 + scale) * progress * 0.5f);
        rotate(matrix, 2, -pi * 0.5f);
        rotate(matrix, 1, -pi * 0.5f);
        translate(matrix, -scale * half, shift, -scale * half);
        unsigned angle_units = startup->bounce_quarter_turns * 16384 *
                               state->bounce_counter / startup->bounce_ticks;
        unsigned limit = startup->bounce_quarter_turns * 16384;
        if (angle_units > limit)
            angle_units = limit;
        rotate(matrix, 0, (float)angle_units * (pi / 32768));
        float transform[12];
        cc_affine_identity(transform);
        transform[0] = transform[5] = transform[10] = scale;
        cc_affine_multiply(matrix, matrix, transform);
    }
}

static void trail_pose(const GcStartup *startup, const StartupState *state,
                       GcStartupPose *pose) {
    float edge = (float)startup->cube_edge;
    float center = edge * 1.5f;
    unsigned order[GC_STARTUP_MAX_TRAILS];
    /* USA 0x8130ff80 submits face, X, Y in ascending raster order. The
     * route creates cells in a different order; retain native blending.
     */
    for (unsigned index = 0; index < state->cell_count; ++index) {
        const TrailCell *cell = &state->cells[index];
        unsigned key = cell->face * 1024 + cell->x * 32 + cell->y;
        unsigned position = index;
        while (position) {
            const TrailCell *previous = &state->cells[order[position - 1]];
            unsigned previous_key =
                previous->face * 1024 + previous->x * 32 + previous->y;
            if (previous_key <= key)
                break;
            order[position] = order[position - 1];
            --position;
        }
        order[position] = index;
    }
    for (size_t index = 0; index < state->cell_count; ++index) {
        const TrailCell *cell = &state->cells[order[index]];
        if (!cell->kind || pose->trail_count == GC_STARTUP_MAX_TRAILS)
            continue;
        int alpha = 255;
        if (cell->change == 1)
            alpha = (int)(state->roll_counter * 255 / startup->roll_ticks);
        else if (cell->change == 2)
            alpha = ((int)startup->roll_ticks - (int)state->roll_counter) * 255 /
                    (int)startup->roll_ticks;
        alpha = alpha < 0 ? 0 : alpha > 255 ? 255 : alpha;
        GcStartupTrail *trail = &pose->trails[pose->trail_count++];
        trail->alpha = (uint8_t)((unsigned)alpha * pose->moving_cube_alpha / 255);
        trail->face = cell->face;
        const unsigned corners[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
        for (unsigned vertex = 0; vertex < 4; ++vertex) {
            float x = ((float)cell->x - 16 + (float)corners[vertex][0]) * edge;
            float y = ((float)cell->y - 16 + (float)corners[vertex][1]) * edge;
            float *point = trail->positions[vertex];
            point[0] = center + (cell->face == 2 ? 0 : cell->face == 1 ? -y : -x);
            point[1] = center + (cell->face == 0 ? 0 : cell->face == 1 ? -x : -y);
            point[2] = center + (cell->face == 1 ? 0 : cell->face == 2 ? -x : -y);
        }
    }
}

static bool sample_state(const GcStartup *startup, unsigned tick, StartupState *state) {
    if (!startup || !state || !startup->step_count ||
        startup->step_count > GC_STARTUP_MAX_STEPS || !startup->roll_ticks ||
        !startup->corner_ticks || !startup->drop_ticks || !startup->bounce_ticks ||
        !startup->rise_ticks || !startup->reveal_ticks)
        return false;
    memset(state, 0, sizeof(*state));
    state->x = 16;
    state->y = 18;
    state->height = startup->drop_height;
    state->acceleration =
        2 * startup->drop_height / (float)(startup->drop_ticks * startup->drop_ticks);
    if (tick >= GC_STARTUP_SAMPLE_TICK_LIMIT)
        tick = GC_STARTUP_SAMPLE_TICK_LIMIT - 1;
    for (unsigned frame = 0; frame <= tick; ++frame) {
        update(startup, state);
        if (state->phase == GC_STARTUP_COMPLETE)
            break;
    }
    return true;
}

bool gc_startup_sample(const GcStartup *startup, unsigned tick, GcStartupPose *pose) {
    StartupState state;
    if (!pose || !sample_state(startup, tick, &state))
        return false;
    memset(pose, 0, sizeof(*pose));
    for (unsigned axis = 0; axis < 3; ++axis)
        pose->model_scale[axis] = pose->glass_scale[axis] = 1;
    cc_affine_identity(pose->glass_matrix);
    pose->phase = (GcStartupPhase)state.phase;
    pose->step_index = state.step;
    memcpy(pose->waves, state.waves, sizeof(pose->waves));
    pose->wave_phase = state.wave_phase;
    pose->cover_alpha =
        (uint8_t)(state.reveal_counter >= startup->reveal_ticks
                      ? 255
                      : state.reveal_counter * 255 / startup->reveal_ticks);
    pose->moving_cube_alpha = (uint8_t)(255 - pose->cover_alpha);
    /* USA 0x8130d3b8 overrides the base and mark object alpha. The cover
     * object's kinetic-wobble alpha is zero during this initial sequence;
     * the complex glass cube is introduced by the later menu transition.
     */
    unsigned wave_alpha = state.waves[0] + state.waves[1];
    if (wave_alpha > 3000)
        wave_alpha = 3000;
    pose->base_cube_alpha = (uint8_t)(wave_alpha * 255 / 3000);
    pose->boot_mark_alpha = pose->cover_alpha;
    pose->cover_cube_alpha = 0;
    pose->glass_cube_alpha = 0;
    pose->logotype_alpha = 255;
    pose->logotype_frame =
        state.logotype_counter > startup->logotype_delay_ticks
            ? (float)(state.logotype_counter - startup->logotype_delay_ticks) * 60 /
                  (float)startup->logotype_ticks
            : 0;
    pose->complete = state.phase == GC_STARTUP_COMPLETE;
    pose->drawing_ready =
        pose->complete ||
        (state.phase == GC_STARTUP_REVEAL &&
         state.reveal_counter > startup->reveal_ticks &&
         state.logotype_counter >=
             startup->logotype_ticks + startup->logotype_delay_ticks &&
         !state.waves[1]);
    GcStartupMotion motion = {.kinetic_phase = 2000,
                              .waves = {state.waves[0], state.waves[1]},
                              .wave_phase = state.wave_phase};
    gc_startup_motion_scene(startup, &motion, pose->scene_matrix);
    cube_matrix(startup, &state, pose->cube_matrix);
    if (state.phase == GC_STARTUP_DROP || state.phase == GC_STARTUP_RISE)
        pose->moving_cube_world_y = state.height;
    else if (state.phase == GC_STARTUP_BOUNCE) {
        float progress =
            fminf(1, (float)state.bounce_counter / (float)startup->bounce_ticks);
        float inverse = 1 - progress;
        pose->moving_cube_world_y =
            startup->bounce_height * (1 - inverse * inverse * inverse * inverse);
    }
    trail_pose(startup, &state, pose);
    return true;
}

/* USA 0x8130c6e0, 0x8130bdbc, 0x8130ddf8 and 0x8130e41c.
 * This samples the ready/absent drive branch; it does not guess drive delays.
 * The dispatcher draws the old state for the tick that changes state.
 */
bool gc_startup_sample_menu(const GcStartup *startup, unsigned tick,
                            GcStartupPose *pose) {
    if (!startup || !pose || !startup->sequence_ticks || !startup->spin_ticks ||
        !startup->menu_ticks || startup->spin_target <= 0 ||
        startup->spin_acceleration <= 0 || !startup->transition_fade_ticks ||
        !startup->transition_ticks[1] || !startup->transition_ticks[2] ||
        !gc_startup_sample(startup, tick, pose))
        return false;
    pose->complete = tick >= startup->menu_ticks;
    /* Native +0x1f becomes ready when REVEAL finishes its conditions, one
     * update before the synthetic COMPLETE phase. The same update starts
     * automatic spinning, because state 1 checks ready after drawing.
     */
    if (tick < startup->sequence_ticks - 1)
        return true;
    StartupState state;
    if (!sample_state(startup, startup->sequence_ticks - 1, &state))
        return false;
    unsigned elapsed = tick - (startup->sequence_ticks - 1) + 1;
    unsigned spin_frames =
        elapsed < startup->spin_ticks ? elapsed : startup->spin_ticks;
    float acceleration = 0, velocity = 0;
    unsigned kinetic = 0, kinetic_phase = 2000;
    int16_t spin_angle = 0;
    for (unsigned frame = 0; frame < spin_frames; ++frame) {
        /* State 1 updates kinetic/waves before drawing, then again after
         * the automatic absent-disc acceleration (USA 0x8130c6e0).
         */
        float amplitude = acceleration > 0 ? (float)kinetic + startup->kinetic_increment
                                           : (float)kinetic * startup->kinetic_decay;
        kinetic = amplitude < 1 ? 0 : (unsigned)fminf(startup->spin_target, amplitude);
        if (kinetic)
            kinetic_phase += 7;
        decay_waves(startup, &state);
        acceleration += startup->spin_acceleration;
        velocity += acceleration;
        spin_angle = gc_angle_wrap((float)spin_angle + velocity);
        kinetic = (unsigned)fminf(startup->spin_target,
                                  (float)kinetic + startup->kinetic_increment);
        kinetic_phase += 7;
        decay_waves(startup, &state);
    }
    unsigned wave = state.waves[0] + state.waves[1];
    if (wave > 3000)
        wave = 3000;
    pose->scene_phase = GC_STARTUP_SCENE_SPINNING;
    pose->base_cube_alpha = (uint8_t)(wave * 255 / 3000);
    pose->cover_cube_alpha = (uint8_t)((kinetic > 3000 ? 3000 : kinetic) * 255 / 3000);
    pose->trail_count = 0;
    pose->moving_cube_alpha = 0;
    pose->moving_cube_world_y = 0;
    unsigned transition_frames =
        elapsed > startup->spin_ticks ? elapsed - startup->spin_ticks : 0;
    if (spin_frames == startup->spin_ticks)
        spin_angle = 0;
    unsigned menu_phase = 0;
    unsigned frame_counter = 1;
    if (transition_frames) {
        pose->scene_phase = GC_STARTUP_SCENE_TRANSITION;
        spin_angle = 0; /* Native ready/absent branch restores the saved angle. */
        int16_t saved_angle = 0;
        unsigned first_end = startup->transition_ticks[0];
        unsigned second_end = first_end + startup->transition_ticks[1];
        unsigned last_end = second_end + startup->transition_ticks[2];
        unsigned count = transition_frames > last_end ? last_end : transition_frames;
        for (unsigned frame = 1; frame <= count; ++frame) {
            int16_t previous = spin_angle;
            if (frame < first_end) {
                spin_angle = gc_angle_wrap((float)spin_angle + velocity);
                saved_angle = spin_angle;
            } else if (frame < second_end) {
                float first =
                    -((float)saved_angle + (float)startup->transition_turns * 65536);
                spin_angle = gc_angle_wrap(cc_cubic_hermite(
                    (float)(frame - first_end), (float)startup->transition_ticks[1],
                    first, startup->spin_target, startup->transition_angle, 0));
                if (frame > first_end)
                    velocity =
                        (float)gc_angle_wrap((float)((int)spin_angle - previous));
            } else if (frame < last_end) {
                spin_angle = gc_angle_wrap(cc_cubic_hermite(
                    (float)(frame - second_end), (float)startup->transition_ticks[2],
                    startup->transition_angle, 0, 0, 0));
                velocity = (float)gc_angle_wrap((float)((int)spin_angle - previous));
            } else {
                spin_angle = 0;
                velocity = 0;
            }
        }
        frame_counter = count + 1;
        menu_phase = count ? (count - 1) * 7 : 0;
        if (tick >= startup->menu_ticks) {
            pose->scene_phase = GC_STARTUP_SCENE_MENU;
            unsigned idle = tick - startup->menu_ticks + 1;
            idle %= (unsigned)GC_STARTUP_MENU_PERIOD;
            menu_phase = (count + idle) * 7;
        }
        pose->perspective = true;
        pose->boot_mark_alpha = pose->cover_alpha = pose->cover_cube_alpha = 0;
        pose->base_cube_alpha = pose->logotype_alpha = 0;
        pose->glass_cube_alpha = 255;
        if (frame_counter <= startup->transition_fade_ticks) {
            unsigned left = startup->transition_fade_ticks - frame_counter;
            unsigned combined = wave + kinetic;
            if (combined > 3000)
                combined = 3000;
            pose->base_cube_alpha = pose->logotype_alpha =
                (uint8_t)(left * 255 * combined /
                          (startup->transition_fade_ticks * 3000));
            pose->glass_cube_alpha =
                (uint8_t)(frame_counter * 255 / startup->transition_fade_ticks);
        }
        unsigned wordmark_start = last_end > 100 ? last_end - 100 : 0;
        unsigned wordmark =
            frame_counter >= last_end ? 255
            : frame_counter > wordmark_start
                ? (frame_counter - wordmark_start) * 255 / (last_end - wordmark_start)
                : 0;
        /* Native 0x81312448 multiplies the supplied alpha by itself. */
        pose->menu_labels_alpha = (uint8_t)(wordmark * wordmark / 255);
    }
    float blend = velocity / startup->spin_target;
    pose->model_scale[0] = pose->model_scale[2] =
        1 - (1 - startup->spin_squash[0]) * blend;
    pose->model_scale[1] = 1 + startup->spin_squash[1] * blend;
    for (unsigned axis = 0; axis < 3; ++axis)
        pose->glass_scale[axis] = 1.1f * (1 - blend) + pose->model_scale[axis] * blend;
    GcStartupMotion motion = {.velocity = velocity,
                              .kinetic = kinetic,
                              .kinetic_phase = kinetic_phase,
                              .menu_phase = menu_phase,
                              .waves = {state.waves[0], state.waves[1]},
                              .wave_phase = state.wave_phase,
                              .spin_angle = spin_angle};
    gc_startup_motion_scene(startup, &motion, pose->scene_matrix);
    if (transition_frames)
        gc_startup_motion_glass(startup, &motion, pose->glass_matrix);
    memcpy(pose->waves, state.waves, sizeof(pose->waves));
    pose->wave_phase = state.wave_phase;
    return true;
}
