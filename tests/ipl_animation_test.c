#include "gamecube/ipl_animation.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static void put16(uint8_t *data, unsigned value) {
    data[0] = (uint8_t)(value >> 8);
    data[1] = (uint8_t)value;
}

static void put32(uint8_t *data, uint32_t value) {
    data[0] = (uint8_t)(value >> 24);
    data[1] = (uint8_t)(value >> 16);
    data[2] = (uint8_t)(value >> 8);
    data[3] = (uint8_t)value;
}

static void put_float(uint8_t *data, float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    put32(data, bits);
}

static void joint_fixture(uint8_t data[256]) {
    memset(data, 0, 256);
    memcpy(data, "J3D1bck1", 8);
    put32(data + 8, 256);
    put32(data + 12, 1);
    uint8_t *block = data + 32;
    memcpy(block, "ANK1", 4);
    put32(block + 4, 224);
    put16(block + 10, 10);
    put16(block + 12, 1);
    put16(block + 14, 1);
    put16(block + 16, 1);
    put16(block + 18, 8);
    put32(block + 20, 36);
    put32(block + 24, 96);
    put32(block + 28, 128);
    put32(block + 32, 136);
    put16(data + 68, 1); /* Constant X scale. */
    put16(data + 74, 1); /* Constant X rotation. */
    put16(data + 80, 2); /* Two translation keys with distinct tangents. */
    put16(data + 84, 1);
    put_float(data + 128, 2);
    put16(data + 160, 16384);
    const float keys[8] = {0, 0, 0, 1, 10, 20, 3, 0};
    for (unsigned index = 0; index < 8; ++index)
        put_float(data + 168 + index * 4, keys[index]);
}

static void test_joint_hermite(void) {
    uint8_t data[256];
    joint_fixture(data);
    GcIplAnimation animation = {0};
    assert(gc_ipl_animation_decode(data, sizeof(data), GC_IPL_ANIMATION_JOINT,
                                   &animation));
    GcIplJoint joint = {0};
    GcIplModel model = {0};
    model.joints = &joint;
    model.joint_count = 1;
    assert(gc_ipl_animation_apply(&animation, 5, &model));
    assert(fabsf(joint.translation[0] - 7.5f) < 0.0001f);
    assert(joint.scale[0] == 2 && joint.scale[1] == 1 && joint.scale[2] == 1);
    assert(fabsf(joint.rotation[0] - 1.57079632679f) < 0.0001f);
    assert(gc_ipl_animation_apply(&animation, -100, &model));
    assert(joint.translation[0] == 0);
    assert(gc_ipl_animation_apply(&animation, 100, &model));
    assert(joint.translation[0] == 20);
    assert(!gc_ipl_animation_apply(&animation, NAN, &model));
    model.joint_count = 2;
    assert(!gc_ipl_animation_apply(&animation, 5, &model));
    gc_ipl_animation_destroy(&animation);
    assert(!animation.data);
}

static void test_invalid_joint_tracks(void) {
    uint8_t data[256];
    GcIplAnimation animation = {0};
    joint_fixture(data);
    put16(data + 84, 2); /* Invalid tangent mode. */
    assert(!gc_ipl_animation_decode(data, sizeof(data), GC_IPL_ANIMATION_JOINT,
                                    &animation));
    joint_fixture(data);
    put_float(data + 184, 0); /* Duplicate key time. */
    assert(!gc_ipl_animation_decode(data, sizeof(data), GC_IPL_ANIMATION_JOINT,
                                    &animation));
    joint_fixture(data);
    put_float(data + 180, NAN);
    assert(!gc_ipl_animation_decode(data, sizeof(data), GC_IPL_ANIMATION_JOINT,
                                    &animation));
    joint_fixture(data);
    put16(data + 82, 65535); /* Out-of-bounds array index. */
    assert(!gc_ipl_animation_decode(data, sizeof(data), GC_IPL_ANIMATION_JOINT,
                                    &animation));
    joint_fixture(data);
    assert(!gc_ipl_animation_decode(data, 190, GC_IPL_ANIMATION_JOINT, &animation));
    assert(!animation.data);
}

static void test_joint_block_bounds(void) {
    uint8_t data[256];
    GcIplAnimation animation = {0};
    joint_fixture(data);
    put32(data + 8, 199); /* Key array ends at 200, outside the declared file. */
    assert(!gc_ipl_animation_decode(data, sizeof(data), GC_IPL_ANIMATION_JOINT,
                                    &animation));
    assert(!animation.data && !animation.size);
    joint_fixture(data);
    put32(data + 36, 167); /* Caller padding cannot extend the ANK1 block. */
    assert(!gc_ipl_animation_decode(data, sizeof(data), GC_IPL_ANIMATION_JOINT,
                                    &animation));
    assert(!animation.data && !animation.size);
    joint_fixture(data);
    put32(data + 36, 225); /* ANK1 exceeds the enclosing J3D file. */
    assert(!gc_ipl_animation_decode(data, sizeof(data), GC_IPL_ANIMATION_JOINT,
                                    &animation));
    assert(!animation.data && !animation.size);
}

static void test_color_tracks(void) {
    uint8_t data[78] = {0};
    memcpy(data, "IPK1", 4);
    put32(data + 4, sizeof(data));
    put16(data + 8, 1);
    put16(data + 10, 65);
    put32(data + 12, 36);
    put32(data + 16, 38);
    const float colors[4] = {255, 128.75f, 64, 200};
    for (unsigned component = 0; component < 4; ++component) {
        put32(data + 20 + component * 4, 62 + component * 4);
        put16(data + 38 + component * 6, 1);
        put_float(data + 62 + component * 4, colors[component]);
    }
    GcIplAnimation animation = {0};
    assert(gc_ipl_animation_decode(data, sizeof(data), GC_IPL_ANIMATION_COLOR,
                                   &animation));
    GcIplMaterial material = {0};
    GcIplModel model = {0};
    model.materials = &material;
    model.material_count = 1;
    assert(gc_ipl_animation_apply(&animation, 32, &model));
    const uint8_t expected[4] = {255, 128, 64, 200};
    assert(!memcmp(material.color[0], expected, 4));
    gc_ipl_animation_destroy(&animation);
    put16(data + 36, 1); /* Out-of-bounds material remap. */
    assert(!gc_ipl_animation_decode(data, sizeof(data), GC_IPL_ANIMATION_COLOR,
                                    &animation));
}

int main(void) {
    test_joint_hermite();
    test_invalid_joint_tracks();
    test_joint_block_bounds();
    test_color_tracks();
    puts("IPL animation tests passed");
    return 0;
}
