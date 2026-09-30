#define _POSIX_C_SOURCE 200809L

#include "vblk.h"
#include "debug.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---- raw backend: a plain image file, sector N at byte N*512 ------------ */

typedef struct {
    int fd;
    int ro;
    uint64_t bytes;
} raw_ctx;

static int raw_read(void *ctx, uint64_t off, void *buf, size_t len)
{
    raw_ctx *r = ctx;
    uint8_t *p = buf;
    size_t got = 0;
    while (got < len) {
        ssize_t n = pread(r->fd, p + got, len - got, (off_t)(off + got));
        if (n <= 0) return -1;
        got += (size_t)n;
    }
    return 0;
}

static int raw_write(void *ctx, uint64_t off, const void *buf, size_t len)
{
    raw_ctx *r = ctx;
    if (r->ro) return -1;
    const uint8_t *p = buf;
    size_t put = 0;
    while (put < len) {
        ssize_t n = pwrite(r->fd, p + put, len - put, (off_t)(off + put));
        if (n <= 0) return -1;
        put += (size_t)n;
    }
    return 0;
}

static uint64_t raw_size(void *ctx)
{
    return ((raw_ctx *)ctx)->bytes;
}

static int raw_read_only(void *ctx)
{
    return ((raw_ctx *)ctx)->ro;
}

static void raw_close(void *ctx)
{
    raw_ctx *r = ctx;
    if (r->fd >= 0) close(r->fd);
    free(r);
}

static const la64m68_vblk_ops g_raw_ops = {
    "raw", raw_read, raw_write, raw_size, raw_read_only, raw_close
};

int la64m68_vblk_open_raw(const char *path, int writable,
                          la64m68_vblk_src *out)
{
    if (!path || !out) return -1;
    int fd = open(path, writable ? O_RDWR : O_RDONLY);
    if (fd < 0) {
        la64m68_trace("vblk: raw open %s failed errno=%d", path, errno);
        return -1;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        return -1;
    }
    raw_ctx *r = calloc(1, sizeof(*r));
    if (!r) { close(fd); return -1; }
    r->fd = fd;
    r->ro = !writable;
    r->bytes = (uint64_t)st.st_size;

    out->ops = &g_raw_ops;
    out->ctx = r;
    out->kind = VBLK_KIND_RAW;
    la64m68_trace("vblk: raw %s %llu bytes%s", path,
                  (unsigned long long)r->bytes, r->ro ? " (read-only)" : "");
    return 0;
}

void la64m68_vblk_src_close(la64m68_vblk_src *s)
{
    if (!s || !s->ops) return;
    if (s->ops->close) s->ops->close(s->ctx);
    s->ops = NULL;
    s->ctx = NULL;
    s->kind = VBLK_KIND_NONE;
}

/* ---- device ------------------------------------------------------------ */

void la64m68_vblk_init(la64m68_vblk *b, la64m68_memory *guest_mem)
{
    memset(b, 0, sizeof(*b));
    b->guest_mem = guest_mem;
}

void la64m68_vblk_attach(la64m68_vblk *b, const la64m68_vblk_src *src)
{
    la64m68_vblk_src_close(&b->src);
    if (src) b->src = *src;
}

uint64_t la64m68_vblk_sectors(const la64m68_vblk *b)
{
    if (!b || !b->src.ops) return 0;
    return b->src.ops->size(b->src.ctx) / LA64M68_VBLK_SECTOR;
}

