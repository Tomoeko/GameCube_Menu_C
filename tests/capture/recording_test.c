#define _POSIX_C_SOURCE 200809L

#include "audio/audio_internal.h"
#include "app/recording.h"

#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    const uint8_t *bytes;
    size_t size;
    size_t header;
} TestBox;

typedef struct {
    uint64_t video_ticks;
    uint32_t video_frames;
    uint32_t audio_frames;
    unsigned audio_rate;
    uint32_t *audio_words;
} TestMovie;

static uint32_t read_u32(const uint8_t *bytes) {
    return (uint32_t)bytes[0] << 24 | (uint32_t)bytes[1] << 16 |
           (uint32_t)bytes[2] << 8 | bytes[3];
}

static uint64_t read_u64(const uint8_t *bytes) {
    return (uint64_t)read_u32(bytes) << 32 | read_u32(bytes + 4);
}

static TestBox find_box(const uint8_t *bytes, size_t size, const char type[4]) {
    size_t offset = 0;
    while (size - offset >= 8) {
        uint64_t length = read_u32(bytes + offset);
        size_t header = 8;
        if (length == 1) {
            assert(size - offset >= 16);
            length = read_u64(bytes + offset + 8);
            header = 16;
        }
        assert(length >= header && length <= size - offset);
        if (!memcmp(bytes + offset + 4, type, 4))
            return (TestBox){bytes + offset, (size_t)length, header};
        offset += (size_t)length;
    }
    return (TestBox){0};
}

static TestBox child_box(TestBox parent, const char type[4]) {
    assert(parent.bytes && parent.size >= parent.header);
    TestBox child =
        find_box(parent.bytes + parent.header, parent.size - parent.header, type);
    assert(child.bytes);
    return child;
}

static uint8_t *read_file(const char *path, size_t *size) {
    FILE *file = fopen(path, "rb");
    assert(file && fseek(file, 0, SEEK_END) == 0);
    long length = ftell(file);
    assert(length > 0 && fseek(file, 0, SEEK_SET) == 0);
    uint8_t *bytes = malloc((size_t)length);
    assert(bytes && fread(bytes, 1, (size_t)length, file) == (size_t)length);
    assert(fclose(file) == 0);
    *size = (size_t)length;
    return bytes;
}

static void read_audio_chunks(TestMovie *movie, TestBox table, const uint8_t *file,
                              size_t file_size) {
    TestBox sizes = child_box(table, "stsz");
    TestBox chunks = child_box(table, "co64");
    TestBox layout = child_box(table, "stsc");
    assert(sizes.size >= 20 && read_u32(sizes.bytes + 12) == 8);
    movie->audio_frames = read_u32(sizes.bytes + 16);
    size_t word_count = (size_t)movie->audio_frames * 2;
    assert(word_count <= SIZE_MAX / sizeof(uint32_t));
    movie->audio_words = calloc(word_count ? word_count : 1, sizeof(uint32_t));
    assert(movie->audio_words && chunks.size >= 16 && layout.size >= 16);
    unsigned chunk_count = read_u32(chunks.bytes + 12);
    unsigned layout_count = read_u32(layout.bytes + 12);
    assert(chunk_count <= (chunks.size - 16) / 8);
    assert(layout_count <= (layout.size - 16) / 12);
    unsigned entry = 0;
    size_t completed = 0;
    for (unsigned chunk = 1; chunk <= chunk_count; ++chunk) {
        assert(layout_count && read_u32(layout.bytes + 16) == 1);
        while (entry + 1 < layout_count &&
               read_u32(layout.bytes + 16 + (size_t)(entry + 1) * 12) <= chunk)
            ++entry;
        unsigned count = read_u32(layout.bytes + 20 + (size_t)entry * 12);
        assert(read_u32(layout.bytes + 24 + (size_t)entry * 12) == 1);
        uint64_t offset = read_u64(chunks.bytes + 16 + (size_t)(chunk - 1) * 8);
        assert(offset <= file_size && count <= (file_size - (size_t)offset) / 8);
        assert(completed <= movie->audio_frames &&
               count <= movie->audio_frames - completed);
        for (size_t word = 0; word < (size_t)count * 2; ++word) {
            const uint8_t *bytes = file + (size_t)offset + word * 4;
            movie->audio_words[completed * 2 + word] =
                (uint32_t)bytes[3] << 24 | (uint32_t)bytes[2] << 16 |
                (uint32_t)bytes[1] << 8 | bytes[0];
        }
        completed += count;
    }
    assert(completed == movie->audio_frames);
}

