#ifndef LA64M68_VMMU_H
#define LA64M68_VMMU_H

#include <stdint.h>
#include "memory.h"

/* 68060 pMMU model. Register-level state + table walk.
 *
 * Stage 1 (this file): URP/SRP/TC/TTR state, transparent-translation check,
 * 3-level table walk (root -> pointer -> index) for 4K/8K page sizes.
 * Result reports fault classes; status bits (U/M) are tracked, not enforced.
 *
 * Stage 2 (later): push resulting VA->PA mappings into the host vm_* HAL
 * as shadow mprotect/userfaultfd regions (see 0-POOL design note).
 */

#define LA64M68_VMMU_TC_E        0x8000u   /* translation enable */
#define LA64M68_VMMU_TC_PS_4K    0x4000u   /* PS bit14 set = 4K pages */
#define LA64M68_VMMU_TC_PS_8K    0x0000u   /* PS bit14 clear = 8K pages */
#define LA64M68_VMMU_TC_PS_MASK  0x6000u

#define LA64M68_VMMU_TTR_E       0x8000u   /* TTR enable */
#define LA64M68_VMMU_TTR_S       0x0004u   /* supervisor-only match */
#define LA64M68_VMMU_TTR_CM_MASK 0x0060u   /* cache mode */
#define LA64M68_VMMU_TTR_U_MASK  0x0300u   /* user page attributes */

typedef struct la64m68_vmmu {
    uint32_t urp;      /* user root pointer (table base, phys) */
    uint32_t srp;      /* supervisor root pointer */
    uint32_t tc;       /* translation control */
    uint32_t dttr[2];  /* data transparent translation regs */
    uint32_t ittr[2];  /* instruction transparent translation regs */
    uint16_t mmusr;    /* MMU status register (PTEST result) */
} la64m68_vmmu;

/* Fault classes (negative return of la64m68_vmmu_translate). */
enum {
    LA64M68_VMMU_OK        =  0,
    LA64M68_VMMU_F_BUS     = -1,   /* descriptor fetch failed / unmapped table */
    LA64M68_VMMU_F_PAGE    = -2,   /* invalid descriptor in walk */
    LA64M68_VMMU_F_PROT    = -3,   /* write to write-protected page */
    LA64M68_VMMU_F_TCRSV   = -4    /* reserved TC page size */
};

void la64m68_vmmu_reset(la64m68_vmmu *m);

/* Translate a guest logical address to physical via the 68k tables held in
 * `mem` (physical reads). `supervisor` selects SRP vs URP + TTR set,
 * `instr` selects ITTR vs DTTR. On success returns LA64M68_VMMU_OK and
 * fills *phys. Attributes of the leaf descriptor go to *desc (may be NULL). */
int la64m68_vmmu_translate(la64m68_vmmu *m, la64m68_memory *mem,
                           uint32_t vaddr, int supervisor, int instr,
                           int write, uint32_t *phys, uint32_t *desc);

/* 68060 MMUSR bit subset we maintain on PTEST */
#define LA64M68_VMMU_MMUSR_R     0x0001u   /* resident/valid hit */
#define LA64M68_VMMU_MMUSR_W     0x0002u   /* write-protected */
#define LA64M68_VMMU_MMUSR_T     0x0004u   /* TTR transparent hit */
#define LA64M68_VMMU_MMUSR_M     0x0008u   /* modified */
#define LA64M68_VMMU_MMUSR_U     0x0040u   /* used */
#define LA64M68_VMMU_MMUSR_S     0x0080u   /* supervisor-protected */

#endif
