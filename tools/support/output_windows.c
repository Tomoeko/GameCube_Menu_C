#include "output.h"
#include "platform/windows/file_util.h"
#include "console_common/support/portable_path.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum { TOOL_PATH_LIMIT = 4096, TOOL_COMPONENT_LIMIT = 256, TOOL_DIRECTORIES = 512 };

static bool valid_component(const char *name) {
    size_t length = strlen(name);
    if (!length || length >= TOOL_COMPONENT_LIMIT || !strcmp(name, ".") ||
        !strcmp(name, "..") || name[length - 1] == '.' || name[length - 1] == ' ' ||
        cc_path_reserved_component((const uint8_t *)name, length))
        return false;
    for (size_t index = 0; index < length; ++index) {
        if ((unsigned char)name[index] < 32 || strchr("\\:*?\"<>|", name[index]))
            return false;
    }
    return true;
}

static FILE *open_output(const char *path, bool create_directories,
                         bool *directory_ok) {
    *directory_ok = false;
    if (!path || strncmp(path, "Files/", 6) || strlen(path) >= TOOL_PATH_LIMIT)
        return NULL;
    char current[TOOL_PATH_LIMIT];
    memcpy(current, path, strlen(path) + 1);
    HANDLE directories[TOOL_DIRECTORIES];
    size_t count = 0;
    current[5] = 0;
    HANDLE root = cc_windows_open_directory(current);
    current[5] = '/';
    if (root == INVALID_HANDLE_VALUE)
        return NULL;
    directories[count++] = root;
    if (create_directories && !current[6]) {
        *directory_ok = true;
        CloseHandle(root);
        return NULL;
    }
    FILE *stream = NULL;
    char *component = current + 6;
    for (;;) {
        char *separator = strchr(component, '/');
        if (separator)
            *separator = 0;
        if (!valid_component(component))
            goto release_directories;
        if (!separator && !create_directories) {
            HANDLE file = cc_windows_open_regular(current, GENERIC_WRITE, OPEN_ALWAYS);
            if (file == INVALID_HANDLE_VALUE)
                goto release_directories;
            LARGE_INTEGER start = {0};
            if (!SetFilePointerEx(file, start, NULL, FILE_BEGIN) ||
                !SetEndOfFile(file)) {
                CloseHandle(file);
                goto release_directories;
            }
            stream = cc_windows_stream(file, "wb");
            break;
        }
        if (create_directories && !cc_windows_create_directory(current))
            goto release_directories;
        HANDLE child = cc_windows_open_directory(current);
        if (child == INVALID_HANDLE_VALUE || count == TOOL_DIRECTORIES) {
            if (child != INVALID_HANDLE_VALUE)
                CloseHandle(child);
            goto release_directories;
        }
        directories[count++] = child;
        if (!separator) {
            *directory_ok = true;
            break;
        }
        *separator = '/';
        component = separator + 1;
    }
release_directories:
    while (count)
        CloseHandle(directories[--count]);
    return stream;
}

bool gc_tool_output_directory(const char *path) {
    bool okay;
    open_output(path, true, &okay);
    return okay;
}

FILE *gc_tool_output_open(const char *path) {
    bool unused;
    return open_output(path, false, &unused);
}
