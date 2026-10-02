#include "gamecube/ipl_model.h"
#include "console_common/support/endian.h"
#include "console_common/support/bounds.h"
#include "gamecube/angle.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define MODEL_MAX_TRIANGLES ((size_t)65536)
#define MODEL_MAX_JOINTS 256
#define MODEL_MAX_TEXTURES 64

typedef struct {
    const uint8_t *bytes;
    size_t size;
} ModelBlock;

typedef struct {
    ModelBlock info;
    ModelBlock vertex;
    ModelBlock draw;
    ModelBlock joint;
    ModelBlock shape;
    ModelBlock material;
    ModelBlock texture;
} ModelBlocks;

typedef struct {
    unsigned attribute;
    unsigned components;
    unsigned component_type;
    unsigned fraction_bits;
    size_t data_offset;
    size_t data_size;
} VertexArray;

typedef struct {
    unsigned attribute;
    unsigned index_type;
} VertexDescriptor;

typedef struct {
    ModelBlocks blocks;
    VertexArray arrays[3];
    unsigned shape_material[256];
    unsigned shape_joint[256];
    unsigned shape_order[256];
    unsigned shape_count;
    GcIplModel *model;
    size_t triangle_cursor;
} ModelDecoder;

static bool span(ModelBlock block, size_t offset, size_t size) {
    return cc_bounds_contains(block.size, offset, size);
}

static bool collect_blocks(const uint8_t *bytes, size_t size, ModelBlocks *blocks) {
    if (!bytes || size < 32 || memcmp(bytes, "J3D1bmd1", 8) ||
        cc_read_be32(bytes + 8) < 32 || cc_read_be32(bytes + 8) > size)
        return false;
    /* Some native compressed models retain a stale J3D size. Bound sections
     * by the enclosing decoded resource, which includes their texture data. */
    unsigned count = cc_read_be32(bytes + 12);
    if (count > 32)
        return false;
    size_t offset = 32;
    for (unsigned index = 0; index < count; ++index) {
        if (offset > size || size - offset < 8)
            return false;
        const uint8_t *section = bytes + offset;
        size_t section_size = cc_read_be32(section + 4);
        if (section_size < 8 || section_size > size - offset)
            return false;
        ModelBlock block = {section, section_size};
        if (!memcmp(section, "INF1", 4))
            blocks->info = block;
        else if (!memcmp(section, "VTX1", 4))
            blocks->vertex = block;
        else if (!memcmp(section, "DRW1", 4))
            blocks->draw = block;
        else if (!memcmp(section, "JNT1", 4))
            blocks->joint = block;
        else if (!memcmp(section, "SHP1", 4))
            blocks->shape = block;
        else if (!memcmp(section, "MAT1", 4))
            blocks->material = block;
        else if (!memcmp(section, "TEX1", 4))
            blocks->texture = block;
        offset += section_size;
    }
    return blocks->info.bytes && blocks->vertex.bytes && blocks->draw.bytes &&
           blocks->joint.bytes && blocks->shape.bytes;
}

static bool read_native_name(ModelBlock block, size_t names, unsigned index,
                             char output[64]) {
    if (!span(block, names, 4 + (size_t)(index + 1) * 4))
        return false;
    size_t relative = cc_read_be16(block.bytes + names + 4 + index * 4 + 2);
    if (!span(block, names, relative))
        return false;
    size_t start = names + relative;
    size_t length = 0;
    while (start + length < block.size && block.bytes[start + length])
        ++length;
    if (start + length == block.size || length >= 64)
        return false;
    memcpy(output, block.bytes + start, length + 1);
    return true;
}

static bool decode_joints(ModelDecoder *decoder) {
    ModelBlock block = decoder->blocks.joint;
    if (block.size < 24)
        return false;
    unsigned count = cc_read_be16(block.bytes + 8);
    size_t entries = cc_read_be32(block.bytes + 12);
    size_t remap = cc_read_be32(block.bytes + 16);
    size_t names = cc_read_be32(block.bytes + 20);
    if (!count || count > MODEL_MAX_JOINTS || entries > block.size ||
        !span(block, remap, (size_t)count * 2))
        return false;
    decoder->model->joints = calloc(count, sizeof(GcIplJoint));
    if (!decoder->model->joints)
        return false;
    decoder->model->joint_count = count;
    for (unsigned index = 0; index < count; ++index) {
        unsigned remapped = cc_read_be16(block.bytes + remap + index * 2);
        size_t offset = entries + (size_t)remapped * 64;
        if (!span(block, offset, 64))
            return false;
        GcIplJoint *joint = &decoder->model->joints[index];
        joint->parent = -1;
        if (!read_native_name(block, names, index, joint->name))
            return false;
        for (unsigned component = 0; component < 3; ++component) {
            joint->scale[component] =
                cc_read_be_float(block.bytes + offset + 4 + component * 4);
            joint->rotation[component] =
                (int16_t)cc_read_be16(block.bytes + offset + 16 + component * 2) *
                (3.14159265358979323846f / 32768.0f);
            joint->translation[component] =
                cc_read_be_float(block.bytes + offset + 24 + component * 4);
            if (!isfinite(joint->scale[component]) ||
                !isfinite(joint->translation[component]))
                return false;
        }
    }
    return true;
}

