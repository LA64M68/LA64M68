#define _POSIX_C_SOURCE 200809L

#include "cpu.h"
#include "opts.h"
#include "memory.h"
#include "vmmu.h"
#include "vfpu.h"
#include "vm.h"
#include "vrtg.h"
#include "vnic.h"
#include "vhid.h"
#include "vaga.h"
#include "pis.h"
#include "vblk.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

static void test_reset_step(void)
{
    la64m68_memory *mem = la64m68_ram_memory_create(0, 65536);
    assert(mem);

    la64m68_mem_write32(mem, 0, 0x00002000);
    la64m68_mem_write32(mem, 4, 0x00001000);

    /* program at 0x1000:
     *   MOVEQ #5,D0          7005
     *   MOVE.L #$12345678,D1 223C 1234 5678
     *   ADDQ.L #3,D0         5680
     *   MOVE.W D0,$2000      33C0 2000
     *   MOVEQ #4,D2          7404        ; loop counter
     *   MOVEQ #0,D3          7600        ; sum
     * loop:
     *   ADD.L D0,D3          D680        ; D3 += 8
     *   DBRA D2,loop         51CA FFFC   ; 4 iterations
     *   SWAP D0              4840
     *   MOVEM.L D0-D1,-(A7)  48E7 C000   ; push D0,D1 (predec-reversed mask)
     *   CLR.L D0             4280
     *   MOVEM.L (A7)+,D0-D1  4CDF 0003   ; pop D0,D1
     *   STOP #$2000          4E72 2000
     */
    la64m68_mem_write16(mem, 0x1000, 0x7005);
    la64m68_mem_write16(mem, 0x1002, 0x223c);
    la64m68_mem_write32(mem, 0x1004, 0x12345678);
    la64m68_mem_write16(mem, 0x1008, 0x5680);
    la64m68_mem_write16(mem, 0x100a, 0x31c0);   /* MOVE.W D0,$2000.W (dst ea reg/mode swapped) */
    la64m68_mem_write16(mem, 0x100c, 0x2000);
    la64m68_mem_write16(mem, 0x100e, 0x7403);   /* DBRA runs N+1: 3 -> 4 iters */
    la64m68_mem_write16(mem, 0x1010, 0x7600);
    la64m68_mem_write16(mem, 0x1012, 0xd680);   /* ADD.L D0,D3 */
    la64m68_mem_write16(mem, 0x1014, 0x51ca);   /* DBRA D2 */
    la64m68_mem_write16(mem, 0x1016, 0xfffc);   /* disp -4 -> 0x1012 */
    la64m68_mem_write16(mem, 0x1018, 0x4840);   /* SWAP D0 */
    la64m68_mem_write16(mem, 0x101a, 0x48e7);   /* MOVEM.L regs->-(A7) */
    la64m68_mem_write16(mem, 0x101c, 0xc000);   /* mask D0,D1 (predec order) */
    la64m68_mem_write16(mem, 0x101e, 0x4280);   /* CLR.L D0 */
    la64m68_mem_write16(mem, 0x1020, 0x4cdf);   /* MOVEM.L (A7)+->regs */
    la64m68_mem_write16(mem, 0x1022, 0x0003);   /* mask D0,D1 */
    /* FMOVE.L #42,FP0  (f200|imm-ea  ext: rmode2 fmt0 dn0 move) */
    la64m68_mem_write16(mem, 0x1024, 0xf23c);   /* FPU ea = #imm */
    la64m68_mem_write16(mem, 0x1026, 0x4000);   /* ext: rmode2 fmt0 dst0 FMOVE */
    la64m68_mem_write32(mem, 0x1028, 42);
    /* FMOVE.L FP0,$2100 (rmode3 fmt0 dn0) */
    la64m68_mem_write16(mem, 0x102c, 0xf238);   /* ea = abs.w */
    la64m68_mem_write16(mem, 0x102e, 0x6000);   /* ext: rmode3 fmt0 dn0 */
    la64m68_mem_write16(mem, 0x1030, 0x2100);
    la64m68_mem_write16(mem, 0x1032, 0x283c);   /* MOVE.L #$e000,D4 */
    la64m68_mem_write32(mem, 0x1034, 0x0000e000);
    la64m68_mem_write16(mem, 0x1038, 0x4e7b);   /* MOVEC D4,VBR */
    la64m68_mem_write16(mem, 0x103a, 0x4801);
    la64m68_mem_write16(mem, 0x103c, 0x4e7a);   /* MOVEC VBR,D5 */
    la64m68_mem_write16(mem, 0x103e, 0x5801);
    la64m68_mem_write16(mem, 0x1040, 0x7c25);   /* MOVEQ #0x25,D6 (BCD 25) */
    la64m68_mem_write16(mem, 0x1042, 0x7e39);   /* MOVEQ #0x39,D7 (BCD 39) */
    la64m68_mem_write16(mem, 0x1044, 0xcf06);   /* ABCD D6,D7 -> 0x64 BCD */
    la64m68_mem_write16(mem, 0x1046, 0x51fc);   /* TRAPF (never traps) */
    la64m68_mem_write16(mem, 0x1048, 0x45f9);   /* LEA $3000,A2 */
    la64m68_mem_write32(mem, 0x104a, 0x00003000);
    la64m68_mem_write16(mem, 0x104e, 0x47f9);   /* LEA $3100,A3 */
    la64m68_mem_write32(mem, 0x1050, 0x00003100);
    la64m68_mem_write16(mem, 0x1054, 0xf623);   /* MOVE16 (A2)+,(A3)+ */
    la64m68_mem_write16(mem, 0x1056, 0x2000);   /* ext: Ax=A2 */
    la64m68_mem_write16(mem, 0x1058, 0x49f9);   /* LEA $3300,A4 */
    la64m68_mem_write32(mem, 0x105a, 0x00003300);
    la64m68_mem_write16(mem, 0x105e, 0x4bf9);   /* LEA $3400,A5 */
    la64m68_mem_write32(mem, 0x1060, 0x00003400);
    la64m68_mem_write16(mem, 0x1064, 0x303c);   /* MOVE.W #$00AA,D0 */
    la64m68_mem_write16(mem, 0x1066, 0x00aa);
    la64m68_mem_write16(mem, 0x1068, 0x323c);   /* MOVE.W #$00BB,D1 */
    la64m68_mem_write16(mem, 0x106a, 0x00bb);
    la64m68_mem_write16(mem, 0x106c, 0x343c);   /* MOVE.W #$7777,D2 */
    la64m68_mem_write16(mem, 0x106e, 0x7777);
    la64m68_mem_write16(mem, 0x1070, 0x363c);   /* MOVE.W #$8888,D3 */
    la64m68_mem_write16(mem, 0x1072, 0x8888);
    /* CAS2.W D0:D1,D2:D3,(A4):(A5) — mem matches -> stores land */
    la64m68_mem_write16(mem, 0x1074, 0x0cfc);
    la64m68_mem_write16(mem, 0x1076, 0xd043);   /* ext1 = pair2: Rn2=A5 Dc2=D1 Du2=D3 */
    la64m68_mem_write16(mem, 0x1078, 0xc002);   /* ext2 = pair1: Rn1=A4 Dc1=D0 Du1=D2 */
    la64m68_mem_write16(mem, 0x107a, 0x7c15);   /* MOVEQ #0x15,D6 */
    /* CMP2.B ($3200).W,D6 (D6=0x15 in 0x10..0x20 -> C=0,Z=0) */
    la64m68_mem_write16(mem, 0x107c, 0x00f8);
    la64m68_mem_write16(mem, 0x107e, 0x6000);   /* Dn=D6 */
    la64m68_mem_write16(mem, 0x1080, 0x3200);
    la64m68_mem_write16(mem, 0x1082, 0x42c4);   /* MOVE CCR,D4 */
    /* scaled brief index: MOVE.L $10(A6,D6.L*4),D5 ; A6=0x3500 D6=4
     * -> EA = 0x3500+0x10+0x10 = 0x3520 */
    la64m68_mem_write16(mem, 0x1084, 0x4df9);   /* LEA $3500,A6 */
    la64m68_mem_write32(mem, 0x1086, 0x00003500);
    la64m68_mem_write16(mem, 0x108a, 0x7c04);   /* MOVEQ #4,D6 */
    la64m68_mem_write16(mem, 0x108c, 0x2a36);   /* MOVE.L d8(A6,Xn),D5 */
    la64m68_mem_write16(mem, 0x108e, 0x6c10);   /* ext: D6.L scale4 disp 0x10 */
    /* full extension, pre-indexed + word outer disp:
     * MOVE.L ([0x10,A6,D6.L],$08),D2 -> [[0x3514]] + 8 = [0x3608] */
    la64m68_mem_write16(mem, 0x1090, 0x2436);   /* MOVE.L ea,D2 */
    la64m68_mem_write16(mem, 0x1092, 0x6922);   /* ext: D6.L full bd=W iis=2 */
    la64m68_mem_write16(mem, 0x1094, 0x0010);   /* bd = 0x10 */
    la64m68_mem_write16(mem, 0x1096, 0x0008);   /* od = 0x08 */
    la64m68_mem_write16(mem, 0x1098, 0x4e72);   /* STOP */
    la64m68_mem_write16(mem, 0x109a, 0x2000);
    la64m68_mem_write32(mem, 0x3000, 0x11223344);
    la64m68_mem_write32(mem, 0x3004, 0x55667788);
    la64m68_mem_write16(mem, 0x3300, 0x00aa);   /* CAS2 operand 1 */
    la64m68_mem_write16(mem, 0x3400, 0x00bb);   /* CAS2 operand 2 */
    la64m68_mem_write16(mem, 0x3200, 0x1020);   /* CMP2 bounds lo=0x10 hi=0x20 */
    la64m68_mem_write32(mem, 0x3520, 0xcafebabe);   /* scaled-index target */
    la64m68_mem_write32(mem, 0x3514, 0x00003600);   /* full-ext inner pointer */
    la64m68_mem_write32(mem, 0x3608, 0x12345678);   /* full-ext target */

    la64m68_cpu cpu;
    la64m68_cpu_reset(&cpu, mem, NULL);
    assert(cpu.ssp == 0x00002000);
    assert(cpu.pc  == 0x00001000);

    for (int i = 0; i < 120; i++) {
        if (la64m68_cpu_step(&cpu) != 0) break;
    }
    assert(cpu.regs[0] == 0x000800aa);           /* SWAP 8, low word CAS2 cmp */
    assert(cpu.regs[1] == 0x123400bb);           /* low word = CAS2 cmp 2 */
    assert(cpu.regs[3] == 0x00008888);           /* CAS2 update operand */
    assert(la64m68_mem_read16(mem, 0x2000) == 8);
    assert(la64m68_mem_read32(mem, 0x2100) == 42);   /* FMOVE.L FP0->mem */
    assert(cpu.vbr == 0x0000e000);                   /* MOVEC D4,VBR */
    assert(cpu.regs[5] == 0xcafebabe);               /* scaled d8(A6,D6.L*4) */
    assert(cpu.regs[2] == 0x12345678);               /* full-ext pre-indexed */
    assert((cpu.regs[7] & 0xff) == 0x64);            /* ABCD 25+39=64 BCD */
    assert(la64m68_mem_read32(mem, 0x3100) == 0x11223344);  /* MOVE16 */
    assert(la64m68_mem_read32(mem, 0x3104) == 0x55667788);
    assert(cpu.regs[10] == 0x3010);                  /* A2 postinc */
    assert(la64m68_mem_read16(mem, 0x3300) == 0x7777);   /* CAS2 store 1 */
    assert(la64m68_mem_read16(mem, 0x3400) == 0x8888);   /* CAS2 store 2 */
    assert((cpu.regs[4] & (LA64M68_SR_Z | LA64M68_SR_C)) == 0); /* CMP2 in-range */
    assert(la64m68_fp80_to_double(cpu.vfpu.fp[0]) == 42.0);
    assert(cpu.sr == 0x2000);                    /* STOP loaded SR */

    la64m68_ram_memory_destroy(mem);
    printf("test_core: reset/step ok\n");
}

