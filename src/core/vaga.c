#include "vaga.h"
#include "debug.h"
#include <stdlib.h>
#include <string.h>

/* Custom-chip register offsets (word offsets from 0xDFF000) */
#define REG_VPOSR    0x004   /* long: version + vpos high */
#define REG_VHPOSR   0x006
#define REG_JOY0DAT  0x00a
#define REG_JOY1DAT  0x00c
#define REG_ADKCONR  0x010
#define REG_POTGOR   0x016
#define REG_SERDATR  0x018
#define REG_DSKBYTR  0x01a
#define REG_INTENAR  0x01c
#define REG_INTREQR  0x01e
#define REG_DSKPTH   0x020
#define REG_DMACONR  0x002
#define REG_INTENA   0x09a
#define REG_INTREQ   0x09c
#define REG_DMACON   0x096
#define REG_ADKCON   0x09e
#define REG_SERDAT   0x030

#define INTENA_SETCLR 0x8000

typedef struct { la64m68_vaga *v; } vaga_ctx;

static uint8_t rd8(void *ctx, uint32_t off)
{
    vaga_ctx *c = ctx;
    la64m68_vaga *v = c->v;
    off &= LA64M68_VAGA_SIZE - 1;
    uint32_t w = off & ~1u;
    uint16_t r;
    switch (w) {
    case REG_VPOSR:  /* ECS/AGA board id + vpos high bits */
        r = (uint16_t)((v->vpos >> 16) & 0x1ff) | 0x2200; /* 8375-class */
        break;
    case REG_VHPOSR:
        r = (uint16_t)(v->vpos & 0xffff);
        break;
    case REG_INTENAR:
        r = v->regs[REG_INTENA >> 1];
        break;
    case REG_INTREQR:
        r = v->intreq;
        break;
    case REG_DMACONR:
        r = v->regs[REG_DMACON >> 1] | 0x8000;
        break;
    default:
        r = v->regs[w >> 1];
        break;
    }
    return (off & 1) ? (uint8_t)r : (uint8_t)(r >> 8);
}

static uint16_t rd16(void *ctx, uint32_t off)
{
    vaga_ctx *c = ctx;
    off &= ~1u;
    uint8_t hi = rd8(c, off), lo = rd8(c, off + 1);
    return ((uint16_t)hi << 8) | lo;
}

static uint32_t rd32(void *ctx, uint32_t off)
{
    return ((uint32_t)rd16(ctx, off) << 16) | rd16(ctx, off + 2);
}

static void wr16(void *ctx, uint32_t off, uint16_t val);

static void wr8(void *ctx, uint32_t off, uint8_t val)
{
    /* byte write merges into the word register */
    vaga_ctx *c = ctx;
    uint32_t w = off & ~1u;
    uint16_t cur = c->v->regs[(w & (LA64M68_VAGA_SIZE - 1)) >> 1];
    if (off & 1)
        wr16(c, w, (cur & 0xff00) | val);
    else
        wr16(c, w, (cur & 0x00ff) | ((uint16_t)val << 8));
}

static void wr16(void *ctx, uint32_t off, uint16_t val)
{
    vaga_ctx *c = ctx;
    la64m68_vaga *v = c->v;
    off &= LA64M68_VAGA_SIZE - 1;
    off &= ~1u;
    switch (off) {
    case REG_INTENA:
        if (val & INTENA_SETCLR) v->regs[off >> 1] |= (val & 0x7fff);
        else                   v->regs[off >> 1] &= ~val;
        v->regs[off >> 1] = (v->regs[off >> 1] & 0x7fff) |
                            (val & INTENA_SETCLR);
        break;
    case REG_INTREQ:
        if (val & INTENA_SETCLR) v->intreq |= (val & 0x7fff);
        else                   v->intreq &= ~val;
        break;
    default:
        /* tolerant model: store writes to any offset so unknown registers
         * stay observable while probing the chipset */
        v->regs[off >> 1] = val;
        break;
    }
    la64m68_trace("vaga: write %03x <- %04x", off, val);
}

static void wr32(void *ctx, uint32_t off, uint32_t val)
{
    wr16(ctx, off, (uint16_t)(val >> 16));
    wr16(ctx, off + 2, (uint16_t)val);
}

void la64m68_vaga_init(la64m68_vaga *v)
{
    memset(v, 0, sizeof(*v));
}

la64m68_memory *la64m68_vaga_memory(la64m68_vaga *v)
{
    vaga_ctx *c = malloc(sizeof(*c));
    la64m68_memory *m = malloc(sizeof(*m));
    if (!c || !m) { free(c); free(m); return NULL; }
    *c = (vaga_ctx){ v };
    *m = (la64m68_memory){ c, rd8, rd16, rd32, wr8, wr16, wr32 };
    return m;
}

void la64m68_vaga_mem_destroy(la64m68_memory *m)
{
    if (!m) return;
    free(m->ctx);
    free(m);
}

void la64m68_vaga_raise(la64m68_vaga *v, int bit)
{
    v->intreq |= (uint16_t)(1u << (bit & 15));
}

