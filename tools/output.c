#define _POSIX_C_SOURCE 200809L
#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif

#include "output.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum { TOOL_PATH_LIMIT = 4096, TOOL_COMPONENT_LIMIT = 256 };

static bool valid_component(const char *component) {
    return *component && strlen(component) < TOOL_COMPONENT_LIMIT &&
           strcmp(component, ".") && strcmp(component, "..");
}

static int open_child_directory(int parent, const char *name, bool create) {
    int flags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC;
    int child = openat(parent, name, flags);
    if (child < 0 && create && errno == ENOENT) {
        if (mkdirat(parent, name, 0755) && errno != EEXIST)
            return -1;
        child = openat(parent, name, flags);
    }
    return child;
}

/* Walk from the opened root descriptor. Resolving one component at a time
 * keeps existing links from redirecting a capture outside its output root. */
static int open_output_directory(const char *path, bool create,
                                 char leaf[TOOL_COMPONENT_LIMIT]) {
    if (!path || strncmp(path, "Files/", 6) || strlen(path) >= TOOL_PATH_LIMIT)
        return -1;
    char relative[TOOL_PATH_LIMIT];
    memcpy(relative, path + 6, strlen(path + 6) + 1);
    int directory = open("Files", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (directory < 0)
        return -1;
    if (create && !leaf && !*relative)
        return directory;
    char *component = relative;
    while (true) {
        char *separator = strchr(component, '/');
        if (separator)
            *separator = '\0';
        if (!valid_component(component))
            break;
        if (!separator && leaf) {
            memcpy(leaf, component, strlen(component) + 1);
            return directory;
        }
        int child = open_child_directory(directory, component, create);
        if (child < 0)
            break;
        close(directory);
        directory = child;
        if (!separator)
            return directory;
        component = separator + 1;
    }
    close(directory);
    return -1;
}

bool gc_tool_output_directory(const char *path) {
    int descriptor = open_output_directory(path, true, NULL);
    return descriptor >= 0 && close(descriptor) == 0;
}

FILE *gc_tool_output_open(const char *path) {
    char leaf[TOOL_COMPONENT_LIMIT];
    int directory = open_output_directory(path, false, leaf);
    if (directory < 0)
        return NULL;
    struct stat named;
    bool exists = fstatat(directory, leaf, &named, AT_SYMLINK_NOFOLLOW) == 0;
    if ((!exists && errno != ENOENT) || (exists && !S_ISREG(named.st_mode))) {
        close(directory);
        return NULL;
    }
    int descriptor =
        openat(directory, leaf,
               O_WRONLY | O_CREAT | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0644);
    close(directory);
    if (descriptor < 0)
        return NULL;
    struct stat opened;
    bool regular =
        fstat(descriptor, &opened) == 0 && S_ISREG(opened.st_mode) &&
        (!exists || (opened.st_dev == named.st_dev && opened.st_ino == named.st_ino));
    /* Validate the opened object before truncation, including raced paths. */
    if (!regular || ftruncate(descriptor, 0)) {
        close(descriptor);
        return NULL;
    }
    FILE *stream = fdopen(descriptor, "wb");
    if (!stream)
        close(descriptor);
    return stream;
}