static bool decode_hierarchy(ModelDecoder *decoder) {
    ModelBlock block = decoder->blocks.info;
    if (block.size < 24)
        return false;
    size_t offset = cc_read_be32(block.bytes + 20);
    int parent_stack[64];
    unsigned depth = 0;
    int current_joint = -1;
    unsigned current_material = UINT_MAX;
    while (span(block, offset, 4)) {
        unsigned command = cc_read_be16(block.bytes + offset);
        unsigned index = cc_read_be16(block.bytes + offset + 2);
        offset += 4;
        if (!command)
            return depth == 0;
        if (command == 1) {
            if (depth == 64)
                return false;
            parent_stack[depth++] = current_joint;
        } else if (command == 2) {
            if (!depth)
                return false;
            current_joint = parent_stack[--depth];
        } else if (command == 0x10) {
            if (index >= decoder->model->joint_count)
                return false;
            decoder->model->joints[index].parent = depth ? parent_stack[depth - 1] : -1;
            current_joint = (int)index;
        } else if (command == 0x11) {
            if (index >= 256)
                return false;
            current_material = index;
        } else if (command == 0x12) {
            if (index >= 256 || current_joint < 0 || decoder->shape_count == 256 ||
                decoder->shape_joint[index] != UINT_MAX)
                return false;
            decoder->shape_joint[index] = (unsigned)current_joint;
            decoder->shape_material[index] = current_material;
            decoder->shape_order[decoder->shape_count++] = index;
        } else {
            return false;
        }
    }
    return false;
}

static unsigned component_size(unsigned type) {
    if (type <= 1)
        return 1;
    if (type <= 3)
        return 2;
    return type == 4 ? 4 : 0;
}

static bool decode_arrays(ModelDecoder *decoder) {
    ModelBlock block = decoder->blocks.vertex;
    if (block.size < 64)
        return false;
    size_t formats = cc_read_be32(block.bytes + 8);
    for (unsigned index = 0; index < 16 && span(block, formats, 16); ++index) {
        unsigned attribute = cc_read_be32(block.bytes + formats);
        if (attribute == 0xff)
            return decoder->arrays[0].data_offset != 0;
        int array_index = attribute == 9    ? 0
                          : attribute == 10 ? 1
                          : attribute == 13 ? 2
                                            : -1;
        if (array_index >= 0) {
            VertexArray *array = &decoder->arrays[array_index];
            array->attribute = attribute;
            unsigned count = cc_read_be32(block.bytes + formats + 4);
            /* Position and UV formats encode one of two component counts.
             * Validate before addition so malformed counts cannot wrap. */
            if (attribute != 10 && count > 1)
                return false;
            array->components = attribute == 9    ? count + 2
                                : attribute == 10 ? 3
                                                  : count + 1;
            array->component_type = cc_read_be32(block.bytes + formats + 8);
            array->fraction_bits = block.bytes[formats + 12];
            /* GXSetVtxAttrFmt (USA 4a16c) ignores the file's fraction
             * operand for normals. The vertex unit uses signed 1.6 / 1.14
             * fixed point, including original S16 records marked frac15.
             */
            if (attribute == 10) {
                if (array->component_type == 1)
                    array->fraction_bits = 6;
                else if (array->component_type == 3)
                    array->fraction_bits = 14;
                else if (array->component_type == 4)
                    array->fraction_bits = 0;
                else
                    return false;
            }
            unsigned data_slot = attribute == 9 ? 0 : attribute == 10 ? 1 : 5;
            array->data_offset = cc_read_be32(block.bytes + 12 + data_slot * 4);
            if (!array->data_offset || array->data_offset > block.size ||
                array->components > (attribute == 13 ? 2u : 3u) ||
                array->fraction_bits > 30 || !component_size(array->component_type))
                return false;
            size_t end = block.size;
            for (unsigned slot = 0; slot < 13; ++slot) {
                size_t candidate = cc_read_be32(block.bytes + 12 + slot * 4);
                if (candidate > array->data_offset && candidate < end)
                    end = candidate;
            }
            array->data_size = end - array->data_offset;
        }
        formats += 16;
    }
    return false;
}

