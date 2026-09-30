#define _POSIX_C_SOURCE 200809L

/* fbdev presenter: /dev/fb0.
 *
 * This is "VideoCore direkt" in its classic form. A Pi whose kernel runs the
 * bcm2835 framebuffer rather than the VC4 KMS driver exposes /dev/fb0 and no
 * /dev/dri at all -- which is exactly what the A124B bench machine does --
 * so the DRM presenter cannot work there. Falling through to "null" would
 * mean we can never see anything the guest draws.
 *
 * Nothing is installed anywhere: the device node is used as it is. */

#include "fb_internal.h"
#include "../core/debug.h"

#include <fcntl.h>
#include <linux/fb.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

typedef struct {
    int fd;
    uint8_t *mem;
    size_t mem_len;
    uint32_t line_length;
    uint32_t xres, yres;
    int bpp;
    /* channel placement read from the hardware, so RGB565 and friends work */
    int roff, goff, boff;
    int rlen, glen, blen;
} fbdev_state;

static fbdev_state g_fb;

static void fbdev_present(la64m68_fb *fb, const void *pixels, int bpp)
{
    (void)fb;
    if (!g_fb.mem) return;
    const uint8_t *src = pixels;
    uint32_t w = (uint32_t)(fb && fb->width  > 0 ? fb->width  : (int)g_fb.xres);
    uint32_t h = (uint32_t)(fb && fb->height > 0 ? fb->height : (int)g_fb.yres);
    (void)w; (void)h;

    /* Letterbox fill: the source's first pixel is the picture's own
     * background, so the border matches instead of being a foreign colour. */
    uint32_t border = 0x222255;
    if (bpp == 24) border = ((uint32_t)src[0] << 16) | ((uint32_t)src[1] << 8) | src[2];

    la64m68_fb_blit_scaled(g_fb.mem, g_fb.line_length, (uint32_t)g_fb.bpp,
                           g_fb.xres, g_fb.yres,
                           src, fb ? (uint32_t)fb->width  : g_fb.xres,
                               fb ? (uint32_t)fb->height : g_fb.yres,
                           bpp, border);
    return;

}

static void fbdev_destroy(la64m68_fb *fb)
{
    (void)fb;
    if (g_fb.mem && g_fb.mem_len) munmap(g_fb.mem, g_fb.mem_len);
    if (g_fb.fd >= 0) close(g_fb.fd);
    memset(&g_fb, 0, sizeof(g_fb));
    g_fb.fd = -1;
}

int la64m68_fb_fbdev_init(la64m68_fb *fb)
{
    memset(&g_fb, 0, sizeof(g_fb));
    g_fb.fd = -1;

    int fd = open("/dev/fb0", O_RDWR);
    if (fd < 0) {
        la64m68_trace("fbdev: /dev/fb0 not available");
        return -1;
    }

    struct fb_var_screeninfo var;
    struct fb_fix_screeninfo fix;
    if (ioctl(fd, FBIOGET_VSCREENINFO, &var) != 0 ||
        ioctl(fd, FBIOGET_FSCREENINFO, &fix) != 0) {
        la64m68_trace("fbdev: cannot query /dev/fb0");
        close(fd);
        return -1;
    }

    g_fb.fd = fd;
    g_fb.xres = var.xres;
    g_fb.yres = var.yres;
    g_fb.bpp = (int)var.bits_per_pixel;
    g_fb.line_length = fix.line_length;
    g_fb.roff = (int)var.red.offset;   g_fb.rlen = (int)var.red.length;
    g_fb.goff = (int)var.green.offset; g_fb.glen = (int)var.green.length;
    g_fb.boff = (int)var.blue.offset;  g_fb.blen = (int)var.blue.length;

    if (g_fb.bpp != 16 && g_fb.bpp != 24 && g_fb.bpp != 32) {
        la64m68_trace("fbdev: %d bpp not supported", g_fb.bpp);
        close(fd);
        g_fb.fd = -1;
        return -1;
    }

    g_fb.mem_len = (size_t)fix.line_length * var.yres;
    g_fb.mem = mmap(NULL, g_fb.mem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (g_fb.mem == MAP_FAILED) {
        la64m68_trace("fbdev: mmap failed");
        close(fd);
        memset(&g_fb, 0, sizeof(g_fb));
        g_fb.fd = -1;
        return -1;
    }

    fb->priv = &g_fb;
    fb->present = fbdev_present;
    fb->destroy = fbdev_destroy;
    la64m68_trace("fbdev: /dev/fb0 %ux%u %d bpp, line %u, RGB %d:%d:%d @%d/%d/%d",
                  g_fb.xres, g_fb.yres, g_fb.bpp, g_fb.line_length,
                  g_fb.rlen, g_fb.glen, g_fb.blen,
                  g_fb.roff, g_fb.goff, g_fb.boff);
    return 0;
}
