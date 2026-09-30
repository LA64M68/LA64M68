#ifndef LA64M68_PLUGIN_H
#define LA64M68_PLUGIN_H

#include <stdint.h>

/* Plugin interface for architecture-specific backends (Amiga, Atari, ...). */
typedef struct la64m68_plugin {
    const char *name;
    void *ctx;
    void (*reset)(void *ctx);
    void (*tick)(void *ctx, uint32_t cycles);
    uint32_t (*read32)(void *ctx, uint32_t addr);
    void (*write32)(void *ctx, uint32_t addr, uint32_t value);
} la64m68_plugin;

#endif
