#include "console_common/support/affine.h"
#include "gamecube/face_geometry.h"
#include "console_common/support/endian.h"
#include "gamecube/angle.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static bool card_valid(const GcFaceGeometry *geometry);

bool gc_face_random_next(GcFaceRandom *random, uint32_t time_base, uint32_t *value) {
    if (!random || !value)
        return false;
    /* USA 07fe8: two independent LCG/LFSR pairs, selected by mftb bit8.
     * Unsigned arithmetic intentionally reproduces the PPC word wrap.
     */
    unsigned stream = time_base & 256 ? 0u : 2u;
    uint32_t increment = stream ? UINT32_C(0x10932) : UINT32_C(0x3039);
    random->values[stream] = random->values[stream] * UINT32_C(0x41c64e6d) + increment;
    uint32_t shift = random->values[stream + 1];
    if (shift & 1)
        shift ^= UINT32_C(0x11020);
    random->values[stream + 1] = shift >> 1;
    *value = stream ? random->values[stream] + random->values[stream + 1]
                    : random->values[stream] ^ random->values[stream + 1];
    return true;
}

bool gc_face_random_init(const uint32_t time_base_samples[5], GcFaceRandom *random) {
    if (!time_base_samples || !random)
        return false;
    memcpy(random->values, time_base_samples, sizeof(random->values));
    uint32_t discarded;
    return gc_face_random_next(random, time_base_samples[4], &discarded);
}

/* Recover stores from the verified native argument-r3 initializers. Only
 * immediate integer and r2 constant-pool loads contribute output values.
 * USA 14b04/206a0 correspond to PAL 15800/21778.
 */
static bool initializer(const uint8_t *rom, size_t code, size_t r2,
                        uint8_t output[128]) {
    uint32_t values[32] = {0}, floats[32] = {0};
    bool known[32] = {false}, float_known[32] = {false};
    for (unsigned i = 0; i < 128; ++i) {
        if (code + 4 > GC_IPL_SCRAMBLED_END)
            return false;
        uint32_t word = cc_read_be32(rom + code);
        code += 4;
        if (word == UINT32_C(0x4e800020))
            return true;
        unsigned op = word >> 26, target = word >> 21 & 31, base = word >> 16 & 31;
        int displacement = (int16_t)word;
        if (op == 14 && !base) {
            values[target] = (uint32_t)displacement;
            known[target] = true;
        } else if (op == 48 && base == 2) {
            int64_t address = (int64_t)r2 + displacement;
            if (address < 0 || address + 4 > (int64_t)GC_IPL_SCRAMBLED_END)
                return false;
            floats[target] = cc_read_be32(rom + (size_t)address);
            float_known[target] = true;
        } else if (base == 3 && displacement >= 0 && displacement <= 124) {
            uint8_t *p = output + displacement;
            if (op == 36 && known[target])
                cc_write_be32(p, values[target]);
            else if (op == 44 && known[target])
                cc_write_be16(p, (uint16_t)values[target]);
            else if (op == 52 && float_known[target])
                cc_write_be32(p, floats[target]);
        }
    }
    return false;
}

static bool valid(const GcFaceGeometry *geometry) {
    if (!geometry || (geometry->frame_rate != 50 && geometry->frame_rate != 60) ||
        !isfinite(geometry->memory_scale) || geometry->memory_scale <= 0 ||
        !isfinite(geometry->memory_inner_scale) || geometry->memory_inner_scale <= 0 ||
        !isfinite(geometry->options_wave_height) ||
        !isfinite(geometry->options_wave_duration) ||
        geometry->options_wave_duration <= 0 || !geometry->options_exit_step ||
        !geometry->options_exit_fade || !geometry->memory_exit_step ||
        !geometry->memory_exit_fade || !geometry->gameplay_radius ||
        !isfinite(geometry->gameplay_scale) || geometry->gameplay_scale <= 0 ||
        !geometry->gameplay_exit_step || !geometry->gameplay_exit_fade ||
        !isfinite(geometry->calendar_scale) || geometry->calendar_scale <= 0 ||
        !isfinite(geometry->calendar_hand_scale) ||
        geometry->calendar_hand_scale <= 0 || !geometry->calendar_exit_step ||
        !geometry->calendar_exit_fade)
        return false;
    unsigned selected = 0;
    for (unsigned i = 0; i < 72; ++i) {
        if (geometry->options_mask[i] > 1)
            return false;
        selected += geometry->options_mask[i];
    }
    for (unsigned i = 0; i < 4; ++i)
        if (geometry->options_corners[i] >= 30 ||
            geometry->options_corner_order[i] >= 30 ||
            geometry->options_corner_start[i] >= 4 ||
            !isfinite(geometry->options_edges[i]))
            return false;
    for (unsigned i = 0; i < 3; ++i)
        if (!isfinite(geometry->memory_turns[i]))
            return false;
    return selected == 31;
}

static bool pool_float(const uint8_t *rom, size_t instruction, size_t r2,
                       float *value) {
    if (instruction > GC_IPL_SCRAMBLED_END - 4)
        return false;
    uint32_t word = cc_read_be32(rom + instruction);
    if (word >> 26 != 48 || (word >> 16 & 31) != 2)
        return false;
    int64_t address = (int64_t)r2 + (int16_t)word;
    if (address < 0 || address + 4 > (int64_t)GC_IPL_SCRAMBLED_END)
        return false;
    *value = cc_read_be_float(rom + (size_t)address);
    return isfinite(*value);
}

/* Native card palette lookup USA 1fcf8 / PAL 20dd0 returns four signed
 * TEV color pointers per sector size. Read the verified r13-relative operands
 * rather than placing original palette bytes in the source tree.
 */
