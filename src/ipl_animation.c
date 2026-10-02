#include "gamecube/ipl_animation.h"
#include "console_common/support/endian.h"
#include "console_common/support/bounds.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static float array_value(const GcIplAnimation *animation, unsigned array, size_t index,
                         bool time) {
    const uint8_t *data = animation->data + animation->arrays[array];
    if (animation->kind == GC_IPL_ANIMATION_JOINT && array == 1)
        return time ? cc_read_be16(data + index * 2)
                    : (int16_t)cc_read_be16(data + index * 2);
    return cc_read_be_float(data + index * 4);
}

static bool validate_track(const GcIplAnimation *animation, const uint8_t *descriptor,
                           unsigned array) {
    unsigned count = cc_read_be16(descriptor);
    size_t start = cc_read_be16(descriptor + 2);
    unsigned tangent_mode = cc_read_be16(descriptor + 4);
    if (tangent_mode > 1)
        return false;
    size_t stride = tangent_mode ? 4 : 3;
    size_t length = count > 1 ? (size_t)count * stride : count;
    if (!cc_bounds_contains(animation->array_counts[array], start, length))
        return false;
    float previous_time = -INFINITY;
    for (size_t index = 0; index < length; ++index) {
        bool time = count > 1 && index % stride == 0;
        float value = array_value(animation, array, start + index, time);
        if (!isfinite(value))
            return false;
        if (time) {
            if (value <= previous_time)
                return false;
            previous_time = value;
        }
    }
    return true;
}

static bool validate_tracks(const GcIplAnimation *animation) {
    unsigned groups = animation->kind == GC_IPL_ANIMATION_JOINT ? 3 : 4;
    size_t descriptor_size = animation->kind == GC_IPL_ANIMATION_JOINT ? 54 : 24;
    for (unsigned track = 0; track < animation->track_count; ++track) {
        unsigned remapped =
            animation->kind == GC_IPL_ANIMATION_COLOR
                ? cc_read_be16(animation->data + animation->remap + track * 2)
                : track;
        if (remapped >= animation->track_count)
            return false;
        const uint8_t *descriptor =
            animation->data + animation->descriptors + remapped * descriptor_size;
        unsigned axes = animation->kind == GC_IPL_ANIMATION_JOINT ? 3 : 1;
        for (unsigned axis = 0; axis < axes; ++axis)
            for (unsigned group = 0; group < groups; ++group)
                if (!validate_track(animation, descriptor + (axis * groups + group) * 6,
                                    group))
                    return false;
    }
    return true;
}

static bool decode_joint_header(const uint8_t *data, size_t size,
                                GcIplAnimation *result) {
    if (size < 68 || memcmp(data, "J3D1bck1", 8))
        return false;
    size_t declared_size = cc_read_be32(data + 8);
    const uint8_t *block = data + 32;
    size_t block_size = cc_read_be32(block + 4);
    if (declared_size < 68 || declared_size > size || cc_read_be32(data + 12) != 1 ||
        memcmp(block, "ANK1", 4) || block_size < 36 || block_size > declared_size - 32)
        return false;
    result->size = declared_size;
    result->loop_mode = block[8];
    result->rotation_shift = block[9];
    result->duration_frames = cc_read_be16(block + 10);
    result->track_count = cc_read_be16(block + 12);
    if (result->rotation_shift > 15)
        return false;
    size_t descriptors = cc_read_be32(block + 20);
    if (!cc_bounds_contains(block_size, descriptors, (size_t)result->track_count * 54))
        return false;
    result->descriptors = 32 + descriptors;
    /* BCK offsets are relative to ANK1. Caller padding must not extend
     * the descriptor or key-array bounds of that block. */
    for (unsigned array = 0; array < 3; ++array) {
        size_t offset = cc_read_be32(block + 24 + array * 4);
        result->array_counts[array] = cc_read_be16(block + 14 + array * 2);
        size_t width = array == 1 ? 2 : 4;
        if (!cc_bounds_contains(block_size, offset,
                                result->array_counts[array] * width))
            return false;
        result->arrays[array] = 32 + offset;
    }
    return true;
}

