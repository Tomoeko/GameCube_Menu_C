#include "gamecube/boot_control.h"
#include "gamecube/angle.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static void test_angle_wrapping(void) {
    assert(gc_angle_wrap(0) == 0);
    assert(gc_angle_wrap(32767.75f) == INT16_MAX);
    assert(gc_angle_wrap(32768) == INT16_MIN);
    assert(gc_angle_wrap(-32769) == INT16_MAX);
    assert(gc_angle_wrap(65536) == 0);
    assert(gc_angle_wrap(-65536) == 0);
    assert(gc_angle_wrap(65537.75f) == 1);
    assert(gc_angle_wrap(-65537.75f) == -1);
    assert(gc_angle_wrap(-0.75f) == 0);

    /* Both pose paths round-trip signed native angles through radians. */
    const float unit = 3.14159265358979323846f / 32768;
    const float inverse = 32768 / 3.14159265358979323846f;
    for (int units = INT16_MIN; units <= INT16_MAX; ++units) {
        float radians = (float)units * unit;
        assert(lrintf(radians / unit) == units);
        assert(lrintf(radians * inverse) == units);
    }
}

static GcStartup synthetic_startup(void) {
    GcStartup startup = {0};
    startup.frame_rate = 60;
    startup.roll_ticks = startup.corner_ticks = 10;
    startup.drop_ticks = 10;
    startup.drop_wait_ticks = 12;
    startup.bounce_ticks = startup.rise_ticks = 12;
    startup.reveal_ticks = startup.logotype_ticks = 30;
    startup.drop_height = 10;
    startup.bounce_height = 20;
    startup.rise_velocity = 10;
    startup.cube_edge = 54;
    startup.bounce_quarter_turns = 2;
    startup.wave_decay[0] = startup.wave_decay[1] = 0.5f;
    startup.wave_translation[0] = startup.wave_translation[1] = 0.01f;
    startup.step_count = 33;
    for (size_t index = 0; index < startup.step_count; ++index)
        startup.steps[index] = (GcStartupStep){0, 1, 255};
    startup.steps[startup.step_count - 1].direction = 7;
    startup.spin_target = 5000;
    startup.spin_acceleration = 1.5f;
    startup.scene_origin_offset = 15;
    startup.scene_base_angles[0] = 8192;
    startup.scene_base_angles[1] = -6420;
    startup.scene_base_angles[2] = -5461;
    startup.transition_base_angles[0] = 8191;
    startup.transition_base_angles[1] = -6371;
    startup.transition_base_angles[2] = -5461;
    startup.kinetic_phase_rates[0] = 90;
    startup.kinetic_phase_rates[1] = 60;
    startup.kinetic_phase_rates[2] = 70;
    startup.spin_ticks = 82;
    startup.spin_squash[0] = 0.8f;
    startup.spin_squash[1] = 0.4f;
    startup.kinetic_increment = 30;
    startup.kinetic_decay = 0.96f;
    startup.transition_ticks[0] = 15;
    startup.transition_ticks[1] = 80;
    startup.transition_ticks[2] = 50;
    startup.transition_turns = 3;
    startup.transition_angle = 5000;
    startup.transition_fade_ticks = 30;
    for (unsigned tick = 0; tick < 2000; ++tick) {
        GcStartupPose pose;
        assert(gc_startup_sample(&startup, tick, &pose));
        if (pose.complete) {
            startup.sequence_ticks = tick;
            break;
        }
    }
    assert(startup.sequence_ticks > 82);
    startup.menu_ticks = startup.sequence_ticks - 1 + 82 + 145;
    return startup;
}

static GcBootInput input_for(uint8_t drive, uint16_t first_held) {
    GcBootInput input = {0};
    input.drive_state = drive;
    input.controllers[0] = (GcBootPad){true, first_held};
    return input;
}

