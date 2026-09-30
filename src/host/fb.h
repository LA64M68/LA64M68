#ifndef LA64M68_FB_H
#define LA64M68_FB_H

#include <stdint.h>

/* Host framebuffer presenter — vRTG backend.
 * Selection order per cfg vrtg.backend: auto tries vc (VideoCore,
 * dispmanx/DRM on RPi4B), then x11 window (XOrg only, no Wayland),
 * then null. Real PCI/Zorro passthrough outranks this whole device.
 */

typedef struct la64m68_fb la64m68_fb;

la64m68_fb *la64m68_fb_create(const char *backend, int width, int height);
void        la64m68_fb_present(la64m68_fb *fb, const void *pixels, int bpp);
void        la64m68_fb_destroy(la64m68_fb *fb);
const char *la64m68_fb_backend_name(const la64m68_fb *fb);

#endif
