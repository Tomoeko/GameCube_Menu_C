#include "startup_internal.h"
#include "gamecube/boot_control.h"
#include "gamecube/angle.h"
#include "console_common/support/interpolation.h"

#include <math.h>
#include <string.h>

bool gc_boot_config_init(GcBootConfig *config, const GcStartup *startup) {
    if (!config || !startup || !startup->sequence_ticks || !startup->spin_ticks ||
        (startup->frame_rate != 50 && startup->frame_rate != 60) ||
        !isfinite(startup->spin_acceleration) || startup->spin_acceleration <= 0 ||
        startup->spin_target != 5000 || !startup->transition_fade_ticks ||
        startup->transition_ticks[0] != 15 || startup->transition_ticks[1] != 80 ||
        startup->transition_ticks[2] != 50)
        return false;
    *config = (GcBootConfig){0};
    config->startup = startup;
    /* USA 0x8130e8ac and EUR 0x8130f1c0 initialize the selector counters.
     * Both regions use a strict greater-than comparison when firing a cue.
     */
    config->sound_thresholds[1] = startup->frame_rate == 50 ? 6 : 7;
    config->sound_thresholds[2] = startup->frame_rate == 50 ? 5 : 6;
    config->return_ticks = startup->frame_rate == 50 ? 50 : 60;
    config->lid_wait_ticks = 120;
    config->repeat_delay_ticks = 35;
    config->repeat_period_ticks = 10;
    return true;
}

GcBootPhase gc_boot_initial_phase(uint8_t initial_drive_state,
                                  bool configuration_required, bool force_menu) {
    if (configuration_required)
        return GC_BOOT_SETTINGS_NOTICE;
    if (initial_drive_state == GC_BOOT_DRIVE_LID_OPEN || force_menu)
        return GC_BOOT_LID_DELAY;
    return GC_BOOT_NORMAL;
}

bool gc_boot_control_init(const GcBootConfig *config, GcBootControl *control,
                          GcBootPhase initial_phase) {
    if (!config || !config->startup || !control ||
        (initial_phase != GC_BOOT_NORMAL && initial_phase != GC_BOOT_RESET_PROMPT &&
         initial_phase != GC_BOOT_LID_DELAY &&
         initial_phase != GC_BOOT_SETTINGS_NOTICE))
        return false;
    bool language_pending = config->startup->frame_rate == 50 &&
                            (initial_phase == GC_BOOT_SETTINGS_NOTICE ||
                             initial_phase == GC_BOOT_RESET_PROMPT);
    if (language_pending && config->initial_language >= 6)
        return false;
    *control = (GcBootControl){0};
    control->phase = control->next_phase = initial_phase;
    control->sound_pending = true;
    control->drawing_active = true;
    control->kinetic_phase = 2000;
    control->return_tick = 60;
    control->lid_wait_remaining = config->lid_wait_ticks;
    control->notice_counter = 30;
    control->reset_choice = 1;
    control->language_selection = config->initial_language;
    control->language_pending = language_pending;
    return true;
}

static uint16_t held_buttons(const GcBootInput *input) {
    uint16_t buttons = 0;
    for (unsigned index = 0; index < GC_BOOT_CONTROLLER_COUNT; ++index) {
        if (input->controllers[index].valid)
            buttons |= input->controllers[index].held;
    }
    return buttons;
}

static void emit_sound(GcBootEvents *events, unsigned event) {
    events->sound_event = (int)event;
    if (events->sound_event_count < 4)
        events->sound_events[events->sound_event_count++] = event;
}

/* USA 0x81302818 and regional profile initializers repeat at counts 35, 46,
 * then every ten ticks. A changed direction mask resets the count.
 * Native B edges suppress simultaneous A and Start edges.
 */