int la64m68_vaga_ipl(la64m68_vaga *v)
{
    /* INTREQ bit -> level (standard Amiga groups):
     *   bits0-2 (TBE,DSKBLK,SOFTINT)=1, bit3 (PORTS)=2,
     *   bits4-6 (COPER,VERTB,BLIT)=3, bits7-10 (AUD0-3)=4,
     *   bits11-12 (RBF,DSKSYNC)=5, bit13 (EXTER)=6 */
    static const int8_t lvl[14] = {
        1,1,1, 2, 3,3,3, 4,4,4,4, 5,5, 6
    };
    uint16_t pend = v->intreq & v->regs[REG_INTENA >> 1];
    int best = 0;
    for (int b = 0; b < 14; b++)
        if ((pend & (1u << b)) && lvl[b] > best)
            best = lvl[b];
    return best;
}

void la64m68_vaga_tick(la64m68_vaga *v, int cycles)
{
    v->vpos += (uint32_t)cycles;      /* coarse: vpos advances with cycles */
}

/* ---- display decoding ------------------------------------------------- */

/* custom-register word offsets, as written in the hardware manual */
#define R_BPLCON0   0x100
#define R_DIWSTRT   0x08E
#define R_DIWSTOP   0x090
#define R_BPL1PTH   0x0E0
#define R_BPL1MOD   0x108
#define R_BPL2MOD   0x10A
#define R_COLOR00   0x180

void la64m68_vaga_set_mem(la64m68_vaga *v, la64m68_memory *m)
{
    if (v) v->mem = m;
}

int la64m68_vaga_render(la64m68_vaga *v, uint8_t *rgb24, uint32_t stride,
                        uint32_t *w, uint32_t *h)
{
    if (!v || !v->mem || !rgb24) return -1;

    uint16_t con0 = v->regs[R_BPLCON0 / 2];
    int depth = (con0 >> 12) & 7;
    int hires = (con0 & 0x8000) != 0;
    /* HAM and dual-playfield are not decoded yet: treat as a plain index */
    if (depth < 1 || depth > 6) {
        /* one-shot diagnosis: if the guest never enables a display we must
         * be able to see that instead of guessing why the screen is empty */
        static int told;
        if (!told) {
            told = 1;
            la64m68_trace("vaga: no display configured (BPLCON0=%04x, "
                          "depth=%d, DIWSTRT=%04x) -> nothing to render",
                          con0, depth, v->regs[R_DIWSTRT / 2]);
        }
        return -1;
    }

    uint32_t width  = hires ? 640u : 320u;
    uint16_t diwstrt = v->regs[R_DIWSTRT / 2], diwstop = v->regs[R_DIWSTOP / 2];
    uint32_t height = ((diwstop >> 8) - (diwstrt >> 8)) & 0xffu;
    if (height == 0) height = 256;

    uint32_t row_bytes = width / 8;

    /* bitplane pointers: BPL1PTH/L .. BPL6PTH/L, one long each */
    uint32_t ptr[6];
    for (int p = 0; p < 6; p++)
        ptr[p] = ((uint32_t)v->regs[(R_BPL1PTH + p * 4) / 2] << 16) |
                 v->regs[(R_BPL1PTH + p * 4 + 2) / 2];

    /* odd planes use BPL1MOD, even planes BPL2MOD */
    int mod[6];
    for (int p = 0; p < 6; p++)
        mod[p] = (int16_t)v->regs[((p & 1) ? R_BPL2MOD : R_BPL1MOD) / 2];

    /* 12-bit 0xRGB palette, one entry per index */
    uint8_t pal[64][3];
    for (int i = 0; i < 64; i++) {
        uint16_t c = v->regs[(R_COLOR00 + i * 2) / 2];
        pal[i][0] = (uint8_t)(((c >> 8) & 0xf) * 17);
        pal[i][1] = (uint8_t)(((c >> 4) & 0xf) * 17);
        pal[i][2] = (uint8_t)((c & 0xf) * 17);
    }

    for (uint32_t y = 0; y < height; y++) {
        uint8_t *dst = rgb24 + (size_t)y * stride;
        /* one byte per bitplane for the current 8-pixel group */
        uint8_t row[6][80];                        /* 640/8 = 80 bytes */
        for (int p = 0; p < depth; p++) {
            if (row_bytes > sizeof(row[0])) return -1;
            for (uint32_t i = 0; i < row_bytes; i++)
                row[p][i] = la64m68_mem_read8(v->mem, ptr[p] + i);
            ptr[p] += row_bytes + (uint32_t)mod[p];
        }
        for (uint32_t x = 0; x < width; x++) {
            uint32_t byte = x >> 3;
            int bit = 7 - (int)(x & 7);
            int idx = 0;
            for (int p = 0; p < depth; p++)
                if ((row[p][byte] >> bit) & 1) idx |= 1 << p;
            dst[x * 3 + 0] = pal[idx][0];
            dst[x * 3 + 1] = pal[idx][1];
            dst[x * 3 + 2] = pal[idx][2];
        }
    }
    if (w) *w = width;
    if (h) *h = height;
    return 0;
}
