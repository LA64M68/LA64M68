#define _POSIX_C_SOURCE 200809L

#include "vcia.h"
#include "debug.h"
#include <stdlib.h>
#include <string.h>

/* Register index inside the chip window. The NDK layout puts register N at
 * offset N * 0x100, so this is a shift and a mask -- nothing clever. */
static uint32_t reg_of(uint32_t addr)
{
    return (addr >> 8) & 0x0fu;
}

void la64m68_vcia_init(la64m68_vcia *c, int is_a)
{
    memset(c, 0, sizeof(*c));
    c->is_a = is_a;
}

/* Raise ICR source `bit`; the chip only interrupts when it is also masked in. */
static void raise(la64m68_vcia *c, uint8_t bit)
{
    c->icr_pending |= bit;
}

uint8_t la64m68_vcia_read(la64m68_vcia *c, uint32_t addr)
{
    if (!c) return 0xff;
    switch (reg_of(addr)) {
    case 0x0: return (uint8_t)(c->pra | (uint8_t)~c->ddra);  /* undriven bits float high */
    case 0x1: return (uint8_t)(c->prb | (uint8_t)~c->ddrb);
    case 0x2: return c->ddra;
    case 0x3: return c->ddrb;
    case 0x4: return (uint8_t)(c->ta & 0xff);
    case 0x5: return (uint8_t)(c->ta >> 8);
    case 0x6: return (uint8_t)(c->tb & 0xff);
    case 0x7: return (uint8_t)(c->tb >> 8);
    case 0x8: return c->todlo;
    case 0x9: return c->todmid;
    case 0xa: return c->todhi;
    case 0xb: return 0xff;                      /* unused register */
    case 0xc: return c->sdr;
    case 0xd: {
        /* ICR read: report the sources, and CLEAR them -- that is what makes
         * the handler's acknowledge work on real hardware. */
        uint8_t v = c->icr_pending;
        if (c->icr_pending & c->icr_mask) v |= VCIA_ICR_IR;
        c->icr_pending = 0;
        return v;
    }
    case 0xe: return c->cra;
    case 0xf: return c->crb;
    }
    return 0xff;
}

void la64m68_vcia_write(la64m68_vcia *c, uint32_t addr, uint8_t val)
{
    if (!c) return;
    switch (reg_of(addr)) {
    case 0x0: c->pra = val; break;
    case 0x1: c->prb = val; break;
    case 0x2: c->ddra = val; break;
    case 0x3: c->ddrb = val; break;
    case 0x4: c->talo = val; break;
    case 0x5:
        c->tahi = val;
        c->ta = (uint16_t)((c->tahi << 8) | c->talo);   /* latches on TAHI */
        break;
    case 0x6: c->tblo = val; break;
    case 0x7:
        c->tbhi = val;
        c->tb = (uint16_t)((c->tbhi << 8) | c->tblo);
        break;
    case 0x8: c->todlo = val; break;
    case 0x9: c->todmid = val; break;
    case 0xa: c->todhi = val; break;
    case 0xb: break;
    case 0xc: c->sdr = val; break;
    case 0xd:
        /* bit7 selects set (1) or clear (0) for the mask bits */
        if (val & VCIA_ICR_IR) c->icr_mask |= (uint8_t)(val & 0x7f);
        else                   c->icr_mask &= (uint8_t)~val;
        break;
    case 0xe:
        c->cra = val;
        if (val & 0x10) {                        /* LOAD: force the latch in */
            c->ta = (uint16_t)((c->tahi << 8) | c->talo);
            c->cra &= (uint8_t)~0x10;
        }
        break;
    case 0xf:
        c->crb = val;
        if (val & 0x10) {
            c->tb = (uint16_t)((c->tbhi << 8) | c->tblo);
            c->crb &= (uint8_t)~0x10;
        }
        break;
    }
}

/* Run one timer down by `cycles`, raising its source on every underflow.
 * One-shot mode stops after the first. */
static void timer_run(la64m68_vcia *c, uint16_t *t, uint8_t *ctrlp,
                      uint8_t icr_bit, uint32_t cycles)
{
    uint8_t ctrl = *ctrlp;
    if (!(ctrl & 0x01)) return;                  /* not started */
    uint16_t latch = (uint16_t)((icr_bit == VCIA_ICR_TA)
                                ? ((c->tahi << 8) | c->talo)
                                : ((c->tbhi << 8) | c->tblo));
    int oneshot = (ctrl & 0x08) != 0;

    while (cycles > 0) {
        if (*t == 0) *t = latch ? latch : 0xffff;
        uint32_t step = *t <= cycles ? *t : cycles;
        *t = (uint16_t)(*t - step);
        cycles -= step;
        if (*t == 0) {
            raise(c, icr_bit);
            if (oneshot) { *ctrlp &= (uint8_t)~0x01; break; }
            *t = latch ? latch : 0xffff;
        }
    }
}

void la64m68_vcia_tick(la64m68_vcia *c, uint32_t cycles)
{
    if (!c) return;
    timer_run(c, &c->ta, &c->cra, VCIA_ICR_TA, cycles);
    timer_run(c, &c->tb, &c->crb, VCIA_ICR_TB, cycles);

    /* time of day: free running, one tick per timer underflow is a
     * simplification -- enough for code that only wants progression */
    c->tod = (c->tod + cycles / 10) & 0xffffffu;
    c->todlo  = (uint8_t)(c->tod & 0xff);
    c->todmid = (uint8_t)((c->tod >> 8) & 0xff);
    c->todhi  = (uint8_t)((c->tod >> 16) & 0xff);
}

int la64m68_vcia_ipl(const la64m68_vcia *c)
{
    if (!c) return 0;
    if (!(c->icr_pending & c->icr_mask)) return 0;
    return c->is_a ? 6 : 2;                      /* CIA-A level 6, CIA-B level 2 */
}

/* ---- router window ---------------------------------------------------- */

typedef struct {
    la64m68_memory base;
    la64m68_vcia *c;
} vcia_mem;

static uint8_t  cr8 (void *p, uint32_t a) { return la64m68_vcia_read(((vcia_mem *)p)->c, a); }
static uint16_t cr16(void *p, uint32_t a) { return cr8(p, a); }
static uint32_t cr32(void *p, uint32_t a) { return cr8(p, a); }
static void     cw8 (void *p, uint32_t a, uint8_t v)  { la64m68_vcia_write(((vcia_mem *)p)->c, a, v); }
static void     cw16(void *p, uint32_t a, uint16_t v) { cw8(p, a, (uint8_t)v); }
static void     cw32(void *p, uint32_t a, uint32_t v) { cw8(p, a, (uint8_t)v); }

la64m68_memory *la64m68_vcia_memory(la64m68_vcia *c)
{
    vcia_mem *m = calloc(1, sizeof(*m));
    if (!m) return NULL;
    m->c = c;
    m->base.ctx = m;
    m->base.read8 = cr8;  m->base.read16 = cr16; m->base.read32 = cr32;
    m->base.write8 = cw8; m->base.write16 = cw16; m->base.write32 = cw32;
    return &m->base;
}

void la64m68_vcia_mem_destroy(la64m68_memory *m)
{
    free(m);
}
