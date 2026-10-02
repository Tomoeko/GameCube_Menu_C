#include "audio_internal.h"

#include <math.h>
#include <string.h>

/* Recovered from USA BS2 sub_81355340 and its operand helpers. Offsets are
 * relative to iplrom.com. The interpreter is bounded at each byte, stack,
 * register, child track, table lookup, and instruction dispatch.
 */
#define NO_TRACK GC_AUDIO_TRACKS

static uint32_t read_bytes(GcAudio *audio, GcAudioTrack *track, unsigned count) {
    if (track->pc > audio->sequence_size || count > audio->sequence_size - track->pc) {
        track->active = false;
        ++audio->rejected_commands;
        return 0;
    }
    uint32_t result = 0;
    for (unsigned i = 0; i < count; ++i)
        result = result << 8 | audio->sequence[track->pc++];
    return result;
}

static uint16_t get_register(const GcAudioTrack *track, unsigned reg) {
    if (reg == 0x20)
        return track->registers[6] >> 8;
    if (reg == 0x21)
        return track->registers[6] & 255;
    if (reg == 0x22)
        return (uint16_t)(track->registers[0] << 8 | track->registers[1]);
    if (reg < GC_AUDIO_REGISTERS)
        return reg < 3 ? track->registers[reg] & 255 : track->registers[reg];
    return 0;
}

static uint32_t get_wide_register(const GcAudioTrack *track, unsigned reg) {
    if (reg >= 0x28 && reg < 0x2c) {
        unsigned index = 16 + (reg - 0x28) * 2;
        return (uint32_t)track->registers[index] << 16 | track->registers[index + 1];
    }
    return get_register(track, reg);
}

static void set_register(GcAudioTrack *track, unsigned reg, uint32_t value) {
    if (reg == 0x20) {
        track->registers[6] = (uint16_t)(value << 8 | (track->registers[6] & 255));
    } else if (reg == 0x21) {
        track->registers[6] =
            (uint16_t)((track->registers[6] & 0xff00) | (value & 255));
    } else if (reg == 0x22) {
        track->registers[0] = (uint16_t)(value >> 8 & 255);
        track->registers[1] = (uint16_t)(value & 255);
    } else if (reg >= 0x28 && reg < 0x2c) {
        unsigned index = 16 + (reg - 0x28) * 2;
        track->registers[index] = (uint16_t)(value >> 16);
        track->registers[index + 1] = (uint16_t)value;
    } else if (reg < GC_AUDIO_REGISTERS) {
        track->registers[reg] = (uint16_t)(reg < 3 ? value & 255 : value);
    }
    track->registers[3] = (uint16_t)value;
}

static unsigned note_operand(const GcAudioTrack *track, unsigned operand) {
    if (operand >= 0xc0 && operand < 0xd0)
        return track->ports[operand - 0xc0];
    return operand >= 0x80 ? get_register(track, operand - 0x80) : operand;
}

static bool condition(const GcAudioTrack *track, unsigned mode) {
    uint16_t value = track->registers[3];
    switch (mode & 15) {
        case 0:
            return true;
        case 1:
            return value == 0;
        case 2:
            return value != 0;
        case 3:
            return value == 1;
        case 4:
            return value >= 0x8000;
        case 5:
            return value < 0x8000;
        default:
            return false;
    }
}

void gc_audio_voice_release(GcAudioVoice *voice) {
    if (!voice->released) {
        voice->released = true;
        for (unsigned i = 0; i < 2; ++i)
            gc_audio_envelope_release(&voice->envelopes[i]);
    }
}

static void stop_track(GcAudio *audio, unsigned index) {
    if (index >= GC_AUDIO_TRACKS)
        return;
    GcAudioTrack *track = &audio->tracks[index];
    track->active = false;
    for (unsigned slot = 0; slot < 16; ++slot) {
        unsigned child = track->children[slot];
        track->children[slot] = NO_TRACK;
        if (child < GC_AUDIO_TRACKS && audio->tracks[child].parent == index)
            stop_track(audio, child);
    }
    /* sub_81355340 clears a finished child's owning slot after native
     * sub_81357ce0 tears it down. Reused storage must have exactly one
     * parent; a stale slot otherwise advances the new track twice per tick.
     */
    if (track->parent < GC_AUDIO_TRACKS) {
        GcAudioTrack *parent = &audio->tracks[track->parent];
        for (unsigned slot = 0; slot < 16; ++slot) {
            if (parent->children[slot] == index)
                parent->children[slot] = NO_TRACK;
        }
    }
    for (unsigned i = 0; i < GC_AUDIO_VOICES; ++i) {
        if (audio->voices[i].active && audio->voices[i].track == index)
            gc_audio_voice_release(&audio->voices[i]);
    }
}

