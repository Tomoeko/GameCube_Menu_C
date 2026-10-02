#ifndef GAMECUBE_NATIVE_CONSTANTS_H
#define GAMECUBE_NATIVE_CONSTANTS_H

#include "gamecube/text.h"
#include "console_common/support/endian.h"

/* Read a bounded initializer's immediate halfword stores. This deliberately
 * supports only the li/lis/addi/addis/sth instructions used by native tables. */
static bool gc_native_halfwords(const GcText *text, size_t begin, size_t end,
                                unsigned base_register, unsigned field, unsigned count,
                                uint16_t *values) {
    uint32_t registers[32] = {0};
    bool known[32] = {false};
    bool found[32] = {false};
    if (!text || !text->rom || !values || !count || count > 32 || base_register >= 32 ||
        begin > end || end > text->rom_size || begin % 4 || end % 4)
        return false;
    for (size_t offset = begin; offset + 4 <= end; offset += 4) {
        const uint8_t *bytes = text->rom + offset;
        uint32_t word = cc_read_be32(bytes);
        unsigned opcode = word >> 26;
        unsigned target = word >> 21 & 31;
        unsigned base = word >> 16 & 31;
        int32_t immediate = (int16_t)(word & 0xffff);
        if (opcode == 14 || opcode == 15) {
            known[target] = !base || known[base];
            if (known[target])
                registers[target] =
                    (base ? registers[base] : 0) +
                    (opcode == 15 ? (uint32_t)immediate << 16 : (uint32_t)immediate);
        } else if (opcode == 18 && (word & 1)) {
            known[0] = false;
            for (unsigned index = 3; index <= 12; ++index)
                known[index] = false;
        } else if (opcode == 44 && base == base_register && known[target] &&
                   immediate >= (int32_t)field &&
                   immediate < (int32_t)(field + count * 2)) {
            if (immediate % 2 || registers[target] > UINT16_MAX)
                return false;
            unsigned index = (unsigned)(immediate - (int32_t)field) / 2;
            values[index] = (uint16_t)registers[target];
            found[index] = true;
        }
    }
    for (unsigned index = 0; index < count; ++index)
        if (!found[index])
            return false;
    return true;
}

#endif
