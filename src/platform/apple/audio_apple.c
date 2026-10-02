#include "audio/audio_internal.h"

#include <AudioToolbox/AudioToolbox.h>
#include <stdlib.h>

#define GC_AUDIO_BUFFER_FRAMES 512
#define GC_AUDIO_BUFFERS 3

typedef struct {
    AudioQueueRef queue;
    AudioQueueBufferRef buffers[GC_AUDIO_BUFFERS];
    GcAudio *audio;
    atomic_bool stopping;
    atomic_bool failed;
} GcAudioDevice;

static bool device_available(const GcAudioDevice *device) {
    return !atomic_load_explicit(&device->stopping, memory_order_acquire) &&
           !atomic_load_explicit(&device->failed, memory_order_acquire);
}

static bool refill_buffer(GcAudioDevice *device, AudioQueueRef queue,
                          AudioQueueBufferRef buffer) {
    if (!device_available(device))
        return false;
    gc_audio_render(device->audio, buffer->mAudioData, GC_AUDIO_BUFFER_FRAMES);
    if (!device_available(device))
        return false;
    buffer->mAudioDataByteSize = GC_AUDIO_BUFFER_FRAMES * 2 * sizeof(float);
    if (AudioQueueEnqueueBuffer(queue, buffer, 0, NULL) != noErr) {
        atomic_store_explicit(&device->failed, true, memory_order_release);
        return false;
    }
    return true;
}

static void refill(void *context, AudioQueueRef queue, AudioQueueBufferRef buffer) {
    (void)refill_buffer(context, queue, buffer);
}

bool gc_audio_device_start(GcAudio *audio) {
    if (!audio)
        return false;
    if (audio->device)
        return device_available(audio->device);
    GcAudioDevice *device = calloc(1, sizeof(*device));
    if (!device)
        return false;
    atomic_init(&device->stopping, false);
    atomic_init(&device->failed, false);
    device->audio = audio;
    AudioStreamBasicDescription format = {0};
    format.mSampleRate = audio->sample_rate;
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kAudioFormatFlagsNativeFloatPacked;
    format.mBytesPerPacket = 2 * sizeof(float);
    format.mFramesPerPacket = 1;
    format.mBytesPerFrame = 2 * sizeof(float);
    format.mChannelsPerFrame = 2;
    format.mBitsPerChannel = 32;
    if (AudioQueueNewOutput(&format, refill, device, NULL, NULL, 0, &device->queue) !=
        noErr)
        goto release_failed;
    for (unsigned i = 0; i < GC_AUDIO_BUFFERS; ++i) {
        if (AudioQueueAllocateBuffer(device->queue,
                                     GC_AUDIO_BUFFER_FRAMES * 2 * sizeof(float),
                                     &device->buffers[i]) != noErr)
            goto release_failed;
        if (!refill_buffer(device, device->queue, device->buffers[i]))
            goto release_failed;
    }
    if (AudioQueueStart(device->queue, NULL) != noErr || !device_available(device))
        goto release_failed;
    audio->device = device;
    return true;

release_failed:
    atomic_store_explicit(&device->stopping, true, memory_order_release);
    if (device->queue)
        AudioQueueDispose(device->queue, true);
    free(device);
    return false;
}

void gc_audio_device_stop(GcAudio *audio) {
    if (!audio || !audio->device)
        return;
    GcAudioDevice *device = audio->device;
    /* Immediate stop/dispose may invoke pending callbacks synchronously. */
    atomic_store_explicit(&device->stopping, true, memory_order_release);
    AudioQueueStop(device->queue, true);
    AudioQueueDispose(device->queue, true);
    free(device);
    audio->device = NULL;
}
