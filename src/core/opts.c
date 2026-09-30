#define _POSIX_C_SOURCE 200809L

#include "opts.h"
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void la64m68_opts_defaults(la64m68_opts *o)
{
    *o = (la64m68_opts){
        .rom            = NULL,
        .rom_addr       = 0x800,
        .rom_vectors    = 1,
        .kickstart      = NULL,
        .tos            = NULL,
        .disk           = NULL,
        .disk_ro        = 0,
        .plugin         = NULL,
        .fs             = NULL,
        .ram_kb         = 1024,
        .max_steps      = 0,
        .pis            = 1,
        .pis_arm        = 0,
        .pis_variant    = "auto",
        .fpga_bitstream = NULL,
        .chipset        = 1,
        .vrtg           = 1,
        .vrtg_backend   = "auto",
        .vrtg_fb_addr   = 0x80000,
        .vnic           = 1,
        .vnic_mode      = "off",
        .vnic_ifname    = NULL,
        .vhid           = 1,
        .vhid_devices   = NULL,
    };
}

/* Strict numeric parse: no trailing garbage, no silent fallback. */
static int parse_long(const char *s, long min, long max, long *out)
{
    char *end;
    errno = 0;
    long v = strtol(s, &end, 0);            /* base 0: decimal and 0x... */
    if (end == s || *end != 0 || errno == ERANGE)
        return -1;
    if (v < min || v > max)
        return -1;
    *out = v;
    return 0;
}

/* --name / --no-name */
static const struct { const char *name; size_t off; } g_bools[] = {
    { "rom-vectors", offsetof(la64m68_opts, rom_vectors) },
    { "disk-ro",     offsetof(la64m68_opts, disk_ro) },
    { "pis",         offsetof(la64m68_opts, pis) },
    { "pis-arm",     offsetof(la64m68_opts, pis_arm) },
    { "chipset",     offsetof(la64m68_opts, chipset) },
    { "vrtg",        offsetof(la64m68_opts, vrtg) },
    { "vnic",        offsetof(la64m68_opts, vnic) },
    { "vhid",        offsetof(la64m68_opts, vhid) },
};

enum { V_STR, V_LONG };

/* options that take a value: --name VALUE and --name=VALUE */
static const struct {
    const char *name;
    size_t off;
    int kind;
    long min, max;
} g_vals[] = {
    { "rom",            offsetof(la64m68_opts, rom),            V_STR,  0, 0 },
    { "kickstart",      offsetof(la64m68_opts, kickstart),      V_STR,  0, 0 },
    { "tos",            offsetof(la64m68_opts, tos),            V_STR,  0, 0 },
    { "disk",           offsetof(la64m68_opts, disk),           V_STR,  0, 0 },
    { "plugin",         offsetof(la64m68_opts, plugin),         V_STR,  0, 0 },
    { "fs",             offsetof(la64m68_opts, fs),             V_STR,  0, 0 },
    { "fpga-bitstream", offsetof(la64m68_opts, fpga_bitstream), V_STR,  0, 0 },
    { "pis-variant",    offsetof(la64m68_opts, pis_variant),    V_STR,  0, 0 },
    { "vrtg-backend",   offsetof(la64m68_opts, vrtg_backend),   V_STR,  0, 0 },
    { "vnic-mode",      offsetof(la64m68_opts, vnic_mode),      V_STR,  0, 0 },
    { "vnic-ifname",    offsetof(la64m68_opts, vnic_ifname),    V_STR,  0, 0 },
    { "vhid-devices",   offsetof(la64m68_opts, vhid_devices),   V_STR,  0, 0 },
    { "rom-addr",       offsetof(la64m68_opts, rom_addr),       V_LONG, 0, 0xffffffffL },
    { "ram-kb",         offsetof(la64m68_opts, ram_kb),         V_LONG, 1, 0x100000L },
    { "max-steps",      offsetof(la64m68_opts, max_steps),      V_LONG, 0, 0x7fffffffL },
    { "vrtg-fb-addr",   offsetof(la64m68_opts, vrtg_fb_addr),   V_LONG, 0, 0xffffffffL },
};

