#include "vfpu.h"
#include "debug.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void la64m68_vfpu_reset(la64m68_vfpu *f)
{
    memset(f, 0, sizeof(*f));
}

#define FP80_BIAS   0x3fff
#define DBL_BIAS    1023

la64m68_fp80 la64m68_fp80_from_double(double d)
{
    union { double d; uint64_t u; } u = { d };
    la64m68_fp80 r;

    uint16_t sign = (uint16_t)(u.u >> 63);
    int      exp  = (int)((u.u >> 52) & 0x7ff);
    uint64_t frac = u.u & ((1ull << 52) - 1);

    if (exp == 0) {
        if (frac == 0) { /* zero */
            r.se = (uint16_t)(sign << 15);
            r.m  = 0;
            return r;
        }
        /* subnormal: normalize so bit 52 holds the integer bit, then move it
         * to bit 63 of the fp80 significand. An extra shift would push the
         * integer bit to bit 64 and lose it. */
        int shift = 0;
        while (!(frac & (1ull << 52))) { frac <<= 1; shift++; }
        r.se = (uint16_t)((sign << 15) | (FP80_BIAS - DBL_BIAS + 1 - shift));
        r.m  = frac << 11;
        return r;
    }
    if (exp == 0x7ff) { /* inf / nan */
        r.se = (uint16_t)((sign << 15) | 0x7fff);
        r.m  = frac ? (0x8000000000000000ull | (frac << 11))
                    : 0x8000000000000000ull;
        return r;
    }

    r.se = (uint16_t)((sign << 15) | (exp - DBL_BIAS + FP80_BIAS));
    r.m  = (1ull << 63) | (frac << 11);
    return r;
}

double la64m68_fp80_to_double(la64m68_fp80 v)
{
    uint16_t sign = v.se & 0x8000;
    int      e    = v.se & 0x7fff;
    uint64_t m    = v.m;

    if ((m & 0x8000000000000000ull) == 0 && e == 0)
        return sign ? -0.0 : 0.0;
    if (e == 0x7fff) {
        if (m & 0x7fffffffffffffffull) return NAN;
        return sign ? -INFINITY : INFINITY;
    }

    /* value = m * 2^(e-bias-63); host rounds the binary result. */
    double d = ldexp((double)(m >> 11), e - FP80_BIAS - 52);
    d += ldexp((double)(m & 0x7ff), e - FP80_BIAS - 63);
    return sign ? -d : d;
}

void la64m68_fp80_pack(la64m68_fp80 v, uint8_t out[12])
{
    memset(out, 0, 12);
    out[0] = (uint8_t)(v.se >> 8);
    out[1] = (uint8_t)v.se;
    for (int i = 0; i < 8; i++)
        out[4 + i] = (uint8_t)(v.m >> (56 - 8 * i));
}

la64m68_fp80 la64m68_fp80_unpack(const uint8_t in[12])
{
    la64m68_fp80 v;
    v.se = (uint16_t)(((uint16_t)in[0] << 8) | in[1]);
    v.m = 0;
    for (int i = 0; i < 8; i++)
        v.m |= (uint64_t)in[4 + i] << (56 - 8 * i);
    return v;
}

la64m68_fp80 la64m68_fp80_from_packed(const uint8_t in[12])
{
    uint32_t dw1 = ((uint32_t)in[0] << 24) | ((uint32_t)in[1] << 16) |
                   ((uint32_t)in[2] << 8) | in[3];
    uint32_t dw2 = ((uint32_t)in[4] << 24) | ((uint32_t)in[5] << 16) |
                   ((uint32_t)in[6] << 8) | in[7];
    uint32_t dw3 = ((uint32_t)in[8] << 24) | ((uint32_t)in[9] << 16) |
                   ((uint32_t)in[10] << 8) | in[11];

    char s[48], *p = s;
    if (dw1 & 0x80000000) *p++ = '-';
    *p++ = (char)('0' + (dw1 & 0xf));
    *p++ = '.';
    for (int i = 28; i >= 0; i -= 4) *p++ = (char)('0' + ((dw2 >> i) & 0xf));
    for (int i = 28; i >= 0; i -= 4) *p++ = (char)('0' + ((dw3 >> i) & 0xf));
    *p++ = 'e';
    if (dw1 & 0x40000000) *p++ = '-';
    *p++ = (char)('0' + ((dw1 >> 24) & 0xf));
    *p++ = (char)('0' + ((dw1 >> 20) & 0xf));
    *p++ = (char)('0' + ((dw1 >> 16) & 0xf));
    *p = 0;
    return la64m68_fp80_from_double(strtod(s, NULL));
}

