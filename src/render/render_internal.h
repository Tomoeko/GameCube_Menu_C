#ifndef GAMECUBE_RENDER_INTERNAL_H
#define GAMECUBE_RENDER_INTERNAL_H

#include "gamecube/render.h"
#include "gamecube/card_art.h"
#include "gamecube/ipl_model.h"
#include "gamecube/texture_collection.h"
#include "render_material.h"
#include "render_projection.h"

typedef struct {
    CcMaterialVertex vertices[4];
    float depth;
    unsigned material;
    bool visible;
} GcRenderMeshFace;

struct GcMesh {
    GcIplModel model;
    uint32_t *textures;
    GcRenderMeshFace *faces;
    CcMaterialQuad *materials;
    unsigned register_mask;
    float registers[3][4];
    unsigned material_mask; /* Zero draws all materials. */
    uint32_t icon_texture;  /* Replaces the card-cover material's texture0. */
};

struct GcCardTextures {
    uint32_t banners[GC_CARD_FILE_LIMIT];
    uint32_t icons[GC_CARD_FILE_LIMIT][GC_CARD_ICON_FRAMES];
    GcCardArt timing[GC_CARD_FILE_LIMIT];
};

struct GcUiTextures {
    GcMenuTextures native;
    uint32_t *collection;
    uint32_t digits[10];
    uint32_t weekdays[7][7];
    uint32_t sound[7][2];
    uint32_t card_numbers;
    uint32_t grid;
    GcEditPoint edit_points[GC_EDIT_POINT_LIMIT];
};

void gc_render_card_textures_destroy(GcScene *scene, GcCardTextures *textures);
uint32_t gc_render_ui_image_texture(const GcUiTextures *ui, const GcIplImage *image);
void gc_render_mesh_destroy(GcScene *scene, GcMesh *mesh);
GcMesh *gc_render_mesh_load(GcScene *scene, const char *path, const char *name);
void gc_render_mesh_draw(GcScene *scene, GcMesh *mesh, const float scene_matrix[12],
                         const float model_matrix[12], float x, float y, float alpha);
void gc_render_mesh_piece(GcScene *scene, GcMesh *mesh, const float matrix[12],
                          const float scale[3], const int16_t registers[2][4],
                          unsigned register_mask, float alpha);
const char *gc_render_native_text(const GcScene *scene, GcTextGroup group,
                                  unsigned index, const char *fallback);
CcColor gc_render_color_rgba(uint32_t rgba);
void gc_render_layout_vertices(GcScene *scene, GcLayoutGroup group,
                               CcDrawVertex vertices[4], uint32_t texture,
                               bool squared_alpha);
float gc_render_layout_line_width(const GcScene *scene, const GcLayoutText *layout,
                                  const char *bytes, size_t length);
void gc_render_layout_text_value(GcScene *scene, GcLayoutGroup group,
                                 const GcLayoutText *source, const char *value);
void gc_render_layout_text_measure(const GcScene *scene, const GcLayoutText *layout,
                                   const char *value, float *width, float *height);
uint32_t gc_render_layout_color_alpha(uint32_t color, uint8_t alpha);
void gc_render_layout_text_alpha(GcLayoutText *layout, uint8_t alpha);
void gc_render_layout_highlight_value(GcScene *scene, GcLayoutGroup group,
                                      const GcLayoutText *layout, const char *value,
                                      uint32_t foreground, uint32_t halo,
                                      uint8_t alpha);
void gc_render_layout_string(GcScene *scene, GcLayoutGroup group, const char name[4],
                             const char *value);
void gc_render_layout_string_alpha(GcScene *scene, GcLayoutGroup group,
                                   const char name[4], const char *value,
                                   uint8_t alpha);
void gc_render_layout_image(GcScene *scene, GcLayoutGroup group,
                            const GcLayoutPane *pane, uint32_t texture, CcColor tint);
void gc_render_layout_pane(GcScene *scene, const gc_menu *menu, GcLayoutGroup group,
                           const char name[4], CcColor tint);
void gc_render_layout_frame_value(GcScene *scene, GcLayoutGroup group,
                                  const GcLayoutFrame *frame, uint32_t tint);
void gc_render_layout_frame(GcScene *scene, GcLayoutGroup group, const char name[4],
                            unsigned occurrence, uint32_t tint);
void gc_render_layout_native_entry(GcScene *scene, GcTextGroup text_group,
                                   GcLayoutGroup layout_group, unsigned index,
                                   uint8_t alpha);
void gc_render_card_number(GcScene *scene, unsigned number, const char name[4]);
void gc_render_prompts(GcScene *scene, const gc_menu *menu);
void gc_render_grid_lights(GcScene *scene, const GcCardGridLighting *lighting);
void gc_render_grid(GcScene *scene, double time);
void gc_render_volume_overlay(GcScene *scene);

#endif
