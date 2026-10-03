#include "startup_internal.h"
#include "console_common/support/endian.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define STARTUP_PARAMETER_BYTES 400

/* Reads data stores from two native initializers, without executing code.
 * Only li, lfs from the verified r2 pool, and stores through argument r3
 * contribute values. Unknown computed values are deliberately ignored.
 */
static bool recover_parameters(const uint8_t *rom, size_t start, uint32_t r2,
                               uint8_t output[STARTUP_PARAMETER_BYTES],
                               bool menu_profile) {
    uint32_t registers[32] = {0};
    uint32_t floats[32] = {0};
    bool known[32] = {false};
    bool float_known[32] = {false};
    bool argument[32] = {false};
    argument[3] = true;
    for (unsigned index = 0; index < 160; ++index) {
        size_t offset = start + index * 4;
        if (offset + 4 > GC_IPL_SCRAMBLED_END)
            return false;
        uint32_t word = cc_read_be32(rom + offset);
        if (word == UINT32_C(0x4e800020))
            return true;
        unsigned opcode = word >> 26;
        unsigned target = (word >> 21) & 31;
        unsigned base = (word >> 16) & 31;
        int displacement = (int16_t)(word & 65535);
        if (opcode == 14 && base == 0) {
            registers[target] = (uint32_t)displacement;
            known[target] = true;
            argument[target] = false;
        } else if (opcode == 14 && displacement == 0) {
            argument[target] = argument[base];
            registers[target] = registers[base];
            known[target] = known[base];
        } else if (opcode == 31 && ((word >> 1) & 1023) == 444 &&
                   target == ((word >> 11) & 31)) {
            /* mr is an or with identical source operands. */
            argument[base] = argument[target];
            registers[base] = registers[target];
            known[base] = known[target];
        } else if (opcode == 48 && base == 2) {
            int64_t address = (int64_t)r2 + displacement;
            int64_t file =
                address - (int64_t)GC_IPL_BS2_ADDRESS + (int64_t)GC_IPL_BS2_OFFSET;
            if (file < 0 || file + 4 > (int64_t)GC_IPL_ROM_SIZE)
                return false;
            floats[target] = cc_read_be32(rom + (size_t)file);
            float_known[target] = true;
        } else if (argument[base] && displacement >= 0 &&
                   displacement < STARTUP_PARAMETER_BYTES) {
            uint8_t *destination = output + displacement;
            size_t available = STARTUP_PARAMETER_BYTES - (size_t)displacement;
            if (opcode == 38 && known[target])
                destination[0] = (uint8_t)registers[target];
            else if (opcode == 44 && known[target] && available >= 2)
                cc_write_be16(destination, (uint16_t)registers[target]);
            else if (opcode == 36 && known[target] && available >= 4)
                cc_write_be32(destination, registers[target]);
            else if (opcode == 52 && float_known[target] && available >= 4)
                cc_write_be32(destination, floats[target]);
            if (menu_profile && displacement == 50 && opcode == 44 && known[target])
                return true;
        }
    }
    return false;
}

static bool find_parameters(const uint8_t *rom, uint32_t r2,
                            uint8_t route_parameters[STARTUP_PARAMETER_BYTES],
                            uint8_t scene_parameters[STARTUP_PARAMETER_BYTES]) {
    bool route_found = false;
    bool scene_found = false;
    for (size_t offset = GC_IPL_BS2_OFFSET; offset + 204 <= GC_IPL_SCRAMBLED_END;
         offset += 4) {
        uint32_t first = cc_read_be32(rom + offset);
        if (first == UINT32_C(0x9421ffd8) &&
            cc_read_be32(rom + offset + 12) == UINT32_C(0xbf610014) &&
            cc_read_be32(rom + offset + 16) == UINT32_C(0x3b600000) &&
            cc_read_be32(rom + offset + 44) == UINT32_C(0xb363001a)) {
            route_found = recover_parameters(rom, offset, r2, route_parameters, false);
        } else if (first == UINT32_C(0x9421ffc8) &&
                   cc_read_be32(rom + offset + 12) == UINT32_C(0xbf010018) &&
                   cc_read_be32(rom + offset + 16) == UINT32_C(0x3b000000)) {
            scene_found = recover_parameters(rom, offset, r2, scene_parameters, false);
        }
        if (route_found && scene_found)
            return true;
    }
    return false;
}

static bool pool_load(const uint8_t *rom, size_t offset, uint32_t r2, float *value) {
    uint32_t word = cc_read_be32(rom + offset);
    if (word >> 26 != 48 || ((word >> 16) & 31) != 2)
        return false;
    int64_t file = (int64_t)r2 + (int16_t)word - (int64_t)GC_IPL_BS2_ADDRESS +
                   (int64_t)GC_IPL_BS2_OFFSET;
    if (file < 0 || file + 4 > (int64_t)GC_IPL_ROM_SIZE)
        return false;
    *value = cc_read_be_float(rom + (size_t)file);
    return isfinite(*value);
}

