#include "cpu.h"
#include "debug.h"
#include <math.h>
#include <string.h>

/* 68060 interpreter core — first working subset.
 *
 * Implemented: MOVE.{B,W,L} (common EA modes), MOVEQ, MOVEA,
 * NOP/RESET/RTS/STOP, JSR (abs), ADDQ/SUBQ, Bcc/BSR/BRA,
 * ADD/SUB/AND/OR/EOR/CMP (Dn variants), AS/LS/RO register shifts,
 * LINE A/F exception entry, vMMU-aware bus access, exceptions via VBR.
 * Everything else raises an "unimplemented" trace + LINE-F style stop
 * until the remaining emitters arrive.
 */

#define SR_T   LA64M68_SR_T
#define SR_S   LA64M68_SR_S
#define SR_X   LA64M68_SR_X
#define SR_N   LA64M68_SR_N
#define SR_Z   LA64M68_SR_Z
#define SR_V   LA64M68_SR_V
#define SR_C   LA64M68_SR_C

#define SZ_B 1
#define SZ_W 2
#define SZ_L 4

/* ---- bus access through vMMU ---- */

static int xlat(la64m68_cpu *c, uint32_t va, int instr, int write,
                uint32_t *pa)
{
    return la64m68_vmmu_translate(&c->vmmu, c->mem, va,
                                  (c->sr & SR_S) != 0, instr, write,
                                  pa, NULL);
}

static uint32_t cread(la64m68_cpu *c, uint32_t va, int size, int instr)
{
    uint32_t pa;
    if (xlat(c, va, instr, 0, &pa) != LA64M68_VMMU_OK) {
        c->fault = va;
        return 0;
    }
    switch (size) {
    case SZ_B: return la64m68_mem_read8(c->mem, pa);
    case SZ_W: return la64m68_mem_read16(c->mem, pa);
    default:   return la64m68_mem_read32(c->mem, pa);
    }
}

static void cwrite(la64m68_cpu *c, uint32_t va, uint32_t v, int size)
{
    uint32_t pa;
    if (xlat(c, va, 0, 1, &pa) != LA64M68_VMMU_OK) {
        c->fault = va;
        return;
    }
    switch (size) {
    case SZ_B: la64m68_mem_write8(c->mem, pa, (uint8_t)v); break;
    case SZ_W: la64m68_mem_write16(c->mem, pa, (uint16_t)v); break;
    default:   la64m68_mem_write32(c->mem, pa, v); break;
    }
}

static uint16_t fetch16(la64m68_cpu *c)
{
    uint16_t w = (uint16_t)cread(c, c->pc, SZ_W, 1);
    c->pc += 2;
    return w;
}

static uint32_t fetch32(la64m68_cpu *c)
{
    uint32_t hi = fetch16(c);
    return (hi << 16) | fetch16(c);
}

/* ---- condition helpers ---- */

static void set_nz(la64m68_cpu *c, uint32_t v, int size)
{
    uint32_t sign;
    switch (size) {
    case SZ_B: v &= 0xff;   sign = 0x80; break;
    case SZ_W: v &= 0xffff; sign = 0x8000; break;
    default:                sign = 0x80000000; break;
    }
    c->sr &= ~(SR_N | SR_Z | SR_V | SR_C);
    if (v == 0) c->sr |= SR_Z;
    if (v & sign) c->sr |= SR_N;
}

static uint32_t msb_of(int size)
{
    return size == SZ_B ? 0x80 : size == SZ_W ? 0x8000 : 0x80000000u;
}

static uint32_t mask_of(int size)
{
    return size == SZ_B ? 0xff : size == SZ_W ? 0xffff : 0xffffffffu;
}

static int32_t signext(uint32_t v, int size)
{
    if (size == SZ_B) return (int8_t)(v & 0xff);
    if (size == SZ_W) return (int16_t)(v & 0xffff);
    return (int32_t)v;
}

/* exact 68060 flag rules (MC68060UM §integer unit):
 * ADD:  C=X=(s&d)|(d&~r)|(s&~r)   V=(s&d&~r)|(~s&~d&r)   Z cleared iff r!=0
 * SUB:  C=X=(s&~d)|(r&~d)|(s&r)   V=(~s&d&~r)|(s&~d&r)   Z cleared iff r!=0
 * CMP:  same as SUB but X preserved.
 * Sticky-Z applies to *X variants only; plain ops set Z from r==0. */
static void flags_add(la64m68_cpu *c, uint32_t s, uint32_t d, uint32_t r, int size)
{
    uint32_t m = mask_of(size), sm = msb_of(size);
    uint32_t S = s & m, D = d & m, R = r & m;
    c->sr &= ~(SR_X | SR_N | SR_Z | SR_V | SR_C);
    if (((S & D) | (D & ~R) | (S & ~R)) & sm) c->sr |= SR_C | SR_X;
    if (((S & D & ~R) | (~S & ~D & R)) & sm) c->sr |= SR_V;
    if (R == 0) c->sr |= SR_Z;
    if (R & sm) c->sr |= SR_N;
}

static void flags_sub(la64m68_cpu *c, uint32_t s, uint32_t d, uint32_t r, int size, int is_cmp)
{
    uint32_t m = mask_of(size), sm = msb_of(size);
    uint32_t S = s & m, D = d & m, R = r & m;
    c->sr &= ~(SR_N | SR_Z | SR_V | SR_C);
    if (!is_cmp) c->sr &= ~SR_X;
    if (((S & ~D) | (R & ~D) | (S & R)) & sm) {
        c->sr |= SR_C;
        if (!is_cmp) c->sr |= SR_X;
    }
    if (((~S & D & ~R) | (S & ~D & R)) & sm) c->sr |= SR_V;
    if (R == 0) c->sr |= SR_Z;
    if (R & sm) c->sr |= SR_N;
}

static int cond_true(la64m68_cpu *c, int cc)
{
    int n = !!(c->sr & SR_N), z = !!(c->sr & SR_Z);
    int v = !!(c->sr & SR_V), cy = !!(c->sr & SR_C);
    switch (cc) {
    case 0: return 1;            /* T  */
    case 1: return 0;            /* F  */
    case 2: return !cy && !z;    /* HI */
    case 3: return cy || z;      /* LS */
    case 4: return !cy;          /* CC/HS */
    case 5: return cy;           /* CS/LO */
    case 6: return !z;           /* NE */
    case 7: return z;            /* EQ */
    case 8: return !v;           /* VC */
    case 9: return v;            /* VS */
    case 10: return !n;          /* PL */
    case 11: return n;           /* MI */
    case 12: return n == v;      /* GE */
    case 13: return n != v;      /* LT */
    case 14: return !z && (n == v); /* GT */
    default: return z || (n != v);  /* LE */
    }
}

/* ---- exceptions ---- */

/* SR write with S-bit stack swap (060 stack model: A7 = active). */
static void cpu_set_sr(la64m68_cpu *c, uint16_t v)
{
    int was_s = c->sr & SR_S;
    c->sr = v & 0xa71f;
    if (was_s && !(c->sr & SR_S)) {          /* supervisor -> user */
        c->ssp = c->regs[15];
        c->regs[15] = c->usp;
    } else if (!was_s && (c->sr & SR_S)) {   /* user -> supervisor */
        c->usp = c->regs[15];
        c->regs[15] = c->ssp;
    }
}

/* extra frame bytes by 68060 stack frame format */
static int frame_extra(int fmt)
{
    switch (fmt) {
    case 0: return 0;
    case 1: return 4;    /* throwaway */
    case 2: return 4;    /* instruction error */
    case 7: return 52;   /* access error (060): fmtvec+SR+PC+SSW/EA/etc */
    default: return 0;
    }
}

void la64m68_cpu_exception_fmt(la64m68_cpu *c, uint32_t vector, int fmt)
{
    if (!(c->sr & SR_S)) {
        c->usp = c->regs[15];
        c->regs[15] = c->ssp;
        c->sr |= SR_S;
    }
    uint16_t sr = c->sr;
    c->sr &= ~(SR_T);
    int extra = frame_extra(fmt);
    /* 68k frame order: push PC, then SR, then format|vector-offset word.
     * Stack layout after entry: +0 fmtvec, +2 SR, +4 PC, then extra. */
    uint32_t sp = c->regs[15];
    cwrite(c, sp - 4, c->pc, SZ_L);            sp -= 4;
    cwrite(c, sp - 2, sr, SZ_W);               sp -= 2;
    cwrite(c, sp - 2, (uint16_t)((fmt << 12) | (vector * 4)), SZ_W);
    sp -= 2;
    sp -= extra;
    c->regs[15] = sp;
    /* format-7 (060 access/bus error): fault effective address at
     * frame +0x0a region (documented approximation; SSW/FSLW later). */
    if (fmt == 7)
        cwrite(c, sp + 0x0a, c->fault, SZ_L);
    c->pc = cread(c, c->vbr + vector * 4, SZ_L, 0);
    la64m68_trace("cpu: exception vec=%u fmt=%d -> pc=%08x",
                  vector, fmt, c->pc);
}

void la64m68_cpu_exception(la64m68_cpu *c, uint32_t vector)
{
    la64m68_cpu_exception_fmt(c, vector, 0);
}

/* ---- effective address ---- */

static int ea_reg(uint16_t op) { return op & 7; }
static int ea_mode(uint16_t op) { return (op >> 3) & 7; }

/* extension word EA: d8(An,Xn.{W,L}*scale) / d8(PC,Xn.{W,L}*scale)
 * and the 68020+ full format (bit8): base/index suppress, bd, memory
 * indirection (pre/post-indexed) with outer displacement. */
static uint32_t ea_ext(la64m68_cpu *c, int is_pc, uint16_t op)
{
    uint32_t base = is_pc ? c->pc : c->regs[8 + ea_reg(op)];
    uint16_t x = fetch16(c);

    if (!(x & 0x0100)) {            /* brief extension */
        int32_t idx = (int32_t)c->regs[((x >> 15) & 1) * 8 + ((x >> 12) & 7)];
        if (!(x & 0x0800)) idx = (int32_t)(int16_t)idx;
        idx <<= (x >> 9) & 3;                    /* scale field */
        return base + idx + (int8_t)(x & 0xff);
    }

    /* full extension */
    int bs = (x >> 7) & 1;          /* base suppress */
    int is = (x >> 6) & 1;          /* index suppress */
    int bds = (x >> 4) & 3;         /* base displacement size */
    int iis = x & 0xf;              /* index/indirect selection */
    uint32_t bd = bds == 2 ? (uint32_t)(int32_t)(int16_t)fetch16(c)
                : bds == 3 ? fetch32(c) : 0;
    int32_t idx = 0;
    if (!is) {
        idx = (int32_t)c->regs[((x >> 15) & 1) * 8 + ((x >> 12) & 7)];
        if (!(x & 0x0800)) idx = (int32_t)(int16_t)idx;
        idx <<= (x >> 9) & 3;
    }
    if (bs) base = 0;

    if (iis == 0 || iis == 4)                       /* none / reserved */
        return base + bd + (uint32_t)idx;
    int ods = iis & 3;                              /* od size: 1=null,2=w,3=l */
    uint32_t od;
    int post = iis >= 5;
    if (post) {                                     /* post-indexed */
        uint32_t inner = cread(c, base + bd, SZ_L, 0);
        od = ods == 2 ? (uint32_t)(int32_t)(int16_t)fetch16(c)
           : ods == 3 ? fetch32(c) : 0;
        return inner + (uint32_t)idx + od;
    }
    uint32_t inner = cread(c, base + bd + (uint32_t)idx, SZ_L, 0); /* pre */
    od = ods == 2 ? (uint32_t)(int32_t)(int16_t)fetch16(c)
       : ods == 3 ? fetch32(c) : 0;
    return inner + od;
}