static void step(const GcBootConfig *config, GcBootControl *control,
                 const GcBootInput *input, GcBootEvents *events) {
    assert(gc_boot_control_step(config, control, input, events));
}

static void test_sound(const GcBootConfig *config, GcBootInput input,
                       unsigned expected_sound) {
    GcBootControl control;
    assert(gc_boot_control_init(config, &control, GC_BOOT_NORMAL));
    GcBootEvents events;
    unsigned firing_tick = config->sound_thresholds[expected_sound] + 1;
    for (unsigned tick = 0; tick < 20; ++tick) {
        step(config, &control, &input, &events);
        assert(events.sound_event == (tick == firing_tick ? (int)expected_sound : -1));
    }
}

static void test_sound_selector(const GcBootConfig *config) {
    GcBootInput input = input_for(GC_BOOT_DRIVE_PENDING, 0);
    test_sound(config, input, 0);
    input.controllers[0].held = GC_BOOT_PAD_Z;
    test_sound(config, input, 2);
    for (unsigned index = 1; index < GC_BOOT_CONTROLLER_COUNT; ++index)
        input.controllers[index] = (GcBootPad){true, GC_BOOT_PAD_Z};
    test_sound(config, input, 1);
    input.controllers[3].valid = false;
    test_sound(config, input, 2);
    input.controllers[3].valid = true;
    input.controllers[0].valid = false;
    test_sound(config, input, 0);
    input.controllers[0].valid = true;
    input.controllers[0].held = 0;
    test_sound(config, input, 0); /* Z on other ports does not select a secret. */

    GcBootControl control;
    GcBootEvents events;
    assert(gc_boot_control_init(config, &control, GC_BOOT_NORMAL));
    input.controllers[0].held = GC_BOOT_PAD_Z;
    step(config, &control, &input, &events);
    step(config, &control, &input, &events);
    assert(events.sound_event == -1);
    input.controllers[0].held = 0;
    step(config, &control, &input, &events);
    assert(events.sound_event == 0); /* Selection stays live until the cue fires. */
    input.controllers[0].held = GC_BOOT_PAD_Z;
    step(config, &control, &input, &events);
    assert(events.sound_event == -1);
}

static void test_normal_absence(const GcBootConfig *config) {
    GcBootInput input = input_for(GC_BOOT_DRIVE_ABSENT, 0);
    GcBootControl control;
    GcBootEvents events;
    assert(gc_boot_control_init(config, &control, GC_BOOT_NORMAL));
    for (unsigned tick = 0; tick <= config->startup->menu_ticks; ++tick) {
        step(config, &control, &input, &events);
        assert(!events.disc_handoff);
        assert(events.menu_begin == (tick == config->startup->menu_ticks));
        if (tick == config->startup->sequence_ticks)
            assert(control.kinetic == (unsigned)config->startup->kinetic_increment * 3);
        GcStartupPose pose;
        assert(gc_boot_control_sample_pose(config, &control, &pose));
        for (unsigned element = 0; element < 12; ++element)
            assert(isfinite(pose.scene_matrix[element]) &&
                   isfinite(pose.glass_matrix[element]));
    }
    assert(control.phase == GC_BOOT_MENU && control.spin_ticks == 82);
    step(config, &control, &input, &events);
    assert(!events.menu_begin);
}

static void assert_matrix_close(const float actual[12], const float expected[12]) {
    for (unsigned element = 0; element < 12; ++element)
        assert(fabsf(actual[element] - expected[element]) < 0.0001f);
}