static bool read_array(ModelDecoder *decoder, unsigned attribute, unsigned index,
                       float *output) {
    unsigned array_index = attribute == 9 ? 0 : attribute == 10 ? 1 : 2;
    const VertexArray *array = &decoder->arrays[array_index];
    size_t element_size = component_size(array->component_type);
    size_t stride = element_size * array->components;
    if (!stride || index >= array->data_size / stride)
        return false;
    const uint8_t *bytes =
        decoder->blocks.vertex.bytes + array->data_offset + index * stride;
    float divisor = (float)(UINT32_C(1) << array->fraction_bits);
    for (unsigned component = 0; component < array->components; ++component) {
        const uint8_t *value = bytes + component * element_size;
        switch (array->component_type) {
            case 0:
                output[component] = *value / divisor;
                break;
            case 1:
                output[component] = (int8_t)*value / divisor;
                break;
            case 2:
                output[component] = cc_read_be16(value) / divisor;
                break;
            case 3:
                output[component] = (int16_t)cc_read_be16(value) / divisor;
                break;
            case 4:
                output[component] = cc_read_be_float(value);
                break;
            default:
                return false;
        }
        if (!isfinite(output[component]))
            return false;
    }
    return true;
}

static bool read_descriptors(ModelBlock block, size_t offset,
                             VertexDescriptor descriptors[16], unsigned *count) {
    *count = 0;
    for (unsigned index = 0; index < 16 && span(block, offset, 8); ++index) {
        unsigned attribute = cc_read_be32(block.bytes + offset);
        if (attribute == 0xff)
            return *count != 0;
        unsigned type = cc_read_be32(block.bytes + offset + 4);
        if (type < 1 || type > 3 || (type == 1 && attribute > 8))
            return false;
        descriptors[(*count)++] = (VertexDescriptor){attribute, type};
        offset += 8;
    }
    return false;
}

static bool read_vertex(ModelDecoder *decoder, ModelBlock packet, size_t *offset,
                        const VertexDescriptor *descriptors, unsigned count,
                        GcIplVertex *vertex) {
    memset(vertex, 0, sizeof(*vertex));
    for (unsigned index = 0; index < count; ++index) {
        const VertexDescriptor *descriptor = &descriptors[index];
        unsigned bytes = descriptor->index_type == 3 ? 2 : 1;
        if (!span(packet, *offset, bytes))
            return false;
        unsigned value =
            bytes == 2 ? cc_read_be16(packet.bytes + *offset) : packet.bytes[*offset];
        *offset += bytes;
        if (descriptor->attribute == 9) {
            if (!read_array(decoder, 9, value, vertex->position))
                return false;
        } else if (descriptor->attribute == 10) {
            if (!read_array(decoder, 10, value, vertex->normal))
                return false;
        } else if (descriptor->attribute == 13) {
            if (!read_array(decoder, 13, value, vertex->uv))
                return false;
        } else if (descriptor->attribute > 8) {
            return false;
        }
    }
    return true;
}

static bool append_triangle(ModelDecoder *decoder, unsigned shape, unsigned joint,
                            const GcIplVertex *a, const GcIplVertex *b,
                            const GcIplVertex *c) {
    if (decoder->triangle_cursor >= MODEL_MAX_TRIANGLES)
        return false;
    if (decoder->model->triangles) {
        if (decoder->triangle_cursor >= decoder->model->triangle_count)
            return false;
        GcIplTriangle *triangle = &decoder->model->triangles[decoder->triangle_cursor];
        triangle->vertices[0] = *a;
        triangle->vertices[1] = *b;
        triangle->vertices[2] = *c;
        triangle->shape_index = shape;
        triangle->material_index = decoder->shape_material[shape];
        triangle->joint_index = joint;
    }
    ++decoder->triangle_cursor;
    return true;
}

