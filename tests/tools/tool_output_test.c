#define _POSIX_C_SOURCE 200809L

#include "support/output.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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
    unlink("Files/directory-link");
    unlink("Files/file-link");
    unlink("Files/pipe");
    assert(symlink("../outside", "Files/directory-link") == 0);
    assert(symlink("../outside/protected.bin", "Files/file-link") == 0);
    assert(mkfifo("Files/pipe", 0600) == 0);
    assert(!gc_tool_output_open("Files/directory-link/protected.bin"));
    assert(!gc_tool_output_open("Files/file-link"));
    assert(!gc_tool_output_open("Files/pipe"));
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
    unlink("Files/directory-link");
    unlink("Files/file-link");
    unlink("Files/pipe");
    puts("Tool output containment and regular-file checks passed.");
    return 0;
}