static uint32_t ea_read(la64m68_cpu *c, uint16_t op, int size)
{
    int m = ea_mode(op), r = ea_reg(op);
    int inc = (r == 7 && size == SZ_B) ? 2 : size;
    switch (m) {
    case 0: return c->regs[r] & (size == SZ_B ? 0xff : size == SZ_W ? 0xffff : 0xffffffffu);
    case 1: return c->regs[8 + r];
    case 2: return cread(c, c->regs[8 + r], size, 0);
    case 3: {
        uint32_t v = cread(c, c->regs[8 + r], size, 0);
        c->regs[8 + r] += inc;
        return v;
    }
    case 4:
        c->regs[8 + r] -= inc;
        return cread(c, c->regs[8 + r], size, 0);
    case 5:
        return cread(c, c->regs[8 + r] + (int16_t)fetch16(c), size, 0);
    case 6:
        return cread(c, ea_ext(c, 0, op), size, 0);
    case 7:
        if (r == 0) return cread(c, (uint16_t)fetch16(c), size, 0);   /* abs.w */
        if (r == 1) return cread(c, fetch32(c), size, 0);             /* abs.l */
        if (r == 2) {                                                  /* d16(PC) */
            uint32_t base = c->pc;
            return cread(c, base + (int16_t)fetch16(c), size, 0);
        }
        if (r == 3) return cread(c, ea_ext(c, 1, op), size, 0);       /* d8(PC,Xn) */
        if (r == 4) {                                                  /* #imm */
            switch (size) {
            case SZ_B: return fetch16(c) & 0xff;
            case SZ_W: return fetch16(c);
            default:   return fetch32(c);
            }
        }
        return 0;
    default:
        return 0;
    }
}

static void ea_write(la64m68_cpu *c, uint16_t op, int size, uint32_t v)
{
    int m = ea_mode(op), r = ea_reg(op);
    int inc = (r == 7 && size == SZ_B) ? 2 : size;
    switch (m) {
    case 0:
        if (size == SZ_L) c->regs[r] = v;
        else if (size == SZ_W) c->regs[r] = (c->regs[r] & 0xffff0000) | (v & 0xffff);
        else c->regs[r] = (c->regs[r] & ~0xff) | (v & 0xff);
        return;
    case 1: c->regs[8 + r] = v; return;
    case 2: cwrite(c, c->regs[8 + r], v, size); return;
    case 3: cwrite(c, c->regs[8 + r], v, size); c->regs[8 + r] += inc; return;
    case 4: c->regs[8 + r] -= inc; cwrite(c, c->regs[8 + r], v, size); return;
    case 5: cwrite(c, c->regs[8 + r] + (int16_t)fetch16(c), v, size); return;
    case 6: cwrite(c, ea_ext(c, 0, op), v, size); return;
    case 7:
        if (r == 0) { cwrite(c, (uint16_t)fetch16(c), v, size); return; }
        if (r == 1) { cwrite(c, fetch32(c), v, size); return; }
        return;
    default: return;
    }
}

/* ---- opcode lines ---- */

static uint32_t ea_addr(la64m68_cpu *c, uint16_t op)
{
    int m = ea_mode(op), r = ea_reg(op);
    switch (m) {
    case 2: return c->regs[8 + r];
    case 5: return c->regs[8 + r] + (int16_t)fetch16(c);
    case 6: return ea_ext(c, 0, op);
    case 7:
        if (r == 0) return (uint16_t)fetch16(c);
        if (r == 1) return fetch32(c);
        if (r == 2) {                   /* d16(PC) */
            uint32_t base = c->pc;
            return base + (int16_t)fetch16(c);
        }
        if (r == 3) return ea_ext(c, 1, op);
        return 0;
    default: return 0;
    }
}

/* Read-modify-write EA: resolve to one concrete address, consuming the
 * extension words exactly once and updating -(An)/(An)+ exactly once.
 *
 * Never use ea_read() followed by ea_write() on the same operand: both
 * re-fetch d16/bd/abs extensions, so the PC would advance twice and the
 * second access would use a wrong displacement. Returns 0 for modes with
 * no writable memory address (Dn/An/#imm/PC-relative). */
static int ea_addr_rmw(la64m68_cpu *c, uint16_t op, int size, uint32_t *out)
{
    int m = ea_mode(op), r = ea_reg(op);
    int inc = (r == 7 && size == SZ_B) ? 2 : size;
    switch (m) {
    case 2: *out = c->regs[8 + r]; return 1;
    case 3: *out = c->regs[8 + r]; c->regs[8 + r] += inc; return 1;
    case 4: c->regs[8 + r] -= inc; *out = c->regs[8 + r]; return 1;
    case 5: *out = c->regs[8 + r] + (uint32_t)(int16_t)fetch16(c); return 1;
    case 6: *out = ea_ext(c, 0, op); return 1;
    case 7:
        if (r == 0) { *out = (uint16_t)fetch16(c); return 1; }
        if (r == 1) { *out = fetch32(c); return 1; }
        if (r == 2) { *out = c->pc + (uint32_t)(int16_t)fetch16(c); return 1; }
        if (r == 3) { *out = ea_ext(c, 1, op); return 1; }
        return 0;      /* #imm: no address */
    default: return 0; /* Dn / An */
    }
}

/* Read an operand that will be written back (read-modify-write).
 * Returns  1: memory operand, *addr set, write back with rmw_write()
 *          0: register operand (Dn/An), write back with rmw_write()
 *         -1: illegal effective address
 * The EA is resolved exactly once, so extension words are consumed once and
 * -(An)/(An)+ update once -- see ea_addr_rmw(). */
static int rmw_read(la64m68_cpu *c, uint16_t op, int size, uint32_t *addr,
                    uint32_t *val)
{
    if (ea_mode(op) >= 2) {
        int ok = ea_addr_rmw(c, op, size, addr);
        if (!ok) return -1;
        *val = cread(c, *addr, size, 0);
        return 1;
    }
    *val = ea_read(c, op, size);
    return 0;
}

static void rmw_write(la64m68_cpu *c, uint16_t op, int size, int mem,
                      uint32_t addr, uint32_t v)
{
    if (mem > 0) cwrite(c, addr, v, size);
    else         ea_write(c, op, size, v);
}

static int op_movem(la64m68_cpu *c, uint16_t op)
{
    int sz = (op >> 6) & 1 ? SZ_L : SZ_W;
    int dir = (op >> 10) & 1;           /* 0: regs->mem, 1: mem->regs */
    int m = ea_mode(op), r = ea_reg(op);
    uint16_t mask = fetch16(c);
    uint32_t a;
    int i;

    if (!dir && m == 4) {               /* -(An): reversed order */
        a = c->regs[8 + r];
        for (i = 15; i >= 0; i--)
            if (mask & (1u << (15 - i))) {
                a -= sz;
                cwrite(c, a, c->regs[i], sz);
            }
        c->regs[8 + r] = a;
        return 1;
    }
    if (dir && m == 3) {                /* (An)+ */
        a = c->regs[8 + r];
        for (i = 0; i < 16; i++)
            if (mask & (1u << i)) {
                uint32_t v = cread(c, a, sz, 0);
                c->regs[i] = (sz == SZ_W) ? (uint32_t)(int32_t)(int16_t)v : v;
                a += sz;
            }
        c->regs[8 + r] = a;
        return 1;
    }
    /* control modes: (An), d16(An), abs.w/l */
    a = ea_addr(c, op);
    for (i = 0; i < 16; i++) {
        if (!(mask & (1u << i))) continue;
        if (dir) {
            uint32_t v = cread(c, a, sz, 0);
            c->regs[i] = (sz == SZ_W) ? (uint32_t)(int32_t)(int16_t)v : v;
        } else {
            cwrite(c, a, c->regs[i], sz);
        }
        a += sz;
    }
    return 1;
}

/* bit ops: kind 0=BTST 1=BCHG 2=BCLR 3=BSET — Z set iff bit was 0 */
static int bitop(la64m68_cpu *c, uint16_t op, int kind, uint32_t n)
{
    int m = ea_mode(op);
    int size = (m == 0) ? SZ_L : SZ_B;      /* Dn: modulo 32, mem: modulo 8 */
    uint32_t a = 0, v;
    int mem = rmw_read(c, op, size, &a, &v);
    if (mem < 0) return -1;
    uint32_t bit = 1u << (n & (size * 8 - 1));
    c->sr &= ~SR_Z;
    if (!(v & bit)) c->sr |= SR_Z;
    if (kind == 0) return 1;                /* BTST: read only */
    if      (kind == 1) v ^= bit;
    else if (kind == 2) v &= ~bit;
    else                v |= bit;
    rmw_write(c, op, size, mem, a, v);
    return 1;
}

static int op_movep(la64m68_cpu *c, uint16_t op);

/* BCD byte add/subtract (Musashi-style adjust semantics).
 * Returns result byte; *cout = carry/borrow out. */
static uint32_t bcd_add(uint8_t s, uint8_t d, int x, int *cout)
{
    uint32_t res = (s & 0x0f) + (d & 0x0f) + x;
    uint32_t corf = res > 9 ? 6 : 0;
    res += (s & 0xf0) + (d & 0xf0) + corf;
    *cout = res > 0x9f;
    if (*cout) res -= 0xa0;
    return res & 0xff;
}
static uint32_t bcd_sub(uint8_t s, uint8_t d, int x, int *cout)
{
    uint32_t res = (d & 0x0f) - (s & 0x0f) - x;
    uint32_t corf = res > 0xf ? 6 : 0;
    res += (d & 0xf0) - (s & 0xf0);
    if (res > 0xff) { res += 0xa0; *cout = 1; }
    else            *cout = res < corf;
    return (res - corf) & 0xff;
}

/* ABCD/SBCD: register or -(An) form. Z is sticky (cleared only if
 * result nonzero), X=C carry out, V/N officially undefined (left). */
static int op_bcd(la64m68_cpu *c, uint16_t op, int sub)
{
    int rm  = (op >> 3) & 1;             /* 0 = Dy,Dx  1 = -(Ay),-(Ax) */
    int rx  = (op >> 9) & 7, ry = op & 7;
    int x   = (c->sr & SR_X) ? 1 : 0;
    uint8_t s, d, r;
    int co;

    if (rm) {
        int decs = ry == 7 ? 2 : 1, decd = rx == 7 ? 2 : 1;
        c->regs[8 + ry] -= decs;
        s = (uint8_t)cread(c, c->regs[8 + ry], SZ_B, 0);
        d = (uint8_t)cread(c, c->regs[8 + rx] - decd, SZ_B, 0);
        r = (uint8_t)(sub ? bcd_sub(s, d, x, &co) : bcd_add(s, d, x, &co));
        c->regs[8 + rx] -= decd;
        cwrite(c, c->regs[8 + rx], r, SZ_B);
    } else {
        s = (uint8_t)c->regs[ry];
        d = (uint8_t)c->regs[rx];
        r = (uint8_t)(sub ? bcd_sub(s, d, x, &co) : bcd_add(s, d, x, &co));
        c->regs[rx] = (c->regs[rx] & ~0xff) | r;
    }
    if (co) c->sr |= SR_X | SR_C;
    else    c->sr &= ~(SR_X | SR_C);
    if (r)  c->sr &= ~SR_Z;              /* sticky Z */
    return 1;
}

