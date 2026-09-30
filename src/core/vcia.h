#ifndef LA64M68_VCIA_H
#define LA64M68_VCIA_H

#include <stdint.h>
#include "memory.h"

/* vCIA -- the two MOS 6526 Complex Interface Adapters.
 *
 * Register layout is the official Amiga one (NDK 3.9 hardware/cia.h): register
 * N sits at offset N * 0x100, so the register index is (offset >> 8) & 0xf.
 * That is not a guess -- it is what makes CIAAICR land on 0xBFED01 and not a
 * byte earlier.
 *
 *   0x000 ciapra     0x400 ciatalo   0x800 ciatodlow   0xC00 ciasdr
 *   0x100 ciaprb     0x500 ciatahi   0x900 ciatodmid   0xD00 ciaicr
 *   0x200 ciaddra    0x600 ciatblo   0xA00 ciatodhi    0xE00 ciacra
 *   0x300 ciaddrb    0x700 ciatbhi   0xB00 (unused)    0xF00 ciacrb
 *
 * Scope: enough for a guest to get its timer tick and interrupt flow going --
 * ports, timers A/B, time-of-day and the interrupt controller. The serial
 * shift register is a stub.
 *
 * CIA-A sits on an odd address (0xBFE001), CIA-B on an even one (0xBFD000). */

#define LA64M68_VCIA_A_BASE   0x00BFE001u
#define LA64M68_VCIA_B_BASE   0x00BFD000u
#define LA64M68_VCIA_SPAN     0x00001000u   /* 16 registers x 0x100 */

/* ICR bits (NDK: CIAICRB_*) */
#define VCIA_ICR_TA    0x01
#define VCIA_ICR_TB    0x02
#define VCIA_ICR_ALRM  0x04
#define VCIA_ICR_SP    0x08
#define VCIA_ICR_FLG   0x10
#define VCIA_ICR_IR    0x80    /* read: interrupt pending */

typedef struct la64m68_vcia {
    uint8_t  pra, prb, ddra, ddrb;
    uint8_t  talo, tahi, tblo, tbhi;
    uint8_t  todlo, todmid, todhi;
    uint8_t  sdr;
    uint8_t  cra, crb;
    uint8_t  icr_pending;      /* latched interrupt sources */
    uint8_t  icr_mask;         /* enabled sources */
    uint16_t ta, tb;           /* live countdown values */
    uint32_t tod;              /* live time-of-day counter */
    int      is_a;             /* 1 = CIA-A (odd base), 0 = CIA-B */
    void    *ctx;
    void   (*flag)(void *ctx); /* FLAG line, unimplemented */
} la64m68_vcia;

void la64m68_vcia_init(la64m68_vcia *c, int is_a);

/* Byte access at a guest address inside the chip's window. */
uint8_t la64m68_vcia_read(la64m68_vcia *c, uint32_t addr);
void    la64m68_vcia_write(la64m68_vcia *c, uint32_t addr, uint8_t val);

/* Advance the timers and the time-of-day counter by host cycles.
 * phi2 is 709379 Hz (PAL); the caller passes elapsed cycles at that rate. */
void la64m68_vcia_tick(la64m68_vcia *c, uint32_t cycles);

/* Interrupt level this chip wants raised (0 = none). CIA-A delivers level 6
 * (the keyboard/serial line), CIA-B level 2. */
int la64m68_vcia_ipl(const la64m68_vcia *c);

/* Router sub-memory covering the chip window. */
la64m68_memory *la64m68_vcia_memory(la64m68_vcia *c);
void            la64m68_vcia_mem_destroy(la64m68_memory *m);

#endif