static void test_native_spin_profile(const GcBootConfig *config) {
    GcStartup startup = *config->startup;
    memset(startup.menu_profile, 0, sizeof(startup.menu_profile));
    GcBootConfig isolated = *config;
    isolated.startup = &startup;
    GcBootControl control;
    GcStartupPose pose;
    assert(gc_boot_control_init(&isolated, &control, GC_BOOT_NORMAL));
    control.phase = GC_BOOT_TRANSITION;
    /* Independently evaluated native 074cc Euler equation with its
     * quantized sine table. The bcd8/c130 reset clears the constructor
     * weight: formation keeps its baseline, while the glass profile at
     * target velocity uses (8191,-6371,-5461). Zero idle amplitudes isolate
     * the reset's effect from the menu oscillation profile.
     */
    const float formation[12] = {0.706098605f,  0.000245367f,  -0.708113417f,
                                 -0.026541679f, -0.408629465f, 0.816837360f,
                                 -0.407183892f, 0.015360053f,  0.578313529f,
                                 0.576867910f,  0.576867910f,  25.980740232f};
    const float glass[12] = {0.708394557f,  0.003629502f, -0.705807237f, 0,
                             -0.409958165f, 0.816131175f, -0.407264319f, 0,
                             0.574553072f,  0.577855193f, 0.579630751f,  0};
    control.velocity = startup.spin_target;
    assert(gc_boot_control_sample_pose(&isolated, &control, &pose));
    assert_matrix_close(pose.scene_matrix, formation);
    assert_matrix_close(pose.glass_matrix, glass);
    /* The global spin is positive Y: it preserves Y, sends Z to X, and
     * sends X to negative Z. This catches swapped axes and concat order.
     */
    control.angle = 16384;
    assert(gc_boot_control_sample_pose(&isolated, &control, &pose));
    for (unsigned column = 0; column < 4; ++column) {
        assert(fabsf(pose.scene_matrix[column] - formation[8 + column]) < 0.0001f);
        assert(fabsf(pose.scene_matrix[4 + column] - formation[4 + column]) < 0.0001f);
        assert(fabsf(pose.scene_matrix[8 + column] + formation[column]) < 0.0001f);
        assert(fabsf(pose.glass_matrix[column] - glass[8 + column]) < 0.0001f);
        assert(fabsf(pose.glass_matrix[4 + column] - glass[4 + column]) < 0.0001f);
        assert(fabsf(pose.glass_matrix[8 + column] + glass[column]) < 0.0001f);
    }
    assert(pose.model_scale[0] == startup.spin_squash[0]);
    assert(pose.model_scale[1] == 1 + startup.spin_squash[1]);
}

static void test_disc_ready(const GcBootConfig *config) {
    GcBootInput input = input_for(GC_BOOT_DRIVE_READY, 0);
    GcBootControl control;
    GcBootEvents events;
    assert(gc_boot_control_init(config, &control, GC_BOOT_NORMAL));
    unsigned handoff = config->startup->sequence_ticks + 20;
    for (unsigned tick = 0; tick <= handoff; ++tick) {
        step(config, &control, &input, &events);
        assert(events.disc_handoff == (tick == handoff));
        assert(!events.menu_begin && !control.spin_ticks);
    }
    assert(control.fader == 255);
    step(config, &control, &input, &events);
    assert(control.phase == GC_BOOT_DISC_HANDOFF && !events.disc_handoff);
}