/* PACK/UNPK (BCD pack): rr or -(An) form, adjustment word follows. */
static int op_pack(la64m68_cpu *c, uint16_t op, int unpk)
{
    int rm  = (op >> 3) & 1;
    int rx  = (op >> 9) & 7, ry = op & 7;
    uint16_t adj = fetch16(c);
    uint32_t r;

    if (unpk) {
        uint8_t b;
        if (rm) {
            c->regs[8 + ry] -= ry == 7 ? 2 : 1;
            b = (uint8_t)cread(c, c->regs[8 + ry], SZ_B, 0);
        } else {
            b = (uint8_t)c->regs[ry];
        }
        r = (((b & 0xf0) << 4) | (b & 0x0f)) + adj;
        if (rm) {
            c->regs[8 + rx] -= 2;
            cwrite(c, c->regs[8 + rx], r, SZ_W);
        } else {
            c->regs[rx] = (c->regs[rx] & 0xffff0000) | (r & 0xffff);
        }
    } else {
        uint16_t w;
        if (rm) {
            c->regs[8 + ry] -= 2;
            w = (uint16_t)cread(c, c->regs[8 + ry], SZ_W, 0);
        } else {
            w = (uint16_t)c->regs[ry];
        }
        uint32_t t = w + adj;
        r = (t & 0x0f) | ((t >> 4) & 0xf0);
        if (rm) {
            c->regs[8 + rx] -= rx == 7 ? 2 : 1;
            cwrite(c, c->regs[8 + rx], r, SZ_B);
        } else {
            c->regs[rx] = (c->regs[rx] & ~0xff) | (r & 0xff);
        }
    }
    return 1;
}

/* MOVEC Rc,Rn / MOVEC Rn,Rc — 68060 control registers (privileged).
 * This is how the guest programs the vMMU (TC/URP/SRP/TTR). */
static int op_movec(la64m68_cpu *c, uint16_t op)
{
    if (!(c->sr & SR_S)) { la64m68_cpu_exception(c, 8); return 1; }
    uint16_t w = fetch16(c);
    int rn = (w >> 12) & 0xf;            /* regs[] index (0-7=D, 8-15=A) */
    int cr = w & 0xfff;
    int to_ctrl = op & 1;
    uint32_t *p;

    switch (cr) {
    case 0x000: p = &c->sfc; break;
    case 0x001: p = &c->dfc; break;
    case 0x002: p = &c->cacr; break;
    case 0x003: p = &c->vmmu.tc; break;
    case 0x004: p = &c->vmmu.ittr[0]; break;
    case 0x005: p = &c->vmmu.ittr[1]; break;
    case 0x006: p = &c->vmmu.dttr[0]; break;
    case 0x007: p = &c->vmmu.dttr[1]; break;
    case 0x800: p = &c->usp; break;
    case 0x801: p = &c->vbr; break;
    case 0x803: p = &c->msp; break;
    case 0x804: p = &c->isp; break;
    case 0x806: p = &c->vmmu.urp; break;
    case 0x807: p = &c->vmmu.srp; break;
    case 0x808: p = &c->pcr; break;
    case 0x805:                          /* MMUSR: read-only status */
        if (to_ctrl) return 1;           /* writes ignored */
        c->regs[rn] = c->vmmu.mmusr;
        return 1;
    default:
        la64m68_trace("cpu: movec unknown ctrl %03x pc=%x", cr, c->pc - 4);
        la64m68_cpu_exception(c, 4);     /* illegal instruction */
        return 1;
    }
    if (to_ctrl) {
        *p = c->regs[rn];
        if (cr <= 1) *p &= 7;            /* SFC/DFC are 3-bit FC */
    } else {
        c->regs[rn] = *p;
    }
    return 1;
}


/* line 0: ORI/ANDI/SUBI/ADDI/EORI/CMPI + static/dynamic bit ops */
static int op_line0(la64m68_cpu *c, uint16_t op)
{
    int szbits = (op >> 6) & 3;
    if ((op & 0x0100) && ea_mode(op) == 1)  /* MOVEP d16(An) <-> Dn */
        return op_movep(c, op);
    /* CHK2/CMP2 (020+): 0000 0sz c 11mmmrrr — bit8 c=1 CHK2, c=0 CMP2.
     * Must be checked BEFORE the dynamic-bitop branch (same bit8 shape);
     * register ea modes (0/1) are illegal here and belong to bitops.
     * Bit 11 must be clear: it is the sole discriminator against static
     * BSET #n,<ea> (Musashi bset_8_s_ai = 0x08d0, same 0xfff8 mask as
     * chk2cmp2_8_di) and against CAS.B/W/L (0x0ac0/0x0cc0/0x0ec0), all of
     * which share the 0x00c0 shape. Without the test both were eaten as
     * CMP2 -- BSET #n,(An) is common in Amiga hardware drivers. */
    if (!(op & 0x0800) && (op & 0x00c0) == 0x00c0 && ((op >> 9) & 3) <= 2 &&
        ea_mode(op) > 1 &&
        !(ea_mode(op) == 7 && ea_reg(op) >= 4)) {   /* #imm+ is not a CHK2 ea */
        int size = ((op >> 9) & 3) == 0 ? SZ_B :
                   ((op >> 9) & 3) == 1 ? SZ_W : SZ_L;
        int chk  = (op & 0x0100) != 0;          /* CHK2 vs CMP2 */
        uint16_t x = fetch16(c);
        int is_a = (x >> 15) & 1, rr = (x >> 12) & 7;
        uint32_t lo = ea_read(c, op, size);
        /* read the high bound: same EA + element size */
        uint32_t hi;
        if (ea_mode(op) == 3 || ea_mode(op) == 4) {
            hi = ea_read(c, op, size);      /* postinc/predec consume */
        } else {
            uint32_t a = ea_addr(c, op);
            hi = cread(c, a + (size == SZ_B ? 1 : size == SZ_W ? 2 : 4),
                       size, 0);
        }
        uint32_t m = mask_of(size);
        int32_t sl = signext(lo, size), sh = signext(hi, size);
        int32_t sv = signext(c->regs[(is_a ? 8 : 0) + rr] & m, size);
        c->sr &= ~(SR_Z | SR_C);
        if (sv == sl || sv == sh) c->sr |= SR_Z;
        if (sl <= sh) {                            /* normal range */
            if (sv < sl || sv > sh) c->sr |= SR_C;
        } else {                                   /* wrapped range */
            if (sv > sh && sv < sl) c->sr |= SR_C;
        }
        if (chk && (c->sr & SR_C) && !(c->sr & SR_Z))
            la64m68_cpu_exception(c, 6);           /* CHK exception */
        return 1;
    }
    if (op & 0x0100)                        /* dynamic bit op: BTST/BCHG/BCLR/BSET Dn,<ea> */
        return bitop(c, op, (op >> 6) & 3, c->regs[(op >> 9) & 7]);
    /* CAS2 Dc1:Dc2,Du1:Du2,(Rn1):(Rn2) (020+) — W/L only, ea field = 0x3c */
    if ((op & 0xf1ff) == 0x00fc) {          /* CAS2.W 0x0cfc / CAS2.L 0x0efc */
        int size = ((op >> 9) & 3) == 2 ? SZ_W :
                   ((op >> 9) & 3) == 3 ? SZ_L : 0;
        if (!size) return -1;
        /* ext order (M68000PRM): first word = pair 2 (Rn2/Dc2/Du2),
         * second word = pair 1 (Rn1/Dc1/Du1). bit15 of Rn field = An. */
        uint16_t x2 = fetch16(c), x1 = fetch16(c);
        int dc1 = (x1 >> 6) & 7, du1 = x1 & 7;
        int dc2 = (x2 >> 6) & 7, du2 = x2 & 7;
        int rn1 = ((x1 >> 12) & 7) | ((x1 & 0x8000) ? 8 : 0);
        int rn2 = ((x2 >> 12) & 7) | ((x2 & 0x8000) ? 8 : 0);
        uint32_t a1 = c->regs[rn1], a2 = c->regs[rn2];
        uint32_t m1 = cread(c, a1, size, 0), m2 = cread(c, a2, size, 0);
        uint32_t c1 = c->regs[dc1] & mask_of(size);
        uint32_t c2 = c->regs[dc2] & mask_of(size);
        if (m1 != c1) {
            flags_sub(c, c1, m1, m1 - c1, size, 1);
            c->regs[dc1] = (c->regs[dc1] & ~mask_of(size)) | m1;
        } else if (m2 != c2) {
            flags_sub(c, c2, m2, m2 - c2, size, 1);
            c->regs[dc2] = (c->regs[dc2] & ~mask_of(size)) | m2;
        } else {
            c->sr |= SR_Z;
            cwrite(c, a1, c->regs[du1], size);
            cwrite(c, a2, c->regs[du2], size);
        }
        return 1;
    }
    if ((op & 0xffc0) == 0x0ac0 || (op & 0xffc0) == 0x0cc0 || (op & 0xffc0) == 0x0ec0) {
        /* CAS Dc,Du,<ea> (020+): ext word = Dc[8:6], Du[2:0] */
        if (ea_mode(op) == 1) return -1;          /* An not allowed */
        static const int cas_sz[4] = { 0, SZ_B, SZ_W, SZ_L };
        int size = cas_sz[(op >> 9) & 3];
        uint16_t x = fetch16(c);
        int dc = (x >> 6) & 7, du = x & 7;
        uint32_t a = 0, mem;
        int ismem = rmw_read(c, op, size, &a, &mem);
        if (ismem < 0) return -1;
        uint32_t cv = c->regs[dc] & mask_of(size);
        flags_sub(c, cv, mem, mem - cv, size, 1);
        if (mem == cv) {
            rmw_write(c, op, size, ismem, a, c->regs[du]);
        } else {
            c->regs[dc] = (c->regs[dc] & ~mask_of(size)) | mem;
        }
        return 1;
    }
    int sel = (op >> 8) & 0xf;
    if (sel == 8)                           /* static bit op: #n in extension */
        return bitop(c, op, szbits, fetch16(c));
    if (szbits == 3) {
        /* immediate-to-CCR/SR forms */
        uint16_t imm = fetch16(c);
        if (sel == 0 && (op & 0x003f) == 0x3c) {          /* ORI #,CCR */
            c->sr |= imm & 0x1f;
            return 1;
        }
        if (sel == 2 && (op & 0x003f) == 0x3c) {          /* ANDI #,CCR */
            /* AND the CCR bits with imm, keep every upper SR bit. The parens
             * make the precedence explicit; the old bare `~0x1f | x` relied
             * on `~` binding tighter than `|`. */
            c->sr &= (uint16_t)(~0x1fu | (imm & 0x1fu));
            return 1;
        }
        if (sel == 0xa && (op & 0x003f) == 0x3c) {        /* EORI #,CCR */
            c->sr ^= imm & 0x1f;
            return 1;
        }
        if ((op & 0x003f) == 0x3c) {                      /* SR forms (privileged) */
            if (!(c->sr & SR_S)) { la64m68_cpu_exception(c, 8); return 1; }
            uint16_t nw;
            if (sel == 0)      nw = c->sr | (imm & 0xa71f);   /* ORI #,SR */
            else if (sel == 2) nw = c->sr & (imm & 0xa71f);   /* ANDI #,SR */
            else if (sel == 0xa) nw = c->sr ^ (imm & 0xa71f); /* EORI #,SR */
            else return -1;
            cpu_set_sr(c, nw);
            return 1;
        }
        return -1;
    }
    int size = szbits == 0 ? SZ_B : szbits == 1 ? SZ_W : SZ_L;
    uint32_t imm = (size == SZ_L) ? fetch32(c) : fetch16(c);
    if (size == SZ_B) imm &= 0xff;
    switch (sel) {
    case 0xc: {                             /* CMPI #imm,<ea> */
        uint32_t v = ea_read(c, op, size);
        flags_sub(c, imm, v, v - imm, size, 1);
        return 1;
    }
    case 0: case 2: case 4: case 6: case 0xa: {
        uint32_t a = 0, v, res;
        int mem = rmw_read(c, op, size, &a, &v);
        if (mem < 0) return -1;
        switch (sel) {
        case 0: res = v | imm; break;
        case 2: res = v & imm; break;
        case 4: res = v - imm; break;
        case 6: res = v + imm; break;
        default: res = v ^ imm; break;
        }
        rmw_write(c, op, size, mem, a, res);
        if (sel == 4)      flags_sub(c, imm, v, res, size, 0);
        else if (sel == 6) flags_add(c, imm, v, res, size);
        else               set_nz(c, res, size);
        return 1;
    }
    default:
        return -1;                          /* MOVEP etc. later */
    }
}

