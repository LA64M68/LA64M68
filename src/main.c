#define _POSIX_C_SOURCE 200809L

#include "cpu.h"
#include "opts.h"
#include "memory.h"
#include "vrtg.h"
#include "vnic.h"
#include "vhid.h"
#include "vaga.h"
#include "vblk.h"
#include "vcia.h"
#include "vsplash.h"
#include "vfs.h"
#include "debug.h"
#include "host.h"
#include "fb.h"
#include "input.h"
#include "pis.h"
#include "plugin_dl.h"
#include "net.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* glue: vRTG present -> host fb backend (recreate on mode change).
 * `vhid` is consulted so the presented frame follows the Ctrl+Alt+Pause
 * input/output focus switch: with the host focused the guest frame is left
 * alone and the machine shows its own output again. */
typedef struct {
    la64m68_fb *fb;
    const char *backend;
    int w, h;
    la64m68_vhid *vhid;
} fb_glue;

static void fb_glue_present(void *ctx, const void *pixels,
                            uint32_t w, uint32_t h, int bpp)
{
    fb_glue *g = ctx;
    if (g->vhid &&
        la64m68_vhid_focus(g->vhid) != LA64M68_VHID_FOCUS_GUEST)
        return;                     /* host has focus: its own output wins */
    if (!g->fb || g->w != (int)w || g->h != (int)h) {
        la64m68_fb_destroy(g->fb);
        g->fb = la64m68_fb_create(g->backend, (int)w, (int)h);
        g->w = (int)w; g->h = (int)h;
        if (!g->fb) return;
    }
    la64m68_fb_present(g->fb, pixels, bpp);
}

/* vNIC transmit callback. A plain wrapper, not a cast: casting between
 * incompatible function pointer types is undefined behaviour in C. */
static int vnic_tx(void *ctx, const void *frame, size_t len)
{
    return la64m68_net_send((la64m68_net *)ctx, frame, len);
}

/* Clean shutdown on SIGINT/SIGTERM: the run loop notices the flag and the
 * normal teardown releases the GPIO header. Leaving the PiS strobes driven
 * after an aborted run is the one thing the iron does not forgive. */
static volatile sig_atomic_t g_stop;

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

/* Every window must land: a silently dropped region is a broken machine. */
static int route(la64m68_memory *rt, uint32_t base, uint32_t size,
                 la64m68_memory *sub, const char *what)
{
    if (la64m68_router_add(rt, base, size, sub) != 0) {
        fprintf(stderr, "la64m68: cannot map %s at %08x\n", what, base);
        return -1;
    }
    return 0;
}

static int load_raw(la64m68_memory *mem, const char *path, uint32_t addr,
                    int vectors)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "la64m68: cannot open ROM %s\n", path);
        return -1;
    }
    uint8_t buf[4096];
    size_t n;
    size_t total = 0;
    uint32_t base = addr;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        for (size_t i = 0; i < n; i++)
            la64m68_mem_write8(mem, addr + (uint32_t)i, buf[i]);
        addr += (uint32_t)n;
        total += n;
    }
    fclose(f);
    fprintf(stderr, "la64m68: loaded %zu bytes at %08x\n", total, base);
    /* The ROM head is the whole vector table, not just SSP/PC. The Amiga
     * overlays the ROM over 0x000000 at reset for exactly this reason; without
     * it every interrupt vector reads 0 and the CPU jumps to address 0, then
     * executes open bus and runs away. Copying the first 256 vectors gives the
     * guest its interrupt table. */
    if (vectors) {
        for (uint32_t i = 0; i < 256 * 4; i++)
            la64m68_mem_write8(mem, i, la64m68_mem_read8(mem, base + i));
    }
    return 0;
}

/* Load a standard ROM image and place it where the machine expects it.
 * Kickstart and TOS are plain binary ROMs in their native format -- no
 * container, no header -- so this is a raw load at the documented base:
 *
 *   Amiga Kickstart   256 KiB -> 0xF80000, 512 KiB -> 0xF00000, 1 MiB -> 0xE00000
 *   Atari TOS         always 0xE00000 (mirrored by the hardware)
 *
 * The first two longs are the reset SSP/PC, so the vectors are installed from
 * the ROM head. */
