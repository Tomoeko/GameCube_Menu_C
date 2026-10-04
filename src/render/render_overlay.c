#include "render_internal.h"
#include "gamecube/audio.h"

#include <math.h>

void gc_scene_volume_indicator(GcScene *scene, unsigned percent, float alpha) {
    if (!scene)
        return;
    scene->volume_indicator_percent =
        percent > GC_AUDIO_MENU_VOLUME_MAX ? GC_AUDIO_MENU_VOLUME_MAX : percent;
    scene->volume_indicator_alpha = isfinite(alpha) ? fminf(1, fmaxf(0, alpha)) : 0;
}

static void draw_capsule(CcPlatform *platform, float x, float y, float width,
                         float height, CcColor color) {
    if (width <= 0 || height <= 0)
        return;
    if (width < height) {
        /* A tiny fill stays inside the track instead of gaining a minimum width. */
        y += (height - width) * 0.5f;
        height = width;
    }
    float radius = height * 0.5f;
    if (width > height) {
        CcQuad middle = {.x = x + radius,
                         .y = y,
                         .width = width - height,
                         .height = height,
                         .color = color};
        cc_platform_draw_quad(platform, &middle);
    }
    static const float arc[5][2] = {{0, -1},
                                    {0.70710678f, -0.70710678f},
                                    {1, 0},
                                    {0.70710678f, 0.70710678f},
                                    {0, 1}};
    for (unsigned side = 0; side < 2; ++side) {
        float direction = side ? 1 : -1;
        float center_x = side ? x + width - radius : x + radius;
        float center_y = y + radius;
        CcDrawVertex center = {center_x, center_y, 0, 0, color};
        for (unsigned segment = 0; segment < 4; ++segment) {
            CcDrawVertex corners[4] = {
                center,
                {center_x + direction * arc[segment][0] * radius,
                 center_y + arc[segment][1] * radius, 0, 0, color},
                center,
                {center_x + direction * arc[segment + 1][0] * radius,
                 center_y + arc[segment + 1][1] * radius, 0, 0, color}};
            cc_platform_draw_vertices(platform, corners, 0);
        }
    }
}

void gc_render_volume_overlay(GcScene *scene) {
    if (!scene || !scene->platform || scene->volume_indicator_alpha <= 0)
        return;
    cc_platform_set_clip(scene->platform, NULL);
    float alpha = scene->volume_indicator_alpha;
    const float x = 480, y = 20, width = 128, height = 4;
    draw_capsule(scene->platform, x, y, width, height,
                 (CcColor){1, 1, 1, alpha * 0.22f});
    float filled = width * (float)scene->volume_indicator_percent /
                   (float)GC_AUDIO_MENU_VOLUME_MAX;
    draw_capsule(scene->platform, x, y, filled, height, (CcColor){1, 1, 1, alpha});
}