static bool decode_packet(ModelDecoder *decoder, ModelBlock packet,
                          const VertexDescriptor *descriptors, unsigned count,
                          unsigned shape, unsigned joint) {
    size_t offset = 0;
    while (offset < packet.size) {
        unsigned opcode = packet.bytes[offset++];
        if (!opcode)
            continue;
        unsigned primitive = opcode & 0xf8;
        if ((primitive != 0x80 && primitive != 0x90 && primitive != 0x98 &&
             primitive != 0xa0) ||
            !span(packet, offset, 2))
            return false;
        unsigned vertex_count = cc_read_be16(packet.bytes + offset);
        offset += 2;
        if (vertex_count > MODEL_MAX_TRIANGLES || vertex_count < 3 ||
            (primitive == 0x80 && vertex_count % 4) ||
            (primitive == 0x90 && vertex_count % 3))
            return false;
        GcIplVertex ring[4];
        for (unsigned vertex_index = 0; vertex_index < vertex_count; ++vertex_index) {
            GcIplVertex vertex;
            if (!read_vertex(decoder, packet, &offset, descriptors, count, &vertex))
                return false;
            if (primitive == 0x80 || primitive == 0x90) {
                unsigned length = primitive == 0x80 ? 4 : 3;
                ring[vertex_index % length] = vertex;
                if (vertex_index % length == length - 1) {
                    if (!append_triangle(decoder, shape, joint, &ring[0], &ring[1],
                                         &ring[2]))
                        return false;
                    if (length == 4 && !append_triangle(decoder, shape, joint, &ring[0],
                                                        &ring[2], &ring[3]))
                        return false;
                }
            } else if (primitive == 0x98) {
                if (vertex_index >= 2) {
                    /* The ring replacement already alternates the previous
                     * pair. Keeping its slot order preserves strip winding:
                     * (0,1,2), (2,1,3), (2,3,4), (4,3,5).
                     */
                    if (!append_triangle(decoder, shape, joint, &ring[0], &ring[1],
                                         &vertex))
                        return false;
                }
                ring[vertex_index % 2] = vertex;
            } else {
                if (!vertex_index)
                    ring[0] = vertex;
                else {
                    if (vertex_index >= 2 &&
                        !append_triangle(decoder, shape, joint, &ring[0], &ring[1],
                                         &vertex))
                        return false;
                    ring[1] = vertex;
                }
            }
        }
    }
    return true;
}

static bool decode_shapes(ModelDecoder *decoder) {
    ModelBlock block = decoder->blocks.shape;
    ModelBlock draw = decoder->blocks.draw;
    if (block.size < 44 || draw.size < 20)
        return false;
    unsigned count = cc_read_be16(block.bytes + 8);
    size_t entries = cc_read_be32(block.bytes + 12);
    size_t remap = cc_read_be32(block.bytes + 16);
    size_t descriptions = cc_read_be32(block.bytes + 24);
    size_t matrix_indices = cc_read_be32(block.bytes + 28);
    size_t display_lists = cc_read_be32(block.bytes + 32);
    size_t matrix_data = cc_read_be32(block.bytes + 36);
    size_t packets = cc_read_be32(block.bytes + 40);
    unsigned draw_count = cc_read_be16(draw.bytes + 8);
    size_t draw_flags = cc_read_be32(draw.bytes + 12);
    size_t draw_joints = cc_read_be32(draw.bytes + 16);
    if (!count || count > 256 || entries > block.size || descriptions > block.size ||
        matrix_indices > block.size || display_lists > block.size ||
        matrix_data > block.size || packets > block.size ||
        !span(block, remap, (size_t)count * 2) || !span(draw, draw_flags, draw_count) ||
        !span(draw, draw_joints, (size_t)draw_count * 2))
        return false;
    if (decoder->shape_count != count)
        return false;
    /* Native 0496c walks INF1 nodes, rather than numerical SHP1 indices.
     * Translucent models depend on that original submission order.
     */
    for (unsigned ordinal = 0; ordinal < decoder->shape_count; ++ordinal) {
        unsigned shape = decoder->shape_order[ordinal];
        if (shape >= count)
            return false;
        unsigned remapped = cc_read_be16(block.bytes + remap + shape * 2);
        size_t entry = entries + (size_t)remapped * 40;
        if (!span(block, entry, 40))
            return false;
        const uint8_t *shape_data = block.bytes + entry;
        unsigned packet_count = cc_read_be16(shape_data + 2);
        unsigned matrix_start = cc_read_be16(shape_data + 6);
        unsigned packet_start = cc_read_be16(shape_data + 8);
        VertexDescriptor descriptors[16];
        unsigned descriptor_count;
        if (packet_count > 4096 ||
            !read_descriptors(block, descriptions + cc_read_be16(shape_data + 4),
                              descriptors, &descriptor_count))
            return false;
        for (unsigned index = 0; index < packet_count; ++index) {
            size_t packet_offset = packets + (size_t)(packet_start + index) * 8;
            size_t matrix_offset = matrix_data + (size_t)(matrix_start + index) * 8;
            if (!span(block, packet_offset, 8) || !span(block, matrix_offset, 8))
                return false;
            unsigned matrix_count = cc_read_be16(block.bytes + matrix_offset + 2);
            size_t first_matrix = cc_read_be32(block.bytes + matrix_offset + 4);
            if (matrix_count != 1 || first_matrix > block.size / 2 ||
                !span(block, matrix_indices + first_matrix * 2, 2))
                return false;
            unsigned draw_index =
                cc_read_be16(block.bytes + matrix_indices + first_matrix * 2);
            if (draw_index >= draw_count || draw.bytes[draw_flags + draw_index])
                return false;
            unsigned joint = cc_read_be16(draw.bytes + draw_joints + draw_index * 2);
            if (joint >= decoder->model->joint_count)
                return false;
            size_t length = cc_read_be32(block.bytes + packet_offset);
            size_t relative_start = cc_read_be32(block.bytes + packet_offset + 4);
            if (relative_start > block.size - display_lists)
                return false;
            size_t start = display_lists + relative_start;
            if (!span(block, start, length))
                return false;
            ModelBlock packet = {block.bytes + start, length};
            if (!decode_packet(decoder, packet, descriptors, descriptor_count, shape,
                               joint))
                return false;
        }
    }
    return true;
}

