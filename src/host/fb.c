#include "fb_internal.h"
#include "../core/debug.h"
#include <stdlib.h>
#include <string.h>

/* Backend chain: vc (VideoCore/DRM) -> x11 window -> null.
 * Real backends land behind LA64M68_HAVE_VC / LA64M68_HAVE_X11
 * compile guards; BSD/headless always get the null presenter. */

static void null_present(la64m68_fb *fb, const void *pixels, int bpp)
{
    (void)fb; (void)pixels; (void)bpp;
}

la64m68_fb *la64m68_fb_create(const char *backend, int width, int height)
{
    const char *want = backend ? backend : "auto";
    la64m68_fb *fb = calloc(1, sizeof(*fb));
    if (!fb) return NULL;
    fb->width = width;
    fb->height = height;
    fb->name = "null";
    fb->present = null_present;

    int try_vc  = !strcmp(want, "auto") || !strcmp(want, "vc");
    int try_x11 = !strcmp(want, "auto") || !strcmp(want, "x11");

#ifdef LA64M68_HAVE_VC
    if (try_vc && la64m68_fb_vc_init(fb) == 0) {
        fb->name = "vc";
        la64m68_trace("fb: backend=vc %dx%d", width, height);
        return fb;
    }
#else
    (void)try_vc;
#endif
#ifdef LA64M68_HAVE_X11
    if (try_x11 && la64m68_fb_x11_init(fb) == 0) {
        fb->name = "x11";
        la64m68_trace("fb: backend=x11 %dx%d", width, height);
        return fb;
    }
#else
    (void)try_x11;
#endif
    la64m68_trace("fb: backend=%s -> null (vc/x11 pending)", want);
    return fb;
}

void la64m68_fb_present(la64m68_fb *fb, const void *pixels, int bpp)
{
    if (fb && fb->present) fb->present(fb, pixels, bpp);
}

void la64m68_fb_destroy(la64m68_fb *fb)
{
    if (!fb) return;
    if (fb->destroy) fb->destroy(fb);
    free(fb);
}

const char *la64m68_fb_backend_name(const la64m68_fb *fb)
{
    return fb ? fb->name : "none";
}
