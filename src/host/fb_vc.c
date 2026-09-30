/* VideoCore direct backend — DRM/KMS dumb-buffer presenter for vRTG.
 * Target: RPi4B+ with vc4-kms-v3d (full KMS). One modeset at create,
 * presents via XRGB8888 dumb BO + page-flip-less dirty update (KISS).
 * Selected via vrtg.backend=vc or auto (tried before x11). */

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "fb_internal.h"
#include "../core/debug.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/ioctl.h>

#include <xf86drm.h>
#include <xf86drmMode.h>

typedef struct {
    int fd;
    uint32_t conn_id, crtc_id, fb_id, bo;
    uint32_t pitch;
    uint64_t map_size;
    void *map;
    drmModeModeInfo mode;
    drmModeCrtc *old_crtc;
    uint32_t *conv;
} fb_vc;

/* guest bpp -> XRGB8888 conversion (same ramps as fb_x11) */
static void vc_convert(fb_vc *v, la64m68_fb *fb, const void *pixels, int bpp)
{
    const int w = fb->width, h = fb->height;
    const uint8_t *src = pixels;
    uint32_t *dst = v->conv;
    if (bpp == 32) {
        memcpy(dst, src, (size_t)w * h * 4);
    } else if (bpp == 16) {
        const uint16_t *s = pixels;
        for (long i = 0; i < (long)w * h; i++) {
            uint16_t p = s[i];
            dst[i] = ((p & 0xf800) << 8) | ((p & 0x07e0) << 5) |
                     ((p & 0x001f) << 3);
        }
    } else {
        for (long i = 0; i < (long)w * h; i++) {
            uint8_t p = src[i];
            dst[i] = ((p & 0xe0) << 16) | ((p & 0x1c) << 11) |
                     ((p & 0x03) << 6);
        }
    }
}

static void vc_present(la64m68_fb *fb, const void *pixels, int bpp)
{
    fb_vc *v = fb->priv;
    vc_convert(v, fb, pixels, bpp);
    /* copy rows honoring dumb-BO pitch */
    uint8_t *d = v->map;
    for (int y = 0; y < fb->height; y++)
        memcpy(d + (size_t)y * v->pitch,
               v->conv + (size_t)y * fb->width,
               (size_t)fb->width * 4);
    /* dumb fb: single buffer, modeset once -> no flip needed; the
     * scanout reads the BO directly. */
}

static void vc_destroy(la64m68_fb *fb)
{
    fb_vc *v = fb->priv;
    if (v->old_crtc)
        drmModeSetCrtc(v->fd, v->old_crtc->crtc_id, v->old_crtc->buffer_id,
                       v->old_crtc->x, v->old_crtc->y, &v->conn_id, 1,
                       &v->old_crtc->mode);
    if (v->fb_id) drmModeRmFB(v->fd, v->fb_id);
    if (v->map && v->map != MAP_FAILED) munmap(v->map, v->map_size);
    if (v->bo) {
        struct drm_gem_close gc = { .handle = v->bo };
        ioctl(v->fd, DRM_IOCTL_GEM_CLOSE, &gc);
    }
    if (v->old_crtc) drmModeFreeCrtc(v->old_crtc);
    if (v->fd >= 0) close(v->fd);
    free(v->conv);
    free(v);
}

int la64m68_fb_vc_init(la64m68_fb *fb)
{
    fb_vc *v = calloc(1, sizeof(*v));
    if (!v) return -1;
    v->fd = -1;
    fb->priv = v;

    /* prefer card1/card0 (Pi: card1 is usually vc4 KMS) */
    for (int c = 0; c < 4 && v->fd < 0; c++) {
        char p[32];
        snprintf(p, sizeof(p), "/dev/dri/card%d", c);
        v->fd = open(p, O_RDWR | O_CLOEXEC);
        if (v->fd >= 0) {
            drmModeRes *r = drmModeGetResources(v->fd);
            if (!r) { close(v->fd); v->fd = -1; continue; }
            drmModeFreeResources(r);
        }
    }
    if (v->fd < 0) { free(v); fb->priv = NULL; return -1; }

    drmModeRes *res = drmModeGetResources(v->fd);
    if (!res) goto fail;
    drmModeConnector *conn = NULL;
    for (int i = 0; i < res->count_connectors; i++) {
        conn = drmModeGetConnector(v->fd, res->connectors[i]);
        if (conn && conn->connection == DRM_MODE_CONNECTED &&
            conn->count_modes > 0)
            break;
        drmModeFreeConnector(conn);
        conn = NULL;
    }
    if (!conn) { drmModeFreeResources(res); goto fail; }

    /* pick a mode matching the requested window size if available,
     * else the connector's preferred mode */
    v->mode = conn->modes[0];
    for (int i = 0; i < conn->count_modes; i++)
        if (conn->modes[i].hdisplay == fb->width &&
            conn->modes[i].vdisplay == fb->height)
            v->mode = conn->modes[i];
    v->conn_id = conn->connector_id;
    la64m68_trace("fb/vc: mode %ux%u on conn %u",
                  v->mode.hdisplay, v->mode.vdisplay, v->conn_id);

    drmModeEncoder *enc = drmModeGetEncoder(v->fd, conn->encoder_id);
    uint32_t crtc_id = 0;
    if (enc) { crtc_id = enc->crtc_id; drmModeFreeEncoder(enc); }
    if (!crtc_id && res->count_crtcs > 0) crtc_id = res->crtcs[0];
    if (!crtc_id) { drmModeFreeConnector(conn); drmModeFreeResources(res); goto fail; }
    v->crtc_id = crtc_id;
    v->old_crtc = drmModeGetCrtc(v->fd, crtc_id);
    drmModeFreeConnector(conn);
    drmModeFreeResources(res);

    /* dumb buffer sized to the guest window (pixels land top-left) */
    struct drm_mode_create_dumb creq = {
        .width = fb->width, .height = fb->height, .bpp = 32 };
    if (ioctl(v->fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq) < 0) goto fail;
    v->bo = creq.handle;
    v->pitch = creq.pitch;
    v->map_size = creq.size;
    if (drmModeAddFB(v->fd, fb->width, fb->height, 24, 32,
                     v->pitch, v->bo, &v->fb_id) < 0) goto fail;
    struct drm_mode_map_dumb mreq = { .handle = v->bo };
    if (ioctl(v->fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq) < 0) goto fail;
    v->map = mmap(NULL, v->map_size, PROT_READ | PROT_WRITE, MAP_SHARED,
                  v->fd, mreq.offset);
    if (v->map == MAP_FAILED) goto fail;
    memset(v->map, 0, v->map_size);

    v->conv = malloc((size_t)fb->width * fb->height * 4);
    if (!v->conv) goto fail;

    if (drmModeSetCrtc(v->fd, v->crtc_id, v->fb_id, 0, 0,
                       &v->conn_id, 1, &v->mode) < 0) goto fail;

    fb->priv = v;
    fb->present = vc_present;
    fb->destroy = vc_destroy;
    return 0;

fail:
    la64m68_trace("fb/vc: init failed: %s", strerror(errno));
    vc_destroy(fb);
    fb->priv = NULL;
    fb->present = NULL;
    fb->destroy = NULL;
    return -1;
}
