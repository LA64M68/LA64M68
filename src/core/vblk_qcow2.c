#define _POSIX_C_SOURCE 200809L

/* qcow2 image backend (QEMU Copy-On-Write, version 2 and 3).
 *
 * Implemented from the published qcow2 format specification: the file is a
 * two-level sparse map, an L1 table of L2 table pointers and L2 tables of
 * cluster pointers. Reading a virtual offset walks that map; an unallocated
 * cluster reads as zero.
 *
 * Scope, deliberately:
 *   - versions 2 and 3, no encryption, uncompressed clusters
 *   - READ always; WRITE only into clusters that already exist
 *   - REFUSED: encryption, backing files, compressed clusters
 *
 * The refusals are fail-closed on purpose. A backing file in particular would
 * make unallocated clusters read as zero instead of as the parent's data --
 * silently wrong bytes, which is worse than not opening the image at all.
 *
 * Write allocation (growing a sparse image) needs refcount updates and is not
 * implemented; a write into an unallocated cluster reports VBLK_ERR_RANGE. */

#include "vblk.h"
#include "debug.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define QCOW_MAGIC          0x514649fbu   /* "QFI\xfb" */
#define QCOW_HEADER_MIN     72
#define QCOW_L2_OFFSET_MASK 0x00fffffffffffe00ull
#define QCOW_L2_COMPRESSED  (1ull << 62)
#define QCOW_L1_PRESENT     0x0000000000000001ull

typedef struct {
    int fd;
    int ro;
    uint64_t file_bytes;
    uint32_t cluster_bits;
    uint64_t cluster_size;
    uint64_t virtual_size;
    uint32_t l1_size;
    uint64_t l1_offset;
    uint64_t *l1;                 /* in-memory L1 table */
} qcow_ctx;

static int rd64(qcow_ctx *q, uint64_t off, uint64_t *out)
{
    unsigned char b[8];
    if (pread(q->fd, b, 8, (off_t)off) != 8) return -1;
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v = (v << 8) | b[i];
    *out = v;
    return 0;
}

static int rd32(qcow_ctx *q, uint64_t off, uint32_t *out)
{
    unsigned char b[4];
    if (pread(q->fd, b, 4, (off_t)off) != 4) return -1;
    *out = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
           ((uint32_t)b[2] << 8) | b[3];
    return 0;
}

/* Resolve one virtual byte offset to the host file offset of its cluster.
 * Returns 0 and the file offset, 1 when the cluster is unallocated (reads as
 * zero), or -1 on error. */
static int map_cluster(qcow_ctx *q, uint64_t voff, uint64_t *foff)
{
    uint32_t cs = (uint32_t)q->cluster_size;
    uint64_t cluster_index = voff >> q->cluster_bits;

    /* L2 entries per table = cluster_size / 8 */
    uint32_t l2_entries = cs / 8;
    uint64_t l1_index = cluster_index / l2_entries;
    if (l1_index >= q->l1_size) return 1;         /* beyond the map = hole */

    if (q->l1[l1_index] == 0) return 1;           /* no L2 table = hole */
    uint64_t l2_off = q->l1[l1_index] & QCOW_L2_OFFSET_MASK;
    if (!l2_off) return 1;

    uint64_t l2_index = cluster_index % l2_entries;
    uint64_t entry;
    if (rd64(q, l2_off + l2_index * 8, &entry) != 0) return -1;
    if (entry & QCOW_L2_COMPRESSED) return -1;    /* not supported */
    if ((entry & QCOW_L2_OFFSET_MASK) == 0) return 1;   /* hole */

    *foff = (entry & QCOW_L2_OFFSET_MASK) + (voff & (q->cluster_size - 1));
    return 0;
}

static int q_read(void *ctx, uint64_t off, void *buf, size_t len)
{
    qcow_ctx *q = ctx;
    unsigned char *p = buf;
    size_t done = 0;
    while (done < len) {
        uint64_t foff;
        int m = map_cluster(q, off + done, &foff);
        if (m < 0) return -1;
        /* never let a read run past the end of the containing cluster */
        uint64_t in_cluster = q->cluster_size - ((off + done) &
                                                 (q->cluster_size - 1));
        size_t chunk = len - done;
        if ((uint64_t)chunk > in_cluster) chunk = (size_t)in_cluster;

        if (m == 1) {
            memset(p + done, 0, chunk);          /* hole reads as zero */
        } else {
            ssize_t n = pread(q->fd, p + done, chunk, (off_t)foff);
            if (n != (ssize_t)chunk) return -1;
        }
        done += chunk;
    }
    return 0;
}