static void initialize_track(GcAudio *audio, unsigned index, unsigned parent,
                             unsigned flags, size_t pc) {
    GcAudioTrack *track = &audio->tracks[index];
    memset(track, 0, sizeof(*track));
    track->active = pc < audio->sequence_size;
    track->pc = pc;
    track->parent = parent;
    track->flags = flags;
    for (unsigned i = 0; i < 16; ++i)
        track->children[i] = NO_TRACK;
    track->parameters[0] = 1;
    track->parameters[3] = 0.5f;
    track->registers[7] = 12;
    track->registers[9] = 1;
    track->registers[11] = 0x7fff;
    track->registers[13] = 0x4000;
    track->timebase = parent < GC_AUDIO_TRACKS ? audio->tracks[parent].timebase : 48;
    track->time_mode = parent < GC_AUDIO_TRACKS ? audio->tracks[parent].time_mode : 1;
    if (audio->sequence_revision) {
        track->routes[0] = 0x150;
        track->routes[1] = 0x210;
        track->routes[2] = 0x352;
        track->routes[3] = 0x412;
    } else {
        track->routes[0] = 0x14;
        track->routes[1] = 0x24;
        track->routes[2] = 0x36;
        track->routes[3] = 0x46;
    }
    if (parent < GC_AUDIO_TRACKS) {
        memcpy(track->routes, audio->tracks[parent].routes, sizeof(track->routes));
        if (!(flags & 2)) {
            track->registers[6] = audio->tracks[parent].registers[6];
            track->registers[7] = audio->tracks[parent].registers[7];
            memcpy(track->registers + 8, audio->tracks[parent].registers + 8,
                   5 * sizeof(track->registers[0]));
        }
    }
}

static void open_track(GcAudio *audio, unsigned parent, unsigned selector, size_t pc) {
    GcAudioTrack *track = &audio->tracks[parent];
    unsigned slot = selector & 15;
    if (selector & 0x20)
        slot = get_register(track, slot);
    if (slot >= 16 || pc >= audio->sequence_size) {
        ++audio->rejected_commands;
        return;
    }
    unsigned child = track->children[slot];
    if (child < GC_AUDIO_TRACKS)
        stop_track(audio, child);
    else {
        child = 1;
        while (child < GC_AUDIO_TRACKS && audio->tracks[child].active)
            ++child;
    }
    if (child == GC_AUDIO_TRACKS) {
        ++audio->rejected_commands;
        return;
    }
    initialize_track(audio, child, parent, selector >> 6, pc);
    track->children[slot] = child;
}

static void port_write(GcAudioTrack *track, unsigned port, uint16_t value,
                       bool imported) {
    if (!track || !track->active || port >= 16)
        return;
    track->ports[port] = value;
    if (imported) {
        track->port_imported |= 1u << port;
        if (port < 2)
            track->interrupt_pending |= 1u << (port + 3);
    } else {
        track->port_exported |= 1u << port;
    }
}

static GcAudioTrack *find_track(GcAudio *audio, uint32_t id) {
    for (unsigned i = 0; i < GC_AUDIO_TRACKS; ++i) {
        if (audio->tracks[i].active && audio->tracks[i].id == id)
            return &audio->tracks[i];
    }
    return NULL;
}

void gc_audio_sequence_event(GcAudio *audio, unsigned event) {
    uint32_t id;
    unsigned value;
    if (event < 3) {
        id = 0x21002;
        value = event;
        audio->native_counter = 0;
    } else if (event == 0x17) {
        audio->native_counter = (audio->native_counter + 1) % 21;
        unsigned direction = audio->native_counter <= 10 ? 0 : 1;
        unsigned numerator =
            direction ? 21 - audio->native_counter : audio->native_counter;
        gc_audio_sequence_cube(audio, direction, (float)numerator / 10.0f);
        return;
    } else if (event >= 0x17) {
        id = 0x20001;
        value = event - 5;
    } else {
        id = 0x20002;
        value = event;
    }
    port_write(find_track(audio, id), 0, (uint16_t)value, true);
}

void gc_audio_sequence_cube(GcAudio *audio, unsigned direction, float fraction) {
    /* sub_81359340, constants at 0x8145e500 (r2=0x81465cc0). */
    float root = sqrtf(fraction);
    unsigned volume = direction ? 0 : (unsigned)(32767.0f * (1.0f - root));
    port_write(find_track(audio, 0x21002), 1, (uint16_t)volume, true);
    if (root == 0)
        return;
    GcAudioTrack *track = find_track(audio, 0x21003);
    float pitch_low;
    float pitch_high;
    if (direction) {
        float projection = 0.15f + root;
        pitch_low = 2048.0f * 3.0f * projection;
        pitch_high = 2048.0f * 3.6f * projection;
    } else {
        float projection = 0.1f + root;
        pitch_low = 2048.0f * 1.6f * projection;
        pitch_high = 2048.0f * projection;
    }
    port_write(track, 3, (uint16_t)pitch_low, true);
    port_write(track, 2, (uint16_t)pitch_high, true);
    if (root > 0.6f) {
        float gain = fminf(100, 10.0f / root);
        port_write(track, 0, (uint16_t)gain, true);
    }
}

static float inherited_parameter_depth(const GcAudio *audio, unsigned index,
                                       unsigned parameter, unsigned depth) {
    const GcAudioTrack *track = &audio->tracks[index];
    float result = track->parameters[parameter];
    if (!(track->flags & 1) && track->parent < GC_AUDIO_TRACKS &&
        depth < GC_AUDIO_TRACKS) {
        float parent =
            inherited_parameter_depth(audio, track->parent, parameter, depth + 1);
        if (parameter == 0)
            result *= parent;
        else if (parameter == 1)
            result += parent;
        else if (parameter == 2 || parameter == 3) {
            /* sub_81356f00 loads the weight at track + 0x22c, register 12.
             * Its native default is zero, preserving the child's panning.
             */
            float weight = (float)track->registers[12] / 32767.0f;
            result = result * (1.0f - weight) + parent * weight;
        }
    }
    return result;
}