static int op_mul(la64m68_cpu *c, uint16_t op, int sig)
{
    uint16_t s = (uint16_t)ea_read(c, op, SZ_W);
    int r = (op >> 9) & 7;
    uint32_t res = sig ? (uint32_t)((int32_t)(int16_t)(uint16_t)c->regs[r] * (int32_t)(int16_t)s)
                       : (uint32_t)(uint16_t)c->regs[r] * s;
    c->regs[r] = res;
    set_nz(c, res, SZ_L);
    return 1;
}

static int op_div(la64m68_cpu *c, uint16_t op, int sig)
{
    uint32_t dvs = ea_read(c, op, SZ_W) & 0xffff;
    int r = (op >> 9) & 7;
    uint32_t dvd = c->regs[r];
    c->sr &= ~(SR_N | SR_Z | SR_V | SR_C);
    if (dvs == 0) {                         /* divide by zero -> vector 5 */
        la64m68_cpu_exception(c, 5);
        return 1;
    }
    if (!sig) {
        uint32_t q = dvd / dvs, rem = dvd % dvs;
        if (q > 0xffff) { c->sr |= SR_V; return 1; }
        c->regs[r] = (rem << 16) | q;
        if (q == 0) c->sr |= SR_Z;
        else if (q & 0x8000) c->sr |= SR_N;
    } else {
        int32_t sd = (int32_t)dvd, ss = (int32_t)(int16_t)dvs;
        /* INT32_MIN / -1 is signed-overflow UB (and traps on x86 hosts). The
         * true quotient 0x80000000 does not fit 16 bits either, so the
         * hardware answer is V with quotient and remainder untouched. */
        if (sd == INT32_MIN && ss == -1) { c->sr |= SR_V; return 1; }
        int32_t q = sd / ss, rem = sd % ss;
        if (q > 32767 || q < -32768) { c->sr |= SR_V; return 1; }
        c->regs[r] = (((uint32_t)rem & 0xffff) << 16) | ((uint32_t)q & 0xffff);
        if (q == 0) c->sr |= SR_Z;
        else if (q < 0) c->sr |= SR_N;
    }
    return 1;
}

static int op_move(la64m68_cpu *c, uint16_t op, int size)
{
    /* MOVE src(mode/reg in low) -> dst(reg/mode in high, swizzled) */
    uint32_t v = ea_read(c, op, size);
    int dr = (op >> 9) & 7, dm = (op >> 6) & 7;
    uint16_t dop = (uint16_t)((dm << 3) | dr);
    if (dm == 1) {                      /* MOVEA: sign-extend, no flags */
        c->regs[8 + dr] = (size == SZ_W) ? (uint32_t)(int32_t)(int16_t)v : v;
    } else {
        ea_write(c, dop, size, v);
        set_nz(c, v, size);
    }
    return 1;
}

