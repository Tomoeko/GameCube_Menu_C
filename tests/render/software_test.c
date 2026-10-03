#include "render/software/software.h"

#include <assert.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static CcMaterialQuad material_quad(void) {
    CcMaterialQuad quad = {0};
    for (unsigned vertex = 0; vertex < 4; ++vertex) {
        quad.vertices[vertex].x = vertex & 1 ? 2 : 0;
        quad.vertices[vertex].y = vertex & 2 ? 2 : 0;
        quad.vertices[vertex].uv[0][0] = vertex & 1 ? 1 : 0;
        quad.vertices[vertex].uv[0][1] = vertex & 2 ? 1 : 0;
        quad.vertices[vertex].color = (CcColor){1, 1, 1, 1};
    }
    memset(quad.tev_swap_table, 0xe4, sizeof(quad.tev_swap_table));
    quad.tev_stage_count = 1;
    quad.tev_stages[0][4] = 0x8f;
    quad.tev_stages[0][5] = 0xfa;
    quad.tev_stages[0][7] = 1;
    quad.tev_stages[0][8] = 0x47;
    quad.tev_stages[0][9] = 0x75;
    quad.tev_stages[0][11] = 1;
    quad.has_blend_mode = true;
    return quad;
}

static void pixel(CcPlatform *platform, const char *path, unsigned x, unsigned y,
                  uint8_t output[3]) {
    assert(gc_software_write_frame(platform, path));
    FILE *file = fopen(path, "rb");
    assert(file);
    char magic[3];
    unsigned width, height, range;
    assert(fscanf(file, "%2s %u %u %u", magic, &width, &height, &range) == 4);
    assert(!strcmp(magic, "P6") && range == 255 && x < width && y < height);
    assert(fgetc(file) == '\n');
    long offset = (long)((y * width + x) * 3);
    assert(fseek(file, offset, SEEK_CUR) == 0);
    assert(fread(output, 1, 3, file) == 3);
    assert(fclose(file) == 0);
}

static void set_material_color(CcMaterialQuad *quad, CcColor color) {
    for (unsigned index = 0; index < 4; ++index)
        quad->vertices[index].color = color;
}

static void set_material_depth(CcMaterialQuad *quad, float depth) {
    for (unsigned index = 0; index < 4; ++index)
        quad->vertices[index].depth = depth;
    quad->has_depth_mode = true;
    quad->depth_mode[0] = 1;
    quad->depth_mode[1] = 3;
    quad->depth_mode[2] = 1;
}

static void test_color_blend_factors(CcPlatform *platform) {
    CcMaterialQuad quad = material_quad();
    quad.tev_stage_count = 0;
    for (unsigned component = 0; component < 4; ++component)
        quad.registers[1][component] = 1;
    set_material_color(&quad, (CcColor){0.8f, 0.3f, 0.6f, 1});
    quad.blend_mode[0] = 1;
    const uint8_t factors[][2] = {{0, 2}, {0, 3}, {2, 0}, {3, 0}};
    const uint8_t expected[][3] = {
        {41, 46, 31}, {10, 107, 20}, {41, 46, 31}, {163, 31, 122}};
    for (unsigned index = 0; index < 4; ++index) {
        quad.blend_mode[1] = factors[index][0];
        quad.blend_mode[2] = factors[index][1];
        cc_platform_begin(platform, (CcColor){0.2f, 0.6f, 0.2f, 1});
        cc_platform_draw_material_quad(platform, &quad);
        uint8_t rgba[4];
        assert(gc_software_read_pixel(platform, 0, 0, rgba));
        for (unsigned component = 0; component < 3; ++component)
            assert(abs((int)rgba[component] - (int)expected[index][component]) <= 1);
    }
}