static int load_standard_rom(la64m68_memory *mem, const char *path, int kind)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "la64m68: cannot open ROM %s\n", path);
        return -1;
    }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long sz = ftell(f);
    fclose(f);
    if (sz <= 0) return -1;

    /* Placement is derived from the ROM itself, not from a size table: the
     * reset PC in the head says where the image is decoded. A 512 KiB
     * Kickstart carries PC=0x00f80100 and must sit at 0xF80000 -- the naive
     * "bigger ROM goes lower" rule would have put it at 0xF00000 and left
     * the reset vector pointing outside the image. */
    uint32_t base;
    if (kind == 1) {                       /* Atari TOS: always 0xE00000 */
        base = 0x00E00000u;
    } else {
        FILE *g = fopen(path, "rb");
        uint8_t head[8] = {0};
        if (g) { if (fread(head, 1, 8, g) != 8) { /* short ROM */ } fclose(g); }
        uint32_t reset_pc = ((uint32_t)head[4] << 24) | ((uint32_t)head[5] << 16) |
                            ((uint32_t)head[6] << 8) | head[7];
        uint32_t sz32 = (uint32_t)sz;
        uint32_t mask = sz32 - 1;               /* ROM size is a power of two */
        base = (sz32 & mask) == 0 && sz32 ? (reset_pc & ~mask) : 0x00F80000u;
    }
    fprintf(stderr, "la64m68: %s %s (%ld bytes) at %08x\n",
            kind == 1 ? "TOS" : "Kickstart", path, sz, base);
    return load_raw(mem, path, base, 1);
}

/* --disk KIND:PATH  ->  a virtual block device backed by an image */
static int attach_disk(la64m68_vblk *blk, const char *spec, int writable,
                       la64m68_memory **regs, la64m68_memory *rt)
{
    const char *colon = strchr(spec, ':');
    if (!colon || colon == spec) {
        fprintf(stderr, "la64m68: --disk wants KIND:PATH "
                        "(raw:file.img or qcow2:file.qcow2)\n");
        return -1;
    }
    char kind[16];
    size_t kl = (size_t)(colon - spec);
    if (kl >= sizeof(kind)) return -1;
    memcpy(kind, spec, kl);
    kind[kl] = 0;
    const char *path = colon + 1;

    la64m68_vblk_src src;
    if (!strcmp(kind, "raw")) {
        if (la64m68_vblk_open_raw(path, writable, &src) != 0) return -1;
    } else if (!strcmp(kind, "qcow2")) {
        if (la64m68_vblk_open_qcow2(path, writable, &src) != 0) return -1;
    } else {
        fprintf(stderr, "la64m68: --disk kind '%s' is raw or qcow2\n", kind);
        return -1;
    }
    la64m68_vblk_attach(blk, &src);
    *regs = la64m68_vblk_regs_memory(blk);
    if (!*regs) return -1;
    if (route(rt, LA64M68_VBLK_REG_BASE, 0x100, *regs, "vblk regs") != 0)
        return -1;
    fprintf(stderr, "la64m68: disk %s (%s, %llu sectors)%s\n", path, kind,
            (unsigned long long)la64m68_vblk_sectors(blk),
            writable ? "" : " read-only");
    return 0;
}

