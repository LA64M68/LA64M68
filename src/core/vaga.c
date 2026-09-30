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
#define REG_BLTSIZE 0x058
#define REG_DENISEID LA64M68_VAGA_REG_DENISEID
#define REG_COP1LCH LA64M68_VAGA_REG_COP1LCH
#define REG_COP1LCL LA64M68_VAGA_REG_COP1LCL
#define REG_COPJMP1 LA64M68_VAGA_REG_COPJMP1
#define REG_INTENAR  0x01c
#define REG_INTREQR  0x01e
#define REG_DSKPTH   0x020
#define REG_DMACONR  0x002
#define REG_BLTSIZE 0x058
#define REG_INTENA   0x09a
#define REG_INTREQ   0x09c
#define REG_DMACON   0x096
#define REG_ADKCON   0x09e
#define REG_SERDAT   0x030

#define INTENA_SETCLR 0x8000

typedef struct { la64m68_vaga *v; } vaga_ctx;

static void wr16(void *ctx, uint32_t off, uint16_t val);
static void trace_read(uint32_t off, uint16_t val);

/* Read trace. Kickstart polls hardware in tight retry loops; without seeing
 * what it reads we can only guess which answer is missing. Rate-limited to
 * the last address per register so a polling loop does not flood stderr. */
static uint32_t last_read_off = 0xffffffffu;
static int      last_read_val = -1;

static void trace_read(uint32_t off, uint16_t val)
{
    if (off == last_read_off && (int)val == last_read_val) return;
    last_read_off = off;
    last_read_val = (int)val;
    la64m68_trace("vaga: read  %03x = %04x", off, val);
}

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
    case REG_DENISEID:
        /* read-only chip id; a stored zero here reads as "no known chip" */
        r = LA64M68_VAGA_DENISEID_AGA;
        trace_read(w, r);
        break;
    default:
        r = v->regs[w >> 1];
        trace_read(w, r);
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

static void vaga_blit_start(la64m68_vaga *v, uint16_t bltsize);
static void vaga_copper_start(la64m68_vaga *v);
static void vaga_copper_run(la64m68_vaga *v, int steps);

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

/* Register write, shared by the CPU-facing window and the copper. The copper
 * must NOT go through wr16(): that one takes a vaga_ctx*, and handing it the
 * device state instead dereferences garbage. */
static void vaga_reg_write(la64m68_vaga *v, uint32_t off, uint16_t val)
{
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
    case REG_COPJMP1:
        /* strobe: restart the copper at COP1LC */
        vaga_copper_start(v);
        break;
    case REG_BLTSIZE:
        /* writing the size is what STARTS the blit */
        v->regs[off >> 1] = val;
        vaga_blit_start(v, val);
        break;
    default:
        /* tolerant model: store writes to any offset so unknown registers
         * stay observable while probing the chipset */
        v->regs[off >> 1] = val;
        break;
    }
    la64m68_trace("vaga: write %03x <- %04x", off, val);
}