static TestMovie read_movie(const char *path) {
    size_t file_size;
    uint8_t *file = read_file(path, &file_size);
    TestBox movie_box = find_box(file, file_size, "moov");
    assert(movie_box.bytes);
    TestMovie movie = {0};
    size_t offset = movie_box.header;
    unsigned seen = 0;
    while (offset < movie_box.size) {
        TestBox track =
            find_box(movie_box.bytes + offset, movie_box.size - offset, "trak");
        if (!track.bytes)
            break;
        TestBox media = child_box(track, "mdia");
        TestBox header = child_box(media, "mdhd");
        TestBox handler = child_box(media, "hdlr");
        TestBox table = child_box(child_box(media, "minf"), "stbl");
        assert(header.size >= 40 && header.bytes[8] == 1 && handler.size >= 20);
        if (!memcmp(handler.bytes + 16, "vide", 4)) {
            assert(!(seen & 1) && read_u32(header.bytes + 28) == 1000000);
            seen |= 1;
            movie.video_ticks = read_u64(header.bytes + 32);
            TestBox sizes = child_box(table, "stsz");
            assert(sizes.size >= 20 && read_u32(sizes.bytes + 12) == 0);
            movie.video_frames = read_u32(sizes.bytes + 16);
            TestBox times = child_box(table, "stts");
            assert(times.size >= 16);
            unsigned runs = read_u32(times.bytes + 12);
            assert(runs <= (times.size - 16) / 8);
            uint64_t duration = 0;
            uint64_t frames = 0;
            for (unsigned run = 0; run < runs; ++run) {
                unsigned count = read_u32(times.bytes + 16 + (size_t)run * 8);
                unsigned ticks = read_u32(times.bytes + 20 + (size_t)run * 8);
                assert(count && ticks);
                frames += count;
                duration += (uint64_t)count * ticks;
            }
            assert(frames == movie.video_frames && duration == movie.video_ticks);
        } else {
            assert(!(seen & 2) && !memcmp(handler.bytes + 16, "soun", 4));
            seen |= 2;
            movie.audio_rate = read_u32(header.bytes + 28);
            assert(movie.audio_rate >= 8000 && movie.audio_rate <= 65535);
            read_audio_chunks(&movie, table, file, file_size);
            assert(read_u64(header.bytes + 32) == movie.audio_frames);
        }
        offset = (size_t)(track.bytes - movie_box.bytes) + track.size;
    }
    assert(seen == 3);
    free(file);
    return movie;
}

static void check_audio(const TestMovie *movie, const float *expected, size_t frames) {
    assert(movie->audio_frames == frames);
    for (size_t word = 0; word < frames * 2; ++word) {
        uint32_t expected_word;
        memcpy(&expected_word, expected + word, sizeof(expected_word));
        assert(movie->audio_words[word] == expected_word);
    }
}

static void install_voice(GcAudio *audio, float gain) {
    audio->tracks[0].parent = GC_AUDIO_TRACKS;
    audio->tracks[0].parameters[0] = 1;
    audio->tracks[0].routes[0] = 0x10;
    audio->voices[0] = (GcAudioVoice){.active = true,
                                      .detached = true,
                                      .released = true,
                                      .step = 1,
                                      .base_gain = gain,
                                      .track_gain = 1,
                                      .envelope_volume = 1,
                                      .envelope_pitch = 1,
                                      .buses = {1},
                                      .routes = {0x10},
                                      .pan_weights = {0, 1, 0}};
}

static GcAudio *test_audio(void) {
    GcAudio *audio = calloc(1, sizeof(*audio));
    assert(audio);
    audio->sample_rate = 48000;
    audio->master_gain = 32767;
    audio->output_gain = 4096;
    audio->wave_count = 1;
    audio->waves[0] = (GcAudioWave){.count = 257, .loop = true, .loop_end = 257};
    audio->waves[0].samples = malloc(257 * sizeof(int16_t));
    assert(audio->waves[0].samples);
    for (unsigned index = 0; index < 257; ++index)
        audio->waves[0].samples[index] = (int16_t)((int)(index * 7919 % 60001) - 30000);
    for (unsigned phase = 0; phase < 64; ++phase)
        audio->resampling_coefficients[phase][3] = 32767;
    atomic_init(&audio->mono, false);
    atomic_init(&audio->event_read, 0);
    atomic_init(&audio->event_write, 0);
    atomic_init(&audio->dropped_events, 0);
    atomic_init(&audio->active_voices, 0);
    atomic_init(&audio->sequence_stopped, true);
    install_voice(audio, 0.75f);
    return audio;
}

static void make_path(char path[1024], const char *directory, const char *name) {
    int count = snprintf(path, 1024, "%s/%s", directory, name);
    assert(count > 0 && count < 1024);
}

static void draw_frame(CcPlatform *platform, float brightness) {
    cc_platform_begin(platform, (CcColor){brightness, 0.25f, 0.5f, 1});
    cc_platform_end(platform);
}

