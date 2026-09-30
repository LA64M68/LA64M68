#ifndef LA64M68_VAGA_H
#define LA64M68_VAGA_H

/* Minimal Denise/AGA-style chipset fallback: custom-register window at
 * 0xDFF000. Lowest display/hardware priority -- only active when no PiS
 * passthrough is present. Not a chipset emulation; keeps register state
 * readable/writable so guest code probing the chipset does not hit open
 * bus. Real bit-true Paula/Denise/Alice timing is out of scope. */

#include <stdint.h>
#include "memory.h"

#define LA64M68_VAGA_BASE   0x00dff000u

/* custom register offsets (byte offset from the base, per the hardware
 * manual) -- shared so tests and the decoder agree on one definition */
#define LA64M68_VAGA_REG_INTREQ  0x09c
#define LA64M68_VAGA_REG_INTENA  0x09a
#define LA64M68_VAGA_REG_BPLCON0 0x100
#define LA64M68_VAGA_REG_COLOR00 0x180
#define LA64M68_VAGA_REG_BLTSIZE 0x058   /* write starts a blit */
#define LA64M68_VAGA_REG_DMACON  0x096
#define LA64M68_VAGA_REG_BLTCON0 0x040
#define LA64M68_VAGA_REG_COP1LCH 0x080
#define LA64M68_VAGA_REG_COP1LCL 0x082
#define LA64M68_VAGA_REG_COPJMP1 0x088
#define LA64M68_VAGA_REG_COPCON  0x02e
#define LA64M68_VAGA_REG_DENISEID 0x07c

/* DENISEID values. Kickstart reads this to find out which graphics chip it
 * is talking to and refuses to proceed on an unknown one -- returning 0 made
 * it retry forever, long before it ever enabled the display.
 *   0xFFFF = OCS Denise (no such register), 0xFFE0 = ECS Denise II,
 *   0xFFC0 = AGA Alice. We model an A1200, so AGA. */
#define LA64M68_VAGA_DENISEID_OCS  0xffffu
#define LA64M68_VAGA_DENISEID_ECS  0xffe0u
#define LA64M68_VAGA_DENISEID_AGA  0xffc0u
#define LA64M68_VAGA_SIZE   0x200u

typedef struct la64m68_vaga {
    uint16_t regs[LA64M68_VAGA_SIZE / 2];   /* word regs at even offsets */
    uint32_t vpos;                          /* free-running VPOS counter */
    uint32_t frame_acc;                     /* cycles toward the next VERTB */
    uint32_t blit_left;                     /* cycles until the blit is done */
    uint32_t cop_pc;                        /* copper program counter */
    int      cop_active;                    /* list is running */
    int      cop_stall;                     /* waiting on a WAIT position */
    uint32_t cop_hpos;                      /* horizontal counter, 0..227 */
    uint16_t intreq;                        /* latched INTREQ bits */
    int      enabled;
    la64m68_memory *mem;   /* guest RAM, for bitplane fetches */
} la64m68_vaga;

void la64m68_vaga_init(la64m68_vaga *v);

/* Router sub-memory covering 0xDFF000..0xDFF1FF. Caller frees with
 * la64m68_vaga_mem_destroy() (regs stay with `v`). */
la64m68_memory *la64m68_vaga_memory(la64m68_vaga *v);
void            la64m68_vaga_mem_destroy(la64m68_memory *m);

/* Raise a guest interrupt request bit (INTREQ bit -> vpos ticks service
 * it); the CPU-facing level comes from INTENA&INTREQ like on real HW. */
void la64m68_vaga_raise(la64m68_vaga *v, int bit);
/* Interrupt level to present on IPL lines (0 = none). */
int  la64m68_vaga_ipl(la64m68_vaga *v);
/* Advance the free-running vertical counter. */
void la64m68_vaga_tick(la64m68_vaga *v, int cycles);

/* ---- display decoding -------------------------------------------------
 *
 * The registers alone do not show anything: something has to turn the
 * bitplanes the guest set up into pixels. This walks BPLCON0/BPLxPT/COLORxx
 * and produces a chunky RGB frame, which the presenter chain then scales to
 * VideoCore or the Amiga/Atari fallback.
 *
 * Scope: OCS-style playfield, 1..6 bitplanes, lores and hires. HAM, EHB and
 * dual-playfield are not decoded yet and fall back to the plain index.
 */

/* The bitplane fetches read guest RAM. */
void la64m68_vaga_set_mem(la64m68_vaga *v, la64m68_memory *m);

/* Render the current playfield into `rgb24` (stride in bytes).
 * Returns 0 and stores the picture size when a display is set up,
 * -1 when the guest has not configured one. */
int la64m68_vaga_render(la64m68_vaga *v, uint8_t *rgb24, uint32_t stride,
                        uint32_t *w, uint32_t *h);

/* Amiga Denise output: the playfield in the hardware's own bitplane layout
 * (all planes of a row together), which is what the fallback display needs.
 * `out` must hold width/8 * height * depth bytes. */
int la64m68_vaga_render_planar(la64m68_vaga *v, uint8_t *out,
                               uint32_t *w, uint32_t *h, int *depth);

/* Atari ST output: the same planes interleaved as 16-bit words per row. */
int la64m68_vaga_render_st(la64m68_vaga *v, uint8_t *out,
                           uint32_t *w, uint32_t *h, int *depth);

#endif
