#include "gamecube/ipl_model.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static void put16(uint8_t *bytes, unsigned value) {
    bytes[0] = (uint8_t)(value >> 8);
    bytes[1] = (uint8_t)value;
}

static void put32(uint8_t *bytes, uint32_t value) {
    bytes[0] = (uint8_t)(value >> 24);
    bytes[1] = (uint8_t)(value >> 16);
    bytes[2] = (uint8_t)(value >> 8);
    bytes[3] = (uint8_t)value;
}

static void put_float(uint8_t *bytes, float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    put32(bytes, bits);
}

static void block(uint8_t *bytes, const char *magic, unsigned size) {
    memcpy(bytes, magic, 4);
    put32(bytes + 4, size);
}

/* A first-party synthetic triangle: no recovered asset bytes are embedded. */
static void fixture(uint8_t data[576]) {
    memset(data, 0, 576);
    memcpy(data, "J3D1bmd1", 8);
    put32(data + 8, 576);
    put32(data + 12, 5);
    uint8_t *info = data + 32;
    block(info, "INF1", 64);
    put32(info + 20, 24);
    const uint16_t hierarchy[10] = {16, 0, 1, 0, 18, 0, 2, 0, 0, 0};
    for (unsigned index = 0; index < 10; ++index)
        put16(info + 24 + index * 2, hierarchy[index]);
    uint8_t *vertex = data + 96;
    block(vertex, "VTX1", 160);
    put32(vertex + 8, 64);
    put32(vertex + 12, 96);
    put32(vertex + 64, 9);
    put32(vertex + 68, 1);
    put32(vertex + 72, 4);
    put32(vertex + 80, 255);
    const float positions[9] = {-1, 0, 0, 1, 0, 0, 0, 1, 0};
    for (unsigned index = 0; index < 9; ++index)
        put_float(vertex + 96 + index * 4, positions[index]);
    uint8_t *draw = data + 256;
    block(draw, "DRW1", 32);
    put16(draw + 8, 1);
    put32(draw + 12, 20);
    put32(draw + 16, 22);
    uint8_t *joint = data + 288;
    block(joint, "JNT1", 128);
    put16(joint + 8, 1);
    put32(joint + 12, 24);
    put32(joint + 16, 88);
    put32(joint + 20, 92);
    put_float(joint + 28, 1);
    put_float(joint + 32, 1);
    put_float(joint + 36, 1);
    put16(joint + 92, 1);
    put16(joint + 98, 8);
    memcpy(joint + 100, "triangle", 9);
    uint8_t *shape = data + 416;
    block(shape, "SHP1", 160);
    put16(shape + 8, 1);
    put32(shape + 12, 44);
    put32(shape + 16, 84);
    put32(shape + 24, 88);
    put32(shape + 28, 104);
    put32(shape + 32, 112);
    put32(shape + 36, 128);
    put32(shape + 40, 136);
    put16(shape + 46, 1);
    put32(shape + 88, 9);
    put32(shape + 92, 2);
    put32(shape + 96, 255);
    shape[112] = 0x90;
    put16(shape + 113, 3);
    shape[115] = 0;
    shape[116] = 1;
    shape[117] = 2;
    put16(shape + 130, 1);
    put32(shape + 136, 8);
}

static void test_native_normal_fraction(void) {
    uint8_t data[576];
    fixture(data);
    uint8_t *vertex = data + 96, *shape = data + 416;
    const float positions[9] = {-1, 0, 0, 1, 0, 0, 0, 1, 0};
    put32(vertex + 12, 112);
    put32(vertex + 16, 148);
    put32(vertex + 80, 10);
    put32(vertex + 84, 0);
    put32(vertex + 88, 3);
    vertex[92] = 15;
    put32(vertex + 96, 255);
    for (unsigned i = 0; i < 9; ++i)
        put_float(vertex + 112 + i * 4, positions[i]);
    put16(vertex + 148, 32767);
    put16(vertex + 150, (uint16_t)-16384);
    put16(vertex + 152, 0);
    put32(shape + 28, 126);
    put32(shape + 96, 10);
    put32(shape + 100, 2);
    put32(shape + 104, 255);
    shape[115] = shape[116] = 0;
    shape[117] = 1;
    shape[118] = 0;
    shape[119] = 2;
    shape[120] = 0;
    put32(shape + 136, 12);
    GcIplModel model = {0};
    assert(gc_ipl_model_decode(data, sizeof(data), &model));
    assert(model.triangles[0].vertices[0].normal[0] == 32767.0f / 16384);
    assert(model.triangles[0].vertices[0].normal[1] == -1);
    gc_ipl_model_destroy(&model);
    put32(vertex + 88, 1);
    vertex[148] = 64;
    vertex[149] = (uint8_t)-32;
    vertex[150] = 127;
    assert(gc_ipl_model_decode(data, sizeof(data), &model));
    assert(model.triangles[0].vertices[0].normal[0] == 1 &&
           model.triangles[0].vertices[0].normal[1] == -0.5f &&
           model.triangles[0].vertices[0].normal[2] == 127.0f / 64);
    gc_ipl_model_destroy(&model);
}

