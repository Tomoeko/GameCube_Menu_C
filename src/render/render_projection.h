#ifndef GAMECUBE_RENDER_PROJECTION_H
#define GAMECUBE_RENDER_PROJECTION_H

#include <stdbool.h>

/* The viewport is half a pixel larger than the active framebuffer. NTSC
 * shifts it half a pixel upward; PAL does not. Keep the centered signal fit. */
static inline float gc_render_projection_scale_x(bool perspective) {
    return 592.5f / (perspective ? 592.0f : 588.0f);
}

static inline float gc_render_projection_center_x(bool perspective) {
    return perspective ? 320.25f : 24 + 296 * gc_render_projection_scale_x(false);
}

static inline float gc_render_projection_x(bool perspective, float position) {
    return gc_render_projection_center_x(perspective) +
           position * gc_render_projection_scale_x(perspective);
}

static inline float gc_render_projection_center_y(bool pal) {
    return pal ? 240 + 0.25f * (480.0f / 576) : 239.75f;
}

static inline float gc_render_projection_scale_y(bool pal) {
    return pal ? 520.5f / 448 * (480.0f / 576) : 448.5f / 448;
}

#endif