static void test_coplanar_depth(CcPlatform *platform) {
    CcMaterialQuad first = material_quad(), second = material_quad();
    set_material_color(&first, (CcColor){1, 0, 0, 1});
    set_material_color(&second, (CcColor){0, 0, 1, 1});
    set_material_depth(&first, 0.47f);
    set_material_depth(&second, 0.47f);
    for (unsigned vertex = 0; vertex < 4; ++vertex) {
        first.vertices[vertex].x = vertex & 1 ? 13.29f : 0.13f;
        first.vertices[vertex].y = vertex & 2 ? 11.41f : 0.37f;
        second.vertices[vertex].x = vertex & 1 ? 11.31f : 3.2f;
        second.vertices[vertex].y = vertex & 2 ? 9.9f : 2.1f;
    }
    /* Distinct triangulations of one flat plane must have identical depth.
     * Equality catches roundoff that otherwise stipples coplanar card icons. */
    const uint8_t comparisons[] = {2, 3, 6, 5};
    for (unsigned comparison = 0; comparison < 4; ++comparison) {
        second.depth_mode[1] = comparisons[comparison];
        cc_platform_begin(platform, (CcColor){0, 0, 0, 1});
        cc_platform_draw_material_quad(platform, &first);
        cc_platform_draw_material_quad(platform, &second);
        for (unsigned y = 3; y < 10; ++y) {
            for (unsigned x = 4; x < 11; ++x) {
                uint8_t rgba[4];
                assert(gc_software_read_pixel(platform, x, y, rgba));
                bool passes = comparisons[comparison] != 5;
                assert(rgba[passes ? 2 : 0] == 255);
                assert(rgba[passes ? 0 : 2] == 0 && !rgba[1] && rgba[3] == 255);
            }
        }
    }
}

static void test_fractional_shared_edge(CcPlatform *platform) {
    const uint8_t white[4] = {255, 255, 255, 255};
    uint32_t texture = cc_platform_create_texture(platform, 1, 1, white);
    assert(texture);
    CcMaterialQuad quad = material_quad();
    quad.texture_count = 1;
    quad.textures[0] = texture;
    quad.blend_mode[0] = 1;
    quad.blend_mode[1] = 4;
    quad.blend_mode[2] = 5;
    /* This card icon's shared diagonal crosses pixel centers exactly. The
     * two triangles must cover every interior pixel once, in either winding. */
    const float positions[4][2] = {
        {261.993713f, 308.006287f},
        {261.993713f, 271.993713f},
        {298.006287f, 308.006287f},
        {298.006287f, 271.993713f},
    };
    for (unsigned index = 0; index < 4; ++index) {
        quad.vertices[index].x = positions[index][0];
        quad.vertices[index].y = positions[index][1];
        quad.vertices[index].color = (CcColor){1, 0, 0, 0.5f};
    }
    for (unsigned winding = 0; winding < 2; ++winding) {
        cc_platform_begin(platform, (CcColor){0, 0, 1, 1});
        cc_platform_draw_material_quad(platform, &quad);
        for (unsigned y = 272; y < 308; ++y) {
            for (unsigned x = 262; x < 298; ++x) {
                uint8_t rgba[4];
                assert(gc_software_read_pixel(platform, x, y, rgba));
                assert(rgba[0] >= 127 && rgba[0] <= 128 && !rgba[1]);
                assert(rgba[2] >= 127 && rgba[2] <= 128);
            }
        }
        CcMaterialVertex swap = quad.vertices[1];
        quad.vertices[1] = quad.vertices[2];
        quad.vertices[2] = swap;
    }
    cc_platform_destroy_texture(platform, texture);
}