static void test_vmmu(void)
{
    /* 64 KiB RAM bus doubles as phys memory holding the page tables. */
    la64m68_memory *mem = la64m68_ram_memory_create(0, 65536);
    assert(mem);
    la64m68_vmmu v;
    la64m68_vmmu_reset(&v);

    /* TC disabled -> 1:1 passthrough. */
    uint32_t phys = 0;
    assert(la64m68_vmmu_translate(&v, mem, 0x12345, 1, 0, 0, &phys, NULL) == LA64M68_VMMU_OK);
    assert(phys == 0x12345);

    /* TTR enabled, TA = 0x00 / TAM = 0x00 -> transparent 1:1 for the whole
     * 0x00xxxxxx block, walk bypassed. This used to write 0x80000000 as an
     * "enable" bit, but bits 31:24 are TA, so that was TA = 0x80; it only
     * ever matched because the mask compare was vacuous (hardening #11). */
    v.tc = LA64M68_VMMU_TC_E | LA64M68_VMMU_TC_PS_4K;
    v.dttr[0] = LA64M68_VMMU_TTR_E;
    assert(la64m68_vmmu_translate(&v, mem, 0x12345, 1, 0, 0, &phys, NULL) == LA64M68_VMMU_OK);
    assert(phys == 0x12345);
    v.dttr[0] = 0;

    /* 3-level walk, 4K pages:
     *   URP -> root[va>>25] -> ptr[(va>>18)&0x7f] -> page[(va>>12)&0x3f]
     * Layout: root@0x4000, ptr@0x4800, pagetab@0x5000. */
    v.srp = 0x00004000;
    v.urp = 0x00004000;
    uint32_t va = 0x00c12345;               /* r=1, p=4, i=18, off=0x345 */
    uint32_t r = (va >> 25) & 0x7f;
    uint32_t p = (va >> 18) & 0x7f;
    uint32_t i = (va >> 12) & 0x3f;
    la64m68_mem_write32(mem, 0x4000 + r * 4, 0x00004802); /* DT=table */
    la64m68_mem_write32(mem, 0x4800 + p * 4, 0x00005002); /* DT=table */
    la64m68_mem_write32(mem, 0x5000 + i * 8 + 4, 0x00020001); /* DT=page, base 0x20000 */

    assert(la64m68_vmmu_translate(&v, mem, va, 1, 0, 0, &phys, NULL) == LA64M68_VMMU_OK);
    assert(phys == (0x20000 | (va & 0xfff)));

    /* Read hit sets U; write hit sets U|M in the leaf descriptor. */
    uint32_t leaf = la64m68_mem_read32(mem, 0x5000 + i * 8 + 4);
    assert(leaf & 0x8u);                        /* U set by the read above */
    assert(la64m68_vmmu_translate(&v, mem, va, 1, 0, 1, &phys, NULL) == LA64M68_VMMU_OK);
    leaf = la64m68_mem_read32(mem, 0x5000 + i * 8 + 4);
    assert((leaf & 0x18u) == 0x18u);            /* U|M after write */

    /* Write-protected page: read ok, write -> protection fault. */
    la64m68_mem_write32(mem, 0x5000 + i * 8 + 4, 0x00020005); /* DT=page, W */
    assert(la64m68_vmmu_translate(&v, mem, va, 1, 0, 0, &phys, NULL) == LA64M68_VMMU_OK);
    assert(la64m68_vmmu_translate(&v, mem, va, 1, 0, 1, &phys, NULL) == LA64M68_VMMU_F_PROT);

    /* Invalid leaf -> page fault. */
    la64m68_mem_write32(mem, 0x5000 + i * 8 + 4, 0x00000000);
    assert(la64m68_vmmu_translate(&v, mem, va, 1, 0, 0, &phys, NULL) == LA64M68_VMMU_F_PAGE);

    la64m68_ram_memory_destroy(mem);
    printf("test_core: vmmu ok\n");
}