static bool decode_textures(ModelDecoder *decoder) {
    ModelBlock block = decoder->blocks.texture;
    if (!block.bytes)
        return true;
    if (block.size < 20)
        return false;
    unsigned count = cc_read_be16(block.bytes + 8);
    size_t entries = cc_read_be32(block.bytes + 12);
    if (count > MODEL_MAX_TEXTURES || !span(block, entries, (size_t)count * 32))
        return false;
    if (!count)
        return true;
    decoder->model->textures = calloc(count, sizeof(GcIplImage));
    if (!decoder->model->textures)
        return false;
    decoder->model->texture_count = count;
    for (unsigned index = 0; index < count; ++index) {
        size_t offset = entries + index * 32;
        if (!gc_ipl_texture_decode(block.bytes + offset, block.size - offset,
                                   &decoder->model->textures[index]))
            return false;
    }
    return true;
}

static bool decode_materials(ModelDecoder *decoder) {
    ModelBlock block = decoder->blocks.material;
    if (!block.bytes)
        return true;
    if (block.size < 100)
        return false;
    unsigned count = cc_read_be16(block.bytes + 8);
    size_t entries = cc_read_be32(block.bytes + 12);
    size_t remap = cc_read_be32(block.bytes + 16);
    if (count > 256 || entries > block.size || !span(block, remap, (size_t)count * 2))
        return false;
    decoder->model->material_count = count;
    if (count) {
        decoder->model->materials = calloc(count, sizeof(GcIplMaterial));
        if (!decoder->model->materials)
            return false;
    }
    /* MAT1's first texture selector is a u16 table index at entry +0x38.
     * The texture-number table pointer is section +0x34. Independent checks
     * against boot_demo_mark and logotype resources are retained in the
     * recovery evidence; native TEV state is not approximated here.
     */
    size_t texture_numbers = cc_read_be32(block.bytes + 52);
    for (unsigned material = 0; material < count; ++material) {
        unsigned index = cc_read_be16(block.bytes + remap + material * 2);
        size_t entry = entries + (size_t)index * 152;
        if (!span(block, entry, 152))
            return false;
        const uint8_t *native = block.bytes + entry;
        GcIplMaterial *decoded = &decoder->model->materials[material];
        decoded->flags = native[0];
        /* USA BS2 0x8130593c relocates the MAT1 tables; 0x81306304
         * submits these exact colors, orders, and 20-byte TEV programs.
         */
        const unsigned byte_tables[4] = {24, 32, 40, 64};
        uint8_t *byte_fields[4] = {&decoded->cull_mode, &decoded->channel_count,
                                   &decoded->texture_generator_count,
                                   &decoded->stage_count};
        for (unsigned field = 0; field < 4; ++field) {
            size_t table = cc_read_be32(block.bytes + byte_tables[field]);
            unsigned selector = native[field + 1];
            size_t width = field == 0 ? 4 : 1;
            if (selector == 255) {
                *byte_fields[field] = 0;
            } else {
                size_t offset = table + (size_t)selector * width;
                if (!span(block, offset, width))
                    return false;
                *byte_fields[field] =
                    (uint8_t)(width == 4 ? cc_read_be32(block.bytes + offset)
                                         : block.bytes[offset]);
            }
        }
        if (decoded->stage_count > 8 || decoded->texture_generator_count > 8 ||
            decoded->channel_count > 2 || decoded->cull_mode > 3)
            return false;
        const unsigned table_fields[5] = {28, 36, 60, 56, 68};
        const unsigned entry_fields[5] = {8, 12, 104, 72, 112};
        const unsigned element_sizes[5] = {4, 8, 8, 4, 20};
        const unsigned element_counts[5] = {2, 4, 4, 8, 8};
        for (unsigned field = 0; field < 5; ++field) {
            size_t table = cc_read_be32(block.bytes + table_fields[field]);
            for (unsigned element = 0; element < element_counts[field]; ++element) {
                unsigned selector =
                    cc_read_be16(native + entry_fields[field] + element * 2);
                if (selector == 65535)
                    continue;
                size_t offset = table + (size_t)selector * element_sizes[field];
                if (!span(block, offset, element_sizes[field]))
                    return false;
                if (field == 0)
                    memcpy(decoded->color[element], block.bytes + offset, 4);
                else if (field == 1)
                    memcpy(decoded->channels[element], block.bytes + offset, 8);
                else if (field == 2) {
                    for (unsigned component = 0; component < 4; ++component)
                        decoded->registers[element][component] =
                            (int16_t)cc_read_be16(block.bytes + offset + component * 2);
                } else if (field == 3)
                    memcpy(decoded->orders[element], block.bytes + offset, 4);
                else
                    memcpy(decoded->stages[element], block.bytes + offset, 20);
            }
        }
        for (unsigned stage = 0; stage < 8; ++stage) {
            unsigned selector = cc_read_be16(native + 56 + stage * 2);
            decoded->textures[stage] = UINT16_MAX;
            if (selector != UINT16_MAX) {
                size_t offset = texture_numbers + (size_t)selector * 2;
                if (!span(block, offset, 2))
                    return false;
                decoded->textures[stage] = cc_read_be16(block.bytes + offset);
            }
            selector = cc_read_be16(native + 20 + stage * 2);
            if (selector != UINT16_MAX) {
                size_t offset = cc_read_be32(block.bytes + 44) + (size_t)selector * 4;
                if (!span(block, offset, 4))
                    return false;
                memcpy(decoded->texture_coordinates[stage], block.bytes + offset, 4);
            }
        }
        for (unsigned matrix = 0; matrix < 10; ++matrix) {
            unsigned selector = cc_read_be16(native + 36 + matrix * 2);
            if (selector == UINT16_MAX)
                continue;
            size_t offset = cc_read_be32(block.bytes + 48) + (size_t)selector * 52;
            if (!span(block, offset, 52))
                return false;
            decoded->texture_matrix_mask |= (uint16_t)(1u << matrix);
            decoded->texture_matrix_types[matrix] = block.bytes[offset + 48];
            for (unsigned component = 0; component < 12; ++component) {
                float value = cc_read_be_float(block.bytes + offset + component * 4);
                if (!isfinite(value))
                    return false;
                decoded->texture_matrices[matrix][component] = value;
            }
        }
        const unsigned state_tables[3] = {76, 80, 92};
        const unsigned state_sizes[3] = {8, 4, 4};
        uint8_t *state_fields[3] = {decoded->alpha_compare, decoded->blend,
                                    decoded->depth};
        for (unsigned state = 0; state < 3; ++state) {
            unsigned selector =
                state == 2 ? native[7] : cc_read_be16(native + 146 + state * 2);
            if (selector == (state == 2 ? 255u : 65535u))
                continue;
            size_t offset = cc_read_be32(block.bytes + state_tables[state]) +
                            (size_t)selector * state_sizes[state];
            if (!span(block, offset, state_sizes[state]))
                return false;
            memcpy(state_fields[state], block.bytes + offset, state_sizes[state]);
        }
        unsigned selector = cc_read_be16(block.bytes + entry + 56);
        unsigned texture = UINT_MAX;
        if (selector != 0xffff && texture_numbers <= block.size &&
            span(block, texture_numbers + (size_t)selector * 2, 2)) {
            texture = cc_read_be16(block.bytes + texture_numbers + selector * 2);
            if (texture >= decoder->model->texture_count)
                texture = UINT_MAX;
        }
        decoder->model->material_texture[material] = texture;
    }
    return true;
}

