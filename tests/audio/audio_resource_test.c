#include "audio/audio_internal.h"
#include "console_common/support/endian.h"
#include "gamecube/ipl.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static size_t wave_descriptor(const uint8_t *rom, bool last) {
    size_t offset = 0;
    for (; offset + 4 <= GC_IPL_ROM_SIZE; offset += 4) {
        if (!memcmp(rom + offset, "WSYS", 4))
            break;
    }
    assert(offset + 24 <= GC_IPL_ROM_SIZE);
    const uint8_t *system = rom + offset;
    size_t info = cc_read_be32(system + 16);
    size_t archive = cc_read_be32(system + info + 8);
    size_t count_offset = cc_read_be32(system + archive + 0x44) ? 0x44 : 0x70;
    unsigned count = cc_read_be32(system + archive + count_offset);
    assert(count > 0 && count <= GC_AUDIO_WAVES);
    size_t index = last ? count - 1 : 0;
    return offset + cc_read_be32(system + archive + count_offset + 4 + index * 4);
}

static void write_rate(uint8_t *destination, float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    destination[0] = (uint8_t)(bits >> 24);
    destination[1] = (uint8_t)(bits >> 16);
    destination[2] = (uint8_t)(bits >> 8);
    destination[3] = (uint8_t)bits;
}

static void write_halfword(uint8_t *destination, uint16_t value) {
    destination[0] = (uint8_t)(value >> 8);
    destination[1] = (uint8_t)value;
}

static void assert_released(GcAudio *audio) {
    gc_audio_resources_release(audio);
    assert(audio->wave_count == 0 && !audio->sequence && !audio->sequence_size);
    for (unsigned index = 0; index < GC_AUDIO_WAVES; ++index)
        assert(!audio->waves[index].samples);
}

static void test_note_pitch(GcAudio *audio, unsigned rate_index) {
    /* Descriptor precision enters the USA double chain and the EUR chain
     * of separately rounded binary32 division and multiplication.
     */
    const float pitches[2][4] = {
        {0x1.eb1528p-3f, 0x1.eb172p-1f, 0x1.526378p0f, 0x1.704fdep2f},
        {0x1.eb152ap-3f, 0x1.eb172p-1f, 0x1.526378p0f, 0x1.704fdep2f},
    };
    uint8_t sequence[] = {(uint8_t)audio->waves[0].key, 1, 127, 0x80, 255};
    uint8_t *original_sequence = audio->sequence;
    size_t original_size = audio->sequence_size;
    audio->sequence = sequence;
    audio->sequence_size = sizeof(sequence);
    atomic_init(&audio->mono, false);
    atomic_init(&audio->event_read, 0);
    atomic_init(&audio->event_write, 0);
    memset(audio->instruments, 0, sizeof(audio->instruments));
    audio->instruments[0] = (GcAudioInstrument){
        .volume = 1,
        .pitch = 0.8f,
        .region_count = 1,
        .regions = {{.key = 127,
                     .velocity = 127,
                     .wave = (uint16_t)audio->waves[0].id,
                     .volume = 1,
                     .pitch = 1.2f}},
    };
    GcAudioTrack *track = &audio->tracks[0];
    track->active = true;
    track->parent = GC_AUDIO_TRACKS;
    track->parameters[0] = 1;
    track->routes[0] = audio->sequence_revision ? 0x100 : 0x10;
    track->registers[9] = 1;
    track->envelope_modes[0] = track->envelope_modes[1] = 15;
    for (unsigned child = 0; child < 16; ++child)
        track->children[child] = GC_AUDIO_TRACKS;
    gc_audio_sequence_tick(audio);
    assert(audio->notes_started == 1);
    assert(audio->voices[0].base_step == pitches[audio->sequence_revision][rate_index]);
    audio->sequence = original_sequence;
    audio->sequence_size = original_size;
}

