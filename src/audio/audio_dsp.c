#include "audio_internal.h"

#include <math.h>
#include <string.h>

float gc_audio_bus_gain(const GcAudio *audio, float gain) {
    if (!audio || !isfinite(gain))
        return 0;
    /* Native 0x81351880 clamps each routed gain before 0x81351580 truncates
     * master*gain into the signed DSP multiplier's target field.
     */
    gain = fmaxf(0, fminf(1, gain));
    uint16_t multiplier = (uint16_t)((float)audio->master_gain * gain);
    return (float)multiplier / 65536;
}

float gc_audio_pitch_ratio(const GcAudio *audio, float semitones) {
    if (!audio || !isfinite(semitones))
        return 1;
    semitones = fmaxf(-60, fminf(67, semitones));
    float integer = floorf(semitones);
    unsigned fraction = (unsigned)((semitones - integer) * 64);
    unsigned index = (unsigned)(integer + 60);
    return audio->semitone_ratios[index] * audio->fractional_semitone_ratios[fraction];
}

static int16_t history_sample(const GcAudioWave *wave, size_t index, unsigned back) {
    if (index < back)
        return 0;
    size_t sample = index - back;
    if (wave->loop && sample >= wave->loop_end && wave->loop_start < wave->loop_end)
        sample = wave->loop_start +
                 (sample - wave->loop_start) % (wave->loop_end - wave->loop_start);
    return sample < wave->count ? wave->samples[sample] : 0;
}

int16_t gc_audio_resample(const GcAudio *audio, const GcAudioWave *wave,
                          double position) {
    if (!audio || !wave || !wave->samples || !isfinite(position) || position < 0 ||
        position >= (double)SIZE_MAX)
        return 0;
    size_t index = (size_t)position;
    unsigned phase = (unsigned)((position - (double)index) * 64) & 63;
    int64_t accumulator = 0;
    for (unsigned tap = 0; tap < 4; ++tap) {
        /* DSP 0x013b inserts four history samples before the decoded block;
         * 0x0184-0x019c performs the four products at the selected phase.
         * Initial history is zero. Across a loop it retains the previous tail.
         */
        accumulator += (int64_t)history_sample(wave, index, 4 - tap) *
                       audio->resampling_coefficients[phase][tap];
    }
    /* M2 doubles each product. MOVPZ/MULMVZ round the low word to the
     * nearest middle-word integer, with exact halves rounded to even.
     */
    int64_t result = accumulator / 32768;
    int64_t remainder = accumulator % 32768;
    if (remainder < 0)
        remainder = -remainder;
    if (remainder > 16384 || (remainder == 16384 && result % 2))
        result += accumulator < 0 ? -1 : 1;
    return (int16_t)(result > INT16_MAX   ? INT16_MAX
                     : result < INT16_MIN ? INT16_MIN
                                          : result);
}

int16_t gc_audio_resample_pitch(const GcAudio *audio, const GcAudioWave *wave,
                                double position, double pitch) {
    if (!audio || !wave || !wave->samples || !isfinite(position) || position < 0 ||
        position >= (double)SIZE_MAX || !isfinite(pitch))
        return 0;
    /* DSP 0x0157 branches to 0x01b0 when the integer pitch is at least four.
     * That path reads the source pointer directly, retaining the same four
     * prepended history samples, rather than applying the interpolation FIR.
     */
    return pitch >= 4 ? history_sample(wave, (size_t)position, 4)
                      : gc_audio_resample(audio, wave, position);
}

/* DSP middle-word reads in SET16 wrap; SET40 reads saturate. Express the
 * arithmetic shifts explicitly so negative samples behave identically on
 * every host. No host float enters the native PCM or delay-bus arithmetic.
 */
int64_t gc_audio_dsp_shift(int64_t value, unsigned bits) {
    int64_t divisor = INT64_C(1) << bits;
    return value >= 0 ? value / divisor : -((-value + divisor - 1) / divisor);
}