bool gc_ipl_model_decode(const uint8_t *data, size_t size, GcIplModel *model) {
    if (!model || size > 2 * 1024 * 1024 || model->triangles || model->joints ||
        model->textures || model->materials)
        return false;
    GcIplModel result = {0};
    ModelDecoder decoder = {0};
    decoder.model = &result;
    for (unsigned index = 0; index < 256; ++index) {
        decoder.shape_material[index] = UINT_MAX;
        decoder.shape_joint[index] = UINT_MAX;
        result.material_texture[index] = UINT_MAX;
    }
    bool okay = collect_blocks(data, size, &decoder.blocks) &&
                decode_joints(&decoder) && decode_hierarchy(&decoder) &&
                decode_arrays(&decoder) && decode_textures(&decoder) &&
                decode_materials(&decoder) && decode_shapes(&decoder);
    if (okay && decoder.triangle_cursor) {
        result.triangle_count = decoder.triangle_cursor;
        result.triangles = calloc(result.triangle_count, sizeof(GcIplTriangle));
        decoder.triangle_cursor = 0;
        okay = result.triangles && decode_shapes(&decoder) &&
               decoder.triangle_cursor == result.triangle_count;
    } else {
        okay = false;
    }
    if (!okay) {
        gc_ipl_model_destroy(&result);
        return false;
    }
    *model = result;
    return true;
}

