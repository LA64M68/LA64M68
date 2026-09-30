#include "debug.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int trace_enabled(void)
{
    /* Capability is always compiled in (RULES), default is quiet. The switch
     * is read once: getenv() per call would sit in the instruction hot path.
     * Accepts the same truthy spellings as la64m68_cfg_get_bool(). */
    static int cached = -1;
    if (cached < 0) {
        const char *e = getenv("LA64M68_MAX_DEBUG");
        cached = (e && (*e == '1' || *e == 'y' || *e == 'Y' ||
                        !strcmp(e, "true") || !strcmp(e, "on"))) ? 1 : 0;
    }
    return cached;
}

void la64m68_trace(const char *fmt, ...)
{
    if (!trace_enabled()) return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "la64m68: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}
