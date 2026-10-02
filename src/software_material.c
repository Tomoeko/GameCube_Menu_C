#include "software_material.h"

#include <math.h>
#include <string.h>

static float unit(float value) {
    return fmaxf(0, fminf(1, value));
}

static float wrapped_coordinate(float value, unsigned mode) {
    if (mode == 1)
        return value - floorf(value);
    if (mode == 2) {
        value = fmodf(value, 2);
        if (value < 0)
            value += 2;
        return value > 1 ? 2 - value : value;
    }
    return unit(value);
}

static int wrapped_texel(int value, int dimension, unsigned mode) {
    if (mode == 1) {
        value %= dimension;
        return value < 0 ? value + dimension : value;
    }
    return value < 0 ? 0 : value >= dimension ? dimension - 1 : value;
}

void gc_software_sample_texture(const GcSoftwareTexture *image, float u, float v,
                                unsigned wrap_s, unsigned wrap_t, bool nearest,
                                float output[4]) {
    for (unsigned component = 0; component < 4; ++component)
        output[component] = 1;
    if (!image || !image->pixels || !isfinite(u) || !isfinite(v))
        return;
    if (nearest) {
        int x = (int)floorf(wrapped_coordinate(u, wrap_s) * (float)image->width);
        int y = (int)floorf(wrapped_coordinate(v, wrap_t) * (float)image->height);
        x = wrapped_texel(x, image->width, wrap_s);
        y = wrapped_texel(y, image->height, wrap_t);
        size_t offset = ((size_t)y * (size_t)image->width + (size_t)x) * 4;
        for (unsigned component = 0; component < 4; ++component)
            output[component] = (float)image->pixels[offset + component] / 255.0f;
        return;
    }
    float x = wrapped_coordinate(u, wrap_s) * (float)image->width - 0.5f;
    float y = wrapped_coordinate(v, wrap_t) * (float)image->height - 0.5f;
    int left = (int)floorf(x), top = (int)floorf(y);
    float horizontal = x - (float)left, vertical = y - (float)top;
    for (unsigned component = 0; component < 4; ++component) {
        float value = 0;
        for (unsigned row = 0; row < 2; ++row) {
            int ty = wrapped_texel(top + (int)row, image->height, wrap_t);
            for (unsigned column = 0; column < 2; ++column) {
                int tx = wrapped_texel(left + (int)column, image->width, wrap_s);
                size_t offset = ((size_t)ty * (size_t)image->width + (size_t)tx) * 4;
                float weight = (row ? vertical : 1 - vertical) *
                               (column ? horizontal : 1 - horizontal);
                value += (float)image->pixels[offset + component] / 255.0f * weight;
            }
        }
        output[component] = value;
    }
}

static void swizzle(float color[4], unsigned pattern) {
    float input[4];
    memcpy(input, color, sizeof(input));
    for (unsigned component = 0; component < 4; ++component)
        color[component] = input[pattern >> (component * 2) & 3];
}

static float konst_component(const CcMaterialQuad *material, unsigned selector,
                             unsigned component, bool alpha) {
    if (selector < 8)
        return (float)(8 - selector) / 8.0f;
    if (!alpha && selector >= 12 && selector < 16)
        return material->konst_colors[selector - 12][component];
    if (selector >= 16 && selector < 32) {
        unsigned index = selector - 16;
        return material->konst_colors[index & 3][index >> 2];
    }
    return 1;
}

static float color_input(unsigned selector, unsigned component,
                         const float registers[4][4], const float texture[4],
                         const float raster[4], float konst) {
    if (selector < 8)
        return registers[selector >> 1][selector & 1 ? 3 : component];
    switch (selector) {
        case 8:
            return texture[component];
        case 9:
            return texture[3];
        case 10:
            return raster[component];
        case 11:
            return raster[3];
        case 12:
            return 1;
        case 13:
            return 0.5f;
        case 14:
            return konst;
        default:
            return 0;
    }
}

static float alpha_input(unsigned selector, const float registers[4][4],
                         const float texture[4], const float raster[4], float konst) {
    if (selector < 4)
        return registers[selector][3];
    switch (selector) {
        case 4:
            return texture[3];
        case 5:
            return raster[3];
        case 6:
            return konst;
        default:
            return 0;
    }
}

static float tev_byte(float value) {
    float rounded = floorf(value * 255.0f + 0.5f);
    return rounded - 256 * floorf(rounded / 256);
}

