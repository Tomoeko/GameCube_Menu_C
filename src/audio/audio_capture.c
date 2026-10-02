#include "audio_internal.h"
#include "console_common/capture/audio_buffer.h"

bool gc_audio_capture_begin(GcAudio *audio, size_t capacity_frames) {
    if (!audio || audio->device || audio->capture)
        return false;
    audio->capture = cc_audio_buffer_create(capacity_frames);
    return audio->capture != NULL;
}

void gc_audio_capture_output(GcAudio *audio, const float *stereo, size_t frames) {
    cc_audio_buffer_write(audio->capture, stereo, frames);
}

size_t gc_audio_capture_read(GcAudio *audio, float *stereo, size_t capacity_frames,
                             uint64_t *first_sample_index) {
    return audio ? cc_audio_buffer_read(audio->capture, stereo, capacity_frames,
                                        first_sample_index)
                 : 0;
}

bool gc_audio_capture_failed(const GcAudio *audio) {
    return audio && cc_audio_buffer_failed(audio->capture);
}

void gc_audio_capture_end(GcAudio *audio) {
    if (!audio || !audio->capture)
        return;
    gc_audio_device_stop(audio);
    cc_audio_buffer_destroy(audio->capture);
    audio->capture = NULL;
}
