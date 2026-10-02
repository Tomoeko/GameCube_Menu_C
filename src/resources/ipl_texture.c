#include "gamecube/ipl_model.h"
#include "console_common/resources/gx_texture.h"
#include "console_common/support/bounds.h"
#include "console_common/support/endian.h"

#include <stdlib.h>

bool gc_ipl_texture_decode(const uint8_t *data, size_t size, GcIplImage *image) {
    if (!data || size < 32 || !image || image->rgba)
        return false;
    unsigned width = cc_read_be16(data + 2);
    unsigned height = cc_read_be16(data + 4);
    size_t pixels = cc_read_be32(data + 28);
    size_t encoded_bytes;
    size_t rgba_bytes;
    if (data[0] > 6 || width > 1024 || height > 1024 || data[6] > 2 || data[7] > 2 ||
        !cc_gx_texture_size(data[0], width, height, &encoded_bytes, &rgba_bytes) ||
        !cc_bounds_contains(size, pixels, encoded_bytes))
        return false;
    GcIplImage result = {
        .width = width, .height = height, .wrap_s = data[6], .wrap_t = data[7]};
    result.rgba = malloc(rgba_bytes);
    if (!result.rgba)
        return false;
    CcGxTexture texture = {.width = width,
                           .height = height,
                           .format = data[0],
                           .pixels = data + pixels,
                           .pixel_bytes = encoded_bytes,
                           .expansion = CC_GX_COLOR_SCALE};
    if (!cc_gx_texture_decode(&texture, result.rgba, rgba_bytes)) {
        free(result.rgba);
        return false;
    }
    /* USA 06304 passes BTI bytes 6/7 to GXInitTexObj's wrap S/T arguments. */
    *image = result;
    return true;
}
