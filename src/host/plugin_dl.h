#ifndef LA64M68_PLUGIN_DL_H
#define LA64M68_PLUGIN_DL_H

#include "../core/plugin.h"

/* Runtime loading of the architecture backends (Amiga, Atari).
 *
 * A backend is a shared module exporting one symbol:
 *
 *     la64m68_plugin *la64m68_plugin_create(void);
 *
 * The returned object describes itself and is kept for the lifetime of the
 * process -- the module is never unloaded, so the backend may hold pointers
 * into its own text without needing an ABI teardown call.
 *
 * Lives in src/host because opening shared objects is a host concern
 * (0-POOL/RULES.md: platform specifics stay behind the host HAL).
 * Returns NULL when the file is missing, is not a module, or does not export
 * the entry point. */
la64m68_plugin *la64m68_plugin_load(const char *path);

/* The module handle for an already loaded plugin, or NULL. Only needed by
 * code that wants to distinguish "loaded" from "not". */
int la64m68_plugin_is_loaded(const la64m68_plugin *p);

#endif