static int op_line4(la64m68_cpu *c, uint16_t op)
{
    switch (op) {
    case 0x4e70: /* RESET */
        if (c->plugin && c->plugin->reset) c->plugin->reset(c->plugin->ctx);
        return 1;
    case 0x4e71: return 1;              /* NOP */
    case 0x4e72: {                      /* STOP (privileged) */
        uint16_t newsr = fetch16(c);
        if (!(c->sr & SR_S)) { la64m68_cpu_exception(c, 8); return 1; }
        cpu_set_sr(c, newsr);           /* masked + S-bit stack swap */
        return 0;                       /* halts stepping */
    }
    case 0x4e76:                        /* TRAPV */
        if (c->sr & SR_V) la64m68_cpu_exception(c, 7);
        return 1;
    case 0x4e77: {                      /* RTR: CCR + PC */
        uint16_t cc = (uint16_t)cread(c, c->regs[15], SZ_W, 0);
        c->pc = cread(c, c->regs[15] + 2, SZ_L, 0);
        c->sr = (c->sr & ~0x1f) | (cc & 0x1f);
        c->regs[15] += 6;
        return 1;
    }
    case 0x4e75: {                      /* RTS */
        c->pc = cread(c, c->regs[15], SZ_L, 0);
        c->regs[15] += 4;
        return 1;
    }
    default:
        if ((op & 0xfff0) == 0x4e40) {  /* TRAP #n -> vector 32+n */
            la64m68_cpu_exception(c, 32 + (op & 0xf));
            return 1;
        }
        if (op == 0x4e74) {             /* RTD #disp (010+) */
            c->pc = cread(c, c->regs[15], SZ_L, 0);
            c->regs[15] += 4 + (int16_t)fetch16(c);
            return 1;
        }
        if ((op & 0xfff8) == 0x4848) {  /* BKPT #n */
            la64m68_trace("cpu: bkpt #%u pc=%x", op & 7, c->pc - 2);
            la64m68_cpu_exception(c, 4);
            return 1;
        }
        if ((op & 0xfff8) == 0x4808) {  /* LINK An,#disp32 (020+) */
            int r = op & 7;
            int32_t disp = (int32_t)fetch32(c);
            uint32_t sp = c->regs[15];
            cwrite(c, sp - 4, c->regs[8 + r], SZ_L);
            c->regs[8 + r] = sp - 4;
            c->regs[15] = sp - 4 + disp;
            return 1;
        }
        if ((op & 0xfff8) == 0x4e50) {  /* LINK An,#disp */
            int r = op & 7;
            int32_t disp = (int16_t)fetch16(c);
            uint32_t sp = c->regs[15];
            cwrite(c, sp - 4, c->regs[8 + r], SZ_L);
            c->regs[8 + r] = sp - 4;
            c->regs[15] = sp - 4 + disp;
            return 1;
        }
        if ((op & 0xfff8) == 0x4e58) {  /* UNLK An */
            int r = op & 7;
            uint32_t fp = c->regs[8 + r];
            c->regs[15] = fp + 4;
            c->regs[8 + r] = cread(c, fp, SZ_L, 0);
            return 1;
        }
        if (op == 0x4e73) {             /* RTE (formats 0/1/2/7) */
            /* privileged: it swaps the active stack pointer, exactly like the
             * MOVE USP case a few lines below */
            if (!(c->sr & SR_S)) { la64m68_cpu_exception(c, 8); return 1; }
            uint16_t fv = (uint16_t)cread(c, c->regs[15], SZ_W, 0);
            int fmt = fv >> 12;
            int extra = frame_extra(fmt);
            if (fmt != 0 && extra == 0) return -1;   /* unknown frame */
            c->sr = (uint16_t)cread(c, c->regs[15] + 2, SZ_W, 0);
            c->pc = cread(c, c->regs[15] + 4, SZ_L, 0);
            c->regs[15] += 8 + extra;
            if (!(c->sr & SR_S)) {       /* return to user mode: swap stacks */
                c->ssp = c->regs[15];
                c->regs[15] = c->usp;
            }
            return 1;
        }
        if (op == 0x4e7a || op == 0x4e7b)
            return op_movec(c, op);
        if ((op & 0xfff8) == 0x4e60 || (op & 0xfff8) == 0x4e68) {
            /* MOVE USP (privileged) */
            if (!(c->sr & SR_S)) { la64m68_cpu_exception(c, 8); return 1; }
            int r = op & 7;
            if (op & 8) c->regs[8 + r] = c->usp;
            else        c->usp = c->regs[8 + r];
            return 1;
        }
        if ((op & 0xffc0) == 0x4800) {  /* NBCD <ea> */
            uint32_t a = 0, w;
            int mem = rmw_read(c, op, SZ_B, &a, &w);
            if (mem < 0) return -1;
            int co;
            uint8_t r = (uint8_t)bcd_sub((uint8_t)w, 0,
                                         (c->sr & SR_X) ? 1 : 0, &co);
            rmw_write(c, op, SZ_B, mem, a, r);
            if (co) c->sr |= SR_X | SR_C;
            else    c->sr &= ~(SR_X | SR_C);
            if (r)  c->sr &= ~SR_Z;
            return 1;
        }
        if ((op & 0xfff8) == 0x4840) {  /* SWAP Dn */
            int r = op & 7;
            c->regs[r] = (c->regs[r] << 16) | (c->regs[r] >> 16);
            set_nz(c, c->regs[r], SZ_L);
            return 1;
        }
        if ((op & 0xfff8) == 0x4880) {  /* EXT.W Dn */
            int r = op & 7;
            c->regs[r] = (c->regs[r] & 0xffff0000) |
                         ((uint32_t)(int32_t)(int16_t)(int8_t)c->regs[r] & 0xffff);
            set_nz(c, c->regs[r], SZ_W);
            return 1;
        }
        if ((op & 0xfff8) == 0x48c0) {  /* EXT.L Dn */
            int r = op & 7;
            c->regs[r] = (uint32_t)(int32_t)(int16_t)c->regs[r];
            set_nz(c, c->regs[r], SZ_L);
            return 1;
        }
        if ((op & 0xffc0) == 0x40c0) {  /* MOVE SR,<ea> */
            ea_write(c, op, SZ_W, c->sr);
            return 1;
        }
        if ((op & 0xffc0) == 0x42c0) {  /* MOVE CCR,<ea> */
            ea_write(c, op, SZ_B, c->sr & 0x1f);
            return 1;
        }
        if ((op & 0xffc0) == 0x44c0) {  /* MOVE <ea>,CCR */
            c->sr = (c->sr & ~0x1f) | (ea_read(c, op, SZ_W) & 0x1f);
            return 1;
        }
        if ((op & 0xffc0) == 0x46c0) {  /* MOVE <ea>,SR (privileged) */
            if (!(c->sr & SR_S)) { la64m68_cpu_exception(c, 8); return 1; }
            cpu_set_sr(c, (uint16_t)ea_read(c, op, SZ_W));
            return 1;
        }
        {
            int fam = op & 0xff00;
            if (fam == 0x4000 || fam == 0x4200 || fam == 0x4400 || fam == 0x4600 || fam == 0x4a00) {
                int sz = (op >> 6) & 3;
                if (fam == 0x4a00 && (op & 0xffc0) == 0x4ac0) {
                    /* TAS <ea>: test + set bit7 */
                    uint32_t a = 0, v;
                    int mem = rmw_read(c, op, SZ_B, &a, &v);
                    if (mem < 0) return -1;
                    set_nz(c, v, SZ_B);
                    rmw_write(c, op, SZ_B, mem, a, v | 0x80);
                    return 1;
                }
                if (sz == 3) return -1;
                int size = sz == 0 ? SZ_B : sz == 1 ? SZ_W : SZ_L;
                if (fam == 0x4000) {                    /* NEGX: 0-v-X, sticky Z */
                    uint32_t a = 0, v;
                    int mem = rmw_read(c, op, size, &a, &v);
                    if (mem < 0) return -1;
                    uint32_t m = mask_of(size), sign = msb_of(size);
                    int x = !!(c->sr & SR_X);
                    uint32_t res = (0 - v - x) & m;
                    rmw_write(c, op, size, mem, a, res);
                    c->sr &= ~(SR_N | SR_V | SR_C | SR_X);
                    if ((v & m) + x > 0) c->sr |= SR_C | SR_X;
                    if ((res & v) & sign) c->sr |= SR_V;
                    if (res) c->sr &= ~SR_Z;
                    if (res & sign) c->sr |= SR_N;
                } else if (fam == 0x4200) {             /* CLR */
                    ea_write(c, op, size, 0);
                    c->sr &= ~(SR_N | SR_Z | SR_V | SR_C);
                    c->sr |= SR_Z;
                } else if (fam == 0x4400) {             /* NEG: sticky Z */
                    uint32_t a = 0, v;
                    int mem = rmw_read(c, op, size, &a, &v);
                    if (mem < 0) return -1;
                    uint32_t res = 0 - v;
                    rmw_write(c, op, size, mem, a, res);
                    uint16_t z = c->sr & SR_Z;
                    flags_sub(c, v, 0, res, size, 0);
                    if (res & mask_of(size)) c->sr &= ~SR_Z;
                    else c->sr |= z;
                } else if (fam == 0x4600) {             /* NOT */
                    uint32_t a = 0, v;
                    int mem = rmw_read(c, op, size, &a, &v);
                    if (mem < 0) return -1;
                    uint32_t res = ~v;
                    rmw_write(c, op, size, mem, a, res);
                    set_nz(c, res, size);
                } else {                                /* TST */
                    set_nz(c, ea_read(c, op, size), size);
                }
                return 1;
            }
        }
        if ((op & 0xf1c0) == 0x4180 || (op & 0xf1c0) == 0x4100) {
            /* CHK.W / CHK.L <ea>,Dn -> CHK exception vector 6 */
            int is_l = !(op & 0x80);
            int32_t bound = is_l ? (int32_t)ea_read(c, op, SZ_L)
                                 : (int16_t)ea_read(c, op, SZ_W);
            int r = (op >> 9) & 7;
            int32_t d = is_l ? (int32_t)c->regs[r] : (int16_t)c->regs[r];
            c->sr &= ~(SR_N | SR_Z | SR_V | SR_C);
            if (d < 0)      { c->sr |= SR_N; la64m68_cpu_exception(c, 6); }
            else if (d > bound) la64m68_cpu_exception(c, 6);
            else if (d == 0) c->sr |= SR_Z;
            return 1;
        }
        if ((op & 0xffc0) == 0x4c00 || (op & 0xffc0) == 0x4c40) {
            /* MULU.L/MULS.L (0x4c00) / DIVU.L/DIVS.L/DIVL (0x4c40) + ext */
            if (ea_mode(op) == 1) return -1;
            int mul = !(op & 0x40);
            uint16_t x = fetch16(c);
            int sign  = (x >> 11) & 1;
            int wide  = (x >> 10) & 1;      /* 64-bit result/dividend */
            int rd_hi = (x >> 12) & 7;      /* MUL: Dh, DIV: Dq */
            int rd_lo = x & 7;              /* MUL: Dl, DIV: Dr */
            uint32_t ea = ea_read(c, op, SZ_L);
            if (mul) {
                uint64_t p = sign ? (uint64_t)((int64_t)(int32_t)c->regs[rd_hi] *
                                               (int32_t)ea)
                                  : (uint64_t)c->regs[rd_hi] * ea;
                if (wide && rd_lo != rd_hi) {
                    c->regs[rd_hi] = (uint32_t)(p >> 32);
                    c->regs[rd_lo] = (uint32_t)p;
                } else {
                    c->regs[rd_hi] = (uint32_t)p;
                }
                c->sr &= ~(SR_N | SR_Z | SR_V | SR_C);
                /* N/Z follow the full product: for a 64-bit result Z must
                 * test all 64 bits, N is bit 63 (== sign of the high half) */
                if (wide) {
                    if (p == 0) c->sr |= SR_Z;
                    if (p >> 63) c->sr |= SR_N;
                } else {
                    if ((uint32_t)p == 0) c->sr |= SR_Z;
                    if (p & 0x80000000u) c->sr |= SR_N;
                }
                return 1;
            }
            /* DIV: ext[10]=0 32/32, 1 = 64/32; Dq quo, Dr rem */
            int dq = rd_hi, dr = rd_lo;
            if (ea == 0) { la64m68_cpu_exception(c, 5); return 1; }
            c->sr &= ~(SR_N | SR_Z | SR_V | SR_C);
            uint32_t q32;
            if (wide) {
                uint64_t num = ((uint64_t)c->regs[dq] << 32) | c->regs[dr];
                uint64_t q = sign ? (uint64_t)((int64_t)num / (int32_t)ea)
                                  : num / ea;
                uint64_t rem = sign ? (uint64_t)((int64_t)num % (int32_t)ea)
                                    : num % ea;
                int ovf = sign
                    ? ((int64_t)q > 0x7fffffffLL || (int64_t)q < -0x80000000LL)
                    : (q > 0xffffffffu);
                if (ovf) { c->sr |= SR_V; return 1; }  /* operands untouched */
                c->regs[dq] = (uint32_t)q;
                c->regs[dr] = (uint32_t)rem;
                q32 = (uint32_t)q;
            } else {
                uint32_t num = c->regs[dq];
                uint32_t q = sign ? (uint32_t)((int32_t)num / (int32_t)ea)
                                  : num / ea;
                uint32_t rem = sign ? (uint32_t)((int32_t)num % (int32_t)ea)
                                    : num % ea;
                if (dr != dq) c->regs[dr] = rem;
                c->regs[dq] = q;
                q32 = q;
            }
            /* N/Z follow the quotient in both the 32/32 and the 64/32 form */
            if (q32 == 0) c->sr |= SR_Z;
            if (q32 & 0x80000000u) c->sr |= SR_N;
            return 1;
        }
        if ((op & 0xf1c0) == 0x41c0) {  /* LEA <ea>,An (control modes only) */
            int m = ea_mode(op);
            if (m != 2 && m != 5 && m != 7) return -1;
            c->regs[8 + ((op >> 9) & 7)] = ea_addr(c, op);
            return 1;
        }
        if ((op & 0xffc0) == 0x4840) {  /* PEA <ea> (control modes only) */
            int m = ea_mode(op);
            if (m != 2 && m != 5 && m != 7) return -1;
            uint32_t a = ea_addr(c, op);
            uint32_t sp = c->regs[15];
            cwrite(c, sp - 4, a, SZ_L);
            c->regs[15] = sp - 4;
            return 1;
        }
        if ((op & 0xfb80) == 0x4880)    /* MOVEM */
            return op_movem(c, op);
        if ((op & 0xffc0) == 0x4ec0) {  /* JMP <ea> (control modes only) */
            int m = ea_mode(op);
            if (m != 2 && m != 5 && m != 7) return -1;
            c->pc = ea_addr(c, op);
            return 1;
        }
        if ((op & 0xffc0) == 0x4e80) {  /* JSR <ea> (control modes only) */
            int m = ea_mode(op);
            if (m != 2 && m != 5 && m != 7) return -1;
            uint32_t sp = c->regs[15];
            uint32_t target = ea_addr(c, op);
            cwrite(c, sp - 4, c->pc, SZ_L);
            c->regs[15] = sp - 4;
            c->pc = target;
            return 1;
        }
        return -1;
    }
}

static int op_line5(la64m68_cpu *c, uint16_t op)
{
    int q = (op >> 9) & 7;
    if (q == 0) q = 8;
    int size = (op >> 6) & 3;
    int sz = size == 0 ? SZ_B : size == 1 ? SZ_W : SZ_L;
    int sub = (op >> 8) & 1;
    int m = ea_mode(op), r = ea_reg(op);
    if (size == 3) {
        int low = op & 0xff, cc = (op >> 8) & 0xf;
        if (low == 0xfc || low == 0xfa || low == 0xfb) {  /* TRAPcc */
            if (low == 0xfa) (void)fetch16(c);          /* word operand */
            else if (low == 0xfb) (void)fetch32(c);     /* long operand */
            if (cond_true(c, cc))
                la64m68_cpu_exception(c, 7);            /* TRAPcc -> vector 7 */
            return 1;
        }
        if (m == 1) {                   /* DBcc Dn,label */
            int16_t disp = (int16_t)fetch16(c);
            if (cond_true(c, cc)) return 1;
            uint32_t lo = (c->regs[r] & 0xffff) - 1;
            c->regs[r] = (c->regs[r] & 0xffff0000) | (lo & 0xffff);
            if ((lo & 0xffff) != 0xffff)
                c->pc += disp - 2;      /* disp relative to extension word */
            return 1;
        }
        /* Scc <ea>: byte 0x00/0xff */
        ea_write(c, op, SZ_B, cond_true(c, cc) ? 0xff : 0x00);
        return 1;
    }
    if (m == 1) {                       /* address register: always L, no flags */
        c->regs[8 + r] += sub ? -(int32_t)q : q;
        return 1;
    }
    uint32_t a = 0, v;
    int mem = rmw_read(c, op, sz, &a, &v);
    if (mem < 0) return -1;
    uint32_t res = sub ? v - q : v + q;
    rmw_write(c, op, sz, mem, a, res);
    if (sub) flags_sub(c, q, v, res, sz, 0);
    else     flags_add(c, q, v, res, sz);
    return 1;
}

static int op_line6(la64m68_cpu *c, uint16_t op)
{
    int cc = (op >> 8) & 0xf;
    int16_t d8 = (int8_t)(op & 0xff);
    /* A byte displacement is relative to the next instruction (PC is already
     * there). Word/long displacements are relative to the address of the
     * extension word, which fetch16/fetch32 just stepped over, so undo that
     * step -- the same convention op_line5() already uses for DBcc. Verified
     * against Musashi m68k_op_bra_16/_bcc_16/_bsr_16: each does REG_PC -= 2
     * (or -= 4) before adding the offset. */
    int32_t disp = d8;
    if (d8 == 0)       disp = (int16_t)fetch16(c) - 2;
    else if (d8 == -1) disp = (int32_t)fetch32(c) - 4;

    if (cc == 1) {                      /* BSR */
        uint32_t sp = c->regs[15];
        cwrite(c, sp - 4, c->pc, SZ_L);
        c->regs[15] = sp - 4;
        c->pc += disp;
        return 1;
    }
    if (cc == 0 || cond_true(c, cc))
        c->pc += disp;
    return 1;
}

