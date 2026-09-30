#define _POSIX_C_SOURCE 200809L

#include "plugin_dl.h"
#include "../core/debug.h"
#include <dlfcn.h>
#include <stdlib.h>

#define LA64M68_PLUGIN_MAX 8

typedef struct {
    void *handle;
    la64m68_plugin *plugin;
} plugin_slot;

static plugin_slot g_slots[LA64M68_PLUGIN_MAX];

la64m68_plugin *la64m68_plugin_load(const char *path)
{
    if (!path || !*path) return NULL;

    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        la64m68_trace("plugin: dlopen %s failed: %s", path, dlerror());
        return NULL;
    }

    /* The entry point must be a plain function returning the descriptor. */
    la64m68_plugin *(*create)(void);
    *(void **)&create = dlsym(h, "la64m68_plugin_create");
    if (!create) {
        la64m68_trace("plugin: %s has no la64m68_plugin_create", path);
        dlclose(h);
        return NULL;
    }

    la64m68_plugin *p = create();
    if (!p || !p->name) {
        la64m68_trace("plugin: %s returned no usable descriptor", path);
        dlclose(h);
        return NULL;
    }

    /* keep the handle for the process lifetime: no unload, no teardown ABI */
    for (int i = 0; i < LA64M68_PLUGIN_MAX; i++) {
        if (!g_slots[i].handle) {
            g_slots[i].handle = h;
            g_slots[i].plugin = p;
            break;
        }
    }
    la64m68_trace("plugin: loaded %s (%s)", path, p->name);
    return p;
}

int la64m68_plugin_is_loaded(const la64m68_plugin *p)
{
    if (!p) return 0;
    for (int i = 0; i < LA64M68_PLUGIN_MAX; i++)
        if (g_slots[i].plugin == p) return 1;
    return 0;
}