static void test_material_sampling(CcPlatform *platform) {
    const uint8_t gradient[8] = {0, 0, 0, 255, 255, 255, 255, 255};
    uint32_t texture = cc_platform_create_texture(platform, 2, 1, gradient);
    assert(texture);
    CcMaterialQuad quad = material_quad();
    quad.texture_count = 1;
    quad.textures[0] = texture;
    for (unsigned vertex = 0; vertex < 4; ++vertex) {
        for (unsigned slot = 0; slot < 2; ++slot) {
            quad.vertices[vertex].uv[slot][0] = 0.4f;
            quad.vertices[vertex].uv[slot][1] = 0.5f;
        }
    }
    for (unsigned stage_count = 0; stage_count <= 1; ++stage_count) {
        quad.tev_stage_count = (uint8_t)stage_count;
        for (unsigned component = 0; component < 4; ++component)
            quad.registers[1][component] = 1;
        for (unsigned pass = 0; pass < 4; ++pass) {
            quad.nearest[0] = !(pass & 1);
            cc_platform_begin(platform, (CcColor){0, 0, 0, 1});
            cc_platform_draw_material_quad(platform, &quad);
            uint8_t rgba[4];
            assert(gc_software_read_pixel(platform, 0, 0, rgba));
            unsigned expected = quad.nearest[0] ? 0 : 77;
            for (unsigned component = 0; component < 3; ++component)
                assert(rgba[component] >= (expected ? expected - 1 : 0) &&
                       rgba[component] <= expected);
        }
    }
    /* Aliased handles still have independent sampler choices in each slot. */
    quad.tev_stage_count = 0;
    quad.texture_count = 2;
    quad.textures[1] = texture;
    quad.nearest[0] = true;
    quad.nearest[1] = false;
    quad.konst_colors[3][3] = 0.5f;
    cc_platform_begin(platform, (CcColor){0, 0, 0, 1});
    cc_platform_draw_material_quad(platform, &quad);
    uint8_t rgba[4];
    assert(gc_software_read_pixel(platform, 0, 0, rgba));
    assert(rgba[0] == 38 && rgba[1] == 38 && rgba[2] == 38);

    quad.texture_count = 1;
    const float coordinates[] = {1.0f, 1.4f, 1.6f, -0.6f};
    const uint8_t modes[] = {0, 1, 2, 1};
    const uint8_t expected[] = {255, 0, 0, 0};
    for (unsigned sample = 0; sample < 4; ++sample) {
        quad.wrap_s[0] = modes[sample];
        for (unsigned vertex = 0; vertex < 4; ++vertex)
            quad.vertices[vertex].uv[0][0] = coordinates[sample];
        cc_platform_begin(platform, (CcColor){0, 0, 0, 1});
        cc_platform_draw_material_quad(platform, &quad);
        assert(gc_software_read_pixel(platform, 0, 0, rgba));
        assert(rgba[0] == expected[sample]);
    }
    CcQuad basic = {.width = 2,
                    .height = 2,
                    .u0 = 0.4f,
                    .u1 = 0.4f,
                    .v0 = 0.5f,
                    .v1 = 0.5f,
                    .color = {1, 1, 1, 1},
                    .texture = texture};
    cc_platform_begin(platform, (CcColor){0, 0, 0, 1});
    cc_platform_draw_quad(platform, &basic);
    assert(gc_software_read_pixel(platform, 0, 0, rgba));
    assert(rgba[0] >= 76 && rgba[0] <= 77);
    cc_platform_destroy_texture(platform, texture);
}

static void test_depth_overlap(CcPlatform *platform, const char *path) {
    CcMaterialQuad red = material_quad(), blue = material_quad();
    set_material_color(&red, (CcColor){1, 0, 0, 1});
    set_material_color(&blue, (CcColor){0, 0, 1, 1});
    set_material_depth(&red, 0);
    set_material_depth(&blue, 0);
    for (unsigned index = 0; index < 4; ++index) {
        red.vertices[index].x *= 2;
        red.vertices[index].y *= 2;
        blue.vertices[index].x *= 2;
        blue.vertices[index].y *= 2;
        red.vertices[index].depth = index & 1 ? 0.8f : 0.2f;
        blue.vertices[index].depth = index & 1 ? 0.2f : 0.8f;
    }
    uint8_t output[3];
    for (unsigned order = 0; order < 2; ++order) {
        cc_platform_begin(platform, (CcColor){0, 0, 0, 1});
        cc_platform_draw_material_quad(platform, order ? &blue : &red);
        cc_platform_draw_material_quad(platform, order ? &red : &blue);
        pixel(platform, path, 0, 0, output);
        assert(output[0] == 255 && !output[1] && !output[2]);
        pixel(platform, path, 3, 0, output);
        assert(!output[0] && !output[1] && output[2] == 255);
    }
}

