#ifndef GAMECUBE_SOFTWARE_H
#define GAMECUBE_SOFTWARE_H

#include "console_common/platform/platform.h"

#include <stdio.h>

/* Offline rendering uses the same ordered draw calls as the native backends. */
/* Write PPM pixels to a borrowed stream; the caller flushes and closes it. */
bool gc_software_write_stream(CcPlatform *platform, FILE *stream);
bool gc_software_write_frame(CcPlatform *platform, const char *path);
/* Exact RGBA hash for deterministic pause and replay verification. */
uint64_t gc_software_frame_hash(const CcPlatform *platform);
/* Bounded offline inspection; normal rendering never reads pixels back. */
bool gc_software_read_pixel(const CcPlatform *platform, unsigned x, unsigned y,
                            uint8_t rgba[4]);

#endif
