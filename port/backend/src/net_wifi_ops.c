/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * Wi-Fi socket vtable — the ONE place aware of the W5500 --wrap.
 *
 * In WSM_DRIVER_SOCKET_WRAP=1 the plain lwip_* symbols are redirected to the
 * W5500 hardware sockets (see ioLibrary_Driver/wiztoe_wrap.c), so Wi-Fi must bind
 * to the linker's __real_lwip_* (the un-wrapped originals) to reach the REAL
 * software LwIP stack that the Wi-Fi netif is attached to. The W5500 TOE sockets
 * and these real-LwIP fds never cross: each call site fixes which namespace an fd
 * belongs to.
 *
 * In WSM_DRIVER_SOCKET_WRAP=0 there is no --wrap; Wi-Fi and Ethernet share one LwIP stack,
 * so the vtable is just the plain lwip_* (identical to net_eth_ops). Servers on
 * the two interfaces use different ports (example config) to avoid a bind clash
 * on that shared stack.
 *
 * This lives in `port` because the --wrap is declared here; keeping the wrap
 * bypass next to it lets every example stay free of #if WSM_DRIVER_SOCKET_WRAP.
 * The macro is defined PUBLIC by the component CMakeLists from
 * CONFIG_WSM_DRIVER_SOCKET_WRAP, so consumers inherit it via REQUIRES.
 */
#include "lwip/sockets.h"

#include "net_sock_ops.h"

#if defined(WSM_DRIVER_SOCKET_WRAP) && (WSM_DRIVER_SOCKET_WRAP)
/* Un-wrapped LwIP entry points provided by the linker because these symbols are
 * listed in --wrap (see the component CMakeLists). Calling __real_* bypasses the
 * W5500 redirect and hits the software LwIP stack. */
extern int     __real_lwip_socket(int domain, int type, int protocol);
extern int     __real_lwip_bind(int s, const struct sockaddr *name, socklen_t namelen);
extern int     __real_lwip_listen(int s, int backlog);
extern int     __real_lwip_accept(int s, struct sockaddr *addr, socklen_t *addrlen);
extern int     __real_lwip_connect(int s, const struct sockaddr *name, socklen_t namelen);
extern ssize_t __real_lwip_recv(int s, void *mem, size_t len, int flags);
extern ssize_t __real_lwip_send(int s, const void *data, size_t size, int flags);
extern ssize_t __real_lwip_recvfrom(int s, void *mem, size_t len, int flags,
                                    struct sockaddr *from, socklen_t *fromlen);
extern ssize_t __real_lwip_sendto(int s, const void *data, size_t size, int flags,
                                  const struct sockaddr *to, socklen_t tolen);
extern int     __real_lwip_setsockopt(int s, int level, int optname, const void *optval, socklen_t optlen);
extern int     __real_lwip_close(int s);
extern int     __real_lwip_fcntl(int s, int cmd, int val);

const net_sock_ops_t net_wifi_ops = {
    .socket = __real_lwip_socket,   .bind = __real_lwip_bind,
    .listen = __real_lwip_listen,   .accept = __real_lwip_accept,
    .connect = __real_lwip_connect, .recv = __real_lwip_recv,
    .send = __real_lwip_send,       .recvfrom = __real_lwip_recvfrom,
    .sendto = __real_lwip_sendto,   .setsockopt = __real_lwip_setsockopt,
    .close = __real_lwip_close,
    .fcntl = __real_lwip_fcntl,
};
#else /* SOCKET_WRAP=0: no --wrap; plain lwIP (shared stack with Ethernet). */
const net_sock_ops_t net_wifi_ops = {
    .socket = lwip_socket,   .bind = lwip_bind,
    .listen = lwip_listen,   .accept = lwip_accept,
    .connect = lwip_connect, .recv = lwip_recv,
    .send = lwip_send,       .recvfrom = lwip_recvfrom,
    .sendto = lwip_sendto,   .setsockopt = lwip_setsockopt,
    .close = lwip_close,
    .fcntl = lwip_fcntl,
};
#endif