static float tev_arithmetic(float a, float b, float c, float d, unsigned operation,
                            unsigned control) {
    unsigned bias_index = operation >> 4 & 3;
    float bias = bias_index == 1 ? 0.5f : bias_index == 2 ? -0.5f : 0;
    unsigned scale_index = operation >> 6;
    float scale = scale_index == 1   ? 2
                  : scale_index == 2 ? 4
                  : scale_index == 3 ? 0.5f
                                     : 1;
    float mixed = a + (b - a) * c;
    float result = (d + ((operation & 15) == 1 ? -mixed : mixed) + bias) * scale;
    return control & 1 ? unit(result) : result;
}

static void tev_color(const CcMaterialQuad *material, const uint8_t stage[16],
                      const float registers[4][4], const float texture[4],
                      const float raster[4], float output[3]) {
    float inputs[4][3];
    unsigned selectors[4] = {stage[4] & 15u, stage[4] >> 4, stage[5] & 15u,
                             stage[5] >> 4};
    for (unsigned input = 0; input < 4; ++input) {
        for (unsigned component = 0; component < 3; ++component) {
            float konst = konst_component(material, stage[7] >> 3, component, false);
            inputs[input][component] = color_input(selectors[input], component,
                                                   registers, texture, raster, konst);
        }
    }
    unsigned operation = stage[6] & 15;
    bool packed_pass = false;
    if (operation >= 8 && operation < 14) {
        unsigned count = operation < 10 ? 1 : operation < 12 ? 2 : 3;
        float a = 0, b = 0, weight = 1;
        for (unsigned component = 0; component < count; ++component) {
            a += tev_byte(inputs[0][component]) * weight;
            b += tev_byte(inputs[1][component]) * weight;
            weight *= 256;
        }
        packed_pass = operation & 1 ? a == b : a > b;
    }
    for (unsigned component = 0; component < 3; ++component) {
        float value;
        if (operation >= 8) {
            bool pass = packed_pass;
            if (operation >= 14) {
                float a = tev_byte(inputs[0][component]);
                float b = tev_byte(inputs[1][component]);
                pass = operation & 1 ? a == b : a > b;
            }
            value = inputs[3][component] + (pass ? inputs[2][component] : 0);
            if (stage[7] & 1)
                value = unit(value);
        } else {
            value = tev_arithmetic(inputs[0][component], inputs[1][component],
                                   inputs[2][component], inputs[3][component], stage[6],
                                   stage[7]);
        }
        output[component] = value;
    }
}

static float tev_alpha(const CcMaterialQuad *material, const uint8_t stage[16],
                       const float registers[4][4], const float texture[4],
                       const float raster[4]) {
    float konst = konst_component(material, stage[11] >> 3, 3, true);
    float a = alpha_input(stage[8] & 15, registers, texture, raster, konst);
    float b = alpha_input(stage[8] >> 4, registers, texture, raster, konst);
    float c = alpha_input(stage[9] & 15, registers, texture, raster, konst);
    float d = alpha_input(stage[9] >> 4, registers, texture, raster, konst);
    if ((stage[10] & 15) < 8)
        return tev_arithmetic(a, b, c, d, stage[10], stage[11]);
    bool pass = stage[10] & 1 ? tev_byte(a) == tev_byte(b) : tev_byte(a) > tev_byte(b);
    float result = d + (pass ? c : 0);
    return stage[11] & 1 ? unit(result) : result;
}

static bool compare_alpha(unsigned kind, float value, unsigned reference) {
    switch (kind) {
        case 0:
            return false;
        case 1:
            return value < (float)reference;
        case 2:
            return value == (float)reference;
        case 3:
            return value <= (float)reference;
        case 4:
            return value > (float)reference;
        case 5:
            return value != (float)reference;
        case 6:
            return value >= (float)reference;
        default:
            return true;
    }
}

static void sample_coordinate(const GcSoftwareTexture *textures,
                              size_t texture_capacity, const CcMaterialQuad *material,
                              unsigned slot, unsigned coordinate,
                              const unsigned indices[3], const float weights[3],
                              float output[4]) {
    float u = 0, v = 0;
    for (unsigned vertex = 0; vertex < 3; ++vertex) {
        const float *uv = material->vertices[indices[vertex]].uv[coordinate];
        u += uv[0] * weights[vertex];
        v += uv[1] * weights[vertex];
    }
    uint32_t handle = material->textures[slot];
    const GcSoftwareTexture *image =
        handle < texture_capacity ? &textures[handle] : NULL;
    gc_software_sample_texture(image, u, v, material->wrap_s[slot],
                               material->wrap_t[slot], material->nearest[slot], output);
}

