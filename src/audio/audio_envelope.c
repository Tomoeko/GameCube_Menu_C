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

void gc_audio_envelope_release(GcAudioEnvelopeState *state) {
    if (state->released)
        return;
    state->released = true;
    state->pc = 0;
    state->remaining = 0;
    state->target = state->current;
    state->held = false;
    state->ended = false;
}

static float envelope_curve(float value, unsigned curve) {
    if (curve == 1)
        return copysignf(value * value, value);
    if (curve == 2)
        return copysignf(sqrtf(fabsf(value)), value);
    return value;
}

float gc_audio_envelope_step(GcAudioEnvelopeState *state) {
    const GcAudioEnvelope *envelope = &state->envelope;
    if (!envelope->enabled)
        return 1;
    if (state->ended)
        return envelope->offset;
    if (state->held)
        return fmaf(state->current, envelope->scale, envelope->offset);
    const GcAudioEnvelopeStep *steps =
        state->released ? envelope->release : envelope->attack;
    unsigned count = state->released ? envelope->release_count : envelope->attack_count;
    if (!count) {
        if (state->released) {
            state->ended = true;
            return envelope->offset;
        }
        state->current = 1;
        return envelope->scale + envelope->offset;
    }
    state->remaining -= envelope->rate;
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
        state->length = step.ticks;
        state->remaining = step.ticks;
        state->step = (state->target - state->current) / state->length;
    }
    /* USA 0x813542f0 FNMSUBS and 0x81354364 FMADDS each round once. */
    state->current = fmaf(-state->step, state->remaining, state->target);
    return fmaf(envelope_curve(state->current, state->curve), envelope->scale,
                envelope->offset);
}

void gc_audio_voice_envelopes(GcAudioVoice *voice) {
    voice->envelope_volume = 1;
    voice->envelope_pitch = 1;
    voice->envelope_pan = 0.5f;
    for (unsigned i = 0; i < 2; ++i) {
        GcAudioEnvelopeState *state = &voice->envelopes[i];
        if (!state->envelope.enabled)
            continue;
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
}
