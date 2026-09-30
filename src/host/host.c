#define _GNU_SOURCE 1

#include "host.h"
#include "debug.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
#include <sys/types.h>
#include <sys/sysctl.h>
#endif

struct la64m68_host {
    int dummy;
};

#ifndef LA64M68_ROOT_MAX
#define LA64M68_ROOT_MAX 4096
#endif

static char g_root[LA64M68_ROOT_MAX];

la64m68_host *la64m68_host_create(void)
{
    la64m68_host *h = calloc(1, sizeof(*h));
    la64m68_trace("host: linux arm64 host created");
    return h;
}

void la64m68_host_destroy(la64m68_host *host)
{
    free(host);
}

int la64m68_host_step(la64m68_host *host)
{
    (void)host;
    /* Framebuffer/input bridge placeholder. */
    return 0;
}

/* Copy the directory part of `path` into out. Returns 0 on success. */
static int dirname_of(const char *path, char *out, size_t outsz)
{
    size_t n = strlen(path);
    if (n == 0 || n >= outsz)
        return -1;
    memcpy(out, path, n + 1);
    char *slash = strrchr(out, '/');
    if (!slash)
        return -1;
    if (slash == out)
        slash[1] = 0;                    /* "/la64m68" -> "/" */
    else
        *slash = 0;
    return 0;
}

const char *la64m68_host_selfroot(const char *argv0)
{
    char exe[LA64M68_ROOT_MAX];
    int have = 0;

    /* 1. the running executable: authoritative, and it resolves symlinks so
     *    a linked binary still finds the solution it really lives in. */
#if defined(__linux__)
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n > 0) {
        exe[n] = 0;
        if (dirname_of(exe, g_root, sizeof(g_root)) == 0)
            have = 1;
    }
#elif defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
    {
        int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, -1 };
        size_t len = sizeof(exe);
        if (sysctl(mib, 4, exe, &len, NULL, 0) == 0 && len > 1)
            have = dirname_of(exe, g_root, sizeof(g_root)) == 0;
    }
#endif

    /* 2. argv[0], when it carries a path */
    if (!have && argv0 && strchr(argv0, '/') &&
        dirname_of(argv0, g_root, sizeof(g_root)) == 0)
        have = 1;

    /* 3. last resort */
    if (!have && !getcwd(g_root, sizeof(g_root))) {
        la64m68_trace("host: cannot determine the program root");
        return NULL;
    }

    if (chdir(g_root) != 0) {
        la64m68_trace("host: cannot chdir to the program root %s", g_root);
        return NULL;
    }
    /* normalise: makes a relative argv[0] result absolute and drops ".." */
    if (!getcwd(g_root, sizeof(g_root))) {
        la64m68_trace("host: cannot resolve the program root");
        return NULL;
    }

    la64m68_trace("host: program root %s (self-contained, no system paths)",
                  g_root);
    return g_root;
}

const char *la64m68_host_root(void)
{
    return g_root[0] ? g_root : NULL;
}
