#include "audio_internal.h"
#include "console_common/support/endian.h"
#include "console_common/support/bounds.h"
#include "gamecube/ipl.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* USA IPL: AFC predictor table at ROM 0xa9400 (BS2 0x813a8be0).
 * These are codec constants, recovered from the supplied IPL, not audio data.
 */
static const int16_t afc_coefficients[16][2] = {
    {0, 0},        {2048, 0},     {0, 2048},     {1024, 1024},
    {4096, -2048}, {3584, -1536}, {3072, -1024}, {4608, -2560},
    {4200, -2248}, {4800, -2300}, {5120, -3072}, {2048, -2048},
    {1024, -1024}, {-1024, 1024}, {-1024, 0},    {-2048, 0},
};

static bool decode_resampling_coefficients(GcAudio *audio, const uint8_t *rom) {
    /* sub_81352ce0 supplies the table immediately after the AFC predictors
     * to DSP command 0x81. USA/JAP ROM 0xa9440 and EUR ROM 0xf6c40 contain
     * the same 64 phases, each with four signed 15-bit fractional weights.
     * Locate the adjacent codec table so regional code relocation is harmless.
     */
    for (size_t offset = 0; cc_bounds_contains(GC_IPL_ROM_SIZE, offset, 64 + 512);
         offset += 32) {
        bool match = true;
        for (unsigned index = 0; index < 32; ++index) {
            if ((int16_t)cc_read_be16(rom + offset + index * 2) !=
                afc_coefficients[index / 2][index % 2]) {
                match = false;
                break;
            }
        }
        if (!match)
            continue;
        for (unsigned phase = 0; phase < 64; ++phase) {
            int sum = 0;
            for (unsigned tap = 0; tap < 4; ++tap) {
                int16_t coefficient =
                    (int16_t)cc_read_be16(rom + offset + 64 + phase * 8 + tap * 2);
                audio->resampling_coefficients[phase][tap] = coefficient;
                sum += coefficient;
            }
            if (sum < 32700 || sum > 32800)
                return false;
        }
        return true;
    }
    return false;
}

static bool decode_pitch_ratios(GcAudio *audio, const uint8_t *rom) {
    /* sub_813587c0 selects the integer note table; sub_81357e60 multiplies
     * it by a 64-entry fractional semitone table. The native entries are
     * rounded decimal constants, so substituting exp2 changes DSP pitch.
     */
    size_t integer_offset = audio->sequence_revision ? 0xf7058 : 0xa97c0;
    size_t fraction_offset = audio->sequence_revision ? 0xf7968 : 0xa9b88;
    for (unsigned index = 0; index < 128; ++index) {
        float value = cc_read_be_float(rom + integer_offset + index * 4);
        if (!isfinite(value) || value <= 0 || value > 64 ||
            (index && value <= audio->semitone_ratios[index - 1]))
            return false;
        audio->semitone_ratios[index] = value;
    }
    for (unsigned index = 0; index < 64; ++index) {
        float value = cc_read_be_float(rom + fraction_offset + index * 4);
        if (!isfinite(value) || value < 1 || value >= 1.06f ||
            (index && value <= audio->fractional_semitone_ratios[index - 1]))
            return false;
        audio->fractional_semitone_ratios[index] = value;
    }
    return audio->semitone_ratios[60] == 1 && audio->fractional_semitone_ratios[0] == 1;
}

static bool decode_mixer_gains(GcAudio *audio, const uint8_t *rom) {
    /* USA/JAP sub_81359a40 and EUR sub_8135b0c0 pass two independent
     * constants to the master and DSP-output setters. Their units are
     * 16384 and 4096; the DSP voice mixer uses M0 (Q16), while the output
     * multiplier shifts its product left four bits (Q12).
     */
    size_t offset = audio->sequence_revision ? 0x1aecf8 : 0x15ed58;
    float master = cc_read_be_float(rom + offset);
    float output = cc_read_be_float(rom + offset + 4);
    if (!isfinite(master) || !isfinite(output) || master <= 0 || master >= 4 ||
        output <= 0 || output >= 16)
        return false;
    audio->master_gain = (uint16_t)(master * 16384);
    audio->output_gain = (uint16_t)(output * 4096);
    return true;
}