static void test_depth_comparisons(CcPlatform *platform, const char *path) {
    static const bool expected[8][3] = {
        {false, false, false}, {true, false, false}, {false, true, false},
        {true, true, false},   {false, false, true}, {true, false, true},
        {false, true, true},   {true, true, true},
    };
    CcMaterialQuad first = material_quad(), second = material_quad();
    set_material_color(&first, (CcColor){1, 0, 0, 1});
    set_material_color(&second, (CcColor){0, 0, 1, 1});
    set_material_depth(&first, 0.5f);
    uint8_t output[3];
    for (unsigned comparison = 0; comparison < 8; ++comparison) {
        for (unsigned sample = 0; sample < 3; ++sample) {
            set_material_depth(&second, (float)(sample + 1) * 0.25f);
            second.depth_mode[1] = (uint8_t)comparison;
            cc_platform_begin(platform, (CcColor){0, 0, 0, 1});
            cc_platform_draw_material_quad(platform, &first);
            cc_platform_draw_material_quad(platform, &second);
            pixel(platform, path, 0, 0, output);
            assert(output[expected[comparison][sample] ? 2 : 0] == 255);
            assert(output[expected[comparison][sample] ? 0 : 2] == 0);
        }
    }
}

static void test_depth_writes(CcPlatform *platform, const char *path) {
    CcMaterialQuad red = material_quad(), green = material_quad(),
                   blue = material_quad();
    set_material_color(&red, (CcColor){1, 0, 0, 1});
    set_material_color(&green, (CcColor){0, 1, 0, 1});
    set_material_color(&blue, (CcColor){0, 0, 1, 1});
    set_material_depth(&red, 0.2f);
    set_material_depth(&green, 0.1f);
    set_material_depth(&blue, 0.15f);
    green.depth_mode[2] = 0;
    cc_platform_begin(platform, (CcColor){0, 0, 0, 1});
    cc_platform_draw_material_quad(platform, &red);
    cc_platform_draw_material_quad(platform, &green);
    cc_platform_draw_material_quad(platform, &blue);
    uint8_t output[3];
    pixel(platform, path, 0, 0, output);
    assert(!output[0] && !output[1] && output[2] == 255);

    green.depth_mode[0] = 0;
    green.depth_mode[2] = 1;
    cc_platform_begin(platform, (CcColor){0, 0, 0, 1});
    cc_platform_draw_material_quad(platform, &green);
    cc_platform_draw_material_quad(platform, &blue);
    pixel(platform, path, 0, 0, output);
    assert(output[2] == 255);

    red.has_alpha_compare = true;
    red.alpha_compare[0] = 0;
    cc_platform_begin(platform, (CcColor){0, 0, 0, 1});
    cc_platform_draw_material_quad(platform, &red);
    set_material_depth(&blue, 0.4f);
    cc_platform_draw_material_quad(platform, &blue);
    pixel(platform, path, 0, 0, output);
    assert(output[2] == 255);
}

static void test_perspective(CcPlatform *platform, const char *path) {
    CcMaterialQuad quad = material_quad();
    quad.vertices[0].x = quad.vertices[0].y = 0;
    quad.vertices[1].x = 4;
    quad.vertices[1].y = 0;
    quad.vertices[3].x = 0;
    quad.vertices[3].y = 4;
    quad.vertices[2] = quad.vertices[0];
    quad.vertices[0].color = (CcColor){1, 0, 0, 1};
    quad.vertices[1].color = (CcColor){0, 1, 0, 1};
    quad.vertices[3].color = (CcColor){0, 0, 1, 1};
    quad.vertices[0].clip_w = 1;
    quad.vertices[1].clip_w = 2;
    quad.vertices[3].clip_w = 4;
    cc_platform_begin(platform, (CcColor){0, 0, 0, 1});
    cc_platform_draw_material_quad(platform, &quad);
    uint8_t output[3];
    pixel(platform, path, 0, 0, output);
    assert(output[0] == 227 && output[1] == 19 && output[2] == 9);

    const uint8_t gradient[8] = {0, 0, 0, 255, 255, 255, 255, 255};
    uint32_t texture = cc_platform_create_texture(platform, 2, 1, gradient);
    assert(texture);
    quad.textures[0] = texture;
    quad.texture_count = 1;
    set_material_color(&quad, (CcColor){1, 1, 1, 1});
    for (unsigned index = 0; index < 4; ++index) {
        quad.vertices[index].uv[0][0] = index == 1 ? 0.75f : 0.25f;
        quad.vertices[index].uv[0][1] = 0.5f;
    }
    cc_platform_begin(platform, (CcColor){0, 0, 0, 1});
    cc_platform_draw_material_quad(platform, &quad);
    pixel(platform, path, 0, 0, output);
    assert(output[0] == 19 && output[1] == 19 && output[2] == 19);
    cc_platform_destroy_texture(platform, texture);
}