/* USA 0x8130c150/0x8130c200/0x8130e120 and EUR equivalents.
 * Selects native initializer and SDA loads by their instruction structure;
 * constants and the menu oscillation profile stay private in memory.
 */
static bool recover_scene_tables(const uint8_t *rom, uint32_t r2, GcStartup *startup) {
    bool kinetic = false, profile = false, angles = false;
    bool formation = false, reset = false;
    uint8_t parameters[STARTUP_PARAMETER_BYTES] = {0};
    for (size_t offset = GC_IPL_BS2_OFFSET; offset + 640 <= GC_IPL_SCRAMBLED_END;
         offset += 4) {
        uint32_t first = cc_read_be32(rom + offset);
        /* USA/JAP 0x8130bcd8 and EUR 0x8130c130 overwrite the
         * constructor's scene weight with zero before startup begins.
         */
        float reset_weight;
        if (first == UINT32_C(0x39200000) &&
            cc_read_be32(rom + offset + 4) == UINT32_C(0xb12300b2) &&
            cc_read_be32(rom + offset + 96) == UINT32_C(0xd0030024))
            reset = pool_load(rom, offset + 40, r2, &reset_weight) && reset_weight == 0;
        else if (first == UINT32_C(0x7c0802a6) &&
                 cc_read_be32(rom + offset + 4) == UINT32_C(0x38e00001) &&
                 cc_read_be32(rom + offset + 40) == UINT32_C(0x3bc30000) &&
                 cc_read_be32(rom + offset + 120) == UINT32_C(0xd01e0024))
            reset = pool_load(rom, offset + 64, r2, &reset_weight) && reset_weight == 0;
        if (first != UINT32_C(0x7c0802a6))
            continue;
        if (cc_read_be32(rom + offset + 16) == UINT32_C(0xa8830022) &&
            cc_read_be32(rom + offset + 24) == UINT32_C(0xc0230014) &&
            cc_read_be32(rom + offset + 52) == UINT32_C(0xa8830020)) {
            kinetic = pool_load(rom, offset + 68, r2, &startup->kinetic_increment) &&
                      pool_load(rom, offset + 80, r2, &startup->kinetic_decay);
        } else if (cc_read_be32(rom + offset + 8) == UINT32_C(0x9421ff18) &&
                   cc_read_be32(rom + offset + 36) == UINT32_C(0x7c7b1b78) &&
                   cc_read_be32(rom + offset + 44) == UINT32_C(0x8003000c) &&
                   cc_read_be32(rom + offset + 72) == UINT32_C(0xc05b0024)) {
            formation = pool_load(rom, offset + 124, r2, &startup->scene_origin_offset);
        } else if (cc_read_be32(rom + offset + 12) == UINT32_C(0x380002ee) &&
                   cc_read_be32(rom + offset + 36) == UINT32_C(0x3be30000)) {
            profile = recover_parameters(rom, offset, r2, parameters, true);
            for (unsigned index = 0; index < 13; ++index)
                startup->menu_profile[index] =
                    (int16_t)cc_read_be16(parameters + 26 + index * 2);
            startup->menu_rotation_step = cc_read_be16(parameters + 22);
            startup->menu_focus_distance = cc_read_be_float(parameters + 8);
            /* The regional structures insert language fields before these
             * members. PAL stores at +0x180; NTSC stores at +0x148.
             */
            size_t focus = cc_read_be16(parameters + 384) ? 384 : 328;
            startup->menu_focus_enter_step = cc_read_be16(parameters + focus);
            startup->menu_focus_exit_step = cc_read_be16(parameters + focus + 2);
            startup->menu_glass_min_alpha = cc_read_be16(parameters + focus + 4);
            startup->menu_focus_twist = cc_read_be_float(parameters + focus + 8);
        } else if (cc_read_be32(rom + offset + 8) == UINT32_C(0x9421ff00) &&
                   cc_read_be32(rom + offset + 40) == UINT32_C(0x3b430000) &&
                   cc_read_be32(rom + offset + 44) == UINT32_C(0x3b640000)) {
            const size_t angle_loads[3] = {632, 512, 384};
            const size_t rate_loads[3] = {588, 468, 324};
            angles = true;
            for (unsigned axis = 0; axis < 3; ++axis)
                angles = pool_load(rom, offset + angle_loads[axis], r2,
                                   &startup->transition_base_angles[axis]) &&
                         pool_load(rom, offset + rate_loads[axis], r2,
                                   &startup->kinetic_phase_rates[axis]) &&
                         angles;
        }
        if (kinetic && profile && angles && formation && reset)
            return true;
    }
    return false;
}

