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
#define LA64M68_VAGA_SIZE   0x200u

typedef struct la64m68_vaga {
    uint16_t regs[LA64M68_VAGA_SIZE / 2];   /* word regs at even offsets */
    uint32_t vpos;                          /* free-running VPOS counter */
    uint16_t intreq;                        /* latched INTREQ bits */
    int      enabled;
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

#endif
