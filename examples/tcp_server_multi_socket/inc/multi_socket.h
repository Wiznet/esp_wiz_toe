/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * Backend-neutral multi-socket TCP echo server, shared by the Ethernet (WIZnet
 * chip) and Wi-Fi paths. The socket entry points are injected via a vtable so
 * the exact same server logic drives either stack:
 *   - Ethernet: plain lwIP BSD ops (lwip_socket/...). With WSM_DRIVER_SOCKET_WRAP=1
 *     these are redirected to the WIZnet hardware sockets by -Wl,--wrap
 *     (wiztoe_wrap.c); with =0 they are the software LwIP over esp_eth.
 *   - Wi-Fi: with WSM_DRIVER_SOCKET_WRAP=1 the __real_lwip_* symbols (bypassing
 *     --wrap) so the traffic goes to the REAL software LwIP; with =0 the same
 *     lwip_* as Ethernet (both share one stack). See wifi_multi_socket.c.
 */
#ifndef MULTI_SOCKET_H
#define MULTI_SOCKET_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "lwip/sockets.h"   /* struct sockaddr, socklen_t, ssize_t */

/* Socket ops injected into the server engine (signatures match lwip_*). */
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
} multi_socket_ops_t;

/*
 * Standard lwIP BSD socket vtable, used by the Ethernet server. These plain
 * lwip_* entry points are redirected to the WIZnet hardware sockets by -Wl,--wrap
 * with WSM_DRIVER_SOCKET_WRAP=1, and are software LwIP over esp_eth with =0.
 */
extern const multi_socket_ops_t multi_socket_lwip_ops;

/*
 * Wi-Fi socket vtable (defined in wifi_multi_socket.c). With
 * WSM_DRIVER_SOCKET_WRAP=1 this is the __real_lwip_* set that bypasses the
 * --wrap so Wi-Fi traffic reaches the real software LwIP stack.
 */
extern const multi_socket_ops_t wifi_multi_socket_ops;

/*
 * Spawn a supervisor task that waits until is_up() reports the interface ready,
 * then opens `count` listeners on port_base..port_base+count-1 and gives each
 * one its own echo task. Ethernet and Wi-Fi are started with identical calls.
 *
 * The listeners are created by the supervisor (not by the echo tasks) on
 * purpose: the TOE allocator walks two plain global tables (descriptor -> chip
 * socket, and its inverse) with no lock, so concurrent socket() calls on the
 * same interface could hand out the same one. Creating them one at a time from
 * a single task removes that race at start-up.
 *
 * It does not remove it afterwards, and this example is honest about that: each
 * accept() takes a descriptor for the new connection and relocates its listener
 * onto a free chip socket, so the echo tasks do touch those shared tables. The
 * window is small and an echo server tolerates it, but a production application
 * that accepts from several tasks at once wants the driver to lock them. See
 * count in net_config.h for the socket budget this implies.
 *
 *   name        - short label; also the task-name prefix and log tag ("eth"/"wifi")
 *   ops         - socket vtable for this interface
 *   port_base   - listener i binds port_base + i (one port per socket)
 *   count       - number of listeners
 *   is_up       - predicate the supervisor polls for link/IP readiness
 */
void multi_socket_start(const char *name, const multi_socket_ops_t *ops,
                        uint16_t port_base, int count, bool (*is_up)(void));

#endif /* MULTI_SOCKET_H */