static bool find_route(const uint8_t *rom, GcStartup *startup) {
    /* Structural signature, not the route itself. ROM routes are retained
     * only in memory. USA/JAP table RAM 0x813a5a80, EUR ROM 0x81980.
     */
    const uint8_t prefix[16] = {0, 1, 5, 0, 0, 1, 255, 0, 3, 1, 255, 0, 3, 1, 255, 0};
    for (size_t offset = GC_IPL_BS2_OFFSET; offset + 16 <= GC_IPL_SCRAMBLED_END;
         offset += 4) {
        if (memcmp(rom + offset, prefix, sizeof(prefix)))
            continue;
        for (unsigned index = 0; index < GC_STARTUP_MAX_STEPS; ++index) {
            size_t record = offset + index * 4;
            if (record + 4 > GC_IPL_SCRAMBLED_END)
                break;
            unsigned direction = rom[record];
            if (direction == 8 && rom[record + 1] == 0 && rom[record + 2] == 0) {
                startup->step_count = index;
                return index > 16;
            }
            if (direction > 7 || rom[record + 1] > 3 || rom[record + 3])
                break;
            startup->steps[index].direction = (uint8_t)direction;
            startup->steps[index].trail_kind = rom[record + 1];
            startup->steps[index].trail_lifetime = rom[record + 2];
        }
    }
    return false;
}

static bool find_trail_texture(const uint8_t *rom, GcIplImage *image) {
    for (size_t offset = GC_IPL_BS2_OFFSET; offset + 16 < GC_IPL_SCRAMBLED_END;
         ++offset) {
        size_t size;
        if (!gc_ipl_yay0_size(rom + offset, GC_IPL_SCRAMBLED_END - offset, &size) ||
            size != 4128)
            continue;
        uint8_t bytes[4128];
        if (!gc_ipl_yay0_decode(rom + offset, GC_IPL_SCRAMBLED_END - offset, bytes,
                                sizeof(bytes), NULL, NULL))
            continue;
        const uint8_t header[8] = {1, 0, 0, 64, 0, 64, 2, 2};
        if (!memcmp(bytes, header, sizeof(header)) && cc_read_be32(bytes + 28) == 32 &&
            gc_ipl_texture_decode(bytes, sizeof(bytes), image)) {
            /* USA/JAP 0x81301a04 and EUR 0x8130180c use the I8 texture only
             * for alpha. White RGB lets the basic shader preserve raster color
             * without multiplying the mask intensity into it a second time. */
            for (size_t pixel = 0; pixel < (size_t)image->width * image->height;
                 ++pixel)
                image->rgba[pixel * 4] = image->rgba[pixel * 4 + 1] =
                    image->rgba[pixel * 4 + 2] = 255;
            return true;
        }
    }
    return false;
}

