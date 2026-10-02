#include "audio/audio_internal.h"

#include <AudioToolbox/AudioToolbox.h>
#include <stdlib.h>

#define GC_AUDIO_BUFFER_FRAMES 512
#define GC_AUDIO_BUFFERS 3

typedef struct {
    AudioQueueRef queue;
    AudioQueueBufferRef buffers[GC_AUDIO_BUFFERS];
    GcAudio *audio;
} GcAudioDevice;

static void refill(void *context, AudioQueueRef queue, AudioQueueBufferRef buffer) {
    GcAudioDevice *device = context;
    gc_audio_render(device->audio, buffer->mAudioData, GC_AUDIO_BUFFER_FRAMES);
    buffer->mAudioDataByteSize = GC_AUDIO_BUFFER_FRAMES * 2 * sizeof(float);
    AudioQueueEnqueueBuffer(queue, buffer, 0, NULL);
}

bool gc_audio_device_start(GcAudio *audio) {
    if (!audio)
        return false;
    if (audio->device)
        return true;
    GcAudioDevice *device = calloc(1, sizeof(*device));
    if (!device)
        return false;
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
        refill(device, device->queue, device->buffers[i]);
    }
    if (AudioQueueStart(device->queue, NULL) != noErr)
        goto release_failed;
    audio->device = device;
    return true;

release_failed:
    if (device->queue)
        AudioQueueDispose(device->queue, true);
    free(device);
    return false;
}

void gc_audio_device_stop(GcAudio *audio) {
    if (!audio || !audio->device)
        return;
    GcAudioDevice *device = audio->device;
    AudioQueueStop(device->queue, true);
    AudioQueueDispose(device->queue, true);
    free(device);
    audio->device = NULL;
}