static bool matching_model(const uint8_t *bytes, size_t size,
                           const char *native_joint_name, GcIplModel *model) {
    ModelBlocks blocks = {0};
    if (!collect_blocks(bytes, size, &blocks) || blocks.joint.size < 24)
        return false;
    unsigned count = cc_read_be16(blocks.joint.bytes + 8);
    size_t names = cc_read_be32(blocks.joint.bytes + 20);
    for (unsigned index = 0; index < count && index < MODEL_MAX_JOINTS; ++index) {
        char name[64];
        if (read_native_name(blocks.joint, names, index, name) &&
            !strcmp(name, native_joint_name))
            return gc_ipl_model_decode(bytes, size, model);
    }
    return false;
}

bool gc_ipl_model_load(const char *ipl_path, const char *native_joint_name,
                       GcIplModel *model) {
    if (!ipl_path || !native_joint_name || !model || model->triangles ||
        model->joints || model->textures || model->materials)
        return false;
    uint8_t *rom = NULL;
    if (!gc_ipl_rom_read(ipl_path, &rom))
        return false;
    if (cc_read_be32(rom + GC_IPL_SCRAMBLED_START) != UINT32_C(0x3c800011))
        gc_ipl_descramble(rom, GC_IPL_ROM_SIZE);
    bool okay = false;
    for (size_t offset = GC_IPL_BS2_OFFSET; offset < GC_IPL_SCRAMBLED_END - 16;
         ++offset) {
        size_t size;
        /* Native USA 0x8132e078 uses uncompressed resource 0x49 as the
         * reusable three-dimensional glyph piece. Other models are Yay0.
         */
        if (!memcmp(rom + offset, "J3D1bmd1", 8)) {
            size = cc_read_be32(rom + offset + 8);
            if (size >= 32 && size <= GC_IPL_SCRAMBLED_END - offset &&
                matching_model(rom + offset, size, native_joint_name, model)) {
                okay = true;
                break;
            }
        }
        if (!gc_ipl_yay0_size(rom + offset, GC_IPL_SCRAMBLED_END - offset, &size) ||
            size < 32 || size > 2 * 1024 * 1024)
            continue;
        uint8_t *decoded = malloc(size);
        if (!decoded)
            break;
        size_t consumed = 0;
        bool valid = gc_ipl_yay0_decode(rom + offset, GC_IPL_SCRAMBLED_END - offset,
                                        decoded, size, NULL, &consumed);
        if (valid)
            okay = matching_model(decoded, size, native_joint_name, model);
        free(decoded);
        if (okay)
            break;
        if (valid && consumed)
            offset += consumed - 1;
    }
    free(rom);
    return okay;
}

void gc_ipl_model_destroy(GcIplModel *model) {
    if (!model)
        return;
    for (size_t index = 0; index < model->texture_count; ++index)
        gc_ipl_image_destroy(&model->textures[index]);
    free(model->textures);
    free(model->joints);
    free(model->triangles);
    free(model->materials);
    memset(model, 0, sizeof(*model));
}

static void rotate_vector(const float rotation[3], float vector[3]) {
    for (unsigned axis = 0; axis < 3; ++axis) {
        unsigned a = (axis + 1) % 3;
        unsigned b = (axis + 2) % 3;
        int angle = (int)lrintf(rotation[axis] * (32768 / 3.14159265358979323846f));
        float cosine = gc_angle_cosine(angle);
        float sine = gc_angle_sine(angle);
        float first = vector[a];
        vector[a] = cosine * first - sine * vector[b];
        vector[b] = sine * first + cosine * vector[b];
    }
}