static void test_drive_waits_and_latches(const GcBootConfig *config) {
    GcBootInput input = input_for(GC_BOOT_DRIVE_PENDING, 0);
    GcBootControl control;
    GcBootEvents events;
    assert(gc_boot_control_init(config, &control, GC_BOOT_NORMAL));
    unsigned delay = config->startup->menu_ticks + 100;
    for (unsigned tick = 0; tick < delay; ++tick)
        step(config, &control, &input, &events);
    assert(control.phase == GC_BOOT_NORMAL && control.drawing_complete);
    assert(!control.absence_latched && control.velocity == 0);
    input.controllers[0].held = GC_BOOT_PAD_A;
    step(config, &control, &input, &events);
    assert(control.velocity == 0); /* Late A cannot revive an inactive drawing. */
    input.drive_state = GC_BOOT_DRIVE_UNRECOGNIZED;
    for (unsigned tick = 0; tick < 60; ++tick)
        step(config, &control, &input, &events);
    assert(control.unrecognized_ticks == 30 && !control.absence_latched);
    input.drive_state = GC_BOOT_DRIVE_ABSENT;
    step(config, &control, &input, &events);
    assert(control.absence_latched && control.velocity == 0);
    input.drive_state = GC_BOOT_DRIVE_READY;
    for (unsigned tick = 0; tick < 82 + 145 + 1; ++tick) {
        step(config, &control, &input, &events);
        assert(!events.disc_handoff);
    }
    assert(events.menu_begin && control.phase == GC_BOOT_MENU);

    assert(gc_boot_control_init(config, &control, GC_BOOT_NORMAL));
    input.drive_state = GC_BOOT_DRIVE_LID_OPEN;
    step(config, &control, &input, &events);
    assert(control.absence_latched && !events.drawing_fast_forwarded);
    input.drive_state = GC_BOOT_DRIVE_FATAL;
    input.controllers[0].held = 0;
    assert(gc_boot_control_init(config, &control, GC_BOOT_NORMAL));
    for (unsigned tick = 0; tick < 50; ++tick)
        step(config, &control, &input, &events);
    assert(control.fatal_error && control.error_ticks == 30);
    input.drive_state = GC_BOOT_DRIVE_READY;
    for (unsigned tick = 0; tick < delay; ++tick) {
        step(config, &control, &input, &events);
        assert(!events.disc_handoff && !events.menu_begin);
    }
}

static void test_held_a_and_release(const GcBootConfig *config) {
    GcBootControl control;
    GcBootEvents events;
    GcBootInput input = input_for(GC_BOOT_DRIVE_READY, GC_BOOT_PAD_A);
    assert(gc_boot_control_init(config, &control, GC_BOOT_NORMAL));
    for (unsigned tick = 0; tick < 82; ++tick) {
        step(config, &control, &input, &events);
        unsigned spin = tick + 1;
        bool motion = spin == 64 || spin == 73 || spin == 80;
        assert(events.cube_motion == motion);
        if (motion) {
            float velocity =
                config->startup->spin_acceleration * (float)(spin * (spin + 1)) / 2;
            assert(events.cube_direction == 0);
            assert(fabsf(events.cube_fraction - velocity / 5000) < 0.0001f);
        }
    }
    assert(control.next_phase == GC_BOOT_TRANSITION && !control.drawing_complete);
    assert(control.kinetic == (unsigned)config->startup->kinetic_increment * 82);
    unsigned frozen = control.drawing_tick;
    step(config, &control, &input, &events);
    GcStartupPose pose;
    assert(gc_boot_control_sample_pose(config, &control, &pose));
    assert(control.phase == GC_BOOT_TRANSITION && control.transition_tick == 1);
    assert(control.drawing_tick == frozen && pose.phase < GC_STARTUP_BOUNCE);
    assert(!pose.moving_cube_alpha && !pose.trail_count);
    assert(pose.glass_cube_alpha == 17 && pose.perspective);
    for (unsigned tick = 1; tick < 145 + 1; ++tick) {
        step(config, &control, &input, &events);
        assert(!events.disc_handoff && control.drawing_tick == frozen);
    }
    assert(events.menu_begin);

    assert(gc_boot_control_init(config, &control, GC_BOOT_NORMAL));
    input = input_for(GC_BOOT_DRIVE_PENDING, 0);
    input.controllers[3] = (GcBootPad){true, GC_BOOT_PAD_A};
    for (unsigned tick = 0; tick < 20; ++tick)
        step(config, &control, &input, &events);
    float acceleration = control.acceleration, velocity = control.velocity;
    int16_t driven_angle = control.angle;
    input.controllers[3].held = 0;
    step(config, &control, &input, &events);
    assert(fabsf(control.acceleration - acceleration * 0.8f) < 0.0001f);
    assert(fabsf(control.velocity - velocity * 0.96f) < 0.0001f);
    assert(control.angle == driven_angle);
    for (unsigned tick = 0; tick < config->return_ticks; ++tick)
        step(config, &control, &input, &events);
    assert(control.angle == 0 && control.acceleration == 0);
    assert(control.phase == GC_BOOT_NORMAL && control.spin_ticks == 20);
}

