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
