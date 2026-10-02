#include "render_internal.h"
#include "software.h"

#include <assert.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static GcIplMaterial source_material(void);

static void read_rgb(CcPlatform *platform, const char *path, uint8_t rgb[3]) {
    assert(gc_software_write_frame(platform, path));
    FILE *file = fopen(path, "rb");
    assert(file);
    unsigned width, height, range;
    char magic[3];
    assert(fscanf(file, "%2s %u %u %u", magic, &width, &height, &range) == 4);
    assert(!strcmp(magic, "P6") && width == 640 && height == 480 && range == 255);
    assert(fgetc(file) == '\n' && fread(rgb, 1, 3, file) == 3);
    assert(fclose(file) == 0);
}

static void compare_material(GcIplMaterial *source, const uint8_t texel[4],
                             const uint8_t raster[4], const char *path) {
    CcPlatform *platform = cc_platform_create("Material contract", 640, 480);
    assert(platform);
    uint32_t texture = cc_platform_create_texture(platform, 1, 1, texel);
    assert(texture);
    CcMaterialQuad converted;
    assert(gc_render_material(source, &texture, 1, &converted));
    assert(converted.has_alpha_compare && converted.alpha_compare[0] == 0x77 &&
           converted.alpha_compare[1] == 1);
    assert(converted.has_blend_mode && converted.blend_mode[0] == 1 &&
           converted.blend_mode[1] == 4 && converted.blend_mode[2] == 5);
    for (unsigned vertex = 0; vertex < 4; ++vertex) {
        converted.vertices[vertex].x = vertex & 1 ? 2 : 0;
        converted.vertices[vertex].y = vertex & 2 ? 2 : 0;
        converted.vertices[vertex].color =
            (CcColor){(float)raster[0] / 255, (float)raster[1] / 255,
                      (float)raster[2] / 255, (float)raster[3] / 255};
    }
    uint8_t expected[4];
    assert(gc_ipl_material_shade(source, texel, raster, expected));
    cc_platform_begin(platform, (CcColor){0, 0, 0, 1});
    cc_platform_draw_material_quad(platform, &converted);
    uint8_t actual[3];
    read_rgb(platform, path, actual);
    for (unsigned component = 0; component < 3; ++component) {
        int blended = (int)roundf((float)expected[component] * expected[3] / 255);
        int difference = (int)actual[component] - blended;
        if (difference < -1 || difference > 1)
            fprintf(stderr, "material mode=%u component=%u actual=%u expected=%u\n",
                    source->stages[0][0], component, actual[component],
                    (unsigned)blended);
        assert(difference >= -1 && difference <= 1);
    }
    cc_platform_destroy(platform);
}

static void test_native_global_state_and_opacity(const char *path) {
    GcIplMaterial source = source_material();
    source.stages[0][0] = 3;
    /* Serialized records are deliberately impossible to pass. The recovered
     * submitter uses its global ALWAYS/OR/ALWAYS and SRCALPHA blend instead.
     */
    memset(source.alpha_compare, 0, sizeof(source.alpha_compare));
    source.blend[0] = 1;
    source.blend[1] = 0;
    source.blend[2] = 1;
    const uint8_t texel[4] = {80, 160, 240, 255};
    const uint8_t raster[4] = {255, 255, 255, 255};
    compare_material(&source, texel, raster, path);
    CcPlatform *platform = cc_platform_create("Native opacity", 640, 480);
    assert(platform);
    uint32_t texture = cc_platform_create_texture(platform, 1, 1, texel);
    CcMaterialQuad material;
    assert(texture && gc_render_material(&source, &texture, 1, &material));
    for (unsigned vertex = 0; vertex < 4; ++vertex) {
        material.vertices[vertex].x = vertex & 1 ? 2 : 0;
        material.vertices[vertex].y = vertex & 2 ? 2 : 0;
        material.vertices[vertex].color = (CcColor){1, 1, 1, 1};
    }
    const float opacity[] = {0, 0.25f, 0.5f, 1};
    for (unsigned index = 0; index < sizeof(opacity) / sizeof(opacity[0]); ++index) {
        CcMaterialQuad faded = material;
        gc_render_material_opacity(&faded, opacity[index]);
        cc_platform_begin(platform, (CcColor){0, 0, 0, 1});
        cc_platform_draw_material_quad(platform, &faded);
        uint8_t rgb[3];
        read_rgb(platform, path, rgb);
        for (unsigned channel = 0; channel < 3; ++channel)
            assert(abs((int)rgb[channel] -
                       (int)roundf(texel[channel] * opacity[index])) <= 1);
    }
    cc_platform_destroy(platform);
}