static void test_menu_request(const GcBootConfig *config) {
    for (unsigned phase = GC_STARTUP_DROP; phase <= GC_STARTUP_COMPLETE; ++phase) {
        GcBootControl control;
        GcBootEvents events;
        GcBootInput input = input_for(GC_BOOT_DRIVE_PENDING, 0);
        GcStartupPose pose;
        assert(gc_boot_control_init(config, &control, GC_BOOT_NORMAL));
        for (unsigned tick = 0; tick <= config->startup->sequence_ticks; ++tick) {
            step(config, &control, &input, &events);
            assert(gc_boot_control_sample_pose(config, &control, &pose));
            bool reached = phase == GC_STARTUP_COMPLETE
                               ? control.drawing_complete
                               : pose.phase == (GcStartupPhase)phase;
            if (reached && control.drawing_tick)
                break;
        }
        /* The controller stops drawing at its ready flag, before the sampler's
         * synthetic COMPLETE phase. Exercise that late input window as well. */
        if (phase == GC_STARTUP_COMPLETE)
            assert(control.drawing_complete);
        else
            assert(pose.phase == (GcStartupPhase)phase);
        unsigned drawing_tick = control.drawing_tick;
        int16_t angle = control.angle;
        gc_boot_control_request_menu(&control);
        assert(control.menu_requested && control.velocity == 0 &&
               control.angle == angle && control.drawing_tick == drawing_tick);
        input.drive_state = phase & 1 ? GC_BOOT_DRIVE_READY : GC_BOOT_DRIVE_ABSENT;
        for (unsigned tick = 1; tick <= 82; ++tick) {
            float velocity = control.velocity;
            step(config, &control, &input, &events);
            assert(control.spin_ticks == tick && control.velocity > velocity);
            assert(!events.menu_begin && !events.disc_handoff &&
                   !events.drawing_fast_forwarded && !control.fader);
            assert(control.kinetic ==
                   (unsigned)config->startup->kinetic_increment * tick);
        }
        assert(control.next_phase == GC_BOOT_TRANSITION);
        unsigned frozen = control.drawing_tick;
        for (unsigned tick = 1; tick <= 145; ++tick) {
            step(config, &control, &input, &events);
            assert(control.phase == GC_BOOT_TRANSITION &&
                   control.transition_tick == tick);
            assert(control.drawing_tick == frozen && !events.menu_begin &&
                   !events.disc_handoff);
            assert(gc_boot_control_sample_pose(config, &control, &pose));
            assert(!pose.moving_cube_alpha && !pose.trail_count && pose.perspective);
            if (tick == 1)
                assert(pose.glass_cube_alpha == 17 && !pose.menu_labels_alpha);
        }
        assert(pose.glass_cube_alpha == 255 && pose.menu_labels_alpha == 255);
        step(config, &control, &input, &events);
        assert(events.menu_begin && !events.disc_handoff &&
               control.phase == GC_BOOT_MENU);
        step(config, &control, &input, &events);
        assert(!events.menu_begin);
        assert(gc_boot_control_init(config, &control, GC_BOOT_NORMAL));
        assert(!control.menu_requested);
    }

    const GcBootPhase dialogs[] = {GC_BOOT_SETTINGS_NOTICE, GC_BOOT_RESET_PROMPT,
                                   GC_BOOT_LID_DELAY};
    GcBootControl control;
    for (unsigned index = 0; index < sizeof(dialogs) / sizeof(dialogs[0]); ++index) {
        assert(gc_boot_control_init(config, &control, dialogs[index]));
        gc_boot_control_request_menu(&control);
        assert(!control.menu_requested);
    }
    assert(gc_boot_control_init(config, &control, GC_BOOT_NORMAL));
    control.next_phase = GC_BOOT_DISC_HANDOFF;
    gc_boot_control_request_menu(&control);
    assert(!control.menu_requested);
    control.next_phase = GC_BOOT_NORMAL;
    control.fatal_error = true;
    gc_boot_control_request_menu(&control);
    assert(!control.menu_requested);
    gc_boot_control_request_menu(NULL);

    assert(gc_boot_control_init(config, &control, GC_BOOT_NORMAL));
    GcBootInput input = input_for(GC_BOOT_DRIVE_READY, 0);
    GcBootEvents events;
    while (!control.drawing_complete)
        step(config, &control, &input, &events);
    assert(control.fader > 0 && !events.disc_handoff);
    gc_boot_control_request_menu(&control);
    for (unsigned tick = 0; tick < 82 + 145 + 1; ++tick) {
        step(config, &control, &input, &events);
        assert(!events.disc_handoff);
    }
    assert(events.menu_begin && control.phase == GC_BOOT_MENU && !control.fader);

    assert(gc_boot_control_init(config, &control, GC_BOOT_NORMAL));
    gc_boot_control_request_menu(&control);
    step(config, &control, &input, &events);
    input.drive_state = GC_BOOT_DRIVE_FATAL;
    for (unsigned tick = 0; tick < 82 + 145 + 1; ++tick) {
        step(config, &control, &input, &events);
        assert(!events.disc_handoff && !events.menu_begin);
    }
    assert(control.fatal_error && !control.menu_requested && control.error_ticks == 30);
}

