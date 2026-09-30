#include "memory.h"
#include "debug.h"
#include <stdlib.h>
#include <string.h>

typedef struct la64m68_ram_bus {
    uint8_t *ram;
    size_t size;
    uint32_t base;
} la64m68_ram_bus;

static uint8_t ram_read8(void *ctx, uint32_t addr)
{
    la64m68_ram_bus *bus = ctx;
    uint32_t off = addr - bus->base;
    return off < bus->size ? bus->ram[off] : 0;
}

static uint16_t ram_read16(void *ctx, uint32_t addr)
{
    return (uint16_t)ram_read8(ctx, addr) << 8 | ram_read8(ctx, addr + 1);
}

static uint32_t ram_read32(void *ctx, uint32_t addr)
{
    return (uint32_t)ram_read16(ctx, addr) << 16 | ram_read16(ctx, addr + 2);
}

static void ram_write8(void *ctx, uint32_t addr, uint8_t v)
{
    la64m68_ram_bus *bus = ctx;
    uint32_t off = addr - bus->base;
    if (off < bus->size) bus->ram[off] = v;
}

static void ram_write16(void *ctx, uint32_t addr, uint16_t v)
{
    ram_write8(ctx, addr, (uint8_t)(v >> 8));
    ram_write8(ctx, addr + 1, (uint8_t)v);
}

static void ram_write32(void *ctx, uint32_t addr, uint32_t v)
{
    ram_write16(ctx, addr, (uint16_t)(v >> 16));
    ram_write16(ctx, addr + 2, (uint16_t)v);
}

la64m68_memory *la64m68_ram_memory_create(uint32_t base, size_t size)
{
    la64m68_ram_bus *bus = calloc(1, sizeof(*bus));
    if (!bus) return NULL;
    bus->ram = calloc(1, size);
    if (!bus->ram) { free(bus); return NULL; }
    bus->size = size;
    bus->base = base;

    la64m68_memory *m = calloc(1, sizeof(*m));
    if (!m) { free(bus->ram); free(bus); return NULL; }
    m->ctx = bus;
    m->read8 = ram_read8;
    m->read16 = ram_read16;
    m->read32 = ram_read32;
    m->write8 = ram_write8;
    m->write16 = ram_write16;
    m->write32 = ram_write32;
    la64m68_trace("memory: ram bus base=%08x size=%zu", base, size);
    return m;
}

void la64m68_ram_memory_destroy(la64m68_memory *m)
{
    if (!m) return;
    la64m68_ram_bus *bus = m->ctx;
    if (bus) {
        free(bus->ram);
        free(bus);
    }
    free(m);
}

/* ---- range router ---- */

typedef struct {
    uint32_t base, size;
    la64m68_memory *sub;
    int used;
} router_region;

typedef struct {
    router_region regions[LA64M68_ROUTER_MAX_REGIONS];
    uint32_t last_miss;      /* dedup for the unmapped-access trace */
    int      have_miss;
} router_ctx;

/* An unmapped access is never silently swallowed: it is the first thing to
 * look at when a ROM image or the iron misbehaves. Rate-limited against the
 * LAST address only -- a guest alternating between two unmapped addresses
 * still repeats itself. Good enough to stop a runaway loop flooding stderr,
 * not a general "once per address" log. */
static void router_unmapped(router_ctx *r, uint32_t addr, const char *op)
{
    if (r->have_miss && r->last_miss == addr) return;
    r->have_miss = 1;
    r->last_miss = addr;
    la64m68_trace("memory: unmapped %s @%08x", op, addr);
}

static router_region *router_find(router_ctx *r, uint32_t addr)
{
    /* Newest region first: device register windows are added after RAM and
     * must overlay it (see pis.h/fb.h priority rules). Searching forward
     * would let a large RAM region silently swallow every device window
     * inside it. addr - base < size is overflow-safe: base + size can wrap
     * 32 bits (e.g. base=0xfff00000 size=0x200000), which would mis-route. */
    for (int i = LA64M68_ROUTER_MAX_REGIONS - 1; i >= 0; i--) {
        router_region *e = &r->regions[i];
        if (e->used && addr >= e->base && addr - e->base < e->size)
            return e;
    }
    return NULL;
}

#define ROUTER_OP(name, type, expr_stmt)                                     \
static type name(void *ctx, uint32_t addr)                                   \
{                                                                            \
    router_region *e = router_find(ctx, addr);                               \
    if (!e) { router_unmapped(ctx, addr, #name); return (type)0; }           \
    return expr_stmt;                                                        \
}

ROUTER_OP(rt_read8,  uint8_t,  la64m68_mem_read8(e->sub, addr))
ROUTER_OP(rt_read16, uint16_t, la64m68_mem_read16(e->sub, addr))
ROUTER_OP(rt_read32, uint32_t, la64m68_mem_read32(e->sub, addr))

#define ROUTER_WR(name, vtype, call)                                         \
static void name(void *ctx, uint32_t addr, vtype v)                          \
{                                                                            \
    router_region *e = router_find(ctx, addr);                               \
    if (!e) { router_unmapped(ctx, addr, #name); return; }                   \
    call;                                                                    \
}

ROUTER_WR(rt_write8,  uint8_t,  la64m68_mem_write8(e->sub, addr, v))
ROUTER_WR(rt_write16, uint16_t, la64m68_mem_write16(e->sub, addr, v))
ROUTER_WR(rt_write32, uint32_t, la64m68_mem_write32(e->sub, addr, v))

la64m68_memory *la64m68_router_create(void)
{
    router_ctx *r = calloc(1, sizeof(*r));
    if (!r) return NULL;
    la64m68_memory *m = calloc(1, sizeof(*m));
    if (!m) { free(r); return NULL; }
    m->ctx = r;
    m->read8 = rt_read8;  m->read16 = rt_read16;  m->read32 = rt_read32;
    m->write8 = rt_write8; m->write16 = rt_write16; m->write32 = rt_write32;
    return m;
}

int la64m68_router_add(la64m68_memory *router, uint32_t base,
                       uint32_t size, la64m68_memory *sub)
{
    if (!router || !sub || !size) return -1;
    router_ctx *r = router->ctx;
    /* Overlap is legitimate (device windows overlay RAM) but worth tracing:
     * the newest region wins, so a shadowed window is a config surprise. */
    for (int i = 0; i < LA64M68_ROUTER_MAX_REGIONS; i++) {
        const router_region *e = &r->regions[i];
        if (!e->used) continue;
        if (base - e->base < e->size || e->base - base < size) {
            la64m68_trace("memory: %08x..%08x overlays %08x..%08x",
                          base, base + size - 1, e->base, e->base + e->size - 1);
            break;
        }
    }
    for (int i = 0; i < LA64M68_ROUTER_MAX_REGIONS; i++) {
        if (!r->regions[i].used) {
            r->regions[i] = (router_region){ base, size, sub, 1 };
            la64m68_trace("memory: route %08x..%08x", base, base + size - 1);
            return 0;
        }
    }
    la64m68_trace("memory: router full, refused %08x", base);
    return -1;
}

void la64m68_router_destroy(la64m68_memory *router)
{
    if (!router) return;
    free(router->ctx);
    free(router);
}