int64_t gc_audio_dsp_round(int64_t value, unsigned bits) {
    int64_t divisor = INT64_C(1) << bits;
    int64_t middle = value / divisor;
    int64_t remainder = value % divisor;
    if (remainder < 0)
        remainder = -remainder;
    if (remainder > divisor / 2 || (remainder == divisor / 2 && middle % 2))
        middle += value < 0 ? -1 : 1;
    return middle;
}

int16_t gc_audio_dsp_saturate(int64_t sample) {
    return (int16_t)(sample > INT16_MAX   ? INT16_MAX
                     : sample < INT16_MIN ? INT16_MIN
                                          : sample);
}

int16_t gc_audio_dsp_wrap(int64_t sample) {
    uint16_t low = (uint16_t)((uint64_t)sample & UINT16_MAX);
    return low <= INT16_MAX ? (int16_t)low : (int16_t)((int32_t)low - 65536);
}

void gc_audio_dsp_gain_prepare(GcAudioDspGain *gain, int16_t target, bool revised) {
    if (!gain->initialized) {
        /* USA 0x813527e0 and EUR 0x81350980 initialize both fields directly. */
        gain->current = target;
        gain->initialized = true;
    }
    gain->accumulator = (int64_t)gain->current * 65536;
    gain->product_multiplier = gain->current;
    gain->frame = 0;
    int64_t difference = (int64_t)(target - gain->current) * 65536;
    if (revised) {
        /* EUR DSP 0x05df and 0x011e ramp 32 pairs then hold eight pairs. */
        gain->increment = (int32_t)gc_audio_dsp_shift(difference, 5);
    } else {
        /* USA 0x81352820 divides toward zero, then arithmetic-shifts by ten.
         * DSP 0x0519 reconstructs that Q6 delta, advancing it every pair.
         */
        int64_t delta = gc_audio_dsp_shift(difference / 40, 10);
        delta = delta > 32767 ? 32767 : delta < -32767 ? -32767 : delta;
        if (delta == 1 || delta == -1)
            delta *= 2;
        gain->increment = (int32_t)(delta * 1024);
    }
}

int16_t gc_audio_dsp_gain_mix(GcAudioDspGain *gain, int16_t sample, int16_t bus,
                              bool revised) {
    /* M0 MULCAC retains the fractional product; SRRI reads the saturated
     * middle word of existing_bus<<16 + sample*gain. It does not round.
     */
    int16_t mixed = gc_audio_dsp_saturate(gc_audio_dsp_shift(
        (int64_t)bus * 65536 + (int64_t)sample * gain->product_multiplier, 16));
    /* MULCAC computes the next product before ADDAX advances the pair's
     * gain. The first sample of the next pair still uses the previous gain.
     */
    gain->product_multiplier =
        gc_audio_dsp_saturate(gc_audio_dsp_shift(gain->accumulator, 16));
    if ((gain->frame & 1) && (!revised || gain->frame < 64))
        gain->accumulator += gain->increment;
    if (++gain->frame == 80) {
        gain->current =
            gc_audio_dsp_saturate(gc_audio_dsp_shift(gain->accumulator, 16));
        gain->increment = 0;
    }
    return mixed;
}

static void prepare_voice_gains(GcAudio *audio, GcAudioVoice *voice, bool mono) {
    float gain = voice->gain * voice->envelope_volume;
    for (unsigned route = 0; route < 6; ++route) {
        float scale = mono ? gc_audio_route_scale(audio, voice, route, true)
                           : voice->bus_scales[route];
        int16_t target = (int16_t)(gc_audio_bus_gain(audio, gain * scale) * 65536);
        gc_audio_dsp_gain_prepare(&voice->dsp_gains[route], target,
                                  audio->sequence_revision != 0);
    }
}

