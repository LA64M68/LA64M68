#include "plugin.h"
#include "debug.h"
#include <stdlib.h>

typedef struct {
    int initialized;
} atari_ctx;

static void atari_reset(void *ctx)
{
    atari_ctx *a = ctx;
    a->initialized = 1;
    la64m68_trace("atari: reset");
}

static void atari_tick(void *ctx, uint32_t cycles)
{
    (void)ctx;
    (void)cycles;
}

la64m68_plugin *la64m68_plugin_create(void)
{
    atari_ctx *a = calloc(1, sizeof(*a));
    la64m68_plugin *p = calloc(1, sizeof(*p));
    if (!p) return NULL;
    p->abi = LA64M68_PLUGIN_ABI;
    if (!a || !p) { free(a); free(p); return NULL; }
    p->name = "atari";
    p->ctx = a;
    p->reset = atari_reset;
    p->tick = atari_tick;
    return p;
}