bool gc_ipl_model_transform_vertex_native(const GcIplModel *model, unsigned joint_index,
                                          const GcIplVertex *vertex,
                                          GcIplVertex *output) {
    if (!model || !vertex || !output || joint_index >= model->joint_count)
        return false;
    *output = *vertex;
    int index = (int)joint_index;
    for (size_t depth = 0; index >= 0; ++depth) {
        if (depth >= model->joint_count || (unsigned)index >= model->joint_count)
            return false;
        const GcIplJoint *joint = &model->joints[index];
        for (unsigned component = 0; component < 3; ++component) {
            if (!isfinite(joint->scale[component]) || joint->scale[component] == 0)
                return false;
            output->position[component] *= joint->scale[component];
            output->normal[component] /= joint->scale[component];
        }
        rotate_vector(joint->rotation, output->position);
        rotate_vector(joint->rotation, output->normal);
        for (unsigned component = 0; component < 3; ++component)
            output->position[component] += joint->translation[component];
        index = joint->parent;
    }
    return true;
}

bool gc_ipl_model_transform_vertex(const GcIplModel *model, unsigned joint_index,
                                   const GcIplVertex *vertex, GcIplVertex *output) {
    if (!gc_ipl_model_transform_vertex_native(model, joint_index, vertex, output))
        return false;
    float length = sqrtf(output->normal[0] * output->normal[0] +
                         output->normal[1] * output->normal[1] +
                         output->normal[2] * output->normal[2]);
    if (length > 0)
        for (unsigned component = 0; component < 3; ++component)
            output->normal[component] /= length;
    return true;
}

bool gc_ipl_model_transform_direction(const GcIplModel *model, unsigned joint_index,
                                      const float normal[3], float output[3]) {
    if (!model || !normal || !output || joint_index >= model->joint_count)
        return false;
    float result[3];
    memcpy(result, normal, sizeof(result));
    for (unsigned component = 0; component < 3; ++component)
        if (!isfinite(result[component]))
            return false;
    int index = (int)joint_index;
    for (size_t depth = 0; index >= 0; ++depth) {
        if (depth >= model->joint_count || (unsigned)index >= model->joint_count)
            return false;
        const GcIplJoint *joint = &model->joints[index];
        for (unsigned component = 0; component < 3; ++component) {
            if (!isfinite(joint->scale[component]) ||
                !isfinite(joint->rotation[component]))
                return false;
            result[component] *= joint->scale[component];
        }
        rotate_vector(joint->rotation, result);
        index = joint->parent;
    }
    for (unsigned component = 0; component < 3; ++component)
        if (!isfinite(result[component]))
            return false;
    memcpy(output, result, sizeof(result));
    return true;
}

bool gc_ipl_model_transform_normal(const float matrix[12], const float normal[3],
                                   float output[3]) {
    if (!matrix || !normal || !output)
        return false;
    for (unsigned i = 0; i < 12; ++i)
        if (!isfinite(matrix[i]))
            return false;
    for (unsigned i = 0; i < 3; ++i)
        if (!isfinite(normal[i]))
            return false;
    /* USA 07938 computes the inverse-transpose; 0496c / 04c10 load this
     * into the separate GX lighting normal matrix. The normal is not
     * normalized here; NORMAL texgen uses the ordinary position matrix.
     */
    float cofactors[9] = {matrix[5] * matrix[10] - matrix[6] * matrix[9],
                          matrix[6] * matrix[8] - matrix[4] * matrix[10],
                          matrix[4] * matrix[9] - matrix[5] * matrix[8],
                          matrix[2] * matrix[9] - matrix[1] * matrix[10],
                          matrix[0] * matrix[10] - matrix[2] * matrix[8],
                          matrix[1] * matrix[8] - matrix[0] * matrix[9],
                          matrix[1] * matrix[6] - matrix[2] * matrix[5],
                          matrix[2] * matrix[4] - matrix[0] * matrix[6],
                          matrix[0] * matrix[5] - matrix[1] * matrix[4]};
    float determinant =
        matrix[0] * cofactors[0] + matrix[1] * cofactors[1] + matrix[2] * cofactors[2];
    if (!isfinite(determinant) || determinant == 0)
        return false;
    float result[3];
    for (unsigned row = 0; row < 3; ++row) {
        result[row] =
            (cofactors[row * 3] * normal[0] + cofactors[row * 3 + 1] * normal[1] +
             cofactors[row * 3 + 2] * normal[2]) /
            determinant;
        if (!isfinite(result[row]))
            return false;
    }
    memcpy(output, result, sizeof(result));
    return true;
}