/* ADDX/SUBX: reg or -(An) form, X in computation, sticky Z */
static int op_addx_subx(la64m68_cpu *c, uint16_t op, int sub)
{
    int sz = (op >> 6) & 3;
    if (sz == 3) return -1;
    int size = sz == 0 ? SZ_B : sz == 1 ? SZ_W : SZ_L;
    uint32_t m = mask_of(size), sign = msb_of(size);
    int rx = (op >> 9) & 7, ry = op & 7;
    int x = !!(c->sr & SR_X);
    uint32_t s, d;
    int mem = op & 8;
    if (mem) {
        c->regs[8 + ry] -= (ry == 7 && size == SZ_B) ? 2 : size;
        s = cread(c, c->regs[8 + ry], size, 0);
        c->regs[8 + rx] -= (rx == 7 && size == SZ_B) ? 2 : size;
        d = cread(c, c->regs[8 + rx], size, 0);
    } else {
        s = c->regs[ry] & m;
        d = c->regs[rx] & m;
    }
    uint64_t rr = sub ? (uint64_t)d - s - (uint32_t)x : (uint64_t)d + s + (uint32_t)x;
    uint32_t res = (uint32_t)rr & m;
    if (mem) cwrite(c, c->regs[8 + rx], res, size);
    else c->regs[rx] = (c->regs[rx] & ~m) | res;

    c->sr &= ~(SR_N | SR_V | SR_C | SR_X);
    if (sub ? ((uint64_t)s + x > (d & m)) : (rr > m)) c->sr |= SR_C | SR_X;
    if ((sub ? ((d ^ s) & (d ^ res)) : (~(d ^ s) & (d ^ res))) & sign)
        c->sr |= SR_V;
    if (res) c->sr &= ~SR_Z;            /* sticky Z: only cleared */
    if (res & sign) c->sr |= SR_N;
    return 1;
}

/* CMPM (Ay)+,(Ax)+ */
static int op_cmpm(la64m68_cpu *c, uint16_t op)
{
    int sz = (op >> 6) & 3;
    int size = sz == 0 ? SZ_B : sz == 1 ? SZ_W : SZ_L;
    int rx = (op >> 9) & 7, ry = op & 7;
    int inc = (rx == 7 && size == SZ_B) ? 2 : size;
    uint32_t s = cread(c, c->regs[8 + ry], size, 0);
    c->regs[8 + ry] += (ry == 7 && size == SZ_B) ? 2 : size;
    uint32_t d = cread(c, c->regs[8 + rx], size, 0);
    c->regs[8 + rx] += inc;
    flags_sub(c, s, d, d - s, size, 1);
    return 1;
}

/* MOVEP: Dn <-> d16(An) byte-interleaved */
static int op_movep(la64m68_cpu *c, uint16_t op)
{
    int dr = (op >> 9) & 7, ar = op & 7;
    uint16_t disp = fetch16(c);
    uint32_t a = c->regs[8 + ar] + (int16_t)disp;
    int dir = (op >> 7) & 1;            /* 0: mem->Dn, 1: Dn->mem */
    int n = (op & 0x40) ? 4 : 2;
    if (!dir) {
        uint32_t v = 0;
        for (int i = 0; i < n; i++)
            v = (v << 8) | cread(c, a + 2 * i, SZ_B, 0);
        if (n == 4) c->regs[dr] = v;
        else c->regs[dr] = (c->regs[dr] & 0xffff0000) | v;
    } else {
        for (int i = 0; i < n; i++)
            cwrite(c, a + 2 * i, (c->regs[dr] >> (8 * (n - 1 - i))) & 0xff, SZ_B);
    }
    return 1;
}

/* ADDA/SUBA (sz==3 forms): source sign-extended to L, no flags */
static int op_adda_suba(la64m68_cpu *c, uint16_t op, int sub)
{
    int size = (op & 0x100) ? SZ_L : SZ_W;
    uint32_t v = ea_read(c, op, size);
    if (size == SZ_W) v = (uint32_t)(int32_t)(int16_t)v;
    c->regs[8 + ((op >> 9) & 7)] += sub ? -(int32_t)v : (int32_t)v;
    return 1;
}

static int op_ded(la64m68_cpu *c, uint16_t op, int which) /* which: 0=OR 1=AND 2=SUB 3=ADD 4=EOR 5=CMP */
{
    int sz = (op >> 6) & 3;
    int size = sz == 0 ? SZ_B : sz == 1 ? SZ_W : SZ_L;
    int dir = (op >> 8) & 1;
    int dr = (op >> 9) & 7;
    uint32_t a = 0, ea;
    int mem;
    if (!dir && which != 4) {
        /* <ea> op Dn -> Dn: the operand is only read, so a read-only EA such
         * as #imm is legal here. Asking rmw_read() for a write-back slot
         * rejected exactly those and turned ADD.L #imm,Dn into a Line-F. */
        ea = ea_read(c, op, size);
        mem = 0;
    } else {
        mem = rmw_read(c, op, size, &a, &ea);
        if (mem < 0) return -1;
    }
    uint32_t dn = c->regs[dr];
    uint32_t mask = mask_of(size);
    dn &= mask;
    uint32_t res;

    if (which == 5) {                   /* CMP: Dn - <ea>, flags only, X kept */
        res = dn - ea;
        flags_sub(c, ea, dn, res, size, 1);
        return 1;
    }
    switch (which) {
    case 0: res = dn | ea; break;
    case 1: res = dn & ea; break;
    case 2: res = dn - ea; break;
    case 3: res = dn + ea; break;
    default: res = dn ^ ea; break;
    }

    if (!dir || which == 4) {           /* <ea> op Dn -> Dn (EOR only this dir) */
        c->regs[dr] = (c->regs[dr] & ~mask) | (res & mask);
    } else {                            /* Dn op <ea> -> <ea> */
        rmw_write(c, op, size, mem, a, res);
    }
    if (which == 2)      flags_sub(c, ea, dn, res, size, 0);
    else if (which == 3) flags_add(c, ea, dn, res, size);
    else                 set_nz(c, res, size);   /* logic: N/Z, V=C=0, X kept */
    return 1;
}

static int op_shift(la64m68_cpu *c, uint16_t op)
{
    /* register shifts: bits 8=dir, 5=type pair, 11:9=count/reg, 7:6=size, 2:0=Dn */
    int sz = (op >> 6) & 3;
    if (sz == 3) {                      /* memory shifts: single W shift */
        int kind = (op >> 8) & 7;       /* 0 ASR 1 ASL 2 LSR 3 LSL 4 ROXR 5 ROXL 6 ROR 7 ROL */
        if (ea_mode(op) < 2) return -1;
        uint32_t a = 0, v;
        int mem = rmw_read(c, op, SZ_W, &a, &v);
        if (mem < 0) return -1;
        v &= 0xffff;
        uint32_t res;
        int carry = 0;
        switch (kind) {
        case 0: carry = v & 1;        res = (v >> 1) | (v & 0x8000); break;
        case 1: carry = v & 0x8000;   res = v << 1; break;
        case 2: carry = v & 1;        res = v >> 1; break;
        case 3: carry = v & 0x8000;   res = v << 1; break;
        case 4: carry = v & 1;        res = (v >> 1) | ((c->sr & SR_X) ? 0x8000 : 0); break;
        case 5: carry = v & 0x8000;   res = (v << 1) | ((c->sr & SR_X) ? 1 : 0); break;
        case 6: carry = v & 1;        res = (v >> 1) | (carry << 15); break;
        default: carry = v & 0x8000;  res = (v << 1) | (carry ? 1 : 0); break;
        }
        res &= 0xffff;
        rmw_write(c, op, SZ_W, mem, a, res);
        c->sr &= ~(SR_N | SR_Z | SR_V | SR_C);
        if (kind != 6 && kind != 7) c->sr &= ~SR_X;
        if (carry) { c->sr |= SR_C; if (kind != 6 && kind != 7) c->sr |= SR_X; }
        if (res == 0) c->sr |= SR_Z;
        if (res & 0x8000) c->sr |= SR_N;
        return 1;
    }
    int size = sz == 0 ? SZ_B : sz == 1 ? SZ_W : SZ_L;
    int count = (op >> 9) & 7;
    if (op & 0x20) count = (int)(c->regs[count] & 63);
    else if (count == 0) count = 8;
    int dir = (op >> 8) & 1;
    int kind = (op >> 3) & 3;           /* 0=AS 1=LS 2=ROX 3=RO */
    int r = op & 7;
    uint32_t v = c->regs[r];
    uint32_t mask = size == SZ_B ? 0xff : size == SZ_W ? 0xffff : 0xffffffffu;
    int bits = size * 8;
    v &= mask;
    uint32_t res = v;
    uint32_t msb = 1u << (bits - 1);      /* uint32: bits==32 must not overflow */

    int vflag = 0;
    for (int i = 0; i < count; i++) {
        int carry;
        if (dir) {                      /* left */
            carry = !!(res & msb);
            res = (res << 1) & mask;
            if (kind == 0 && ((res ^ v) & msb)) vflag = 1; /* ASL: sign changed */
            if (kind == 3 && carry) res |= 1;
            if (kind == 2 && (c->sr & SR_X)) res |= 1;
        } else {                        /* right */
            carry = res & 1;
            if (kind == 0 && (res & msb))
                res = (res >> 1) | msb; /* ASR keeps sign */
            else
                res >>= 1;
            if (kind == 2 && (c->sr & SR_X)) res |= msb;
            if (kind == 3 && carry) res |= msb;
        }
        /* C follows every shifted bit; X mirrors it for AS/LS/ROX and must
         * also be cleared when the last bit shifted out was 0. RO keeps X. */
        if (carry) c->sr |= SR_C;
        else       c->sr &= ~SR_C;
        if (kind != 3) {
            if (carry) c->sr |= SR_X;
            else       c->sr &= ~SR_X;
        }
    }
    if (count == 0) c->sr &= ~SR_C;
    c->sr = (c->sr & ~SR_V) | (vflag ? SR_V : 0);   /* V: ASL only */
    c->regs[r] = (c->regs[r] & ~mask) | (res & mask);
    set_nz(c, res, size);
    return 1;
}

/* ---- LINE F / FPU (cp id 1) ---- */

/* FPSR condition codes (per 68060 FPSR layout) */
#define FCC_N   0x01000000
#define FCC_Z   0x02000000
#define FCC_I   0x04000000
#define FCC_NAN 0x08000000

static void fp_set_cc(la64m68_cpu *c, double v)
{
    c->vfpu.fpsr &= ~(FCC_N | FCC_Z | FCC_I | FCC_NAN);
    if (isnan(v))      c->vfpu.fpsr |= FCC_NAN;
    else if (isinf(v)) c->vfpu.fpsr |= FCC_I;
    else if (v == 0.0) c->vfpu.fpsr |= FCC_Z;
    if (v < 0.0)       c->vfpu.fpsr |= FCC_N;
}

/* FP condition predicates, cc & 0x1f. Table per MC68881/68060 and verified
 * against the Musashi TEST_CONDITION() reference (MIT, 999-fnd/pistorm32l).
 * n/z/nan are the FPSR condition bits; 0x10..0x1f mirror 0x00..0x0f. */
static int fcc_true(la64m68_cpu *c, int cc)
{
    uint32_t f = c->vfpu.fpsr;
    int n = !!(f & FCC_N), z = !!(f & FCC_Z), nan = !!(f & FCC_NAN);
    switch (cc & 0x0f) {
    case 0x00: return 0;                          /* F  false            */
    case 0x01: return z;                          /* EQ equal            */
    case 0x02: return !(nan || z || n);           /* OGT greater than    */
    case 0x03: return z || !(nan || n);           /* OGE greater/equal   */
    case 0x04: return n && !(nan || z);           /* OLT less than       */
    case 0x05: return z || (n && !nan);           /* OLE less/equal      */
    case 0x06: return !nan && !z;                 /* GL  greater/less    */
    case 0x07: return !nan;                       /* OR  ordered         */
    case 0x08: return nan;                        /* UN  unordered       */
    case 0x09: return nan || z;                   /* UEQ unordered equal */
    case 0x0a: return nan || !(n || z);           /* UGT not less/equal  */
    case 0x0b: return nan || z || !n;             /* UGE not less than   */
    case 0x0c: return nan || (n && !z);           /* ULT not great/equal */
    case 0x0d: return nan || z || n;              /* ULE not greater     */
    case 0x0e: return !z;                         /* NE  not equal       */
    default:   return 1;                          /* T   true            */
    }
}