static void test_delay_prefill_and_restart(const char *directory) {
    char path[1024];
    make_path(path, directory, "recording-restart.mp4");
    remove(path);
    CcPlatform *platform = cc_platform_create("Recording test", 640, 480);
    GcAudio *audio = test_audio();
    assert(platform);
    CcRecording *recording =
        gc_recording_open_path(platform, audio, 60, true, false, path);
    assert(recording && !strcmp(cc_recording_path(recording), path));
    draw_frame(platform, 0);
    assert(cc_recording_frame(recording, 1000));
    assert(cc_recording_pump(recording, 1000.125));
    assert(cc_recording_audio_start(recording, 1000.125));
    float first[1536 * 2];
    float restarted[1536 * 2];
    gc_audio_render(audio, first, 1536);
    assert(cc_recording_pump(recording, 1000.140625));
    assert(cc_recording_audio_stop(recording, 1000.15625));
    draw_frame(platform, 0.5f);
    assert(cc_recording_frame(recording, 1000.15625));
    gc_audio_reset(audio);
    install_voice(audio, 0.375f);
    assert(cc_recording_pump(recording, 1000.1875));
    assert(cc_recording_audio_start(recording, 1000.1875));
    gc_audio_render(audio, restarted, 1536);
    assert(cc_recording_pump(recording, 1000.203125));
    assert(cc_recording_audio_stop(recording, 1000.21875));
    draw_frame(platform, 1);
    assert(cc_recording_frame(recording, 1000.21875));
    assert(cc_recording_close(recording, 1000.25));
    TestMovie movie = read_movie(path);
    assert(movie.video_ticks == 250000 && movie.video_frames == 3 &&
           movie.audio_rate == 48000);
    float expected[12000 * 2] = {0};
    memcpy(expected + 6000 * 2, first, 1500 * 2 * sizeof(float));
    memcpy(expected + 9000 * 2, restarted, 1500 * 2 * sizeof(float));
    check_audio(&movie, expected, 12000);
    free(movie.audio_words);
    assert(!gc_audio_capture_failed(audio));
    assert(gc_audio_capture_begin(audio, 1));
    gc_audio_capture_end(audio);
    gc_audio_destroy(audio);
    cc_platform_destroy(platform);
}

static void test_step_pause_timeline(const char *directory) {
    char path[1024];
    make_path(path, directory, "recording-step.mp4");
    remove(path);
    CcPlatform *platform = cc_platform_create("Step recording test", 640, 480);
    GcAudio *audio = test_audio();
    assert(platform);
    CcRecording *recording =
        gc_recording_open_path(platform, audio, 50, false, false, path);
    assert(recording);
    draw_frame(platform, 0);
    assert(cc_recording_frame(recording, 1000));
    float inaudible[960 * 2];
    gc_audio_render(audio, inaudible, 960);
    assert(cc_recording_audio_start(recording, 1000));
    assert(cc_recording_pump(recording, 1000.125));
    draw_frame(platform, 0.5f);
    assert(cc_recording_frame(recording, 1000.125));
    assert(cc_recording_pump(recording, 1000.375));
    draw_frame(platform, 0);
    assert(cc_recording_frame(recording, 1000.375));
    assert(cc_recording_close(recording, 1000.5));
    TestMovie movie = read_movie(path);
    assert(movie.video_ticks == 500000 && movie.video_frames == 3);
    assert(movie.audio_frames == 24000);
    for (size_t word = 0; word < (size_t)movie.audio_frames * 2; ++word)
        assert(movie.audio_words[word] == 0);
    free(movie.audio_words);
    gc_audio_destroy(audio);
    cc_platform_destroy(platform);
}

static void test_failure_cleanup(const char *directory) {
    char path[1024];
    make_path(path, directory, "recording-exclusive.mp4");
    remove(path);
    FILE *existing = fopen(path, "wb");
    const char marker[] = "retained existing file";
    assert(existing && fwrite(marker, 1, sizeof(marker), existing) == sizeof(marker));
    assert(fclose(existing) == 0);
    CcPlatform *platform = cc_platform_create("Failure recording test", 640, 480);
    GcAudio *audio = test_audio();
    assert(platform);
    assert(!gc_recording_open_path(platform, audio, 60, true, false, path));
    size_t size;
    uint8_t *bytes = read_file(path, &size);
    assert(size == sizeof(marker) && !memcmp(bytes, marker, size));
    free(bytes);
    assert(gc_audio_capture_begin(audio, 1));
    gc_audio_capture_end(audio);
    CcFramebuffer frame;
    assert(cc_platform_capture_begin(platform, &frame));
    cc_platform_capture_end(platform);
    assert(!gc_recording_open_path(platform, audio, 59, true, false, path));
    make_path(path, directory, "recording-overflow.mp4");
    remove(path);
    CcRecording *recording =
        gc_recording_open_path(platform, audio, 60, true, false, path);
    assert(recording);
    draw_frame(platform, 0);
    assert(cc_recording_frame(recording, 1000));
    assert(cc_recording_audio_start(recording, 1000));
    float *stereo = malloc(96001 * 2 * sizeof(float));
    assert(stereo);
    gc_audio_render(audio, stereo, 96001);
    assert(!cc_recording_pump(recording, 1000.125));
    assert(!cc_recording_close(recording, 1000.25));
    assert(gc_audio_capture_begin(audio, 1));
    gc_audio_capture_end(audio);
    assert(cc_platform_capture_begin(platform, &frame));
    cc_platform_capture_end(platform);
    free(stereo);
    gc_audio_destroy(audio);
    cc_platform_destroy(platform);
}

