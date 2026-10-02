#ifndef GAMECUBE_TOOL_OUTPUT_H
#define GAMECUBE_TOOL_OUTPUT_H

#include <stdbool.h>
#include <stdio.h>

/* Output paths are relative to Files. Directory traversal and symlink
 * components are rejected; the caller owns and closes a returned stream. */
FILE *gc_tool_output_open(const char *path);
bool gc_tool_output_directory(const char *path);

#endif
