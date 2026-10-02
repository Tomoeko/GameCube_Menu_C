#include "render_material.h"

#include <string.h>

CcMaterialQuad gc_render_raster_material(uint32_t texture) {
    CcMaterialQuad material = {
        .texture_count = 1, .tev_stage_count = 1, .has_blend_mode = true};
    material.textures[0] = texture;
    memset(material.tev_swap_table, 0xe4, sizeof(material.tev_swap_table));
    material.blend_mode[0] = 1;
    material.blend_mode[1] = 4;
    material.blend_mode[2] = 5;
    material.tev_stages[0][1] = 4;
    material.tev_stages[0][4] = 0x8f;
    material.tev_stages[0][5] = 0xfa;
    material.tev_stages[0][7] = 1;
    material.tev_stages[0][8] = 0x47;
    material.tev_stages[0][9] = 0x75;
    material.tev_stages[0][11] = 1;
    return material;
}

void gc_render_material_opacity(CcMaterialQuad *material, float alpha) {
    if (!material || alpha >= 1 || material->tev_stage_count >= 6)
        return;
    uint8_t *stage = material->tev_stages[material->tev_stage_count++];
    memset(stage, 0, 16);
    stage[0] = stage[2] = UINT8_MAX;
    stage[1] = 0;
    stage[4] = 0xcf;
    stage[5] = 0xf0;
    stage[7] = 1;
    stage[8] = 0x67;
    stage[9] = 0x70;
    stage[11] = (uint8_t)(1 | 28 << 3);
    material->konst_colors[0][3] = alpha;
}

static void simple_stage(unsigned mode, uint8_t bytes[16]) {
    unsigned color[4] = {15, 8, 10, 15};
    unsigned alpha[4] = {7, 4, 5, 7};
    if (mode == 1) {
        color[0] = 10;
        color[2] = 9;
        alpha[1] = alpha[2] = 7;
        alpha[3] = 5;
    } else if (mode == 2) {
        color[0] = 10;
        color[1] = 2;
        color[2] = 8;
    } else if (mode >= 3) {
        color[1] = color[2] = 15;
        color[3] = mode == 3 ? 8 : 10;
        alpha[1] = alpha[2] = 7;
        alpha[3] = mode == 3 ? 4 : 5;
    }
    bytes[4] = (uint8_t)(color[0] | color[1] << 4);
    bytes[5] = (uint8_t)(color[2] | color[3] << 4);
    bytes[7] = 1;
    bytes[8] = (uint8_t)(alpha[0] | alpha[1] << 4);
    bytes[9] = (uint8_t)(alpha[2] | alpha[3] << 4);
    bytes[11] = 1;
}

static bool custom_stage(const uint8_t source[20], uint8_t bytes[16]) {
    for (unsigned index = 1; index <= 4; index++)
        if (source[index] > 15)
            return false;
    for (unsigned index = 10; index <= 13; index++)
        if (source[index] > 7)
            return false;
    if (source[5] > 1 || source[6] > 2 || source[7] > 3 || source[8] > 1 ||
        source[9] > 3 || source[14] > 1 || source[15] > 2 || source[16] > 3 ||
        source[17] > 1 || source[18] > 3)
        return false;
    bytes[4] = (uint8_t)(source[1] | source[2] << 4);
    bytes[5] = (uint8_t)(source[3] | source[4] << 4);
    bytes[6] = (uint8_t)(source[5] | source[6] << 4 | source[7] << 6);
    /* GXSetTevKColorSel selects 1/4; KAlphaSel selects 1 in the IPL. */
    bytes[7] = (uint8_t)(source[8] | source[9] << 1 | 6 << 3);
    bytes[8] = (uint8_t)(source[10] | source[11] << 4);
    bytes[9] = (uint8_t)(source[12] | source[13] << 4);
    bytes[10] = (uint8_t)(source[14] | source[15] << 4 | source[16] << 6);
    bytes[11] = (uint8_t)(source[17] | source[18] << 1);
    return true;
}

bool gc_render_material(const GcIplMaterial *source, const uint32_t *textures,
                        size_t texture_count, CcMaterialQuad *output) {
    if (!source || !output || (!textures && texture_count) || !source->stage_count ||
        source->stage_count > 6)
        return false;
    *output = (CcMaterialQuad){0};
    for (unsigned index = 0; index < 3; index++)
        for (unsigned component = 0; component < 4; component++)
            output->registers[index][component] =
                (float)source->registers[index][component] / 255.0f;
    memset(output->tev_swap_table, 0xe4, sizeof(output->tev_swap_table));
    for (unsigned stage = 0; stage < source->stage_count; stage++) {
        uint8_t *bytes = output->tev_stages[stage];
        unsigned coordinate = source->orders[stage][0];
        unsigned map = source->orders[stage][1];
        bytes[0] = coordinate < 4 ? (uint8_t)coordinate : UINT8_MAX;
        bytes[1] = source->orders[stage][2];
        bytes[2] = UINT8_MAX;
        if (map < 8 && source->textures[map] < texture_count) {
            unsigned slot = 0;
            uint32_t texture = textures[source->textures[map]];
            while (slot < output->texture_count && output->textures[slot] != texture)
                slot++;
            if (slot == CC_MATERIAL_TEXTURES)
                return false;
            output->textures[slot] = texture;
            if (slot == output->texture_count)
                output->texture_count++;
            bytes[2] = (uint8_t)slot;
        }
        if (source->stages[stage][0] < 5)
            simple_stage(source->stages[stage][0], bytes);
        else if (!custom_stage(source->stages[stage], bytes))
            return false;
    }
    output->tev_stage_count = source->stage_count;
    /* USA BS2 0x81304e00 sets this global state. The native material
     * submitter ignores the serialized MAT1 alpha and blend records. */
    output->has_alpha_compare = true;
    output->alpha_compare[0] = 0x77;
    output->alpha_compare[1] = 1;
    output->has_blend_mode = true;
    output->blend_mode[0] = 1;
    output->blend_mode[1] = 4;
    output->blend_mode[2] = 5;
    return true;
}

void gc_render_texture_coordinates(const GcIplMaterial *material,
                                   const GcIplVertex *vertex,
                                   const float view_normal[3],
                                   CcMaterialVertex *output) {
    for (unsigned index = 0; index < CC_MATERIAL_TEXTURES; index++) {
        const uint8_t *generator = material->texture_coordinates[index];
        float input[3] = {vertex->uv[0], vertex->uv[1], 1};
        if (generator[1] == 1)
            memcpy(input, view_normal, sizeof(input));
        unsigned matrix = generator[2] >= 30 ? (generator[2] - 30u) / 3u : 10;
        if (matrix < 10 && (material->texture_matrix_mask & (1u << matrix))) {
            const float *values = material->texture_matrices[matrix];
            for (unsigned row = 0; row < 2; row++)
                output->uv[index][row] =
                    values[row * 4] * input[0] + values[row * 4 + 1] * input[1] +
                    values[row * 4 + 2] * input[2] + values[row * 4 + 3];
        } else {
            output->uv[index][0] = input[0];
            output->uv[index][1] = input[1];
        }
    }
}