static int q_write(void *ctx, uint64_t off, const void *buf, size_t len)
{
    qcow_ctx *q = ctx;
    if (q->ro) return -1;
    const unsigned char *p = buf;
    size_t done = 0;
    while (done < len) {
        uint64_t foff;
        int m = map_cluster(q, off + done, &foff);
        if (m != 0) return -1;      /* hole or error: no allocation implemented */
        uint64_t in_cluster = q->cluster_size - ((off + done) &
                                                 (q->cluster_size - 1));
        size_t chunk = len - done;
        if ((uint64_t)chunk > in_cluster) chunk = (size_t)in_cluster;
        ssize_t n = pwrite(q->fd, p + done, chunk, (off_t)foff);
        if (n != (ssize_t)chunk) return -1;
        done += chunk;
    }
    return 0;
}

static uint64_t q_size(void *ctx)
{
    return ((qcow_ctx *)ctx)->virtual_size;
}

static int q_read_only(void *ctx)
{
    return ((qcow_ctx *)ctx)->ro;
}

static void q_close(void *ctx)
{
    qcow_ctx *q = ctx;
    free(q->l1);
    if (q->fd >= 0) close(q->fd);
    free(q);
}

static const la64m68_vblk_ops g_qcow2_ops = {
    "qcow2", q_read, q_write, q_size, q_read_only, q_close
};

int la64m68_vblk_open_qcow2(const char *path, int writable,
                            la64m68_vblk_src *out)
{
    if (!path || !out) return -1;
    int fd = open(path, writable ? O_RDWR : O_RDONLY);
    if (fd < 0) {
        la64m68_trace("vblk: qcow2 open %s failed errno=%d", path, errno);
        return -1;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        return -1;
    }

    qcow_ctx *q = calloc(1, sizeof(*q));
    if (!q) { close(fd); return -1; }
    q->fd = fd;
    q->ro = !writable;
    q->file_bytes = (uint64_t)st.st_size;

    unsigned char hdr[QCOW_HEADER_MIN];
    if (q->file_bytes < QCOW_HEADER_MIN ||
        pread(fd, hdr, sizeof(hdr), 0) != QCOW_HEADER_MIN) {
        la64m68_trace("vblk: qcow2 %s too small", path);
        goto fail;
    }
    uint32_t magic = ((uint32_t)hdr[0] << 24) | ((uint32_t)hdr[1] << 16) |
                     ((uint32_t)hdr[2] << 8) | hdr[3];
    if (magic != QCOW_MAGIC) {
        la64m68_trace("vblk: %s is not a qcow2 image (magic %08x)", path, magic);
        goto fail;
    }

    uint32_t version, cluster_bits, crypt, l1_size;
    uint64_t backing, size, l1_off;
    if (rd32(q, 4, &version) != 0) goto fail;
    if (rd64(q, 8, &backing) != 0) goto fail;
    if (rd32(q, 20, &cluster_bits) != 0) goto fail;
    if (rd64(q, 24, &size) != 0) goto fail;
    if (rd32(q, 32, &crypt) != 0) goto fail;
    if (rd32(q, 36, &l1_size) != 0) goto fail;
    if (rd64(q, 40, &l1_off) != 0) goto fail;

    if (version != 2 && version != 3) {
        la64m68_trace("vblk: qcow2 version %u not supported", version);
        goto fail;
    }
    if (crypt != 0) {
        la64m68_trace("vblk: qcow2 encrypted image refused (%s)", path);
        goto fail;
    }
    if (backing != 0) {
        /* fail closed: unallocated clusters would otherwise read as zero
         * instead of as the parent image's data */
        la64m68_trace("vblk: qcow2 backing file refused (%s) -- chains are "
                      "not supported", path);
        goto fail;
    }
    if (cluster_bits < 9 || cluster_bits > 21) {
        la64m68_trace("vblk: qcow2 cluster_bits %u out of range", cluster_bits);
        goto fail;
    }
    if (l1_size == 0 || l1_size > (1u << 24)) {
        la64m68_trace("vblk: qcow2 l1_size %u out of range", l1_size);
        goto fail;
    }

    q->cluster_bits = cluster_bits;
    q->cluster_size = 1ull << cluster_bits;
    q->virtual_size = size;
    q->l1_size = l1_size;
    q->l1_offset = l1_off;

    q->l1 = calloc(l1_size, sizeof(uint64_t));
    if (!q->l1) goto fail;
    for (uint32_t i = 0; i < l1_size; i++) {
        if (rd64(q, l1_off + (uint64_t)i * 8, &q->l1[i]) != 0) goto fail;
    }

    out->ops = &g_qcow2_ops;
    out->ctx = q;
    out->kind = VBLK_KIND_QCOW2;
    la64m68_trace("vblk: qcow2 v%u %s %llu bytes, cluster %llu, l1=%u%s",
                  version, path, (unsigned long long)size,
                  (unsigned long long)q->cluster_size, l1_size,
                  q->ro ? " (read-only)" : "");
    return 0;

fail:
    q_close(q);
    return -1;
}
