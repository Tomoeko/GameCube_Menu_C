#include "audio_internal.h"
#include "console_common/audio/output.h"

static void render_output(void *context, float *stereo, size_t frames) {
    gc_audio_render(context, stereo, frames);
}

bool gc_audio_device_start(GcAudio *audio) {
    if (!audio)
        return false;
    if (audio->device)
        return cc_audio_output_start(audio->device);
    CcAudioOutputOptions options = {.sample_rate = audio->sample_rate,
                                    .render = render_output,
                                    .context = audio,
                                    .mode = CC_AUDIO_OUTPUT_BUFFERED};
    CcAudioOutput *device = cc_audio_output_open(&options);
    if (!device)
        return false;
    if (!cc_audio_output_start(device)) {
        cc_audio_output_close(device);
        return false;
    }
    audio->device = device;
    return true;
}

void gc_audio_device_stop(GcAudio *audio) {
    if (!audio || !audio->device)
        return;
    cc_audio_output_close(audio->device);
    audio->device = NULL;
}