static void test_vfpu(void)
{
    la64m68_vfpu f;
    la64m68_vfpu_reset(&f);

    double vals[] = { 0.0, -0.0, 1.0, -1.5, 3.141592653589793, 1e300, 1e-300 };
    for (unsigned i = 0; i < sizeof(vals) / sizeof(vals[0]); i++) {
        la64m68_fp80 x = la64m68_fp80_from_double(vals[i]);
        double back = la64m68_fp80_to_double(x);
        assert(back == vals[i] || (signbit(back) == signbit(vals[i]) && back == vals[i]));
        /* pack/unpack round-trip */
        uint8_t img[12];
        la64m68_fp80_pack(x, img);
        la64m68_fp80 y = la64m68_fp80_unpack(img);
        assert(y.se == x.se && y.m == x.m);
    }

    la64m68_fp80 inf = la64m68_fp80_from_double(INFINITY);
    assert(la64m68_fp80_to_double(inf) == INFINITY);
    assert(isnan(la64m68_fp80_to_double(la64m68_fp80_from_double(NAN))));

    /* packed-decimal round trip: 1234.5 -> 96-bit packed -> fp80 */
    uint8_t pb[12];
    la64m68_fp80_to_packed(la64m68_fp80_from_double(1234.5), 0, pb);
    double pv = la64m68_fp80_to_double(la64m68_fp80_from_packed(pb));
    assert(fabs(pv - 1234.5) < 1e-9);
    la64m68_fp80_to_packed(la64m68_fp80_from_double(-6.022e23), 0, pb);
    pv = la64m68_fp80_to_double(la64m68_fp80_from_packed(pb));
    assert(fabs(pv / -6.022e23 - 1.0) < 1e-12);
    /* sign + exponent field check: -6.022e23 -> dw1 bit31+bit30 clear,
     * exp digits 0,2,3 in bits 27-16 */
    assert((pb[0] & 0x80) && !(pb[0] & 0x40));
    assert(((pb[0] & 0xf) == 0) && (pb[1] == 0x23));

    printf("test_core: vfpu ok\n");
}

static volatile int trap_count;
static int trap_cb(void *ctx, uintptr_t guest_addr, int write, void *pc)
{
    (void)write; (void)pc;
    trap_count++;
    /* resolve: make region writable again */
    la64m68_vm *vm = ctx;
    size_t off = guest_addr & ~((size_t)la64m68_vm_pagesize() - 1);
    la64m68_vm_protect(vm, off, (size_t)la64m68_vm_pagesize(),
                       LA64M68_VM_PROT_RW);
    return 0;
}

static void test_vm(void)
{
    la64m68_vm *vm = la64m68_vm_create(64 * 1024);
    assert(vm);
    assert(la64m68_vm_pagesize() >= 4096);
    uint8_t *base = la64m68_vm_base(vm);
    assert(base);

    base[0] = 42;
    assert(base[0] == 42);

    assert(la64m68_vm_trap_region(vm, 0x2000, 0x1000, trap_cb, vm) == 0);
    assert(la64m68_vm_protect(vm, 0x2000, 0x1000, LA64M68_VM_PROT_NONE) == 0);

    /* trap_count starts 0; keep no reset here so -O3 cannot reorder it
     * across the faulting store below */
    *(volatile uint8_t *)(base + 0x2000) = 7; /* fault -> cb re-protects -> retry */
    assert(*(volatile uint8_t *)(base + 0x2000) == 7);
    assert(trap_count >= 1);

    la64m68_vm_destroy(vm);
    printf("test_core: vm ok\n");
}

static void test_router(void)
{
    la64m68_memory *chip = la64m68_ram_memory_create(0, 0x200000);
    la64m68_memory *zr   = la64m68_ram_memory_create(0x400000, 0x100000);
    la64m68_memory *rt   = la64m68_router_create();
    assert(chip && zr && rt);

    /* region mix: chip RAM + passthrough stand-in at Zorro space */
    assert(la64m68_router_add(rt, 0x000000, 0x200000, chip) == 0);
    assert(la64m68_router_add(rt, 0x400000, 0x100000, zr) == 0);

    la64m68_mem_write32(rt, 0x1000, 0xdeadbeef);
    la64m68_mem_write32(rt, 0x400010, 0xcafebabe);
    assert(la64m68_mem_read32(rt, 0x1000) == 0xdeadbeef);
    assert(la64m68_mem_read32(rt, 0x400010) == 0xcafebabe);
    /* hole -> 0, no crash */
    assert(la64m68_mem_read32(rt, 0x300000) == 0);

    la64m68_router_destroy(rt);
    la64m68_ram_memory_destroy(chip);
    la64m68_ram_memory_destroy(zr);
    printf("test_core: router ok\n");
}

static void test_present_cb(void *ctx, const void *pixels,
                            uint32_t w, uint32_t h, int bpp)
{
    (void)pixels;
    *(int *)ctx += (int)(w + h + bpp);
}