static bool recover_card_parameters(const uint8_t *rom, bool pal, size_t r2,
                                    const uint8_t memory[128],
                                    GcFaceGeometry *geometry) {
    size_t code = (pal ? 0x20dd0 : 0x1fcf8) + GC_IPL_BS2_OFFSET;
    size_t r13 = (pal ? 0x1b4fc0 : 0x165320) + GC_IPL_BS2_OFFSET;
    unsigned count = 0;
    for (unsigned i = 0; i < 130; ++i) {
        uint32_t word = cc_read_be32(rom + code + i * 4);
        if (word >> 26 != 14 || (word >> 21 & 31) != 3 || (word >> 16 & 31) != 13)
            continue;
        int64_t address = (int64_t)r13 + (int16_t)word;
        if (address < 0 || address + 8 > (int64_t)GC_IPL_SCRAMBLED_END || count >= 24)
            return false;
        for (unsigned channel = 0; channel < 4; ++channel)
            geometry->card_colors[count / 4][count % 4][channel] =
                (int16_t)cc_read_be16(rom + (size_t)address + channel * 2);
        ++count;
    }
    geometry->card_min_scale = cc_read_be_float(memory + 0x4c);
    geometry->card_max_scale = cc_read_be_float(memory + 0x48);
    geometry->card_hover_offset[0] = cc_read_be_float(memory + 0x64);
    geometry->card_hover_offset[1] = cc_read_be_float(memory + 0x68);
    code = (pal ? 0x1f770 : 0x1e698) + GC_IPL_BS2_OFFSET;
    bool okay = pool_float(rom, code + 0xb8, r2, &geometry->card_particle_radius) &&
                pool_float(rom, code + 0xb0, r2, &geometry->card_particle_start) &&
                pool_float(rom, code + 0xac, r2, &geometry->card_particle_growth);
    uint32_t high = cc_read_be32(rom + code + 0xf8),
             low = cc_read_be32(rom + code + 0xfc);
    if (high >> 26 != 15 || (high >> 16 & 31) != 0 || low >> 26 != 14 ||
        (low >> 16 & 31) != (high >> 21 & 31))
        return false;
    int64_t address = (int64_t)(high & 65535) * 65536 + (int16_t)low -
                      (int64_t)GC_IPL_BS2_ADDRESS + (int64_t)GC_IPL_BS2_OFFSET;
    if (address < 0 || address + 12 > (int64_t)GC_IPL_SCRAMBLED_END)
        return false;
    for (unsigned i = 0; i < 3; ++i)
        geometry->card_particle_scale[i] =
            cc_read_be_float(rom + (size_t)address + i * 4);
    return okay && count == 24;
}

static bool grid_valid(const GcMenuGridLighting *light) {
    if (!light || !isfinite(light->reference_distance) ||
        light->reference_distance <= 0 || !isfinite(light->reference_brightness) ||
        light->reference_brightness <= 0 || light->reference_brightness >= 1 ||
        !isfinite(light->cutoff_degrees) || light->cutoff_degrees <= 0 ||
        light->cutoff_degrees >= 90)
        return false;
    for (unsigned i = 0; i < 3; ++i)
        if (!isfinite(light->position[i]))
            return false;
    return light->position[2] > 0;
}

/* USA menu initializer 109ac / PAL 11464 stores six grid-light floats
 * through r31, at +154 / +18c. Read only the native r2 constant operands.
 */
static bool recover_grid_parameters(const uint8_t *rom, bool pal, size_t r2,
                                    GcMenuGridLighting *light) {
    size_t code = (pal ? 0x11464 : 0x109ac) + GC_IPL_BS2_OFFSET;
    unsigned first = pal ? 0x18c : 0x154;
    float registers[32] = {0}, fields[6] = {0};
    bool known[32] = {false};
    unsigned found = 0;
    for (unsigned i = 0; i < 512; ++i) {
        if (code + 4 > GC_IPL_SCRAMBLED_END)
            return false;
        uint32_t word = cc_read_be32(rom + code);
        code += 4;
        unsigned op = word >> 26, target = word >> 21 & 31, base = word >> 16 & 31;
        if (op == 48 && base == 2) {
            if (!pool_float(rom, code - 4, r2, &registers[target]))
                return false;
            known[target] = true;
        } else if (op == 52 && base == 31 && known[target]) {
            unsigned offset = word & 65535;
            if (offset >= first && offset < first + 24 && !(offset % 4)) {
                unsigned field = (offset - first) / 4;
                fields[field] = registers[target];
                found |= 1u << field;
            }
        }
        if (found == 63)
            break;
        if (word == UINT32_C(0x4e800020))
            return false;
    }
    if (found != 63)
        return false;
    light->reference_distance = fields[0];
    light->reference_brightness = fields[1];
    memcpy(light->position, fields + 2, sizeof(light->position));
    light->cutoff_degrees = fields[5];
    return grid_valid(light);
}

static bool recover_arrow_parameters(const uint8_t *rom, bool pal, size_t r2,
                                     GcFaceGeometry *geometry) {
    /* USA card init 14ea8 / PAL 15bcc stores the counter maximum through
     * r13; draw 18d58 / 19d70 loads the Hermite endpoint from the r2 pool.
     */
    size_t code = (pal ? 0x15bcc : 0x14ea8) + GC_IPL_BS2_OFFSET;
    int target_offset = pal ? -0x7b18 : -0x7b60;
    uint32_t registers[32] = {0};
    bool known[32] = {false};
    bool found = false;
    for (unsigned i = 0; i < 512; ++i) {
        uint32_t word = cc_read_be32(rom + code + i * 4);
        unsigned op = word >> 26, target = word >> 21 & 31, base = word >> 16 & 31;
        if (op == 14 && !base) {
            registers[target] = (uint32_t)(int32_t)(int16_t)word;
            known[target] = true;
        } else if (op == 44 && base == 13 && (int16_t)word == target_offset &&
                   known[target]) {
            geometry->card_arrow_ticks = registers[target];
            found = true;
            break;
        }
        if (word == UINT32_C(0x4e800020))
            break;
    }
    code = (pal ? 0x19d70 : 0x18d58) + GC_IPL_BS2_OFFSET;
    float endpoint;
    if (!found || !geometry->card_arrow_ticks || geometry->card_arrow_ticks > 32767 ||
        !pool_float(rom, code + 0x10c, r2, &endpoint) || endpoint < 0 ||
        endpoint > 32767)
        return false;
    geometry->card_arrow_amplitude = endpoint / 16;
    return true;
}

bool gc_menu_grid_color(const GcFaceGeometry *geometry, float logical_x,
                        float logical_y, uint8_t page_alpha, float rgba[4]) {
    return geometry && gc_menu_grid_light_color(&geometry->grid_lighting, logical_x,
                                                logical_y, page_alpha, rgba);
}

bool gc_menu_grid_light_color(const GcMenuGridLighting *light, float logical_x,
                              float logical_y, uint8_t light_alpha, float rgba[4]) {
    if (!rgba || !isfinite(logical_x) || !isfinite(logical_y) || !grid_valid(light))
        return false;
    float x = logical_x - light->position[0];
    float y = logical_y - light->position[1];
    float z = light->position[2];
    float distance = sqrtf(x * x + y * y + z * z);
    if (!isfinite(distance) || distance <= 0)
        return false;
    /* USA aee8 sets alpha-channel CLAMP diffuse and SPOT attenuation;
     * native GXInitLightSpot 4ba94 uses enum3's quadratic cosine, and
     * 4bc14 uses GENTLE. The bb34 branch sets (0, -c/(1-c), 1/(1-c)).
     * Color lighting is disabled. Normals are +Z, light direction is -Z.
     */
    float cosine = z / distance;
    float cutoff = cosf(light->cutoff_degrees * 0.01745329251994329577f);
    float spot = fmaxf(cosine * (cosine - cutoff) / (1 - cutoff), 0);
    float slope = (1 - light->reference_brightness) /
                  (light->reference_brightness * light->reference_distance);
    float alpha = (float)light_alpha / 255 * spot * cosine / (1 + slope * distance);
    rgba[0] = rgba[1] = rgba[2] = 1;
    rgba[3] = fminf(alpha, 1);
    return true;
}