void la64m68_fp80_to_packed(la64m68_fp80 v, int k, uint8_t out[12])
{
    uint32_t dw1 = 0, dw2 = 0, dw3 = 0;
    char s[48];
    double dv = la64m68_fp80_to_double(v);

    /* NaN and Inf have no packed-decimal representation, and neither has an
     * fp80 operand that the double conversion cannot carry. Emit a defined
     * zero instead.
     *
     * Note this guard and the digit clamp below are deliberately redundant
     * (defence in depth): the clamp alone already makes the output defined,
     * and the guard alone already keeps the parser away from "nan"/"inf".
     * Neither is individually pinned by a test for that reason -- removing
     * both is what breaks the behaviour. This guard also keeps the intent
     * explicit and traces the case.
     *
     * The 68881 encoding for these cases is NOT implemented here -- that
     * needs the FPSRM and is deliberately not guessed. */
    if (!isfinite(dv)) {
        la64m68_trace("vfpu: to_packed on %s -> defined zero",
                      ((v.se & 0x7fff) == 0x7fff)
                          ? "NaN/Inf" : "an operand outside double range");
        memset(out, 0, 12);
        if (v.se & 0x8000) out[0] = 0x80;
        return;
    }

    snprintf(s, sizeof(s), "%.16e", dv);
    /* s = [-]d.dddddddddddddddde[-]XXX : mantissa digits d[0..16] */

    char *p = s;
    if (*p == '-') { dw1 |= 0x80000000; p++; }
    if (*p == '+') p++;
    char d[17];
    d[0] = *p++;
    if (*p == '.') p++;
    for (int i = 1; i < 17; i++) d[i] = (*p >= '0' && *p <= '9') ? *p++ : '0';
    int esign = 0, exp = 0;
    if (*p == 'e' || *p == 'E') {
        p++;
        if (*p == '-') { esign = 1; p++; }
        if (*p == '+') p++;
        while (*p >= '0' && *p <= '9') exp = exp * 10 + (*p++ - '0');
    }

    /* k-factor: k<0 rounds/truncates the mantissa at k+exp-1 digits,
     * k>0 zeroes the last k mantissa digits. NOTE: the truncation rules are
     * NOT pinned to Musashi on purpose -- its store_pack_float80() rounds
     * with a bare `ch[k]++` (no carry, turns '9' into ':') and parses the
     * exponent as `(exp << 4) | digit`, which is wrong for exponents >= 10.
     * We keep the true decimal exponent and propagate the carry properly. */
    if (k < 0 && k >= -13) {
        int kk = -k + exp - 1;                 /* last kept digit index */
        if (kk < 0) kk = 0;
        if (kk < 16 && d[kk + 1] >= '5') {
            int i;
            for (i = kk; i >= 0; i--) {        /* carry propagate */
                if (d[i] < '9') { d[i]++; break; }
                d[i] = '0';
            }
            if (i < 0) {
                /* The carry ran past the most significant digit, so "99..9"
                 * became "100..0": the MSD is 1 and the *signed* decimal
                 * exponent increments by one. Dropping the carry to the MSD
                 * would make the value 0.000e+00, and incrementing only the
                 * magnitude would turn 9.999e-1 into 0.000e-2 instead of
                 * 1.000e+00. */
                d[0] = '1';
                if (esign) { if (exp > 0) exp--; if (exp == 0) esign = 0; }
                else       exp++;
            }
        }
        for (int i = kk + 1; i < 17; i++) d[i] = '0';
    } else if (k > 0) {
        int keep = 17 - (k > 17 ? 17 : k);
        for (int i = keep; i < 17; i++) d[i] = '0';
    }

    /* Every digit is clamped to 0..9: a non-digit must never reach the BCD
     * nibbles, where `c - '0'` can exceed the nibble and corrupt the
     * reserved and exponent fields of dw1. */
#define PCK_DIGIT(i) ((d[i] >= '0' && d[i] <= '9') ? (uint32_t)(d[i] - '0') : 0u)
    dw1 |= PCK_DIGIT(0);                       /* MSD in low nibble */
    for (int i = 1; i <= 8; i++)  dw2 = (dw2 << 4) | PCK_DIGIT(i);
    for (int i = 9; i <= 16; i++) dw3 = (dw3 << 4) | PCK_DIGIT(i);
#undef PCK_DIGIT
    if (esign) dw1 |= 0x40000000;
    if (exp > 999) exp = 999;                  /* 3-digit BCD exponent */
    dw1 |= ((uint32_t)(exp / 100) << 24) |
           ((uint32_t)((exp / 10) % 10) << 20) |
           ((uint32_t)(exp % 10) << 16);

    out[0] = dw1 >> 24; out[1] = dw1 >> 16; out[2] = dw1 >> 8; out[3] = dw1;
    out[4] = dw2 >> 24; out[5] = dw2 >> 16; out[6] = dw2 >> 8; out[7] = dw2;
    out[8] = dw3 >> 24; out[9] = dw3 >> 16; out[10] = dw3 >> 8; out[11] = dw3;
}