static void test_vrtg(void)
{
    la64m68_memory *ram = la64m68_ram_memory_create(0, 1 << 20);
    la64m68_memory *rt  = la64m68_router_create();
    la64m68_vrtg v;
    la64m68_vrtg_init(&v, ram, LA64M68_VRTG_FB_BASE);
    la64m68_memory *regs = la64m68_vrtg_regs_memory(&v);
    assert(la64m68_router_add(rt, 0x000000, 1 << 20, ram) == 0);
    assert(la64m68_router_add(rt, LA64M68_VRTG_REG_BASE, 0x100, regs) == 0);

    /* guest writes mode + enable + flip through the register window */
    la64m68_mem_write32(rt, LA64M68_VRTG_REG_BASE + VRTG_REG_WIDTH, 320);
    la64m68_mem_write32(rt, LA64M68_VRTG_REG_BASE + VRTG_REG_HEIGHT, 200);
    la64m68_mem_write32(rt, LA64M68_VRTG_REG_BASE + VRTG_REG_BPP, 16);
    la64m68_mem_write32(rt, LA64M68_VRTG_REG_BASE + VRTG_REG_CTRL, 1);
    assert(v.width == 320 && v.height == 200 && v.bpp == 16 && v.enabled);
    assert(la64m68_mem_read32(rt, LA64M68_VRTG_REG_BASE + VRTG_REG_BPP) == 16);

    static int presented;
    v.present_ctx = &presented;
    v.present = test_present_cb;
    la64m68_mem_write32(rt, LA64M68_VRTG_REG_BASE + VRTG_REG_FLIP, 1);
    assert(presented == 320 + 200 + 16);

    la64m68_router_destroy(rt);
    la64m68_vrtg_regs_destroy(regs);
    la64m68_ram_memory_destroy(ram);
    printf("test_core: vrtg ok\n");
}

static int test_tx_cb(void *ctx, const void *frame, size_t len)
{
    (void)frame;
    *(int *)ctx += (int)len;
    return (int)len;
}

static void test_vnic_vhid(void)
{
    la64m68_memory *ram = la64m68_ram_memory_create(0, 1 << 20);
    la64m68_memory *rt  = la64m68_router_create();
    assert(la64m68_router_add(rt, 0, 1 << 20, ram) == 0);

    /* --- vNIC --- */
    la64m68_vnic n;
    la64m68_vnic_init(&n, ram);
    la64m68_memory *nregs = la64m68_vnic_regs_memory(&n);
    assert(la64m68_router_add(rt, LA64M68_VNIC_REG_BASE, 0x100, nregs) == 0);
    la64m68_mem_write32(rt, LA64M68_VNIC_REG_BASE + VNIC_REG_CTRL, 1);
    la64m68_mem_write32(rt, LA64M68_VNIC_REG_BASE + VNIC_REG_TXBUF, 0x4000);
    assert((la64m68_mem_read32(rt, LA64M68_VNIC_REG_BASE + VNIC_REG_STATUS) & 1) == 1);

    static int txed;
    n.tx_ctx = &txed;
    n.tx = test_tx_cb;
    la64m68_mem_write32(ram, 0x4000, 0xdeadbeef);
    la64m68_mem_write32(rt, LA64M68_VNIC_REG_BASE + VNIC_REG_TXLEN, 60);
    assert(txed == 60);

    /* host->guest rx */
    uint8_t pkt[64];
    memset(pkt, 0xa5, sizeof(pkt));
    la64m68_mem_write32(rt, LA64M68_VNIC_REG_BASE + VNIC_REG_RXBUF, 0x6000);
    la64m68_vnic_rx(&n, pkt, sizeof(pkt));
    assert(la64m68_mem_read32(rt, LA64M68_VNIC_REG_BASE + VNIC_REG_RXSTAT) == 64);
    assert(la64m68_mem_read8(ram, 0x6000) == 0xa5);

    /* --- vHID --- */
    la64m68_vhid h;
    la64m68_vhid_init(&h);
    la64m68_memory *hregs = la64m68_vhid_regs_memory(&h);
    assert(la64m68_router_add(rt, LA64M68_VHID_REG_BASE, 0x100, hregs) == 0);
    la64m68_mem_write32(rt, LA64M68_VHID_REG_BASE + VHID_REG_CTRL, 1);
    la64m68_vhid_push_key(&h, 0x20, 1);
    la64m68_vhid_push_button(&h, 0, 1);
    assert(la64m68_mem_read32(rt, LA64M68_VHID_REG_BASE + VHID_REG_STATUS) == 1);
    uint32_t ev = la64m68_mem_read32(rt, LA64M68_VHID_REG_BASE + VHID_REG_POP);
    assert((ev >> 24) == 1 && ((ev >> 16) & 0xff) == 0x20 && (ev & 1) == 1);
    ev = la64m68_mem_read32(rt, LA64M68_VHID_REG_BASE + VHID_REG_POP);
    assert((ev >> 24) == 3);
    assert(la64m68_mem_read32(rt, LA64M68_VHID_REG_BASE + VHID_REG_POP) == 0xffffffff);

    la64m68_router_destroy(rt);
    la64m68_vnic_regs_destroy(nregs);
    la64m68_vhid_regs_destroy(hregs);
    la64m68_ram_memory_destroy(ram);
    printf("test_core: vnic/vhid ok\n");
}

static void test_ipl(void)
{
    la64m68_memory *mem = la64m68_ram_memory_create(0, 65536);
    assert(mem);
    la64m68_mem_write32(mem, 0, 0x00002000);       /* SSP */
    la64m68_mem_write32(mem, 4, 0x00001000);       /* PC */
    la64m68_mem_write32(mem, 25 * 4, 0x00003000);  /* autovector 1 handler */
    la64m68_mem_write16(mem, 0x1000, 0x4e71);      /* NOP */
    la64m68_mem_write16(mem, 0x3000, 0x4e73);      /* RTE */

    la64m68_cpu cpu;
    la64m68_cpu_reset(&cpu, mem, NULL);
    /* masked: SR mask (from reset vector SR=0 typically 0x2700? here 0) */
    cpu.sr = 0x0700;            /* mask everything -> no interrupt */
    la64m68_cpu_ipl(&cpu, 1);
    la64m68_cpu_step(&cpu);     /* executes NOP, no interrupt */
    assert(cpu.pc == 0x1002);

    cpu.sr = 0x2000;            /* supervisor, mask 0 */
    la64m68_cpu_ipl(&cpu, 1);
    la64m68_cpu_step(&cpu);     /* takes autovector 25 */
    assert(cpu.pc == 0x3000);
    assert((cpu.sr & 0x0700) == 0x0100); /* mask raised to level */
    la64m68_cpu_step(&cpu);     /* RTE -> back to 0x1002 */
    assert(cpu.pc == 0x1002);
    assert(cpu.sr == 0x2000);

    la64m68_ram_memory_destroy(mem);
    printf("test_core: ipl ok\n");
}

