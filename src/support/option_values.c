#include "option_values.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

bool gc_option_region(const char *text, gc_region *region) {
    if (!text || !region)
        return false;
    if (!strcmp(text, "USA"))
        *region = GC_REGION_USA;
    else if (!strcmp(text, "EUR"))
        *region = GC_REGION_EUROPE;
    else if (!strcmp(text, "JAP"))
        *region = GC_REGION_JAPAN;
    else
        return false;
    return true;
}

const char *gc_option_default_ipl(gc_region region) {
    static const char *const paths[] = {"Files/GameCube_BIOS/JAP/IPL.bin",
                                        "Files/GameCube_BIOS/USA/IPL.bin",
                                        "Files/GameCube_BIOS/EUR/IPL.bin"};
    if ((unsigned)region >= sizeof(paths) / sizeof(paths[0]))
        return NULL;
    return paths[region];
}

bool gc_option_seconds(const char *text, double *seconds) {
    if (!text || !seconds)
        return false;
    char *end = NULL;
    errno = 0;
    double parsed = strtod(text, &end);
    if (errno || end == text || *end || !isfinite(parsed) || parsed < 0)
        return false;
    *seconds = parsed;
    return true;
}

bool gc_option_unsigned(const char *text, int base, unsigned long minimum,
                        unsigned long maximum, unsigned long *value) {
    if (!text || !value || minimum > maximum || (base != 0 && (base < 2 || base > 36)))
        return false;
    const char *first = text;
    while (isspace((unsigned char)*first))
        ++first;
    if (*first == '-')
        return false;
    char *end = NULL;
    errno = 0;
    unsigned long parsed = strtoul(text, &end, base);
    if (errno || end == text || *end || parsed < minimum || parsed > maximum)
        return false;
    *value = parsed;
    return true;
}