static float inherited_parameter(const GcAudio *audio, unsigned index,
                                 unsigned parameter) {
    return inherited_parameter_depth(audio, index, parameter, 0);
}

static float inherited_pitch(const GcAudio *audio, unsigned index) {
    float result = 1;
    for (unsigned depth = 0; depth < GC_AUDIO_TRACKS; ++depth) {
        const GcAudioTrack *track = &audio->tracks[index];
        /* sub_81357e60 uses 32768 / 8192 before the semitone lookup. */
        float semitones = 4.0f * track->parameters[1] * track->registers[7];
        result *= gc_audio_pitch_ratio(audio, semitones);
        if (track->flags & 1 || track->parent >= GC_AUDIO_TRACKS)
            break;
        index = track->parent;
    }
    return result;
}

static void update_voice_pan(GcAudio *audio, GcAudioVoice *voice) {
    const GcAudioTrack *track = &audio->tracks[voice->track];
    float total =
        (float)track->registers[8] + track->registers[9] + track->registers[10];
    float pan = 0.5f;
    float reverb = 0;
    if (total > 0) {
        /* sub_81358540 normalizes instrument, oscillator, and track weights.
         * The bank's instrument and volume oscillators start at center.
         */
        pan = (0.5f * track->registers[8] + voice->envelope_pan * track->registers[9] +
               inherited_parameter(audio, voice->track, 3) * track->registers[10]) /
              total;
        reverb =
            inherited_parameter(audio, voice->track, 2) * track->registers[10] / total;
    }
    voice->pan = fmaxf(0, fminf(1, pan));
    voice->reverb = fmaxf(0, fminf(1, reverb));
    for (unsigned route = 0; route < 6; ++route) {
        unsigned bus = audio->sequence_revision ? track->routes[route] >> 8
                                                : track->routes[route] >> 4;
        voice->buses[route] = bus < 12 ? bus : 0;
        voice->bus_scales[route] = gc_audio_route_scale(audio, voice, route, false);
    }
    /* Native main buses use sin(pi/2 * (1-pan)) and sin(pi/2 * pan). */
    voice->left_scale = sinf(1.5707963267948966f * (1.0f - voice->pan));
    voice->right_scale = sinf(1.5707963267948966f * voice->pan);
}

static float route_component(unsigned selector, unsigned route, float pan, float reverb,
                             bool revised) {
    if (!selector)
        return 1;
    float value = 0;
    if (revised) {
        value = selector == 1   ? pan
                : selector == 2 ? reverb
                : selector == 5 ? 1 - pan
                : selector == 6 ? 1 - reverb
                : selector == 7 ? 1
                                : 0;
    } else if (selector == 1) {
        value = route & 1 ? pan : 1 - pan;
    } else if (selector == 2) {
        value = route < 2 ? 1 - reverb : reverb;
    } else if (selector == 3) {
        value = route < 2 ? 1 : 0;
    }
    return sinf(1.5707963267948966f * value);
}

float gc_audio_route_scale(const GcAudio *audio, const GcAudioVoice *voice,
                           unsigned route, bool mono) {
    if (route >= 6 || voice->track >= GC_AUDIO_TRACKS || !voice->buses[route])
        return 0;
    unsigned flags = audio->tracks[voice->track].routes[route];
    unsigned first = audio->sequence_revision ? flags >> 4 & 15 : flags >> 2 & 3;
    unsigned second = audio->sequence_revision ? flags & 15 : flags & 3;
    float pan = mono ? 0.5f : voice->pan;
    return route_component(first, route, pan, voice->reverb,
                           audio->sequence_revision != 0) *
           route_component(second, route, pan, voice->reverb,
                           audio->sequence_revision != 0);
}

void gc_audio_sequence_envelopes(GcAudio *audio, GcAudioVoice *voice) {
    const GcAudioTrack *track = &audio->tracks[voice->track];
    for (unsigned i = 0; i < 2; ++i) {
        if (track->envelopes[i].enabled) {
            GcAudioEnvelope *envelope = &voice->envelopes[i].envelope;
            envelope->rate = track->envelopes[i].rate;
            envelope->scale = track->envelopes[i].scale;
            envelope->offset = track->envelopes[i].offset;
        }
    }
    gc_audio_voice_envelopes(voice);
    update_voice_pan(audio, voice);
}

static unsigned wave_index(const GcAudio *audio, unsigned id) {
    for (unsigned i = 0; i < audio->wave_count; ++i) {
        if (audio->waves[i].id == id)
            return i;
    }
    return GC_AUDIO_WAVES;
}