static void wr16(void *ctx, uint32_t off, uint16_t val)
{
    vaga_reg_write(((vaga_ctx *)ctx)->v, off, val);
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

/* Writing BLTSIZE starts a blit. We only model the completion semantics the
 * guest waits on, not the pixel work -- that is a separate piece. */
static void vaga_blit_start(la64m68_vaga *v, uint16_t bltsize)
{
    uint32_t w = (uint32_t)(bltsize & 0x3f);
    uint32_t h = (uint32_t)(bltsize >> 6) & 0x3ffu;
    if (w == 0) w = 64;                     /* encoded 0 means 64 */
    if (h == 0) h = 1024;
    v->blit_left = w * h * 2u + 16u;        /* rough cycle cost */
}

/* ---- copper -----------------------------------------------------------
 *
 * The Amiga does its display setup through the copper, not through direct
 * register writes: the list holds MOVE instructions that set BPLCON0, DIWSTRT,
 * the colours and the bitplane pointers. Without a copper the guest writes a
 * list, strobes COPJMP1 and then polls forever -- which is exactly what
 * Kickstart 3.9 did.
 *
 * Instruction format (two words each):
 *   bit0 of word1 == 0 : MOVE  -- word1 bits 15:9 = register offset >> 1,
 *                                 word2 = data
 *   bit0 of word1 == 1 : WAIT or SKIP
 *                        word1 = (VP << 8) | HP, word2 bit0 selects
 *                        (0 = WAIT, 1 = SKIP), word2 bits 15:8 = VP mask,
 *                        bits 7:1 = HP mask
 * A WAIT for an unreachable position ends the list. */

#define COPPER_HPOS_MAX 228u

static int copper_pos_match(la64m68_vaga *v, uint32_t vp, uint32_t hp,
                            uint32_t vpm, uint32_t hpm)
{
    uint32_t vpos = (v->vpos / 456u) & 0x1ffu;    /* 456 colour clocks per line */
    uint32_t hpos = v->cop_hpos;
    if ((vpos & vpm) != (vp & vpm)) return 0;
    if (vpos < vp) return 0;
    if (vpos == vp && (hpos & hpm) < (hp & hpm)) return 0;
    return 1;
}

static void vaga_copper_start(la64m68_vaga *v)
{
    uint32_t list = ((uint32_t)v->regs[REG_COP1LCH >> 1] << 16) |
                    v->regs[REG_COP1LCL >> 1];
    v->cop_pc = list & 0xfffffffeu;
    v->cop_active = 1;
    v->cop_stall = 0;
    la64m68_trace("vaga: copper started at %08x", v->cop_pc);
}

static void vaga_copper_run(la64m68_vaga *v, int steps)
{
    while (v->cop_active && steps-- > 0) {
        uint16_t w1 = la64m68_mem_read16(v->mem, v->cop_pc);
        uint16_t w2 = la64m68_mem_read16(v->mem, v->cop_pc + 2);

        if ((w1 & 1) == 0) {
            /* MOVE: word1 IS the register offset with bit0 clear (that zero
             * bit is what marks it a MOVE), word2 is the data. Shifting the
             * offset out of the high byte would have aimed every write at the
             * wrong register -- BPLCON0 (0x100) would have landed on 0x02. */
            uint32_t reg = w1 & 0xfffeu;
            if (reg < LA64M68_VAGA_SIZE)
                vaga_reg_write(v, reg, w2);
            v->cop_pc += 4;
            continue;
        }

        uint32_t vp  = (uint32_t)(w1 >> 8) & 0xffu;
        uint32_t hp  = (uint32_t)(w1 >> 1) & 0xfeu;
        int      skip = (w2 & 1) != 0;
        uint32_t vpm = (uint32_t)(w2 >> 8) & 0xffu;
        uint32_t hpm = (uint32_t)(w2 >> 1) & 0xfeu;

        /* the classic end-of-list sentinel */
        if (vp == 0xff && hp == 0xfe && !skip) {
            v->cop_active = 0;
            la64m68_trace("vaga: copper list ended at %08x", v->cop_pc);
            return;
        }
        if (!copper_pos_match(v, vp, hp, vpm, hpm)) {
            v->cop_stall = 1;                      /* resume on a later line */
            return;
        }
        v->cop_stall = 0;
        /* SKIP: on a true condition the following instruction pair is
         * skipped, so advance by two pairs instead of one. */
        v->cop_pc += skip ? 8 : 4;
    }
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

/* PAL frame length in CPU cycles (7.09 MHz / 50 Hz). The guest drives its
 * display setup from the vertical blank, so without this event Kickstart
 * never reaches BPLCON0 -- it waits for an interrupt that never comes. */
#define VAGA_FRAME_CYCLES   709379u
#define VERTB_BIT           5         /* INTREQ bit 5 = vertical blank */
#define BLIT_BIT            6         /* INTREQ bit 6 = blitter done */

void la64m68_vaga_tick(la64m68_vaga *v, int cycles)
{
    if (!v || cycles <= 0) return;
    v->vpos += (uint32_t)cycles;

    v->frame_acc += (uint32_t)cycles;
    while (v->frame_acc >= VAGA_FRAME_CYCLES) {
        v->frame_acc -= VAGA_FRAME_CYCLES;
        /* Raise VERTB unconditionally: INTREQ latches the request and
         * la64m68_vaga_ipl() already masks it against INTENA, so a disabled
         * source cannot interrupt the CPU. */
        la64m68_vaga_raise(v, VERTB_BIT);
    }

    /* copper: advance the horizontal counter and run what is runnable */
    v->cop_hpos += (uint32_t)cycles;
    while (v->cop_hpos >= COPPER_HPOS_MAX) v->cop_hpos -= COPPER_HPOS_MAX;
    if (v->cop_active) vaga_copper_run(v, 64);

    /* Blitter: a started blit runs to completion and then raises BLIT.
     * Without this the guest starts a blit and waits forever -- which is
     * exactly where Kickstart stopped before the display was enabled. */
    if (v->blit_left) {
        if (v->blit_left <= (uint32_t)cycles) {
            v->blit_left = 0;
            la64m68_vaga_raise(v, BLIT_BIT);
        } else {
            v->blit_left -= (uint32_t)cycles;
        }
    }
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

/* ---- fallback display layouts -----------------------------------------
 *
 * Both formats are the same playfield, only arranged the way the target
 * hardware wants it. Denise takes the planes of a row consecutively, the
 * Atari ST interleaves them as words. */

static int vaga_layout(la64m68_vaga *v, uint32_t *w, uint32_t *h, int *depth,
                       uint32_t *ptr, int *mod)
{
    uint16_t con0 = v->regs[R_BPLCON0 / 2];
    int d = (con0 >> 12) & 7;
    if (d < 1 || d > 6) return -1;
    uint32_t width = (con0 & 0x8000) ? 640u : 320u;
    uint16_t ds = v->regs[R_DIWSTRT / 2], de = v->regs[R_DIWSTOP / 2];
    uint32_t height = ((de >> 8) - (ds >> 8)) & 0xffu;
    if (height == 0) height = 256;
    for (int p = 0; p < 6; p++)
        ptr[p] = ((uint32_t)v->regs[(R_BPL1PTH + p * 4) / 2] << 16) |
                 v->regs[(R_BPL1PTH + p * 4 + 2) / 2];
    for (int p = 0; p < 6; p++)
        mod[p] = (int16_t)v->regs[((p & 1) ? R_BPL2MOD : R_BPL1MOD) / 2];
    *w = width; *h = height; *depth = d;
    return 0;
}

int la64m68_vaga_render_planar(la64m68_vaga *v, uint8_t *out,
                               uint32_t *w, uint32_t *h, int *depth)
{
    if (!v || !v->mem || !out) return -1;
    uint32_t width, height; int d, mod[6]; uint32_t ptr[6];
    if (vaga_layout(v, &width, &height, &d, ptr, mod) != 0) return -1;
    uint32_t row_bytes = width / 8;
    uint8_t *o = out;
    for (uint32_t y = 0; y < height; y++) {
        for (int p = 0; p < d; p++) {
            for (uint32_t i = 0; i < row_bytes; i++)
                *o++ = la64m68_mem_read8(v->mem, ptr[p] + i);
            ptr[p] += row_bytes + (uint32_t)mod[p];
        }
    }
    *w = width; *h = height; *depth = d;
    return 0;
}

int la64m68_vaga_render_st(la64m68_vaga *v, uint8_t *out,
                           uint32_t *w, uint32_t *h, int *depth)
{
    if (!v || !v->mem || !out) return -1;
    uint32_t width, height; int d, mod[6]; uint32_t ptr[6];
    if (vaga_layout(v, &width, &height, &d, ptr, mod) != 0) return -1;
    uint32_t row_bytes = width / 8;
    uint8_t *o = out;
    for (uint32_t y = 0; y < height; y++) {
        for (int p = 0; p < d; p++) {
            for (uint32_t i = 0; i < row_bytes; i++)
                *o++ = la64m68_mem_read8(v->mem, ptr[p] + i);
            ptr[p] += row_bytes + (uint32_t)mod[p];
        }
    }
    *w = width; *h = height; *depth = d;
    return 0;
}