static GcIplMaterial source_material(void) {
    GcIplMaterial material = {0};
    material.stage_count = 1;
    material.orders[0][2] = 0;
    return material;
}

static void test_stage_modes(const char *path) {
    const uint8_t texture[4] = {37, 141, 233, 181};
    const uint8_t raster[4] = {231, 93, 52, 207};
    for (unsigned mode = 0; mode < 5; ++mode) {
        GcIplMaterial source = source_material();
        source.stages[0][0] = (uint8_t)mode;
        source.registers[0][0] = 73;
        source.registers[0][1] = 112;
        source.registers[0][2] = 59;
        source.registers[0][3] = -128;
        compare_material(&source, texture, raster, path);
    }
    for (unsigned operation = 0; operation < 2; ++operation) {
        for (unsigned bias = 0; bias < 3; ++bias) {
            for (unsigned scale = 0; scale < 4; ++scale) {
                GcIplMaterial source = source_material();
                uint8_t *stage = source.stages[0];
                stage[0] = 255;
                stage[1] = 2;
                stage[2] = 8;
                stage[3] = 14;
                stage[4] = 10;
                stage[5] = (uint8_t)operation;
                stage[6] = (uint8_t)bias;
                stage[7] = (uint8_t)scale;
                stage[8] = 1;
                stage[10] = 1;
                stage[11] = 4;
                stage[12] = 5;
                stage[13] = 7;
                stage[17] = 1;
                source.registers[0][0] = -200;
                source.registers[0][1] = 112;
                source.registers[0][2] = 59;
                source.registers[0][3] = -128;
                compare_material(&source, texture, raster, path);
            }
        }
    }
}

static void test_negative_alpha(const char *path) {
    GcIplMaterial source = source_material();
    source.stage_count = 2;
    source.registers[0][3] = -128;
    uint8_t *first = source.stages[0];
    first[0] = 255;
    first[1] = first[2] = first[3] = first[4] = 15;
    first[8] = 1;
    first[10] = 1;
    first[11] = 4;
    first[12] = 5;
    first[13] = 7;
    first[17] = 1;
    uint8_t *second = source.stages[1];
    second[0] = 255;
    second[1] = second[2] = second[3] = 15;
    second[4] = 1; /* Previous alpha becomes all three color channels. */
    second[8] = 1;
    second[10] = second[11] = second[12] = 7;
    second[13] = 0;
    second[17] = 1;
    const uint8_t texture[4] = {37, 141, 233, 181};
    const uint8_t raster[4] = {231, 93, 52, 207};
    compare_material(&source, texture, raster, path);
}

static void test_texture_matrix(void) {
    GcIplMaterial material = source_material();
    material.texture_coordinates[0][2] = 30;
    material.texture_coordinates[1][1] = 1;
    material.texture_coordinates[1][2] = 33;
    material.texture_matrix_mask = 3;
    material.texture_matrices[0][0] = 2;
    material.texture_matrices[0][3] = 0.1f;
    material.texture_matrices[0][5] = 3;
    material.texture_matrices[0][7] = 0.2f;
    material.texture_matrices[1][0] = 0.5f;
    material.texture_matrices[1][2] = 0.25f;
    material.texture_matrices[1][3] = 0.3f;
    material.texture_matrices[1][5] = 0.5f;
    material.texture_matrices[1][6] = 0.25f;
    material.texture_matrices[1][7] = 0.4f;
    GcIplVertex vertex = {.uv = {0.25f, 0.5f}};
    const float normal[3] = {0.2f, 0.4f, 0.8f};
    CcMaterialVertex output = {0};
    gc_render_texture_coordinates(&material, &vertex, normal, &output);
    assert(fabsf(output.uv[0][0] - 0.6f) < 0.00001f);
    assert(fabsf(output.uv[0][1] - 1.7f) < 0.00001f);
    assert(fabsf(output.uv[1][0] - 0.6f) < 0.00001f);
    assert(fabsf(output.uv[1][1] - 0.8f) < 0.00001f);
    assert(output.uv[2][0] == 0.25f && output.uv[2][1] == 0.5f);
}