bool gc_startup_load(const char *ipl_path, GcStartup *startup) {
    if (!ipl_path || !startup || startup->trail_texture.rgba)
        return false;
    uint8_t *rom = NULL;
    if (!gc_ipl_rom_read(ipl_path, &rom))
        return false;
    if (cc_read_be32(rom + GC_IPL_SCRAMBLED_START) != UINT32_C(0x3c800011))
        gc_ipl_descramble(rom, GC_IPL_ROM_SIZE);
    size_t entry = GC_IPL_BS2_OFFSET;
    if ((cc_read_be32(rom + entry + 160) & UINT32_C(0xffff0000)) !=
            UINT32_C(0x3c400000) ||
        (cc_read_be32(rom + entry + 164) & UINT32_C(0xffff0000)) !=
            UINT32_C(0x60420000)) {
        free(rom);
        return false;
    }
    uint32_t r2 =
        (cc_read_be32(rom + entry + 160) << 16) | cc_read_be16(rom + entry + 166);
    uint8_t route[STARTUP_PARAMETER_BYTES] = {0};
    uint8_t scene[STARTUP_PARAMETER_BYTES] = {0};
    GcStartup result = {0};
    bool okay = find_parameters(rom, r2, route, scene) &&
                recover_scene_tables(rom, r2, &result) && find_route(rom, &result) &&
                find_trail_texture(rom, &result.trail_texture);
    free(rom);
    if (!okay) {
        gc_startup_destroy(&result);
        return false;
    }
    result.roll_ticks = cc_read_be16(route + 20);
    result.corner_ticks = cc_read_be16(route + 22);
    result.drop_ticks = route[34];
    result.drop_wait_ticks = route[35];
    result.bounce_ticks = route[53];
    result.bounce_wait_ticks = route[54];
    result.rise_ticks = route[61];
    result.rise_wait_ticks = route[62];
    result.reveal_ticks = route[84];
    result.logotype_ticks = route[87];
    result.logotype_delay_ticks = route[88];
    result.bounce_quarter_turns = route[55];
    result.cube_edge = route[89];
    result.drop_height = cc_read_be_float(route + 44);
    result.bounce_height = cc_read_be_float(route + 56);
    result.rise_velocity = cc_read_be_float(route + 80);
    result.frame_rate = result.roll_ticks == 8 ? 50 : 60;
    result.wave_decay[0] = cc_read_be_float(scene + 140);
    result.wave_decay[1] = cc_read_be_float(scene + 144);
    result.wave_translation[0] = cc_read_be_float(scene + 148);
    result.wave_translation[1] = cc_read_be_float(scene + 152);
    memcpy(result.trail_color, route + 90, 3);
    result.spin_target = (float)cc_read_be32(scene + 12);
    result.spin_acceleration = cc_read_be_float(scene + 24);
    for (unsigned axis = 0; axis < 3; ++axis)
        result.scene_base_angles[axis] = (int16_t)cc_read_be16(scene + 158 + axis * 2);
    result.spin_squash[0] = cc_read_be_float(scene + 72);
    result.spin_squash[1] = cc_read_be_float(scene + 76);
    result.transition_fade_ticks = cc_read_be16(scene + 40);
    result.transition_turns = cc_read_be16(scene + 42);
    result.transition_angle = (int16_t)cc_read_be16(scene + 44);
    for (unsigned stage = 0; stage < 3; ++stage)
        result.transition_ticks[stage] = cc_read_be32(scene + 48 + stage * 4);
    if ((result.roll_ticks != 8 && result.roll_ticks != 10) || !result.corner_ticks ||
        !result.drop_ticks || !result.bounce_ticks || !result.rise_ticks ||
        !result.reveal_ticks || result.cube_edge != 54 || result.step_count != 33 ||
        !isfinite(result.drop_height) || !isfinite(result.bounce_height) ||
        !isfinite(result.rise_velocity) || result.drop_height <= 0 ||
        result.bounce_height <= 0 || result.rise_velocity <= 0 ||
        !isfinite(result.wave_decay[0]) || !isfinite(result.wave_decay[1]) ||
        result.wave_decay[0] <= 0 || result.wave_decay[0] >= 1 ||
        result.wave_decay[1] <= 0 || result.wave_decay[1] >= 1 ||
        result.spin_target != 5000 || result.spin_acceleration <= 0 ||
        result.scene_origin_offset < 0 || !isfinite(result.spin_acceleration) ||
        result.kinetic_increment <= 0 || result.kinetic_decay <= 0 ||
        result.kinetic_decay >= 1 || result.transition_fade_ticks != 30 ||
        result.transition_ticks[0] != 15 || result.transition_ticks[1] != 80 ||
        result.transition_ticks[2] != 50 || result.transition_turns != 3 ||
        result.transition_angle != 5000 || result.menu_rotation_step != 750 ||
        result.menu_focus_enter_step != 1840 || result.menu_focus_exit_step != 1400 ||
        result.menu_focus_distance != -70 || result.menu_focus_twist != 0 ||
        result.menu_glass_min_alpha != 0) {
        gc_startup_destroy(&result);
        return false;
    }
    unsigned previous = GC_STARTUP_PHASE_COUNT;
    for (unsigned tick = 0; tick < GC_STARTUP_SAMPLE_TICK_LIMIT; ++tick) {
        GcStartupPose pose;
        if (!gc_startup_sample(&result, tick, &pose)) {
            gc_startup_destroy(&result);
            return false;
        }
        if ((unsigned)pose.phase != previous) {
            result.phase_start_ticks[pose.phase] = tick;
            previous = (unsigned)pose.phase;
        }
        if (pose.complete) {
            result.sequence_ticks = tick;
            break;
        }
    }
    if (!result.sequence_ticks) {
        gc_startup_destroy(&result);
        return false;
    }
    float acceleration = 0, velocity = 0;
    while (velocity <= result.spin_target && result.spin_ticks < 256) {
        acceleration += result.spin_acceleration;
        velocity += acceleration;
        ++result.spin_ticks;
    }
    result.menu_ticks = result.sequence_ticks - 1 + result.spin_ticks +
                        result.transition_ticks[0] + result.transition_ticks[1] +
                        result.transition_ticks[2];
    *startup = result;
    return true;
}

void gc_startup_destroy(GcStartup *startup) {
    if (!startup)
        return;
    gc_ipl_image_destroy(&startup->trail_texture);
    memset(startup, 0, sizeof(*startup));
}
