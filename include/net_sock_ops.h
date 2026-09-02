/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * Socket-op vtables for running a server on BOTH interfaces concurrently.
 *
 * An app engine (echo, TCP server, ...) calls BSD sockets through a vtable so the
 * exact same logic can drive either stack. `port` provides two ready vtables so
 * examples don't have to know about the W5500 --wrap:
 *   - net_eth_ops : plain lwip_*. With SOCKET_WRAP=1 these are redirected to the
 *                   W5500 hardware sockets by -Wl,--wrap (ioLibrary_Driver/wiztoe_wrap.c);
 *                   with SOCKET_WRAP=0 they are the software LwIP over esp_eth.
 *                   Needs no #if either way — the linker picks the right target.
 *   - net_wifi_ops: reaches the REAL software LwIP the Wi-Fi netif is on. With
 *                   SOCKET_WRAP=1 it binds to the linker's __real_lwip_* (the
 *                   un-wrapped originals) to bypass the W5500; with SOCKET_WRAP=0
 *                   it is the same plain lwip_* as Ethernet (one shared stack).
 * The wrap awareness lives entirely here in `port` (which owns the --wrap), so
 * app code stays free of any #if WSM_DRIVER_SOCKET_WRAP.
 *
 * This is public component API: examples take a vtable by address and hand it to
 * their engine, exactly as examples/loopback does. (loopback still declares a
 * private copy of this type predating the move; new examples should use this one.)
 */
#ifndef NET_SOCK_OPS_H
#define NET_SOCK_OPS_H

#include <stdint.h>
#include <stddef.h>

#include "lwip/sockets.h"   /* struct sockaddr, socklen_t, ssize_t */

/* Socket ops injected into an app engine (signatures match lwip_*). */
typedef struct {
    int     (*socket)(int domain, int type, int protocol);
    int     (*bind)(int s, const struct sockaddr *name, socklen_t namelen);
    int     (*listen)(int s, int backlog);
    int     (*accept)(int s, struct sockaddr *addr, socklen_t *addrlen);
    int     (*connect)(int s, const struct sockaddr *name, socklen_t namelen);
    ssize_t (*recv)(int s, void *mem, size_t len, int flags);
    ssize_t (*send)(int s, const void *data, size_t size, int flags);
    ssize_t (*recvfrom)(int s, void *mem, size_t len, int flags,
                        struct sockaddr *from, socklen_t *fromlen);
    ssize_t (*sendto)(int s, const void *data, size_t size, int flags,
                      const struct sockaddr *to, socklen_t tolen);
    int     (*setsockopt)(int s, int level, int optname, const void *optval, socklen_t optlen);
    int     (*close)(int s);
    /* Added for wsm_dhcp.h: a polled caller needs O_NONBLOCK, which no
     * setsockopt expresses portably. Appended, so existing designated
     * initialisers still compile -- callers must null-check it. */
    int     (*fcntl)(int s, int cmd, int val);
} net_sock_ops_t;

/* Ethernet (W5500) socket ops — see file header. */
extern const net_sock_ops_t net_eth_ops;
/* Wi-Fi (real software LwIP) socket ops — see file header. */
extern const net_sock_ops_t net_wifi_ops;

#endif /* NET_SOCK_OPS_H */
