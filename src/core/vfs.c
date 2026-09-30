#define _GNU_SOURCE 1

#include "vfs.h"
#include "debug.h"
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* ---- name validation (pure, separately tested) ------------------------- */

int la64m68_vfs_name_ok(const char *name)
{
    if (!name || !*name) return -1;
    if (strlen(name) >= VFS_NAME_MAX) return -1;
    if (name[0] == '/') return -1;                  /* absolute */

    /* no `..` component, and no empty component (a//b) */
    const char *p = name;
    while (*p) {
        const char *slash = strchr(p, '/');
        size_t len = slash ? (size_t)(slash - p) : strlen(p);
        if (len == 0) return -1;
        if (len == 2 && p[0] == '.' && p[1] == '.') return -1;
        if (!slash) break;
        p = slash + 1;
    }
    return 0;
}

/* The resolved target must live under the resolved root. This is what stops a
 * symlink inside the root from reaching outside it -- checking the name alone
 * would not. */
static int path_within_root(const char *rroot, const char *joined)
{
    char dirpart[PATH_MAX], rdir[PATH_MAX];
    snprintf(dirpart, sizeof(dirpart), "%s", joined);
    char *slash = strrchr(dirpart, '/');
    if (slash) *slash = 0;

    if (!realpath(dirpart, rdir)) return -1;
    size_t n = strlen(rroot);
    if (strncmp(rdir, rroot, n) != 0) return -1;
    return (rdir[n] == '/' || rdir[n] == 0) ? 0 : -1;
}

/* ---- guest register window -------------------------------------------- */

static void read_name(la64m68_vfs *v, char *out, size_t outsz)
{
    size_t i;
    for (i = 0; i + 1 < outsz; i++) {
        out[i] = (char)la64m68_mem_read8(v->guest_mem, v->name_addr + (uint32_t)i);
        if (!out[i]) break;
    }
    out[i] = 0;
}

static void vfs_cmd(la64m68_vfs *v, uint32_t cmd)
{
    v->error = VFS_ERR_NONE;
    v->result = 0;

    switch (cmd) {
    case VFS_CMD_LIST: {
        if (v->dir) { closedir((DIR *)v->dir); v->dir = NULL; }
        DIR *d = opendir(v->root);
        if (!d) { v->error = VFS_ERR_IO; return; }
        v->dir = d;
        struct dirent *e;
        for (uint32_t i = 0; (e = readdir(d)) != NULL; i++) {
            if (i != v->index) continue;
            size_t n = strlen(e->d_name);
            for (size_t k = 0; k < n; k++)
                la64m68_mem_write8(v->guest_mem, v->buf + (uint32_t)k,
                                   (uint8_t)e->d_name[k]);
            la64m68_mem_write8(v->guest_mem, v->buf + (uint32_t)n, 0);
            v->result = (uint32_t)n;
            return;
        }
        v->error = VFS_ERR_RANGE;                  /* past the last entry */
        return;
    }
    case VFS_CMD_OPEN: {
        char name[VFS_NAME_MAX], joined[PATH_MAX], rroot[PATH_MAX];
        read_name(v, name, sizeof(name));
        if (la64m68_vfs_name_ok(name) != 0) {
            la64m68_trace("vfs: rejected name '%s'", name);
            v->error = VFS_ERR_NAME;
            return;
        }
        if (!realpath(v->root, rroot)) { v->error = VFS_ERR_IO; return; }
        snprintf(joined, sizeof(joined), "%s/%s", v->root, name);
        if (path_within_root(rroot, joined) != 0) {
            la64m68_trace("vfs: '%s' escapes the root", name);
            v->error = VFS_ERR_NAME;
            return;
        }
        if (v->file) { fclose((FILE *)v->file); v->file = NULL; }
        FILE *f = fopen(joined, "r+b");
        if (!f) f = fopen(joined, "rb");          /* fall back to read-only */
        if (!f) { v->error = VFS_ERR_NOFILE; return; }
        v->file = f;
        return;
    }
    case VFS_CMD_READ: {
        if (!v->file) { v->error = VFS_ERR_NOFILE; return; }
        if (fseeko((FILE *)v->file, (off_t)v->off, SEEK_SET) != 0) {
            v->error = VFS_ERR_IO;
            return;
        }
        uint8_t chunk[512];
        uint32_t done = 0;
        while (done < v->len) {
            uint32_t want = v->len - done;
            if (want > sizeof(chunk)) want = (uint32_t)sizeof(chunk);
            size_t got = fread(chunk, 1, want, (FILE *)v->file);
            for (size_t k = 0; k < got; k++)
                la64m68_mem_write8(v->guest_mem, v->buf + done + (uint32_t)k,
                                   chunk[k]);
            done += (uint32_t)got;
            if (got < want) break;                 /* end of file */
        }
        v->result = done;
        v->off += done;
        return;
    }
    case VFS_CMD_WRITE: {
        if (!v->file) { v->error = VFS_ERR_NOFILE; return; }
        if (fseeko((FILE *)v->file, (off_t)v->off, SEEK_SET) != 0) {
            v->error = VFS_ERR_IO;
            return;
        }
        uint8_t chunk[512];
        uint32_t done = 0;
        while (done < v->len) {
            uint32_t want = v->len - done;
            if (want > sizeof(chunk)) want = (uint32_t)sizeof(chunk);
            for (uint32_t k = 0; k < want; k++)
                chunk[k] = la64m68_mem_read8(v->guest_mem, v->buf + done + k);
            size_t put = fwrite(chunk, 1, want, (FILE *)v->file);
            done += (uint32_t)put;
            if (put < want) { v->error = VFS_ERR_IO; break; }
        }
        v->result = done;
        v->off += done;
        return;
    }
    case VFS_CMD_CLOSE:
        if (v->file) { fclose((FILE *)v->file); v->file = NULL; }
        if (v->dir)  { closedir((DIR *)v->dir);  v->dir  = NULL; }
        return;
    default:
        v->error = VFS_ERR_RANGE;
        return;
    }
}

