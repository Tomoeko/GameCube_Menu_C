#include "audio_internal.h"

#include <math.h>
#include <string.h>

/* Native oscillator state machine, USA BS2 sub_81354060. Each native update
 * processes a bounded table of {curve, ticks, signed target} triples. */
void gc_audio_envelope_start(GcAudioEnvelopeState *state,
                             const GcAudioEnvelope *envelope) {
    memset(state, 0, sizeof(*state));
    state->envelope = *envelope;
    if (!envelope->enabled)
        state->current = 1;
}

bool gc_audio_envelope_release(GcAudioEnvelopeState *state) {
    if (!state->envelope.enabled || state->ended || state->quick_pending ||
        state->forced_release)
        return false;
    state->released = true;
    state->release_pending = true;
    return true;
}

bool gc_audio_envelope_quick_release(GcAudioEnvelopeState *state) {
    if (!state->envelope.enabled || state->ended || state->quick_pending ||
        state->forced_release)
        return false;
    state->quick_pending = true;
    return true;
}

static float envelope_curve(float value, unsigned curve) {
    if (curve == 1)
        return copysignf(value * value, value);
    if (curve == 2)
        return copysignf(sqrtf(fabsf(value)), value);
    return value;
}

static float envelope_duration(const GcAudioEnvelope *envelope, unsigned ticks) {
    if (!envelope->scaled_duration)
        return (float)ticks;
    float factor = (float)GC_AUDIO_DSP_RATE / 80.0f;
    factor /= 600.0f;
    return (float)ticks * factor;
}

float gc_audio_envelope_step(GcAudioEnvelopeState *state) {
    const GcAudioEnvelope *envelope = &state->envelope;
    if (!envelope->enabled)
        return 1;
    bool normal_restart = state->release_pending && !state->quick_pending;
    if (state->quick_pending) {
        /* E9 selects a fixed five-unit table and rate one. USA retains its
         * interpolation when attack/release pointers match; EUR resets it.
         * This differs from EUR's normal null-table sixteen-unit release.
         */
        state->quick_pending = false;
        state->quick_release = true;
        state->forced_release = true;
        state->released = true;
        state->release_pending = false;
        state->held = false;
        if (envelope->null_release_quick || !envelope->release_continues) {
            state->pc = 0;
            state->remaining = 0;
            state->target = state->current;
        }
    }
    if (state->release_pending) {
        /* The physical callback marks release before the next oscillator
         * step reads its live descriptor and compares table identities.
         */
        state->release_pending = false;
        state->held = false;
        state->ended = false;
        if (!envelope->release_continues) {
            state->pc = 0;
            state->remaining = 0;
            state->target = state->current;
        }
    }
    if (state->ended)
        return envelope->offset;
    if (state->held)
        return fmaf(state->current, envelope->scale, envelope->offset);
    if (state->released && !state->forced_release && envelope->null_release_quick &&
        (!envelope->release_count || state->quick_release) &&
        (!state->quick_release || normal_restart)) {
        /* A present oscillator with a null release table receives the native
         * sixteen-unit custom release. That duration survives later normal
         * callbacks even if the live table changes; E9 still overrides it.
         */
        state->quick_release = true;
        state->curve = 0;
        state->length = fmaxf(1, envelope_duration(envelope, 16));
        state->remaining = state->length;
        state->target = 0;
        state->step = (state->target - state->current) / state->length;
    }
    if (state->quick_release && !state->forced_release) {
        state->remaining -= envelope->rate;
        if (state->remaining <= 0) {
            state->current = 0;
            state->ended = true;
            return envelope->offset;
        }
        state->current = fmaf(-state->step, state->remaining, state->target);
        return fmaf(state->current, envelope->scale, envelope->offset);
    }
    static const GcAudioEnvelopeStep forced_steps[] = {{0, 5, 0}, {15, 0, 0}};
    const GcAudioEnvelopeStep *steps = state->forced_release ? forced_steps
                                       : state->released     ? envelope->release
                                                             : envelope->attack;
    unsigned count = state->forced_release ? 2
                     : state->released     ? envelope->release_count
                                           : envelope->attack_count;
    if (!count) {
        state->current = 1;
        return 1;
    }
    state->remaining -= state->forced_release ? 1 : envelope->rate;
    for (unsigned command = 0; state->remaining <= 0; ++command) {
        if (state->pc >= count || command >= GC_AUDIO_ENVELOPE_STEPS * 2) {
            state->ended = true;
            return envelope->offset;
        }
        state->current = state->target;
        GcAudioEnvelopeStep step = steps[state->pc++];
        if (step.curve == 13) {
            state->pc = (uint16_t)step.value;
            continue;
        }
        if (step.curve == 14) {
            state->held = true;
            return fmaf(state->current, envelope->scale, envelope->offset);
        }
        if (step.curve == 15) {
            state->ended = true;
            return envelope->offset;
        }
        state->curve = step.curve;
        state->target = (float)step.value / 32768.0f;
        if (!step.ticks)
            continue;
        state->length = envelope_duration(envelope, step.ticks);
        state->remaining = state->length;
        state->step = (state->target - state->current) / state->length;
    }
    /* USA 0x813542f0 FNMSUBS and 0x81354364 FMADDS each round once. */
    state->current = fmaf(-state->step, state->remaining, state->target);
    return fmaf(envelope_curve(state->current, state->curve), envelope->scale,
                envelope->offset);
}

static void apply_voice_envelope(GcAudioVoice *voice, GcAudioEnvelopeState *state) {
    float value = gc_audio_envelope_step(state);
    switch (state->envelope.target) {
        case 0:
            voice->envelope_volume *= value;
            break;
        case 1:
            voice->envelope_pitch *= value;
            break;
        case 2:
            voice->envelope_pan = value;
            break;
        default:
            break;
    }
    if (state->ended)
        voice->active = false;
}

void gc_audio_voice_envelope_install(GcAudioVoice *voice, unsigned slot,
                                     const GcAudioEnvelope *envelope) {
    if (slot >= GC_AUDIO_OSCILLATORS)
        return;
    gc_audio_envelope_start(&voice->envelopes[slot], envelope);
    apply_voice_envelope(voice, &voice->envelopes[slot]);
}

void gc_audio_voice_envelopes(GcAudioVoice *voice) {
    voice->envelope_volume = 1;
    voice->envelope_pitch = 1;
    voice->envelope_pan = 0.5f;
    for (unsigned i = 0; i < GC_AUDIO_OSCILLATORS; ++i) {
        GcAudioEnvelopeState *state = &voice->envelopes[i];
        if (!state->envelope.enabled)
            continue;
        apply_voice_envelope(voice, state);
        if (!voice->active)
            break;
    }
}
