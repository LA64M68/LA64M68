#ifndef LA64M68_MEMORY_H
#define LA64M68_MEMORY_H

#include <stdint.h>
#include <stddef.h>

typedef uint8_t  (*la64m68_read8_fn)(void *ctx, uint32_t addr);
typedef uint16_t (*la64m68_read16_fn)(void *ctx, uint32_t addr);
typedef uint32_t (*la64m68_read32_fn)(void *ctx, uint32_t addr);
typedef void     (*la64m68_write8_fn)(void *ctx, uint32_t addr, uint8_t value);
typedef void     (*la64m68_write16_fn)(void *ctx, uint32_t addr, uint16_t value);
typedef void     (*la64m68_write32_fn)(void *ctx, uint32_t addr, uint32_t value);

typedef struct la64m68_memory {
    void *ctx;
    la64m68_read8_fn  read8;
    la64m68_read16_fn read16;
    la64m68_read32_fn read32;
    la64m68_write8_fn  write8;
    la64m68_write16_fn write16;
    la64m68_write32_fn write32;
} la64m68_memory;

static inline uint8_t la64m68_mem_read8(la64m68_memory *m, uint32_t addr)
{
    return m && m->read8 ? m->read8(m->ctx, addr) : 0;
}
static inline uint16_t la64m68_mem_read16(la64m68_memory *m, uint32_t addr)
{
    return m && m->read16 ? m->read16(m->ctx, addr) : 0;
}
static inline uint32_t la64m68_mem_read32(la64m68_memory *m, uint32_t addr)
{
    return m && m->read32 ? m->read32(m->ctx, addr) : 0;
}
static inline void la64m68_mem_write8(la64m68_memory *m, uint32_t addr, uint8_t v)
{
    if (m && m->write8) m->write8(m->ctx, addr, v);
}
static inline void la64m68_mem_write16(la64m68_memory *m, uint32_t addr, uint16_t v)
{
    if (m && m->write16) m->write16(m->ctx, addr, v);
}
static inline void la64m68_mem_write32(la64m68_memory *m, uint32_t addr, uint32_t v)
{
    if (m && m->write32) m->write32(m->ctx, addr, v);
}

la64m68_memory *la64m68_ram_memory_create(uint32_t base, size_t size);
void            la64m68_ram_memory_destroy(la64m68_memory *m);

/* Address-range router: forwards each access to the sub-la64m68_memory whose
 * range covers the address. Region mix: host window, plugin emulation,
 * PiS bus passthrough all plug in as sub-memories. */
#define LA64M68_ROUTER_MAX_REGIONS 32

la64m68_memory *la64m68_router_create(void);
int             la64m68_router_add(la64m68_memory *router, uint32_t base,
                                   uint32_t size, la64m68_memory *sub);
void            la64m68_router_destroy(la64m68_memory *router); /* subs untouched */

#endif