static void test_lid_delay(const GcBootConfig *config) {
    GcBootControl control;
    GcBootEvents events;
    GcBootInput input = input_for(GC_BOOT_DRIVE_READY, GC_BOOT_PAD_Z | GC_BOOT_PAD_A);
    assert(gc_boot_control_init(config, &control, GC_BOOT_LID_DELAY));
    unsigned menu_tick = config->lid_wait_ticks + 82 + 145;
    for (unsigned tick = 0; tick <= menu_tick; ++tick) {
        step(config, &control, &input, &events);
        assert(events.sound_event == -1 && !events.disc_handoff);
        assert(events.drawing_fast_forwarded == (tick == 0));
        assert(events.menu_begin == (tick == menu_tick));
        if (tick < config->lid_wait_ticks)
            assert(control.velocity == 0);
    }
    assert(control.fader == 0 && control.phase == GC_BOOT_MENU);
}

static void test_configuration_notice(const GcBootConfig *config) {
    GcBootControl control;
    GcBootEvents events;
    GcBootInput input = input_for(GC_BOOT_DRIVE_READY, 0);
    assert(gc_boot_control_init(config, &control, GC_BOOT_SETTINGS_NOTICE));
    unsigned confirmations = 0;
    for (unsigned tick = 0; tick < config->startup->menu_ticks + 150; ++tick) {
        input.controllers[0].held =
            (control.language_pending && control.language_alpha == 255) ||
                    (!control.language_pending && control.notice_stage == 3)
                ? GC_BOOT_PAD_A
                : 0;
        step(config, &control, &input, &events);
        confirmations += events.sound_event == 8 && !events.language_applied;
        assert(!events.disc_handoff);
        if (control.phase == GC_BOOT_SETTINGS_NOTICE && !control.language_pending &&
            !control.language_alpha) {
            GcStartupPose drawing, pose;
            assert(gc_startup_sample(config->startup, control.drawing_tick, &drawing));
            assert(gc_boot_control_sample_pose(config, &control, &pose));
            assert(pose.boot_mark_alpha == drawing.boot_mark_alpha);
            if (control.notice_stage)
                assert(pose.logotype_alpha == 0);
        }
        if (events.menu_begin)
            break;
    }
    assert(confirmations == 1 && control.phase == GC_BOOT_MENU);
}

