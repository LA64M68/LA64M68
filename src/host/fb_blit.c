#define _POSIX_C_SOURCE 200809L

/* Shared blit helper for every presenter.
 *
 * The splash must look the same on all surfaces even though they differ in
 * size: 320x200 on the Amiga Denise and the Atari ST, 1440x1080 on the
 * bench monitor behind the RPi. Interpolating would smear the pixel art and
 * make the backends disagree, so the image is scaled by a WHOLE factor and
 * centred, with the surrounding area filled in the background colour.
 *
 * On an exact fit (320x200) the scale is 1 and this is a straight copy, so
 * the low-resolution machines stay pixel-exact. */

#include "fb_internal.h"
#include <stddef.h>
#include <stdint.h>

static uint32_t pack_rgb24(uint32_t dst_bpp, uint8_t r, uint8_t g, uint8_t b)
{
    if (dst_bpp == 32) return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
    if (dst_bpp == 24) return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
    if (dst_bpp == 16) return (uint32_t)(((r >> 3) << 11) | ((g >> 2) << 5) |
                                         (b >> 3));
    return 0;
}

static void put_pixel(uint8_t *dst, uint32_t dst_bpp, uint32_t pix)
{
    if (dst_bpp == 32) {
        dst[0] = (uint8_t)(pix & 0xff);
        dst[1] = (uint8_t)((pix >> 8) & 0xff);
        dst[2] = (uint8_t)((pix >> 16) & 0xff);
        dst[3] = 0;
    } else if (dst_bpp == 24) {
        dst[0] = (uint8_t)(pix & 0xff);
        dst[1] = (uint8_t)((pix >> 8) & 0xff);
        dst[2] = (uint8_t)((pix >> 16) & 0xff);
    } else if (dst_bpp == 16) {
        ((uint16_t *)dst)[0] = (uint16_t)pix;
    }
}

void la64m68_fb_blit_scaled(uint8_t *dst, uint32_t dst_stride, uint32_t dst_bpp,
                            uint32_t surf_w, uint32_t surf_h,
                            const uint8_t *src, uint32_t src_w, uint32_t src_h,
                            int src_bpp, uint32_t border_rgb24)
{
    if (!dst || !src || !src_w || !src_h) return;
    if (surf_w == 0) surf_w = src_w;
    if (surf_h == 0) surf_h = src_h;

    /* whole-factor fit: never stretch, never crop */
    uint32_t scale = surf_w / src_w;
    uint32_t sh = surf_h / src_h;
    if (sh < scale) scale = sh;
    if (scale == 0) scale = 1;

    uint32_t out_w = src_w * scale;
    uint32_t out_h = src_h * scale;
    uint32_t ox = (surf_w - out_w) / 2;
    uint32_t oy = (surf_h - out_h) / 2;

    uint32_t border = pack_rgb24(dst_bpp,
                                 (uint8_t)(border_rgb24 >> 16),
                                 (uint8_t)(border_rgb24 >> 8),
                                 (uint8_t)border_rgb24);

    /* fill the surface so the letterbox is a colour, not garbage */
    for (uint32_t y = 0; y < surf_h; y++) {
        uint8_t *row = dst + (size_t)y * dst_stride;
        for (uint32_t x = 0; x < surf_w; x++)
            put_pixel(row + (size_t)x * (dst_bpp / 8), dst_bpp, border);
    }

    for (uint32_t y = 0; y < out_h; y++) {
        uint32_t sy = y / scale;
        uint8_t *row = dst + (size_t)(oy + y) * dst_stride;
        for (uint32_t x = 0; x < out_w; x++) {
            uint32_t sx = x / scale;
            uint8_t r, g, b;
            if (src_bpp == 24) {
                const uint8_t *p = src + ((size_t)sy * src_w + sx) * 3;
                r = p[0]; g = p[1]; b = p[2];
            } else if (src_bpp == 16) {
                uint16_t v = ((const uint16_t *)src)[(size_t)sy * src_w + sx];
                r = (uint8_t)(((v >> 8) & 0xf) * 17);
                g = (uint8_t)(((v >> 4) & 0xf) * 17);
                b = (uint8_t)((v & 0xf) * 17);
            } else {
                return;
            }
            put_pixel(row + (size_t)(ox + x) * (dst_bpp / 8), dst_bpp,
                      pack_rgb24(dst_bpp, r, g, b));
        }
    }
}