static void test_hierarchy_submission_order(void) {
    uint8_t data[736];
    fixture(data);
    memset(data + 576, 0, sizeof(data) - 576);
    put32(data + 8, sizeof(data));
    uint8_t *info = data + 32, *shape = data + 416;
    const uint16_t hierarchy[12] = {16, 0, 1, 0, 18, 1, 18, 0, 2, 0, 0, 0};
    for (unsigned i = 0; i < 12; ++i)
        put16(info + 24 + i * 2, hierarchy[i]);
    memset(shape, 0, 320);
    block(shape, "SHP1", 320);
    put16(shape + 8, 2);
    put32(shape + 12, 44);
    put32(shape + 16, 124);
    put32(shape + 24, 128);
    put32(shape + 28, 144);
    put32(shape + 32, 152);
    put32(shape + 36, 184);
    put32(shape + 40, 200);
    put16(shape + 126, 1);
    put32(shape + 128, 9);
    put32(shape + 132, 2);
    put32(shape + 136, 255);
    for (unsigned i = 0; i < 2; ++i) {
        put16(shape + 44 + i * 40 + 2, 1);
        put16(shape + 44 + i * 40 + 6, i);
        put16(shape + 44 + i * 40 + 8, i);
        put16(shape + 184 + i * 8 + 2, 1);
        put32(shape + 200 + i * 8, 8);
        put32(shape + 200 + i * 8 + 4, i * 8);
        uint8_t *packet = shape + 152 + i * 8;
        packet[0] = 0x90;
        put16(packet + 1, 3);
        packet[3] = (uint8_t)(i ? 2 : 0);
        packet[4] = 1;
        packet[5] = (uint8_t)(i ? 0 : 2);
    }
    GcIplModel model = {0};
    assert(gc_ipl_model_decode(data, sizeof(data), &model));
    assert(model.triangle_count == 2);
    assert(model.triangles[0].shape_index == 1 && model.triangles[1].shape_index == 0);
    assert(model.triangles[0].vertices[0].position[1] == 1);
    gc_ipl_model_destroy(&model);
    put16(info + 38, 1);
    assert(!gc_ipl_model_decode(data, sizeof(data), &model));
}

static void test_vertex_component_overflow(void) {
    uint8_t data[576];
    GcIplModel model = {0};
    fixture(data);
    put32(data + 96 + 68, UINT32_MAX);
    assert(!gc_ipl_model_decode(data, sizeof(data), &model));
    assert(!model.triangles && !model.joints && !model.textures && !model.materials);
    fixture(data);
    put32(data + 96 + 68, UINT32_MAX - 1);
    assert(!gc_ipl_model_decode(data, sizeof(data), &model));
    assert(!model.triangles && !model.joints && !model.textures && !model.materials);
}

static void test_advisory_model_length(void) {
    uint8_t data[576];
    GcIplModel model = {0};
    fixture(data);
    /* Native compressed models can contain sections past a stale header size. */
    put32(data + 8, 575);
    assert(gc_ipl_model_decode(data, sizeof(data), &model));
    assert(model.triangle_count == 1);
    gc_ipl_model_destroy(&model);
    put32(data + 8, 577);
    assert(!gc_ipl_model_decode(data, sizeof(data), &model));
    assert(!model.triangles && !model.joints && !model.textures && !model.materials);
}

