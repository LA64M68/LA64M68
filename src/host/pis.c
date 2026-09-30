#define _DEFAULT_SOURCE
#include "pis.h"
#include "debug.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* ---- runtime host detection --------------------------------------------
 * A PiS host is an RPi exposing GPIO to userspace (/dev/gpiomem) wired to
 * the Amiga bus. The peripheral base differs per SoC; read it from the
 * device tree ranges instead of hardcoding 0x20000000/0x3F000000/0xFE000000.
 */

static uint32_t dt_periph_base(void)
{
    /* /proc/device-tree/soc/ranges: <bus-addr> <cpu-addr> <size>, the
     * cpu-addr of the first 32-bit entry is the peripheral base. */
    FILE *f = fopen("/proc/device-tree/soc/ranges", "rb");
    if (!f)
        return 0;
    uint8_t b[64];
    size_t n = fread(b, 1, sizeof(b), f);
    fclose(f);
    if (n < 12)
        return 0;
    /* Entry layout varies (32/64-bit parent cells). Scan the blob for a
     * known RPi peripheral base instead of trusting fixed offsets:
     * RPi1 0x20000000, RPi2/3 0x3f000000, RPi4 0xfe000000, RPi5 0x107c000000
     * region 0xfc/0xfa low word. Bus-side addresses (0x7c/0x7e...) skip. */
    static const uint32_t bases[] = {
        0x20000000u, 0x3f000000u, 0xfe000000u, 0xfc000000u, 0xfa000000u
    };
    for (size_t i = 0; i + 4 <= n; i += 4) {
        uint32_t w = ((uint32_t)b[i] << 24) | ((uint32_t)b[i + 1] << 16) |
                     ((uint32_t)b[i + 2] << 8) | b[i + 3];
        for (size_t k = 0; k < sizeof(bases) / sizeof(bases[0]); k++)
            if (w == bases[k])
                return w;
    }
    return 0;
}

/* ---- PiS GPIO register protocol ------------------------------------------
 * PiStorm32-family: an FPGA state machine owns the 68k bus; the host talks
 * to it through a small register file bit-banged over GPIO.
 *   data  pins 8..23  (D0..D15)
 *   addr  pins 24..26 (register select A0..A2)
 *   RD pin 6, WR pin 7, TXN(busy) pin 3
 * Regs: 0=data lo, 1=data hi, 2=addr lo, 3=addr hi/txn-launch, 4=status.
 * Encoding knowledge from the MIT-licensed PiStorm ps_protocol reference;
 * this implementation is our own (timeouts, trace, own structure).
 */

#define PIN_TXN   3
#define PIN_RD    6
#define PIN_WR    7
#define PIN_D(x)  (8 + (x))
#define PIN_A(x)  (24 + (x))

#define GPF_OUT(i) (1u << (((i) % 10) * 3))   /* pin i as output */

#define FSEL_IN0  (GPF_OUT(PIN_WR) | GPF_OUT(PIN_RD))
#define FSEL_IN1  0
#define FSEL_IN2  (GPF_OUT(PIN_A(2)) | GPF_OUT(PIN_A(1)) | GPF_OUT(PIN_A(0)))
#define FSEL_OUT0 (FSEL_IN0 | GPF_OUT(PIN_D(1)) | GPF_OUT(PIN_D(0)))
#define FSEL_OUT1 (GPF_OUT(PIN_D(11)) | GPF_OUT(PIN_D(10)) | \
                   GPF_OUT(PIN_D(9))  | GPF_OUT(PIN_D(8))  | \
                   GPF_OUT(PIN_D(7))  | GPF_OUT(PIN_D(6))  | \
                   GPF_OUT(PIN_D(5))  | GPF_OUT(PIN_D(4))  | \
                   GPF_OUT(PIN_D(3))  | GPF_OUT(PIN_D(2)))
#define FSEL_OUT2 (FSEL_IN2 | GPF_OUT(PIN_D(15)) | GPF_OUT(PIN_D(14)) | \
                   GPF_OUT(PIN_D(13)) | GPF_OUT(PIN_D(12)))

#define REG_DATA_LO  0
#define REG_DATA_HI  1
#define REG_ADDR_LO  2
#define REG_ADDR_HI  3
#define REG_STATUS   4
#define REG_CONTROL  4

