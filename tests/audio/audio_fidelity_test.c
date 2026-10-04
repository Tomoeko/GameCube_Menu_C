#include "audio/audio_internal.h"
#include "console_common/support/endian.h"
#include "console_common/support/sha1.h"
#include "gamecube/ipl.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *region;
    unsigned revision;
    const char *decoded_ipl_sha1;
    const char *pcm_sha1[4];
} AudioBaseline;

/* Retained C baseline 43aaebb: 64057/2 native frames per second, stereo
 * signed16 samples in big-endian order, startup at frame zero and menu
 * begin at frame 256228. These checks guard the restored C presentation;
 * no original samples are stored. Input hashes identify resource versions.
 */
static const AudioBaseline baselines[] = {
    {.region = "USA",
     .revision = 0,
     .decoded_ipl_sha1 = "9e681ce3bf49efd4db1cd5a6747accfb39a8ebab",
     .pcm_sha1 = {"1d2bcee513ed9209b4b52dc2e033e1be7ff0a668",
                  "9ae7cf4baae995949e5574c3364a15255baa216e",
                  "bd321751afee9e23e470f130280c5b80636bee99",
                  "ec5867c4110290f53dcf4ff0304a4386d8115152"}},
    {.region = "EUR",
     .revision = 1,
     .decoded_ipl_sha1 = "9909490c6b2ac8428c8d347c3967aa8a930ee058",
     .pcm_sha1 = {"1d2bcee513ed9209b4b52dc2e033e1be7ff0a668",
                  "b3aba4ce795e2171880653f19a492d9632bc0890",
                  "d2cdeb889d22b00a6ab4f9e87c972e2359522a40",
                  "dfc62f39bdc4ecdd250773ff87ec14f2b71158c4"}},
    {.region = "JAP",
     .revision = 0,
     .decoded_ipl_sha1 = "31012739271dcad2f495006265cf0fa292459f39",
     .pcm_sha1 = {"1d2bcee513ed9209b4b52dc2e033e1be7ff0a668",
                  "9ae7cf4baae995949e5574c3364a15255baa216e",
                  "bd321751afee9e23e470f130280c5b80636bee99",
                  "ec5867c4110290f53dcf4ff0304a4386d8115152"}},
};

static void digest_hex(const uint8_t digest[20], char hexadecimal[41]) {
    for (unsigned index = 0; index < 20; ++index)
        snprintf(hexadecimal + index * 2, 3, "%02x", digest[index]);
}

static const AudioBaseline *input_baseline(const char *path, const char *region) {
    uint8_t *rom = NULL;
    GcAudio *inspection = calloc(1, sizeof(*inspection));
    assert(inspection && gc_ipl_rom_read(path, &rom));
    assert(gc_audio_resources_decode(inspection, rom, GC_IPL_ROM_SIZE));
    CcSha1 sha1;
    uint8_t digest[20];
    cc_sha1_init(&sha1);
    cc_sha1_update(&sha1, rom, GC_IPL_ROM_SIZE);
    cc_sha1_final(&sha1, digest);
    char hexadecimal[41];
    digest_hex(digest, hexadecimal);
    const AudioBaseline *baseline = NULL;
    for (unsigned index = 0; index < sizeof(baselines) / sizeof(*baselines); ++index) {
        const AudioBaseline *candidate = &baselines[index];
        if (!strcmp(region, candidate->region) &&
            candidate->revision == inspection->sequence_revision &&
            !strcmp(hexadecimal, candidate->decoded_ipl_sha1)) {
            baseline = candidate;
            break;
        }
    }
    gc_audio_resources_release(inspection);
    free(inspection);
    free(rom);
    return baseline;
}

static void render_discarded_audio(GcAudio *audio, size_t frames) {
    float stereo[2048];
    while (frames) {
        size_t count = frames < 1024 ? frames : 1024;
        gc_audio_render(audio, stereo, count);
        frames -= count;
    }
}

