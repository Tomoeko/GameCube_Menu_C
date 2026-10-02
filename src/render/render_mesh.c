#include "render_internal.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    float x;
    float y;
    float z;
} Point3;

void gc_render_mesh_destroy(GcScene *scene, GcMesh *mesh) {
    if (!mesh)
        return;
    for (size_t index = 0; mesh->textures && index < mesh->model.texture_count; index++)
        cc_platform_destroy_texture(scene->platform, mesh->textures[index]);
    gc_ipl_model_destroy(&mesh->model);
    free(mesh->faces);
    free(mesh->textures);
    free(mesh->materials);
    free(mesh);
}

GcMesh *gc_render_mesh_load(GcScene *scene, const char *path, const char *name) {
    GcMesh *mesh = calloc(1, sizeof(*mesh));
    if (!mesh)
        return NULL;
    if (!gc_ipl_model_load(path, name, &mesh->model)) {
        free(mesh);
        return NULL;
    }
    mesh->textures = calloc(mesh->model.texture_count, sizeof(*mesh->textures));
    mesh->faces = calloc(mesh->model.triangle_count, sizeof(*mesh->faces));
    mesh->materials = calloc(mesh->model.material_count, sizeof(*mesh->materials));
    if (!mesh->textures || !mesh->faces || !mesh->materials) {
        gc_render_mesh_destroy(scene, mesh);
        return NULL;
    }
    for (size_t index = 0; index < mesh->model.texture_count; index++) {
        GcIplImage *image = &mesh->model.textures[index];
        mesh->textures[index] = cc_platform_create_texture(
            scene->platform, (int)image->width, (int)image->height, image->rgba);
        if (!mesh->textures[index]) {
            gc_render_mesh_destroy(scene, mesh);
            return NULL;
        }
    }
    for (size_t index = 0; index < mesh->model.material_count; index++) {
        if (!gc_render_material(&mesh->model.materials[index], mesh->textures,
                                mesh->model.texture_count, &mesh->materials[index])) {
            gc_render_mesh_destroy(scene, mesh);
            return NULL;
        }
        CcMaterialQuad *material = &mesh->materials[index];
        for (unsigned slot = 0; slot < material->texture_count; slot++)
            for (size_t texture = 0; texture < mesh->model.texture_count; texture++) {
                if (material->textures[slot] != mesh->textures[texture])
                    continue;
                material->wrap_s[slot] = mesh->model.textures[texture].wrap_s;
                material->wrap_t[slot] = mesh->model.textures[texture].wrap_t;
                break;
            }
        cc_platform_prepare_material(scene->platform, &mesh->materials[index]);
        CcMaterialQuad translucent = mesh->materials[index];
        gc_render_material_opacity(&translucent, 0.5f);
        cc_platform_prepare_material(scene->platform, &translucent);
    }
    return mesh;
}

static CcColor material_raster(const GcIplMaterial *material, Point3 position,
                               Point3 normal, float ambient) {
    CcColor color = {
        (float)material->color[0][0] / 255, (float)material->color[0][1] / 255,
        (float)material->color[0][2] / 255, (float)material->color[0][3] / 255};
    if (!material->channels[0][0])
        return color;
    /* Native light0 is white at (400,500,200). The base-cube draw sets
     * ambient COLOR0 to (40,40,40), then restores white for other objects.
     * USA BS2 0x813030b8, 0x8130dab4 and 0x8130dcac. */
    Point3 light = {400 - position.x, 500 - position.y, 200 - position.z};
    float length = sqrtf(light.x * light.x + light.y * light.y + light.z * light.z);
    float normal_length =
        sqrtf(normal.x * normal.x + normal.y * normal.y + normal.z * normal.z);
    float diffuse =
        length > 0 && normal_length > 0
            ? (light.x * normal.x + light.y * normal.y + light.z * normal.z) /
                  (length * normal_length)
            : 0;
    if (material->channels[0][3] == 0)
        diffuse = 1;
    else if (material->channels[0][3] == 2)
        diffuse = fmaxf(0, diffuse);
    float factor = fmaxf(0, fminf(1, ambient + diffuse));
    color.r *= factor;
    color.g *= factor;
    color.b *= factor;
    return color;
}

