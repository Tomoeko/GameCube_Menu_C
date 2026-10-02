#ifndef GAMECUBE_SOFTWARE_MATERIAL_H
#define GAMECUBE_SOFTWARE_MATERIAL_H

#include "console_common/platform/platform.h"

#include <stddef.h>

/* The platform owns pixels; material evaluation borrows texture storage. */
typedef struct {
    int width;
    int height;
    uint8_t *pixels;
} GcSoftwareTexture;

/* Nearest or bilinear sampling uses GX clamp, repeat and mirror coordinates. Missing
 * textures and non-finite coordinates select the platform's white sample. */
void gc_software_sample_texture(const GcSoftwareTexture *image, float u, float v,
                                unsigned wrap_s, unsigned wrap_t, bool nearest,
                                float output[4]);

/* The caller chooses the shared TEV/fallback policy once per draw. Raster
 * color enters through color and is replaced only when the alpha test passes. */
bool gc_software_material_color(const GcSoftwareTexture *textures,
                                size_t texture_capacity, const CcMaterialQuad *material,
                                const unsigned indices[3], const float weights[3],
                                bool tev, CcColor *color);

#endif
