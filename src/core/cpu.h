#ifndef LA64M68_CPU_H
#define LA64M68_CPU_H

#include <stdint.h>
#include <stdbool.h>
#include "memory.h"
#include "plugin.h"
#include "vmmu.h"
#include "vfpu.h"

/* 68000..68060 register file. 8 data + 8 address + PC + SR + SSP/USP. */
#define LA64M68_REG_D(n) (n)
#define LA64M68_REG_A(n) (8 + (n))

/* status register bits */
#define LA64M68_SR_T   0x8000
#define LA64M68_SR_S   0x2000
#define LA64M68_SR_X   0x0010
#define LA64M68_SR_N   0x0008
#define LA64M68_SR_Z   0x0004
#define LA64M68_SR_V   0x0002
#define LA64M68_SR_C   0x0001
#define LA64M68_REG_COUNT 16

typedef struct la64m68_cpu {
    uint32_t regs[LA64M68_REG_COUNT];
    uint32_t pc;
    uint16_t sr;
    uint32_t usp;
    uint32_t ssp;
    uint32_t isp;   /* interrupt stack pointer (060) */
    uint32_t msp;   /* master stack pointer (060) */
    uint32_t vbr;
    uint32_t sfc;   /* source function code (MOVEC) */
    uint32_t dfc;   /* destination function code */
    uint32_t cacr;  /* cache control */
    uint32_t pcr;   /* processor configuration (060) */
    la64m68_vmmu vmmu;   /* 68060 pMMU state (embedded, always present) */
    la64m68_vfpu vfpu;   /* 68060 FPU state (embedded, always present) */
    la64m68_memory *mem;
    la64m68_plugin *plugin;
    uint64_t cycles;
    uint32_t fault;   /* pending bus/access fault address, 0 = none */
    int      ipl;     /* pending interrupt level 0-7 (7 = NMI), 0 = none */
} la64m68_cpu;

/* Set/clear the pending interrupt level (host devices call this). */
static inline void la64m68_cpu_ipl(la64m68_cpu *c, int level)
{
    c->ipl = level < 0 ? 0 : level > 7 ? 7 : level;
}

void la64m68_cpu_reset(la64m68_cpu *cpu, la64m68_memory *mem, la64m68_plugin *plugin);
int  la64m68_cpu_step(la64m68_cpu *cpu);
void la64m68_cpu_exception(la64m68_cpu *cpu, uint32_t vector);
void la64m68_cpu_exception_fmt(la64m68_cpu *cpu, uint32_t vector, int fmt);

#endif
