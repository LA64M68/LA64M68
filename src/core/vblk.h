#ifndef LA64M68_VBLK_H
#define LA64M68_VBLK_H

#include "memory.h"
#include <stdint.h>
#include <stddef.h>

/* vBLK — virtual block device.
 *
 * Guest side: a register window plus a sector command protocol. Host side: a
 * storage backend that serves 512-byte sectors from somewhere on Linux.
 *
 * Three ways to hold the Amiga/Atari content, chosen on the command line:
 *
 *   raw    a plain image file. Sector N is at byte N*512. Nothing else.
 *   qcow2  a qcow2 copy-on-write image (QEMU format), addressed through its
 *          L1/L2 cluster tables so a sparse file still reads as a full disk.
 *   fs     host filesystem passthrough: a designated host directory whose
 *          files the guest reaches by NAME -- see vfs.h. This is a file
 *          interface, not a sector device, so it lives beside vBLK rather
 *          than behind it. That is how content which simply lies on Linux is
 *          used without wrapping it in an image at all.
 *
 * Deliberately out of scope: .adf, .st, .hdf container formats.
 *
 * Sector size is 512, the Amiga/Atari disk standard. */

#define LA64M68_VBLK_REG_BASE   0x00F30000
#define LA64M68_VBLK_SECTOR     512u

#define VBLK_REG_CTRL      0x00   /* bit0 enable */
#define VBLK_REG_STATUS    0x04   /* bit0 ready, bit1 error, bit2 read-only */
#define VBLK_REG_CMD       0x08   /* write: VBLK_CMD_* */
#define VBLK_REG_LBA0      0x0c   /* sector number, low 32 bits */
#define VBLK_REG_LBA1      0x10   /* sector number, high 16 bits */
#define VBLK_REG_COUNT     0x14   /* sector count for the command */
#define VBLK_REG_BUF       0x18   /* guest address of the transfer buffer */
#define VBLK_REG_RESULT    0x1c   /* sectors transferred, or error code */
#define VBLK_REG_CAP0      0x20   /* capacity in sectors, low 32 bits */
#define VBLK_REG_CAP1      0x24   /* capacity in sectors, high 16 bits */
#define VBLK_REG_KIND      0x28   /* backend kind, see VBLK_KIND_* */

#define VBLK_CMD_READ      1
#define VBLK_CMD_WRITE     2

#define VBLK_KIND_NONE     0
#define VBLK_KIND_RAW      1
#define VBLK_KIND_QCOW2    2

#define VBLK_ERR_NONE      0
#define VBLK_ERR_IO        1
#define VBLK_ERR_RANGE     2
#define VBLK_ERR_RDONLY    3
#define VBLK_ERR_FORMAT    4

/* A storage backend. All I/O is whole sectors at sector-aligned offsets.
 * read/write return 0 on success, negative on failure. */
typedef struct la64m68_vblk_ops {
    const char *name;
    int      (*read)(void *ctx, uint64_t off, void *buf, size_t len);
    int      (*write)(void *ctx, uint64_t off, const void *buf, size_t len);
    uint64_t (*size)(void *ctx);       /* total bytes */
    int      (*read_only)(void *ctx);
    void     (*close)(void *ctx);
} la64m68_vblk_ops;

/* Backends. Each returns an ops table with its context, or NULL on failure. */
typedef struct la64m68_vblk_src {
    const la64m68_vblk_ops *ops;
    void *ctx;
    int kind;
} la64m68_vblk_src;

int la64m68_vblk_open_raw(const char *path, int writable,
                          la64m68_vblk_src *out);
int la64m68_vblk_open_qcow2(const char *path, int writable,
                            la64m68_vblk_src *out);
void la64m68_vblk_src_close(la64m68_vblk_src *s);

typedef struct la64m68_vblk {
    int enabled;
    int error;                    /* last VBLK_ERR_* */
    int result;                   /* sectors transferred on the last command */
    uint64_t lba;
    uint32_t count;
    uint32_t buf;
    la64m68_memory *guest_mem;
    la64m68_vblk_src src;
} la64m68_vblk;

void la64m68_vblk_init(la64m68_vblk *b, la64m68_memory *guest_mem);
/* Attach a source; takes ownership and closes it with the device. */
void la64m68_vblk_attach(la64m68_vblk *b, const la64m68_vblk_src *src);
uint64_t la64m68_vblk_sectors(const la64m68_vblk *b);

/* guest register window */
la64m68_memory *la64m68_vblk_regs_memory(la64m68_vblk *b);
void            la64m68_vblk_regs_destroy(la64m68_memory *m);

#endif