static bool decode_color_header(const uint8_t *data, size_t size,
                                GcIplAnimation *result) {
    if (memcmp(data, "IPK1", 4))
        return false;
    size_t declared_size = cc_read_be32(data + 4);
    if (declared_size < 36 || declared_size > size)
        return false;
    result->size = declared_size;
    result->loop_mode = cc_read_be16(data + 8);
    result->duration_frames = cc_read_be16(data + 10);
    result->remap = cc_read_be32(data + 12);
    result->descriptors = cc_read_be32(data + 16);
    if (result->descriptors < result->remap ||
        (result->descriptors - result->remap) % 2)
        return false;
    size_t tracks = (result->descriptors - result->remap) / 2;
    if (tracks > 256 || !cc_bounds_contains(declared_size, result->remap, tracks * 2) ||
        !cc_bounds_contains(declared_size, result->descriptors, tracks * 24))
        return false;
    result->track_count = (unsigned)tracks;
    for (unsigned array = 0; array < 4; ++array) {
        result->arrays[array] = cc_read_be32(data + 20 + array * 4);
        size_t end = array == 3 ? declared_size : cc_read_be32(data + 24 + array * 4);
        if (end < result->arrays[array] || end > declared_size ||
            (end - result->arrays[array]) % 4)
            return false;
        result->array_counts[array] = (end - result->arrays[array]) / 4;
    }
    return true;
}

bool gc_ipl_animation_decode(const uint8_t *data, size_t size, GcIplAnimationKind kind,
                             GcIplAnimation *animation) {
    if (!data || !animation || animation->data || size > GC_IPL_ROM_SIZE || size < 36 ||
        (kind != GC_IPL_ANIMATION_JOINT && kind != GC_IPL_ANIMATION_COLOR))
        return false;
    GcIplAnimation result = {.kind = kind};
    bool header_valid = kind == GC_IPL_ANIMATION_JOINT
                            ? decode_joint_header(data, size, &result)
                            : decode_color_header(data, size, &result);
    if (!header_valid || !result.track_count || result.track_count > 256 ||
        !result.duration_frames)
        return false;
    result.data = malloc(result.size);
    if (!result.data)
        return false;
    memcpy(result.data, data, result.size);
    if (!validate_tracks(&result)) {
        gc_ipl_animation_destroy(&result);
        return false;
    }
    *animation = result;
    return true;
}

bool gc_ipl_animation_load(const char *ipl_path, GcIplAnimationKind kind,
                           GcIplAnimation *animation) {
    if (!ipl_path || !animation || animation->data)
        return false;
    uint8_t *rom = NULL;
    if (!gc_ipl_rom_read(ipl_path, &rom))
        return false;
    if (cc_read_be32(rom + GC_IPL_SCRAMBLED_START) != UINT32_C(0x3c800011))
        gc_ipl_descramble(rom, GC_IPL_ROM_SIZE);
    const char *signature = kind == GC_IPL_ANIMATION_JOINT ? "J3D1bck1" : "IPK1";
    size_t signature_size = kind == GC_IPL_ANIMATION_JOINT ? 8 : 4;
    bool okay = false;
    for (size_t offset = GC_IPL_BS2_OFFSET; offset + 36 <= GC_IPL_SCRAMBLED_END;
         ++offset) {
        if (memcmp(rom + offset, signature, signature_size))
            continue;
        size_t length =
            cc_read_be32(rom + offset + (kind == GC_IPL_ANIMATION_JOINT ? 8 : 4));
        if (length <= GC_IPL_SCRAMBLED_END - offset &&
            gc_ipl_animation_decode(rom + offset, length, kind, animation)) {
            okay = true;
            break;
        }
    }
    free(rom);
    return okay;
}

void gc_ipl_animation_destroy(GcIplAnimation *animation) {
    if (!animation)
        return;
    free(animation->data);
    memset(animation, 0, sizeof(*animation));
}