bool gc_face_geometry_load(const char *ipl_path, GcFaceGeometry *geometry) {
    if (!ipl_path || !geometry)
        return false;
    uint8_t *rom = NULL;
    if (!gc_ipl_rom_read(ipl_path, &rom))
        return false;
    if (cc_read_be32(rom + GC_IPL_SCRAMBLED_START) != UINT32_C(0x3c800011))
        gc_ipl_descramble(rom, GC_IPL_ROM_SIZE);
    bool pal = cc_read_be32(rom + 0x82040) == 185;
    if (!pal && cc_read_be32(rom + 0x5f240) != 82) {
        free(rom);
        return false;
    }
    size_t r2 = (pal ? 0x1b5b20 : 0x165cc0) + GC_IPL_BS2_OFFSET;
    uint8_t memory[128] = {0}, options[128] = {0};
    uint8_t gameplay[128] = {0}, calendar[128] = {0};
    bool okay =
        initializer(rom, (pal ? 0x15800 : 0x14b04) + GC_IPL_BS2_OFFSET, r2, memory) &&
        initializer(rom, (pal ? 0x21778 : 0x206a0) + GC_IPL_BS2_OFFSET, r2, options) &&
        initializer(rom, (pal ? 0x1576c : 0x14a70) + GC_IPL_BS2_OFFSET, r2, gameplay) &&
        initializer(rom, (pal ? 0x21838 : 0x20740) + GC_IPL_BS2_OFFSET, r2, calendar);
    GcFaceGeometry result = {0};
    result.frame_rate = pal ? 50 : 60;
    size_t pool = r2 - (pal ? 0x7b50 : 0x7ba8);
    for (unsigned i = 0; i < 4; ++i) {
        result.options_color[i] = (int16_t)cc_read_be32(options + i * 4);
        result.memory_color[i] = (int16_t)cc_read_be32(memory + i * 4);
        result.gameplay_color[i] = (int16_t)cc_read_be32(gameplay + i * 4);
        result.calendar_colors[0][i] = (int16_t)cc_read_be16(calendar + i * 2);
        result.calendar_colors[1][i] = (int16_t)cc_read_be16(calendar + 8 + i * 2);
        result.options_edges[i] = cc_read_be_float(rom + pool + i * 4);
    }
    memcpy(result.options_mask, rom + (pal ? 0x81450 : 0xa5d70) + GC_IPL_BS2_OFFSET,
           sizeof(result.options_mask));
    memcpy(result.options_corners, rom + pool + 32, 4);
    memcpy(result.options_corner_order, rom + pool + 36, 4);
    memcpy(result.options_corner_start, rom + pool + 40, 4);
    result.options_wave_height = cc_read_be_float(rom + pool + 56);
    result.options_wave_duration = cc_read_be_float(rom + pool + 52);
    result.memory_scale = cc_read_be_float(memory + 16);
    result.memory_inner_scale = cc_read_be_float(rom + r2 - (pal ? 0x7b64 : 0x7bbc));
    for (unsigned i = 0; i < 3; ++i)
        result.memory_turns[i] =
            cc_read_be_float(rom + r2 - (pal ? 0x7b70 : 0x7bc8) + i * 4);
    result.options_exit_step = cc_read_be16(options + 30);
    result.options_exit_fade = cc_read_be16(options + 32);
    result.memory_exit_step = cc_read_be16(memory + 94);
    result.memory_exit_fade = cc_read_be16(memory + 96);
    result.gameplay_radius = cc_read_be32(gameplay + 16);
    result.gameplay_scale = cc_read_be_float(gameplay + 20);
    result.gameplay_exit_step = cc_read_be16(gameplay + 26);
    result.gameplay_exit_fade = cc_read_be16(gameplay + 28);
    result.calendar_scale = cc_read_be_float(rom + r2 - (pal ? 0x7af8 : 0x7b50));
    result.calendar_hand_scale = cc_read_be_float(rom + r2 - (pal ? 0x7af0 : 0x7b48));
    result.calendar_exit_step = cc_read_be16(calendar + 34);
    result.calendar_exit_fade = cc_read_be16(calendar + 36);
    okay = recover_card_parameters(rom, pal, r2, memory, &result) && okay;
    okay = recover_grid_parameters(rom, pal, r2, &result.grid_lighting) && okay;
    okay = recover_arrow_parameters(rom, pal, r2, &result) && okay;
    free(rom);
    if (!okay || !valid(&result) || !card_valid(&result))
        return false;
    *geometry = result;
    return true;
}

static uint8_t alpha_add(uint8_t alpha, int step) {
    int result = alpha + step;
    return (uint8_t)(result < 0 ? 0 : result > 255 ? 255 : result);
}

static float hermite(float time, float duration, float first, float tangent,
                     float last) {
    float t = time / duration, t2 = t * t, t3 = t2 * t;
    return (2 * t3 - 3 * t2 + 1) * first + (t3 - 2 * t2 + t) * duration * tangent +
           (3 * t2 - 2 * t3) * last;
}

bool gc_card_arrow_offset(const GcFaceGeometry *geometry, uint64_t ticks,
                          float *offset) {
    if (!geometry || !offset || !geometry->card_arrow_ticks ||
        geometry->card_arrow_ticks > 32767 ||
        !isfinite(geometry->card_arrow_amplitude) ||
        geometry->card_arrow_amplitude < 0 ||
        geometry->card_arrow_amplitude > 32767.0f / 16)
        return false;
    unsigned half_period = geometry->card_arrow_ticks + 1;
    unsigned phase = (unsigned)(ticks % (2u * half_period));
    unsigned counter = phase < half_period ? phase : 2u * half_period - 1u - phase;
    float fixed = hermite((float)counter, (float)geometry->card_arrow_ticks, 0, 0,
                          geometry->card_arrow_amplitude * 16);
    *offset = truncf(fixed) / 16;
    return true;
}

static float clamp_magnitude(float value, float maximum) {
    return value > maximum ? maximum : value < -maximum ? -maximum : value;
}

static float toward_zero(float value, unsigned step) {
    return value > 0 ? fmaxf(value - (float)step, 0) : fminf(value + (float)step, 0);
}

static bool memory_inner(unsigned index) {
    return (index >= 7 && index <= 10) || (index >= 13 && index <= 16) ||
           (index >= 19 && index <= 22);
}

static unsigned memory_distance(unsigned index) {
    unsigned x = index % 6, y = index / 6;
    return x + y;
}

static void options_corner_path(const GcFaceGeometry *geometry, unsigned elapsed,
                                float targets[61][2]) {
    for (unsigned i = 0; i < 4; ++i) {
        unsigned index = geometry->options_corner_order[i];
        unsigned start = geometry->options_corner_start[i];
        int x0 = (int)(start % 2) * 160 - 80;
        int y0 = 70 - (int)(start / 2) * 140;
        int x1 = (int)(i % 2) * 160 - 80;
        int y1 = 70 - (int)(i / 2) * 140;
        targets[index][0] =
            hermite((float)elapsed, 30, (float)x0, (float)((x1 - x0) / 30), (float)x1);
        targets[index][1] =
            hermite((float)elapsed, 30, (float)y0, (float)((y1 - y0) / 30), (float)y1);
    }
}