typedef struct {
    la64m68_memory base;
    la64m68_vfs *v;
} vfs_regs_mem;

static uint32_t fr_r(vfs_regs_mem *m, uint32_t addr)
{
    la64m68_vfs *v = m->v;
    switch (addr - LA64M68_VFS_REG_BASE) {
    case VFS_REG_CTRL:    return v->enabled;
    case VFS_REG_STATUS:  return v->error ? 2u : 1u;
    case VFS_REG_LEN:     return v->len;
    case VFS_REG_ENTLEN:  return v->result;
    case VFS_REG_OFF0:    return (uint32_t)v->off;
    case VFS_REG_OFF1:    return (uint32_t)(v->off >> 32);
    default: return 0;
    }
}

static void fr_w(vfs_regs_mem *m, uint32_t addr, uint32_t val)
{
    la64m68_vfs *v = m->v;
    switch (addr - LA64M68_VFS_REG_BASE) {
    case VFS_REG_CTRL:    v->enabled = val & 1; break;
    case VFS_REG_NAME:    v->name_addr = val; break;
    case VFS_REG_BUF:     v->buf = val; break;
    case VFS_REG_LEN:     v->len = val; break;
    case VFS_REG_OFF0:    v->off = (v->off & 0xffffffff00000000ull) | val; break;
    case VFS_REG_OFF1:    v->off = (v->off & 0xffffffffull) | ((uint64_t)val << 32); break;
    case VFS_REG_INDEX:   v->index = val; break;
    case VFS_REG_CMD:     vfs_cmd(v, val); break;
    default: break;
    }
}

static uint8_t  fr_r8 (void *c, uint32_t a) { return (uint8_t) fr_r(c, a); }
static uint16_t fr_r16(void *c, uint32_t a) { return (uint16_t)fr_r(c, a); }
static uint32_t fr_r32(void *c, uint32_t a) { return fr_r(c, a); }
static void     fr_w8 (void *c, uint32_t a, uint8_t v)  { fr_w(c, a, v); }
static void     fr_w16(void *c, uint32_t a, uint16_t v) { fr_w(c, a, v); }
static void     fr_w32(void *c, uint32_t a, uint32_t v) { fr_w(c, a, v); }

void la64m68_vfs_init(la64m68_vfs *v, la64m68_memory *guest_mem, const char *root)
{
    memset(v, 0, sizeof(*v));
    v->guest_mem = guest_mem;
    snprintf(v->root, sizeof(v->root), "%s", root ? root : ".");
}

la64m68_memory *la64m68_vfs_regs_memory(la64m68_vfs *v)
{
    vfs_regs_mem *m = calloc(1, sizeof(*m));
    if (!m) return NULL;
    m->v = v;
    m->base.ctx = m;
    m->base.read8 = fr_r8;   m->base.read16 = fr_r16;  m->base.read32 = fr_r32;
    m->base.write8 = fr_w8;  m->base.write16 = fr_w16; m->base.write32 = fr_w32;
    return &m->base;
}

void la64m68_vfs_regs_destroy(la64m68_memory *m)
{
    vfs_regs_mem *v = m ? (vfs_regs_mem *)m->ctx : NULL;
    if (v && v->v) {
        if (v->v->file) fclose((FILE *)v->v->file);
        if (v->v->dir)  closedir((DIR *)v->v->dir);
    }
    free(v);
}