#define CONTROL_REQ_BM      (1u << 0)
#define CONTROL_DRIVE_RESET (1u << 1)
#define CONTROL_DRIVE_HALT  (1u << 2)
#define CONTROL_DRIVE_INT2  (1u << 3)
#define CONTROL_DRIVE_INT6  (1u << 4)

#define TXN_SZ_SHIFT 8
#define TXN_RW_READ  (1u << 10)
#define TXN_FC_SHIFT 11
#define SZ_BYTE 0
#define SZ_WORD 1
#define SZ_LONG 3

/* Data bus only: GPIO 8..23. Pins 0..2 are IPL0..2 and pin 3 is TXN -- they
 * are driven by the FPGA and must never be written from here. The previous
 * mask 0x0fffff3f also covered pins 0..5 and was harmless only as long as
 * those pins happened to sit in the input configuration. */
#define DATA_PINS     0x00ffff00u
#define ADDR_PINS     (7u << PIN_A(0))   /* A0..A2 (GPIO 24..26) */
#define TXN_TIMEOUT   2000000u      /* spin budget ~ms-scale on RPi4 */

/* Efinix FPGA config (PiStorm32-lite): passive-serial SPI bitbang during
 * CRESET low. Programming pins reuse the GPIO header:
 * CRESET1=6 CRESET2=7 TESTN=17 CCK=22 SS=24 CDI0=10 CBUS0=14 CBUS1=15 CBUS2=18 */
#define PIN_CRESET1 6
#define PIN_CRESET2 7
#define PIN_TESTN   17
#define PIN_CCK     22
#define PIN_SS      24
#define PIN_CDI0    10
#define PIN_CBUS0   14
#define PIN_CBUS1   15
#define PIN_CBUS2   18

struct la64m68_pis {
    int             fd;          /* /dev/gpiomem, -1 when absent */
    volatile uint32_t *gpio;     /* mapped GPFSEL/GPSET/GPLEV block */
    volatile uint32_t *gpset;    /* gpio + 7  */
    volatile uint32_t *gpclr;    /* gpio + 10 */
    volatile uint32_t *gplev;    /* gpio + 13 */
    uint32_t        periph_base;
    uint32_t        fc;          /* 68k function codes (0 = none yet) */
    int             present;
    int             armed;       /* hardware arming gate (--pis-arm) */
    int             variant;     /* resolved LA64M68_PIS_* descriptor */
    uint64_t        transfers;   /* Amiga bus cycles so far (guards fpga_load) */
    int             poll_txn;    /* inter-cycle TXN prefetch-wait toggle */
    la64m68_memory *chipset;
    la64m68_memory *cia;
    la64m68_memory *zorro2;
};

static void bus_dir(la64m68_pis *p, int out)
{
    if (out) {
        p->gpio[0] = FSEL_OUT0;
        p->gpio[1] = FSEL_OUT1;
        p->gpio[2] = FSEL_OUT2;
    } else {
        p->gpio[0] = FSEL_IN0;
        p->gpio[1] = FSEL_IN1;
        p->gpio[2] = FSEL_IN2;
    }
}

/* Protocol-safe idle -- the state the driver must be in whenever it is not
 * in the middle of a transfer, and what every exit path leaves behind.
 *
 * Deliberately NOT "all pins high-Z": PIN_RD/PIN_WR double as CRESET1/2 of
 * the FPGA, so leaving them floating can be decoded as a register write and
 * turn into an unintended Amiga bus cycle. They stay driven but INACTIVE
 * (high); only the data bus is released to inputs.
 *
 * Every transfer already ends here, so even an abrupt exit -- SIGKILL, where
 * no cleanup runs at all -- leaves the bus alone. That is what makes this
 * driver safe by construction rather than by tidy shutdown. */
static void bus_release(la64m68_pis *p)
{
    *p->gpclr = DATA_PINS;                        /* data output latch low */
    bus_dir(p, 0);                                /* data pins -> inputs */
    *p->gpset = (1u << PIN_RD) | (1u << PIN_WR);  /* strobes inactive */
}