static void options_targets(const GcFaceGeometry *geometry, float targets[61][2]) {
    memset(targets, 0, 61 * 2 * sizeof(float));
    for (unsigned i = 0; i < 30; ++i) {
        if (i >= 1 && i <= 6) {
            targets[i][0] = geometry->options_edges[0];
            targets[i][1] = 70 - (float)i * 20;
        } else if (i >= 8 && i <= 14) {
            targets[i][0] = (float)(15 - i) * 20 - 80;
            targets[i][1] = geometry->options_edges[1];
        } else if (i >= 16 && i <= 21) {
            targets[i][0] = geometry->options_edges[2];
            targets[i][1] = 70 - (float)(22 - i) * 20;
        } else if (i >= 23 && i <= 29) {
            targets[i][0] = (float)(i - 22) * 20 - 80;
            targets[i][1] = geometry->options_edges[3];
        }
    }
    /* USA 23170 / PAL 24268 retain the last entrance sample in the base
     * records. The next phase copies it; it never samples the endpoint at 30
     * or resets the four corner positions to their initial zero values. */
    options_corner_path(geometry, 29, targets);
    unsigned cell = 30;
    for (unsigned i = 0; i < 72; ++i)
        if (geometry->options_mask[i]) {
            targets[cell][0] = (float)(i % 9) * 20 - 80;
            targets[cell++][1] = 70 - (float)(i / 9) * 20;
        }
}

bool gc_face_geometry_init(const GcFaceGeometry *geometry,
                           const uint8_t memory_arrival_delay[GC_FACE_MEMORY_POINTS],
                           GcFaceGeometryState *state) {
    if (!state || !valid(geometry))
        return false;
    memset(state, 0, sizeof(*state));
    state->gameplay_steps[0] = 100;
    state->gameplay_steps[1] = 50;
    if (memory_arrival_delay) {
        for (unsigned i = 0; i < GC_FACE_MEMORY_POINTS; ++i) {
            if (memory_arrival_delay[i] > 31)
                return false;
            state->memory_arrival_delay[i] = memory_arrival_delay[i];
        }
    }
    return true;
}

bool gc_face_geometry_set_clock(GcFaceGeometryState *state, const gc_date_time *clock) {
    if (!state || !clock || clock->hour < 0 || clock->hour > 23 || clock->minute < 0 ||
        clock->minute > 59 || clock->second < 0 || clock->second > 59)
        return false;
    state->clock = *clock;
    return true;
}

/* Options USA 24454, PAL 2554c. After the entrance, the original loops
 * tick 70 through 599 inclusive. All timing is in regional video ticks.
 */
static void options_update(const GcFaceGeometry *geometry, GcFaceGeometryState *state) {
    unsigned tick = state->options_tick;
    float targets[61][2];
    options_targets(geometry, targets);
    GcFaceGeometryCell *cells = state->options;
    if (tick < 20) {
        for (unsigned i = 0; i < 4; ++i) {
            unsigned index = geometry->options_corners[i];
            targets[index][0] = targets[index][1] = 0;
            cells[index].alpha = alpha_add(cells[index].alpha, 6);
        }
    } else if (tick < 40) {
        for (unsigned i = 0; i < 4; ++i) {
            unsigned index = geometry->options_corners[i];
            int x = (int)(i % 2) * 160 - 80;
            int y = 70 - (int)(i / 2) * 140;
            targets[index][0] =
                hermite((float)(tick - 20), 20, 0, (float)(x / 20), (float)x);
            targets[index][1] =
                hermite((float)(tick - 20), 20, 0, (float)(y / 20), (float)y);
            cells[index].alpha = alpha_add(cells[index].alpha, 6);
        }
    } else if (tick < 70) {
        options_corner_path(geometry, tick - 40, targets);
        for (unsigned i = 0; i < 4; ++i) {
            unsigned index = geometry->options_corner_order[i];
            cells[index].alpha = alpha_add(cells[index].alpha, 6);
            if (i == 0) {
                int end = (int)truncf((70 - targets[index][1]) / 20);
                for (int edge = 7; edge > end; --edge)
                    cells[22 - edge].alpha = 255;
            } else if (i == 1) {
                int end = (int)truncf((targets[index][0] + 80) / 20);
                for (int edge = 0; edge < end; ++edge)
                    cells[23 + edge].alpha = 255;
            } else if (i == 2) {
                int end = (int)truncf((targets[index][0] + 80) / 20);
                for (int edge = 8; edge > end; --edge)
                    cells[15 - edge].alpha = 255;
            } else {
                int end = (int)truncf((70 - targets[index][1]) / 20);
                for (int edge = 0; edge < end; ++edge)
                    cells[1 + edge].alpha = 255;
            }
        }
    }
    for (unsigned i = 0; i < 61; ++i) {
        cells[i].position[0] = targets[i][0];
        cells[i].position[1] = targets[i][1];
    }
    if (tick >= 70 && tick < 225) {
        for (unsigned i = 0; i < 30; ++i) {
            int time = (int)(tick - 70) - (int)i * 5;
            if (time < 0 || time > 10)
                continue;
            int angle = time * 16384 / 10;
            cells[i].angles[0] = cells[i].angles[1] = 0;
            if (i < 7)
                cells[i].angles[0] = (int16_t)angle;
            else if (i < 15)
                cells[i].angles[1] = (int16_t)-angle;
            else if (i < 22)
                cells[i].angles[0] = (int16_t)-angle;
            else
                cells[i].angles[1] = (int16_t)angle;
        }
    } else if (tick >= 225 && tick < 265) {
        float limit = truncf(hermite((float)(tick - 225), 40, 80, 0, 0));
        for (unsigned i = 0; i < 30; ++i)
            cells[i].position[0] = clamp_magnitude(targets[i][0], limit);
    } else if (tick >= 265 && tick < 305) {
        if (tick == 265)
            for (unsigned i = 0; i < 61; ++i)
                cells[i].alpha ^= 255;
        float limit = truncf(hermite((float)(tick - 265), 40, 0, 0, 80));
        for (unsigned i = 30; i < 61; ++i)
            cells[i].position[0] = clamp_magnitude(targets[i][0], limit);
    } else if (tick >= 305 && tick < 519) {
        unsigned time = tick < 397 ? tick - 305 : tick - 397;
        for (unsigned i = 30; i < 61; ++i) {
            int row = (int)truncf(7 - (70 - targets[i][1]) / 20);
            float offset = 0;
            for (unsigned wave = 0; wave < 2; ++wave) {
                int start = (row + (int)wave * 8) * 4;
                int elapsed = (int)time - start;
                float duration = geometry->options_wave_duration;
                if (elapsed >= 0 && (float)elapsed < duration)
                    offset = hermite((float)elapsed, duration, 0, 0,
                                     geometry->options_wave_height);
                else if ((float)elapsed >= duration && (float)elapsed < duration * 2)
                    offset = hermite((float)elapsed - duration, duration,
                                     geometry->options_wave_height, 0, 0);
            }
            cells[i].position[1] = targets[i][1] + offset;
        }
    } else if (tick >= 519 && tick < 559) {
        float limit = truncf(hermite((float)(tick - 519), 40, 70, 0, 0));
        for (unsigned i = 30; i < 61; ++i)
            cells[i].position[1] = clamp_magnitude(targets[i][1], limit);
    } else if (tick >= 559 && tick < 599) {
        if (tick == 559)
            for (unsigned i = 0; i < 61; ++i)
                cells[i].alpha ^= 255;
        float limit = truncf(hermite((float)(tick - 559), 40, 0, 0, 80));
        for (unsigned i = 0; i < 30; ++i)
            cells[i].position[1] = clamp_magnitude(targets[i][1], limit);
    }
    state->options_tick = tick == 599 ? 70 : tick + 1;
    state->options_visible = true;
}

