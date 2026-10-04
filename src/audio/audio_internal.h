#ifndef GAMECUBE_AUDIO_INTERNAL_H
#define GAMECUBE_AUDIO_INTERNAL_H

#include "gamecube/audio.h"
#include "console_common/audio/resampler.h"

#include <stdatomic.h>

typedef struct CcAudioBuffer CcAudioBuffer;

#define GC_AUDIO_WAVES 32
#define GC_AUDIO_CHILD_TRACKS 256
#define GC_AUDIO_TRACKS (GC_AUDIO_CHILD_TRACKS + 1)
#define GC_AUDIO_VOICES 64
#define GC_AUDIO_REGISTERS 64
#define GC_AUDIO_SEQUENCE_LIMIT 16384
#define GC_AUDIO_EVENT_QUEUE 64
#define GC_AUDIO_ENVELOPE_STEPS 16
#define GC_AUDIO_OSCILLATORS 4
#define GC_AUDIO_NOTE_SLOTS 8
#define GC_AUDIO_TABLE_BANK UINT32_C(0x10000000)
#define GC_AUDIO_TABLE_SEQUENCE UINT32_C(0x20000000)
#define GC_AUDIO_TABLE_TEMPLATE UINT32_C(0x30000000)
#define GC_AUDIO_TABLE_LOCAL UINT32_C(0x40000000)
#define GC_AUDIO_TABLE_OFFSET_MASK UINT32_C(0x0fffffff)
#define GC_AUDIO_DSP_RATE 32028.5
#define GC_AUDIO_DSP_QUANTUM 80.0
#define GC_AUDIO_DSP_PITCH_SCALE 4096.0
#define GC_AUDIO_EFFECT_SAMPLES 8000

typedef struct {
    uint16_t curve;
    uint16_t ticks;
    int16_t value;
} GcAudioEnvelopeStep;

typedef struct {
    GcAudioEnvelopeStep attack[4];
    GcAudioEnvelopeStep release[2];
} GcAudioLocalEnvelopeTables;

typedef struct {
    bool enabled;
    bool release_continues;
    bool null_release_quick;
    bool scaled_duration;
    unsigned target;
    float rate;
    float scale;
    float offset;
    uint32_t attack_identity;
    uint32_t release_identity;
    unsigned attack_count;
    unsigned release_count;
    GcAudioEnvelopeStep attack[GC_AUDIO_ENVELOPE_STEPS];
    GcAudioEnvelopeStep release[GC_AUDIO_ENVELOPE_STEPS];
} GcAudioEnvelope;

typedef struct {
    GcAudioEnvelope envelope;
    unsigned pc;
    unsigned curve;
    float remaining;
    float length;
    float current;
    float target;
    float step;
    bool held;
    bool released;
    bool release_pending;
    bool ended;
    bool quick_release;
    bool quick_pending;
    bool forced_release;
} GcAudioEnvelopeState;

typedef struct {
    int16_t *samples;
    size_t count;
    float rate;
    unsigned key;
    unsigned id;
    bool loop;
    bool afc_source;
    size_t loop_start;
    size_t loop_end;
} GcAudioWave;

typedef struct {
    uint8_t key;
    uint8_t velocity;
    uint16_t wave;
    float volume;
    float pitch;
} GcAudioRegion;

typedef struct {
    unsigned region_count;
    GcAudioRegion regions[8];
    float volume;
    float pitch;
    GcAudioEnvelope envelopes[2];
} GcAudioInstrument;

typedef struct {
    int16_t current;
    int16_t product_multiplier;
    int32_t increment;
    int64_t accumulator;
    unsigned frame;
    bool initialized;
} GcAudioDspGain;

