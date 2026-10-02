#ifndef GAMECUBE_IPL_MODEL_H
#define GAMECUBE_IPL_MODEL_H

#include "gamecube/ipl.h"

typedef struct {
    float position[3];
    float normal[3];
    float uv[2];
} GcIplVertex;

typedef struct {
    GcIplVertex vertices[3];
    unsigned shape_index;
    unsigned material_index;
    unsigned joint_index;
} GcIplTriangle;

typedef struct {
    char name[64];
    int parent;
    float scale[3];
    float rotation[3]; /* Radians. */
    float translation[3];
} GcIplJoint;

typedef struct {
    uint8_t flags;
    uint8_t cull_mode;
    uint8_t channel_count;
    uint8_t texture_generator_count;
    uint8_t stage_count;
    uint8_t color[2][4];
    uint8_t channels[4][8];
    uint8_t texture_coordinates[8][4];
    float texture_matrices[10][12];
    uint8_t texture_matrix_types[10];
    uint16_t texture_matrix_mask;
    uint16_t textures[8];
    int16_t registers[4][4]; /* Native signed TEV C0, C1, C2, and spare. */
    uint8_t orders[8][4];
    uint8_t stages[8][20];
    uint8_t alpha_compare[8];
    uint8_t blend[4];
    uint8_t depth[4];
} GcIplMaterial;

typedef struct {
    GcIplTriangle *triangles;
    size_t triangle_count;
    GcIplJoint *joints;
    size_t joint_count;
    GcIplImage *textures;
    size_t texture_count;
    unsigned material_texture[256]; /* UINT_MAX means no texture. */
    size_t material_count;
    GcIplMaterial *materials;
} GcIplModel;

/* Owned arrays are released together. Inputs are borrowed and never modified.
 * Supports the rigid J3D1bmd1 models embedded in the supplied three IPL ROMs.
 * Skinning, native TEV material programs, and animation tracks remain separate.
 */
bool gc_ipl_model_decode(const uint8_t *data, size_t size, GcIplModel *model);
bool gc_ipl_model_load(const char *ipl_path, const char *native_joint_name,
                       GcIplModel *model);
void gc_ipl_model_destroy(GcIplModel *model);

/* Applies the recovered bind-pose joint hierarchy. A caller may edit joints
 * to pose a model; exact native animation timing is not implied by this API.
 */
bool gc_ipl_model_transform_vertex(const GcIplModel *model, unsigned joint_index,
                                   const GcIplVertex *vertex, GcIplVertex *output);

/* Native inverse-transpose normals preserve scale magnitude. Normalize a
 * separate copy when evaluating the lit color channel.
 */
bool gc_ipl_model_transform_vertex_native(const GcIplModel *model, unsigned joint_index,
                                          const GcIplVertex *vertex,
                                          GcIplVertex *output);
bool gc_ipl_model_transform_normal(const float matrix[12], const float normal[3],
                                   float output[3]);

/* USA 0496c / 04c10 pass the position matrix to 04628 for NORMAL texgen.
 * Its TEXmatrix * position-matrix linear transform is independent of the
 * inverse-transpose lighting normal. This applies the bind hierarchy's
 * ordinary scale/rotation to a local decoded normal, with no translation.
 * Apply the model, scene and camera linear transforms afterward, followed
 * by the material texture matrix. Inputs and output may alias.
 */
bool gc_ipl_model_transform_direction(const GcIplModel *model, unsigned joint_index,
                                      const float normal[3], float output[3]);

/* Evaluates the recovered native additive/subtractive TEV stages. The raster
 * input contains the lit channel color. Texture input is a decoded texel.
 * Returns false for a program requiring an unsupported compare operation.
 */
bool gc_ipl_material_shade(const GcIplMaterial *material, const uint8_t texture[4],
                           const uint8_t raster[4], uint8_t output[4]);

#endif
