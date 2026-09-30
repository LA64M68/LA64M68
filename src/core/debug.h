#ifndef LA64M68_DEBUG_H
#define LA64M68_DEBUG_H

/* AI-friendly debug/trace. INI-gated via env LA64M68_MAX_DEBUG.
 * Capability is always compiled in; default stays quiet. */

void la64m68_trace(const char *fmt, ...);

#endif
