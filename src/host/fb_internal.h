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

#endif