static void fpga_prog_pins(la64m68_pis *p)
{
    /* outputs for config: FSEL0 pins 0-9, FSEL1 pins 10-19, FSEL2 20-29.
     * (PiS32-lite pinout: CRESET 6/7, CDI 1/8/9/10/11/13/16/25, TESTN 17,
     *  CBUS 14/15/18, CCK 22, SS 24) */
    p->gpio[0] = GPF_OUT(1) | GPF_OUT(6) | GPF_OUT(7) | GPF_OUT(8) |
                 GPF_OUT(9);
    p->gpio[1] = GPF_OUT(10) | GPF_OUT(11) | GPF_OUT(13) | GPF_OUT(14) |
                 GPF_OUT(15) | GPF_OUT(16) | GPF_OUT(17) | GPF_OUT(18);
    p->gpio[2] = GPF_OUT(22) | GPF_OUT(24) | GPF_OUT(25);

    *p->gpset = (1u << PIN_CRESET1) | (1u << PIN_CRESET2);
    *p->gpset = (1u << PIN_CBUS0) | (1u << PIN_CBUS1) | (1u << PIN_CBUS2);
    *p->gpset = (1u << PIN_TESTN) | (1u << PIN_CCK);
    *p->gpclr = 1u << PIN_SS;

    /* reset pulse: latch CBUS=x1 SPI config mode */
    *p->gpclr = (1u << PIN_CRESET1) | (1u << PIN_CRESET2);
    usleep(10000);                          /* glitch-filter RC discharge */
    *p->gpset = (1u << PIN_CRESET1) | (1u << PIN_CRESET2);
    usleep(2000);
}

static void fpga_shift_byte(la64m68_pis *p, uint8_t d)
{
    for (uint8_t m = 0x80; m; m >>= 1) {
        *p->gpclr = 1u << PIN_CCK;
        if (d & m) *p->gpset = 1u << PIN_CDI0;
        else       *p->gpclr = 1u << PIN_CDI0;
        *p->gpset = 1u << PIN_CCK;
        *p->gpset = 1u << PIN_CCK;          /* ~50% duty */
    }
    *p->gpclr = 1u << PIN_CCK;
}

/* Load an Efinix bitstream file into the FPGA. Returns 0 on completion. */
int la64m68_pis_fpga_load(la64m68_pis *p, const char *path)
{
    /* Refuse before anything is touched. Reconfiguring the FPGA pulls the
     * device that owns the 68k bus out from under the Amiga, which is the
     * most damaging operation this driver can perform -- so the arming gate
     * is checked first, before even the argument or mapping checks. */
    if (!p || !p->armed) {
        la64m68_trace("pis: fpga load REFUSED (disarmed, --pis-arm not given)");
        return -1;
    }
    if (!la64m68_pis_variant_is_fpga(p->variant)) {
        la64m68_trace("pis: fpga load REFUSED (variant %s has no "
                      "reconfigurable FPGA)",
                      la64m68_pis_variant_name(p->variant));
        return -1;
    }
    if (!p->gpio || !path)
        return -1;
    if (p->transfers) {
        la64m68_trace("pis: fpga load REFUSED (%llu Amiga bus cycles already "
                      "done -- programming must precede any bus traffic)",
                      (unsigned long long)p->transfers);
        return -1;
    }
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    la64m68_trace("pis: fpga load %s", path);
    fpga_prog_pins(p);
    uint8_t buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        for (size_t i = 0; i < n; i++)
            fpga_shift_byte(p, buf[i]);
    fclose(f);
    for (int i = 0; i < 1000; i++)          /* startup clocks */
        fpga_shift_byte(p, 0x00);
    *p->gpset = 1u << PIN_SS;
    bus_release(p);                         /* back to bus register mode */
    la64m68_trace("pis: fpga bitstream shifted");
    return 0;
}


/* GPSET/GPCLR are set-only registers: a bit written as 0 has no effect, so a
 * stale address from the previous transfer would survive. A0..A2 must be
 * cleared explicitly before the new register select is driven. */
static void reg_write(la64m68_pis *p, uint32_t reg, uint32_t data)
{
    *p->gpclr = ADDR_PINS;
    *p->gpset = (data << PIN_D(0)) | (reg << PIN_A(0));
    *p->gpclr = 1u << PIN_WR;   /* WR low, hold ~3 GPIO writes (Pi4 pace) */
    *p->gpclr = 1u << PIN_WR;
    *p->gpclr = 1u << PIN_WR;
    *p->gpset = 1u << PIN_WR;
    *p->gpclr = DATA_PINS;   /* release data, keep the strobes driven */
}

