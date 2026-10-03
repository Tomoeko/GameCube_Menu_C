#include "audio/audio_internal.h"
#include "console_common/audio/output.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

struct CcAudioOutput {
    CcAudioOutputOptions options;
    bool running;
};

typedef struct {
    CcAudioOutput device;
    unsigned opens;
    unsigned starts;
    unsigned closes;
    size_t rendered;
    bool fail_open;
    bool fail_start;
} DeviceFixture;

static DeviceFixture fixture;

CcAudioOutput *cc_audio_output_open(const CcAudioOutputOptions *options) {
    assert(options && options->sample_rate == 48000);
    assert(options->mode == CC_AUDIO_OUTPUT_BUFFERED);
    assert(options->render && options->context);
    ++fixture.opens;
    if (fixture.fail_open)
        return NULL;
    fixture.device.options = *options;
    return &fixture.device;
}

bool cc_audio_output_start(CcAudioOutput *device) {
    assert(device == &fixture.device);
    ++fixture.starts;
    if (fixture.fail_start)
        return false;
    if (!device->running) {
        float stereo[1024];
        for (unsigned index = 0; index < 3; ++index) {
            device->options.render(device->options.context, stereo, 512);
            assert(stereo[0] == 0.25f && stereo[1] == -0.5f);
        }
        device->running = true;
    }
    return true;
}

void cc_audio_output_close(CcAudioOutput *device) {
    if (!device)
        return;
    assert(device == &fixture.device);
    ++fixture.closes;
    device->running = false;
}

void gc_audio_render(GcAudio *audio, float *stereo, size_t frames) {
    assert(audio == fixture.device.options.context);
    assert(stereo && frames == 512);
    fixture.rendered += frames;
    memset(stereo, 0, frames * 2 * sizeof(*stereo));
    stereo[0] = 0.25f;
    stereo[1] = -0.5f;
}

static void test_failed_start(bool fail_open) {
    fixture = (DeviceFixture){0};
    fixture.fail_open = fail_open;
    fixture.fail_start = !fail_open;
    GcAudio audio = {.sample_rate = 48000};
    assert(!gc_audio_device_start(&audio));
    assert(!audio.device && fixture.opens == 1 && !fixture.rendered);
    assert(fixture.closes == (fail_open ? 0u : 1u));
    gc_audio_device_stop(&audio);
    assert(fixture.closes == (fail_open ? 0u : 1u));
}

static void test_restart(void) {
    fixture = (DeviceFixture){0};
    GcAudio audio = {.sample_rate = 48000};
    assert(gc_audio_device_start(&audio));
    assert(audio.device && fixture.opens == 1 && fixture.rendered == 1536);
    assert(gc_audio_device_start(&audio));
    assert(fixture.opens == 1 && fixture.rendered == 1536);
    fixture.fail_start = true;
    assert(!gc_audio_device_start(&audio));
    assert(audio.device && fixture.opens == 1);
    gc_audio_device_stop(&audio);
    assert(!audio.device && fixture.closes == 1 && !fixture.device.running);
    gc_audio_device_stop(&audio);
    assert(fixture.closes == 1);
    fixture.fail_start = false;
    assert(gc_audio_device_start(&audio));
    assert(fixture.opens == 2 && fixture.rendered == 3072);
    gc_audio_device_stop(&audio);
    assert(fixture.closes == 2);
}

int main(void) {
    assert(!gc_audio_device_start(NULL));
    gc_audio_device_stop(NULL);
    test_failed_start(true);
    test_failed_start(false);
    test_restart();
    puts("Shared audio device adapter tests passed.");
    return 0;
}
