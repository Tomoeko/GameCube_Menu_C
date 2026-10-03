#include "render/render_projection.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>

static void native_projection(bool pal, bool perspective, float x, float y,
                              float output[2]) {
    /* Compose the original projection and viewport independently of helpers. */
    float clip_x = perspective ? x / 296 : (2 * x + 4) / 588;
    float clip_y = y / 224;
    float framebuffer_height = pal ? 520 : 448;
    float signal_fit = pal ? 480.0f / 576 : 1;
    float border_y = pal ? 28 : 16;
    float viewport_shift_y = pal ? 0 : -0.5f;
    output[0] = 24 + (clip_x + 1) * (592 + 0.5f) / 2;
    output[1] =
        (border_y + viewport_shift_y + (1 - clip_y) * (framebuffer_height + 0.5f) / 2) *
        signal_fit;
}

int main(void) {
    const float positions[][2] = {
        {0, 0}, {-292, 224}, {296, -224}, {144, -55}, {-27.25f, 91.75f}};
    for (unsigned region = 0; region < 2; ++region) {
        for (unsigned mode = 0; mode < 2; ++mode) {
            for (unsigned index = 0; index < sizeof(positions) / sizeof(positions[0]);
                 ++index) {
                float expected[2];
                native_projection(region != 0, mode != 0, positions[index][0],
                                  positions[index][1], expected);
                float x = gc_render_projection_x(mode != 0, positions[index][0]);
                float y =
                    gc_render_projection_center_y(region != 0) -
                    positions[index][1] * gc_render_projection_scale_y(region != 0);
                assert(fabsf(x - expected[0]) < 0.0001f);
                assert(fabsf(y - expected[1]) < 0.0001f);
            }
        }
    }
    puts("Fractional viewport projection matches both regional signal fits.");
    return 0;
}