static void test_vaga(void)
{
    la64m68_vaga v;
    la64m68_vaga_init(&v);
    la64m68_memory *m = la64m68_vaga_memory(&v);
    assert(m);

    /* VPOSR nonzero (board id), INTENAR starts 0 */
    assert(la64m68_mem_read16(m, 0x04) != 0);
    assert(la64m68_mem_read16(m, 0x1c) == 0);

    /* INTENA: set VERTB (bit5) via set-bit protocol */
    la64m68_mem_write16(m, 0x9a, 0x8020);
    assert(la64m68_mem_read16(m, 0x1c) & 0x20);

    /* raise VERTB -> level 3 */
    la64m68_vaga_raise(&v, 5);
    assert(la64m68_vaga_ipl(&v) == 3);
    /* clear INTREQ -> level 0 */
    la64m68_mem_write16(m, 0x9c, 0x0020);
    assert(la64m68_vaga_ipl(&v) == 0);

    /* higher level wins: EXTER(13)=lvl6 vs TBE(0)=lvl1 */
    la64m68_mem_write16(m, 0x9a, 0xa001);   /* enable EXTER+TBE */
    la64m68_mem_write16(m, 0x9c, 0xa001);   /* raise both */
    assert(la64m68_vaga_ipl(&v) == 6);

    la64m68_vaga_mem_destroy(m);
    printf("test_core: vaga ok\n");
}

/* Regression cover for the 2026-09-30 hardening pass
 * (0-POOL/notes/2026-09-30-code-review-haertung.md). */
