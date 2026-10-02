#ifndef GAMECUBE_IPL_INTERNAL_H
#define GAMECUBE_IPL_INTERNAL_H

#include "gamecube/ipl.h"

/* Borrow the complete raw ROM's clear ANSI range; the empty font owns the
 * decoded atlas on success. The caller retains and releases the ROM. */
bool gc_ipl_ansi_font_decode(const uint8_t *rom, size_t size, GcIplFont *font);

#endif