/* load a numeric source operand as double; fmt: 0=L 1=S 2=X 3=P 4=W 5=D 6=B 7=FPm */
static int fp_src_load(la64m68_cpu *c, uint16_t op, int fmt, int fpnum, double *out)
{
    switch (fmt) {
    case 7: *out = la64m68_fp80_to_double(c->vfpu.fp[fpnum]); return 1;
    case 0: *out = (double)(int32_t)ea_read(c, op, SZ_L); return 1;
    case 4: *out = (double)(int16_t)ea_read(c, op, SZ_W); return 1;
    case 6: *out = (double)(int8_t)ea_read(c, op, SZ_B);  return 1;
    case 1: {
        union { uint32_t u; float f; } t = { .u = ea_read(c, op, SZ_L) };
        *out = (double)t.f;
        return 1;
    }
    case 5: {
        union { uint64_t u; double d; } t;
        uint32_t a = 0, w;
        int mem = rmw_read(c, op, SZ_L, &a, &w);
        if (mem < 0) return 0;
        /* second half at addr+4: the EA is already resolved, do not re-fetch */
        t.u = ((uint64_t)w << 32) |
              (mem > 0 ? cread(c, a + 4, SZ_L, 0) : ea_read(c, op, SZ_L));
        *out = t.d;
        return 1;
    }
    case 2: {
        uint8_t b[12];
        uint32_t a = ea_mode(op) == 2 ? c->regs[8 + ea_reg(op)]
                                      : ea_addr(c, op);
        for (int i = 0; i < 12; i++)
            b[i] = (uint8_t)cread(c, a + i, SZ_B, 0);
        *out = la64m68_fp80_to_double(la64m68_fp80_unpack(b));
        return 1;
    }
    case 3: {                            /* packed BCD (12 bytes) */
        uint8_t b[12];
        uint32_t a = ea_mode(op) == 2 ? c->regs[8 + ea_reg(op)]
                                      : ea_addr(c, op);
        for (int i = 0; i < 12; i++)
            b[i] = (uint8_t)cread(c, a + i, SZ_B, 0);
        *out = la64m68_fp80_to_double(la64m68_fp80_from_packed(b));
        return 1;
    }
    default:
        return 0;
    }
}

/* write a result in destination format to <ea>; k = static k-factor for
 * packed-decimal stores (sign-extended ext bits 6:0). */
static int fp_dst_store(la64m68_cpu *c, uint16_t op, int fmt, double v, int k)
{
    switch (fmt) {
    case 0: ea_write(c, op, SZ_L, (uint32_t)(int32_t)v); return 1;
    case 4: ea_write(c, op, SZ_W, (uint32_t)(int32_t)(int16_t)v); return 1;
    case 6: ea_write(c, op, SZ_B, (uint32_t)(int32_t)(int8_t)v); return 1;
    case 1: {
        union { uint32_t u; float f; } t = { .f = (float)v };
        ea_write(c, op, SZ_L, t.u);
        return 1;
    }
    case 5: {
        union { uint64_t u; double d; } t = { .d = v };
        uint32_t a;
        if (!ea_addr_rmw(c, op, SZ_L, &a)) return 0;
        cwrite(c, a, (uint32_t)(t.u >> 32), SZ_L);
        cwrite(c, a + 4, (uint32_t)t.u, SZ_L);
        return 1;
    }
    case 2: {
        uint8_t b[12];
        la64m68_fp80_pack(la64m68_fp80_from_double(v), b);
        uint32_t a = ea_mode(op) == 2 ? c->regs[8 + ea_reg(op)]
                                      : ea_addr(c, op);
        for (int i = 0; i < 12; i++)
            cwrite(c, a + i, b[i], SZ_B);
        return 1;
    }
    case 3: {                            /* packed BCD, static k-factor */
        uint8_t b[12];
        la64m68_fp80_to_packed(la64m68_fp80_from_double(v), k, b);
        uint32_t a = ea_mode(op) == 2 ? c->regs[8 + ea_reg(op)]
                                      : ea_addr(c, op);
        for (int i = 0; i < 12; i++)
            cwrite(c, a + i, b[i], SZ_B);
        return 1;
    }
    default: return 0;
    }
}

/* FPU op not in the 68060 hardware set: raise FP unimplemented
 * (vector 55) so a host/guest FPSP-style handler can emulate. */
static int fpu_unimpl(la64m68_cpu *c, const char *what, unsigned detail)
{
    la64m68_trace("fpu: unimplemented %s=%x pc=%08x -> vec55",
                  what, detail, c->pc);
    la64m68_cpu_exception(c, 55);
    return 1;
}

static int op_fpu_general(la64m68_cpu *c, uint16_t op)
{
    uint16_t x = fetch16(c);
    int rmode  = (x >> 13) & 7;
    int fmt    = (x >> 10) & 7;
    int dn     = (x >> 7) & 7;
    int opcode = x & 0x7f;

    /* FMOVECR #ccc,FPn — on-chip ROM constants */
    if ((x & 0xfc00) == 0x5c00) {
        static const struct { int idx; double v; } rom[] = {
            {0x00, 3.1415926535897932385},       /* pi   */
            {0x0b, 0.30102999566398119521},      /* log10(2) */
            {0x0c, 2.7182818284590452354},       /* e    */
            {0x0d, 1.4426950408889634074},       /* log2(e) */
            {0x0e, 0.43429448190325182765},      /* log10(e) */
            {0x0f, 0.0},
            {0x30, 0.69314718055994530942},      /* ln(2)  */
            {0x31, 2.30258509299404568402},      /* ln(10) */
        };
        int idx = x & 0x3f;
        double v;
        int found = 0;
        for (size_t i = 0; i < sizeof(rom)/sizeof(rom[0]); i++)
            if (rom[i].idx == idx) { v = rom[i].v; found = 1; break; }
        if (!found && idx >= 0x32 && idx <= 0x3f) {
            v = pow(10.0, (double)(1u << (idx - 0x32)));  /* 10^(2^k) */
            found = 1;
        }
        if (!found) return fpu_unimpl(c, "fmovecr", idx);
        c->vfpu.fp[dn] = la64m68_fp80_from_double(v);
        fp_set_cc(c, v);
        return 1;
    }

    if (rmode == 0 || rmode == 2) {     /* F<op>: rmode0 = FPm src, rmode2 = <ea> src */
        double s;
        if (rmode == 0) {
            s = la64m68_fp80_to_double(c->vfpu.fp[fmt]);   /* bits 12-10 = FPm */
        } else if (!fp_src_load(c, op, fmt, 0, &s)) {
            return fpu_unimpl(c, "fmt", fmt);
        }
        double r;
        switch (opcode) {
        case 0x00: r = s; break;                       /* FMOVE */
        case 0x01: r = floor(s + 0.5); break;          /* FINT (RN-ish) */
        case 0x03: r = s < 0 ? ceil(s) : floor(s); break;      /* FINTRZ */
        case 0x04: r = sqrt(s); break;                 /* FSQRT */
        case 0x18: r = fabs(s); break;                 /* FABS */
        case 0x1a: r = -s; break;                      /* FNEG */
        case 0x20: r = la64m68_fp80_to_double(c->vfpu.fp[dn]) / s; break; /* FDIV */
        case 0x22: r = la64m68_fp80_to_double(c->vfpu.fp[dn]) + s; break; /* FADD */
        case 0x23: r = la64m68_fp80_to_double(c->vfpu.fp[dn]) * s; break; /* FMUL */
        case 0x28: r = la64m68_fp80_to_double(c->vfpu.fp[dn]) - s; break; /* FSUB */
        default:
            return fpu_unimpl(c, "op", opcode);
        }
        c->vfpu.fp[dn] = la64m68_fp80_from_double(r);
        fp_set_cc(c, r);
        return 1;
    }
    if (rmode == 3) {                   /* FMOVE FPn,<ea> */
        double v = la64m68_fp80_to_double(c->vfpu.fp[dn]);
        int k = (x & 0x40) ? (x | ~0x7f) : (x & 0x7f);   /* sign-ext k */
        if (!fp_dst_store(c, op, fmt, v, k))
            return fpu_unimpl(c, "fmt", fmt);
        return 1;
    }
    if (rmode == 4 || rmode == 5) {     /* system regs: 4 = ea->FPU, 5 = FPU->ea */
        int sel = (x >> 10) & 7;
        /* FMOVEM of 1..3 longwords walks one resolved EA, not three */
        uint32_t a;
        if (!ea_addr_rmw(c, op, SZ_L, &a)) return -1;
        if (rmode == 5) {
            uint32_t o = a;
            if (sel & 4) { cwrite(c, o, c->vfpu.fpcr, SZ_L); o += 4; }
            if (sel & 2) { cwrite(c, o, c->vfpu.fpsr, SZ_L); o += 4; }
            if (sel & 1) { cwrite(c, o, c->vfpu.fpiar, SZ_L); }
        } else {
            uint32_t o = a;
            if (sel & 4) { c->vfpu.fpcr = (uint16_t)cread(c, o, SZ_L, 0); o += 4; }
            if (sel & 2) { c->vfpu.fpsr = cread(c, o, SZ_L, 0);           o += 4; }
            if (sel & 1) { c->vfpu.fpiar = cread(c, o, SZ_L, 0); }
        }
        return 1;
    }
    if (rmode == 6 || rmode == 7) {     /* FMOVEM: 6 = ea->FPn list, 7 = FPn list->ea */
        int load = (rmode == 6);
        uint8_t mask = x & 0xff;
        int m = ea_mode(op), r = ea_reg(op);
        if (!load && m == 4) {          /* -(An): reversed order, like MOVEM */
            uint32_t a = c->regs[8 + r];
            for (int i = 7; i >= 0; i--) {
                if (!(mask & (1u << (7 - i)))) continue;
                a -= 12;
                uint8_t b[12];
                la64m68_fp80_pack(c->vfpu.fp[i], b);
                for (int k = 0; k < 12; k++)
                    cwrite(c, a + k, b[k], SZ_B);
            }
            c->regs[8 + r] = a;
            return 1;
        }
        uint32_t a = ea_addr(c, op);
        for (int i = 0; i < 8; i++) {
            if (!(mask & (1u << i))) continue;
            uint8_t b[12];
            if (load) {
                for (int k = 0; k < 12; k++)
                    b[k] = (uint8_t)cread(c, a + k, SZ_B, 0);
                c->vfpu.fp[i] = la64m68_fp80_unpack(b);
            } else {
                la64m68_fp80_pack(c->vfpu.fp[i], b);
                for (int k = 0; k < 12; k++)
                    cwrite(c, a + k, b[k], SZ_B);
            }
            a += 12;
        }
        if (m == 3) c->regs[8 + r] = a; /* post-increment */
        return 1;
    }
    return fpu_unimpl(c, "rmode", rmode);
}