static uint32_t reg_read(la64m68_pis *p, uint32_t reg)
{
    *p->gpclr = ADDR_PINS;
    *p->gpset = reg << PIN_A(0);
    *p->gpclr = 1u << PIN_RD;
    *p->gpclr = 1u << PIN_RD;
    *p->gpclr = 1u << PIN_RD;
    *p->gpclr = 1u << PIN_RD;
    uint32_t d = *p->gplev;
    *p->gpset = 1u << PIN_RD;
    *p->gpclr = DATA_PINS;
    return (d >> PIN_D(0)) & 0xffff;
}

/* returns 0 when TXN clears, -1 on timeout (FPGA/bus not responding) */
static int txn_wait(la64m68_pis *p)
{
    for (uint32_t i = 0; i < TXN_TIMEOUT; i++)
        if (!(*p->gplev & (1u << PIN_TXN)))
            return 0;
    return -1;
}

static uint32_t pis_bus_read(la64m68_pis *p, uint32_t addr, int size)
{
    if (!p->armed) {                 /* defence in depth: windows are gated */
        la64m68_trace("pis: read @%06x refused (disarmed)", addr);
        return size == 4 ? 0xffffffffu : size == 2 ? 0xffffu : 0xffu;
    }
    p->transfers++;
    int sz = size == 1 ? SZ_BYTE : size == 4 ? SZ_LONG : SZ_WORD;
    bus_dir(p, 1);
    reg_write(p, REG_ADDR_LO, addr & 0xffff);
    if (p->poll_txn && txn_wait(p) < 0)
        goto timeout;
    reg_write(p, REG_ADDR_HI,
              TXN_RW_READ | (p->fc << TXN_FC_SHIFT) |
              ((uint32_t)sz << TXN_SZ_SHIFT) | ((addr >> 16) & 0xff));
    bus_dir(p, 0);
    if (txn_wait(p) < 0)
        goto timeout;
    uint32_t d = reg_read(p, REG_DATA_LO);
    if (size == 4)
        d |= reg_read(p, REG_DATA_HI) << 16;
    else if (size == 1)
        d &= 0xff;
    p->poll_txn = 0;
    return d;
timeout:
    bus_dir(p, 0);
    la64m68_trace("pis: read @%06x timeout -> open-bus", addr);
    /* open 68k bus floats to all ones (pull-ups), at the requested width */
    return size == 4 ? 0xffffffffu : size == 2 ? 0xffffu : 0xffu;
}

static void pis_bus_write(la64m68_pis *p, uint32_t addr, uint32_t v, int size)
{
    if (!p->armed) {                 /* defence in depth: windows are gated */
        la64m68_trace("pis: write @%06x refused (disarmed)", addr);
        return;
    }
    p->transfers++;
    int sz = size == 1 ? SZ_BYTE : size == 4 ? SZ_LONG : SZ_WORD;
    bus_dir(p, 1);
    reg_write(p, REG_DATA_LO, v & 0xffff);
    if (size == 4)
        reg_write(p, REG_DATA_HI, (v >> 16) & 0xffff);
    reg_write(p, REG_ADDR_LO, addr & 0xffff);
    if (p->poll_txn && txn_wait(p) < 0) {
        bus_dir(p, 0);
        la64m68_trace("pis: write @%06x timeout", addr);
        return;
    }
    reg_write(p, REG_ADDR_HI,
              (p->fc << TXN_FC_SHIFT) |
              ((uint32_t)sz << TXN_SZ_SHIFT) | ((addr >> 16) & 0xff));
    bus_dir(p, 0);
    p->poll_txn = 1;
}

/* Window facade. The router hands every sub-memory the ABSOLUTE guest
 * address, so the base must not be added again -- doing so wrote to
 * base+addr (0x00dff000 + 0x00dff180 = 0x01bfe180) and every cycle timed
 * out on an address that does not exist. */
typedef struct { la64m68_pis *p; uint32_t base; } pis_win;