static bool alpha_passes(const CcMaterialQuad *material, float alpha_value) {
    if (material->has_alpha_compare) {
        const uint8_t *comparison = material->alpha_compare;
        float alpha = tev_byte(alpha_value);
        bool first = compare_alpha(comparison[0] & 15, alpha, comparison[2]);
        bool second = compare_alpha(comparison[0] >> 4, alpha, comparison[3]);
        bool pass = comparison[1] == 0   ? first && second
                    : comparison[1] == 1 ? first || second
                    : comparison[1] == 2 ? first != second
                                         : first == second;
        if (!pass)
            return false;
    }
    return true;
}

static bool tev_material_color(const GcSoftwareTexture *textures,
                               size_t texture_capacity, const CcMaterialQuad *material,
                               const unsigned indices[3], const float weights[3],
                               CcColor *color) {
    float registers[4][4] = {{0}};
    memcpy(registers + 1, material->registers, sizeof(material->registers));
    for (unsigned index = 0; index < material->tev_stage_count; ++index) {
        const uint8_t *stage = material->tev_stages[index];
        unsigned coordinate = stage[0];
        unsigned slot = stage[2] | ((unsigned)(stage[3] & 1) << 8);
        float texture[4] = {1, 1, 1, 1};
        if (coordinate < CC_MATERIAL_TEXTURES && slot < material->texture_count) {
            sample_coordinate(textures, texture_capacity, material, slot, coordinate,
                              indices, weights, texture);
        }
        float raster[4] = {color->r, color->g, color->b, color->a};
        if (stage[1] == 255 || stage[1] == 6 || stage[1] == 7)
            memset(raster, 0, sizeof(raster));
        swizzle(texture, material->tev_swap_table[stage[3] >> 3 & 3]);
        swizzle(raster, material->tev_swap_table[stage[3] >> 1 & 3]);
        float rgb[3];
        tev_color(material, stage, registers, texture, raster, rgb);
        float alpha = tev_alpha(material, stage, registers, texture, raster);
        unsigned color_destination = stage[7] >> 1 & 3;
        unsigned alpha_destination = stage[11] >> 1 & 3;
        memcpy(registers[color_destination], rgb, sizeof(rgb));
        registers[alpha_destination][3] = alpha;
    }
    if (!alpha_passes(material, registers[0][3]))
        return false;
    *color =
        (CcColor){registers[0][0], registers[0][1], registers[0][2], registers[0][3]};
    return true;
}

/* Match the Metal/GLES2 fallback, including its two-texture limit.
 * This is also the supported zero-stage material path. */
static bool simple_material_color(const GcSoftwareTexture *textures,
                                  size_t texture_capacity,
                                  const CcMaterialQuad *material,
                                  const unsigned indices[3], const float weights[3],
                                  CcColor *color) {
    float sampled[2][4] = {{0}};
    unsigned count = material->texture_count < 2 ? material->texture_count : 2;
    for (unsigned slot = 0; slot < count; ++slot)
        sample_coordinate(textures, texture_capacity, material, slot, slot, indices,
                          weights, sampled[slot]);
    float raster[4] = {color->r, color->g, color->b, color->a};
    float output[4];
    for (unsigned component = 0; component < 4; ++component) {
        float value = material->registers[1][component];
        if (count) {
            float texture = sampled[0][component];
            if (count == 2) {
                float amount = material->konst_colors[3][3];
                texture = sampled[1][component] +
                          (sampled[0][component] - sampled[1][component]) * amount;
            }
            value = material->registers[0][component] +
                    (value - material->registers[0][component]) * texture;
        }
        output[component] = value * raster[component];
    }
    if (!alpha_passes(material, output[3]))
        return false;
    *color = (CcColor){output[0], output[1], output[2], output[3]};
    return true;
}

bool gc_software_material_color(const GcSoftwareTexture *textures,
                                size_t texture_capacity, const CcMaterialQuad *material,
                                const unsigned indices[3], const float weights[3],
                                bool tev, CcColor *color) {
    return tev ? tev_material_color(textures, texture_capacity, material, indices,
                                    weights, color)
               : simple_material_color(textures, texture_capacity, material, indices,
                                       weights, color);
}