static void memory_pattern(GcFaceGeometryCell cells[36], bool first) {
    /* USA 20e68 updates six columns in dependency order; each new column
     * starts after the previous column's lead cube exceeds alpha 100.
     */
    if (first) {
        cells[7].alpha = alpha_add(cells[7].alpha, 30);
        if (cells[7].alpha > 100) {
            cells[8].alpha = alpha_add(cells[8].alpha, 30);
            cells[13].alpha = alpha_add(cells[13].alpha, 30);
        }
        if (cells[8].alpha > 100) {
            cells[9].alpha = alpha_add(cells[9].alpha, 30);
            cells[14].alpha = alpha_add(cells[14].alpha, 30);
            cells[19].alpha = alpha_add(cells[19].alpha, 30);
        }
        if (cells[9].alpha > 100) {
            cells[10].alpha = alpha_add(cells[10].alpha, 30);
            cells[15].alpha = alpha_add(cells[15].alpha, 30);
            cells[20].alpha = alpha_add(cells[20].alpha, 30);
        }
        if (cells[10].alpha > 100) {
            cells[16].alpha = alpha_add(cells[16].alpha, 30);
            cells[21].alpha = alpha_add(cells[21].alpha, 30);
        }
        if (cells[16].alpha > 100)
            cells[22].alpha = alpha_add(cells[22].alpha, 30);
    } else {
        static const unsigned inner[12] = {7, 8, 13, 9, 14, 19, 10, 15, 20, 16, 21, 22};
        for (unsigned i = 0; i < 12; ++i)
            cells[inner[i]].alpha = alpha_add(cells[inner[i]].alpha, -15);
    }
}

/* Memory USA 21558, PAL 22650; repeat range is 210 through 490.
 * Arrival coordinates truncate to the original signed 16-bit integer units.
 */
static void memory_update(const GcFaceGeometry *geometry, GcFaceGeometryState *state) {
    unsigned tick = state->memory_tick;
    GcFaceGeometryCell *cells = state->memory;
    for (unsigned i = 0; i < 36; ++i) {
        unsigned delay = state->memory_arrival_delay[i];
        float x = (float)(i % 6) * 20 - 50;
        float y = 50 - (float)(i / 6) * 20;
        if (tick <= 113) {
            float fraction = tick <= delay        ? 0
                             : tick >= delay + 20 ? 1
                                                  : (float)(tick - delay) / 20;
            cells[i].position[0] = truncf(hermite(fraction * 20, 20, 0, x / 20, x));
            cells[i].position[1] = truncf(hermite(fraction * 20, 20, 0, y / 20, y));
            cells[i].position[2] = 0;
            if (!memory_inner(i))
                cells[i].alpha = (uint8_t)(fraction * 255);
        }
        int elapsed = -1;
        if (tick >= 150 && tick <= 195)
            elapsed = (int)(tick - 150);
        else if (tick >= 300 && tick <= 345)
            elapsed = (int)(tick - 300);
        else if (tick >= 440 && tick <= 485)
            elapsed = (int)(tick - 430);
        else
            cells[i].angles[0] = cells[i].angles[1] = 0;
        int time = elapsed - (int)memory_distance(i) * 3;
        if (!memory_inner(i) && time >= 0 && time <= 15)
            cells[i].angles[0] = cells[i].angles[1] = (int16_t)(time * 16384 / 15);
    }
    memory_pattern(cells, tick >= 340);
    if (tick >= 210 && tick <= 490) {
        unsigned time = tick - 210;
        float turn;
        if (time < 50)
            turn = hermite((float)time, 50, 0, 0, geometry->memory_turns[0]);
        else if (time < 90)
            turn = hermite((float)(time - 50), 40, geometry->memory_turns[0], 0,
                           geometry->memory_turns[1]);
        else if (time < 140)
            turn = 32768;
        else if (time < 190)
            turn = hermite((float)(time - 140), 50, geometry->memory_turns[1], 0,
                           geometry->memory_turns[2]);
        else if (time < 230)
            turn = hermite((float)(time - 190), 40, geometry->memory_turns[2], 0, 0);
        else
            turn = 0;
        state->memory_turn = (int16_t)(uint16_t)(int32_t)truncf(turn);
    }
    state->memory_tick = tick == 490 ? 210 : tick + 1;
    state->memory_visible = true;
}

static float angle_sine(int value) {
    return gc_angle_sine(value);
}

static float angle_cosine(int value) {
    return gc_angle_cosine(value);
}

/* Gameplay USA 222d0, PAL 233c8. Sixteen cubes emerge at ten-tick
 * intervals into a rotating ring, then its plane rocks on two axes.
 */
static void gameplay_update(const GcFaceGeometry *geometry,
                            GcFaceGeometryState *state) {
    unsigned tick = state->gameplay_tick;
    for (unsigned i = 0; i < 16; ++i) {
        unsigned first = i * 10;
        unsigned time = tick <= first ? 0 : tick - first;
        if (time > 20)
            time = 20;
        unsigned radius = geometry->gameplay_radius * time / 20;
        int angle = (int)i * 4096 - state->gameplay_phase;
        state->gameplay[i].position[0] = (float)radius * angle_sine(angle);
        state->gameplay[i].position[1] = (float)radius * angle_cosine(angle);
        state->gameplay[i].alpha = (uint8_t)(time * 255 / 20);
    }
    state->gameplay_phase = (uint16_t)(state->gameplay_phase + 409);
    state->gameplay_tick = tick < 300 ? tick + 1 : 300;
    if (tick >= 300) {
        int x = state->gameplay_angles[0] + state->gameplay_steps[0];
        int z = state->gameplay_angles[2] + state->gameplay_steps[1];
        state->gameplay_angles[0] = (int16_t)x;
        state->gameplay_angles[2] = (int16_t)z;
        if (x > 32000)
            state->gameplay_steps[0] = -100;
        if (x < 1000)
            state->gameplay_steps[0] = 100;
        if (z > 8192)
            state->gameplay_steps[1] = -50;
        if (z < -8192)
            state->gameplay_steps[1] = 50;
        int phase = (int16_t)state->gameplay_phase;
        if (phase > 4096)
            state->gameplay_phase = (uint16_t)(phase % 4096);
    }
    state->gameplay_visible = true;
}

