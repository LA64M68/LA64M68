#ifndef LA64M68_NET_H
#define LA64M68_NET_H

#include <stdint.h>
#include <stddef.h>

/* Host network backend for vNIC. Modes: off/bridge/masq (TAP based).
 * null until the Linux TAP backend lands; BSD uses the same interface
 * with tap/bridge/pf equivalents later. */

typedef struct la64m68_net la64m68_net;

la64m68_net *la64m68_net_create(const char *mode /* off|bridge|masq */,
                                const char *ifname /* NULL = la68tap0 */);
int          la64m68_net_send(la64m68_net *n, const void *frame, size_t len);
/* host->guest; returns frame len, 0 = none pending (nonblocking) */
int          la64m68_net_recv(la64m68_net *n, void *frame, size_t cap);
void         la64m68_net_destroy(la64m68_net *n);
const char  *la64m68_net_mode(const la64m68_net *n);

#endif
