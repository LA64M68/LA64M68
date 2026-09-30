#ifndef LA64M68_VFS_H
#define LA64M68_VFS_H

#include "memory.h"
#include <stdint.h>

/* VFS — host filesystem passthrough.
 *
 * This is the third way to hold Amiga/Atari content, and unlike raw/qcow2 it
 * is NOT a sector device: it hands the guest the files that simply lie on
 * Linux, by name. No image, no container.
 *
 * The guest sees one directory -- the root given at setup -- and nothing else.
 * A name is a relative path inside that root. Escape attempts are rejected:
 * absolute paths, `..` components and symlink escapes out of the root all fail.
 * That boundary is the point of the module, so it is enforced on every open.
 *
 * Out of scope, like the other storage forms: .adf, .st, .hdf containers. */

#define LA64M68_VFS_REG_BASE   0x00F40000

#define VFS_REG_CTRL      0x00   /* bit0 enable */
#define VFS_REG_STATUS    0x04   /* bit0 ready, bit1 error */
#define VFS_REG_CMD       0x08   /* write: VFS_CMD_* */
#define VFS_REG_NAME      0x0c   /* guest addr of a NUL-terminated name */
#define VFS_REG_BUF       0x10   /* guest addr of the data buffer */
#define VFS_REG_LEN       0x14   /* bytes to transfer / result */
#define VFS_REG_OFF0      0x18   /* file offset, low 32 bits */
#define VFS_REG_OFF1      0x1c   /* file offset, high 16 bits */
#define VFS_REG_INDEX     0x20   /* list: entry index */
#define VFS_REG_ENTLEN    0x24   /* read: length of the last listed name */

#define VFS_CMD_LIST      1      /* INDEX -> entry name copied to BUF */
#define VFS_CMD_OPEN      2      /* open NAME as the current file */
#define VFS_CMD_READ      3      /* read LEN at OFF into BUF */
#define VFS_CMD_WRITE     4      /* write LEN from BUF at OFF */
#define VFS_CMD_CLOSE     5

#define VFS_NAME_MAX      256

#define VFS_ERR_NONE      0
#define VFS_ERR_IO        1
#define VFS_ERR_RANGE     2
#define VFS_ERR_NAME      3      /* malformed or escaping name */
#define VFS_ERR_NOFILE    4

typedef struct la64m68_vfs {
    int enabled;
    int error;
    uint32_t len;                /* requested transfer length */
    uint32_t result;             /* bytes transferred / entry name length */
    uint32_t name_addr, buf, index;
    uint64_t off;
    char root[512];
    void *dir;                   /* DIR* while listing */
    void *file;                  /* FILE* for the open file */
    la64m68_memory *guest_mem;
} la64m68_vfs;

/* Set up with the host directory the guest may see. */
void la64m68_vfs_init(la64m68_vfs *v, la64m68_memory *guest_mem, const char *root);

la64m68_memory *la64m68_vfs_regs_memory(la64m68_vfs *v);
void            la64m68_vfs_regs_destroy(la64m68_memory *m);

/* Pure helper, separately testable: 0 when `name` is a safe relative path
 * inside the root, -1 when it tries to escape or is malformed. */
int la64m68_vfs_name_ok(const char *name);

#endif
