#include "gamecube/ipl_model.h"

#include <math.h>
#include <string.h>

static float color_input(unsigned selector, unsigned component,
                         const float registers[4][4], const uint8_t texture[4],
                         const uint8_t raster[4]) {
    if (selector < 8)
        return registers[selector / 2][selector % 2 ? 3 : component];
    if (selector == 8)
        return texture[component];
    if (selector == 9)
        return texture[3];
    if (selector == 10)
        return raster[component];
    if (selector == 11)
        return raster[3];
    if (selector == 12)
        return 255;
    if (selector == 13)
        return 128;
    /* The native renderer submits KCSEL_1_4 at USA 0x81306304. */
    if (selector == 14)
        return 64;
    return 0;
}

static float alpha_input(unsigned selector, const float registers[4][4],
                         const uint8_t texture[4], const uint8_t raster[4]) {
    if (selector < 4)
        return registers[selector][3];
    if (selector == 4)
        return texture[3];
    if (selector == 5)
        return raster[3];
    /* Native KASEL_1. */
    if (selector == 6)
        return 255;
    return 0;
}

static bool combine(const uint8_t *program, unsigned offset, const float inputs[4],
                    float *output) {
    unsigned operation = program[offset];
    unsigned bias = program[offset + 1];
    unsigned scale = program[offset + 2];
    unsigned clamp = program[offset + 3];
    if (operation > 1 || bias > 2 || scale > 3 || clamp > 1)
        return false;
    float weight = inputs[2] / 255.0f;
    float blend = inputs[0] * (1 - weight) + inputs[1] * weight;
    float value = inputs[3] + (operation ? -blend : blend);
    if (bias == 1)
        value += 128;
    else if (bias == 2)
        value -= 128;
    static const float factors[4] = {1, 2, 4, 0.5f};
    value *= factors[scale];
    float minimum = clamp ? 0 : -1024;
    float maximum = clamp ? 255 : 1023;
    *output = fmaxf(minimum, fminf(maximum, value));
    return true;
}

bool gc_ipl_material_shade(const GcIplMaterial *material, const uint8_t texture[4],
                           const uint8_t raster[4], uint8_t output[4]) {
    if (!material || !texture || !raster || !output || material->stage_count > 8)
        return false;
    float registers[4][4] = {{0}};
    for (unsigned index = 0; index < 3; ++index)
        for (unsigned component = 0; component < 4; ++component)
            registers[index + 1][component] = material->registers[index][component];
    for (unsigned stage = 0; stage < material->stage_count; ++stage) {
        const uint8_t *program = material->stages[stage];
        if (program[0] < 5) {
            /* The five GXSetTevOp modes are permitted by the native parser. */
            for (unsigned component = 0; component < 4; ++component) {
                float texel = texture[component];
                float color = raster[component];
                if (program[0] == 0)
                    registers[0][component] = texel * color / 255;
                else if (program[0] == 1)
                    registers[0][component] = component == 3
                                                  ? color
                                                  : color * (1 - texture[3] / 255.0f) +
                                                        texel * texture[3] / 255.0f;
                else if (program[0] == 2)
                    registers[0][component] =
                        component == 3 ? texel * color / 255
                                       : color * (1 - texel / 255.0f) +
                                             registers[1][component] * texel / 255.0f;
                else
                    registers[0][component] = program[0] == 3 ? texel : color;
            }
            continue;
        }
        if (program[9] > 3 || program[18] > 3)
            return false;
        float results[4];
        for (unsigned component = 0; component < 3; ++component) {
            float inputs[4];
            for (unsigned input = 0; input < 4; ++input) {
                if (program[input + 1] > 15)
                    return false;
                inputs[input] = color_input(program[input + 1], component, registers,
                                            texture, raster);
            }
            if (!combine(program, 5, inputs, &results[component]))
                return false;
        }
        float inputs[4];
        for (unsigned input = 0; input < 4; ++input) {
            if (program[input + 10] > 7)
                return false;
            inputs[input] =
                alpha_input(program[input + 10], registers, texture, raster);
        }
        if (!combine(program, 14, inputs, &results[3]))
            return false;
        for (unsigned component = 0; component < 3; ++component)
            registers[program[9]][component] = results[component];
        registers[program[18]][3] = results[3];
    }
    for (unsigned component = 0; component < 4; ++component)
        output[component] =
            (uint8_t)fmaxf(0, fminf(255, roundf(registers[0][component])));
    return true;
}