static void poll_actions(const GcBootConfig *config, GcBootControl *control,
                         const GcBootInput *input) {
    uint16_t held = held_buttons(input);
    uint16_t directions = held & UINT16_C(0x000f);
    control->pressed_buttons = held & (uint16_t)~control->previous_buttons;
    if (control->pressed_buttons & GC_BOOT_PAD_B)
        control->pressed_buttons &= (uint16_t)~UINT16_C(0x1100);
    control->navigation_buttons = directions & (uint16_t)~control->previous_directions;
    if (directions != control->previous_directions || !directions) {
        control->repeat_tick = 0;
    } else {
        ++control->repeat_tick;
        if (control->repeat_tick == config->repeat_delay_ticks) {
            control->navigation_buttons = directions;
        } else if (control->repeat_tick >
                   config->repeat_delay_ticks + config->repeat_period_ticks) {
            control->navigation_buttons = directions;
            control->repeat_tick -= config->repeat_period_ticks;
        }
    }
    control->previous_buttons = held;
    control->previous_directions = directions;
}

static unsigned startup_sound(const GcBootInput *input) {
    if (!input->controllers[0].valid || !(input->controllers[0].held & GC_BOOT_PAD_Z))
        return 0;
    for (unsigned index = 1; index < GC_BOOT_CONTROLLER_COUNT; ++index) {
        if (!input->controllers[index].valid ||
            !(input->controllers[index].held & GC_BOOT_PAD_Z))
            return 2;
    }
    return 1;
}

static void poll_startup_sound(const GcBootConfig *config, GcBootControl *control,
                               const GcBootInput *input, GcBootEvents *events) {
    if (!control->sound_pending)
        return;
    unsigned sound = startup_sound(input);
    if (control->sound_counter > config->sound_thresholds[sound]) {
        control->sound_pending = false;
        emit_sound(events, sound);
    } else {
        ++control->sound_counter;
    }
}

static void decay_waves(const GcStartup *startup, GcBootControl *control) {
    for (unsigned wave = 0; wave < 2; ++wave) {
        float value = (float)control->waves[wave] * startup->wave_decay[wave];
        control->waves[wave] = value < (wave ? 80 : 10) ? 0 : (unsigned)value;
    }
    if (control->waves[0] || control->waves[1])
        control->wave_phase = (control->wave_phase + 1000) & 65535;
}

static void advance_kinetic(const GcStartup *startup, GcBootControl *control) {
    float amplitude = control->acceleration > 0
                          ? (float)control->kinetic + startup->kinetic_increment
                          : (float)control->kinetic * startup->kinetic_decay;
    if (amplitude < 1)
        amplitude = 0;
    if (amplitude > 0)
        control->kinetic_phase = (control->kinetic_phase + 7) & 65535;
    control->kinetic = (unsigned)fminf(amplitude, startup->spin_target);
    decay_waves(startup, control);
}

/* USA 0x8130bdbc / EUR 0x8130c250 retain the last driven angle and
 * velocity for a Hermite return, while acceleration and speed decay.
 * A is a held level and can only accelerate while the drawing is active.
 */
static void queue_cube_motion(const GcBootConfig *config, GcBootControl *control,
                              int16_t previous_angle, float threshold,
                              unsigned direction, GcBootEvents *events) {
    bool crossed = (previous_angle < 0 && control->angle >= 0) ||
                   (previous_angle > 0 && control->angle < 0);
    float fraction = fminf(1, control->velocity / config->startup->spin_target);
    if (crossed && fraction > threshold) {
        events->cube_motion = true;
        events->cube_direction = direction;
        events->cube_fraction = fraction;
    }
}

static void update_spin(const GcBootConfig *config, GcBootControl *control,
                        bool accelerate, GcBootEvents *events) {
    int16_t previous = control->angle;
    if (accelerate) {
        control->acceleration += config->startup->spin_acceleration;
        control->velocity += control->acceleration;
        control->angle = gc_angle_wrap((float)control->angle + control->velocity);
        control->return_angle = control->angle;
        control->return_velocity = control->velocity;
        control->return_tick = 0;
        ++control->spin_ticks;
        queue_cube_motion(config, control, previous, 0.4f, 0, events);
        return;
    }
    control->acceleration *= 0.8f;
    control->velocity *= 0.96f;
    if (control->acceleration < 0.1f)
        control->acceleration = 0;
    if (control->velocity < 0.01f)
        control->velocity = 0;
    unsigned time = control->return_tick;
    if (time > config->return_ticks)
        time = config->return_ticks;
    control->angle = gc_angle_wrap(
        cc_cubic_hermite((float)time, (float)config->return_ticks,
                         (float)control->return_angle, control->return_velocity, 0, 0));
    if (control->return_tick < config->return_ticks)
        ++control->return_tick;
    if (control->drawing_active)
        queue_cube_motion(config, control, previous, 0.4f, 0, events);
}

