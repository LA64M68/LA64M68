#ifndef LA64M68_HOST_H
#define LA64M68_HOST_H

#include <stdint.h>

/* Linux ARM64 host integration: framebuffer, input, timing. */

typedef struct la64m68_host la64m68_host;

la64m68_host *la64m68_host_create(void);
void          la64m68_host_destroy(la64m68_host *host);
int           la64m68_host_step(la64m68_host *host);

/* ---- program root ------------------------------------------------------
 *
 * LA64M68 is a self-contained solution directory. It installs nothing into
 * POSIX/Linux/BSD hierarchies: no /usr/share, no /etc, no XDG, no HOME.
 * The directory that holds the `la64m68` executable IS the root, whether the
 * tree was built in place or unpacked from a release .tar.xz.
 *
 * la64m68_host_selfroot() resolves that directory and makes it the process
 * working directory, so every relative path the program uses is root-relative
 * no matter where it was started from. Call it first, before anything else
 * touches the filesystem.
 *
 * Resolution order:
 *   1. the running executable (/proc/self/exe on Linux, KERN_PROC_PATHNAME
 *      on BSD) -- authoritative and symlink-resolving
 *   2. argv[0], when it carries a path
 *   3. the current directory
 *
 * Returns the absolute root, or NULL when it cannot be determined. The
 * returned pointer is owned by the function and stays valid for the process
 * lifetime. argv0 may be NULL. */
const char *la64m68_host_selfroot(const char *argv0);

/* The resolved program root, or NULL before la64m68_host_selfroot() ran. */
const char *la64m68_host_root(void);

#endif
