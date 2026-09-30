#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "net.h"
#include "../core/debug.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef __has_include
# if __has_include(<linux/if_tun.h>)
#  define LA64M68_HAVE_TAP 1
# endif
#endif

#ifdef LA64M68_HAVE_TAP
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <linux/if_tun.h>
#endif

/* TAP backend (Linux): /dev/net/tun with IFF_TAP|IFF_NO_PI.
 * mode bridge/masq both use the same TAP fd; bridge-vs-NAT is pure
 * host setup (bridge-utils or nftables), not our code. BSD: /dev/tap. */

struct la64m68_net {
    char mode[8];
    int  fd;
};

la64m68_net *la64m68_net_create(const char *mode, const char *ifname)
{
    la64m68_net *n = calloc(1, sizeof(*n));
    if (!n) return NULL;
    snprintf(n->mode, sizeof(n->mode), "%s", mode ? mode : "off");
    n->fd = -1;

    if (!strcmp(n->mode, "off")) {
        la64m68_trace("net: off");
        return n;
    }

#ifdef LA64M68_HAVE_TAP
    int fd = open("/dev/net/tun", O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        la64m68_trace("net: /dev/net/tun unavailable: %s", strerror(errno));
        snprintf(n->mode, sizeof(n->mode), "off");
        return n;
    }
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    ifr.ifr_flags = IFF_TAP | IFF_NO_PI;
    if (ifname && *ifname)
        snprintf(ifr.ifr_name, IFNAMSIZ, "%s", ifname);
    else
        snprintf(ifr.ifr_name, IFNAMSIZ, "la68tap%d", 0);
    if (ioctl(fd, TUNSETIFF, &ifr) < 0) {
        la64m68_trace("net: TUNSETIFF failed: %s", strerror(errno));
        close(fd);
        snprintf(n->mode, sizeof(n->mode), "off");
        return n;
    }
    n->fd = fd;
    la64m68_trace("net: tap %s up (mode=%s) — host must "
                  "bridge/nft-masq it", ifr.ifr_name, n->mode);
#else
    la64m68_trace("net: mode=%s -> off (no tap on this platform)",
                  n->mode);
    snprintf(n->mode, sizeof(n->mode), "off");
#endif
    return n;
}

int la64m68_net_send(la64m68_net *n, const void *frame, size_t len)
{
#ifdef LA64M68_HAVE_TAP
    if (n && n->fd >= 0) {
        ssize_t w = write(n->fd, frame, len);
        return w < 0 ? 0 : (int)w;
    }
#endif
    (void)n; (void)frame;
    return (int)len;            /* null backend: frames "sent" */
}

/* host -> guest: returns frame length, 0 = nothing pending */
int la64m68_net_recv(la64m68_net *n, void *frame, size_t cap)
{
#ifdef LA64M68_HAVE_TAP
    if (n && n->fd >= 0) {
        ssize_t r = read(n->fd, frame, cap);
        return r > 0 ? (int)r : 0;
    }
#endif
    (void)n; (void)frame; (void)cap;
    return 0;
}

void la64m68_net_destroy(la64m68_net *n)
{
    if (!n) return;
#ifdef LA64M68_HAVE_TAP
    if (n->fd >= 0) close(n->fd);
#endif
    free(n);
}

const char *la64m68_net_mode(const la64m68_net *n)
{
    return n ? n->mode : "off";
}