static void test_hardening(void)
{
    /* 1. read-modify-write with d16(An) must consume the extension word
     * exactly once. Before the fix ea_read()+ea_write() fetched it twice:
     * the write went to a wrong address and the PC overshot by 2. */
    {
        la64m68_memory *mem = la64m68_ram_memory_create(0, 65536);
        assert(mem);
        la64m68_mem_write32(mem, 0, 0x00002000);
        la64m68_mem_write32(mem, 4, 0x00001000);
        la64m68_mem_write16(mem, 0x1000, 0x207c);          /* MOVEA.L #$2000,A0 */
        la64m68_mem_write32(mem, 0x1002, 0x00002000);
        la64m68_mem_write16(mem, 0x1006, 0xd168);          /* ADD.W D0,$10(A0) */
        la64m68_mem_write16(mem, 0x1008, 0x0010);          /* disp = 16 */
        la64m68_mem_write16(mem, 0x100a, 0x4e72);          /* STOP */
        la64m68_mem_write16(mem, 0x100c, 0x2700);
        la64m68_mem_write16(mem, 0x2010, 7);

        la64m68_cpu cpu;
        la64m68_cpu_reset(&cpu, mem, NULL);
        cpu.regs[0] = 5;
        la64m68_cpu_step(&cpu);                    /* MOVEA */
        la64m68_cpu_step(&cpu);                    /* ADD.W D0,$10(A0) */
        assert(cpu.pc == 0x100a);                  /* opcode + one ext word */
        assert(la64m68_mem_read16(mem, 0x2010) == 12);
        assert(la64m68_mem_read16(mem, 0x100a) == 0x4e72);  /* STOP intact */
        la64m68_ram_memory_destroy(mem);
    }

    /* 2. an acknowledged interrupt must drop the pending level, otherwise
     * the same request re-triggers on every step */
    {
        la64m68_memory *mem = la64m68_ram_memory_create(0, 65536);
        assert(mem);
        la64m68_mem_write32(mem, 0, 0x00002000);
        la64m68_mem_write32(mem, 4, 0x00001000);
        la64m68_mem_write32(mem, 27 * 4, 0x00003000);      /* autovector 3 */
        la64m68_mem_write16(mem, 0x1000, 0x4e71);
        la64m68_mem_write16(mem, 0x3000, 0x4e71);

        la64m68_cpu cpu;
        la64m68_cpu_reset(&cpu, mem, NULL);
        cpu.sr = 0x2000;
        la64m68_cpu_ipl(&cpu, 3);
        la64m68_cpu_step(&cpu);
        assert(cpu.ipl == 0);
        la64m68_ram_memory_destroy(mem);
    }

    /* 3. router: newest region wins, so a device window overlays RAM */
    {
        la64m68_memory *rt = la64m68_router_create();
        /* the sub-bus base must match the router window: ram_read8() computes
         * off = addr - base, so a bus created at 0 but mounted at 0x2000
         * would reject every access (off >= size) and silently drop it */
        la64m68_memory *a = la64m68_ram_memory_create(0x2000, 0x1000);
        la64m68_memory *b = la64m68_ram_memory_create(0x2000, 0x1000);
        assert(rt && a && b);
        assert(la64m68_router_add(rt, 0x2000, 0x1000, a) == 0);
        assert(la64m68_router_add(rt, 0x2000, 0x1000, b) == 0);
        la64m68_mem_write32(rt, 0x2000, 0xdeadbeef);
        assert(la64m68_mem_read32(b, 0x2000) == 0xdeadbeef);
        assert(la64m68_mem_read32(a, 0x2000) == 0);        /* shadowed */
        la64m68_router_destroy(rt);
        la64m68_ram_memory_destroy(a);
        la64m68_ram_memory_destroy(b);
    }

    /* 4. router: a region whose base+size wraps 32 bits must not alias low
     * addresses (the old `addr < base + size` test mis-routed them) */
    {
        la64m68_memory *rt = la64m68_router_create();
        la64m68_memory *hi = la64m68_ram_memory_create(0xfff00000, 0x100000);
        assert(rt && hi);
        assert(la64m68_router_add(rt, 0xfff00000, 0x200000, hi) == 0);
        la64m68_mem_write32(rt, 0xfff00000, 0x11223344);
        assert(la64m68_mem_read32(rt, 0xfff00000) == 0x11223344);
        assert(la64m68_mem_read32(rt, 0x00050000) == 0);
        la64m68_router_destroy(rt);
        la64m68_ram_memory_destroy(hi);
    }

    /* 5. fp80: subnormal doubles must survive the round trip. The old code
     * shifted the normalised significand one bit too far and lost bit 63. */
    {
        double sub = 5e-324;                        /* smallest subnormal */
        la64m68_fp80 f = la64m68_fp80_from_double(sub);
        assert(la64m68_fp80_to_double(f) == sub);
        double two = 1e-310;                        /* another subnormal */
        assert(la64m68_fp80_to_double(la64m68_fp80_from_double(two)) == two);
        assert(la64m68_fp80_to_double(la64m68_fp80_from_double(1.5)) == 1.5);
    }

    /* 6. command line: trailing garbage must not parse as a number, and an
     * out-of-range value must be rejected rather than silently defaulted */
    {
        la64m68_opts o;
        char *argv1[] = { "la64m68", "--ram-kb", "12abc" };
        la64m68_opts_defaults(&o);
        assert(la64m68_opts_parse(&o, 3, argv1) == -1);

        char *argv2[] = { "la64m68", "--ram-kb", "2048" };
        la64m68_opts_defaults(&o);
        assert(la64m68_opts_parse(&o, 3, argv2) == 0);
        assert(o.ram_kb == 2048);

        char *argv3[] = { "la64m68", "--ram-kb=0x20" };
        la64m68_opts_defaults(&o);
        assert(la64m68_opts_parse(&o, 2, argv3) == 0);
        assert(o.ram_kb == 32);

        char *argv4[] = { "la64m68", "--ram-kb", "0" };
        la64m68_opts_defaults(&o);
        assert(la64m68_opts_parse(&o, 3, argv4) == -1);

        /* unknown option must say so, not "needs a value" */
        char *argv5[] = { "la64m68", "--bogus" };
        la64m68_opts_defaults(&o);
        assert(la64m68_opts_parse(&o, 2, argv5) == -1);

        /* booleans: --flag / --no-flag, and they take no value */
        char *argv6[] = { "la64m68", "--no-pis-arm", "--pis-arm" };
        la64m68_opts_defaults(&o);
        assert(la64m68_opts_parse(&o, 3, argv6) == 0);
        assert(o.pis_arm == 1);                /* later flag wins */
        assert(o.pis == 1 && o.vrtg == 1 && o.vhid == 1);   /* defaults */

        char *argv7[] = { "la64m68", "--pis-arm=yes" };
        la64m68_opts_defaults(&o);
        assert(la64m68_opts_parse(&o, 2, argv7) == -1);
    }

    /* 7. shifts: X must be cleared when the last bit shifted out was 0 */
    {
        la64m68_memory *mem = la64m68_ram_memory_create(0, 65536);
        assert(mem);
        la64m68_mem_write32(mem, 0, 0x00002000);
        la64m68_mem_write32(mem, 4, 0x00001000);
        la64m68_mem_write16(mem, 0x1000, 0x7201);   /* MOVEQ #1,D1 */
        la64m68_mem_write16(mem, 0x1002, 0x7002);   /* MOVEQ #2,D0 */
        la64m68_mem_write16(mem, 0x1004, 0xe249);   /* LSR.W #1,D1 -> X=1 */
        la64m68_mem_write16(mem, 0x1006, 0xe248);   /* LSR.W #1,D0 -> X=0 */
        la64m68_mem_write16(mem, 0x1008, 0x4e72);
        la64m68_mem_write16(mem, 0x100a, 0x2700);

        la64m68_cpu cpu;
        la64m68_cpu_reset(&cpu, mem, NULL);
        la64m68_cpu_step(&cpu);
        la64m68_cpu_step(&cpu);
        la64m68_cpu_step(&cpu);
        assert(cpu.sr & 0x0010);                   /* X set by D1 shift */
        la64m68_cpu_step(&cpu);
        assert(!(cpu.sr & 0x0010));                /* X cleared by D0 shift */
        la64m68_ram_memory_destroy(mem);
    }

    /* 8. vRTG: a guest-supplied absurd mode must not overflow the pitch */
    {
        la64m68_vrtg v;
        la64m68_vrtg_init(&v, NULL, 0);
        la64m68_vrtg_set_mode(&v, 0xffffffffu, 0xffffffffu, 32);
        assert(v.width == 640 && v.height == 480);
        la64m68_vrtg_set_mode(&v, 0, 0, 7);
        assert(v.width == 640 && v.height == 480 && v.bpp == 8);
    }

    /* 9. branches: a word displacement is relative to the extension word,
     * not to the address behind it. Musashi does REG_PC -= 2 (or -= 4 for
     * the long form) before adding the offset; the old code added the
     * displacement uncorrected, so every Bcc.W/BRA.W/BSR.W landed two
     * bytes too far -- the most common branch form in real code. */
    {
        la64m68_memory *mem = la64m68_ram_memory_create(0, 65536);
        assert(mem);
        la64m68_mem_write32(mem, 0, 0x00002000);
        la64m68_mem_write32(mem, 4, 0x00001000);
        la64m68_mem_write16(mem, 0x1000, 0x6000);   /* BRA.W */
        la64m68_mem_write16(mem, 0x1002, 0x0006);   /* disp -> 0x1008 */
        la64m68_mem_write16(mem, 0x1004, 0x7207);   /* MOVEQ #7,D1 (skipped) */
        la64m68_mem_write16(mem, 0x1006, 0x7207);
        la64m68_mem_write16(mem, 0x1008, 0x7405);   /* MOVEQ #5,D2 = target */

        la64m68_cpu cpu;
        la64m68_cpu_reset(&cpu, mem, NULL);
        la64m68_cpu_step(&cpu);                     /* BRA.W */
        assert(cpu.pc == 0x1008);
        la64m68_cpu_step(&cpu);                     /* MOVEQ #5,D2 */
        assert(cpu.regs[2] == 5);
        assert(cpu.regs[1] == 0);                   /* skipped body never ran */
        la64m68_ram_memory_destroy(mem);

        /* BSR.W: the pushed return address is the instruction behind the
         * extension word, and the target uses the same corrected disp. */
        la64m68_memory *m2 = la64m68_ram_memory_create(0, 65536);
        assert(m2);
        la64m68_mem_write32(m2, 0, 0x00002000);
        la64m68_mem_write32(m2, 4, 0x00001000);
        la64m68_mem_write16(m2, 0x1000, 0x6100);    /* BSR.W */
        la64m68_mem_write16(m2, 0x1002, 0x0006);    /* disp -> 0x1008 */
        la64m68_mem_write16(m2, 0x1004, 0x7609);    /* MOVEQ #9,D3 = return */
        la64m68_mem_write16(m2, 0x1006, 0x4e71);    /* NOP */
        la64m68_mem_write16(m2, 0x1008, 0x4e75);    /* RTS = target */

        la64m68_cpu_reset(&cpu, m2, NULL);
        la64m68_cpu_step(&cpu);                     /* BSR.W */
        assert(cpu.pc == 0x1008);
        la64m68_cpu_step(&cpu);                     /* RTS */
        assert(cpu.pc == 0x1004);
        la64m68_cpu_step(&cpu);                     /* MOVEQ #9,D3 */
        assert(cpu.regs[3] == 9);
        la64m68_ram_memory_destroy(m2);
    }

    /* 10. static BSET #n,(An) must not be swallowed by the CHK2/CMP2
     * decoder: Musashi bset_8_s_ai = 0x08d0 and chk2cmp2_8_di share the
     * 0xfff8 mask and differ only in bit 11. Amiga hardware drivers use
     * BSET #n,<ea> heavily, CAS.B/W/L share the same shape. */
    {
        la64m68_memory *mem = la64m68_ram_memory_create(0, 65536);
        assert(mem);
        la64m68_mem_write32(mem, 0, 0x00002000);
        la64m68_mem_write32(mem, 4, 0x00001000);
        la64m68_mem_write16(mem, 0x1000, 0x207c);   /* MOVEA.L #$2000,A0 */
        la64m68_mem_write32(mem, 0x1002, 0x00002000);
        la64m68_mem_write16(mem, 0x1006, 0x08d0);   /* BSET #n,(A0) */
        la64m68_mem_write16(mem, 0x1008, 0x0003);   /* n = 3 */
        la64m68_mem_write16(mem, 0x100a, 0x08d0);   /* BSET #n,(A0) again */
        la64m68_mem_write16(mem, 0x100c, 0x0003);

        la64m68_cpu cpu;
        la64m68_cpu_reset(&cpu, mem, NULL);
        la64m68_cpu_step(&cpu);                     /* MOVEA */
        la64m68_cpu_step(&cpu);                     /* BSET */
        assert(cpu.pc == 0x100a);                   /* opcode + one ext word */
        assert(la64m68_mem_read8(mem, 0x2000) == 0x08);
        assert(cpu.sr & 0x0004);                    /* Z: old bit value was 0 */
        la64m68_cpu_step(&cpu);                     /* BSET again */
        assert(la64m68_mem_read8(mem, 0x2000) == 0x08);
        assert(!(cpu.sr & 0x0004));                 /* Z: old bit value was 1 */
        la64m68_ram_memory_destroy(mem);
    }

    /* 11. pMMU: an enabled TTR must match only its own logical block. The
     * old mask expression folded to 0xffffffff, so ~mask was 0, the compare
     * was vacuous and every enabled TTR transparently translated the whole
     * address space. */
    {
        la64m68_memory *mem = la64m68_ram_memory_create(0, 65536);
        assert(mem);
        la64m68_vmmu mmu;
        la64m68_vmmu_reset(&mmu);
        mmu.tc = LA64M68_VMMU_TC_E;
        /* TA = 0x01 (block 0x01000000), TAM = 0x00 (no don't-care bits),
         * supervisor-only, enabled */
        mmu.dttr[0] = (0x01u << 24) | LA64M68_VMMU_TTR_E | LA64M68_VMMU_TTR_S;

        uint32_t pa = 0;
        assert(la64m68_vmmu_translate(&mmu, mem, 0x01000000, 1, 0, 0,
                                      &pa, NULL) == LA64M68_VMMU_OK);
        assert(pa == 0x01000000);
        /* outside the block: no TTR hit and srp/urp are empty -> must fault */
        assert(la64m68_vmmu_translate(&mmu, mem, 0x02000000, 1, 0, 0,
                                      &pa, NULL) != LA64M68_VMMU_OK);
        /* user mode must not match a supervisor-only TTR */
        assert(la64m68_vmmu_translate(&mmu, mem, 0x01000000, 0, 0, 0,
                                      &pa, NULL) != LA64M68_VMMU_OK);

        /* TAM widens the block: TA = 0x00 with TAM = 0xfe leaves only TA
         * bit 24 significant, so the whole 0x00xxxxxx range matches while
         * 0x01000000 does not. */
        mmu.dttr[0] = (0xfeu << 16) | LA64M68_VMMU_TTR_E | LA64M68_VMMU_TTR_S;
        assert(la64m68_vmmu_translate(&mmu, mem, 0x00abcdef, 1, 0, 0,
                                      &pa, NULL) == LA64M68_VMMU_OK);
        assert(pa == 0x00abcdef);
        assert(la64m68_vmmu_translate(&mmu, mem, 0x01000000, 1, 0, 0,
                                      &pa, NULL) != LA64M68_VMMU_OK);
        la64m68_ram_memory_destroy(mem);
    }

    printf("test_core: hardening ok\n");
}