bool gc_audio_afc_decode(const uint8_t *encoded, size_t encoded_size, int16_t *samples,
                         size_t sample_capacity, int16_t history[2]) {
    if (!encoded || !samples || !history || encoded_size % 9 ||
        encoded_size / 9 > sample_capacity / 16)
        return false;
    int32_t recent = history[0];
    int32_t previous = history[1];
    size_t output = 0;
    for (size_t frame = 0; frame < encoded_size; frame += 9) {
        unsigned scale = encoded[frame] >> 4;
        unsigned predictor = encoded[frame] & 15;
        int32_t multiplier = INT32_C(1) << scale;
        for (unsigned sample = 0; sample < 16; ++sample) {
            unsigned byte = encoded[frame + 1 + sample / 2];
            int32_t residual = sample & 1 ? (int32_t)(byte & 15) : (int32_t)(byte >> 4);
            if (residual >= 8)
                residual -= 16;
            int64_t accumulator = (int64_t)residual * multiplier * 2048 +
                                  (int64_t)afc_coefficients[predictor][0] * recent +
                                  (int64_t)afc_coefficients[predictor][1] * previous;
            /* Arithmetic shift, expressed without implementation-defined
             * signed right shifts or shifting a negative residual.
             */
            int64_t decoded =
                accumulator >= 0 ? accumulator / 2048 : -((-accumulator + 2047) / 2048);
            if (decoded > INT16_MAX)
                decoded = INT16_MAX;
            if (decoded < INT16_MIN)
                decoded = INT16_MIN;
            previous = recent;
            recent = (int32_t)decoded;
            samples[output++] = (int16_t)decoded;
        }
    }
    history[0] = (int16_t)recent;
    history[1] = (int16_t)previous;
    return true;
}

static size_t find_magic(const uint8_t *rom, size_t start, const char *magic) {
    for (size_t offset = start; cc_bounds_contains(GC_IPL_ROM_SIZE, offset, 4);
         offset += 4) {
        if (!memcmp(rom + offset, magic, 4))
            return offset;
    }
    return SIZE_MAX;
}

static bool wave_mapping(const uint8_t *ws, size_t size, unsigned *ids,
                         unsigned count) {
    /* sub_813533a0 resolves WBCT -> SCNE -> C-DF; sub_813537a0 assigns
     * archive descriptors in order to its wave identifiers.
     */
    size_t wbct = cc_read_be32(ws + 0x14);
    if (!cc_bounds_contains(size, wbct, 16) || memcmp(ws + wbct, "WBCT", 4) ||
        cc_read_be32(ws + wbct + 8) != 1)
        return false;
    size_t scene = cc_read_be32(ws + wbct + 12);
    if (!cc_bounds_contains(size, scene, 24) || memcmp(ws + scene, "SCNE", 4))
        return false;
    size_t cdf = cc_read_be32(ws + scene + 12);
    if (!cc_bounds_contains(size, cdf, 8 + 4 * count) || memcmp(ws + cdf, "C-DF", 4) ||
        cc_read_be32(ws + cdf + 4) != count)
        return false;
    for (unsigned i = 0; i < count; ++i) {
        size_t entry = cc_read_be32(ws + cdf + 8 + i * 4);
        if (!cc_bounds_contains(size, entry, 12))
            return false;
        ids[i] = cc_read_be32(ws + entry) & 0xffff;
        for (unsigned previous = 0; previous < i; ++previous) {
            if (ids[previous] == ids[i])
                return false;
        }
    }
    return true;
}