static void assert_pixel(CcPlatform *platform, unsigned x, unsigned y,
                         const uint8_t expected[4]) {
    uint8_t rgba[4];
    assert(gc_software_read_pixel(platform, x, y, rgba));
    assert(memcmp(rgba, expected, 4) == 0);
}

static void test_basic_linear_sampling(CcPlatform *platform) {
    const uint8_t pixels[8] = {255, 0, 0, 255, 0, 0, 255, 255};
    uint32_t texture = cc_platform_create_texture(platform, 2, 1, pixels);
    assert(texture);
    CcQuad quad = {.width = 4,
                   .height = 2,
                   .u1 = 1,
                   .v1 = 1,
                   .color = {1, 1, 1, 1},
                   .texture = texture};
    cc_platform_begin(platform, (CcColor){0, 0, 0, 1});
    cc_platform_draw_quad(platform, &quad);
    /* This quarter interpolation was also checked against an actual Metal
     * offscreen draw with the same texture, geometry and sampler. */
    assert_pixel(platform, 1, 0, (const uint8_t[4]){191, 0, 64, 255});
    assert_pixel(platform, 2, 0, (const uint8_t[4]){64, 0, 191, 255});

    quad.x = 4;
    quad.width = -4;
    cc_platform_begin(platform, (CcColor){0, 0, 0, 1});
    cc_platform_draw_quad(platform, &quad);
    assert_pixel(platform, 1, 0, (const uint8_t[4]){64, 0, 191, 255});
    cc_platform_destroy_texture(platform, texture);
}

static void test_clip_bounds(CcPlatform *platform) {
    const CcClipRect rejected[] = {
        {FLT_MAX, 0, 1, 1}, {0, FLT_MAX, 1, 1},  {INFINITY, 0, 1, 1},
        {0, NAN, 1, 1},     {0, 0, INFINITY, 1}, {0, 0, 1, NAN},
        {0, 0, -1, 2},      {0, 0, 2, -1},       {0, 0, 0, 2},
    };
    CcQuad quad = {.width = 4, .height = 2, .color = {1, 0, 0, 1}};
    cc_platform_begin(platform, (CcColor){0, 0, 1, 1});
    uint64_t before = gc_software_frame_hash(platform);
    for (size_t index = 0; index < sizeof(rejected) / sizeof(rejected[0]); ++index) {
        cc_platform_set_clip(platform, &rejected[index]);
        cc_platform_draw_quad(platform, &quad);
        assert(gc_software_frame_hash(platform) == before);
    }
    cc_platform_set_clip(platform, NULL);
    quad.x = 3e9f;
    quad.width = 4096;
    cc_platform_draw_quad(platform, &quad);
    assert(gc_software_frame_hash(platform) == before);

    CcClipRect visible = {1, 0, 1, 2};
    quad.x = 0;
    quad.width = 4;
    cc_platform_set_clip(platform, &visible);
    cc_platform_draw_quad(platform, &quad);
    assert_pixel(platform, 0, 0, (const uint8_t[4]){0, 0, 255, 255});
    assert_pixel(platform, 1, 0, (const uint8_t[4]){255, 0, 0, 255});
    assert_pixel(platform, 2, 0, (const uint8_t[4]){0, 0, 255, 255});
    cc_platform_set_clip(platform, NULL);
}