static void test_variable_rate_and_clock_failure(const char *directory) {
    char path[1024];
    make_path(path, directory, "recording-rate.mp4");
    remove(path);
    CcPlatform *platform = cc_platform_create("Rate recording test", 640, 480);
    GcAudio *audio = test_audio();
    assert(platform);
    audio->sample_rate = 96000;
    assert(!gc_recording_open_path(platform, audio, 60, true, false, path));
    audio->sample_rate = 44100;
    CcRecording *recording =
        gc_recording_open_path(platform, audio, 60, true, false, path);
    assert(recording);
    draw_frame(platform, 0);
    assert(cc_recording_frame(recording, 1000));
    assert(cc_recording_audio_start(recording, 1000));
    float prefill[1536 * 2];
    gc_audio_render(audio, prefill, 1536);
    assert(cc_recording_pump(recording, 1000.015625));
    assert(cc_recording_audio_stop(recording, 1000.03125));
    assert(cc_recording_close(recording, 1000.03125));
    TestMovie movie = read_movie(path);
    assert(movie.video_ticks == 31250 && movie.video_frames == 1 &&
           movie.audio_rate == 44100);
    check_audio(&movie, prefill, 1378);
    free(movie.audio_words);
    make_path(path, directory, "recording-clock-failure.mp4");
    remove(path);
    recording = gc_recording_open_path(platform, audio, 60, false, false, path);
    assert(recording);
    draw_frame(platform, 0);
    assert(cc_recording_frame(recording, 1000));
    assert(cc_recording_pump(recording, 1000.125));
    assert(!cc_recording_close(recording, NAN));
    movie = read_movie(path);
    assert(movie.video_ticks == 125000 && movie.video_frames == 1 &&
           movie.audio_frames == 6000 && movie.audio_rate == 48000);
    free(movie.audio_words);
    CcFramebuffer frame;
    assert(cc_platform_capture_begin(platform, &frame));
    cc_platform_capture_end(platform);
    gc_audio_destroy(audio);
    cc_platform_destroy(platform);
}

static void test_movies_destination(const char *directory) {
    char home[1024];
    make_path(home, directory, "recording-home");
    assert(mkdir(home, 0700) == 0 || errno == EEXIST);
    const char *previous = getenv("HOME");
    char *saved_home = previous ? strdup(previous) : NULL;
    assert(!previous || saved_home);
    assert(setenv("HOME", home, 1) == 0);
    CcPlatform *platform = cc_platform_create("Movies recording test", 640, 480);
    GcAudio *audio = test_audio();
    assert(platform);
    CcRecording *recording = gc_recording_open(platform, audio, 50, false, false);
    assert(recording);
    char path[1024];
    int count = snprintf(path, sizeof(path), "%s", cc_recording_path(recording));
    assert(count > 0 && (size_t)count < sizeof(path));
    assert(!strncmp(path, home, strlen(home)) &&
           !strncmp(path + strlen(home), "/Movies/GameCube-", 17) &&
           !strcmp(path + strlen(path) - 4, ".mp4"));
    draw_frame(platform, 0);
    assert(cc_recording_frame(recording, 1000));
    assert(cc_recording_close(recording, 1000.125));
    TestMovie movie = read_movie(path);
    assert(movie.video_ticks == 125000 && movie.video_frames == 1 &&
           movie.audio_frames == 6000);
    free(movie.audio_words);
    assert(remove(path) == 0);
    if (saved_home)
        assert(setenv("HOME", saved_home, 1) == 0);
    else
        assert(unsetenv("HOME") == 0);
    free(saved_home);
    gc_audio_destroy(audio);
    cc_platform_destroy(platform);
}

int main(int argc, char **argv) {
    assert(argc == 2 && argv[1][0]);
    test_delay_prefill_and_restart(argv[1]);
    test_step_pause_timeline(argv[1]);
    test_failure_cleanup(argv[1]);
    test_variable_rate_and_clock_failure(argv[1]);
    test_movies_destination(argv[1]);
    puts("Recording coordinator tests passed.");
    return 0;
}
