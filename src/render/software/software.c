#include "software.h"
#include "software_material.h"
#include "console_common/internal/render/geometry.h"
#include "console_common/internal/render/material.h"
#include "console_common/internal/render/material_blend.h"
#include "console_common/internal/render/material_depth.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { GC_SOFTWARE_TEXTURES = 2304 };

struct CcPlatform {
    uint8_t pixels[CC_FRAME_WIDTH * CC_FRAME_HEIGHT * 4];
    float depth[CC_FRAME_WIDTH * CC_FRAME_HEIGHT];
    GcSoftwareTexture textures[GC_SOFTWARE_TEXTURES];
    CcClipRect clip;
    float fade_alpha;
};

uint64_t gc_software_frame_hash(const CcPlatform *platform) {
    if (!platform)
        return 0;
    uint64_t hash = UINT64_C(14695981039346656037);
    for (size_t index = 0; index < sizeof(platform->pixels); ++index) {
        hash ^= platform->pixels[index];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

bool gc_software_read_pixel(const CcPlatform *platform, unsigned x, unsigned y,
                            uint8_t rgba[4]) {
    if (!platform || !rgba || x >= CC_FRAME_WIDTH || y >= CC_FRAME_HEIGHT)
        return false;
    memcpy(rgba, platform->pixels + ((size_t)y * CC_FRAME_WIDTH + x) * 4, 4);
    return true;
}

static uint8_t channel(float value) {
    if (value <= 0.0f)
        return 0;
    if (value >= 1.0f)
        return 255;
    return (uint8_t)lroundf(value * 255.0f);
}

CcPlatform *cc_platform_create(const char *title, int width, int height) {
    (void)title;
    (void)width;
    (void)height;
    return calloc(1, sizeof(CcPlatform));
}

void cc_platform_destroy(CcPlatform *platform) {
    if (!platform)
        return;
    for (size_t index = 0; index < GC_SOFTWARE_TEXTURES; index++)
        free(platform->textures[index].pixels);
    free(platform);
}

bool cc_platform_poll(CcPlatform *platform, CcEvent *event) {
    (void)platform;
    if (event)
        *event = (CcEvent){0};
    return false;
}

bool cc_platform_is_fullscreen(CcPlatform *platform) {
    (void)platform;
    return false;
}

bool cc_platform_set_fullscreen(CcPlatform *platform, bool fullscreen) {
    return platform != NULL && !fullscreen;
}

void cc_platform_begin(CcPlatform *platform, CcColor clear) {
    if (!platform)
        return;
    for (size_t pixel = 0; pixel < sizeof(platform->pixels); pixel += 4) {
        platform->pixels[pixel] = channel(clear.r);
        platform->pixels[pixel + 1] = channel(clear.g);
        platform->pixels[pixel + 2] = channel(clear.b);
        platform->pixels[pixel + 3] = channel(clear.a);
    }
    platform->clip = (CcClipRect){0, 0, CC_FRAME_WIDTH, CC_FRAME_HEIGHT};
    for (size_t index = 0; index < CC_FRAME_WIDTH * CC_FRAME_HEIGHT; ++index)
        platform->depth[index] = 1;
    platform->fade_alpha = 0;
}

void cc_platform_set_clip(CcPlatform *platform, const CcClipRect *clip) {
    if (platform)
        platform->clip =
            clip ? *clip : (CcClipRect){0, 0, CC_FRAME_WIDTH, CC_FRAME_HEIGHT};
}

static float edge(CcDrawVertex a, CcDrawVertex b, float x, float y) {
    /* Evaluate shared edges from the same endpoint. Independent origins can
     * round both triangle tests below zero and leave a diagonal raster crack. */
    bool reversed = a.x > b.x || (a.x == b.x && a.y > b.y);
    if (reversed) {
        CcDrawVertex swap = a;
        a = b;
        b = swap;
    }
    float value = (b.x - a.x) * (y - a.y) - (b.y - a.y) * (x - a.x);
    return reversed ? -value : value;
}

static bool inclusive_edge(CcDrawVertex a, CcDrawVertex b) {
    return b.y < a.y || (b.y == a.y && b.x > a.x);
}

static bool covered(float value, CcDrawVertex a, CcDrawVertex b) {
    return value > 0.0f || (value == 0.0f && inclusive_edge(a, b));
}

static bool compare_depth(unsigned kind, float value, float stored) {
    switch (kind) {
        case 0:
            return false;
        case 1:
            return value < stored;
        case 2:
            return value == stored;
        case 3:
            return value <= stored;
        case 4:
            return value > stored;
        case 5:
            return value != stored;
        case 6:
            return value >= stored;
        default:
            return true;
    }
}

static float blend_factor(unsigned kind, unsigned component, const CcColor *source,
                          const float destination[4]) {
    switch (kind) {
        case 0:
            return 0;
        case 1:
            return 1;
        case 2:
            return destination[component];
        case 3:
            return 1 - destination[component];
        case 4:
            return source->a;
        case 5:
            return 1 - source->a;
        case 6:
            return destination[3];
        default:
            return 1 - destination[3];
    }
}

static void blend_pixel(uint8_t pixel[4], CcColor color, const CcMaterialBlend *blend) {
    float source[4] = {color.r, color.g, color.b, color.a};
    float destination[4];
    for (unsigned component = 0; component < 4; ++component)
        destination[component] = (float)pixel[component] / 255.0f;
    for (unsigned component = 0; component < 3; ++component) {
        float value = source[component];
        if (blend->enabled)
            value =
                value * blend_factor(blend->source, component, &color, destination) +
                destination[component] *
                    blend_factor(blend->destination, component, &color, destination);
        pixel[component] = channel(value);
    }
    pixel[3] =
        channel(blend->enabled ? color.a + destination[3] * (1 - color.a) : color.a);
}

static void draw_triangle(CcPlatform *platform, CcDrawVertex a, CcDrawVertex b,
                          CcDrawVertex c, uint32_t texture,
                          const CcMaterialQuad *material, unsigned ia, unsigned ib,
                          unsigned ic, const CcMaterialBlend *blend, bool tev) {
    if (!isfinite(platform->clip.x) || !isfinite(platform->clip.y) ||
        !isfinite(platform->clip.width) || !isfinite(platform->clip.height) ||
        platform->clip.width <= 0 || platform->clip.height <= 0)
        return;
    float area = edge(a, b, c.x, c.y);
    if (!isfinite(area) || fabsf(area) < 0.0001f)
        return;
    if (area < 0) {
        CcDrawVertex swap = b;
        b = c;
        c = swap;
        unsigned index = ib;
        ib = ic;
        ic = index;
        area = -area;
    }
    float left = fmaxf(0, fmaxf(platform->clip.x, fminf(a.x, fminf(b.x, c.x))));
    float top = fmaxf(0, fmaxf(platform->clip.y, fminf(a.y, fminf(b.y, c.y))));
    float right = fminf(CC_FRAME_WIDTH, fminf(platform->clip.x + platform->clip.width,
                                              fmaxf(a.x, fmaxf(b.x, c.x))));
    float bottom =
        fminf(CC_FRAME_HEIGHT, fminf(platform->clip.y + platform->clip.height,
                                     fmaxf(a.y, fmaxf(b.y, c.y))));
    /* Intersect before converting bounds to integers: a finite offscreen
     * coordinate can still exceed the integer range. */
    if (left >= right || top >= bottom)
        return;
    const GcSoftwareTexture *image =
        texture < GC_SOFTWARE_TEXTURES ? &platform->textures[texture] : NULL;
    for (int y = (int)floorf(top); y < (int)ceilf(bottom); y++) {
        for (int x = (int)floorf(left); x < (int)ceilf(right); x++) {
            float px = (float)x + 0.5f, py = (float)y + 0.5f;
            float ea = edge(b, c, px, py);
            float eb = edge(c, a, px, py);
            float ec = edge(a, b, px, py);
            if (!covered(ea, b, c) || !covered(eb, c, a) || !covered(ec, a, b))
                continue;
            float wa = ea / area, wb = eb / area, wc = ec / area;
            size_t pixel_index = (size_t)y * CC_FRAME_WIDTH + (size_t)x;
            float depth = 0;
            bool depth_enabled =
                material && material->has_depth_mode && material->depth_mode[0];
            if (material) {
                const CcMaterialVertex *va = &material->vertices[ia];
                const CcMaterialVertex *vb = &material->vertices[ib];
                const CcMaterialVertex *vc = &material->vertices[ic];
                /* GPU depth is linear in window coordinates. Varying colors
                 * and UVs divide their barycentric weights by projection W.
                 * Difference form keeps a flat plane's depth exact; summing
                 * weighted constants can spuriously fail coplanar LEQUAL. */
                depth = va->depth + (vb->depth - va->depth) * wb +
                        (vc->depth - va->depth) * wc;
                if (depth < 0 || depth > 1 ||
                    (depth_enabled && !compare_depth(material->depth_mode[1], depth,
                                                     platform->depth[pixel_index])))
                    continue;
                wa /= cc_material_clip_w(va);
                wb /= cc_material_clip_w(vb);
                wc /= cc_material_clip_w(vc);
                float sum = wa + wb + wc;
                if (!isfinite(sum) || sum <= 0)
                    continue;
                wa /= sum;
                wb /= sum;
                wc /= sum;
            }
            CcColor color = {a.color.r * wa + b.color.r * wb + c.color.r * wc,
                             a.color.g * wa + b.color.g * wb + c.color.g * wc,
                             a.color.b * wa + b.color.b * wb + c.color.b * wc,
                             a.color.a * wa + b.color.a * wb + c.color.a * wc};
            if (material) {
                unsigned indices[3] = {ia, ib, ic};
                float weights[3] = {wa, wb, wc};
                if (!gc_software_material_color(platform->textures,
                                                GC_SOFTWARE_TEXTURES, material, indices,
                                                weights, tev, &color))
                    continue;
            } else if (image && image->pixels) {
                float u = a.u * wa + b.u * wb + c.u * wc;
                float v = a.v * wa + b.v * wb + c.v * wc;
                float sampled[4];
                gc_software_sample_texture(image, u, v, 0, 0, false, sampled);
                color.r *= sampled[0];
                color.g *= sampled[1];
                color.b *= sampled[2];
                color.a *= sampled[3];
            }
            if (depth_enabled && material->depth_mode[2])
                platform->depth[pixel_index] = depth;
            size_t offset = pixel_index * 4;
            uint8_t *pixel = &platform->pixels[offset];
            blend_pixel(pixel, color, blend);
        }
    }
}

void cc_platform_draw_vertices(CcPlatform *platform, const CcDrawVertex vertices[4],
                               uint32_t texture) {
    if (!platform || !vertices)
        return;
    CcMaterialBlend blend = {.enabled = true, .source = 4, .destination = 5};
    draw_triangle(platform, vertices[0], vertices[1], vertices[3], texture, NULL, 0, 1,
                  3, &blend, false);
    draw_triangle(platform, vertices[0], vertices[3], vertices[2], texture, NULL, 0, 3,
                  2, &blend, false);
}

void cc_platform_prepare_material(CcPlatform *platform,
                                  const CcMaterialQuad *material) {
    (void)platform;
    (void)material;
}

void cc_platform_draw_material_quad(CcPlatform *platform,
                                    const CcMaterialQuad *material) {
    CcMaterialBlend blend;
    unsigned depth_key;
    if (!platform || !material || material->texture_count > CC_MATERIAL_TEXTURES ||
        !cc_material_blend_resolve(material, &blend) ||
        !cc_material_depth_key(material, &depth_key))
        return;
    bool tev = cc_material_tev_support(material, true) == CC_TEV_SUPPORTED;
    CcDrawVertex vertices[4];
    for (unsigned index = 0; index < 4; ++index) {
        const CcMaterialVertex *vertex = &material->vertices[index];
        vertices[index] = (CcDrawVertex){vertex->x, vertex->y, 0, 0, vertex->color};
    }
    draw_triangle(platform, vertices[0], vertices[1], vertices[3], 0, material, 0, 1, 3,
                  &blend, tev);
    draw_triangle(platform, vertices[0], vertices[3], vertices[2], 0, material, 0, 3, 2,
                  &blend, tev);
}

void cc_platform_draw_quad(CcPlatform *platform, const CcQuad *quad) {
    if (!cc_render_quad_has_area(quad))
        return;
    CcDrawVertex vertices[CC_QUAD_CORNERS];
    cc_render_quad_corners(quad, vertices);
    cc_platform_draw_vertices(platform, vertices, quad->texture);
}

void cc_platform_set_fade_alpha(CcPlatform *platform, float alpha) {
    if (platform)
        platform->fade_alpha = fmaxf(0, fminf(1, alpha));
}

void cc_platform_end(CcPlatform *platform) {
    if (!platform || platform->fade_alpha <= 0)
        return;
    CcQuad fade = {.width = CC_FRAME_WIDTH,
                   .height = CC_FRAME_HEIGHT,
                   .color = {0, 0, 0, platform->fade_alpha}};
    cc_platform_draw_quad(platform, &fade);
}

uint32_t cc_platform_create_texture(CcPlatform *platform, int width, int height,
                                    const uint8_t *rgba) {
    if (!platform || !rgba || width <= 0 || height <= 0 ||
        (size_t)width > SIZE_MAX / 4 / (size_t)height)
        return 0;
    for (uint32_t slot = 1; slot < GC_SOFTWARE_TEXTURES; slot++) {
        if (platform->textures[slot].pixels)
            continue;
        size_t bytes = (size_t)width * (size_t)height * 4;
        uint8_t *pixels = malloc(bytes);
        if (!pixels)
            return 0;
        memcpy(pixels, rgba, bytes);
        platform->textures[slot] = (GcSoftwareTexture){width, height, pixels};
        return slot;
    }
    return 0;
}

void cc_platform_destroy_texture(CcPlatform *platform, uint32_t texture) {
    if (!platform || texture >= GC_SOFTWARE_TEXTURES)
        return;
    free(platform->textures[texture].pixels);
    platform->textures[texture] = (GcSoftwareTexture){0};
}

bool gc_software_write_stream(CcPlatform *platform, FILE *stream) {
    if (!platform || !stream)
        return false;
    if (fprintf(stream, "P6\n%d %d\n255\n", CC_FRAME_WIDTH, CC_FRAME_HEIGHT) <= 0)
        return false;
    uint8_t row[CC_FRAME_WIDTH * 3];
    for (size_t y = 0; y < CC_FRAME_HEIGHT; ++y) {
        for (size_t x = 0; x < CC_FRAME_WIDTH; ++x)
            memcpy(row + x * 3, platform->pixels + (y * CC_FRAME_WIDTH + x) * 4, 3);
        if (fwrite(row, 1, sizeof(row), stream) != sizeof(row))
            return false;
    }
    return !ferror(stream);
}

bool gc_software_write_frame(CcPlatform *platform, const char *path) {
    if (!platform || !path)
        return false;
    FILE *file = fopen(path, "wb");
    if (!file)
        return false;
    bool okay = gc_software_write_stream(platform, file);
    if (fclose(file) != 0)
        okay = false;
    return okay;
}