static void start_note(GcAudio *audio, unsigned index, unsigned key, unsigned slot,
                       unsigned velocity, unsigned duration) {
    GcAudioTrack *track = &audio->tracks[index];
    unsigned program = track->registers[6] & 255;
    if ((track->registers[6] >> 8) != 0 || program >= 128) {
        ++audio->rejected_commands;
        return;
    }
    GcAudioInstrument *instrument = &audio->instruments[program];
    const GcAudioRegion *region = NULL;
    for (unsigned i = 0; i < instrument->region_count; ++i) {
        if (key <= instrument->regions[i].key &&
            velocity <= instrument->regions[i].velocity) {
            region = &instrument->regions[i];
            break;
        }
    }
    if (!region) {
        ++audio->rejected_commands;
        return;
    }
    unsigned wave = wave_index(audio, region->wave);
    if (wave == GC_AUDIO_WAVES) {
        ++audio->rejected_commands;
        return;
    }
    unsigned voice_index = GC_AUDIO_VOICES;
    for (unsigned i = 0; i < GC_AUDIO_VOICES; ++i) {
        if (audio->voices[i].active && audio->voices[i].track == index &&
            audio->voices[i].slot == slot) {
            gc_audio_voice_release(&audio->voices[i]);
        }
        if (!audio->voices[i].active && voice_index == GC_AUDIO_VOICES)
            voice_index = i;
    }
    if (voice_index == GC_AUDIO_VOICES) {
        ++audio->rejected_commands;
        return;
    }
    GcAudioVoice *voice = &audio->voices[voice_index];
    memset(voice, 0, sizeof(*voice));
    voice->active = true;
    voice->track = index;
    voice->slot = slot;
    voice->wave = wave;
    float semitones = (float)((int)key - (int)audio->waves[wave].key);
    float wave_pitch = (float)((double)instrument->pitch * audio->waves[wave].rate /
                               GC_AUDIO_DSP_RATE * region->pitch);
    voice->base_step = wave_pitch * gc_audio_pitch_ratio(audio, semitones);
    voice->step = (float)voice->base_step * inherited_pitch(audio, index);
    voice->base_gain = instrument->volume * region->volume * (float)velocity / 127.0f;
    voice->gain = voice->base_gain * inherited_parameter(audio, index, 0);
    update_voice_pan(audio, voice);
    voice->duration = duration;
    for (unsigned i = 0; i < 2; ++i) {
        const GcAudioEnvelope *envelope = track->envelopes[i].enabled
                                              ? &track->envelopes[i]
                                              : &instrument->envelopes[i];
        gc_audio_envelope_start(&voice->envelopes[i], envelope);
    }
    gc_audio_voice_envelopes(voice);
    update_voice_pan(audio, voice);
    ++audio->notes_started;
}

static void note_off(GcAudio *audio, unsigned index, unsigned slot) {
    for (unsigned i = 0; i < GC_AUDIO_VOICES; ++i) {
        if (audio->voices[i].active && audio->voices[i].track == index &&
            audio->voices[i].slot == slot)
            gc_audio_voice_release(&audio->voices[i]);
    }
}

static void note_command(GcAudio *audio, unsigned index, unsigned note) {
    GcAudioTrack *track = &audio->tracks[index];
    unsigned flags = read_bytes(audio, track, 1);
    unsigned velocity = note_operand(track, read_bytes(audio, track, 1));
    unsigned slot = flags & 7;
    unsigned duration = UINT32_MAX;
    unsigned gate = 100;
    if (!slot) {
        gate = note_operand(track, read_bytes(audio, track, 1));
        unsigned count = flags >> 3 & 3;
        duration = read_bytes(audio, track, count);
        if (count == 1)
            duration = note_operand(track, duration);
    } else if (flags & 0x18) {
        slot = get_register(track, slot - 1);
    }
    int key = (int)(flags & 0x80 ? note_operand(track, note + 0x80) : note);
    for (unsigned parent = index, depth = 0; depth < GC_AUDIO_TRACKS; ++depth) {
        key += audio->tracks[parent].transpose;
        parent = audio->tracks[parent].parent;
        if (parent >= GC_AUDIO_TRACKS)
            break;
    }
    if (flags & 0x40)
        key = track->previous_key;
    key &= 255;
    track->previous_key = key;
    if (!track->active || velocity > 127 || slot > 7)
        return;
    unsigned note_duration = UINT32_MAX;
    if (duration != UINT32_MAX && !(flags & 0x20)) {
        /* sub_813552a0 converts gates into native DSP updates. Mode zero uses
         * 120 * 7 / 10; mode one divides by the sequence's tempo fraction.
         */
        double length = (double)duration * gate / 100.0;
        double fraction = audio->sequence_revision
                              ? (double)audio->tempo * audio->timebase *
                                    GC_AUDIO_DSP_QUANTUM / (60.0 * GC_AUDIO_DSP_RATE)
                              : (double)audio->tempo * audio->timebase / 25200.0;
        length =
            track->time_mode == 1
                ? length / fraction
                : length * (audio->sequence_revision ? 120.0 : 84.0) / track->timebase;
        note_duration = length >= UINT32_MAX ? UINT32_MAX : (unsigned)length;
    }
    start_note(audio, index, (unsigned)key, slot, velocity, note_duration);
    if (duration != UINT32_MAX)
        track->wait = duration;
}

static int32_t immediate_operand(GcAudio *audio, GcAudioTrack *track, unsigned type) {
    switch (type & 12) {
        case 0:
            return get_register(track, read_bytes(audio, track, 1));
        case 4:
            return (int32_t)read_bytes(audio, track, 1);
        case 8:
            return (int32_t)(int8_t)read_bytes(audio, track, 1) * 256;
        default:
            return (int16_t)read_bytes(audio, track, 2);
    }
}

