#include "card_checksum.h"
#include "console_common/support/endian.h"

static void checksums(const uint8_t *bytes, size_t length, uint16_t *sum,
                      uint16_t *inverse) {
    uint32_t total = 0;
    uint32_t complemented = 0;
    for (size_t offset = 0; offset < length; offset += 2) {
        uint16_t value = cc_read_be16(bytes + offset);
        total += value;
        complemented += (uint16_t)~value;
    }
    *sum = (uint16_t)total;
    *inverse = (uint16_t)complemented;
    if (*sum == UINT16_MAX)
        *sum = 0;
    if (*inverse == UINT16_MAX)
        *inverse = 0;
}

bool gc_card_checksum_valid(const uint8_t *bytes, size_t length,
                            const uint8_t stored[4]) {
    uint16_t sum;
    uint16_t inverse;
    checksums(bytes, length, &sum, &inverse);
    return cc_read_be16(stored) == sum && cc_read_be16(stored + 2) == inverse;
}

void gc_card_checksum_write(const uint8_t *bytes, size_t length, uint8_t stored[4]) {
    uint16_t sum;
    uint16_t inverse;
    checksums(bytes, length, &sum, &inverse);
    cc_write_be16(stored, sum);
    cc_write_be16(stored + 2, inverse);
}