static void test_simple_material_fallback(CcPlatform *platform) {
    CcMaterialQuad quad = material_quad();
    quad.tev_stage_count = 0;
    const float color[4] = {0.25f, 0.5f, 0.75f, 1};
    memcpy(quad.registers[1], color, sizeof(color));
    cc_platform_begin(platform, (CcColor){0, 0, 0, 1});
    cc_platform_draw_material_quad(platform, &quad);
    assert_pixel(platform, 0, 0, (const uint8_t[4]){64, 128, 191, 255});

    /* Stage limits and malformed selectors choose the same fallback used by
     * both GPU backends, instead of an independent software interpretation. */
    quad.tev_stage_count = 7;
    cc_platform_begin(platform, (CcColor){0, 0, 0, 1});
    cc_platform_draw_material_quad(platform, &quad);
    assert_pixel(platform, 0, 0, (const uint8_t[4]){64, 128, 191, 255});
    quad.tev_stage_count = 1;
    quad.tev_stages[0][8] = 0x88;
    cc_platform_begin(platform, (CcColor){0, 0, 0, 1});
    cc_platform_draw_material_quad(platform, &quad);
    assert_pixel(platform, 0, 0, (const uint8_t[4]){64, 128, 191, 255});

    const uint8_t red_blue[8] = {255, 0, 0, 255, 0, 0, 255, 255};
    const uint8_t green[4] = {0, 255, 0, 255};
    quad.textures[0] = cc_platform_create_texture(platform, 2, 1, red_blue);
    quad.textures[1] = cc_platform_create_texture(platform, 1, 1, green);
    assert(quad.textures[0] && quad.textures[1]);
    quad.tev_stage_count = 0;
    quad.texture_count = 1;
    for (unsigned component = 0; component < 4; ++component)
        quad.registers[1][component] = 1;
    for (unsigned vertex = 0; vertex < 4; ++vertex) {
        quad.vertices[vertex].x = vertex & 1 ? 4 : 0;
        quad.vertices[vertex].y = vertex & 2 ? 2 : 0;
        quad.vertices[vertex].uv[1][0] = quad.vertices[vertex].uv[0][0];
        quad.vertices[vertex].uv[1][1] = quad.vertices[vertex].uv[0][1];
    }
    cc_platform_begin(platform, (CcColor){0, 0, 0, 1});
    cc_platform_draw_material_quad(platform, &quad);
    assert_pixel(platform, 1, 0, (const uint8_t[4]){191, 0, 64, 255});
    quad.texture_count = 3;
    quad.konst_colors[3][3] = 0.25f;
    cc_platform_begin(platform, (CcColor){0, 0, 0, 1});
    cc_platform_draw_material_quad(platform, &quad);
    assert_pixel(platform, 1, 0, (const uint8_t[4]){48, 191, 16, 255});
    quad.has_alpha_compare = true;
    quad.alpha_compare[0] = 0;
    cc_platform_begin(platform, (CcColor){0, 0, 1, 1});
    cc_platform_draw_material_quad(platform, &quad);
    assert_pixel(platform, 1, 0, (const uint8_t[4]){0, 0, 255, 255});
    cc_platform_destroy_texture(platform, quad.textures[0]);
    cc_platform_destroy_texture(platform, quad.textures[1]);
}

