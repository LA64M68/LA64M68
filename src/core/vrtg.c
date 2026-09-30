#include "vrtg.h"
#include "debug.h"
#include <stdlib.h>
#include <string.h>

void la64m68_vrtg_init(la64m68_vrtg *v, la64m68_memory *guest_mem,
                       uint32_t fb_base)
{
    memset(v, 0, sizeof(*v));
    v->guest_mem = guest_mem;
    v->fb_base = fb_base ? fb_base : LA64M68_VRTG_FB_BASE;
    la64m68_vrtg_set_mode(v, 640, 480, 8);      /* VESA-alike default */
}

void la64m68_vrtg_set_mode(la64m68_vrtg *v, uint32_t w, uint32_t h, int bpp)
{
    if (bpp != 8 && bpp != 16 && bpp != 32) bpp = 8;
    /* Bounds: an unbounded guest-supplied width/height would overflow the
     * pitch*size product in la64m68_vrtg_present(). */
    if (!w || w > LA64M68_VRTG_MAX_DIM) w = 640;
    if (!h || h > LA64M68_VRTG_MAX_DIM) h = 480;
    v->width = w; v->height = h; v->bpp = bpp;
    la64m68_trace("vrtg: mode %ux%u@%d fb=%08x", w, h, bpp, v->fb_base);
}

void la64m68_vrtg_present(la64m68_vrtg *v)
{
    if (!v->enabled || !v->present) return;
    size_t stride = (size_t)v->width * (size_t)(v->bpp / 8);
    size_t total = stride * v->height;
    if (!stride || total / stride != v->height) return;   /* product wrapped */
    uint8_t *buf = malloc(total);
    if (!buf) return;
    for (size_t i = 0; i < total; i++)
        buf[i] = la64m68_mem_read8(v->guest_mem, v->fb_base + (uint32_t)i);
    v->present(v->present_ctx, buf, v->width, v->height, v->bpp);
    free(buf);
}

/* ---- register window as memory ---- */

typedef struct {
    la64m68_memory base;
    la64m68_vrtg *v;
} vrtg_regs_mem;

static uint32_t rr_r(void *ctx, uint32_t addr, int size)
{
    la64m68_vrtg *v = ((vrtg_regs_mem *)ctx)->v;
    uint32_t off = addr - LA64M68_VRTG_REG_BASE;
    uint32_t val = 0;
    switch (off) {
    case VRTG_REG_CTRL:   val = v->enabled; break;
    case VRTG_REG_WIDTH:  val = v->width; break;
    case VRTG_REG_HEIGHT: val = v->height; break;
    case VRTG_REG_BPP:    val = (uint32_t)v->bpp; break;
    case VRTG_REG_ADDR:   val = v->fb_base; break;
    default: break;
    }
    if (size == 1) return val & 0xff;
    if (size == 2) return val & 0xffff;
    return val;
}

static void rr_w(void *ctx, uint32_t addr, uint32_t val, int size)
{
    la64m68_vrtg *v = ((vrtg_regs_mem *)ctx)->v;
    uint32_t off = addr - LA64M68_VRTG_REG_BASE;
    (void)size;
    switch (off) {
    case VRTG_REG_CTRL:   v->enabled = val & 1; break;
    case VRTG_REG_WIDTH:
        if (val && val <= LA64M68_VRTG_MAX_DIM) v->width = val;
        else la64m68_trace("vrtg: refused width %u", val);
        break;
    case VRTG_REG_HEIGHT:
        if (val && val <= LA64M68_VRTG_MAX_DIM) v->height = val;
        else la64m68_trace("vrtg: refused height %u", val);
        break;
    case VRTG_REG_BPP:    la64m68_vrtg_set_mode(v, v->width, v->height, (int)val); break;
    case VRTG_REG_ADDR:   v->fb_base = val; break;
    case VRTG_REG_FLIP:   la64m68_vrtg_present(v); break;
    default: break;
    }
}

static uint8_t  rr_r8 (void *c, uint32_t a) { return (uint8_t) rr_r(c, a, 1); }
static uint16_t rr_r16(void *c, uint32_t a) { return (uint16_t)rr_r(c, a, 2); }
static uint32_t rr_r32(void *c, uint32_t a) { return rr_r(c, a, 4); }
static void     rr_w8 (void *c, uint32_t a, uint8_t v)  { rr_w(c, a, v, 1); }
static void     rr_w16(void *c, uint32_t a, uint16_t v) { rr_w(c, a, v, 2); }
static void     rr_w32(void *c, uint32_t a, uint32_t v) { rr_w(c, a, v, 4); }

la64m68_memory *la64m68_vrtg_regs_memory(la64m68_vrtg *v)
{
    vrtg_regs_mem *m = calloc(1, sizeof(*m));
    if (!m) return NULL;
    m->v = v;
    m->base.ctx = m;
    m->base.read8 = rr_r8;
    m->base.read16 = rr_r16;
    m->base.read32 = rr_r32;
    m->base.write8 = rr_w8;
    m->base.write16 = rr_w16;
    m->base.write32 = rr_w32;
    return &m->base;
}

void la64m68_vrtg_regs_destroy(la64m68_memory *m)
{
    free(m);            /* 'base' is the first member of vrtg_regs_mem */
}