static void test_rates(const uint8_t *original) {
    const float rates[] = {8000, 32000.5f, 44100.25f, 192000};
    const float invalid[] = {0, -1, 7999.5f, 192000.5f, INFINITY, NAN};
    uint8_t *rom = malloc(GC_IPL_ROM_SIZE);
    GcAudio *audio = calloc(1, sizeof(*audio));
    assert(rom && audio);
    size_t first = wave_descriptor(original, false);
    size_t last = wave_descriptor(original, true);
    for (unsigned index = 0; index < sizeof(rates) / sizeof(rates[0]); ++index) {
        memcpy(rom, original, GC_IPL_ROM_SIZE);
        memset(audio, 0, sizeof(*audio));
        write_rate(rom + first + 4, rates[index]);
        assert(gc_audio_resources_decode(audio, rom, GC_IPL_ROM_SIZE));
        assert(audio->waves[0].rate == rates[index]);
        test_note_pitch(audio, index);
        assert_released(audio);
    }
    for (unsigned index = 0; index < sizeof(invalid) / sizeof(invalid[0]); ++index) {
        memcpy(rom, original, GC_IPL_ROM_SIZE);
        memset(audio, 0, sizeof(*audio));
        write_rate(rom + last + 4, invalid[index]);
        assert(!gc_audio_resources_decode(audio, rom, GC_IPL_ROM_SIZE));
        /* Earlier wave allocations must also be releasable after failure. */
        assert(audio->wave_count > 1 && audio->waves[0].samples);
        assert_released(audio);
        assert_released(audio);
    }
    free(audio);
    free(rom);
}

static void test_effect_aliases(const uint8_t *original, unsigned revision) {
    if (!revision)
        return;
    uint8_t *rom = malloc(GC_IPL_ROM_SIZE);
    GcAudio *audio = calloc(1, sizeof(*audio));
    assert(rom && audio);
    const size_t table = 0xf7dc0;
    const uint16_t gains[] = {0, 1, 0xffff};
    for (unsigned channel = 0; channel < 2; ++channel) {
        for (unsigned bus = 3; bus <= 6; ++bus) {
            for (unsigned index = 0; index < 3; ++index) {
                memcpy(rom, original, GC_IPL_ROM_SIZE);
                memset(audio, 0, sizeof(*audio));
                /* Mutate the final descriptor after earlier resources exist.
                 * Serialized buses are zero-based; runtime zero is disabled.
                 */
                uint8_t *descriptor = rom + table + 2 * 32;
                write_halfword(descriptor + 2 + channel * 4, (uint16_t)(bus - 1));
                write_halfword(descriptor + 4 + channel * 4, gains[index]);
                bool decoded = gc_audio_resources_decode(audio, rom, GC_IPL_ROM_SIZE);
                assert(decoded == (gains[index] == 0));
                if (decoded) {
                    assert(audio->effects[2].return_bus[channel] == bus);
                    assert(audio->effects[2].return_gain[channel] == 0);
                } else {
                    assert(audio->wave_count > 1 && audio->waves[0].samples);
                }
                assert_released(audio);
                assert_released(audio);
            }
        }
    }
    free(audio);
    free(rom);
}

int main(int argc, char **argv) {
    assert(!gc_audio_create(NULL, 48000));
    assert(!gc_audio_create("unused", 7999));
    assert(!gc_audio_create("unused", 192001));
    assert(!gc_audio_resources_decode(NULL, NULL, GC_IPL_ROM_SIZE));
    assert(argc == 1 || argc == 2);
    if (argc == 2) {
        uint8_t *rom = NULL;
        GcAudio *audio = calloc(1, sizeof(*audio));
        assert(audio && gc_ipl_rom_read(argv[1], &rom));
        assert(gc_audio_resources_decode(audio, rom, GC_IPL_ROM_SIZE));
        unsigned revision = audio->sequence_revision;
        for (unsigned index = 0; index < 4; ++index) {
            for (unsigned channel = 0; channel < 2; ++channel) {
                unsigned bus = audio->effects[index].return_bus[channel];
                assert(!audio->effects[index].return_gain[channel] || bus < 3 ||
                       bus > 6);
            }
        }
        assert_released(audio);
        free(audio);
        test_rates(rom);
        test_effect_aliases(rom, revision);
        free(rom);
    }
    puts("Audio resource tests passed.");
    return 0;
}