static void oscillator_parameter(GcAudioTrack *track, unsigned parameter, float value) {
    if (parameter < 6 || parameter > 11)
        return;
    unsigned index = (parameter - 6) / 3;
    switch ((parameter - 6) % 3) {
        case 0:
            track->envelopes[index].scale = value;
            break;
        case 1:
            track->envelopes[index].rate = fmaxf(0, value);
            break;
        case 2:
            track->envelopes[index].offset = value;
            break;
    }
}

static void parameter_command(GcAudio *audio, GcAudioTrack *track, unsigned opcode) {
    unsigned parameter = read_bytes(audio, track, 1);
    int32_t value = immediate_operand(audio, track, opcode);
    unsigned duration;
    switch (opcode & 3) {
        case 0:
            /* sub_81356180 uses -1 to retain the current interpolation
             * countdown. Cube motion writes its upper pitch with 0x90,
             * then its lower target and duration with 0x91; snapping the
             * first command changes every subsequent glissando.
             */
            duration = parameter < 17 ? track->parameter_ticks[parameter] : 0;
            break;
        case 1:
            duration = get_register(track, read_bytes(audio, track, 1));
            break;
        case 2:
            duration = read_bytes(audio, track, 1);
            break;
        default:
            duration = read_bytes(audio, track, 2);
            break;
    }
    if (parameter >= 17) {
        ++audio->rejected_commands;
        return;
    }
    float target = (float)value / 32768.0f;
    track->parameter_targets[parameter] = target;
    track->parameter_ticks[parameter] = duration;
    if (duration)
        track->parameter_steps[parameter] =
            (target - track->parameters[parameter]) / (float)duration;
    else {
        track->parameters[parameter] = target;
        oscillator_parameter(track, parameter, target);
    }
}

static void oscillator_cycle(GcAudioTrack *track, unsigned mode) {
    if (mode > 1)
        return;
    GcAudioEnvelope *envelope = &track->envelopes[1];
    memset(envelope, 0, sizeof(*envelope));
    envelope->enabled = true;
    envelope->target = mode ? 0 : 1;
    envelope->rate = 1;
    envelope->scale = 1;
    envelope->offset = 1;
    envelope->attack_count = 6;
    envelope->attack[0] = (GcAudioEnvelopeStep){0, 0, mode ? 32767 : 0};
    envelope->attack[1] = (GcAudioEnvelopeStep){0, 20, mode ? 0 : 32767};
    envelope->attack[2] = (GcAudioEnvelopeStep){0, 20, mode ? -32767 : 0};
    envelope->attack[3] = (GcAudioEnvelopeStep){0, 20, mode ? 0 : -16384};
    envelope->attack[4] = (GcAudioEnvelopeStep){0, 20, mode ? 32767 : 0};
    envelope->attack[5] = (GcAudioEnvelopeStep){13, 0, 1};
    envelope->release_count = envelope->attack_count;
    memcpy(envelope->release, envelope->attack,
           envelope->attack_count * sizeof(envelope->attack[0]));
}

static void oscillator_adsr(GcAudio *audio, GcAudioTrack *track) {
    unsigned values[5];
    for (unsigned i = 0; i < 5; ++i)
        values[i] = read_bytes(audio, track, 2);
    GcAudioEnvelope *envelope = &track->envelopes[0];
    memset(envelope, 0, sizeof(*envelope));
    envelope->enabled = true;
    envelope->rate = 1;
    envelope->scale = 1;
    envelope->attack_count = 4;
    envelope->attack[0] = (GcAudioEnvelopeStep){0, (uint16_t)values[0], 32767};
    envelope->attack[1] = (GcAudioEnvelopeStep){0, (uint16_t)values[1], 32767};
    envelope->attack[2] =
        (GcAudioEnvelopeStep){0, (uint16_t)values[2], (int16_t)values[3]};
    envelope->attack[3] = (GcAudioEnvelopeStep){14, 0, 0};
    envelope->release_count = 2;
    envelope->release[0] = (GcAudioEnvelopeStep){0, (uint16_t)values[4], 0};
    envelope->release[1] = (GcAudioEnvelopeStep){15, 1, 0};
}

static uint32_t table_value(GcAudio *audio, GcAudioTrack *track, size_t base,
                            unsigned index, unsigned bytes, unsigned stride) {
    if (bytes > 4 || base > audio->sequence_size ||
        (size_t)index * stride > audio->sequence_size - base) {
        track->active = false;
        ++audio->rejected_commands;
        return 0;
    }
    size_t offset = base + (size_t)index * stride;
    if (bytes > audio->sequence_size - offset) {
        track->active = false;
        ++audio->rejected_commands;
        return 0;
    }
    uint32_t value = 0;
    for (unsigned i = 0; i < bytes; ++i)
        value = value << 8 | audio->sequence[offset + i];
    return value;
}

