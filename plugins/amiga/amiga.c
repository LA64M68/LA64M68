#include "plugin.h"
#include "debug.h"
#include <stdlib.h>

typedef struct {
    int initialized;
} amiga_ctx;

static void amiga_reset(void *ctx)
{
    amiga_ctx *a = ctx;
    a->initialized = 1;
    la64m68_trace("amiga: reset");
}

static void amiga_tick(void *ctx, uint32_t cycles)
{
    (void)ctx;
    (void)cycles;
}

la64m68_plugin *la64m68_plugin_create(void)
{
    amiga_ctx *a = calloc(1, sizeof(*a));
    la64m68_plugin *p = calloc(1, sizeof(*p));
    if (!a || !p) { free(a); free(p); return NULL; }
    p->name = "amiga";
    p->ctx = a;
    p->reset = amiga_reset;
    p->tick = amiga_tick;
    return p;
}
