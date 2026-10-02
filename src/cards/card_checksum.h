#ifndef GAMECUBE_CARD_CHECKSUM_H
#define GAMECUBE_CARD_CHECKSUM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The caller supplies a bounded, even-sized native card section and a
 * separate four-byte sum/complement record. An all-ones total stores zero. */
bool gc_card_checksum_valid(const uint8_t *bytes, size_t length,
                            const uint8_t stored[4]);
void gc_card_checksum_write(const uint8_t *bytes, size_t length, uint8_t stored[4]);

#endif