static void register_command(GcAudio *audio, GcAudioTrack *track, unsigned opcode) {
    unsigned kind = opcode & 15;
    unsigned type = kind & 12;
    unsigned operation = kind & 3;
    unsigned table_type = 0;
    if (kind == 0xa) {
        unsigned mode = read_bytes(audio, track, 1);
        type = mode & 12;
        operation = 0xa;
        table_type = (mode >> 4) + 4;
    } else if (kind == 9) {
        unsigned mode = read_bytes(audio, track, 1);
        type = mode & 12;
        operation = mode & 0xf0;
    } else if (kind == 0xb) {
        type = 0;
        operation = 0xb;
    }
    unsigned reg = read_bytes(audio, track, 1);
    uint32_t base = 0;
    if (operation == 0xa)
        base = get_wide_register(track, read_bytes(audio, track, 1));
    int32_t operand = immediate_operand(audio, track, type);
    int32_t current = (int16_t)get_register(track, reg);
    uint32_t value = (uint32_t)operand;
    switch (operation) {
        case 1:
            value = (uint32_t)(current + (type == 4 ? (int8_t)operand : operand));
            break;
        case 2:
            track->registers[4] = (uint16_t)((current * operand) >> 16);
            track->registers[5] = (uint16_t)(current * operand);
            return;
        case 3:
            track->registers[3] = (uint16_t)(current - operand);
            return;
        case 0xa: {
            unsigned width = table_type <= 4   ? 1
                             : table_type <= 5 ? 2
                             : table_type == 6 ? 3
                                               : 4;
            unsigned stride = table_type == 8 ? 1 : width;
            value = table_value(audio, track, base, (uint16_t)operand, width, stride);
            break;
        }
        case 0xb:
            value = (uint32_t)(current - operand);
            break;
        case 0x10:
        case 0x20:
            value = (uint32_t)current << ((unsigned)operand & 31);
            break;
        case 0x30:
            value &= (uint32_t)current;
            break;
        case 0x40:
            value |= (uint32_t)current;
            break;
        case 0x50:
            value ^= (uint32_t)current;
            break;
        case 0x60:
            value = (uint32_t)-current;
            break;
        case 0x90:
            /* Native random operator is not present in the verified startup path.
             * Deterministic recovery tooling avoids a host RNG dependency.
             */
            value = operand ? (uint32_t)(audio->sequence_ticks % (uint16_t)operand) : 0;
            break;
        default:
            break;
    }
    set_register(track, reg, value);
}

static bool push_return(GcAudio *audio, GcAudioTrack *track, size_t pc) {
    if (track->depth >= 16) {
        ++audio->rejected_commands;
        track->active = false;
        return false;
    }
    track->stack[track->depth] = pc;
    track->repeats[track->depth++] = 0;
    return true;
}

static void jump_command(GcAudio *audio, GcAudioTrack *track, unsigned opcode) {
    unsigned mode = read_bytes(audio, track, 1);
    uint32_t target;
    if (mode & 0x80) {
        target = get_register(track, read_bytes(audio, track, 1));
        if (mode & 0x40) {
            uint32_t base = mode & 0x20
                                ? get_register(track, read_bytes(audio, track, 1))
                                : read_bytes(audio, track, 3);
            target = table_value(audio, track, base, target, 3, 3);
        }
    } else {
        target = read_bytes(audio, track, 3);
    }
    if (!condition(track, mode) || !track->active)
        return;
    if (target >= audio->sequence_size) {
        ++audio->rejected_commands;
        track->active = false;
        return;
    }
    if (opcode == 0xc4 && !push_return(audio, track, track->pc))
        return;
    track->pc = target;
}

static void begin_interrupt(GcAudioTrack *track) {
    if (track->interrupt_active)
        return;
    unsigned pending = track->interrupt_pending & track->interrupt_mask;
    for (unsigned i = 0; i < 8; ++i) {
        if (pending & (1u << i)) {
            track->saved_pc = track->pc;
            track->saved_wait = track->wait;
            track->pc = track->interrupts[i];
            track->wait = 0;
            track->interrupt_pending &= ~(1u << i);
            track->interrupt_active = true;
            return;
        }
    }
}