static bool advance_drawing(const GcBootConfig *config, GcBootControl *control) {
    if (control->drawing_complete)
        return true;
    GcStartupPose drawing;
    if (!gc_startup_sample(config->startup, control->drawing_tick, &drawing))
        return false;
    control->drawing_complete = drawing.drawing_ready;
    control->drawing_started = true;
    control->drawing_active = drawing.phase < GC_STARTUP_BOUNCE;
    memcpy(control->waves, drawing.waves, sizeof(control->waves));
    control->wave_phase = drawing.wave_phase;
    return true;
}

static bool spin_reached_menu(const GcBootConfig *config, GcBootControl *control) {
    if (control->velocity <= config->startup->spin_target)
        return false;
    control->next_phase = GC_BOOT_TRANSITION;
    control->angle = 0; /* Native route restores the scene's saved neutral angle. */
    return true;
}

static void fade_in_disc(GcBootControl *control, GcBootEvents *events) {
    unsigned fade = (unsigned)control->fader + 12;
    if (fade > 255) {
        control->fader = 255;
        control->next_phase = GC_BOOT_DISC_HANDOFF;
        events->disc_handoff = true;
    } else {
        control->fader = (uint8_t)fade;
    }
}

static void fade_out(GcBootControl *control) {
    control->fader = control->fader > 6 ? (uint8_t)(control->fader - 6) : 0;
}

static bool update_normal(const GcBootConfig *config, GcBootControl *control,
                          const GcBootInput *input, GcBootEvents *events) {
    bool was_complete = control->drawing_complete;
    if (!was_complete)
        update_spin(config, control,
                    (held_buttons(input) & GC_BOOT_PAD_A) && control->drawing_active,
                    events);
    advance_kinetic(config->startup, control);
    poll_startup_sound(config, control, input, events);
    if (!advance_drawing(config, control))
        return false;
    if (spin_reached_menu(config, control))
        return true;
    if (control->fatal_error) {
        if (control->error_ticks < 30)
            ++control->error_ticks;
        return true;
    }
    if (input->drive_state == GC_BOOT_DRIVE_UNRECOGNIZED ||
        input->drive_state == GC_BOOT_DRIVE_RETRY) {
        if (control->unrecognized_ticks < 30)
            ++control->unrecognized_ticks;
    } else if (control->unrecognized_ticks) {
        --control->unrecognized_ticks;
    }
    if (!control->absence_latched) {
        if (input->drive_state == GC_BOOT_DRIVE_READY && control->drawing_complete)
            fade_in_disc(control, events);
        else if (input->drive_state == GC_BOOT_DRIVE_ABSENT ||
                 input->drive_state == GC_BOOT_DRIVE_LID_OPEN)
            control->absence_latched = true;
        else if (input->drive_state == GC_BOOT_DRIVE_FATAL)
            control->fatal_error = true;
        return true;
    }
    if (!control->drawing_complete)
        return true;
    fade_out(control);
    if (!control->fader) {
        update_spin(config, control, true, events);
        advance_kinetic(config->startup, control);
        spin_reached_menu(config, control);
    }
    return true;
}