static void enter_reset_prompt(const GcBootConfig *config, GcBootControl *control,
                               GcBootInput *input) {
    assert(gc_boot_control_init(config, control, GC_BOOT_RESET_PROMPT));
    GcBootEvents events;
    for (unsigned tick = 0; tick < 300; ++tick) {
        input->controllers[0].held =
            control->language_pending && control->language_alpha == 255 ? GC_BOOT_PAD_A
                                                                        : 0;
        step(config, control, input, &events);
        assert(!events.disc_handoff && !events.menu_begin);
        if (control->reset_stage == 1 && control->reset_choice_counter == 30)
            return;
    }
    assert(false);
}

static void test_reset_prompt(const GcBootConfig *config) {
    GcBootControl control;
    GcBootEvents events;
    GcBootInput input = input_for(GC_BOOT_DRIVE_READY, 0);
    enter_reset_prompt(config, &control, &input);
    GcBootUi ui;
    gc_boot_control_ui(&control, &ui);
    assert(ui.reset_alpha == 255 && ui.choices_alpha == 255 && ui.reset_choice == 1);
    input.controllers[0].held = GC_BOOT_PAD_UP | GC_BOOT_PAD_A;
    step(config, &control, &input, &events);
    assert(control.reset_choice == 0 && control.reset_stage == 3);
    assert(events.sound_event_count == 2 && events.sound_events[0] == 11 &&
           events.sound_events[1] == 8);
    input.controllers[0].held = 0;
    for (unsigned tick = 0; tick < 400 && !events.menu_begin; ++tick) {
        step(config, &control, &input, &events);
        assert(!events.disc_handoff);
    }
    assert(events.menu_begin && control.phase == GC_BOOT_MENU);

    enter_reset_prompt(config, &control, &input);
    input.controllers[0].held = GC_BOOT_PAD_A | GC_BOOT_PAD_B;
    step(config, &control, &input, &events);
    assert(control.reset_stage == 2 && events.sound_event_count == 1 &&
           events.sound_event == 7); /* Native B suppresses simultaneous A. */
    input.controllers[0].held = 0;
    for (unsigned tick = 0; tick < 100; ++tick) {
        step(config, &control, &input, &events);
        if (events.disc_handoff)
            break;
    }
    assert(events.disc_handoff && control.fader == 255);

    input.drive_state = GC_BOOT_DRIVE_PENDING;
    enter_reset_prompt(config, &control, &input);
    input.controllers[0].held = GC_BOOT_PAD_B;
    step(config, &control, &input, &events);
    input.controllers[0].held = 0;
    for (unsigned tick = 0; tick < 120; ++tick) {
        step(config, &control, &input, &events);
        assert(!events.disc_handoff && !events.menu_begin);
    }
    assert(control.reset_stage == 5 && !control.reset_body_counter);
    input.drive_state = GC_BOOT_DRIVE_RETRY;
    step(config, &control, &input, &events);
    assert(control.reset_stage == 3);
    for (unsigned tick = 0; tick < 400 && !events.menu_begin; ++tick)
        step(config, &control, &input, &events);
    assert(events.menu_begin);
}