/* Calendar USA 251ec, PAL 262e4: five cardinal markers, a four-cube
 * minute hand, three-cube hour hand, and one orbiting second marker.
 * Integer division preserves the native 16-bit angle quantization.
 */
static void calendar_update(const GcFaceGeometry *geometry,
                            GcFaceGeometryState *state) {
    (void)geometry;
    if (state->calendar_tick < 50)
        ++state->calendar_tick;
    unsigned first = state->calendar_tick < 30 ? state->calendar_tick : 30;
    unsigned hands = state->calendar_tick > 40 ? state->calendar_tick - 40 : 0;
    for (unsigned i = 0; i < 5; ++i) {
        int angle = ((int)i - 1) * 16384;
        float radius = i ? 100 : 0;
        state->calendar[i].position[0] = radius * angle_sine(angle);
        state->calendar[i].position[1] = radius * angle_cosine(angle);
        state->calendar[i].alpha = (uint8_t)(first * 255 / 30);
    }
    int minute = -state->clock.minute * 65536 / 60 - state->clock.second * 65536 / 3600;
    int hour =
        -(state->clock.hour % 12) * 65536 / 12 - state->clock.minute * 65536 / 720;
    int second = -state->clock.second * 65536 / 60 - 3;
    for (unsigned i = 0; i < 7; ++i) {
        float radius = (float)((i < 4 ? i + 1 : i - 3) * 20 * hands) / 10;
        int angle = i < 4 ? minute : hour;
        state->calendar[i + 5].position[0] = radius * angle_sine(angle);
        state->calendar[i + 5].position[1] = radius * angle_cosine(angle);
        state->calendar[i + 5].alpha = (uint8_t)(hands * 255 / 10);
    }
    state->calendar[12].position[0] = 100 * angle_sine(second);
    state->calendar[12].position[1] = 100 * angle_cosine(second);
    state->calendar[12].alpha = (uint8_t)(hands * 255 / 10);
    state->calendar_visible = true;
}

static void fade_cells(GcFaceGeometryCell *cells, size_t count, unsigned step,
                       unsigned fade, bool collapse) {
    for (size_t i = 0; i < count; ++i) {
        cells[i].alpha = alpha_add(cells[i].alpha, -(int)fade);
        if (collapse)
            for (unsigned axis = 0; axis < 3; ++axis)
                cells[i].position[axis] = toward_zero(cells[i].position[axis], step);
    }
}

bool gc_face_geometry_update(const GcFaceGeometry *geometry, GcFaceGeometryState *state,
                             bool selected, gc_face face, bool editing) {
    if (!state || !valid(geometry) || (unsigned)face > GC_FACE_OPTIONS ||
        state->options_tick > 599 || state->memory_tick > 490 ||
        state->gameplay_tick > 300 || state->calendar_tick > 50)
        return false;
    bool options = selected && face == GC_FACE_OPTIONS;
    bool memory = selected && face == GC_FACE_MEMORY_CARD;
    bool gameplay = selected && face == GC_FACE_GAME_PLAY;
    bool calendar = selected && face == GC_FACE_CALENDAR;
    if (options && !editing) {
        if (!state->options_visible)
            memset(state->options, 0, sizeof(state->options));
        options_update(geometry, state);
    } else {
        fade_cells(state->options, 61, geometry->options_exit_step,
                   options && editing ? geometry->options_exit_fade : 10,
                   options && editing);
        state->options_tick = 0;
        state->options_visible = false;
    }
    if (memory && !editing) {
        if (!state->memory_visible)
            memset(state->memory, 0, sizeof(state->memory));
        memory_update(geometry, state);
    } else {
        fade_cells(state->memory, 36, geometry->memory_exit_step,
                   memory && editing ? geometry->memory_exit_fade : 10,
                   memory && editing);
        state->memory_tick = 0;
        state->memory_turn = 0;
        state->memory_visible = false;
    }
    if (gameplay && !editing) {
        if (!state->gameplay_visible) {
            memset(state->gameplay, 0, sizeof(state->gameplay));
            state->gameplay_phase = 0;
            memset(state->gameplay_angles, 0, sizeof(state->gameplay_angles));
            state->gameplay_steps[0] = 100;
            state->gameplay_steps[1] = 50;
        }
        gameplay_update(geometry, state);
    } else {
        fade_cells(state->gameplay, 16, geometry->gameplay_exit_step,
                   gameplay && editing ? geometry->gameplay_exit_fade : 20,
                   gameplay && editing);
        state->gameplay_tick = 0;
        state->gameplay_visible = false;
        if (editing)
            memset(state->gameplay_angles, 0, sizeof(state->gameplay_angles));
    }
    if (calendar && !editing) {
        if (!state->calendar_visible)
            memset(state->calendar, 0, sizeof(state->calendar));
        calendar_update(geometry, state);
    } else {
        fade_cells(state->calendar, 13, geometry->calendar_exit_step,
                   calendar && editing ? geometry->calendar_exit_fade : 10,
                   calendar && editing);
        state->calendar_tick = 0;
        state->calendar_visible = false;
    }
    return true;
}

bool gc_face_geometry_editor_ready(const GcFaceGeometryState *state, gc_face face) {
    if (!state)
        return false;
    const GcFaceGeometryCell *cells;
    size_t count;
    switch (face) {
        case GC_FACE_GAME_PLAY:
            cells = state->gameplay;
            count = 16;
            break;
        case GC_FACE_CALENDAR:
            cells = state->calendar;
            count = 13;
            break;
        case GC_FACE_OPTIONS:
            cells = state->options;
            count = 61;
            break;
        case GC_FACE_MEMORY_CARD:
            cells = state->memory;
            count = GC_FACE_MEMORY_POINTS;
            break;
        default:
            return false;
    }
    /* USA 22ac8 / 25ee4 / 24cb8 compare all 32 / 26 / 122 X/Y
     * components with zero before setting each original editor-ready flag.
     */
    for (size_t i = 0; i < count; ++i)
        if (cells[i].position[0] != 0 || cells[i].position[1] != 0 ||
            (face == GC_FACE_MEMORY_CARD &&
             (cells[i].position[2] != 0 || cells[i].alpha != 0)))
            return false;
    return true;
}

