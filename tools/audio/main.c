#include "gamecube/audio.h"
#include "console_common/support/endian.h"
#include "output.h"
#include "option_values.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool write_le(FILE *file, uint32_t value, unsigned count) {
    uint8_t bytes[4];
    cc_write_le32(bytes, value);
    return count <= sizeof(bytes) && fwrite(bytes, 1, count, file) == count;
}

static bool write_header(FILE *file, unsigned sample_rate, uint32_t frames) {
    return fwrite("RIFF", 1, 4, file) == 4 && write_le(file, frames * 4 + 36, 4) &&
           fwrite("WAVEfmt ", 1, 8, file) == 8 && write_le(file, 16, 4) &&
           write_le(file, 1, 2) && write_le(file, 2, 2) &&
           write_le(file, sample_rate, 4) && write_le(file, sample_rate * 4, 4) &&
           write_le(file, 4, 2) && write_le(file, 16, 2) &&
           fwrite("data", 1, 4, file) == 4 && write_le(file, frames * 4, 4);
}

static bool number(const char *text, unsigned minimum, unsigned maximum,
                   unsigned *value) {
    unsigned long parsed;
    if (!gc_option_unsigned(text, 0, minimum, maximum, &parsed))
        return false;
    *value = (unsigned)parsed;
    return true;
}

int main(int argc, char **argv) {
    const char *ipl = NULL;
    const char *output = NULL;
    unsigned seconds = 10;
    unsigned rate = 48000;
    unsigned event = 0;
    for (int i = 1; i < argc; ++i) {
        if (i + 1 == argc)
            goto usage;
        const char *argument = argv[++i];
        if (!strcmp(argv[i - 1], "--ipl"))
            ipl = argument;
        else if (!strcmp(argv[i - 1], "--output"))
            output = argument;
        else if (!strcmp(argv[i - 1], "--seconds")) {
            if (!number(argument, 1, 600, &seconds))
                goto usage;
        } else if (!strcmp(argv[i - 1], "--rate")) {
            if (!number(argument, 8000, 192000, &rate))
                goto usage;
        } else if (!strcmp(argv[i - 1], "--event")) {
            if (!number(argument, 0, 255, &event))
                goto usage;
        } else if (!strcmp(argv[i - 1], "--startup")) {
            if (!number(argument, 0, 2, &event))
                goto usage;
        } else {
            goto usage;
        }
    }
    if (!ipl || !output)
        goto usage;
    GcAudio *audio = gc_audio_create(ipl, rate);
    if (!audio) {
        fputs("IPL audio bank could not be decoded.\n", stderr);
        return 1;
    }
    FILE *file = gc_tool_output_open(output);
    if (!file) {
        gc_audio_destroy(audio);
        fputs("Audio output could not be opened.\n", stderr);
        return 1;
    }
    uint32_t frames = seconds * rate;
    bool okay = write_header(file, rate, frames) && gc_audio_event(audio, event);
    float samples[1024];
    for (uint32_t start = 0; okay && start < frames; start += 512) {
        size_t count = frames - start < 512 ? frames - start : 512;
        gc_audio_render(audio, samples, count);
        for (size_t i = 0; okay && i < count * 2; ++i) {
            /* DSP samples use signed PCM / 32768. Preserve their integer
             * values while clamping the positive endpoint of host floats. */
            float sample = fmaxf(-1, fminf(1, samples[i]));
            int32_t pcm = (int32_t)lrintf(sample * 32768.0f);
            if (pcm > INT16_MAX)
                pcm = INT16_MAX;
            okay = write_le(file, (uint16_t)pcm, 2);
        }
    }
    if (fclose(file))
        okay = false;
    GcAudioInfo info = {0};
    gc_audio_info(audio, &info);
    printf("waves=%u instruments=%u ticks=%llu notes=%llu rejected=%u\n",
           info.wave_count, info.instrument_count,
           (unsigned long long)info.sequence_ticks,
           (unsigned long long)info.notes_started, info.rejected_commands);
    gc_audio_destroy(audio);
    return okay && !info.rejected_commands ? 0 : 1;

usage:
    fputs("Usage: gc-audio-tool --ipl Files/path/IPL.bin --output Files/audio.wav "
          "[--seconds N] [--rate Hz] [--startup 0|1|2] [--event ID]\n",
          stderr);
    return 2;
}
