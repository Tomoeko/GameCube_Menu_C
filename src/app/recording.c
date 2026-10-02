#include "recording.h"

static bool capture_begin(void *context, size_t capacity) {
    return gc_audio_capture_begin(context, capacity);
}

static size_t capture_read(void *context, float *stereo, size_t capacity) {
    return gc_audio_capture_read(context, stereo, capacity, NULL);
}

static bool capture_failed(void *context) {
    return gc_audio_capture_failed(context);
}

static void capture_end(void *context) {
    gc_audio_capture_end(context);
}

static CcRecordingOptions recording_options(CcPlatform *platform, GcAudio *audio,
                                            unsigned video_rate, bool audible,
                                            bool half_size) {
    CcRecordingOptions options = {.platform = platform,
                                  .sample_rate =
                                      audible ? gc_audio_sample_rate(audio) : 48000,
                                  .video_rate = video_rate,
                                  .half_size = half_size,
                                  .filename_prefix = "GameCube"};
    if (audible) {
        options.audio = (CcRecordingAudioSource){.context = audio,
                                                 .begin = capture_begin,
                                                 .read = capture_read,
                                                 .failed = capture_failed,
                                                 .end = capture_end};
    }
    return options;
}

CcRecording *gc_recording_open(CcPlatform *platform, GcAudio *audio,
                               unsigned video_rate, bool audible, bool half_size) {
    if (!audio)
        return NULL;
    CcRecordingOptions options =
        recording_options(platform, audio, video_rate, audible, half_size);
    return cc_recording_open(&options);
}

CcRecording *gc_recording_open_path(CcPlatform *platform, GcAudio *audio,
                                    unsigned video_rate, bool audible, bool half_size,
                                    const char *path) {
    if (!audio)
        return NULL;
    CcRecordingOptions options =
        recording_options(platform, audio, video_rate, audible, half_size);
    return cc_recording_open_path(&options, path);
}