static bool decode_wave(GcAudioWave *wave, const uint8_t *descriptor,
                        const uint8_t *archive, size_t archive_size) {
    size_t offset = cc_read_be32(descriptor + 8);
    size_t encoded_size = cc_read_be32(descriptor + 12);
    size_t sample_count = cc_read_be32(descriptor + 28);
    float rate = cc_read_be_float(descriptor + 4);
    size_t capacity = encoded_size / 9 * 16;
    if (descriptor[1] != 0 || encoded_size % 9 || sample_count > capacity ||
        !sample_count || capacity > GC_IPL_ROM_SIZE * 2 ||
        !cc_bounds_contains(archive_size, offset, encoded_size) || !isfinite(rate) ||
        rate < 8000 || rate > 192000)
        return false;
    wave->samples = calloc(capacity, sizeof(*wave->samples));
    if (!wave->samples)
        return false;
    int16_t history[2] = {0};
    if (!gc_audio_afc_decode(archive + offset, encoded_size, wave->samples, capacity,
                             history))
        return false;
    wave->count = sample_count;
    wave->afc_source = true;
    wave->rate = rate;
    wave->key = descriptor[2];
    wave->loop = cc_read_be32(descriptor + 16) != 0;
    wave->loop_start = cc_read_be32(descriptor + 20);
    wave->loop_end = cc_read_be32(descriptor + 24);
    return !wave->loop ||
           (wave->loop_start < wave->loop_end && wave->loop_end <= wave->count);
}

static bool decode_waves(GcAudio *audio, const uint8_t *rom, size_t ws_offset,
                         size_t *sequence_offset) {
    const uint8_t *ws = rom + ws_offset;
    size_t size = cc_read_be32(ws + 4);
    if (size < 24 || !cc_bounds_contains(GC_IPL_ROM_SIZE, ws_offset, size))
        return false;
    size_t winf = cc_read_be32(ws + 16);
    if (!cc_bounds_contains(size, winf, 12) || memcmp(ws + winf, "WINF", 4) ||
        cc_read_be32(ws + winf + 4) != 1)
        return false;
    size_t archive_descriptor = cc_read_be32(ws + winf + 8);
    if (!cc_bounds_contains(size, archive_descriptor, 0x48) ||
        memcmp(ws + archive_descriptor, "ipl_0.aw", 9))
        return false;
    /* The EUR revision expands the archive header to 0x74 bytes and wave
     * descriptors to 0x2c. Native offsets, rather than assumed record strides,
     * identify both layouts. The sample codec and bank layout are unchanged.
     */
    size_t count_offset = 0x44;
    if (!cc_read_be32(ws + archive_descriptor + count_offset))
        count_offset = 0x70;
    if (!cc_bounds_contains(size, archive_descriptor, count_offset + 4))
        return false;
    unsigned count = cc_read_be32(ws + archive_descriptor + count_offset);
    size_t wave_table = count_offset + 4;
    audio->sequence_revision = count_offset == 0x70 ? 1 : 0;
    if (!count || count > GC_AUDIO_WAVES ||
        !cc_bounds_contains(size, archive_descriptor, wave_table + count * 4))
        return false;
    unsigned ids[GC_AUDIO_WAVES];
    if (!wave_mapping(ws, size, ids, count))
        return false;
    size_t archive_size = 0;
    for (unsigned i = 0; i < count; ++i) {
        size_t descriptor = cc_read_be32(ws + archive_descriptor + wave_table + i * 4);
        if (!cc_bounds_contains(size, descriptor, 36))
            return false;
        size_t offset = cc_read_be32(ws + descriptor + 8);
        size_t length = cc_read_be32(ws + descriptor + 12);
        if (!cc_bounds_contains(GC_IPL_ROM_SIZE, offset, length))
            return false;
        if (offset + length > archive_size)
            archive_size = offset + length;
    }
    archive_size = (archive_size + 31) & ~(size_t)31;
    size_t archive_offset = (ws_offset + size + 31) & ~(size_t)31;
    if (!cc_bounds_contains(GC_IPL_ROM_SIZE, archive_offset, archive_size))
        return false;
    audio->wave_count = count;
    for (unsigned i = 0; i < count; ++i) {
        size_t descriptor = cc_read_be32(ws + archive_descriptor + wave_table + i * 4);
        audio->waves[i].id = ids[i];
        if (!decode_wave(&audio->waves[i], ws + descriptor, rom + archive_offset,
                         archive_size))
            return false;
    }
    *sequence_offset = archive_offset + archive_size;
    return true;
}

