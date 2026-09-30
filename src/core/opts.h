#ifndef LA64M68_OPTS_H
#define LA64M68_OPTS_H

/* Command line options.
 *
 * LA64M68 is configured from its own command line only. There is deliberately
 * no configuration file: no `la64m68.ini`, no /etc, no XDG, no HOME lookup
 * (see host.h -- the solution directory is self-contained). Example command
 * lines live in ./scripts/ next to the executable.
 *
 * Every option has a built-in default, so `la64m68` runs with no arguments at
 * all. Numbers are validated strictly: `--ram-kb 12abc` is an error, never a
 * silent fallback to a default. */

typedef struct la64m68_opts {
    /* core */
    const char *rom;        /* raw ROM image to load, NULL = none */
    long         rom_addr;  /* load address */
    int          rom_vectors; /* install the reset vectors from the ROM head */
    const char *kickstart;  /* Amiga Kickstart ROM, standard placement */
    const char *tos;        /* Atari TOS ROM, standard placement */
    const char *disk;       /* storage spec KIND:PATH (raw:.. / qcow2:..) */
    int          disk_ro;   /* attach the disk read-only */
    const char *plugin;     /* backend module, e.g. plugins/libla64m68_amiga.so */
    const char *fs;          /* host directory exposed to the guest */
    long         ram_kb;    /* guest RAM in KiB */
    long         max_steps; /* instructions to run */
    /* PiS passthrough (real Amiga bus) */
    int          pis;
    int          pis_arm;   /* hardware gate: drive the bus (default OFF) */
    const char  *pis_variant;      /* auto|classic|32|32lite|pistormx */
    const char  *fpga_bitstream;   /* Efinix bitstream, NULL = none */
    /* devices */
    int          chipset;   /* vAGA chipset fallback */
    int          vrtg;
    const char  *vrtg_backend;
    long         vrtg_fb_addr;
    int          vnic;
    const char  *vnic_mode;
    const char  *vnic_ifname;
    int          vhid;
    const char  *vhid_devices;
} la64m68_opts;

/* Built-in defaults. */
void la64m68_opts_defaults(la64m68_opts *o);

/* Parse argv[1..argc-1].
 * Returns  0 on success
 *          1 when the caller should exit(0) (--help was given)
 *         -1 on a usage error (the message has already been printed). */
int  la64m68_opts_parse(la64m68_opts *o, int argc, char **argv);

void la64m68_opts_usage(const char *prog);

#endif