static void advance_rock(int16_t *value, int16_t *step, int minimum, int maximum,
                         int increment, uint64_t ticks) {
    unsigned half = (unsigned)((maximum - minimum) / increment);
    unsigned phase = (unsigned)((*value - minimum) / increment);
    if (*step < 0)
        phase = 2 * half - phase;
    phase = (phase + (unsigned)(ticks % (2 * half))) % (2 * half);
    unsigned height = phase <= half ? phase : 2 * half - phase;
    *value = (int16_t)(minimum + (int)height * increment);
    *step = (int16_t)(phase < half ? increment : -increment);
}

uint64_t gc_face_geometry_period(gc_face face) {
    switch (face) {
        case GC_FACE_GAME_PLAY:
            return GC_FACE_GAMEPLAY_PERIOD;
        case GC_FACE_CALENDAR:
            return 1;
        case GC_FACE_MEMORY_CARD:
            return GC_FACE_MEMORY_PERIOD;
        case GC_FACE_OPTIONS:
            return GC_FACE_OPTIONS_PERIOD;
        default:
            return 0;
    }
}

bool gc_face_geometry_advance(const GcFaceGeometry *geometry,
                              GcFaceGeometryState *state, uint64_t ticks, bool selected,
                              gc_face face, bool editing) {
    if (!state || !valid(geometry) || (unsigned)face > GC_FACE_OPTIONS ||
        state->options_tick > 599 || state->memory_tick > 490 ||
        state->gameplay_tick > 300 || state->calendar_tick > 50)
        return false;
    /* Two complete passes settle retained alpha/rotation state, including
     * the memory icon's different first entrance. Thereafter each native
     * loop returns exactly the same state at its corresponding tick.
     */
    uint64_t first = ticks > 1200 ? 1200 : ticks;
    for (uint64_t i = 0; i < first; ++i)
        if (!gc_face_geometry_update(geometry, state, selected, face, editing))
            return false;
    ticks -= first;
    if (selected && !editing && face == GC_FACE_OPTIONS)
        ticks %= GC_FACE_OPTIONS_PERIOD;
    else if (selected && !editing && face == GC_FACE_MEMORY_CARD)
        ticks %= GC_FACE_MEMORY_PERIOD;
    else if (selected && !editing && face == GC_FACE_GAME_PLAY && ticks) {
        advance_rock(&state->gameplay_angles[0], &state->gameplay_steps[0], 900, 32100,
                     100, ticks);
        advance_rock(&state->gameplay_angles[2], &state->gameplay_steps[1], -8200, 8200,
                     50, ticks);
        unsigned phase =
            ((state->gameplay_phase + 4095) % 4096 + (unsigned)(ticks % 4096) * 409) %
                4096 +
            1;
        state->gameplay_phase = (uint16_t)phase;
        unsigned previous = (phase + 4095 - 409) % 4096 + 1;
        for (unsigned i = 0; i < 16; ++i) {
            int angle = (int)i * 4096 - (int)previous;
            state->gameplay[i].position[0] =
                (float)geometry->gameplay_radius * angle_sine(angle);
            state->gameplay[i].position[1] =
                (float)geometry->gameplay_radius * angle_cosine(angle);
        }
        ticks = 0;
    } else
        ticks = 0;
    for (uint64_t i = 0; i < ticks; ++i)
        if (!gc_face_geometry_update(geometry, state, selected, face, editing))
            return false;
    return true;
}

static void matrix(const int16_t angles[3], const float position[3], float m[12]) {
    float sx = gc_angle_sine(angles[0]), cx = gc_angle_cosine(angles[0]);
    float sy = gc_angle_sine(angles[1]), cy = gc_angle_cosine(angles[1]);
    float sz = gc_angle_sine(angles[2]), cz = gc_angle_cosine(angles[2]);
    m[0] = cy * cz;
    m[1] = cz * sx * sy - cx * sz;
    m[2] = sx * sz + cx * cz * sy;
    m[3] = position[0];
    m[4] = cy * sz;
    m[5] = cz * cx + sx * sz * sy;
    m[6] = -cz * sx + cx * sz * sy;
    m[7] = position[1];
    m[8] = -sy;
    m[9] = cy * sx;
    m[10] = cy * cx;
    m[11] = position[2];
}

size_t gc_face_geometry_count(gc_face face) {
    switch (face) {
        case GC_FACE_OPTIONS:
            return 61;
        case GC_FACE_MEMORY_CARD:
            return 36;
        case GC_FACE_GAME_PLAY:
            return 16;
        case GC_FACE_CALENDAR:
            return 13;
    }
    return 0;
}

bool gc_face_geometry_get(const GcFaceGeometry *geometry,
                          const GcFaceGeometryState *state,
                          const GcMenuAnimationPose *menu_pose, gc_face face,
                          size_t index, GcFaceGeometryPoint *point) {
    if (!state || !menu_pose || !point || !valid(geometry) ||
        index >= gc_face_geometry_count(face))
        return false;
    memset(point, 0, sizeof(*point));
    point->model = GC_FACE_MODEL_MENU_CUBE;
    point->register_mask = 1;
    const GcFaceGeometryCell *cell;
    int16_t base[3] = {0};
    float origin[3] = {0}, local[12], face_matrix[12], turn[12];
    float scale;
    if (face == GC_FACE_OPTIONS) {
        cell = state->options + index;
        base[1] = -16384;
        scale = 0.670000017f;
        memcpy(point->registers[0], geometry->options_color,
               sizeof(geometry->options_color));
    } else if (face == GC_FACE_MEMORY_CARD) {
        cell = state->memory + index;
        base[0] = 16384;
        scale = geometry->memory_scale;
        if (memory_inner((unsigned)index))
            scale *= geometry->memory_inner_scale;
        memcpy(point->registers[0], geometry->memory_color,
               sizeof(geometry->memory_color));
    } else if (face == GC_FACE_GAME_PLAY) {
        cell = state->gameplay + index;
        base[0] = -16384;
        scale = geometry->gameplay_scale;
        memcpy(point->registers[0], geometry->gameplay_color,
               sizeof(geometry->gameplay_color));
    } else {
        cell = state->calendar + index;
        base[1] = -16384;
        if (index < 5)
            scale = geometry->calendar_scale *
                    (float)(state->calendar_tick < 30 ? state->calendar_tick : 30) / 30;
        else if (index < 12)
            scale = geometry->calendar_hand_scale;
        else
            scale = geometry->calendar_hand_scale *
                    (float)(state->calendar_tick > 40 ? state->calendar_tick - 40 : 0) /
                    10;
        memcpy(point->registers[0], geometry->calendar_colors[index < 5 ? 1 : 0],
               sizeof(geometry->calendar_colors[0]));
    }
    matrix(base, origin, face_matrix);
    matrix(cell->angles, cell->position, local);
    if (face == GC_FACE_MEMORY_CARD) {
        int16_t angles[3] = {0, state->memory_turn, 0};
        matrix(angles, origin, turn);
        cc_affine_multiply(local, turn, local);
    } else if (face == GC_FACE_GAME_PLAY) {
        matrix(state->gameplay_angles, origin, turn);
        cc_affine_multiply(local, turn, local);
    }
    cc_affine_multiply(local, face_matrix, local);
    cc_affine_multiply(point->matrix, menu_pose->cube_matrix, local);
    point->scale[0] = point->scale[1] = point->scale[2] = scale;
    point->alpha = cell->alpha;
    return true;
}