typedef struct {
    bool active;
    unsigned parent;
    unsigned children[16];
    unsigned note_voices[GC_AUDIO_NOTE_SLOTS];
    bool clear_note_on_wait;
    unsigned flags;
    uint32_t id;
    size_t pc;
    unsigned wait;
    size_t stack[16];
    unsigned repeats[16];
    unsigned depth;
    uint16_t registers[GC_AUDIO_REGISTERS];
    uint16_t ports[16];
    unsigned port_imported;
    unsigned port_exported;
    size_t interrupts[8];
    unsigned interrupt_mask;
    unsigned interrupt_pending;
    bool interrupt_active;
    size_t saved_pc;
    unsigned saved_wait;
    unsigned timer;
    unsigned timer_period;
    unsigned timer_count;
    int transpose;
    int previous_key;
    bool tie;
    unsigned timebase;
    unsigned time_mode;
    uint16_t routes[6];
    GcAudioEnvelope envelopes[2];
    GcAudioLocalEnvelopeTables local_envelope_tables;
    uint8_t envelope_modes[2];
    float parameters[17];
    float parameter_targets[17];
    float parameter_steps[17];
    unsigned parameter_ticks[17];
} GcAudioTrack;

typedef struct {
    bool active;
    bool detached;
    bool menu_music;
    bool on_release_list;
    unsigned track;
    unsigned owner_track;
    unsigned slot;
    unsigned wave;
    double position;
    double step;
    double base_step;
    float base_gain;
    float track_gain;
    float pan;
    float reverb;
    float track_pan;
    float track_reverb;
    float pan_weights[3];
    uint16_t routes[6];
    unsigned buses[6];
    unsigned duration;
    GcAudioEnvelopeState envelopes[GC_AUDIO_OSCILLATORS];
    uint8_t envelope_tracks[GC_AUDIO_OSCILLATORS];
    float envelope_volume;
    float envelope_pitch;
    float envelope_pan;
    bool released;
    bool block_started;
    bool end_pending;
    double block_pitch;
    GcAudioDspGain dsp_gains[6];
} GcAudioVoice;

typedef struct {
    uint16_t number;
    float value;
} GcAudioEvent;

typedef struct {
    unsigned mode;
    unsigned length;
    unsigned position;
    unsigned return_bus[2];
    int16_t return_gain[2];
    int16_t filter[8];
    int16_t history[8];
    int16_t delay[GC_AUDIO_EFFECT_SAMPLES];
} GcAudioEffect;

typedef struct {
    GcAudioEffect effects[4];
    int16_t chorus[160];
    uint32_t chorus_read;
    unsigned chorus_write;
    unsigned chorus_frame;
    int chorus_direction;
    int16_t surround_delay[80];
    float stereo[2];
} GcAudioMusicOutput;

struct GcAudio {
    unsigned sample_rate;
    unsigned tempo;
    unsigned timebase;
    double tick_fraction;
    unsigned update_samples;
    unsigned wave_count;
    unsigned instrument_count;
    GcAudioWave waves[GC_AUDIO_WAVES];
    GcAudioInstrument instruments[128];
    uint8_t *sequence;
    size_t sequence_size;
    unsigned sequence_revision;
    uint16_t master_gain;
    uint16_t output_gain;
    float semitone_ratios[128];
    float fractional_semitone_ratios[64];
    float route_sine_table[257];
    int16_t resampling_coefficients[64][4];
    CcAudioResampler *output_resampler;
    CcAudioResampleState *output_state;
    CcAudioResampleState *music_output_state;
    GcAudioMusicOutput music_output;
    bool render_mono;
    GcAudioEffect effects[4];
    int16_t chorus[160];
    uint32_t chorus_read;
    unsigned chorus_write;
    unsigned chorus_frame;
    int chorus_direction;
    int16_t surround_delay[80];
    unsigned dsp_frame;
    GcAudioTrack tracks[GC_AUDIO_TRACKS];
    unsigned free_tracks[GC_AUDIO_CHILD_TRACKS];
    bool track_available[GC_AUDIO_CHILD_TRACKS];
    unsigned free_track_read;
    unsigned free_track_write;
    unsigned free_track_count;
    GcAudioVoice voices[GC_AUDIO_VOICES];
    unsigned native_counter;
    GcAudioEvent pending_events[GC_AUDIO_EVENT_QUEUE];
    atomic_uint event_read;
    atomic_uint event_write;
    atomic_bool mono;
    /* -100..700 adjustment; zero preserves the original 100-percent level. */
    atomic_int menu_volume_adjustment;
    atomic_uint dropped_events;
    atomic_uint active_voices;
    atomic_bool sequence_stopped;
    uint64_t sequence_ticks;
    uint64_t notes_started;
    uint32_t rejected_commands;
    void *device;
    CcAudioBuffer *capture;
};