/* Hardware safety gate. An automated run must not be able to drive the Amiga
 * bus: pis_open(0) records the disarmed flag before any probe, so the arming
 * assertion below is discriminating even on a host with no RPi attached -- a
 * mutation that ignores the argument fails here.
 *
 * Note pis_open(0) does write the GPIO block, but only to force the
 * protocol-safe idle (data bus to inputs, RD/WR inactive), which is the
 * repair action for a previously crashed run. It never starts a transfer. */
static void test_pis_safety(void)
{
    la64m68_pis *p = la64m68_pis_open(0, LA64M68_PIS_AUTO); /* DISARMED */
    if (p) {
        assert(la64m68_pis_armed(p) == 0);       /* gate honoured */
        assert(la64m68_pis_present(p) == 0);     /* nothing to attach to */
        /* no window => no reachable bus transfer */
        assert(la64m68_pis_chipset_mem(p) == NULL);
        assert(la64m68_pis_cia_mem(p) == NULL);
        assert(la64m68_pis_zorro2_mem(p) == NULL);
        /* reconfiguring the FPGA is refused outright */
        assert(la64m68_pis_fpga_load(p, "does-not-exist.svf") == -1);
        assert(la64m68_pis_status(p) == -1);     /* gpio absent/disarmed */
        assert(la64m68_pis_ipl_level(p) == 0);
        la64m68_pis_close(p);
    }

    /* NULL tolerance: the shared teardown runs on every error path */
    assert(la64m68_pis_armed(NULL) == 0);
    assert(la64m68_pis_present(NULL) == 0);
    assert(la64m68_pis_chipset_mem(NULL) == NULL);
    assert(la64m68_pis_cia_mem(NULL) == NULL);
    assert(la64m68_pis_zorro2_mem(NULL) == NULL);
    assert(la64m68_pis_fpga_load(NULL, "x") == -1);
    la64m68_pis_close(NULL);

    printf("test_core: pis safety ok\n");
}

/* Focus switch (Ctrl+Alt+Pause is wired in input.c, the state lives here).
 * Two things are easy to get wrong and are therefore pinned down: the default
 * must keep the operator in control of the host, and a switch must release
 * every key still held in the guest or Ctrl/Alt stay stuck on the side we
 * left. */
static void test_vhid_focus(void)
{
    la64m68_vhid h;
    la64m68_vhid_init(&h);

    /* fail-safe default: the host keeps its input until asked otherwise */
    assert(la64m68_vhid_focus(&h) == LA64M68_VHID_FOCUS_HOST);

    assert(la64m68_vhid_toggle_focus(&h) == LA64M68_VHID_FOCUS_GUEST);
    assert(la64m68_vhid_focus(&h) == LA64M68_VHID_FOCUS_GUEST);
    assert(la64m68_vhid_toggle_focus(&h) == LA64M68_VHID_FOCUS_HOST);
    assert(la64m68_vhid_focus(&h) == LA64M68_VHID_FOCUS_HOST);

    /* switching away releases the held keys */
    la64m68_vhid_set_focus(&h, LA64M68_VHID_FOCUS_GUEST);
    la64m68_vhid_push_key(&h, 0x63, 1);     /* Ctrl down */
    la64m68_vhid_push_key(&h, 0x64, 1);     /* Alt down */
    la64m68_vhid_set_focus(&h, LA64M68_VHID_FOCUS_HOST);

    int down = 0, rel = 0;
    while (h.q_head != h.q_tail) {
        uint32_t w = h.queue[h.q_head];
        h.q_head = (h.q_head + 1) % VHID_QUEUE_DEPTH;
        if ((w >> 24) != 1) continue;       /* kind 1 = key */
        if ((w & 0xffff) == 0) rel++; else down++;
    }
    assert(down == 2);                      /* the two presses */
    assert(rel == 2);                       /* both released on the switch */

    /* re-selecting the current focus must not emit anything */
    la64m68_vhid_set_focus(&h, LA64M68_VHID_FOCUS_HOST);
    assert(h.q_head == h.q_tail);

    /* NULL tolerance */
    assert(la64m68_vhid_focus(NULL) == LA64M68_VHID_FOCUS_HOST);
    assert(la64m68_vhid_toggle_focus(NULL) == LA64M68_VHID_FOCUS_HOST);
    la64m68_vhid_set_focus(NULL, 1);

    printf("test_core: vhid focus ok\n");
}

/* fp80 -> packed decimal. NaN/Inf and out-of-range operands must produce a
 * defined value instead of leaking character arithmetic into the BCD nibbles,
 * and a rounding carry must advance the *signed* exponent. */