static void test_initial_language(const GcBootConfig *config) {
    if (config->startup->frame_rate != 50)
        return;
    GcBootControl control;
    GcBootEvents events;
    GcBootInput input = input_for(GC_BOOT_DRIVE_ABSENT, 0);
    GcBootConfig selected = *config;
    selected.initial_language = 4; /* Native and local Italian row. */
    assert(gc_boot_control_init(&selected, &control, GC_BOOT_SETTINGS_NOTICE));
    assert(control.language_selection == 4);
    GcBootControl before;
    memcpy(&before, &control, sizeof(before));
    selected.initial_language = 6;
    assert(!gc_boot_control_init(&selected, &control, GC_BOOT_SETTINGS_NOTICE));
    assert(!memcmp(&control, &before, sizeof(control)));
    assert(!gc_boot_control_init(&selected, &control, GC_BOOT_RESET_PROMPT));
    assert(!memcmp(&control, &before, sizeof(control)));
    assert(gc_boot_control_init(config, &control, GC_BOOT_SETTINGS_NOTICE));
    for (unsigned tick = 0; tick < 26; ++tick) {
        step(config, &control, &input, &events);
        assert(!control.drawing_started && control.drawing_tick == 0);
        assert(events.sound_event == -1 && !events.language_applied);
    }
    assert(control.language_alpha == 255 && control.language_pending);
    input.controllers[0].held = GC_BOOT_PAD_UP;
    step(config, &control, &input, &events);
    assert(control.language_selection == 5 && events.sound_event == 11);
    for (unsigned tick = 1; tick <= 56; ++tick) {
        step(config, &control, &input, &events);
        bool repeat = tick == 35 || tick == 46 || tick == 56;
        assert((events.sound_event == 11) == repeat);
    }
    assert(control.language_selection == 2);
    input.controllers[0].held = GC_BOOT_PAD_B;
    step(config, &control, &input, &events);
    assert(events.sound_event == 13 && control.language_pending);
    input.controllers[0].held = GC_BOOT_PAD_A;
    step(config, &control, &input, &events);
    assert(events.sound_event == 8 && events.language_applied && events.language == 2);
    /* The applied raw index is the native and local French row. */
    for (unsigned tick = 0; tick < 26; ++tick) {
        step(config, &control, &input, &events);
        assert(!control.drawing_started && events.sound_event == -1);
    }
    assert(control.language_alpha == 0);
    input.controllers[0].held = 0;
    step(config, &control, &input, &events);
    assert(control.drawing_started && control.drawing_tick == 0);
    assert(events.sound_event == -1);
    step(config, &control, &input, &events);
    assert(events.sound_event == 0);
}

static void test_all(const GcStartup *startup) {
    GcBootConfig config;
    assert(gc_boot_config_init(&config, startup));
    assert(config.sound_thresholds[0] == 0 && config.lid_wait_ticks == 120);
    assert(config.sound_thresholds[1] == (startup->frame_rate == 50 ? 6 : 7));
    assert(config.sound_thresholds[2] == (startup->frame_rate == 50 ? 5 : 6));
    test_sound_selector(&config);
    test_normal_absence(&config);
    test_native_spin_profile(&config);
    test_disc_ready(&config);
    test_drive_waits_and_latches(&config);
    test_held_a_and_release(&config);
    test_menu_request(&config);
    test_lid_delay(&config);
    test_configuration_notice(&config);
    test_reset_prompt(&config);
    test_initial_language(&config);
}

int main(int argc, char **argv) {
    test_angle_wrapping();
    GcStartup startup = synthetic_startup();
    test_all(&startup);
    assert(gc_boot_initial_phase(GC_BOOT_DRIVE_LID_OPEN, false, false) ==
           GC_BOOT_LID_DELAY);
    assert(gc_boot_initial_phase(GC_BOOT_DRIVE_READY, false, true) ==
           GC_BOOT_LID_DELAY);
    assert(gc_boot_initial_phase(GC_BOOT_DRIVE_ABSENT, false, false) == GC_BOOT_NORMAL);
    assert(gc_boot_initial_phase(GC_BOOT_DRIVE_READY, true, false) ==
           GC_BOOT_SETTINGS_NOTICE);
    assert(!gc_boot_config_init(NULL, &startup));
    if (argc == 2) {
        startup = (GcStartup){0};
        assert(gc_startup_load(argv[1], &startup));
        test_all(&startup);
        gc_startup_destroy(&startup);
    }
    puts("Boot control tests passed");
    return 0;
}