/* Loading owns decoded samples/sequence; the ROM is borrowed and may be
 * descrambled in place. Both paths require a zero-initialized audio owner.
 * Release partial resources after failure before releasing the owner.
 */
bool gc_audio_resources_decode(GcAudio *audio, uint8_t *rom, size_t size);
bool gc_audio_resources_load(GcAudio *audio, const char *ipl_path);
void gc_audio_resources_release(GcAudio *audio);
void gc_audio_dsp_render_frame(GcAudio *audio, bool mono, float output[2]);
void gc_audio_capture_output(GcAudio *audio, const float *stereo, size_t frames);
void gc_audio_dsp_voice_begin(GcAudio *audio, GcAudioVoice *voice, bool mono);

void gc_audio_sequence_init(GcAudio *audio);
void gc_audio_sequence_tick(GcAudio *audio);
void gc_audio_sequence_event(GcAudio *audio, unsigned event);
void gc_audio_sequence_cube(GcAudio *audio, unsigned direction, float fraction);
void gc_audio_voice_release(GcAudio *audio, GcAudioVoice *voice);
void gc_audio_envelope_start(GcAudioEnvelopeState *state,
                             const GcAudioEnvelope *envelope);
bool gc_audio_envelope_release(GcAudioEnvelopeState *state);
bool gc_audio_envelope_quick_release(GcAudioEnvelopeState *state);
float gc_audio_envelope_step(GcAudioEnvelopeState *state);
void gc_audio_voice_envelopes(GcAudioVoice *voice);
void gc_audio_voice_envelope_install(GcAudioVoice *voice, unsigned slot,
                                     const GcAudioEnvelope *envelope);
void gc_audio_sequence_envelopes(GcAudio *audio, GcAudioVoice *voice);
bool gc_audio_envelope_decode_steps(GcAudioEnvelopeStep *steps, unsigned *count,
                                    const uint8_t *data, size_t size, size_t offset);
bool gc_audio_envelope_decode_sequence_steps(GcAudioEnvelopeStep *steps,
                                             unsigned *count, const uint8_t *data,
                                             size_t size, size_t offset);
int16_t gc_audio_resample(const GcAudio *audio, const GcAudioWave *wave,
                          double position);
int16_t gc_audio_resample_pitch(const GcAudio *audio, const GcAudioWave *wave,
                                double position, double pitch);
int16_t gc_audio_dsp_saturate(int64_t sample);
int16_t gc_audio_dsp_wrap(int64_t sample);
int64_t gc_audio_dsp_shift(int64_t value, unsigned bits);
int64_t gc_audio_dsp_round(int64_t value, unsigned bits);
void gc_audio_dsp_gain_prepare(GcAudioDspGain *gain, int16_t target, bool revised);
int16_t gc_audio_dsp_gain_mix(GcAudioDspGain *gain, int16_t sample, int16_t bus,
                              bool revised);
void gc_audio_effects_begin(GcAudio *audio, int16_t buses[12]);
void gc_audio_effects_end(GcAudio *audio, int16_t buses[12]);
int16_t gc_audio_dsp_output(const GcAudio *audio, int16_t sample);
float gc_audio_route_scale(const GcAudio *audio, const GcAudioVoice *voice,
                           unsigned route, bool mono);
void gc_audio_route_table_init(GcAudio *audio);
float gc_audio_route_sine(const GcAudio *audio, float value);
float gc_audio_route_gain(const GcAudio *audio, const GcAudioVoice *voice,
                          unsigned route, bool mono);
float gc_audio_pitch_ratio(const GcAudio *audio, float semitones);
float gc_audio_bus_gain(const GcAudio *audio, float gain);

#endif