static bool update_lid_delay(const GcBootConfig *config, GcBootControl *control,
                             GcBootEvents *events) {
    if (!control->drawing_complete) {
        control->drawing_tick = config->startup->sequence_ticks - 1;
        if (!advance_drawing(config, control))
            return false;
        control->fader = 255;
        events->drawing_fast_forwarded = true;
    }
    /* USA 0x8130cec4 / EUR 0x8130d4e0 fast-forward drawing with selector
     * disabled, then decrement a 120-tick counter before accelerating.
     */
    fade_out(control);
    if (control->lid_wait_remaining) {
        --control->lid_wait_remaining;
    } else {
        update_spin(config, control, true, events);
        advance_kinetic(config->startup, control);
        spin_reached_menu(config, control);
    }
    return true;
}

/* EUR 0x8130cebc/0x8130d1bc fade the initial language panel. The
 * selector at 0x8130cd68 runs only after alpha would exceed 255.
 * Cancel reports event 13 and leaves this mandatory selector open.
 */
static bool update_language(GcBootControl *control, GcBootEvents *events) {
    if (control->language_pending) {
        unsigned alpha = (unsigned)control->language_alpha + 10;
        control->language_alpha = (uint8_t)(alpha > 255 ? 255 : alpha);
        if (alpha <= 255)
            return true;
        if (control->navigation_buttons & GC_BOOT_PAD_UP) {
            control->language_selection =
                control->language_selection ? control->language_selection - 1 : 5;
            emit_sound(events, 11);
        }
        if (control->navigation_buttons & GC_BOOT_PAD_DOWN) {
            control->language_selection = (control->language_selection + 1) % 6;
            emit_sound(events, 11);
        }
        if (control->pressed_buttons & GC_BOOT_PAD_A) {
            emit_sound(events, 8);
            control->language_pending = false;
            events->language_applied = true;
            events->language = control->language_selection;
        }
        if (control->pressed_buttons & GC_BOOT_PAD_B)
            emit_sound(events, 13);
        return true;
    }
    if (control->language_alpha) {
        control->language_alpha =
            control->language_alpha > 10 ? (uint8_t)(control->language_alpha - 10) : 0;
        if (!control->language_alpha && control->phase == GC_BOOT_RESET_PROMPT)
            control->fader = 255;
        return true;
    }
    return false;
}

static bool update_settings_notice(const GcBootConfig *config, GcBootControl *control,
                                   const GcBootInput *input, GcBootEvents *events) {
    if (input->drive_state == GC_BOOT_DRIVE_FATAL)
        control->fatal_error = true;
    if (control->fatal_error) {
        if (control->error_ticks < 30)
            ++control->error_ticks;
        return true;
    }
    if (update_language(control, events))
        return true;
    if (!control->drawing_complete) {
        advance_kinetic(config->startup, control);
        poll_startup_sound(config, control, input, events);
        return advance_drawing(config, control);
    }
    switch (control->notice_stage) {
        case 0:
            if (control->notice_counter)
                --control->notice_counter;
            else
                control->notice_stage = 1;
            break;
        case 1:
            if (control->notice_counter < 30)
                ++control->notice_counter;
            else
                control->notice_stage = 3;
            break;
        case 3:
            /* USA 0x8130ca10 waits for a fresh non-direction button edge. */
            if (control->pressed_buttons & UINT16_C(0x1f70)) {
                emit_sound(events, 8);
                control->notice_stage = 4;
            }
            break;
        case 4:
            if (control->notice_counter)
                --control->notice_counter;
            else
                control->notice_stage = 6;
            break;
        case 6:
            update_spin(config, control, true, events);
            advance_kinetic(config->startup, control);
            spin_reached_menu(config, control);
            break;
        default:
            return false;
    }
    return true;
}

static void reset_choice_input(GcBootControl *control, GcBootEvents *events) {
    if (control->navigation_buttons & GC_BOOT_PAD_UP) {
        if (control->reset_choice)
            emit_sound(events, 11);
        control->reset_choice = 0;
    }
    if (control->navigation_buttons & GC_BOOT_PAD_DOWN) {
        if (control->reset_choice != 1)
            emit_sound(events, 11);
        control->reset_choice = 1;
    }
    if (control->pressed_buttons & GC_BOOT_PAD_A) {
        emit_sound(events, control->reset_choice ? 7 : 8);
        control->reset_stage = control->reset_choice ? 2 : 3;
    }
    if (control->pressed_buttons & GC_BOOT_PAD_B) {
        emit_sound(events, 7);
        control->reset_stage = 2;
    }
}

