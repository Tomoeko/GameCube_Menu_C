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
/* Atomic host queries are safe while the audio device is rendering. */
unsigned gc_audio_active_voices(const GcAudio *audio);
bool gc_audio_sequence_stopped(const GcAudio *audio);
void gc_audio_set_mono(GcAudio *audio, bool mono);

/* Interleaved stereo float samples, overwritten and clamped to [-1, 1]. */
void gc_audio_render(GcAudio *audio, float *stereo, size_t frames);
/* Query on the rendering thread or while the device is stopped. */
void gc_audio_info(const GcAudio *audio, GcAudioInfo *info);

/* IPL AFC codec: 9-byte frames, 16 samples, signed four-bit residuals.
 * History is most recent first and is updated on success. This low-level
 * operation accepts complete frames and is useful for independent validation.
 */
bool gc_audio_afc_decode(const uint8_t *encoded, size_t encoded_size, int16_t *samples,
                         size_t sample_capacity, int16_t history[2]);

/* Native Apple AudioQueue host. Other hosts may call gc_audio_render directly. */
bool gc_audio_device_start(GcAudio *audio);
void gc_audio_device_stop(GcAudio *audio);

#endif