/* Native USA BS2 0x81306b54/0x81306c28 select neighboring keys;
 * 0x81307e20 performs this cubic Hermite interpolation with split tangents.
 */
static float evaluate_track(const GcIplAnimation *animation, const uint8_t *descriptor,
                            unsigned array, float time, float default_value) {
    unsigned count = cc_read_be16(descriptor);
    size_t start = cc_read_be16(descriptor + 2);
    unsigned stride = cc_read_be16(descriptor + 4) ? 4 : 3;
    if (!count)
        return default_value;
    if (count == 1)
        return array_value(animation, array, start, false);
    if (time <= array_value(animation, array, start, true))
        return array_value(animation, array, start + 1, false);
    size_t last = start + (size_t)(count - 1) * stride;
    if (time >= array_value(animation, array, last, true))
        return array_value(animation, array, last + 1, false);
    unsigned first = 0;
    unsigned end = count - 1;
    while (end - first > 1) {
        unsigned middle = first + (end - first) / 2;
        if (time < array_value(animation, array, start + (size_t)middle * stride, true))
            end = middle;
        else
            first = middle;
    }
    size_t left = start + (size_t)first * stride;
    size_t right = left + stride;
    float t0 = array_value(animation, array, left, true);
    float t1 = array_value(animation, array, right, true);
    float interval = t1 - t0;
    float t = (time - t0) / interval;
    float t2 = t * t;
    float t3 = t2 * t;
    float value0 = array_value(animation, array, left + 1, false);
    float value1 = array_value(animation, array, right + 1, false);
    float tangent0 = array_value(animation, array, left + stride - 1, false);
    float tangent1 = array_value(animation, array, right + 2, false);
    return (2 * t3 - 3 * t2 + 1) * value0 + (t3 - 2 * t2 + t) * interval * tangent0 +
           (-2 * t3 + 3 * t2) * value1 + (t3 - t2) * interval * tangent1;
}

bool gc_ipl_animation_apply(const GcIplAnimation *animation, float frame,
                            GcIplModel *model) {
    if (!animation || !animation->data || !model || !isfinite(frame))
        return false;
    float time = fmaxf(0, fminf((float)animation->duration_frames, frame));
    if (animation->kind == GC_IPL_ANIMATION_JOINT) {
        if (!model->joints || model->joint_count != animation->track_count)
            return false;
        for (unsigned joint = 0; joint < animation->track_count; ++joint) {
            const uint8_t *descriptor =
                animation->data + animation->descriptors + joint * 54;
            for (unsigned axis = 0; axis < 3; ++axis) {
                model->joints[joint].scale[axis] =
                    evaluate_track(animation, descriptor + axis * 18, 0, time, 1);
                float native_angle = truncf(
                    evaluate_track(animation, descriptor + axis * 18 + 6, 1, time, 0));
                float wrapped = fmodf(
                    native_angle * (float)(1u << animation->rotation_shift), 65536.0f);
                if (wrapped < 0)
                    wrapped += 65536;
                uint16_t bits = (uint16_t)wrapped;
                int16_t angle;
                memcpy(&angle, &bits, sizeof(angle));
                model->joints[joint].rotation[axis] =
                    angle * (3.14159265358979323846f / 32768.0f);
                model->joints[joint].translation[axis] =
                    evaluate_track(animation, descriptor + axis * 18 + 12, 2, time, 0);
            }
        }
    } else {
        if (!model->materials || model->material_count != animation->track_count)
            return false;
        /* USA 0x8130702c divides frame time by the verified SDA float 30. */
        time /= 30;
        for (unsigned material = 0; material < animation->track_count; ++material) {
            unsigned index =
                cc_read_be16(animation->data + animation->remap + material * 2);
            const uint8_t *descriptor =
                animation->data + animation->descriptors + index * 24;
            for (unsigned component = 0; component < 4; ++component) {
                float value = evaluate_track(animation, descriptor + component * 6,
                                             component, time, 0);
                model->materials[material].color[0][component] =
                    (uint8_t)fmaxf(0, fminf(255, truncf(value)));
            }
        }
    }
    return true;
}