static bool instrument_regions(GcAudioInstrument *instrument, const uint8_t *bank,
                               size_t size, const uint8_t *entry) {
    unsigned keys = cc_read_be32(entry + 0x28);
    if (keys > 8 || !cc_bounds_contains(size, (size_t)(entry - bank), 0x2c + keys * 4))
        return false;
    for (unsigned key_index = 0; key_index < keys; ++key_index) {
        size_t key = cc_read_be32(entry + 0x2c + key_index * 4);
        if (!cc_bounds_contains(size, key, 8))
            return false;
        unsigned velocities = cc_read_be32(bank + key + 4);
        if (velocities > 8 || velocities > 8 - instrument->region_count ||
            !cc_bounds_contains(size, key, 8 + velocities * 4))
            return false;
        for (unsigned j = 0; j < velocities; ++j) {
            size_t velocity = cc_read_be32(bank + key + 8 + j * 4);
            if (!cc_bounds_contains(size, velocity, 16))
                return false;
            GcAudioRegion *region = &instrument->regions[instrument->region_count++];
            region->key = bank[key];
            region->velocity = bank[velocity];
            region->wave = (uint16_t)cc_read_be32(bank + velocity + 4);
            region->volume = cc_read_be_float(bank + velocity + 8);
            region->pitch = cc_read_be_float(bank + velocity + 12);
            if (!isfinite(region->volume) || !isfinite(region->pitch) ||
                region->volume < 0 || region->pitch <= 0)
                return false;
        }
    }
    return instrument->region_count != 0;
}

static bool decode_envelope_table(GcAudioEnvelopeStep *steps, unsigned *count,
                                  const uint8_t *data, size_t size, size_t offset) {
    *count = 0;
    for (unsigned i = 0; i < GC_AUDIO_ENVELOPE_STEPS; ++i) {
        if (!cc_bounds_contains(size, offset, 6))
            return false;
        const uint8_t *entry = data + offset;
        steps[i].curve = cc_read_be16(entry);
        steps[i].ticks = cc_read_be16(entry + 2);
        steps[i].value = (int16_t)cc_read_be16(entry + 4);
        *count = i + 1;
        if (steps[i].curve >= 13 && steps[i].curve <= 15)
            return true;
        if (steps[i].curve > 2)
            return false;
        offset += 6;
    }
    return false;
}

bool gc_audio_envelope_decode_steps(GcAudioEnvelopeStep *steps, unsigned *count,
                                    const uint8_t *data, size_t size, size_t offset) {
    *count = 0;
    return !offset || decode_envelope_table(steps, count, data, size, offset);
}

bool gc_audio_envelope_decode_sequence_steps(GcAudioEnvelopeStep *steps,
                                             unsigned *count, const uint8_t *data,
                                             size_t size, size_t offset) {
    return decode_envelope_table(steps, count, data, size, offset);
}

static bool decode_envelope(GcAudioEnvelope *envelope, const uint8_t *bank, size_t size,
                            size_t offset) {
    if (!offset)
        return true;
    if (!cc_bounds_contains(size, offset, 24))
        return false;
    const uint8_t *entry = bank + offset;
    envelope->enabled = true;
    uint32_t attack = cc_read_be32(entry + 8);
    uint32_t release = cc_read_be32(entry + 12);
    if (attack > GC_AUDIO_TABLE_OFFSET_MASK || release > GC_AUDIO_TABLE_OFFSET_MASK)
        return false;
    envelope->attack_identity = attack ? GC_AUDIO_TABLE_BANK | attack : 0;
    envelope->release_identity = release ? GC_AUDIO_TABLE_BANK | release : 0;
    envelope->release_continues = attack && attack == release;
    envelope->target = entry[0];
    envelope->rate = cc_read_be_float(entry + 4);
    envelope->scale = cc_read_be_float(entry + 16);
    envelope->offset = cc_read_be_float(entry + 20);
    if (envelope->target > 2 || !isfinite(envelope->rate) ||
        !isfinite(envelope->scale) || !isfinite(envelope->offset) || envelope->rate < 0)
        return false;
    return gc_audio_envelope_decode_steps(envelope->attack, &envelope->attack_count,
                                          bank, size, cc_read_be32(entry + 8)) &&
           gc_audio_envelope_decode_steps(envelope->release, &envelope->release_count,
                                          bank, size, cc_read_be32(entry + 12));
}

