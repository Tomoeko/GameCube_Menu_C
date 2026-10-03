#ifndef GAMECUBE_APP_RECORDING_H
#define GAMECUBE_APP_RECORDING_H

#include "console_common/capture/recording.h"
#include "gamecube/audio.h"

CcRecording *gc_recording_open(CcPlatform *platform, GcAudio *audio,
                               unsigned video_rate, bool audible, bool half_size);
CcRecording *gc_recording_open_with_audio(CcPlatform *platform, GcAudio *audio,
                                          unsigned video_rate, bool audible,
                                          bool half_size, CcCaptureAudioMode mode);
CcRecording *gc_recording_open_path(CcPlatform *platform, GcAudio *audio,
                                    unsigned video_rate, bool audible, bool half_size,
                                    const char *path);

#endif
