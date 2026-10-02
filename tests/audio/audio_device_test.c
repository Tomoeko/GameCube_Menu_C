#include "audio/audio_internal.h"

#include <AudioToolbox/AudioToolbox.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* First-party fault injection uses the platform's public types and the real
 * device module. It neither opens hardware nor replaces DSP behavior.
 */
static float sample_data[3][1024];
static AudioQueueBuffer buffers[3] = {
    {.mAudioDataBytesCapacity = 4096, .mAudioData = sample_data[0]},
    {.mAudioDataBytesCapacity = 4096, .mAudioData = sample_data[1]},
    {.mAudioDataBytesCapacity = 4096, .mAudioData = sample_data[2]},
};

typedef struct {
    AudioQueueOutputCallback callback;
    void *context;
    unsigned identity;
    unsigned allocations;
    unsigned buffers;
    unsigned enqueues;
    unsigned starts;
    unsigned stops;
    unsigned disposals;
    unsigned rendered;
    unsigned fail_buffer;
    unsigned fail_enqueue;
    bool fail_owner;
    bool fail_new;
    bool fail_start;
    bool callback_on_start;
} DeviceFixture;

static DeviceFixture fixture;

static AudioQueueRef fixture_queue(void) {
    return (AudioQueueRef)&fixture.identity;
}

static void *device_allocate(size_t count, size_t size) {
    return fixture.fail_owner ? NULL : calloc(count, size);
}

static OSStatus queue_new(const AudioStreamBasicDescription *format,
                          AudioQueueOutputCallback callback, void *context,
                          CFRunLoopRef loop, CFStringRef mode, UInt32 flags,
                          AudioQueueRef *output) {
    assert(format->mSampleRate == 48000 && !loop && !mode && !flags);
    assert(format->mChannelsPerFrame == 2 && format->mBytesPerFrame == 8);
    assert(format->mFormatID == kAudioFormatLinearPCM);
    if (fixture.fail_new)
        return -1;
    fixture.callback = callback;
    fixture.context = context;
    *output = fixture_queue();
    return noErr;
}

static OSStatus queue_allocate(AudioQueueRef queue, UInt32 bytes,
                               AudioQueueBufferRef *output) {
    assert(queue == fixture_queue() && bytes == 4096);
    ++fixture.allocations;
    if (fixture.allocations == fixture.fail_buffer)
        return -1;
    assert(fixture.buffers < 3);
    *output = &buffers[fixture.buffers++];
    return noErr;
}

static OSStatus queue_enqueue(AudioQueueRef queue, AudioQueueBufferRef buffer,
                              UInt32 packets,
                              const AudioStreamPacketDescription *descriptions) {
    assert(queue == fixture_queue() && buffer && !packets && !descriptions);
    assert(buffer->mAudioDataByteSize == 4096);
    ++fixture.enqueues;
    return fixture.enqueues == fixture.fail_enqueue ? -1 : noErr;
}

static OSStatus queue_start(AudioQueueRef queue, const AudioTimeStamp *timestamp) {
    assert(queue == fixture_queue() && !timestamp);
    ++fixture.starts;
    if (fixture.callback_on_start)
        fixture.callback(fixture.context, queue, &buffers[0]);
    return fixture.fail_start ? -1 : noErr;
}

static void pending_callbacks(AudioQueueRef queue) {
    for (unsigned index = 0; index < fixture.buffers; ++index)
        fixture.callback(fixture.context, queue, &buffers[index]);
}

static OSStatus queue_dispose(AudioQueueRef queue, Boolean immediate) {
    assert(queue == fixture_queue() && immediate);
    ++fixture.disposals;
    pending_callbacks(queue);
    return noErr;
}

static OSStatus queue_stop(AudioQueueRef queue, Boolean immediate) {
    assert(queue == fixture_queue() && immediate);
    ++fixture.stops;
    /* Immediate platform stops normally invoke pending callbacks. */
    pending_callbacks(queue);
    return noErr;
}

void gc_audio_render(GcAudio *audio, float *stereo, size_t frames) {
    assert(audio && stereo && frames == 512);
    fixture.rendered += (unsigned)frames;
    memset(stereo, 0, frames * 2 * sizeof(*stereo));
}

#define calloc device_allocate
#define AudioQueueNewOutput queue_new
#define AudioQueueAllocateBuffer queue_allocate
#define AudioQueueEnqueueBuffer queue_enqueue
#define AudioQueueStart queue_start
#define AudioQueueStop queue_stop
#define AudioQueueDispose queue_dispose
#include "../../src/platform/apple/audio_apple.c"

static void test_setup_failures(void) {
    assert(!gc_audio_device_start(NULL));
    for (unsigned failure = 0; failure < 9; ++failure) {
        fixture = (DeviceFixture){0};
        GcAudio audio = {.sample_rate = 48000};
        if (failure == 0)
            fixture.fail_owner = true;
        else if (failure == 1)
            fixture.fail_new = true;
        else if (failure < 5)
            fixture.fail_buffer = failure - 1;
        else if (failure < 8)
            fixture.fail_enqueue = failure - 4;
        else
            fixture.fail_start = true;
        assert(!gc_audio_device_start(&audio) && !audio.device);
        unsigned expected = failure < 3   ? 0
                            : failure < 5 ? (failure - 2) * 512
                            : failure < 8 ? (failure - 4) * 512
                                          : 1536;
        assert(fixture.rendered == expected);
        assert(fixture.disposals == (failure > 1 ? 1u : 0u));
        assert(fixture.starts == (failure == 8 ? 1u : 0u));
        gc_audio_device_stop(&audio);
        assert(!fixture.stops);
    }
}

static void test_callbacks(bool fail) {
    fixture = (DeviceFixture){0};
    GcAudio audio = {.sample_rate = 48000};
    assert(gc_audio_device_start(&audio) && audio.device);
    assert(fixture.rendered == 1536 && fixture.enqueues == 3 && fixture.starts == 1);
    assert(gc_audio_device_start(&audio));
    assert(fixture.allocations == 3 && fixture.rendered == 1536);
    if (fail)
        fixture.fail_enqueue = 4;
    fixture.callback(fixture.context, fixture_queue(), &buffers[0]);
    assert(fixture.rendered == 2048 && fixture.enqueues == 4);
    assert(gc_audio_device_start(&audio) == !fail);
    if (fail) {
        pending_callbacks(fixture_queue());
        assert(fixture.rendered == 2048 && fixture.enqueues == 4);
    }
    gc_audio_device_stop(&audio);
    assert(!audio.device && fixture.stops == 1 && fixture.disposals == 1);
    assert(fixture.rendered == 2048 && fixture.enqueues == 4);
    gc_audio_device_stop(&audio);
    assert(fixture.stops == 1 && fixture.disposals == 1);
}

static void test_start_callback_failure(void) {
    fixture = (DeviceFixture){.fail_enqueue = 4, .callback_on_start = true};
    GcAudio audio = {.sample_rate = 48000};
    assert(!gc_audio_device_start(&audio) && !audio.device);
    assert(fixture.rendered == 2048 && fixture.enqueues == 4);
    assert(fixture.starts == 1 && fixture.disposals == 1);
}

int main(void) {
    test_setup_failures();
    test_callbacks(false);
    test_callbacks(true);
    test_start_callback_failure();
    puts("Audio device fault tests passed.");
    return 0;
}