/* Run one command. Transfer is whole sectors at a sector-aligned LBA. */
static void vblk_run(la64m68_vblk *b, int cmd)
{
    b->error = VBLK_ERR_NONE;
    b->result = 0;

    if (!b->src.ops) {
        b->error = VBLK_ERR_IO;
        return;
    }
    uint64_t sectors = la64m68_vblk_sectors(b);
    if (b->count == 0) return;
    /* the range must not wrap and must stay inside the device */
    if (b->lba > sectors || b->count > sectors - b->lba) {
        b->error = VBLK_ERR_RANGE;
        return;
    }
    if (cmd == VBLK_CMD_WRITE && b->src.ops->read_only(b->src.ctx)) {
        b->error = VBLK_ERR_RDONLY;
        return;
    }

    /* one sector at a time keeps the guest buffer arithmetic exact and the
     * partial-failure result meaningful */
    for (uint32_t i = 0; i < b->count; i++) {
        uint8_t sec[LA64M68_VBLK_SECTOR];
        uint64_t off = (b->lba + i) * LA64M68_VBLK_SECTOR;
        uint32_t gaddr = b->buf + i * LA64M68_VBLK_SECTOR;

        if (cmd == VBLK_CMD_READ) {
            if (b->src.ops->read(b->src.ctx, off, sec, sizeof(sec)) != 0) {
                b->error = VBLK_ERR_IO;
                return;
            }
            for (uint32_t k = 0; k < sizeof(sec); k++)
                la64m68_mem_write8(b->guest_mem, gaddr + k, sec[k]);
        } else {
            for (uint32_t k = 0; k < sizeof(sec); k++)
                sec[k] = la64m68_mem_read8(b->guest_mem, gaddr + k);
            if (b->src.ops->write(b->src.ctx, off, sec, sizeof(sec)) != 0) {
                b->error = VBLK_ERR_IO;
                return;
            }
        }
        b->result++;
    }
}

typedef struct {
    la64m68_memory base;
    la64m68_vblk *b;
} vblk_regs_mem;

static uint32_t vr_r(vblk_regs_mem *m, uint32_t addr)
{
    la64m68_vblk *b = m->b;
    switch (addr - LA64M68_VBLK_REG_BASE) {
    case VBLK_REG_CTRL:    return b->enabled;
    case VBLK_REG_STATUS: {
        uint32_t s = b->error ? 2u : 1u;
        if (b->src.ops && b->src.ops->read_only(b->src.ctx)) s |= 4u;
        return s;
    }
    case VBLK_REG_RESULT:  return (uint32_t)b->result;
    case VBLK_REG_LBA0:    return (uint32_t)b->lba;
    case VBLK_REG_LBA1:    return (uint32_t)(b->lba >> 32);
    case VBLK_REG_COUNT:   return b->count;
    case VBLK_REG_BUF:     return b->buf;
    case VBLK_REG_CAP0:    return (uint32_t)la64m68_vblk_sectors(b);
    case VBLK_REG_CAP1:    return (uint32_t)(la64m68_vblk_sectors(b) >> 32);
    case VBLK_REG_KIND:    return (uint32_t)b->src.kind;
    default: return 0;
    }
}

static void vr_w(vblk_regs_mem *m, uint32_t addr, uint32_t val)
{
    la64m68_vblk *b = m->b;
    switch (addr - LA64M68_VBLK_REG_BASE) {
    case VBLK_REG_CTRL:   b->enabled = val & 1; break;
    case VBLK_REG_LBA0:   b->lba = (b->lba & 0xffffffff00000000ull) | val; break;
    case VBLK_REG_LBA1:   b->lba = (b->lba & 0xffffffffull) |
                                  ((uint64_t)val << 32); break;
    case VBLK_REG_COUNT:  b->count = val; break;
    case VBLK_REG_BUF:    b->buf = val; break;
    case VBLK_REG_CMD:
        vblk_run(b, (int)val);
        break;
    default: break;
    }
}

static uint8_t  vr_r8 (void *c, uint32_t a) { return (uint8_t) vr_r(c, a); }
static uint16_t vr_r16(void *c, uint32_t a) { return (uint16_t)vr_r(c, a); }
static uint32_t vr_r32(void *c, uint32_t a) { return vr_r(c, a); }
static void     vr_w8 (void *c, uint32_t a, uint8_t v)  { vr_w(c, a, v); }
static void     vr_w16(void *c, uint32_t a, uint16_t v) { vr_w(c, a, v); }
static void     vr_w32(void *c, uint32_t a, uint32_t v) { vr_w(c, a, v); }

la64m68_memory *la64m68_vblk_regs_memory(la64m68_vblk *b)
{
    vblk_regs_mem *m = calloc(1, sizeof(*m));
    if (!m) return NULL;
    m->b = b;
    m->base.ctx = m;
    m->base.read8 = vr_r8;   m->base.read16 = vr_r16;  m->base.read32 = vr_r32;
    m->base.write8 = vr_w8;  m->base.write16 = vr_w16; m->base.write32 = vr_w32;
    return &m->base;
}

void la64m68_vblk_regs_destroy(la64m68_memory *m)
{
    free(m);
}
