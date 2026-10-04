#define _POSIX_C_SOURCE 200809L

#include "support/output.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include "console_common/support/tool_io.h"
#include "platform/windows/junction.h"
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

static void directory(const char *path) {
    assert(mkdir(path, 0700) == 0 || errno == EEXIST);
}

static void write_text(const char *path, const char *text) {
    FILE *stream = fopen(path, "wb");
    assert(stream);
    assert(fputs(text, stream) >= 0);
    assert(fclose(stream) == 0);
}

static void expect_text(const char *path, const char *expected) {
    char data[64] = {0};
    FILE *stream = fopen(path, "rb");
    assert(stream);
    size_t size = fread(data, 1, sizeof(data) - 1, stream);
    assert(!ferror(stream) && size == strlen(expected));
    assert(fclose(stream) == 0);
    assert(!strcmp(data, expected));
}

int main(void) {
    directory("Files");
    directory("outside");
    assert(gc_tool_output_directory("Files/"));
    assert(gc_tool_output_directory("Files/nested/output"));
    FILE *stream = gc_tool_output_open("Files/nested/output/frame.bin");
    assert(stream && fputs("first", stream) >= 0 && fclose(stream) == 0);
    stream = gc_tool_output_open("Files/nested/output/frame.bin");
    assert(stream && fputs("x", stream) >= 0 && fclose(stream) == 0);
    expect_text("Files/nested/output/frame.bin", "x");
    write_text("outside/protected.bin", "unchanged");
#ifdef _WIN32
    rmdir("Files/directory-link");
#else
    unlink("Files/directory-link");
#endif
    unlink("Files/file-link");
    unlink("Files/pipe");
#ifdef _WIN32
    assert(cc_test_junction("Files/directory-link", "outside"));
#else
    assert(symlink("../outside", "Files/directory-link") == 0);
    assert(symlink("../outside/protected.bin", "Files/file-link") == 0);
    assert(mkfifo("Files/pipe", 0600) == 0);
#endif
    assert(!gc_tool_output_open("Files/directory-link/protected.bin"));
#ifndef _WIN32
    assert(!gc_tool_output_open("Files/file-link"));
#endif
#ifndef _WIN32
    assert(!gc_tool_output_open("Files/pipe"));
#endif
    assert(!gc_tool_output_directory("Files/directory-link/new"));
    expect_text("outside/protected.bin", "unchanged");
    static const char *const invalid[] = {
        NULL,           "",
        "Files//empty", "Files/../outside/protected.bin",
        "Files/./file", "outside/file"};
    for (size_t index = 0; index < sizeof(invalid) / sizeof(invalid[0]); ++index) {
        assert(!gc_tool_output_open(invalid[index]));
        assert(!gc_tool_output_directory(invalid[index]));
    }
    assert(!gc_tool_output_open("Files/"));
#ifdef _WIN32
    rmdir("Files/directory-link");
#else
    unlink("Files/directory-link");
#endif
    unlink("Files/file-link");
    unlink("Files/pipe");
    puts("Tool output containment and regular-file checks passed.");
    return 0;
}