static void test_native_cube_reflection(const char *ipl_path, const char *frame_path) {
    const char *names[] = {"basecube", "cube_in_manu"};
    const uint8_t raster[4] = {125, 210, 94, 255};
    for (unsigned model_index = 0; model_index < 2; ++model_index) {
        GcIplModel model = {0};
        assert(gc_ipl_model_load(ipl_path, names[model_index], &model));
        assert(model.texture_count == 1);
        const GcIplImage *image = &model.textures[0];
        assert(image->rgba && image->width == 128 && image->height == 128);
        unsigned changed_texels = 0;
        for (unsigned sample = 0; sample < 12; ++sample) {
            unsigned x = 5 + sample * 10;
            unsigned y = 121 - sample * 9;
            const uint8_t *texel = image->rgba + ((size_t)y * image->width + x) * 4;
            changed_texels += texel[0] != image->rgba[0];
            for (size_t material_index = 0; material_index < model.material_count;
                 ++material_index) {
                GcIplMaterial *material = &model.materials[material_index];
                assert(material->stage_count == 1 &&
                       material->texture_generator_count == 1);
                const uint8_t *generator = material->texture_coordinates[0];
                assert(generator[0] == 1 && generator[1] == 1 && generator[2] == 30);
                assert(material->texture_matrix_mask == 1);
                const float *matrix = material->texture_matrices[0];
                /* Supply raw normal coordinates at a texel center. The
                 * native MTX2x4 generator has normalization disabled. */
                float normal[3] = {
                    (((float)x + 0.5f) / (float)image->width - matrix[3]) / matrix[0],
                    (((float)y + 0.5f) / (float)image->height - matrix[7]) / matrix[5],
                    0};
                CcMaterialVertex vertex = {0};
                GcIplVertex input = {0};
                gc_render_texture_coordinates(material, &input, normal, &vertex);
                assert(fabsf(vertex.uv[0][0] - ((float)x + 0.5f) / image->width) <
                       0.00001f);
                assert(fabsf(vertex.uv[0][1] - ((float)y + 0.5f) / image->height) <
                       0.00001f);
                compare_material(material, texel, raster, frame_path);
            }
        }
        assert(changed_texels > 6);
        gc_ipl_model_destroy(&model);
    }
}

static void test_mesh_material_mask_bounds(void) {
    const unsigned mask_bits = (unsigned)(sizeof(unsigned) * CHAR_BIT);
    size_t material_count = (size_t)mask_bits + 1;
    GcIplMaterial *materials = calloc(material_count, sizeof(*materials));
    CcMaterialQuad *uploaded = calloc(material_count, sizeof(*uploaded));
    CcPlatform *platform = cc_platform_create("Mesh material bounds", 640, 480);
    assert(materials && uploaded && platform);
    for (size_t index = 0; index < material_count; ++index) {
        memset(materials[index].color[0], 255, sizeof(materials[index].color[0]));
        uploaded[index] = gc_render_raster_material(0);
    }
    GcIplJoint joint = {.parent = -1, .scale = {1, 1, 1}};
    GcIplTriangle triangle = {.vertices = {{{0, 0, 0}, {0, 0, 1}, {0, 0}},
                                           {{10, 0, 0}, {0, 0, 1}, {0, 0}},
                                           {{0, 10, 0}, {0, 0, 1}, {0, 0}}},
                              .material_index = mask_bits};
    GcRenderMeshFace face = {0};
    GcMesh mesh = {.model = {.triangles = &triangle,
                             .triangle_count = 1,
                             .joints = &joint,
                             .joint_count = 1,
                             .materials = materials,
                             .material_count = material_count},
                   .faces = &face,
                   .materials = uploaded};
    GcScene scene = {.platform = platform, .pixel_scale_y = 1};
    const float identity[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    uint8_t pixel[4];
    cc_platform_begin(platform, (CcColor){0, 0, 0, 1});
    gc_render_mesh_draw(&scene, &mesh, identity, identity, 10, 20, 1);
    assert(gc_software_read_pixel(platform, 11, 18, pixel) && pixel[0] == 255);

    /* A selected subset cannot include an index beyond the mask width.
     * The unmasked traversal above must still support that valid index. */
    mesh.material_mask = 1;
    cc_platform_begin(platform, (CcColor){0, 0, 0, 1});
    gc_render_mesh_draw(&scene, &mesh, identity, identity, 10, 20, 1);
    assert(gc_software_read_pixel(platform, 11, 18, pixel) && pixel[0] == 0);

    triangle.material_index = mask_bits - 1;
    mesh.material_mask = 1u << triangle.material_index;
    cc_platform_begin(platform, (CcColor){0, 0, 0, 1});
    gc_render_mesh_draw(&scene, &mesh, identity, identity, 10, 20, 1);
    assert(gc_software_read_pixel(platform, 11, 18, pixel) && pixel[0] == 255);
    cc_platform_destroy(platform);
    free(uploaded);
    free(materials);
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "Files/render-material-test.ppm";
    test_mesh_material_mask_bounds();
    test_stage_modes(path);
    test_negative_alpha(path);
    test_texture_matrix();
    test_native_global_state_and_opacity(path);
    if (argc > 2)
        test_native_cube_reflection(argv[2], path);
    assert(remove(path) == 0);
    puts("render material contract tests passed");
    return 0;
}