static uint8_t  pw_r8(void *c, uint32_t a)
{ pis_win *w = c; return (uint8_t)pis_bus_read(w->p, a, 1); }
static uint16_t pw_r16(void *c, uint32_t a)
{ pis_win *w = c; return (uint16_t)pis_bus_read(w->p, a, 2); }
static uint32_t pw_r32(void *c, uint32_t a)
{ pis_win *w = c; return (uint32_t)pis_bus_read(w->p, a, 4); }
static void pw_w8(void *c, uint32_t a, uint8_t v)
{ pis_win *w = c; pis_bus_write(w->p, a, v, 1); }
static void pw_w16(void *c, uint32_t a, uint16_t v)
{ pis_win *w = c; pis_bus_write(w->p, a, v, 2); }
static void pw_w32(void *c, uint32_t a, uint32_t v)
{ pis_win *w = c; pis_bus_write(w->p, a, v, 4); }

static la64m68_memory *mk_win(la64m68_pis *p, uint32_t base)
{
    pis_win *w = malloc(sizeof(*w));
    la64m68_memory *m = malloc(sizeof(*m));
    if (!w || !m) { free(w); free(m); return NULL; }
    *w = (pis_win){ p, base };
    *m = (la64m68_memory){ w, pw_r8, pw_r16, pw_r32, pw_w8, pw_w16, pw_w32 };
    return m;
}

/* ---- lifecycle --------------------------------------------------------- */

const char *la64m68_pis_variant_name(int v)
{
    switch (v) {
    case LA64M68_PIS_CLASSIC:  return "classic";
    case LA64M68_PIS_32:       return "32";
    case LA64M68_PIS_32LITE:   return "32lite";
    case LA64M68_PIS_PISTORMX: return "pistormx";
    case LA64M68_PIS_AUTO:     return "auto";
    default:                   return "?";
    }
}

int la64m68_pis_variant_parse(const char *s)
{
    if (!s || !*s) return LA64M68_PIS_AUTO;
    if (!strcmp(s, "auto"))     return LA64M68_PIS_AUTO;
    if (!strcmp(s, "classic"))  return LA64M68_PIS_CLASSIC;
    if (!strcmp(s, "32"))       return LA64M68_PIS_32;
    if (!strcmp(s, "32lite"))   return LA64M68_PIS_32LITE;
    if (!strcmp(s, "pistormx")) return LA64M68_PIS_PISTORMX;
    return -1;
}

int la64m68_pis_variant_is_fpga(int v)
{
    return v == LA64M68_PIS_32 || v == LA64M68_PIS_32LITE ||
           v == LA64M68_PIS_PISTORMX;
}

int la64m68_pis_variant_of(const la64m68_pis *p)
{
    return p ? p->variant : LA64M68_PIS_AUTO;
}

la64m68_pis *la64m68_pis_open(int arm, int variant)
{
    la64m68_pis *p = calloc(1, sizeof(*p));
    if (!p)
        return NULL;
    p->fd = -1;
    p->poll_txn = 1;
    p->armed = arm ? 1 : 0;

    /* Resolve the variant up front. There is no board-id register to read
     * (see pis.h), so AUTO selects the only programming pinout we have and
     * says so -- an operator with a different board passes --pis-variant. */
    if (variant == LA64M68_PIS_AUTO) {
        p->variant = LA64M68_PIS_32LITE;
        la64m68_trace("pis: variant auto -> %s (no board id in the protocol, "
                      "override with --pis-variant)",
                      la64m68_pis_variant_name(p->variant));
    } else {
        p->variant = variant;
    }

    int fd = open("/dev/gpiomem", O_RDWR | O_SYNC | O_CLOEXEC);
    p->periph_base = dt_periph_base();
    if (fd < 0 || !p->periph_base) {
        la64m68_trace("pis: absent (gpiomem=%d periph=%08x)",
                      fd, p->periph_base);
        if (fd >= 0) close(fd);
        return p;                      /* object lives, but absent */
    }

    void *g = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED,
                   fd, 0);
    if (g == MAP_FAILED) {
        close(fd);
        return p;
    }
    p->fd = fd;
    p->gpio = g;
    p->gpset = p->gpio + 7;
    p->gpclr = p->gpio + 10;
    p->gplev = p->gpio + 13;

    /* Establish the safe idle FIRST -- this also repairs a driven state left
     * over by a previous run that died mid-transfer. */
    bus_release(p);

    if (!p->armed) {
        /* Fail closed. Stay mapped so the safe idle is enforced, but expose
         * nothing: present stays 0, no window is created and no transfer can
         * be reached, so an automated run physically cannot drive the bus. */
        la64m68_trace("pis: host present but DISARMED (pis.arm != 1) -> "
                      "no bus access, no FPGA programming");
        return p;
    }

    /* FPGA status sanity: read STATUS reg; all-ones/timeout = no PiS
     * FPGA answering -> stay absent (GPIO header alone is not proof). */
    uint32_t st = reg_read(p, REG_STATUS);
    if (st == 0xffff) {
        /* An unprogrammed FPGA answers 0xFFFF. Tearing the mapping down here
         * made provisioning impossible -- the loader needs those pins to put
         * the bitstream in. Keep it when the operator armed the bus, so the
         * chip can be brought up; `present` still stays 0. */
        if (!p->armed) {
            la64m68_trace("pis: gpiomem ok but FPGA status=ffff -> absent");
            bus_release(p);
            munmap(g, 0x1000);
            close(fd);
            p->fd = -1;
            p->gpio = NULL;
            return p;
        }
        la64m68_trace("pis: FPGA status=ffff (unprogrammed) -- armed, keeping "
                      "the pins so it can be provisioned");
        return p;
    }
    p->present = 1;

    p->chipset = mk_win(p, 0x00dff000);
    p->cia     = mk_win(p, 0x00bfe001);   /* covers CIA-A window start */
    p->zorro2  = mk_win(p, 0x00e80000);

    la64m68_trace("pis: present periph=%08x status=%04x",
                  p->periph_base, st);
    return p;
}