static bool reset_route_to_menu(uint8_t drive) {
    return drive == GC_BOOT_DRIVE_ABSENT || drive == GC_BOOT_DRIVE_LID_OPEN ||
           drive == GC_BOOT_DRIVE_UNRECOGNIZED || drive == GC_BOOT_DRIVE_RETRY;
}

static void reset_wait_disc(GcBootControl *control, const GcBootInput *input,
                            GcBootEvents *events) {
    if (reset_route_to_menu(input->drive_state))
        control->reset_stage = 3;
    else if (input->drive_state == GC_BOOT_DRIVE_READY)
        fade_in_disc(control, events);
}

static bool update_reset_prompt(const GcBootConfig *config, GcBootControl *control,
                                const GcBootInput *input, GcBootEvents *events) {
    if (!control->drawing_complete) {
        control->drawing_tick = config->startup->sequence_ticks - 1;
        if (!advance_drawing(config, control))
            return false;
        if (config->startup->frame_rate != 50)
            control->fader = 255;
        events->drawing_fast_forwarded = true;
    }
    if (input->drive_state == GC_BOOT_DRIVE_FATAL)
        control->fatal_error = true;
    if (update_language(control, events))
        return true;
    if (!control->fader) {
        if (control->fatal_error) {
            if (control->reset_choice_counter)
                --control->reset_choice_counter;
            else if (control->error_ticks < 30)
                ++control->error_ticks;
            return true;
        }
        if (control->reset_delay < 20) {
            ++control->reset_delay;
        } else {
            switch (control->reset_stage) {
                case 0:
                    if (control->reset_body_counter < 30)
                        ++control->reset_body_counter;
                    else
                        control->reset_stage = 1;
                    break;
                case 1:
                    if (control->reset_choice_counter < 30)
                        ++control->reset_choice_counter;
                    reset_choice_input(control, events);
                    break;
                case 2:
                    if (control->reset_choice_counter)
                        --control->reset_choice_counter;
                    else if (control->reset_body_counter)
                        --control->reset_body_counter;
                    else
                        control->reset_stage = 5;
                    break;
                case 3:
                    if (control->reset_choice_counter)
                        --control->reset_choice_counter;
                    else
                        control->reset_stage = 4;
                    break;
                case 4:
                    if (control->reset_body_counter) {
                        --control->reset_body_counter;
                    } else {
                        update_spin(config, control, true, events);
                        advance_kinetic(config->startup, control);
                        if (spin_reached_menu(config, control))
                            return true;
                    }
                    break;
                case 5:
                    reset_wait_disc(control, input, events);
                    break;
                default:
                    return false;
            }
        }
    }
    if (control->reset_stage != 5)
        fade_out(control);
    else if (control->next_phase != GC_BOOT_DISC_HANDOFF)
        reset_wait_disc(control, input, events);
    return true;
}

static void update_transition(const GcBootConfig *config, GcBootControl *control,
                              GcBootEvents *events) {
    const GcStartup *startup = config->startup;
    unsigned tick = ++control->transition_tick;
    unsigned first_end = startup->transition_ticks[0];
    unsigned second_end = first_end + startup->transition_ticks[1];
    unsigned last_end = second_end + startup->transition_ticks[2];
    int16_t previous = control->angle;
    if (tick < first_end) {
        control->angle = gc_angle_wrap((float)control->angle + control->velocity);
        control->transition_saved_angle = control->angle;
    } else if (tick < second_end) {
        float first = -((float)control->transition_saved_angle +
                        (float)startup->transition_turns * 65536);
        control->angle = gc_angle_wrap(cc_cubic_hermite(
            (float)(tick - first_end), (float)startup->transition_ticks[1], first,
            startup->spin_target, (float)startup->transition_angle, 0));
        if (tick > first_end)
            control->velocity =
                (float)gc_angle_wrap((float)((int)control->angle - previous));
    } else if (tick < last_end) {
        control->angle = gc_angle_wrap(cc_cubic_hermite(
            (float)(tick - second_end), (float)startup->transition_ticks[2],
            (float)startup->transition_angle, 0, 0, 0));
        control->velocity =
            (float)gc_angle_wrap((float)((int)control->angle - previous));
    } else {
        control->angle = 0;
        control->velocity = 0;
        control->next_phase = GC_BOOT_MENU;
    }
    queue_cube_motion(config, control, previous, startup->spin_squash[0],
                      control->velocity <= startup->spin_target ? 1 : 0, events);
}