int main(int argc, char **argv)
{
    int rc = 0;

    /* Every resource handle is declared up front so all error paths can share
     * one cleanup block. Returning early used to skip it, which left the PiS
     * GPIO mapped and its pins driven on the Amiga bus. */
    la64m68_opts   o;
    la64m68_host   *host      = NULL;
    la64m68_memory *rt        = NULL;
    la64m68_memory *ram       = NULL;
    la64m68_memory *vaga_mem  = NULL;
    la64m68_memory *vregs[3]  = { NULL, NULL, NULL };
    la64m68_pis    *pis       = NULL;
    la64m68_net    *net       = NULL;
    la64m68_input  *input     = NULL;
    la64m68_vrtg    vrtg;
    la64m68_vnic    vnic;
    la64m68_vhid    vhid;
    la64m68_vaga    vaga;
    la64m68_vcia    cia_a, cia_b;
    la64m68_memory *cia_mem[2] = { NULL, NULL };
    la64m68_vblk    vblk;
    int fallback_told = 0;
    la64m68_memory *vblk_regs = NULL;
    la64m68_memory *rom_mem  = NULL;
    la64m68_memory *fast_ram = NULL;
    la64m68_memory *rom_tos  = NULL;
    la64m68_vfs    vfs;
    la64m68_memory *vfs_regs = NULL;
    fb_glue         glue      = { NULL, NULL, 0, 0, NULL };

    /* First thing: the program root. LA64M68 uses only its own directory, so
     * it goes there before any path is touched -- whatever the caller's cwd
     * was. Relative paths are root-relative from here on. */
    const char *root = la64m68_host_selfroot(argv[0]);
    if (!root) {
        fprintf(stderr, "la64m68: cannot determine the program root\n");
        return 1;
    }

    la64m68_opts_defaults(&o);
    int pr = la64m68_opts_parse(&o, argc, argv);
    if (pr > 0) return 0;               /* --help */
    if (pr < 0) return 2;               /* usage error */

    int pis_variant = la64m68_pis_variant_parse(o.pis_variant);
    if (pis_variant < 0) {
        fprintf(stderr, "la64m68: --pis-variant '%s' is not one of "
                        "auto|classic|32|32lite|pistormx\n", o.pis_variant);
        return 2;
    }

    /* Which surface gets the picture. `amiga-gfx`/`atari-gfx` mean the
     * fallback hardware draws it, so we must NOT also push a frame to the
     * host surface -- two owners of one screen is how you get a garbled
     * picture. */
    int disp_host = strcmp(o.display, "videocore") == 0;
    if (!disp_host &&
        strcmp(o.display, "amiga-gfx") != 0 &&
        strcmp(o.display, "atari-gfx") != 0) {
        fprintf(stderr, "la64m68: --display '%s' is videocore|amiga-gfx|atari-gfx\n",
                o.display);
        return 2;
    }

    host = la64m68_host_create();
    if (!host) {
        fprintf(stderr, "la64m68: failed to create host\n");
        rc = 1;
        goto out;
    }

    /* Address space: RAM + device register windows via the router. */
    /* ---- guest memory -------------------------------------------------
     * Chip RAM at 0 is what the chipset DMAs from: Bitplanes, blitter and
     * disk all live here, so a playfield must be placed in it. Fast RAM sits
     * higher and is free of that constraint.
     *
     * The device windows below lie INSIDE the chip range and shadow it --
     * the router resolves overlaps newest-first, so a device always wins.
     * That is why a 16 MiB chip area is "16 MiB minus the windows" rather
     * than a flat block. */
    rt  = la64m68_router_create();
    if (!rt) {
        fprintf(stderr, "la64m68: failed to create the router\n");
        rc = 1;
        goto out;
    }
    size_t chip_bytes = (size_t)o.chip_kb * 1024;
    size_t fast_bytes = (size_t)o.fast_kb * 1024;
    ram = la64m68_ram_memory_create(0x00000000, chip_bytes);
    if (!ram) {
        fprintf(stderr, "la64m68: cannot create Chip RAM\n");
        rc = 1;
        goto out;
    }
    if (route(rt, 0x00000000, (uint32_t)chip_bytes, ram, "chip ram") != 0) {
        rc = 1;
        goto out;
    }
    if (fast_bytes) {
        fast_ram = la64m68_ram_memory_create(0x01000000u, fast_bytes);
        if (!fast_ram ||
            route(rt, 0x01000000u, (uint32_t)fast_bytes, fast_ram,
                  "fast ram") != 0) {
            fprintf(stderr, "la64m68: cannot create Fast RAM\n");
            rc = 1;
            goto out;
        }
    }
    fprintf(stderr, "la64m68: chip=%lu KiB @00000000, fast=%lu KiB @01000000\n",
            (unsigned long)o.chip_kb, (unsigned long)o.fast_kb);

    /* PiS passthrough first: real Amiga HW wins over every v-device.
     *
     * --pis-arm is the hardware safety gate and defaults to OFF. An automated
     * run must never be able to drive the Amiga bus without an explicit,
     * deliberate opt-in; disarmed, no pin is driven and the emulated device
     * chain takes over instead. */
    if (o.pis) {
        if (!o.pis_arm)
            fprintf(stderr, "la64m68: PiS DISARMED (no --pis-arm) -- no bus "
                            "transfer is performed and no FPGA programming is "
                            "allowed; pass --pis-arm to attach to the bus\n");
        pis = la64m68_pis_open(o.pis_arm, pis_variant);
        /* Provisioning must NOT require a working FPGA: `present` is only
         * set once the chip answers, and an unprogrammed one answers 0xFFFF
         * by definition. Gating the load on it made the bootstrapping
         * impossible. The arming gate is the safety here. */
        if (o.fpga_bitstream && pis && la64m68_pis_armed(pis)) {
            if (la64m68_pis_fpga_load(pis, o.fpga_bitstream) == 0)
                printf("la64m68: PiS FPGA loaded, status=%04x\n",
                       la64m68_pis_status(pis));
            else
                fprintf(stderr, "la64m68: PiS FPGA load failed: %s\n",
                        o.fpga_bitstream);
        }
        if (la64m68_pis_present(pis)) {
            printf("la64m68: PiS variant %s\n",
                   la64m68_pis_variant_name(la64m68_pis_variant_of(pis)));
            if (la64m68_pis_chipset_mem(pis) &&
                route(rt, 0x00dff000, 0x1000,
                      la64m68_pis_chipset_mem(pis), "pis chipset") != 0) {
                rc = 1;
                goto out;
            }
            if (la64m68_pis_cia_mem(pis) &&
                route(rt, 0x00bfe001, 0x2000,
                      la64m68_pis_cia_mem(pis), "pis cia") != 0) {
                rc = 1;
                goto out;
            }
            if (la64m68_pis_zorro2_mem(pis) &&
                route(rt, 0x00e80000, 0x80000,
                      la64m68_pis_zorro2_mem(pis), "pis zorro2") != 0) {
                rc = 1;
                goto out;
            }
            /* Without releasing the CPU every bus cycle times out: the
             * processor sits in HALT and never completes the transfer. */
            if (la64m68_pis_cpu_release(pis) != 0)
                fprintf(stderr, "la64m68: could not release the 68k\n");
            printf("la64m68: PiS passthrough active (ARMED)\n");
        }
    }

    /* CIA-A (0xBFE001) and CIA-B (0xBFD000): the guest's timer tick. Without
     * them Kickstart stalls before it ever enables the display. Routed here,
     * below the PiS windows, so real hardware still wins when it is present. */
    la64m68_vcia_init(&cia_a, 1);
    la64m68_vcia_init(&cia_b, 0);
    cia_mem[0] = la64m68_vcia_memory(&cia_a);
    cia_mem[1] = la64m68_vcia_memory(&cia_b);
    if (!cia_mem[0] || !cia_mem[1] ||
        route(rt, LA64M68_VCIA_A_BASE & ~0xfffu, LA64M68_VCIA_SPAN,
              cia_mem[0], "cia-a") != 0 ||
        route(rt, LA64M68_VCIA_B_BASE, LA64M68_VCIA_SPAN,
              cia_mem[1], "cia-b") != 0) {
        rc = 1;
        goto out;
    }

    /* Denise/AGA-style chipset fallback -- lowest priority, only when no
     * PiS window claimed the custom-chip space. Disarmed PiS leaves this
     * path open, so an automated run still gets a working (emulated) chipset. */
    int chipset_claimed = pis && la64m68_pis_present(pis) &&
                          la64m68_pis_chipset_mem(pis);
    if (!chipset_claimed && o.chipset) {
        la64m68_vaga_init(&vaga);
        la64m68_vaga_set_mem(&vaga, rt);   /* bitplane fetches read guest RAM */
        vaga_mem = la64m68_vaga_memory(&vaga);
        if (vaga_mem &&
            route(rt, LA64M68_VAGA_BASE, LA64M68_VAGA_SIZE, vaga_mem,
                  "vaga chipset") != 0) {
            rc = 1;
            goto out;
        }
    }

    glue.backend = o.vrtg_backend;
    if (o.vrtg) {
        la64m68_vrtg_init(&vrtg, rt, (uint32_t)o.vrtg_fb_addr);
        vrtg.present = fb_glue_present;
        vrtg.present_ctx = &glue;
        vregs[0] = la64m68_vrtg_regs_memory(&vrtg);
        if (route(rt, LA64M68_VRTG_REG_BASE, 0x100, vregs[0],
                  "vrtg regs") != 0) {
            rc = 1;
            goto out;
        }
    }
    if (o.vnic) {
        la64m68_vnic_init(&vnic, rt);
        net = la64m68_net_create(o.vnic_mode, o.vnic_ifname);
        vnic.enabled = 1;
        vnic.tx_ctx = net;
        vnic.tx = vnic_tx;
        vregs[1] = la64m68_vnic_regs_memory(&vnic);
        if (route(rt, LA64M68_VNIC_REG_BASE, 0x100, vregs[1],
                  "vnic regs") != 0) {
            rc = 1;
            goto out;
        }
    }
    if (o.vhid) {
        la64m68_vhid_init(&vhid);
        vhid.enabled = 1;
        /* vhid is initialised here, so the glue may only point at it now.
         * Left NULL otherwise: fb_glue_present() then presents unconditionally,
         * which is correct -- without vHID there is no focus switch to honour. */
        glue.vhid = &vhid;
        input = la64m68_input_create(&vhid, o.vhid_devices);
        /* Initial focus decides whose output is presented. Default is the
         * host (fail-safe: you keep control of the machine); --focus guest
         * hands keyboard/mouse and the display to LA64M68. Placed AFTER the
         * input device exists, or the grab would target NULL. */
        if (o.focus && !strcmp(o.focus, "guest")) {
            la64m68_vhid_set_focus(&vhid, LA64M68_VHID_FOCUS_GUEST);
            la64m68_input_grab(input, 1);
        }
        vregs[2] = la64m68_vhid_regs_memory(&vhid);
        if (route(rt, LA64M68_VHID_REG_BASE, 0x100, vregs[2],
                  "vhid regs") != 0) {
            rc = 1;
            goto out;
        }
    }

    /* A ROM needs somewhere to live. The standard bases (0xE00000 for TOS,
     * 0xF00000/0xF80000 for Kickstart) all sit outside guest RAM, so without
     * a routed region there the loader writes into nothing and every vector
     * reads back zero -- which looks like a bad ROM instead of a missing
     * window. 2 MiB covers every standard base. */
    /* ROM windows, sized to stop below the device registers.
     *   0xE00000..0xE7FFFF  Atari TOS (512 KiB)
     *   0xF80000..0xFFFFFF  Amiga Kickstart (512 KiB)
     * A wider window (0xF00000..0xFFFFFF) swallowed the vBLK registers at
     * 0xF30000: 0xF00000 + 0x100000 = 0x1000000, so 0xF30000 was inside it.
     * The 512 KiB Kickstart window ends exactly at 0xFFFFFF and leaves the
     * device area alone. */
    rom_mem = la64m68_ram_memory_create(0x00F80000u, 0x80000u);
    rom_tos = la64m68_ram_memory_create(0x00E00000u, 0x80000u);
    if (!rom_mem || !rom_tos ||
        route(rt, 0x00F80000u, 0x80000u, rom_mem, "kickstart window") != 0 ||
        route(rt, 0x00E00000u, 0x80000u, rom_tos, "tos window") != 0) {
        fprintf(stderr, "la64m68: cannot create the ROM windows\n");
        rc = 1;
        goto out;
    }

    /* Reset vectors. These go in BEFORE any ROM is loaded, not after: a ROM
     * brings its own vector table and must be the last writer. Placing these
     * afterwards overwrote the Kickstart entry point and sent the CPU to
     * 0x00000800 -- inside RAM -- from where it executed open bus forever. */
    la64m68_mem_write32(rt, 0x00000000, (uint32_t)o.chip_kb * 1024);
    la64m68_mem_write32(rt, 0x00000004, 0x00000800);

    /* ROM images: --kickstart / --tos place themselves at the documented
     * standard bases, --rom takes an explicit address. */
    if (o.kickstart && load_standard_rom(rt, o.kickstart, 0) < 0) {
        rc = 1;
        goto out;
    }
    if (o.tos && load_standard_rom(rt, o.tos, 1) < 0) {
        rc = 1;
        goto out;
    }

    /* Filesystem passthrough: --fs DIR exposes a host directory by name. */
    if (o.fs) {
        la64m68_vfs_init(&vfs, rt, o.fs);
        vfs_regs = la64m68_vfs_regs_memory(&vfs);
        if (!vfs_regs || route(rt, LA64M68_VFS_REG_BASE, 0x100, vfs_regs,
                               "vfs regs") != 0) {
            rc = 1;
            goto out;
        }
        fprintf(stderr, "la64m68: host directory %s exposed to the guest\n", o.fs);
    }

    /* Storage: --disk KIND:PATH attaches a virtual block device. */
    if (o.disk) {
        la64m68_vblk_init(&vblk, rt);
        if (attach_disk(&vblk, o.disk, !o.disk_ro, &vblk_regs, rt) != 0) {
            rc = 1;
            goto out;
        }
    }

    if (o.rom &&
        load_raw(rt, o.rom, (uint32_t)o.rom_addr, o.rom_vectors) < 0) {
        rc = 1;
        goto out;
    }

    /* Splash on the primary surface (VideoCore via vRTG) before the guest
     * draws anything. Silent when the assets are absent. */
    if (o.vrtg && disp_host && la64m68_vsplash_load(root) == 0) {
        static uint8_t splash[LA64M68_VSPLASH_WIDTH * LA64M68_VSPLASH_HEIGHT * 3];
        la64m68_vsplash_render_rgb24(splash, LA64M68_VSPLASH_WIDTH * 3);
        glue.fb = la64m68_fb_create(glue.backend, LA64M68_VSPLASH_WIDTH,
                                    LA64M68_VSPLASH_HEIGHT);
        if (glue.fb) {
            glue.w = LA64M68_VSPLASH_WIDTH;
            glue.h = LA64M68_VSPLASH_HEIGHT;
            la64m68_fb_present(glue.fb, splash, 24);
            fprintf(stderr, "la64m68: splash on the primary surface "
                            "(%dx%d, 16 colours, 3-bit channel steps)\n",
                            LA64M68_VSPLASH_WIDTH, LA64M68_VSPLASH_HEIGHT);
        }
    }

    la64m68_cpu cpu;
    /* the architecture backend: Amiga, Atari, or none (pure core) */
    la64m68_plugin *plugin = NULL;
    if (o.plugin) {
        plugin = la64m68_plugin_load(o.plugin);
        if (!plugin) {
            fprintf(stderr, "la64m68: cannot load backend %s\n", o.plugin);
            rc = 1;
            goto out;
        }
    }
    la64m68_cpu_reset(&cpu, rt, plugin);

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    /* max_steps 0 = run until SIGINT/SIGTERM: that is the live default.
     * A bounded count is for automated smoke runs only. */
    for (long i = 0; (o.max_steps <= 0 || i < o.max_steps) && !g_stop; i++) {
        if (input && (i & 0xfff) == 0) la64m68_input_poll(input);
        /* advance the CIA timers and take the highest requested level */
        la64m68_vcia_tick(&cia_a, 64);
        la64m68_vcia_tick(&cia_b, 64);
        if ((i & 0xff) == 0) {
            int ipl = la64m68_pis_ipl_level(pis);
            if (vaga_mem) {
                int vi = la64m68_vaga_ipl(&vaga);
                if (vi > ipl) ipl = vi;
            }
            int ca = la64m68_vcia_ipl(&cia_a);
            if (ca > ipl) ipl = ca;
            int cb = la64m68_vcia_ipl(&cia_b);
            if (cb > ipl) ipl = cb;
            la64m68_cpu_ipl(&cpu, ipl);
        }
        if (net && (i & 0xfff) == 0) {
            uint8_t fr[1518];
            int rl;
            while ((rl = la64m68_net_recv(net, fr, sizeof(fr))) > 0)
                la64m68_vnic_rx(&vnic, fr, (size_t)rl);
        }
        /* Emulated chipset: turn the guest's playfield into a picture and
         * hand it to the presenter. fb_glue_present() re-creates the surface
         * when the guest changes resolution, so the two sizes (splash 320x200
         * vs. playfield 320x256) do not have to agree up front. */
        if (vaga_mem && (i & 0x3ff) == 0) {
            static uint8_t frame[640 * 256 * 3];
            uint32_t fw = 0, fh = 0;
            if (la64m68_vaga_render(&vaga, frame, 640 * 3, &fw, &fh) == 0) {
                if (disp_host) {
                    fb_glue_present(&glue, frame, fw, fh, 24);
                } else {
                    /* fallback display: emit the layout the target hardware
                     * wants instead of a host frame */
                    static uint8_t planar[640 * 256 * 6 / 8];
                    uint32_t pw = 0, ph = 0; int pdepth = 0;
                    int ok = strcmp(o.display, "atari-gfx") == 0
                        ? la64m68_vaga_render_st(&vaga, planar, &pw, &ph, &pdepth)
                        : la64m68_vaga_render_planar(&vaga, planar, &pw, &ph, &pdepth);
                    if (ok == 0 && !fallback_told) {
                        fallback_told = 1;
                        fprintf(stderr, "la64m68: display=%s -> %ux%u, %d bitplanes "
                                        "(%u bytes planar)\n",
                                o.display, pw, ph, pdepth,
                                (unsigned)(pw / 8 * ph * (uint32_t)pdepth));
                    }
                }
            }
        }

        if (la64m68_cpu_step(&cpu) != 0) break;
    }

    printf("la64m68: stopped at pc=%08x cycles=%llu%s\n",
           (unsigned)cpu.pc, (unsigned long long)cpu.cycles,
           g_stop ? " (signal)" : "");

out:
    /* PiS GPIO first: getting the pins back to the protocol-safe idle is the
     * one thing the iron does not forgive us forgetting. */
    la64m68_pis_close(pis);
    la64m68_vaga_mem_destroy(vaga_mem);
    la64m68_router_destroy(rt);
    la64m68_ram_memory_destroy(ram);
    if (vregs[0]) la64m68_vrtg_regs_destroy(vregs[0]);
    la64m68_fb_destroy(glue.fb);
    la64m68_input_destroy(input);
    la64m68_net_destroy(net);
    if (vregs[1]) la64m68_vnic_regs_destroy(vregs[1]);
    if (vregs[2]) la64m68_vhid_regs_destroy(vregs[2]);
    la64m68_ram_memory_destroy(rom_mem);
    la64m68_ram_memory_destroy(rom_tos);
    la64m68_ram_memory_destroy(fast_ram);
    if (vblk_regs) {
        la64m68_vblk_regs_destroy(vblk_regs);
        la64m68_vblk_src_close(&vblk.src);
    }
    if (vfs_regs) la64m68_vfs_regs_destroy(vfs_regs);
    if (cia_mem[0]) la64m68_vcia_mem_destroy(cia_mem[0]);
    if (cia_mem[1]) la64m68_vcia_mem_destroy(cia_mem[1]);
    la64m68_host_destroy(host);
    return rc;
}