int main(void) {
    test_vertex_component_overflow();
    test_advisory_model_length();
    test_native_normal_fraction();
    test_hierarchy_submission_order();
    uint8_t texture[64] = {0};
    texture[0] = 1;
    put16(texture + 2, 8);
    put16(texture + 4, 4);
    texture[6] = 1;
    texture[7] = 2;
    put32(texture + 28, 32);
    texture[32] = 128;
    GcIplImage image = {0};
    assert(gc_ipl_texture_decode(texture, sizeof(texture), &image));
    assert(image.wrap_s == 1 && image.wrap_t == 2 && image.rgba[0] == 128);
    gc_ipl_image_destroy(&image);
    texture[6] = 3;
    assert(!gc_ipl_texture_decode(texture, sizeof(texture), &image));
    uint8_t data[576];
    fixture(data);
    GcIplModel model = {0};
    assert(gc_ipl_model_decode(data, sizeof(data), &model));
    assert(model.triangle_count == 1 && model.joint_count == 1);
    assert(!strcmp(model.joints[0].name, "triangle"));
    assert(model.triangles[0].vertices[2].position[1] == 1);
    model.joints[0].translation[0] = 3;
    model.joints[0].rotation[2] = 1.57079632679f;
    GcIplVertex transformed;
    assert(gc_ipl_model_transform_vertex(&model, 0, &model.triangles[0].vertices[0],
                                         &transformed));
    assert(fabsf(transformed.position[0] - 3) < 0.0001f);
    assert(fabsf(transformed.position[1] + 1) < 0.0001f);
    GcIplVertex with_normal = {.normal = {1, 0, 0}};
    model.joints[0].scale[0] = 2;
    assert(gc_ipl_model_transform_vertex_native(&model, 0, &with_normal, &transformed));
    assert(fabsf(transformed.normal[0]) < 0.0001f &&
           fabsf(transformed.normal[1] - 0.5f) < 0.0001f);
    assert(gc_ipl_model_transform_vertex(&model, 0, &with_normal, &transformed));
    assert(fabsf(transformed.normal[1] - 1) < 0.0001f);
    float texgen_normal[3] = {1, 0, 0};
    assert(gc_ipl_model_transform_direction(&model, 0, texgen_normal, texgen_normal));
    assert(fabsf(texgen_normal[0]) < 0.0001f && fabsf(texgen_normal[1] - 2) < 0.0001f &&
           texgen_normal[2] == 0);
    model.joints[0].parent = 0;
    assert(!gc_ipl_model_transform_direction(&model, 0, with_normal.normal,
                                             texgen_normal));
    assert(!gc_ipl_model_transform_vertex(&model, 0, &model.triangles[0].vertices[0],
                                          &transformed));
    gc_ipl_model_destroy(&model);
    assert(!model.joints && !model.triangles);
    float affine[12] = {2, 0, 1, 50, 0, 3, 0, 20, 0, 0, 4, 70};
    float normal[3] = {1, 0, 0};
    assert(gc_ipl_model_transform_normal(affine, normal, normal));
    assert(normal[0] == 0.5f && normal[1] == 0 && normal[2] == -0.125f);
    affine[0] = 0;
    assert(!gc_ipl_model_transform_normal(affine, normal, normal));
    fixture(data);
    data[416 + 112] = 0x98;
    put16(data + 416 + 113, 4);
    data[416 + 118] = 3;
    put_float(data + 96 + 132, 2);
    put_float(data + 96 + 136, 1);
    assert(gc_ipl_model_decode(data, sizeof(data), &model));
    assert(model.triangle_count == 2);
    const GcIplTriangle *second = &model.triangles[1];
    assert(second->vertices[0].position[1] == 1);
    assert(second->vertices[1].position[0] == 1);
    assert(second->vertices[2].position[0] == 2);
    float first_x = second->vertices[1].position[0] - second->vertices[0].position[0];
    float first_y = second->vertices[1].position[1] - second->vertices[0].position[1];
    float second_x = second->vertices[2].position[0] - second->vertices[0].position[0];
    float second_y = second->vertices[2].position[1] - second->vertices[0].position[1];
    assert(first_x * second_y - first_y * second_x > 0);
    gc_ipl_model_destroy(&model);
    fixture(data);
    data[416 + 117] = 255;
    assert(!gc_ipl_model_decode(data, sizeof(data), &model));
    fixture(data);
    put_float(data + 96 + 96, NAN);
    assert(!gc_ipl_model_decode(data, sizeof(data), &model));
    fixture(data);
    assert(!gc_ipl_model_decode(data, sizeof(data) - 1, &model));
    fixture(data);
    put32(data + 416 + 140, UINT32_MAX);
    assert(!gc_ipl_model_decode(data, sizeof(data), &model));
    puts("IPL model tests passed");
    return 0;
}