static bool card_valid(const GcFaceGeometry *geometry) {
    if (!geometry || !isfinite(geometry->card_min_scale) ||
        !isfinite(geometry->card_max_scale) || geometry->card_min_scale <= 0 ||
        geometry->card_max_scale <= geometry->card_min_scale ||
        !isfinite(geometry->card_particle_radius) ||
        geometry->card_particle_radius <= 0 ||
        !isfinite(geometry->card_particle_start) || geometry->card_particle_start < 0 ||
        !isfinite(geometry->card_particle_growth) || geometry->card_particle_growth < 0)
        return false;
    for (unsigned i = 0; i < 2; ++i)
        if (!isfinite(geometry->card_hover_offset[i]))
            return false;
    for (unsigned i = 0; i < 3; ++i)
        if (!isfinite(geometry->card_particle_scale[i]) ||
            geometry->card_particle_scale[i] <= 0)
            return false;
    for (unsigned sector = 0; sector < 6; ++sector)
        for (unsigned color = 0; color < 4; ++color)
            for (unsigned channel = 0; channel < 4; ++channel) {
                int value = geometry->card_colors[sector][color][channel];
                if (value < -1024 || value > 1023)
                    return false;
            }
    return true;
}

static bool card_sector_index(unsigned sector_size, unsigned *index) {
    for (unsigned i = 0; i < 6; ++i)
        if (sector_size == (8192u << i)) {
            *index = i;
            return true;
        }
    return false;
}

static void card_placement(const GcFaceGeometry *geometry, const GcStartup *startup,
                           float center_x, float center_y, bool populated,
                           bool selected, unsigned sector, float scale, float amount,
                           uint16_t phase, uint8_t alpha, GcFaceGeometryPoint *point) {
    memset(point, 0, sizeof(*point));
    point->model = populated ? GC_FACE_MODEL_CARD_COVER : GC_FACE_MODEL_CARD_BASE;
    point->alpha = alpha;
    point->register_mask = 1;
    unsigned color = (populated ? 0u : 2u) + (selected ? 0u : 1u);
    memcpy(point->registers[0], geometry->card_colors[sector][color],
           sizeof(point->registers[0]));
    point->scale[0] = point->scale[1] = point->scale[2] = scale;
    int phase_yz = (int)(int16_t)(uint16_t)((unsigned)phase * 35);
    int phase_x = (int)(int16_t)(uint16_t)((unsigned)phase * 70);
    int16_t angles[3] = {
        (int16_t)(amount * startup->menu_profile[3] * angle_cosine(phase_x)),
        (int16_t)(amount * startup->menu_profile[4] *
                  angle_cosine(phase_yz + startup->menu_profile[10])),
        (int16_t)(amount * startup->menu_profile[5] * angle_cosine(phase_yz))};
    float position[3] = {
        center_x - 292 +
            amount * geometry->card_hover_offset[0] *
                angle_sine(phase_yz + startup->menu_profile[11]),
        224 - center_y - amount * geometry->card_hover_offset[1] * angle_sine(phase_x),
        amount * 2};
    /* USA 1df70: translation * GUI origin * Euler rotation, root scale
     * submitted separately. GUI origin has unit scale and zero rotation.
     */
    matrix(angles, position, point->matrix);
}

bool gc_card_geometry_sample(const GcFaceGeometry *geometry, const GcStartup *startup,
                             float center_x, float center_y, bool populated,
                             bool selected, unsigned sector_size,
                             unsigned selection_tick, uint16_t phase, uint8_t alpha,
                             GcFaceGeometryPoint *point) {
    unsigned sector;
    if (!point || !startup || !card_valid(geometry) || selection_tick > 6 ||
        !isfinite(center_x) || !isfinite(center_y) ||
        !card_sector_index(sector_size, &sector))
        return false;
    float scale = hermite((float)selection_tick, 6, geometry->card_min_scale, 0,
                          geometry->card_max_scale);
    float amount = (scale - geometry->card_min_scale) /
                   (geometry->card_max_scale - geometry->card_min_scale);
    card_placement(geometry, startup, center_x, center_y, populated, selected, sector,
                   scale, amount, phase, alpha, point);
    return true;
}

bool gc_card_geometry_erase(const GcFaceGeometry *geometry, const GcStartup *startup,
                            float center_x, float center_y, float fraction,
                            unsigned sector_size, uint16_t phase, uint8_t alpha,
                            GcFaceGeometryPoint *point) {
    unsigned sector;
    if (!point || !startup || !card_valid(geometry) || !isfinite(fraction) ||
        fraction < 0 || fraction > 1 || !isfinite(center_x) || !isfinite(center_y) ||
        !card_sector_index(sector_size, &sector))
        return false;
    card_placement(geometry, startup, center_x, center_y, true, true, sector,
                   geometry->card_max_scale * fraction, fraction, phase, alpha, point);
    return true;
}

bool gc_card_geometry_particle(const GcFaceGeometry *geometry,
                               const float parent_matrix[12], unsigned sector_size,
                               unsigned tick, unsigned delay, int16_t angle,
                               uint8_t page_alpha, GcFaceGeometryPoint *point) {
    unsigned sector;
    if (!point || !parent_matrix || !card_valid(geometry) || delay > 3 ||
        !card_sector_index(sector_size, &sector))
        return false;
    for (unsigned i = 0; i < 12; ++i)
        if (!isfinite(parent_matrix[i]))
            return false;
    memset(point, 0, sizeof(*point));
    point->model = GC_FACE_MODEL_CARD_COVER;
    point->register_mask = 1;
    memcpy(point->registers[0], geometry->card_colors[sector][1],
           sizeof(point->registers[0]));
    memcpy(point->scale, geometry->card_particle_scale, sizeof(point->scale));
    /* Native 1e698 / draw 202a0: ticks at the delay and at duration have
     * zero visible alpha. Radius starts at a quarter of 48 and expands to48.
     */
    unsigned elapsed = tick > delay ? tick - delay : 0;
    float fraction = elapsed <= 10 ? (float)elapsed / 10 : 0;
    float radius =
        geometry->card_particle_radius *
        (geometry->card_particle_start + geometry->card_particle_growth * fraction);
    if (elapsed && elapsed <= 10)
        point->alpha = (uint8_t)((unsigned)page_alpha * (10 - elapsed) / 10);
    float position[3] = {radius * angle_sine(angle), radius * angle_cosine(angle), 0};
    int16_t angles[3] = {0};
    float local[12];
    matrix(angles, position, local);
    cc_affine_multiply(point->matrix, parent_matrix, local);
    return true;
}