static void control_command(GcAudio *audio, unsigned index, unsigned opcode) {
    GcAudioTrack *track = &audio->tracks[index];
    unsigned first;
    unsigned second;
    uint32_t value;
    switch (opcode) {
        case 0xc0:
            read_bytes(audio, track, 1);
            break;
        case 0xc1:
        case 0xc2:
            first = read_bytes(audio, track, 1);
            value = read_bytes(audio, track, 3);
            if (opcode == 0xc1)
                open_track(audio, index, first, value);
            else if (track->parent < GC_AUDIO_TRACKS)
                open_track(audio, track->parent, first, value);
            break;
        case 0xc3:
            value = read_bytes(audio, track, 3);
            if (push_return(audio, track, track->pc))
                track->pc = value;
            break;
        case 0xc4:
        case 0xc8:
            jump_command(audio, track, opcode);
            break;
        case 0xc5:
        case 0xc6:
            first = opcode == 0xc6 ? read_bytes(audio, track, 1) : 0;
            if (!condition(track, first))
                break;
            if (track->depth)
                track->pc = track->stack[--track->depth];
            else {
                track->active = false;
                ++audio->rejected_commands;
            }
            break;
        case 0xc7:
            track->pc = read_bytes(audio, track, 3);
            break;
        case 0xc9:
            first = read_bytes(audio, track, 2);
            if (push_return(audio, track, track->pc))
                track->repeats[track->depth - 1] = first;
            break;
        case 0xca:
            if (track->depth) {
                unsigned *repeats = &track->repeats[track->depth - 1];
                if (!*repeats || --*repeats)
                    track->pc = track->stack[track->depth - 1];
                else
                    --track->depth;
            }
            break;
        case 0xcb:
            first = read_bytes(audio, track, 1);
            second = read_bytes(audio, track, 1);
            if (first < 16) {
                track->port_imported &= ~(1u << first);
                set_register(track, second, track->ports[first]);
            }
            break;
        case 0xcc:
            first = read_bytes(audio, track, 1);
            second = read_bytes(audio, track, 1);
            port_write(track, first, get_register(track, second), false);
            break;
        case 0xcd:
        case 0xce:
            first = read_bytes(audio, track, 1);
            value = opcode == 0xcd ? track->port_imported : track->port_exported;
            track->registers[3] =
                (uint16_t)(first < 16 && (value & (1u << first)) ? 1 : 0);
            break;
        case 0xcf:
            track->wait = get_register(track, read_bytes(audio, track, 1));
            break;
        case 0xd0:
            track->id = read_bytes(audio, track, 4);
            break;
        case 0xd1:
        case 0xd2:
        case 0xd3:
            first = read_bytes(audio, track, 1);
            if (opcode == 0xd3)
                first = get_register(track, first);
            second = get_register(track, read_bytes(audio, track, 1));
            if (opcode == 0xd1) {
                if (track->parent < GC_AUDIO_TRACKS)
                    port_write(&audio->tracks[track->parent], first & 15,
                               (uint16_t)second, true);
            } else {
                unsigned child = track->children[first >> 4 & 15];
                if (child < GC_AUDIO_TRACKS)
                    port_write(&audio->tracks[child], first & 15, (uint16_t)second,
                               true);
            }
            break;
        case 0xd4:
            track->previous_key = (int)read_bytes(audio, track, 1);
            break;
        case 0xd5:
            track->time_mode = read_bytes(audio, track, 1);
            break;
        case 0xd6:
            oscillator_cycle(track, read_bytes(audio, track, 1));
            break;
        case 0xd7:
            first = read_bytes(audio, track, 1);
            value = read_bytes(audio, track, 3);
            if (first < 2) {
                GcAudioEnvelope *envelope = &track->envelopes[0];
                if (!first) {
                    memset(envelope, 0, sizeof(*envelope));
                    envelope->enabled = true;
                    envelope->rate = 1;
                    envelope->scale = 1;
                    envelope->release_count = 2;
                    envelope->release[0] = (GcAudioEnvelopeStep){0, 10, 0};
                    envelope->release[1] = (GcAudioEnvelopeStep){15, 1, 0};
                }
                GcAudioEnvelopeStep *steps =
                    first ? envelope->release : envelope->attack;
                unsigned *count =
                    first ? &envelope->release_count : &envelope->attack_count;
                if (!gc_audio_envelope_decode_steps(steps, count, audio->sequence,
                                                    audio->sequence_size, value))
                    ++audio->rejected_commands;
            }
            break;
        case 0xd8:
            oscillator_adsr(audio, track);
            break;
        case 0xd9:
            track->transpose = (int8_t)read_bytes(audio, track, 1);
            break;
        case 0xda:
            first = read_bytes(audio, track, 1);
            if (first & 0x20)
                first = get_register(track, first & 15);
            if (first < 16) {
                stop_track(audio, track->children[first]);
                track->children[first] = NO_TRACK;
            }
            break;
        case 0xdb:
            read_bytes(audio, track, 1);
            break;
        case 0xdc:
            read_bytes(audio, track, 2);
            break;
        case 0xdd:
            first = read_bytes(audio, track, 1);
            value = read_bytes(audio, track, audio->sequence_revision ? 2 : 1);
            if (audio->sequence_revision ? first < 6 : first < 4)
                track->routes[first] = (uint16_t)value;
            else
                ++audio->rejected_commands;
            break;
        case 0xde:
            read_bytes(audio, track, 1);
            break;
        case 0xdf:
            first = read_bytes(audio, track, 1);
            value = read_bytes(audio, track, 3);
            if (first < 8 && value < audio->sequence_size) {
                track->interrupt_mask |= 1u << first;
                track->interrupts[first] = value;
            } else {
                ++audio->rejected_commands;
            }
            break;
        case 0xe0:
            first = read_bytes(audio, track, 1);
            if (first < 8)
                track->interrupt_mask &= ~(1u << first);
            break;
        case 0xe1:
        case 0xe2:
            break;
        case 0xe3:
            if (track->interrupt_active) {
                track->pc = track->saved_pc;
                track->wait = track->saved_wait;
                track->interrupt_active = false;
                begin_interrupt(track);
            }
            break;
        case 0xe4:
            track->timer_count = read_bytes(audio, track, 1);
            track->timer_period = read_bytes(audio, track, 2);
            track->timer = track->timer_period;
            break;
        case 0xe5:
        case 0xe6:
            break;
        case 0xe7:
            read_bytes(audio, track, 2);
            track->registers[3] = 0xffff;
            break;
        case 0xe8:
        case 0xe9:
            for (unsigned i = 0; i < GC_AUDIO_VOICES; ++i) {
                if (audio->voices[i].active && audio->voices[i].track == index)
                    gc_audio_voice_release(&audio->voices[i]);
            }
            break;
        case 0xea:
            track->wait = read_bytes(audio, track, 3);
            break;
        case 0xf0:
            /* The EUR sequence revision carries an additional one-byte control
             * operand here. Its verified event paths use zero.
             */
            if (audio->sequence_revision)
                read_bytes(audio, track, 1);
            break;
        case 0xfd:
            value = read_bytes(audio, track, 2);
            if (!index && value && value <= 1000)
                audio->tempo = value;
            break;
        case 0xfe:
            value = read_bytes(audio, track, 2);
            if (value)
                track->timebase = value;
            if (!index && value && value <= 1000)
                audio->timebase = value;
            break;
        case 0xff:
            stop_track(audio, index);
            break;
        default:
            if (opcode < 0xeb || opcode > 0xfc) {
                track->active = false;
                ++audio->rejected_commands;
            }
            break;
    }
}