static void test_vfpu_packed(void)
{
    uint8_t out[12];

    /* 1. NaN and Inf: defined zero, sign kept, no garbage anywhere */
    la64m68_fp80_to_packed(la64m68_fp80_from_double(NAN), -1, out);
    for (int i = 0; i < 12; i++) assert(out[i] == 0);
    la64m68_fp80_to_packed(la64m68_fp80_from_double(INFINITY), -1, out);
    for (int i = 0; i < 12; i++) assert(out[i] == 0);
    la64m68_fp80_to_packed(la64m68_fp80_from_double(-INFINITY), -1, out);
    assert(out[0] == 0x80);                 /* sign preserved */
    for (int i = 1; i < 12; i++) assert(out[i] == 0);

    /* 2. carry across the MSD: 9.999e-1 rounded to 1.000e+00.
     * The MSD becomes 1 and the signed exponent goes -1 -> 0; leaving the
     * MSD at 0 would silently turn the value into zero. */
    la64m68_fp80_to_packed(la64m68_fp80_from_double(0.9999), -1, out);
    assert(out[3] == 0x01);                            /* MSD = 1 */
    assert(out[0] == 0 && out[1] == 0 && out[2] == 0);  /* exp 0, positive */
    for (int i = 4; i < 12; i++) assert(out[i] == 0);

    printf("test_core: vfpu packed ok\n");
}

/* vBLK: raw image, and the qcow2 cluster map.
 *
 * The qcow2 case builds a real v2 image in a temporary file -- header, L1
 * table, one L2 table, one allocated cluster and one hole -- and then reads
 * across the boundary. That is the only way to know the L1/L2 walk is right:
 * a wrong offset mask would still produce plausible-looking data. */
static void test_vblk(void)
{
    static const uint32_t CL = 1u << 16;          /* cluster_bits = 16 */
    char name[] = "vblk-test-XXXXXX";
    int fd = mkstemp(name);
    assert(fd >= 0);

    /* --- build the image --- */
    uint64_t virtual_size = 2 * CL;               /* 2 clusters, 2nd is a hole */
    unsigned char hdr[72] = {0};
    hdr[0] = 0x51; hdr[1] = 0x46; hdr[2] = 0x49; hdr[3] = 0xfb;  /* QFI\xfb */
    hdr[7] = 2;                                    /* version 2 */
    hdr[23] = 16;                                  /* cluster_bits */
    for (int i = 0; i < 8; i++) hdr[24 + i] = (unsigned char)(virtual_size >> (56 - 8 * i));
    hdr[39] = 1;                                   /* l1_size = 1 */
    for (int i = 0; i < 8; i++) hdr[40 + i] = (unsigned char)(0x200ull >> (56 - 8 * i));  /* l1 at 0x200 */
    assert(write(fd, hdr, sizeof(hdr)) == (ssize_t)sizeof(hdr));

    /* L1[0] -> L2 table at 0x10000, present bit set */
    uint64_t l2_off = 0x10000, data_off = 0x20000;
    unsigned char l1[8];
    uint64_t l1e = l2_off | 1;
    for (int i = 0; i < 8; i++) l1[i] = (unsigned char)(l1e >> (56 - 8 * i));
    assert(pwrite(fd, l1, 8, 0x200) == 8);

    /* L2: entry 0 -> data cluster, entry 1 left zero = hole */
    unsigned char l2[8] = {0};
    for (int i = 0; i < 8; i++) l2[i] = (unsigned char)(data_off >> (56 - 8 * i));
    assert(pwrite(fd, l2, 8, (off_t)l2_off) == 8);

    /* data cluster: 0xa0 + index, so every byte position is identifiable */
    unsigned char *data = malloc(CL);
    assert(data);
    for (uint32_t i = 0; i < CL; i++) data[i] = (unsigned char)(0xa0 + (i & 0x1f));
    assert(pwrite(fd, data, CL, (off_t)data_off) == (ssize_t)CL);

    /* --- raw backend --- */
    la64m68_vblk_src raw;
    assert(la64m68_vblk_open_raw(name, 1, &raw) == 0);
    assert(raw.kind == VBLK_KIND_RAW);
    /* raw IS the whole file -- for a qcow2 image that includes its metadata,
     * which is exactly the difference the qcow2 backend exists to hide */
    assert(raw.ops->size(raw.ctx) == data_off + CL);
    unsigned char buf[16];
    assert(raw.ops->read(raw.ctx, 0, buf, sizeof(buf)) == 0);
    assert(buf[0] == 0x51);                       /* the qcow2 magic, at 0 */
    assert(raw.ops->read(raw.ctx, data_off, buf, sizeof(buf)) == 0);
    assert(buf[0] == 0xa0 && buf[15] == (unsigned char)(0xa0 + 15));
    la64m68_vblk_src_close(&raw);

    /* --- qcow2 backend --- */
    la64m68_vblk_src q;
    assert(la64m68_vblk_open_qcow2(name, 1, &q) == 0);
    assert(q.kind == VBLK_KIND_QCOW2);
    /* qcow2 reports the VIRTUAL size, not the file size -- the metadata is
     * invisible to the guest, which is the point of the format */
    assert(q.ops->size(q.ctx) == 2 * CL);

    /* allocated cluster */
    assert(q.ops->read(q.ctx, 0, buf, sizeof(buf)) == 0);
    assert(buf[0] == 0xa0 && buf[15] == (unsigned char)(0xa0 + 15));

    /* the hole must read as zero, not as file bytes */
    memset(buf, 0xcc, sizeof(buf));
    assert(q.ops->read(q.ctx, CL, buf, sizeof(buf)) == 0);
    for (size_t i = 0; i < sizeof(buf); i++) assert(buf[i] == 0);

    /* a read straddling the boundary: data first, then zeros */
    unsigned char cross[64];
    assert(q.ops->read(q.ctx, CL - 32, cross, sizeof(cross)) == 0);
    assert(cross[0] == (unsigned char)(0xa0 + ((CL - 32) & 0x1f)));
    for (size_t i = 32; i < sizeof(cross); i++) assert(cross[i] == 0);

    /* write into the allocated cluster works */
    unsigned char w1[4] = { 1, 2, 3, 4 }, r1[4] = {0};
    assert(q.ops->write(q.ctx, 100, w1, sizeof(w1)) == 0);
    assert(q.ops->read(q.ctx, 100, r1, sizeof(r1)) == 0);
    assert(memcmp(w1, r1, sizeof(w1)) == 0);

    /* write into the hole must fail: no allocation is implemented, and
     * silently accepting it would lose the data */
    assert(q.ops->write(q.ctx, CL + 8, w1, sizeof(w1)) != 0);

    la64m68_vblk_src_close(&q);
    free(data);
    close(fd);
    unlink(name);
    printf("test_core: vblk ok\n");
}

int main(void)
{
    test_reset_step();
    test_ipl();
    test_vaga();
    test_router();
    test_vmmu();
    test_vfpu();
    test_vrtg();
    test_vnic_vhid();
    test_vm();
    test_hardening();
    test_pis_safety();
    test_vhid_focus();
    test_vfpu_packed();
    test_vblk();
    printf("test_core: all ok\n");
    return 0;
}