void gc_render_mesh_draw(GcScene *scene, GcMesh *mesh, const float scene_matrix[12],
                         const float model_matrix[12], float x, float y, float alpha) {
    if (!mesh)
        return;
    float normal_matrix[9];
    for (unsigned axis = 0; axis < 3; axis++) {
        float input[3] = {0}, model_normal[3], scene_normal[3];
        input[axis] = 1;
        if (!gc_ipl_model_transform_normal(model_matrix, input, model_normal) ||
            !gc_ipl_model_transform_normal(scene_matrix, model_normal, scene_normal))
            return;
        for (unsigned row = 0; row < 3; row++)
            normal_matrix[row * 3 + axis] = scene_normal[row];
    }
    for (size_t index = 0; index < mesh->model.triangle_count; index++) {
        const GcIplTriangle *triangle = &mesh->model.triangles[index];
        GcRenderMeshFace *face = &mesh->faces[index];
        const GcIplMaterial *material =
            &mesh->model.materials[triangle->material_index];
        if (mesh->material_mask &&
            (triangle->material_index >= sizeof(mesh->material_mask) * CHAR_BIT ||
             !(mesh->material_mask & (1u << triangle->material_index)))) {
            face->visible = false;
            continue;
        }
        face->material = triangle->material_index;
        face->visible = true;
        face->depth = 0;
        for (unsigned vertex = 0; vertex < 3; vertex++) {
            GcIplVertex posed;
            if (!gc_ipl_model_transform_vertex_native(
                    &mesh->model, triangle->joint_index, &triangle->vertices[vertex],
                    &posed))
                return;
            Point3 position = {posed.position[0], posed.position[1], posed.position[2]};
            Point3 normal = {posed.normal[0], posed.normal[1], posed.normal[2]};
            float point[3] = {position.x, position.y, position.z};
            float transformed[3];
            gc_startup_transform(model_matrix, point, transformed);
            gc_startup_transform(scene_matrix, transformed, point);
            position = (Point3){point[0], point[1], point[2]};
            for (unsigned row = 0; row < 3; row++)
                point[row] = normal_matrix[row * 3] * normal.x +
                             normal_matrix[row * 3 + 1] * normal.y +
                             normal_matrix[row * 3 + 2] * normal.z;
            normal = (Point3){point[0], point[1], point[2]};
            float depth = -position.z;
            float ambient =
                mesh == scene->menu_cube || mesh == scene->boot_base ? 40.0f / 255 : 1;
            CcColor color = material_raster(material, position, normal, ambient);
            float reflection[3], model_reflection[3], view_reflection[3];
            if (!gc_ipl_model_transform_direction(&mesh->model, triangle->joint_index,
                                                  triangle->vertices[vertex].normal,
                                                  reflection))
                return;
            for (unsigned row = 0; row < 3; row++)
                model_reflection[row] = model_matrix[row * 4] * reflection[0] +
                                        model_matrix[row * 4 + 1] * reflection[1] +
                                        model_matrix[row * 4 + 2] * reflection[2];
            for (unsigned row = 0; row < 3; row++)
                view_reflection[row] = scene_matrix[row * 4] * model_reflection[0] +
                                       scene_matrix[row * 4 + 1] * model_reflection[1] +
                                       scene_matrix[row * 4 + 2] * model_reflection[2];
            float factor = 1;
            float scale_x = 592.0f / 588, scale_y = scene->pixel_scale_y;
            if (scene->perspective) {
                float distance = 224.0f / tanf(3.14159265358979323846f / 18);
                factor = distance / (distance - position.z);
                scale_x = 1;
                x = 320;
                y = 240;
                position.y -= scene->camera_y;
            }
            face->vertices[vertex] = (CcMaterialVertex){
                .x = x + position.x * scale_x * factor + scene->display_offset_x,
                .y = y - position.y * scale_y * factor,
                .color = color};
            float distance = 224.0f / tanf(3.14159265358979323846f / 18);
            float view_z = distance - position.z;
            face->vertices[vertex].depth =
                scene->perspective ? 10000.0f / 9950 - 500000.0f / (9950 * view_z)
                                   : (view_z - 50) / 9950;
            face->vertices[vertex].clip_w = scene->perspective ? view_z / distance : 1;
            gc_render_texture_coordinates(material, &posed, view_reflection,
                                          &face->vertices[vertex]);
            face->depth += depth;
        }
        float winding = (face->vertices[1].x - face->vertices[0].x) *
                            (face->vertices[2].y - face->vertices[0].y) -
                        (face->vertices[1].y - face->vertices[0].y) *
                            (face->vertices[2].x - face->vertices[0].x);
        /* Native model fronts are clockwise in Y-up coordinates, hence
         * positive after the projection flips Y into framebuffer space. */
        face->visible =
            material->cull_mode == 0 || (material->cull_mode == 1   ? winding < 0
                                         : material->cull_mode == 2 ? winding > 0
                                                                    : false);
        /* The shared quad contract's second triangle collapses to a point. */
        face->vertices[3] = face->vertices[2];
        face->vertices[2] = face->vertices[0];
    }
    for (size_t index = 0; index < mesh->model.triangle_count; index++) {
        GcRenderMeshFace *face = &mesh->faces[index];
        if (!face->visible)
            continue;
        CcMaterialQuad material = mesh->materials[face->material];
        if (mesh == scene->card_cover && face->material == 1 && mesh->icon_texture)
            material.textures[0] = mesh->icon_texture;
        memcpy(material.vertices, face->vertices, sizeof(material.vertices));
        for (unsigned color = 0; color < 3; color++)
            if (mesh->register_mask & (1u << color))
                memcpy(material.registers[color], mesh->registers[color],
                       sizeof(material.registers[color]));
        bool opaque = alpha >= 1 && (mesh->model.materials[face->material].flags & 1);
        material.has_depth_mode = true;
        material.depth_mode[0] = opaque ? 1 : 0;
        material.depth_mode[1] = 3;
        material.depth_mode[2] = opaque ? 1 : 0;
        gc_render_material_opacity(&material, alpha);
        cc_platform_draw_material_quad(scene->platform, &material);
    }
}

void gc_render_mesh_piece(GcScene *scene, GcMesh *mesh, const float matrix[12],
                          const float scale[3], const int16_t registers[2][4],
                          unsigned register_mask, float alpha) {
    if (!mesh)
        return;
    float model[12] = {0};
    for (unsigned axis = 0; axis < 3; axis++)
        model[axis * 5] = scale[axis];
    mesh->register_mask = register_mask;
    for (unsigned color = 0; color < 2; color++)
        for (unsigned channel = 0; channel < 4; channel++)
            mesh->registers[color][channel] = (float)registers[color][channel] / 255;
    gc_render_mesh_draw(scene, mesh, matrix, model, 322.18f,
                        240 + scene->camera_y * scene->pixel_scale_y,
                        alpha * scene->value_alpha);
}