static bool active_menu_music(const GcAudio *audio) {
    for (unsigned voice = 0; voice < GC_AUDIO_VOICES; ++voice) {
        if (audio->voices[voice].active && audio->voices[voice].menu_music)
            return true;
    }
    return false;
}

static void check_restored_mix(const char *path, const AudioBaseline *baseline,
                               bool stem_enabled) {
    GcAudio *audio = gc_audio_create(path, 48000);
    assert(audio && gc_audio_menu_volume(audio) == 100);
    if (!stem_enabled) {
        cc_audio_resample_state_destroy(audio->music_output_state);
        audio->music_output_state = NULL;
    }
    /* Exercise host controls and their reconstruction histories, then reset
     * at the restored original level. Native state must start identically.
     */
    assert(gc_audio_startup_sound(audio, 0));
    render_discarded_audio(audio, 8 * 48000);
    assert(gc_audio_menu_begin(audio));
    for (unsigned block = 0; block < 480 && !active_menu_music(audio); ++block)
        render_discarded_audio(audio, 1024);
    assert(active_menu_music(audio));
    const unsigned volumes[] = {0, GC_AUDIO_MENU_VOLUME_MAX, 100};
    for (unsigned index = 0; index < sizeof(volumes) / sizeof(*volumes); ++index) {
        assert(gc_audio_set_menu_volume(audio, volumes[index]));
        render_discarded_audio(audio, 48000);
    }
    gc_audio_reset(audio);
    assert(gc_audio_menu_volume(audio) == 100);
    assert(gc_audio_startup_sound(audio, 0));
    CcSha1 pcm;
    cc_sha1_init(&pcm);
    uint8_t pending[4096];
    size_t pending_count = 0;
    const unsigned seconds[] = {8, 16, 32, 128};
    unsigned checkpoint = 0;
    const size_t menu_begin = 8 * 64057 / 2;
    const size_t all_frames = 128 * 64057 / 2;
    for (size_t frame = 0; frame < all_frames; ++frame) {
        if (frame == menu_begin)
            assert(gc_audio_menu_begin(audio));
        float stereo[2];
        gc_audio_dsp_render_frame(audio, false, stereo);
        for (unsigned channel = 0; channel < 2; ++channel) {
            float sample = stereo[channel] * 32768;
            assert(isfinite(sample) && sample >= INT16_MIN && sample <= INT16_MAX);
            assert(sample == truncf(sample));
            cc_write_be16(pending + pending_count, (uint16_t)(int16_t)sample);
            pending_count += 2;
        }
        bool reached = frame + 1 == (size_t)seconds[checkpoint] * 64057 / 2;
        if (pending_count == sizeof(pending) || reached) {
            cc_sha1_update(&pcm, pending, pending_count);
            pending_count = 0;
        }
        if (reached) {
            CcSha1 snapshot = pcm;
            uint8_t digest[20];
            cc_sha1_final(&snapshot, digest);
            char hexadecimal[41];
            digest_hex(digest, hexadecimal);
            if (strcmp(hexadecimal, baseline->pcm_sha1[checkpoint])) {
                fprintf(stderr, "%s audio at %us: %s, expected %s\n", baseline->region,
                        seconds[checkpoint], hexadecimal,
                        baseline->pcm_sha1[checkpoint]);
                assert(false);
            }
            ++checkpoint;
        }
    }
    assert(checkpoint == 4 && !pending_count && !audio->rejected_commands);
    gc_audio_destroy(audio);
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fputs("Audio fidelity checks require a local IPL and region.\n", stderr);
        return 77;
    }
    const AudioBaseline *baseline = input_baseline(argv[1], argv[2]);
    if (!baseline) {
        fputs("Unsupported audio baseline input profile.\n", stderr);
        return 77;
    }
    check_restored_mix(argv[1], baseline, true);
    check_restored_mix(argv[1], baseline, false);
    printf("Restored %s audio baseline checks passed.\n", baseline->region);
    return 0;
}
