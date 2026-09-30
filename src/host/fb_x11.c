/* X11 window presenter for vRTG — XOrg only, no Wayland.
 * Guest pixels are converted to a TrueColor XImage (8bpp via 3:3:2 RGB,
 * 16bpp via RGB565, 32bpp direct). MIT-SHM can be added later; XPutImage
 * keeps this path simple and correct first. */

#include "fb_internal.h"
#include "../core/debug.h"
#include <stdlib.h>
#include <string.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

typedef struct {
    Display *dpy;
    Window   win;
    GC       gc;
    XImage  *img;
    uint32_t *conv;        /* converted TrueColor buffer */
} fb_x11;

static void x11_present(la64m68_fb *fb, const void *pixels, int bpp)
{
    fb_x11 *x = fb->priv;
    const int w = fb->width, h = fb->height;
    const uint8_t *src = pixels;
    uint32_t *dst = x->conv;

    if (bpp == 32) {
        memcpy(dst, src, (size_t)w * h * 4);
    } else if (bpp == 16) {
        const uint16_t *s = pixels;
        for (long i = 0; i < (long)w * h; i++) {
            uint16_t p = s[i];
            dst[i] = ((p & 0xf800) << 8) | ((p & 0x07e0) << 5) |
                     ((p & 0x001f) << 3);
        }
    } else {                       /* 8bpp indexed -> 3:3:2 RGB ramp */
        for (long i = 0; i < (long)w * h; i++) {
            uint8_t p = src[i];
            dst[i] = ((p & 0xe0) << 16) | ((p & 0x1c) << 11) |
                     ((p & 0x03) << 6);
        }
    }

    memcpy(x->img->data, dst, (size_t)w * h * 4);
    XPutImage(x->dpy, x->win, x->gc, x->img, 0, 0, 0, 0, w, h);
    XFlush(x->dpy);

    /* drain events so the WM stays responsive; quit on close */
    while (XPending(x->dpy)) {
        XEvent ev;
        XNextEvent(x->dpy, &ev);
    }
}

static void x11_destroy(la64m68_fb *fb)
{
    fb_x11 *x = fb->priv;
    if (x->img) { x->img->data = NULL; XDestroyImage(x->img); }
    if (x->gc)  XFreeGC(x->dpy, x->gc);
    if (x->win) XDestroyWindow(x->dpy, x->win);
    if (x->dpy) XCloseDisplay(x->dpy);
    free(x->conv);
    free(x);
}

int la64m68_fb_x11_init(la64m68_fb *fb)
{
    fb_x11 *x = calloc(1, sizeof(*x));
    if (!x) return -1;

    x->dpy = XOpenDisplay(NULL);
    if (!x->dpy) { free(x); return -1; }

    int screen = DefaultScreen(x->dpy);
    x->win = XCreateSimpleWindow(x->dpy, RootWindow(x->dpy, screen),
                               0, 0, fb->width, fb->height, 0,
                               BlackPixel(x->dpy, screen),
                               BlackPixel(x->dpy, screen));
    XStoreName(x->dpy, x->win, "LA64M68 vRTG");
    XSelectInput(x->dpy, x->win, ExposureMask | StructureNotifyMask);
    XMapWindow(x->dpy, x->win);
    x->gc = XCreateGC(x->dpy, x->win, 0, NULL);

    x->conv = malloc((size_t)fb->width * fb->height * 4);
    x->img = XCreateImage(x->dpy, DefaultVisual(x->dpy, screen), 24,
                          ZPixmap, 0, malloc((size_t)fb->width * fb->height * 4),
                          fb->width, fb->height, 32, 0);
    if (!x->conv || !x->img) {
        x11_destroy(&(la64m68_fb){ .priv = x, .width = fb->width,
                                   .height = fb->height });
        return -1;
    }
    XSync(x->dpy, False);

    fb->priv = x;
    fb->present = x11_present;
    fb->destroy = x11_destroy;
    return 0;
}
