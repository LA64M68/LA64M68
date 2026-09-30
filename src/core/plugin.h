#ifndef LA64M68_PLUGIN_H
#define LA64M68_PLUGIN_H

#include <stdint.h>

/* Plugin interface for architecture-specific backends (Amiga, Atari, ...).
 *
 * A backend is a shared module exporting `la64m68_plugin_create()`. It must
 * set `abi` to LA64M68_PLUGIN_ABI: without a version a stale module built
 * against an older layout would load into a newer core and misbehave
 * silently. `destroy` releases anything the backend owns; the module itself
 * is never unloaded. */
#define LA64M68_PLUGIN_ABI 1

typedef struct la64m68_plugin {
    int abi;                     /* must be LA64M68_PLUGIN_ABI */
    const char *name;
    void *ctx;
    void (*reset)(void *ctx);
    void (*tick)(void *ctx, uint32_t cycles);
    uint32_t (*read32)(void *ctx, uint32_t addr);
    void (*write32)(void *ctx, uint32_t addr, uint32_t value);
    void (*destroy)(void *ctx);
} la64m68_plugin;

#endif