static bool decode_instruments(GcAudio *audio, const uint8_t *rom, size_t offset) {
    const uint8_t *bank = rom + offset;
    size_t size = cc_read_be32(bank + 4);
    if (size < 0x224 || !cc_bounds_contains(GC_IPL_ROM_SIZE, offset, size) ||
        memcmp(bank + 0x20, "BANK", 4))
        return false;
    for (unsigned program = 0; program < 128; ++program) {
        size_t entry_offset = cc_read_be32(bank + 0x24 + program * 4);
        if (!entry_offset)
            continue;
        if (!cc_bounds_contains(size, entry_offset, 0x2c) ||
            memcmp(bank + entry_offset, "INST", 4))
            return false;
        GcAudioInstrument *instrument = &audio->instruments[program];
        const uint8_t *entry = bank + entry_offset;
        instrument->volume = cc_read_be_float(entry + 8);
        instrument->pitch = cc_read_be_float(entry + 12);
        if (!isfinite(instrument->volume) || !isfinite(instrument->pitch) ||
            instrument->volume < 0 || instrument->pitch <= 0 ||
            !instrument_regions(instrument, bank, size, entry))
            return false;
        for (unsigned i = 0; i < 2; ++i) {
            if (!decode_envelope(&instrument->envelopes[i], bank, size,
                                 cc_read_be32(entry + 16 + i * 4)))
                return false;
        }
        ++audio->instrument_count;
    }
    return audio->instrument_count != 0;
}

static bool decode_effects(GcAudio *audio, const uint8_t *rom) {
    /* USA/JAP sub_81352d40 configures four native delay buses. EUR
     * sub_8135b0c0 instead submits the three enabled descriptors below.
     * Coefficients are borrowed from the caller's IPL, then retained as data.
     */
    if (!audio->sequence_revision) {
        size_t table = 0x5ec48;
        if (!cc_bounds_contains(GC_IPL_ROM_SIZE, table, 16))
            return false;
        for (unsigned index = 0; index < 4; ++index) {
            GcAudioEffect *effect = &audio->effects[index];
            effect->mode = index < 2 ? 1 : 2;
            effect->length = index < 2 ? 8000 : 2400;
            effect->return_bus[index & 1] = 1 + (index & 1);
            effect->return_gain[index & 1] = (int16_t)(index < 2 ? 0x2fff : 0x7fff);
            if (index < 2)
                for (unsigned tap = 0; tap < 8; ++tap)
                    effect->filter[tap] = (int16_t)cc_read_be16(rom + table + tap * 2);
        }
    } else {
        size_t table = 0xf7dc0;
        if (!cc_bounds_contains(GC_IPL_ROM_SIZE, table, 128))
            return false;
        for (unsigned index = 0; index < 3; ++index) {
            const uint8_t *descriptor = rom + table + index * 32;
            GcAudioEffect *effect = &audio->effects[index];
            effect->mode = descriptor[0];
            uint32_t delay_blocks = cc_read_be32(descriptor + 12);
            if (effect->mode < 1 || effect->mode > 2 || !delay_blocks ||
                delay_blocks > GC_AUDIO_EFFECT_SAMPLES / 80)
                return false;
            effect->length = delay_blocks * 80;
            for (unsigned channel = 0; channel < 2; ++channel) {
                unsigned native_bus = cc_read_be16(descriptor + 2 + channel * 4);
                if (native_bus > 11)
                    return false;
                unsigned bus = native_bus < 11 ? native_bus + 1 : 0;
                int16_t gain = (int16_t)cc_read_be16(descriptor + 4 + channel * 4);
                /* Native returns into delay buses require the whole block's
                 * DMA/FIR ordering. The supplied descriptors never use them.
                 */
                if (gain && bus >= 3 && bus <= 6)
                    return false;
                effect->return_bus[channel] = bus;
                effect->return_gain[channel] = gain;
            }
            for (unsigned tap = 0; tap < 8; ++tap)
                effect->filter[tap] = (int16_t)cc_read_be16(descriptor + 16 + tap * 2);
        }
        /* EUR DSP 0x0aaf initializes its chorus with a 160-sample ring,
         * read position 150, and alternating 1 +/- 1/128 playback rates.
         */
        audio->chorus_read = 150u * 65536u;
        audio->chorus_direction = -1;
    }
    return true;
}