static void step_track(GcAudio *audio, unsigned index, unsigned depth) {
    GcAudioTrack *track = &audio->tracks[index];
    if (!track->active || depth >= GC_AUDIO_TRACKS)
        return;
    if (track->timer && !--track->timer) {
        track->interrupt_pending |= 1u << 6;
        if (!track->timer_count || --track->timer_count)
            track->timer = track->timer_period;
    }
    track->interrupt_pending |= 1u << 7;
    begin_interrupt(track);
    if (track->wait)
        --track->wait;
    if (!track->wait) {
        unsigned instructions = 0;
        while (track->active && !track->wait && instructions++ < 4096) {
            unsigned opcode = read_bytes(audio, track, 1);
            if (!track->active)
                break;
            if (opcode < 0x80) {
                note_command(audio, index, opcode);
            } else if ((opcode & 0xf0) == 0x80) {
                unsigned slot = opcode & 15;
                if (!slot || slot == 8)
                    track->wait = read_bytes(audio, track, slot == 8 ? 2 : 1);
                else {
                    if (slot > 8) {
                        slot -= 8;
                        read_bytes(audio, track, 1);
                    }
                    note_off(audio, index, slot);
                }
            } else if ((opcode & 0xf0) == 0x90) {
                parameter_command(audio, track, opcode);
            } else if ((opcode & 0xf0) == 0xa0) {
                register_command(audio, track, opcode);
            } else {
                control_command(audio, index, opcode);
            }
        }
        if (instructions >= 4096 && track->active && !track->wait) {
            track->active = false;
            ++audio->rejected_commands;
        }
    }
    for (unsigned parameter = 0; parameter < 17; ++parameter) {
        if (track->parameter_ticks[parameter]) {
            track->parameters[parameter] += track->parameter_steps[parameter];
            if (!--track->parameter_ticks[parameter])
                track->parameters[parameter] = track->parameter_targets[parameter];
            oscillator_parameter(track, parameter, track->parameters[parameter]);
        }
    }
    for (unsigned slot = 0; slot < 16; ++slot) {
        unsigned child = track->children[slot];
        if (child < GC_AUDIO_TRACKS && audio->tracks[child].parent == index) {
            step_track(audio, child, depth + 1);
            if (!audio->tracks[child].active && track->children[slot] == child)
                stop_track(audio, child);
        }
    }
}

void gc_audio_sequence_init(GcAudio *audio) {
    atomic_init(&audio->active_voices, 0);
    atomic_init(&audio->sequence_stopped, false);
    audio->tempo = 120;
    audio->timebase = 48;
    initialize_track(audio, 0, NO_TRACK, 0, 0);
    step_track(audio, 0, 0);
}

void gc_audio_sequence_tick(GcAudio *audio) {
    if (!gc_audio_sequence_stopped(audio))
        ++audio->sequence_ticks;
    unsigned read = atomic_load_explicit(&audio->event_read, memory_order_relaxed);
    unsigned write = atomic_load_explicit(&audio->event_write, memory_order_acquire);
    while (read != write) {
        GcAudioEvent event = audio->pending_events[read];
        if (event.number < 0x100)
            gc_audio_sequence_event(audio, event.number);
        else if (event.number < 0x102)
            gc_audio_sequence_cube(audio, event.number - 0x100, event.value);
        else if (event.number == 0x104) {
            stop_track(audio, 0);
            atomic_store_explicit(&audio->sequence_stopped, true, memory_order_release);
        } else {
            GcAudioTrack *menu = find_track(audio, 0x21001);
            port_write(menu, event.number == 0x102 ? 0 : 1,
                       event.number == 0x102 ? 1 : 0, true);
        }
        read = (read + 1) % GC_AUDIO_EVENT_QUEUE;
    }
    atomic_store_explicit(&audio->event_read, read, memory_order_release);
    step_track(audio, 0, 0);
    for (unsigned i = 0; i < GC_AUDIO_VOICES; ++i) {
        GcAudioVoice *voice = &audio->voices[i];
        if (!voice->active)
            continue;
        if (!voice->released) {
            voice->step =
                (float)voice->base_step * inherited_pitch(audio, voice->track);
            voice->gain =
                voice->base_gain * inherited_parameter(audio, voice->track, 0);
            update_voice_pan(audio, voice);
        }
    }
}