static void render_voice(GcAudio *audio, GcAudioVoice *voice, bool mono,
                         int16_t buses[12]) {
    GcAudioWave *wave = &audio->waves[voice->wave];
    size_t end = wave->loop ? wave->loop_end : wave->count;
    if (!wave->loop && voice->position >= (double)end) {
        voice->active = false;
        return;
    }
    if (!voice->dsp_gains[0].initialized)
        prepare_voice_gains(audio, voice, mono);
    double pitch = (float)((float)voice->step * voice->envelope_pitch);
    pitch = floor(fmax(0, fmin(65535, pitch * GC_AUDIO_DSP_PITCH_SCALE))) /
            GC_AUDIO_DSP_PITCH_SCALE;
    int16_t sample = gc_audio_resample_pitch(audio, wave, voice->position, pitch);
    for (unsigned route = 0; route < 6; ++route) {
        unsigned bus = voice->buses[route];
        if (bus)
            buses[bus] =
                gc_audio_dsp_gain_mix(&voice->dsp_gains[route], sample, buses[bus],
                                      audio->sequence_revision != 0);
    }
    /* Native 12-bit fractional pitch and DAC clock are retained before
     * the separate host-rate conversion.
     */
    voice->position += pitch;
}

static int16_t effect_mix(int16_t bus, int16_t sample, int16_t gain) {
    /* M2 MULMVZ rounds its product, then ADDR adds an integer bus. This
     * helper is called in SET16 at USA 0x0342 / EUR 0x038f.
     */
    return gc_audio_dsp_wrap((int64_t)bus +
                             gc_audio_dsp_round((int64_t)sample * gain, 15));
}

void gc_audio_effects_begin(GcAudio *audio, int16_t buses[12]) {
    /* USA 0x035e / EUR 0x03ab begin with the PREVIOUS 80-sample surround
     * block. The right copy uses NOT, so even silent input yields -1.
     */
    int16_t surround = audio->surround_delay[audio->dsp_frame];
    buses[1] = surround;
    buses[2] = gc_audio_dsp_wrap(-1 - (int64_t)surround);
    for (unsigned index = 0; index < 4; ++index) {
        GcAudioEffect *effect = &audio->effects[index];
        if (!effect->length)
            continue;
        int16_t delayed = effect->delay[effect->position];
        int64_t product = 0;
        for (unsigned tap = 0; tap < 8; ++tap)
            product += (int64_t)effect->history[tap] * effect->filter[tap];
        /* The DMA block has eight preceding samples. The in-place FIR
         * (USA 0x01bc / EUR 0x01c2) reads those before the current sample,
         * rounds MOVPZ, and stores its middle word in SET16.
         */
        int16_t filtered = gc_audio_dsp_wrap(gc_audio_dsp_round(product, 15));
        memmove(effect->history, effect->history + 1, 7 * sizeof(effect->history[0]));
        effect->history[7] = delayed;
        int16_t returned = effect->mode == 1 ? filtered : delayed;
        for (unsigned channel = 0; channel < 2; ++channel) {
            unsigned bus = effect->return_bus[channel];
            if (bus)
                buses[bus] =
                    effect_mix(buses[bus], returned, effect->return_gain[channel]);
        }
        /* Voices subsequently saturate their additions into this filtered
         * buffer; 0x0402 / 0x044f writes that complete buffer back to RAM.
         */
        buses[3 + index] = filtered;
    }
}