void la64m68_opts_usage(const char *prog)
{
    fprintf(stderr,
"usage: %s [options]\n"
"\n"
"LA64M68 is self-contained: everything lives in the program root -- the\n"
"directory holding this executable -- and the process starts there. There is\n"
"no configuration file; pass options on the command line. Ready-made command\n"
"lines are in ./scripts/ inside that root.\n"
"\n"
"core\n"
"  --rom FILE             raw ROM image to load\n"
"  --rom-addr N           load address of the ROM           [0x800]\n"
"  --[no-]rom-vectors     reset vectors from the ROM head   [on]\n"
"  --ram-kb N             guest RAM in KiB                  [1024]\n"
"  --max-steps N          instructions to run, 0 = until stopped  [0]\n"
"\n"
"PiS (real Amiga bus -- hardware)\n"
"  --[no-]pis             PiS passthrough                   [on]\n"
"  --pis-arm              drive the Amiga bus               [OFF]\n"
"  --pis-variant NAME     auto|classic|32|32lite|pistormx   [auto]\n"
"  --fpga-bitstream FILE  load an Efinix bitstream into the PiS FPGA\n"
"\n"
"devices\n"
"  --[no-]chipset         vAGA chipset fallback             [on]\n"
"  --[no-]vrtg            virtual RTG                       [on]\n"
"  --vrtg-backend NAME    auto|vc|x11                       [auto]\n"
"  --vrtg-fb-addr N       guest framebuffer address         [0x80000]\n"
"  --[no-]vnic            virtual NIC                       [on]\n"
"  --vnic-mode NAME       off|tap|masq                      [off]\n"
"  --vnic-ifname NAME     host interface for tap mode\n"
"  --[no-]vhid            virtual HID                       [on]\n"
"  --vhid-devices LIST    comma-separated /dev/input/eventN [auto]\n"
"\n"
"backends\n"
"  --plugin FILE          architecture backend module (Amiga/Atari).\n"
"                         Example: --plugin plugins/libla64m68_amiga.so\n"
"  --disk KIND:PATH       storage: raw:file.img or qcow2:file.qcow2\n"
"  --fs DIR               expose a host directory to the guest by name\n"
"  --disk-ro              attach the disk read-only\n"
"  --kickstart FILE       Amiga Kickstart ROM (native format, auto-placed)\n"
"  --tos FILE             Atari TOS ROM (native format)\n"
"\n"
"  -h, --help             this text\n"
"\n"
"hardware safety: --pis-arm is required to drive the Amiga bus and defaults\n"
"  to OFF, so an automated run never touches the iron.\n"
"input/output switch: Ctrl+Alt+Pause hands keyboard/mouse and the shown\n"
"  frame between the host and LA64M68.\n"
"environment: LA64M68_MAX_DEBUG=1 enables the debug/trace output\n",
        prog ? prog : "la64m68");
}

int la64m68_opts_parse(la64m68_opts *o, int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '-' || a[1] != '-' || a[2] == 0) {
            if (a[0] == '-' && a[1] == 'h' && a[2] == 0) {
                la64m68_opts_usage(argv[0]);
                return 1;
            }
            fprintf(stderr, "la64m68: unexpected argument '%s' (see --help)\n", a);
            return -1;
        }
        a += 2;

        /* split --name=value */
        char name[64];
        const char *val = NULL;
        const char *eq = strchr(a, '=');
        size_t nl = eq ? (size_t)(eq - a) : strlen(a);
        if (nl == 0 || nl >= sizeof(name)) {
            fprintf(stderr, "la64m68: bad option '--%s'\n", a);
            return -1;
        }
        memcpy(name, a, nl);
        name[nl] = 0;
        if (eq) val = eq + 1;

        if (!strcmp(name, "help")) {
            la64m68_opts_usage(argv[0]);
            return 1;
        }

        /* --name / --no-name for booleans; these never take a value */
        int neg = strncmp(name, "no-", 3) == 0;
        const char *bn = neg ? name + 3 : name;
        int matched = 0;
        for (size_t k = 0; k < sizeof(g_bools) / sizeof(g_bools[0]); k++) {
            if (strcmp(bn, g_bools[k].name)) continue;
            if (val) {
                fprintf(stderr, "la64m68: --%s takes no value\n", name);
                return -1;
            }
            *(int *)((char *)o + g_bools[k].off) = neg ? 0 : 1;
            matched = 1;
            break;
        }
        if (matched) continue;

        /* value options -- checked before fetching the value, so a stray
         * --bogus is reported as unknown and not as a missing value */
        for (size_t k = 0; k < sizeof(g_vals) / sizeof(g_vals[0]); k++) {
            if (strcmp(name, g_vals[k].name)) continue;
            if (!val) {
                if (++i >= argc) {
                    fprintf(stderr, "la64m68: --%s needs a value\n", name);
                    return -1;
                }
                val = argv[i];
            }
            if (g_vals[k].kind == V_STR) {
                *(const char **)((char *)o + g_vals[k].off) = val;
            } else {
                long v;
                if (parse_long(val, g_vals[k].min, g_vals[k].max, &v) != 0) {
                    fprintf(stderr,
                            "la64m68: --%s '%s' is not a valid value\n", name, val);
                    return -1;
                }
                *(long *)((char *)o + g_vals[k].off) = v;
            }
            matched = 1;
            break;
        }
        if (matched) continue;

        fprintf(stderr, "la64m68: unknown option '--%s' (see --help)\n", name);
        return -1;
    }
    return 0;
}
