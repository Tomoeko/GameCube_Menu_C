#ifndef GAMECUBE_AUDIO_H
#define GAMECUBE_AUDIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct GcAudio GcAudio;

typedef struct {
    unsigned wave_count;
    unsigned instrument_count;
    uint64_t sequence_ticks;
    uint64_t notes_started;
    uint32_t rejected_commands;
    uint32_t dropped_events;
    unsigned active_voices;
    bool sequence_stopped;
} GcAudioInfo;

/* Original IPL data is read locally; no samples are embedded in the program.
 * Both an encrypted complete ROM and a descrambled complete ROM are accepted.
 * The sample rate is the host output rate, between 8000 and 192000 Hz.
 */
GcAudio *gc_audio_create(const char *ipl_path, unsigned sample_rate);
void gc_audio_destroy(GcAudio *audio);
/* Stop the host device and restart the mixer/sequence with retained samples.
 * Call on the host thread; reopen the device when playback should continue. */
void gc_audio_reset(GcAudio *audio);

/* Native sub_813594e0 event numbers. These queue the recovered track ports.
 * One producer may queue events while one consumer calls gc_audio_render.
 */
bool gc_audio_event(GcAudio *audio, unsigned native_event);
bool gc_audio_startup_sound(GcAudio *audio, unsigned selector);
/* Native sub_81359340 modulation: direction 0 or 1, fraction in [0, 1]. */
bool gc_audio_cube_motion(GcAudio *audio, unsigned direction, float fraction);
/* Native menu background start and fade, from sub_81359680/sub_81359640. */
bool gc_audio_menu_begin(GcAudio *audio);
bool gc_audio_menu_end(GcAudio *audio);
/* Native sub_81357980 stops the sequence and releases its channels. Queued
 * like the event APIs; envelope and effect tails continue to render.
 */
bool gc_audio_stop_sequence(GcAudio *audio);
/* The immutable rate and atomic host queries are safe while rendering. */
unsigned gc_audio_sample_rate(const GcAudio *audio);
unsigned gc_audio_active_voices(const GcAudio *audio);
bool gc_audio_sequence_stopped(const GcAudio *audio);
void gc_audio_set_mono(GcAudio *audio, bool mono);

/* Interleaved stereo float samples, overwritten and clamped to [-1, 1]. */
void gc_audio_render(GcAudio *audio, float *stereo, size_t frames);
/* Query on the rendering thread or while the device is stopped. */
void gc_audio_info(const GcAudio *audio, GcAudioInfo *info);

/* Capture the exact interleaved host output without file I/O on the rendering
 * thread. Begin while rendering is stopped, then use one host-thread reader.
 * The bounded queue fails explicitly on overflow; previously queued samples
 * remain readable. Sample indices start at zero and survive gc_audio_reset.
 * End stops the device before freeing the queue; do not read concurrently.
 */
bool gc_audio_capture_begin(GcAudio *audio, size_t capacity_frames);
size_t gc_audio_capture_read(GcAudio *audio, float *stereo, size_t capacity_frames,
                             uint64_t *first_sample_index);
bool gc_audio_capture_failed(const GcAudio *audio);
void gc_audio_capture_end(GcAudio *audio);

/* IPL AFC codec: 9-byte frames, 16 samples, signed four-bit residuals.
 * History is most recent first and is updated on success. This low-level
 * operation accepts complete frames and is useful for independent validation.
 */
bool gc_audio_afc_decode(const uint8_t *encoded, size_t encoded_size, int16_t *samples,
                         size_t sample_capacity, int16_t history[2]);

/* Shared host output; offline consumers may call gc_audio_render directly. */
bool gc_audio_device_start(GcAudio *audio);
void gc_audio_device_stop(GcAudio *audio);

#endif