void gc_audio_effects_end(GcAudio *audio, int16_t buses[12]) {
    for (unsigned index = 0; index < 4; ++index) {
        GcAudioEffect *effect = &audio->effects[index];
        if (!effect->length)
            continue;
        effect->delay[effect->position] = buses[3 + index];
        effect->position = (effect->position + 1) % effect->length;
    }
    audio->surround_delay[audio->dsp_frame] = buses[8];
    if (++audio->dsp_frame == 80)
        audio->dsp_frame = 0;
    if (audio->sequence_revision) {
        if (!audio->chorus_frame) {
            unsigned distance = 160 - (audio->chorus_read >> 16);
            if (distance >= 80)
                distance -= 80;
            if (audio->chorus_direction < 0 && distance >= 48)
                audio->chorus_direction = 1;
            else if (audio->chorus_direction > 0 && distance <= 16)
                audio->chorus_direction = -1;
        }
        unsigned write = audio->chorus_write + audio->chorus_frame;
        audio->chorus[write] = buses[10];
        int16_t chorus = audio->chorus[audio->chorus_read >> 16];
        /* EUR 0x0b1b advances its unsigned Q16 position by 1 +/- 1/128;
         * sample selection uses the integer pointer, with no interpolation.
         */
        uint32_t step = audio->chorus_direction < 0 ? 65536u - 512u : 65536u + 512u;
        audio->chorus_read = (audio->chorus_read + step) % (160u * 65536u);
        if (++audio->chorus_frame == 80) {
            audio->chorus_frame = 0;
            audio->chorus_write = audio->chorus_write ? 0 : 80;
        }
        buses[1] = effect_mix(buses[1], chorus, 0x3fff);
        buses[2] = effect_mix(buses[2], chorus, 0x3fff);
        buses[1] = effect_mix(buses[1], buses[11], 0x5a82);
        buses[2] = effect_mix(buses[2], buses[11], 0x5a82);
    }
    /* Native right-only bus subtraction follows the left output write. */
    buses[2] = effect_mix(buses[2], buses[9], INT16_MIN);
}

int16_t gc_audio_dsp_output(const GcAudio *audio, int16_t sample) {
    /* M0 product, ASRNRX with +4, SET40 middle-word store: Q12 floor. */
    return gc_audio_dsp_saturate(
        gc_audio_dsp_shift((int64_t)sample * audio->output_gain, 12));
}

void gc_audio_dsp_render_frame(GcAudio *audio, bool mono, float output[2]) {
    /* sub_8134fa20 schedules seven DSP updates per 560-sample buffer at
     * the native DAC rate. USA/JAP sub_813571a0 uses tempo*timebase/25200
     * per update; EUR sub_813554c0 derives it from the actual DAC rate.
     * sub_8134fc20 runs the sequence callback before channel oscillators.
     * Notes and parameter changes occur at those 80-sample boundaries.
     */
    if (++audio->update_samples == (unsigned)GC_AUDIO_DSP_QUANTUM) {
        audio->update_samples = 0;
        float tick_step = (float)(audio->tempo * audio->timebase);
        tick_step = audio->sequence_revision ? tick_step * (float)GC_AUDIO_DSP_QUANTUM /
                                                   (60.0f * (float)GC_AUDIO_DSP_RATE)
                                             : tick_step / 25200.0f;
        audio->tick_fraction = (float)((float)audio->tick_fraction + tick_step);
        while (audio->tick_fraction >= 1) {
            audio->tick_fraction = (float)((float)audio->tick_fraction - 1.0f);
            gc_audio_sequence_tick(audio);
        }
        for (unsigned i = 0; i < GC_AUDIO_VOICES; ++i) {
            GcAudioVoice *voice = &audio->voices[i];
            if (voice->active && !voice->released && voice->duration != UINT32_MAX &&
                (!voice->duration || !--voice->duration))
                gc_audio_voice_release(voice);
            if (voice->active) {
                gc_audio_sequence_envelopes(audio, voice);
                prepare_voice_gains(audio, voice, mono);
            }
        }
    }
    int16_t buses[12] = {0};
    gc_audio_effects_begin(audio, buses);
    for (unsigned i = 0; i < GC_AUDIO_VOICES; ++i) {
        if (audio->voices[i].active)
            render_voice(audio, &audio->voices[i], mono, buses);
    }
    gc_audio_effects_end(audio, buses);
    output[0] = (float)gc_audio_dsp_output(audio, buses[1]) / 32768;
    output[1] = (float)gc_audio_dsp_output(audio, buses[2]) / 32768;
}