static void test_borrowed_output_stream(CcPlatform *platform, const char *path) {
    FILE *stream = fopen(path, "wb+");
    assert(stream);
    assert(!gc_software_write_stream(NULL, stream));
    assert(!gc_software_write_stream(platform, NULL));
    assert(gc_software_write_stream(platform, stream));
    assert(fflush(stream) == 0);
    char expected_header[64];
    int header_size = snprintf(expected_header, sizeof(expected_header),
                               "P6\n%d %d\n255\n", CC_FRAME_WIDTH, CC_FRAME_HEIGHT);
    assert(header_size > 0 && (size_t)header_size < sizeof(expected_header));
    assert(ftell(stream) == header_size + (long)CC_FRAME_WIDTH * CC_FRAME_HEIGHT * 3);
    assert(fseek(stream, 0, SEEK_SET) == 0);
    char header[64];
    assert(fread(header, 1, (size_t)header_size, stream) == (size_t)header_size);
    assert(memcmp(header, expected_header, (size_t)header_size) == 0);
    assert(fclose(stream) == 0);
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "Files/software-test.ppm";
    CcPlatform *platform = cc_platform_create("Material test", 640, 480);
    assert(platform);
    const uint8_t rgba[16] = {255, 0, 0,   255, 0,   255, 0,   255,
                              0,   0, 255, 255, 255, 255, 255, 255};
    uint32_t texture = cc_platform_create_texture(platform, 2, 2, rgba);
    assert(texture);
    CcMaterialQuad quad = material_quad();
    quad.texture_count = 1;
    quad.textures[0] = texture;
    cc_platform_begin(platform, (CcColor){0, 0, 0, 1});
    cc_platform_draw_material_quad(platform, &quad);
    uint8_t output[3];
    pixel(platform, path, 0, 0, output);
    assert(output[0] == 255 && !output[1] && !output[2]);
    uint8_t inspected[4];
    assert(gc_software_read_pixel(platform, 0, 0, inspected));
    assert(inspected[0] == 255 && !inspected[1] && !inspected[2] &&
           inspected[3] == 255);
    uint8_t saved[4];
    memcpy(saved, inspected, sizeof(saved));
    assert(!gc_software_read_pixel(NULL, 0, 0, inspected));
    assert(!gc_software_read_pixel(platform, CC_FRAME_WIDTH, 0, inspected));
    assert(!gc_software_read_pixel(platform, 0, CC_FRAME_HEIGHT, inspected));
    assert(!gc_software_read_pixel(platform, 0, 0, NULL));
    assert(!memcmp(saved, inspected, sizeof(saved)));
    pixel(platform, path, 1, 0, output);
    assert(!output[0] && output[1] == 255 && !output[2]);

    quad.tev_stages[0][7] = quad.tev_stages[0][11] = 3;
    quad.tev_stage_count = 2;
    quad.tev_stages[1][4] = 0xff;
    quad.tev_stages[1][5] = 0x2f;
    quad.tev_stages[1][7] = 1;
    quad.tev_stages[1][8] = 0x77;
    quad.tev_stages[1][9] = 0x17;
    quad.tev_stages[1][11] = 1;
    cc_platform_begin(platform, (CcColor){0, 0, 0, 1});
    cc_platform_draw_material_quad(platform, &quad);
    pixel(platform, path, 1, 1, output);
    assert(output[0] == 255 && output[1] == 255 && output[2] == 255);

    quad = material_quad();
    for (unsigned vertex = 0; vertex < 4; ++vertex)
        quad.vertices[vertex].color = (CcColor){1, 0, 0, 0.5f};
    quad.has_alpha_compare = true;
    quad.alpha_compare[0] = 0x74;
    quad.alpha_compare[2] = 200;
    cc_platform_begin(platform, (CcColor){0, 0, 1, 1});
    cc_platform_draw_material_quad(platform, &quad);
    pixel(platform, path, 0, 0, output);
    assert(!output[0] && !output[1] && output[2] == 255);

    quad.has_alpha_compare = false;
    quad.blend_mode[0] = 1;
    quad.blend_mode[1] = quad.blend_mode[2] = 1;
    for (unsigned vertex = 0; vertex < 4; ++vertex)
        quad.vertices[vertex].color = (CcColor){0.25f, 0, 0, 1};
    cc_platform_begin(platform, (CcColor){0, 0, 0.25f, 1});
    cc_platform_draw_material_quad(platform, &quad);
    pixel(platform, path, 0, 0, output);
    assert(output[0] == 64 && !output[1] && output[2] == 64);

    quad = material_quad();
    quad.texture_count = 1;
    quad.textures[0] = texture;
    quad.wrap_s[0] = 1;
    for (unsigned vertex = 0; vertex < 4; ++vertex)
        quad.vertices[vertex].uv[0][0] += 1;
    cc_platform_begin(platform, (CcColor){0, 0, 0, 1});
    cc_platform_draw_material_quad(platform, &quad);
    pixel(platform, path, 0, 0, output);
    assert(output[0] == 255 && !output[1] && !output[2]);
    test_color_blend_factors(platform);
    test_depth_overlap(platform, path);
    test_depth_comparisons(platform, path);
    test_depth_writes(platform, path);
    test_coplanar_depth(platform);
    test_fractional_shared_edge(platform);
    test_perspective(platform, path);
    test_basic_linear_sampling(platform);
    test_material_sampling(platform);
    test_clip_bounds(platform);
    test_simple_material_fallback(platform);
    test_borrowed_output_stream(platform, path);
    cc_platform_destroy(platform);
    assert(remove(path) == 0);
    puts("software material tests passed");
    return 0;
}
