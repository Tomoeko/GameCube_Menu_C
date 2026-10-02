#ifndef GAMECUBE_RENDER_MATERIAL_H
#define GAMECUBE_RENDER_MATERIAL_H

#include "gamecube/ipl_model.h"
#include "console_common/platform/platform.h"

CcMaterialQuad gc_render_raster_material(uint32_t texture);

bool gc_render_material(const GcIplMaterial *source, const uint32_t *textures,
                        size_t texture_count, CcMaterialQuad *output);
void gc_render_material_opacity(CcMaterialQuad *material, float alpha);
void gc_render_texture_coordinates(const GcIplMaterial *material,
                                   const GcIplVertex *vertex,
                                   const float view_normal[3],
                                   CcMaterialVertex *output);

#endif