/* The caller supplies a zero-initialized owner. Failed decoding may retain
 * partial resources, which gc_audio_resources_release always releases.
 */
bool gc_audio_resources_decode(GcAudio *audio, uint8_t *rom, size_t size) {
    if (!audio || !rom || size != GC_IPL_ROM_SIZE)
        return false;
    size_t bank = find_magic(rom, 0, "IBNK");
    if (bank == SIZE_MAX) {
        gc_ipl_descramble(rom, GC_IPL_ROM_SIZE);
        bank = find_magic(rom, 0, "IBNK");
    }
    if (bank == SIZE_MAX || !cc_bounds_contains(GC_IPL_ROM_SIZE, bank, 8))
        return false;
    size_t ws = find_magic(rom, bank + 8, "WSYS");
    size_t sequence_offset;
    if (ws == SIZE_MAX || !cc_bounds_contains(GC_IPL_ROM_SIZE, ws, 24) ||
        !decode_resampling_coefficients(audio, rom) ||
        !decode_instruments(audio, rom, bank) ||
        !decode_waves(audio, rom, ws, &sequence_offset) ||
        !decode_pitch_ratios(audio, rom) || !decode_mixer_gains(audio, rom) ||
        !decode_effects(audio, rom) ||
        !cc_bounds_contains(GC_IPL_ROM_SIZE, sequence_offset, 6) ||
        rom[sequence_offset] != 0xfe || rom[sequence_offset + 3] != 0xfd)
        return false;
    /* Native iplrom.com has an EOF marker followed by 32-byte alignment.
     * USA/JAP resolver sub_81359d40 supplies 0xc20; EUR adds sequence
     * instructions and moves that marker to 0xc29, giving 0xc40.
     */
    size_t marker = 0;
    for (; marker < GC_AUDIO_SEQUENCE_LIMIT; ++marker) {
        if (!cc_bounds_contains(GC_IPL_ROM_SIZE, sequence_offset + marker, 4))
            return false;
        if (!memcmp(rom + sequence_offset + marker, "EOF\0", 4))
            break;
    }
    if (marker == GC_AUDIO_SEQUENCE_LIMIT)
        return false;
    audio->sequence_size = (marker + 4 + 31) & ~(size_t)31;
    if (!cc_bounds_contains(GC_IPL_ROM_SIZE, sequence_offset, audio->sequence_size))
        return false;
    audio->sequence = malloc(audio->sequence_size);
    if (!audio->sequence)
        return false;
    memcpy(audio->sequence, rom + sequence_offset, audio->sequence_size);
    gc_audio_route_table_init(audio);
    return true;
}

bool gc_audio_resources_load(GcAudio *audio, const char *ipl_path) {
    if (!audio || !ipl_path)
        return false;
    uint8_t *rom = NULL;
    bool decoded = gc_ipl_rom_read(ipl_path, &rom) &&
                   gc_audio_resources_decode(audio, rom, GC_IPL_ROM_SIZE);
    free(rom);
    return decoded;
}

void gc_audio_resources_release(GcAudio *audio) {
    if (!audio)
        return;
    for (unsigned wave = 0; wave < audio->wave_count; ++wave) {
        free(audio->waves[wave].samples);
        audio->waves[wave].samples = NULL;
    }
    audio->wave_count = 0;
    free(audio->sequence);
    audio->sequence = NULL;
    audio->sequence_size = 0;
}