bool gc_boot_control_step(const GcBootConfig *config, GcBootControl *control,
                          const GcBootInput *input, GcBootEvents *events) {
    if (!config || !config->startup || !control || !input || !events)
        return false;
    *events = (GcBootEvents){.sound_event = -1};
    poll_actions(config, control, input);
    GcBootPhase previous = control->phase;
    control->phase = control->next_phase;
    if (control->has_frame) {
        ++control->video_tick;
        if (control->drawing_started && !control->drawing_complete &&
            control->phase != GC_BOOT_TRANSITION && control->phase != GC_BOOT_MENU)
            ++control->drawing_tick;
    }
    bool okay = true;
    switch (control->phase) {
        case GC_BOOT_NORMAL:
            okay = update_normal(config, control, input, events);
            break;
        case GC_BOOT_LID_DELAY:
            okay = update_lid_delay(config, control, events);
            break;
        case GC_BOOT_SETTINGS_NOTICE:
            okay = update_settings_notice(config, control, input, events);
            break;
        case GC_BOOT_RESET_PROMPT:
            okay = update_reset_prompt(config, control, input, events);
            break;
        case GC_BOOT_TRANSITION:
            update_transition(config, control, events);
            break;
        case GC_BOOT_MENU:
            events->menu_begin = previous != GC_BOOT_MENU;
            if (!events->menu_begin)
                ++control->menu_tick;
            break;
        case GC_BOOT_DISC_HANDOFF:
            break;
        default:
            okay = false;
            break;
    }
    control->has_frame = true;
    return okay;
}

static uint8_t amplitude_alpha(unsigned amplitude) {
    if (amplitude > 3000)
        amplitude = 3000;
    return (uint8_t)(amplitude * 255 / 3000);
}

