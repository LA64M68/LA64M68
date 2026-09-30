#ifndef LA64M68_PIS_H
#define LA64M68_PIS_H

/* PiS (PiStorm-family) host attachment: detects a PiS-equipped RPi and
 * exposes the real Amiga bus windows (chipset regs, CIA, Zorro II/III)
 * as router sub-memories. Highest priority in the device chain:
 * real hardware always wins over vRTG/vNIC/vHID emulated windows.
 *
 * Variants: classic, 32, 32-lite, PiStormX -- all share the GPIO-banged
 * 68k bus protocol; detection is per-peripheral, protocol is common.
 */

#include <stdint.h>
#include "memory.h"

typedef struct la64m68_pis la64m68_pis;

/* ---- hardware safety contract (A1200 / A124B) ---------------------------
 * This driver is the only code in the tree that can physically drive the
 * Amiga bus. Three invariants hold; they are structural, not conventions.
 *
 *  1. FAIL CLOSED. Nothing is driven unless `arm` is passed explicitly to
 *     la64m68_pis_open() (cfg `pis.arm = 1`). Disarmed, the object reports
 *     nothing present, exposes no bus windows and performs no transfer -- an
 *     automated run therefore cannot touch the iron at all. A disarmed open
 *     additionally forces the pins to the safe idle, so it also repairs the
 *     leftover state of a previously crashed run.
 *
 *  2. PROTOCOL-SAFE IDLE, ALWAYS. Every transfer ends by returning to
 *     bus_release(): RD/WR driven INACTIVE, data bus to inputs. This is the
 *     state the driver is in whenever it is not mid-transfer, so an abrupt
 *     exit (including SIGKILL, where no cleanup runs) leaves the bus alone.
 *     Note this is deliberately NOT "all pins high-Z": RD/WR double as
 *     CRESET1/2 of the FPGA, and a floating strobe can be read as a register
 *     write. Releasing everything would be the dangerous choice here.
 *
 *  3. FPGA PROGRAMMING IS THE MOST DANGEROUS OPERATION (it reconfigures the
 *     device that owns the 68k bus). It needs its own explicit opt-in
 *     (cfg `pis.fpga_bitstream`), requires arming, and is refused once any
 *     bus transfer has already happened.
 *
 * Pins 0..2 are IPL0..2 and pin 3 is TXN: they are driven by the FPGA and
 * must never become outputs here. See 0-POOL/notes/2026-09-30-*.md.
 */

/* PiS board variants.
 *
 * All supported boards share ONE GPIO-banged register protocol -- see
 * src/pistorm32l/gpio/ps_protocol.h -- so there is deliberately no per-variant
 * code path and nothing to fork. The variant is a descriptor for what genuinely
 * differs, which is only the FPGA configuration path:
 *
 *   classic                 : CPLD, nothing to reconfigure
 *   32 / 32lite / pistormx  : Efinix, passive-serial bitbang
 *
 * There is no board-identification register in the protocol, so software
 * cannot tell these apart by probing. LA64M68_PIS_AUTO therefore resolves to
 * the one programming pinout we have (the Efinix layout) and reports what it
 * chose; pass --pis-variant on the command line to say what is on the bench. */
enum {
    LA64M68_PIS_AUTO = 0,
    LA64M68_PIS_CLASSIC,
    LA64M68_PIS_32,
    LA64M68_PIS_32LITE,
    LA64M68_PIS_PISTORMX
};

const char *la64m68_pis_variant_name(int v);   /* "auto", "classic", ... */
int         la64m68_pis_variant_parse(const char *s);   /* -1 = unknown */
/* the variant actually in use after la64m68_pis_open() resolved AUTO */
int         la64m68_pis_variant_of(const la64m68_pis *p);
/* 1 when this variant has a reconfigurable FPGA */
int         la64m68_pis_variant_is_fpga(int v);

/* Probe + open. `arm` is the hardware arming gate: pass 0 to stay strictly
 * read-only w.r.t. the pins (default, safe for automated work). `variant` is
 * one of the LA64M68_PIS_* constants, LA64M68_PIS_AUTO to resolve.
 * Returns NULL when no PiS-capable GPIO host is present (not an RPi,
 * /dev/gpiomem missing, ...) -- callers must tolerate NULL and fall back to
 * the emulated device chain. */
la64m68_pis  *la64m68_pis_open(int arm, int variant);
void          la64m68_pis_close(la64m68_pis *p);

int           la64m68_pis_present(const la64m68_pis *p);

/* 1 when the driver was armed and may drive the bus, 0 otherwise. */
int           la64m68_pis_armed(const la64m68_pis *p);

/* Load an Efinix bitstream into the PiS32-lite FPGA (GPIO bitbang).
 * Explicitly opt-in via cfg `pis.fpga_bitstream` -- touches real pins and
 * reconfigures the device owning the 68k bus. Refused unless armed and no
 * bus transfer has happened yet. Returns 0 on completion, -1 when refused
 * or on error. Pins are returned to the protocol-safe idle on every path. */
int           la64m68_pis_fpga_load(la64m68_pis *p, const char *path);

/* Raw FPGA STATUS register read (REG_STATUS); -1 when not mapped. */
int           la64m68_pis_status(la64m68_pis *p);

/* Release the 68k from reset: assert RESET+HALT briefly, then let go.
 *
 * Without this the processor never completes a bus cycle -- every access runs
 * into the TXN timeout, which is exactly what STATUS_HALT reported. Only
 * meaningful once the FPGA is programmed. */
int la64m68_pis_cpu_release(la64m68_pis *p);

/* 68k function code used for every bus cycle: 1=user data, 2=user program,
 * 5=supervisor data, 6=supervisor program. Defaults to 5 (supervisor data),
 * which is what hardware-register access uses. */
void la64m68_pis_set_fc(la64m68_pis *p, int fc);

/* Read IPL0-2 lines (active low) -> 68k interrupt level 0-7 (0=none). */
int           la64m68_pis_ipl_level(la64m68_pis *p);

/* Amiga bus windows as router sub-memories (NULL when not present).
 * Owned by `p`; do not free the returned objects. */
la64m68_memory *la64m68_pis_chipset_mem(la64m68_pis *p);   /* 0xDFF000 custom regs */
la64m68_memory *la64m68_pis_cia_mem(la64m68_pis *p);       /* 0xBFE001/0xBFD000 CIA-A/B */
la64m68_memory *la64m68_pis_zorro2_mem(la64m68_pis *p);    /* 0xE80000.. autoconfig/Z2 */

#endif