int la64m68_pis_present(const la64m68_pis *p)
{
    return p ? p->present : 0;
}

int la64m68_pis_cpu_release(la64m68_pis *p)
{
    if (!p || !p->gpio || !p->armed) return -1;
    /* CONTROL shares the register slot with STATUS, and bit 15 is NOT data:
     * it selects whether the remaining bits are SET (1) or CLEARED (0).
     * Writing the raw value does nothing at all -- which is why HALT stayed
     * asserted no matter what we sent.
     *
     * Sequence and timing follow ps_protocol.c ps_pulse_reset(): claim bus
     * mastery first, then hold RESET long enough to be seen. */
    reg_write(p, REG_CONTROL, 0x8000u | CONTROL_REQ_BM);   /* request BM */
    usleep(100000);
    reg_write(p, REG_CONTROL, 0x8000u | CONTROL_DRIVE_RESET);
    usleep(150000);
    reg_write(p, REG_CONTROL, CONTROL_DRIVE_RESET);        /* clear it */
    usleep(1000);
    la64m68_trace("pis: 68k released from reset (status now %04x)",
                  reg_read(p, REG_STATUS));
    return 0;
}

int la64m68_pis_status(la64m68_pis *p)
{
    if (!p || !p->gpio)
        return -1;
    /* data pins must be inputs for a reg read; addr/RD stay driven
     * (bus_dir(0) layout) */
    bus_dir(p, 0);
    return (int)reg_read(p, REG_STATUS);
}

int la64m68_pis_ipl_level(la64m68_pis *p)
{
    if (!p || !p->present)
        return 0;
    /* IPL2..0 are ACTIVE-LOW. All three high means "no interrupt", not
     * level 7 -- reading the lines as a plain number turned every floating
     * or idle state into a permanent NMI (vector 31) and the core ran away.
     * The level is the inverted line state. */
    return (int)((~*p->gplev) & 7u);
}

static void free_win(la64m68_memory *m)
{
    if (!m) return;
    free(m->ctx);
    free(m);
}

int la64m68_pis_armed(const la64m68_pis *p)
{
    return p ? p->armed : 0;
}

void la64m68_pis_close(la64m68_pis *p)
{
    if (!p) return;
    free_win(p->chipset);
    free_win(p->cia);
    free_win(p->zorro2);
    if (p->gpio)
        bus_release(p);        /* pins to safe idle BEFORE unmapping */
    if (p->gpio)
        munmap((void *)p->gpio, 0x1000);
    if (p->fd >= 0)
        close(p->fd);
    free(p);
}

la64m68_memory *la64m68_pis_chipset_mem(la64m68_pis *p)
{ return p && p->present && p->armed ? p->chipset : NULL; }
la64m68_memory *la64m68_pis_cia_mem(la64m68_pis *p)
{ return p && p->present && p->armed ? p->cia : NULL; }
la64m68_memory *la64m68_pis_zorro2_mem(la64m68_pis *p)
{ return p && p->present && p->armed ? p->zorro2 : NULL; }