bool gc_boot_control_sample_pose(const GcBootConfig *config,
                                 const GcBootControl *control, GcStartupPose *pose) {
    if (!config || !config->startup || !control || !pose ||
        !gc_startup_sample(config->startup, control->drawing_tick, pose))
        return false;
    const GcStartup *startup = config->startup;
    pose->complete = control->phase == GC_BOOT_MENU;
    memcpy(pose->waves, control->waves, sizeof(pose->waves));
    pose->wave_phase = control->wave_phase;
    GcStartupMotion motion = {.velocity = control->velocity,
                              .kinetic = control->kinetic,
                              .kinetic_phase = control->kinetic_phase,
                              .waves = {control->waves[0], control->waves[1]},
                              .wave_phase = control->wave_phase,
                              .spin_angle = control->angle};
    gc_startup_motion_scene(startup, &motion, pose->scene_matrix);
    pose->base_cube_alpha = amplitude_alpha(control->waves[0] + control->waves[1]);
    pose->cover_cube_alpha = amplitude_alpha(control->kinetic);
    pose->scene_phase =
        control->velocity > 0 ? GC_STARTUP_SCENE_SPINNING : GC_STARTUP_SCENE_DRAWING;
    float blend = control->velocity / startup->spin_target;
    pose->model_scale[0] = pose->model_scale[2] =
        1 - (1 - startup->spin_squash[0]) * blend;
    pose->model_scale[1] = 1 + startup->spin_squash[1] * blend;
    if (control->drawing_complete)
        pose->trail_count = 0;
    if (control->phase == GC_BOOT_SETTINGS_NOTICE) {
        /* USA 0x8130d3b8 fades the logotype through counter 0x66, while
         * the mark retains the drawing reveal alpha at 0x5b.
         */
        pose->logotype_alpha = control->notice_stage == 0
                                   ? (uint8_t)(control->notice_counter * 255 / 30)
                                   : 0;
    } else if (control->phase == GC_BOOT_RESET_PROMPT) {
        pose->boot_mark_alpha = (uint8_t)(255 - control->reset_body_counter * 150 / 30);
        pose->logotype_alpha = pose->boot_mark_alpha;
    }
    if (control->language_pending || control->language_alpha) {
        pose->base_cube_alpha = pose->boot_mark_alpha = pose->cover_cube_alpha = 0;
        pose->cover_alpha = pose->moving_cube_alpha = pose->logotype_alpha = 0;
        pose->trail_count = 0;
    }
    if (control->phase != GC_BOOT_TRANSITION && control->phase != GC_BOOT_MENU)
        return true;

    pose->scene_phase = control->phase == GC_BOOT_MENU ? GC_STARTUP_SCENE_MENU
                                                       : GC_STARTUP_SCENE_TRANSITION;
    pose->perspective = true;
    pose->boot_mark_alpha = pose->cover_alpha = pose->cover_cube_alpha = 0;
    pose->moving_cube_alpha = 0;
    pose->trail_count = 0;
    pose->base_cube_alpha = pose->logotype_alpha = 0;
    unsigned counter = control->transition_tick + 1;
    unsigned fade_ticks = startup->transition_fade_ticks;
    pose->glass_cube_alpha = 255;
    if (counter <= fade_ticks) {
        unsigned combined = control->waves[0] + control->waves[1] + control->kinetic;
        if (combined > 3000)
            combined = 3000;
        pose->base_cube_alpha =
            (uint8_t)((fade_ticks - counter) * 255 * combined / (fade_ticks * 3000));
        pose->logotype_alpha = pose->base_cube_alpha;
        pose->glass_cube_alpha = (uint8_t)(counter * 255 / fade_ticks);
    }
    unsigned last = startup->transition_ticks[0] + startup->transition_ticks[1] +
                    startup->transition_ticks[2];
    unsigned wordmark_start = last > 100 ? last - 100 : 0;
    unsigned wordmark = counter >= last ? 255
                        : counter > wordmark_start
                            ? (counter - wordmark_start) * 255 / (last - wordmark_start)
                            : 0;
    pose->menu_labels_alpha = (uint8_t)(wordmark * wordmark / 255);
    for (unsigned axis = 0; axis < 3; ++axis)
        pose->glass_scale[axis] = 1.1f * (1 - blend) + pose->model_scale[axis] * blend;
    motion.menu_phase =
        control->transition_tick ? (control->transition_tick - 1) * 7 : 0;
    if (control->phase == GC_BOOT_MENU)
        motion.menu_phase = (control->transition_tick + control->menu_tick + 1) * 7;
    gc_startup_motion_glass(startup, &motion, pose->glass_matrix);
    return true;
}

void gc_boot_control_ui(const GcBootControl *control, GcBootUi *ui) {
    if (!ui)
        return;
    *ui = (GcBootUi){0};
    if (!control)
        return;
    ui->notice_alpha =
        control->notice_stage == 0 ? 0 : (uint8_t)(control->notice_counter * 255 / 30);
    ui->reset_alpha = (uint8_t)(control->reset_body_counter * 255 / 30);
    ui->choices_alpha = (uint8_t)(control->reset_choice_counter * 255 / 30);
    ui->language_alpha = control->language_alpha;
    ui->error_alpha = (uint8_t)(control->error_ticks * 255 / 30);
    ui->unrecognized_alpha = (uint8_t)(control->unrecognized_ticks * 255 / 30);
    ui->reset_choice = control->reset_choice;
    ui->language_selection = control->language_selection;
}
