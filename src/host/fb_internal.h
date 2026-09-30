#ifndef LA64M68_FB_INTERNAL_H
#define LA64M68_FB_INTERNAL_H

#include "fb.h"

/* Internal: backend implementations fill present/destroy/priv and
 * fb.c sets name. */

struct la64m68_fb {
    int width, height;
    const char *name;
    void (*present)(la64m68_fb *fb, const void *pixels, int bpp);
    void (*destroy)(la64m68_fb *fb);
    void *priv;
};

int la64m68_fb_x11_init(la64m68_fb *fb);
int la64m68_fb_vc_init(la64m68_fb *fb);
int la64m68_fb_fbdev_init(la64m68_fb *fb);

/* Integer-scale `src` onto a `surf_w x surf_h` surface, centred, with the
 * surrounding area filled in `border_rgb24`. Scale 1 on an exact fit, so the
 * 320x200 machines stay pixel-exact while larger surfaces grow the picture
 * by a whole factor instead of smearing it. */
void la64m68_fb_blit_scaled(uint8_t *dst, uint32_t dst_stride, uint32_t dst_bpp,
                            uint32_t surf_w, uint32_t surf_h,
                            const uint8_t *src, uint32_t src_w, uint32_t src_h,
                            int src_bpp, uint32_t border_rgb24);

#endif
