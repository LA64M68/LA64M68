#define _POSIX_C_SOURCE 200809L

#include "cpu.h"
#include "opts.h"
#include "memory.h"
#include "vrtg.h"
#include "vnic.h"
#include "vhid.h"
#include "vaga.h"
#include "vblk.h"
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
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        for (size_t i = 0; i < n; i++)
            la64m68_mem_write8(mem, addr + (uint32_t)i, buf[i]);
        addr += (uint32_t)n;
        total += n;
    }
    fclose(f);
    fprintf(stderr, "la64m68: loaded %zu bytes at %08x\n", total, addr);
    if (vectors) {
        la64m68_mem_write32(mem, 0, la64m68_mem_read32(mem, addr));
        la64m68_mem_write32(mem, 4, la64m68_mem_read32(mem, addr + 4));
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

    uint32_t base;
    if (kind == 1) {                       /* Atari TOS: always 0xE00000 */
        base = 0x00E00000u;
    } else {                               /* Amiga Kickstart: by ROM size */
        base = sz > 512 * 1024 ? 0x00E00000u :
               sz > 256 * 1024 ? 0x00F00000u : 0x00F80000u;
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
    la64m68_vblk    vblk;
    la64m68_memory *vblk_regs = NULL;
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

    host = la64m68_host_create();
    if (!host) {
        fprintf(stderr, "la64m68: failed to create host\n");
        rc = 1;
        goto out;
    }

    /* Address space: RAM + device register windows via the router. */
    rt  = la64m68_router_create();
    ram = la64m68_ram_memory_create(0x00000000, (size_t)o.ram_kb * 1024);
    if (!rt || !ram) {
        fprintf(stderr, "la64m68: failed to create memory\n");
        rc = 1;
        goto out;
    }
    if (route(rt, 0x00000000, (uint32_t)((size_t)o.ram_kb * 1024), ram,
              "ram") != 0) {
        rc = 1;
        goto out;
    }

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
        if (o.fpga_bitstream && la64m68_pis_present(pis)) {
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
            printf("la64m68: PiS passthrough active (ARMED)\n");
        }
    }

    /* Denise/AGA-style chipset fallback -- lowest priority, only when no
     * PiS window claimed the custom-chip space. Disarmed PiS leaves this
     * path open, so an automated run still gets a working (emulated) chipset. */
    int chipset_claimed = pis && la64m68_pis_present(pis) &&
                          la64m68_pis_chipset_mem(pis);
    if (!chipset_claimed && o.chipset) {
        la64m68_vaga_init(&vaga);
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
        vregs[2] = la64m68_vhid_regs_memory(&vhid);
        if (route(rt, LA64M68_VHID_REG_BASE, 0x100, vregs[2],
                  "vhid regs") != 0) {
            rc = 1;
            goto out;
        }
    }

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

    /* defaults until a ROM image provides vectors */
    la64m68_mem_write32(rt, 0x00000000, (uint32_t)o.ram_kb * 1024);
    la64m68_mem_write32(rt, 0x00000004, 0x00000800);
    if (o.rom &&
        load_raw(rt, o.rom, (uint32_t)o.rom_addr, o.rom_vectors) < 0) {
        rc = 1;
        goto out;
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
        if ((i & 0xff) == 0) {
            int ipl = la64m68_pis_ipl_level(pis);
            if (vaga_mem) {
                int vi = la64m68_vaga_ipl(&vaga);
                if (vi > ipl) ipl = vi;
            }
            la64m68_cpu_ipl(&cpu, ipl);
        }
        if (net && (i & 0xfff) == 0) {
            uint8_t fr[1518];
            int rl;
            while ((rl = la64m68_net_recv(net, fr, sizeof(fr))) > 0)
                la64m68_vnic_rx(&vnic, fr, (size_t)rl);
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
    if (vblk_regs) {
        la64m68_vblk_regs_destroy(vblk_regs);
        la64m68_vblk_src_close(&vblk.src);
    }
    if (vfs_regs) la64m68_vfs_regs_destroy(vfs_regs);
    la64m68_host_destroy(host);
    return rc;
}
