#ifndef GAMECUBE_OPTION_VALUES_H
#define GAMECUBE_OPTION_VALUES_H

#include "gamecube/menu.h"

/* Shared CLI value conversions. Invalid values leave the output unchanged. */
bool gc_option_region(const char *text, gc_region *region);
const char *gc_option_default_ipl(gc_region region);
bool gc_option_seconds(const char *text, double *seconds);
bool gc_option_unsigned(const char *text, int base, unsigned long minimum,
                        unsigned long maximum, unsigned long *value);

#endif
