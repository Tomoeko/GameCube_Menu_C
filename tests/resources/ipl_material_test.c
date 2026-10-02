#include "gamecube/ipl_model.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    GcIplMaterial material = {0};
    material.stage_count = 1;
    const uint8_t program[20] = {255, 4, 2, 8, 15, 0, 0, 0, 1, 0,
                                 7,   5, 6, 7, 0,  0, 0, 1, 0, 255};
    memcpy(material.stages[0], program, sizeof(program));
    const int16_t dark[4] = {20, 5, 35, 255};
    const int16_t light[4] = {25, 10, 50, 255};
    memcpy(material.registers[0], light, sizeof(light));
    memcpy(material.registers[1], dark, sizeof(dark));
    const uint8_t raster[4] = {255, 255, 255, 255};
    uint8_t texture[4] = {0, 0, 0, 0};
    uint8_t output[4];
    assert(gc_ipl_material_shade(&material, texture, raster, output));
    const uint8_t expected_dark[4] = {20, 5, 35, 255};
    assert(!memcmp(output, expected_dark, 4));
    memset(texture, 255, 4);
    assert(gc_ipl_material_shade(&material, texture, raster, output));
    const uint8_t expected_light[4] = {25, 10, 50, 255};
    assert(!memcmp(output, expected_light, 4));

    /* Signed C0 alpha + a half-scale native glass alpha program. */
    material.registers[0][3] = -100;
    material.stages[0][12] = 4;
    material.stages[0][13] = 1;
    material.stages[0][16] = 3;
    memset(texture, 128, 4);
    assert(gc_ipl_material_shade(&material, texture, raster, output));
    assert(output[3] == 14);
    material.stages[0][5] = 8;
    assert(!gc_ipl_material_shade(&material, texture, raster, output));
    material.stages[0][5] = 0;
    material.stages[0][9] = 4;
    assert(!gc_ipl_material_shade(&material, texture, raster, output));
    puts("IPL material tests passed");
    return 0;
}