static int op_linef(la64m68_cpu *c, uint16_t op)
{
    /* 68040/060 PMMU ops (cpid 0 group, fixed opwords, no extension).
     * PFLUSH* = 0xF510-0xF51F; PTESTW (An) = 0xF548+r, PTESTR = 0xF568+r. */
    if ((op & 0xffe0) == 0xf500) {      /* PFLUSH / PFLUSHA(N) */
        if (!(c->sr & SR_S)) { la64m68_cpu_exception(c, 8); return 1; }
        /* Software table walk: no ATC to flush. Stage-2 host shadow
         * mappings re-derive from tables on next fault — nothing to do. */
        la64m68_trace("pmmu: pflush op=%04x (no ATC, nop)", op);
        return 1;
    }
    if ((op & 0xffe0) == 0xf540 || (op & 0xffe0) == 0xf560) {  /* PTESTW/PTESTR */
        if (!(c->sr & SR_S)) { la64m68_cpu_exception(c, 8); return 1; }
        int wr = (op & 0x20) != 0;
        uint32_t addr = c->regs[8 + (op & 7)], phys = 0, desc = 0;
        int rc = la64m68_vmmu_translate(&c->vmmu, c->mem, addr,
                                        !!(c->sr & SR_S), 0, wr,
                                        &phys, &desc);
        /* MMUSR subset: R(resident) + T/W/M/U flags from the walk */
        if (rc == LA64M68_VMMU_OK)
            c->vmmu.mmusr = LA64M68_VMMU_MMUSR_R |
                            (uint16_t)(desc & (LA64M68_VMMU_MMUSR_T |
                                LA64M68_VMMU_MMUSR_M | LA64M68_VMMU_MMUSR_U));
        else
            c->vmmu.mmusr = (uint16_t)(desc & LA64M68_VMMU_MMUSR_T) |
                            (rc == LA64M68_VMMU_F_PROT &&
                             (desc & 0x4u) ? LA64M68_VMMU_MMUSR_W : 0);
        la64m68_trace("pmmu: ptest%c (a%d) @%08x rc=%d",
                      wr ? 'w' : 'r', op & 7, addr, rc);
        return 1;
    }
    if ((op & 0xfff8) == 0xf620 || (op & 0xffe0) == 0xf600) {  /* MOVE16 */
        uint16_t x = fetch16(c);
        uint32_t sa = 0, da = 0;
        int sa_reg = -1, da_reg = -1;          /* An index if postinc */
        if ((op & 0xfff8) == 0xf620) {          /* MOVE16 (Ax)+,(Ay)+ */
            sa_reg = (x >> 12) & 7;
            da_reg = op & 7;
        } else {
            int an = (x >> 12) & 7;
            switch ((op >> 3) & 3) {
            case 0: sa = fetch32(c); da = c->regs[8 + an]; break;          /* abs.L,(An) */
            case 1: sa = c->regs[8 + an]; da = fetch32(c); break;          /* (An),abs.L */
            case 2: sa_reg = an; da = fetch32(c); break;                   /* (An)+,abs.L */
            case 3: sa = fetch32(c); da_reg = an; break;                   /* abs.L,(An)+ */
            }
        }
        if (sa_reg >= 0) sa = c->regs[8 + sa_reg];
        if (da_reg >= 0) da = c->regs[8 + da_reg];
        if (sa_reg >= 0) c->regs[8 + sa_reg] = sa + 16;
        if (da_reg >= 0) c->regs[8 + da_reg] = da + 16;
        for (int i = 0; i < 4; i++)
            cwrite(c, da + 4 * i, cread(c, sa + 4 * i, SZ_L, 0), SZ_L);
        return 1;
    }
    if ((op & 0x0e00) != 0x0200)        /* cp id != 1 (FPU): real LINE-F trap */
        goto trap;
    switch ((op >> 6) & 7) {
    case 0:
        return op_fpu_general(c, op);
    case 2: case 3: {                   /* FBcc.W / FBcc.L */
        int cc = op & 0x3f;
        int32_t disp = ((op >> 6) & 7) == 2 ? (int16_t)fetch16(c)
                                            : (int32_t)fetch32(c);
        if (fcc_true(c, cc))
            c->pc += disp;
        return 1;
    }
    case 1: {                           /* FDBcc / FScc: cc in extension word */
        uint16_t x = fetch16(c);
        int cc = x & 0x3f;
        if (ea_mode(op) == 1) {         /* FDBcc Dn,label: [op][cc-ext][disp] */
            int16_t disp = (int16_t)fetch16(c);
            if (fcc_true(c, cc)) return 1;
            int r = ea_reg(op);
            uint32_t lo = (c->regs[r] & 0xffff) - 1;
            c->regs[r] = (c->regs[r] & 0xffff0000) | (lo & 0xffff);
            if ((lo & 0xffff) != 0xffff)
                c->pc += disp - 4;       /* disp relative to cc-ext word */
            return 1;
        }
        /* FScc <ea> */
        ea_write(c, op, SZ_B, fcc_true(c, cc) ? 0xff : 0x00);
        return 1;
    }
    case 7:                             /* FSAVE/FRESTORE etc: later */
        fetch16(c);
        return -1;
    default:
        return -1;
    }
trap:
    la64m68_cpu_exception(c, 11);
    return 1;
}

static int dispatch(la64m68_cpu *c, uint16_t op)
{
    switch (op >> 12) {
    case 0x0: return op_line0(c, op);
    case 0x1: return op_move(c, op, SZ_B);
    case 0x2: return op_move(c, op, SZ_L);
    case 0x3: return op_move(c, op, SZ_W);
    case 0x4: return op_line4(c, op);
    case 0x5: return op_line5(c, op);
    case 0x6: return op_line6(c, op);
    case 0x7:                            /* MOVEQ */
        c->regs[(op >> 9) & 7] = (uint32_t)(int32_t)(int8_t)(op & 0xff);
        set_nz(c, c->regs[(op >> 9) & 7], SZ_L);
        return 1;
    case 0x8:                            /* OR/DIV/SBCD */
        if ((op & 0xf1c0) == 0x80c0) return op_div(c, op, 0); /* DIVU.W */
        if ((op & 0xf1c0) == 0x81c0) return op_div(c, op, 1); /* DIVS.W */
        if ((op & 0xf1f0) == 0x8100) return op_bcd(c, op, 1); /* SBCD */
        if ((op & 0xf1f8) == 0x8140 || (op & 0xf1f8) == 0x8148)
            return op_pack(c, op, 0);                           /* PACK */
        if ((op & 0xf1f8) == 0x8180 || (op & 0xf1f8) == 0x8188)
            return op_pack(c, op, 1);                           /* UNPK */
        return op_ded(c, op, 0);
    case 0x9:                            /* SUB/SUBA/SUBX */
        if (((op >> 6) & 3) == 3) return op_adda_suba(c, op, 1);
        if ((op & 0x100) && ea_mode(op) <= 1) return op_addx_subx(c, op, 1);
        return op_ded(c, op, 2);
    case 0xa:
        la64m68_cpu_exception(c, 10);        /* LINE A */
        return 1;
    case 0xb:                            /* CMP/CMPA/EOR */
        if (((op >> 6) & 3) == 3) {           /* CMPA: sign-extended src vs An */
            int size = (op & 0x100) ? SZ_L : SZ_W;
            uint32_t v = ea_read(c, op, size);
            if (size == SZ_W) v = (uint32_t)(int32_t)(int16_t)v;
            uint32_t d = c->regs[8 + ((op >> 9) & 7)];
            flags_sub(c, v, d, d - v, SZ_L, 1);
            return 1;
        }
        if (op & 0x100) {
            if (ea_mode(op) == 1) return op_cmpm(c, op);      /* CMPM */
            return op_ded(c, op, 4);                          /* EOR */
        }
        return op_ded(c, op, 5);                   /* CMP */
    case 0xc:                            /* AND/MUL/EXG */
        if ((op & 0xf1c0) == 0xc0c0) return op_mul(c, op, 0); /* MULU.W */
        if ((op & 0xf1c0) == 0xc1c0) return op_mul(c, op, 1); /* MULS.W */
        if ((op & 0xf1f0) == 0xc100) return op_bcd(c, op, 0); /* ABCD */
        if ((op & 0xf100) == 0xc100) {                        /* EXG */
            int mode = (op >> 3) & 0x1f, rx = (op >> 9) & 7, ry = op & 7;
            uint32_t t;
            if (mode == 8)       { t = c->regs[rx]; c->regs[rx] = c->regs[ry]; c->regs[ry] = t; }
            else if (mode == 9)  { t = c->regs[8+rx]; c->regs[8+rx] = c->regs[8+ry]; c->regs[8+ry] = t; }
            else if (mode == 17) { t = c->regs[rx]; c->regs[rx] = c->regs[8+ry]; c->regs[8+ry] = t; }
            else return -1;
            return 1;
        }
        return op_ded(c, op, 1);
    case 0xd:                            /* ADD/ADDA/ADDX */
        if (((op >> 6) & 3) == 3) return op_adda_suba(c, op, 0);
        if ((op & 0x100) && ea_mode(op) <= 1) return op_addx_subx(c, op, 0);
        return op_ded(c, op, 3);
    case 0xe: return op_shift(c, op);
    default:                             /* 0xf LINE F / FPU */
        return op_linef(c, op);
    }
}

/* ---- public ---- */

void la64m68_cpu_reset(la64m68_cpu *cpu, la64m68_memory *mem, la64m68_plugin *plugin)
{
    memset(cpu, 0, sizeof(*cpu));
    cpu->mem = mem;
    cpu->plugin = plugin;
    cpu->ssp = la64m68_mem_read32(mem, 0x00000000);
    cpu->pc  = la64m68_mem_read32(mem, 0x00000004);
    cpu->regs[15] = cpu->ssp;           /* A7 = active stack (S=1 -> SSP) */
    cpu->sr  = 0x2700;
    la64m68_trace("cpu: reset ssp=%08x pc=%08x", cpu->ssp, cpu->pc);
    if (plugin && plugin->reset)
        plugin->reset(plugin->ctx);
}

int la64m68_cpu_step(la64m68_cpu *cpu)
{
    if (!cpu->mem) return -1;

    /* interrupt check: ipl > SR mask, or level 7 (NMI). Autovectored
     * (68k devices on PiS report no vector -> vector 24+level). */
    int ipl = cpu->ipl;
    if (ipl && (ipl == 7 || ipl > ((cpu->sr >> 8) & 7))) {
        la64m68_cpu_exception(cpu, 24 + ipl);
        cpu->sr = (cpu->sr & ~0x0700) | (uint16_t)(ipl << 8);
        /* Acknowledged: drop the pending level, otherwise the same request
         * would re-trigger on every step. The device re-raises while its
         * condition persists (vAGA INTREQ / PiS IPL lines are re-polled). */
        cpu->ipl = 0;
        cpu->cycles += 4;                    /* acknowledge boundary */
        return 0;
    }

    if (cpu->fault) {
        la64m68_cpu_exception_fmt(cpu, 2, 7);  /* access fault -> fmt 7 */
        cpu->fault = 0;
    }
    uint16_t op = fetch16(cpu);
    if (cpu->fault) {                    /* fetch faulted */
        la64m68_cpu_exception_fmt(cpu, 2, 7);
        cpu->fault = 0;
        return 0;
    }
    int rc = dispatch(cpu, op);
    if (cpu->fault) {                    /* data access faulted mid-instruction */
        la64m68_cpu_exception_fmt(cpu, 2, 7);
        cpu->fault = 0;
        return 0;
    }
    if (rc < 0) {
        la64m68_trace("cpu: unimplemented op=%04x pc=%08x", op, cpu->pc - 2);
        la64m68_cpu_exception(cpu, 11);      /* LINE F as unimplemented marker */
        return 1;
    }
    cpu->cycles += 4;
    if (cpu->plugin && cpu->plugin->tick)
        cpu->plugin->tick(cpu->plugin->ctx, 4);
    return rc ? 0 : 1;                   /* 1 = stopped (STOP) */
}
