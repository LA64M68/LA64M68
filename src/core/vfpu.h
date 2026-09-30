#ifndef LA64M68_VFPU_H
#define LA64M68_VFPU_H

#include <stdint.h>

/* 68060 vFPU state. Data registers are kept in 80-bit extended format,
 * the native 68k register representation. FP32/FP64 arithmetic runs on the
 * host ARM64 FPU; FP80 and the non-hardware FP ops run in software. */

typedef struct la64m68_fp80 {
    uint16_t se;    /* sign(15) + biased exponent(14:0), bias 0x3fff */
    uint64_t m;     /* integer bit (63) + 63 fraction bits */
} la64m68_fp80;

typedef struct la64m68_vfpu {
    la64m68_fp80 fp[8];
    uint16_t fpcr;  /* mode/exception-enable */
    uint32_t fpsr;  /* condition codes + accrued exceptions + status */
    uint32_t fpiar; /* instruction address of last FP op */
} la64m68_vfpu;

void la64m68_vfpu_reset(la64m68_vfpu *f);

/* FP80 <-> IEEE754 double. Conversion to fp80 is exact; back to double
 * follows round-to-nearest via host ldexp path (inexact flag left to caller). */
la64m68_fp80 la64m68_fp80_from_double(double d);
double       la64m68_fp80_to_double(la64m68_fp80 v);

/* 68k 96-bit register image: byte 0..1 sign/exp (as stored big-endian),
 * bytes 4..11 mantissa. Pack/unpack for FMOVE.X/M register file access. */
void         la64m68_fp80_pack(la64m68_fp80 v, uint8_t out[12]);
la64m68_fp80 la64m68_fp80_unpack(const uint8_t in[12]);

/* 68k packed-decimal format (12 bytes):
 *   word0/1:  bit31 mantissa sign, bit30 exponent sign,
 *             bits27..16 = 3-digit BCD exponent, bits3..0 = mantissa MSD
 *   word2/3:  16 more mantissa digits (BCD nibbles) — 17 digits total.
 * k = static k-factor (sign-extended, -64..63) on store. */
la64m68_fp80 la64m68_fp80_from_packed(const uint8_t in[12]);
void         la64m68_fp80_to_packed(la64m68_fp80 v, int k, uint8_t out[12]);

#endif
