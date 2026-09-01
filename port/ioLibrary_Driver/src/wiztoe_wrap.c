/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Linker --wrap glue: routes lwIP's BSD socket entry points to the WIZnet TOE
 * hardware-socket backend (wiznet_toe.c). Built only when
 * CONFIG_WSM_DRIVER_SOCKET_WRAP is enabled.
 *
 * ESP-IDF exposes socket()/recv()/... as static-inline wrappers around
 * lwip_socket()/lwip_recv()/... (LWIP_COMPAT_SOCKETS=0). We intercept those
 * symbols with `-Wl,--wrap=lwip_*`, so the loopback source is unchanged. close()
 * on a socket fd routes through the VFS to lwip_close (vfs_lwip.c), which is
 * likewise redirected here — so close() re-arms the TOE listener as expected.
 *
 * fd mapping: wiztoe fds are 0..N-1; we add LWIP_SOCKET_OFFSET so they land in
 * the VFS-routed range [LWIP_SOCKET_OFFSET, MAX_FDS) and close()/read()/write()
 * dispatch correctly. As in the Pico design, TOE owns ALL sockets in this
 * build, so every wrap routes unconditionally to wiztoe_* (no __real fallback).
 *
 * Includes lwIP headers but NOT ioLibrary — no socket()/close() name clash.
 */
#include <string.h>
#include <errno.h>
#include <fcntl.h>            /* F_GETFL, F_SETFL, O_NONBLOCK */
#include <sys/time.h>
#include <sys/uio.h>          /* struct iovec */

#include "lwip/sockets.h"     /* LWIP_SOCKET_OFFSET, struct sockaddr_in, lwip_htons/htonl */

#include "wiznet_toe.h"

/* Both "would have blocked" flavours map to the same POSIX condition. */
static int toe_is_wouldblock(int rc)
{
    return rc == WIZTOE_ERR_TIMEOUT || rc == WIZTOE_ERR_WOULDBLOCK;
}

static void toe_fill_sockaddr(struct sockaddr *addr, socklen_t *addrlen,
                              const uint8_t ip[4], uint16_t port)
{
    if (addr == NULL || addrlen == NULL || *addrlen < (socklen_t)sizeof(struct sockaddr_in))
        return;
    struct sockaddr_in *sin = (struct sockaddr_in *)(void *)addr;
    memset(sin, 0, sizeof(*sin));
    sin->sin_family = AF_INET;
    sin->sin_port = lwip_htons(port);
    sin->sin_addr.s_addr = lwip_htonl(((uint32_t)ip[0] << 24) | ((uint32_t)ip[1] << 16) |
                                      ((uint32_t)ip[2] << 8) | ip[3]);
    *addrlen = sizeof(struct sockaddr_in);
}

static void toe_ip_from_sockaddr(const struct sockaddr *name, uint8_t ip[4], uint16_t *port)
{
    const struct sockaddr_in *sin = (const struct sockaddr_in *)(const void *)name;
    uint32_t a = lwip_ntohl(sin->sin_addr.s_addr);
    ip[0] = (uint8_t)(a >> 24); ip[1] = (uint8_t)(a >> 16);
    ip[2] = (uint8_t)(a >> 8);  ip[3] = (uint8_t)a;
    *port = lwip_ntohs(sin->sin_port);
}

int __wrap_lwip_socket(int domain, int type, int protocol)
{
    int fd = wiztoe_socket(domain, type, protocol);
    if (fd < 0) { errno = ENFILE; return -1; }
    errno = 0;
    return fd + LWIP_SOCKET_OFFSET;
}

int __wrap_lwip_bind(int s, const struct sockaddr *name, socklen_t namelen)
{
    (void)namelen;
    const struct sockaddr_in *sin = (const struct sockaddr_in *)(const void *)name;
    if (wiztoe_bind(s - LWIP_SOCKET_OFFSET, lwip_ntohs(sin->sin_port)) < 0) {
        errno = EADDRINUSE; return -1;
    }
    errno = 0;
    return 0;
}

int __wrap_lwip_listen(int s, int backlog)
{
    if (wiztoe_listen(s - LWIP_SOCKET_OFFSET, backlog) < 0) { errno = EOPNOTSUPP; return -1; }
    errno = 0;
    return 0;
}

int __wrap_lwip_accept(int s, struct sockaddr *addr, socklen_t *addrlen)
{
    int fd = wiztoe_accept(s - LWIP_SOCKET_OFFSET);
    if (toe_is_wouldblock(fd)) { errno = EWOULDBLOCK; return -1; }
    if (fd < 0) { errno = EINVAL; return -1; }
    uint8_t ip[4]; uint16_t port;
    wiztoe_peer(fd, ip, &port);
    toe_fill_sockaddr(addr, addrlen, ip, port);
    errno = 0;
    return fd + LWIP_SOCKET_OFFSET;
}

int __wrap_lwip_connect(int s, const struct sockaddr *name, socklen_t namelen)
{
    (void)namelen;
    uint8_t ip[4]; uint16_t port;
    toe_ip_from_sockaddr(name, ip, &port);
    if (wiztoe_connect(s - LWIP_SOCKET_OFFSET, ip, port) < 0) { errno = ECONNREFUSED; return -1; }
    errno = 0;
    return 0;
}

ssize_t __wrap_lwip_send(int s, const void *data, size_t size, int flags)
{
    (void)flags;
    int n = wiztoe_send(s - LWIP_SOCKET_OFFSET, data, size);
    if (toe_is_wouldblock(n)) { errno = EWOULDBLOCK; return -1; }
    if (n < 0) { errno = EIO; return -1; }
    errno = 0;
    return n;
}

ssize_t __wrap_lwip_recv(int s, void *mem, size_t len, int flags)
{
    (void)flags;
    int toe_fd = s - LWIP_SOCKET_OFFSET;
    int n = wiztoe_recv(toe_fd, mem, len);
    if (toe_is_wouldblock(n)) { errno = EWOULDBLOCK; return -1; }
    if (n < 0) { errno = EIO; return -1; }
    errno = 0;
    return n;
}

ssize_t __wrap_lwip_recvfrom(int s, void *mem, size_t len, int flags,
                             struct sockaddr *from, socklen_t *fromlen)
{
    (void)flags;
    int toe_fd = s - LWIP_SOCKET_OFFSET;
    uint8_t ip[4]; uint16_t port = 0;
    int n;
    if (wiztoe_is_udp(toe_fd)) {
        n = wiztoe_recvfrom(toe_fd, mem, len, ip, &port);
    } else {
        n = wiztoe_recv(toe_fd, mem, len);
        wiztoe_peer(toe_fd, ip, &port);
    }
    if (toe_is_wouldblock(n)) { errno = EWOULDBLOCK; return -1; }
    if (n < 0) { errno = EIO; return -1; }
    toe_fill_sockaddr(from, fromlen, ip, port);
    errno = 0;
    return n;
}

ssize_t __wrap_lwip_sendto(int s, const void *data, size_t size, int flags,
                           const struct sockaddr *to, socklen_t tolen)
{
    (void)flags; (void)tolen;
    int toe_fd = s - LWIP_SOCKET_OFFSET;
    int n;
    if (to == NULL || !wiztoe_is_udp(toe_fd)) {
        n = wiztoe_send(toe_fd, data, size);
    } else {
        uint8_t ip[4]; uint16_t port;
        toe_ip_from_sockaddr(to, ip, &port);
        n = wiztoe_sendto(toe_fd, data, size, ip, port);
    }
    if (toe_is_wouldblock(n)) { errno = EWOULDBLOCK; return -1; }
    if (n < 0) { errno = EIO; return -1; }
    errno = 0;
    return n;
}

int __wrap_lwip_close(int s)
{
    if (wiztoe_close(s - LWIP_SOCKET_OFFSET) < 0) { errno = EBADF; return -1; }
    errno = 0;
    return 0;
}

int __wrap_lwip_getsockname(int s, struct sockaddr *name, socklen_t *namelen)
{
    uint8_t ip[4]; uint16_t port;
    wiztoe_getsockname(s - LWIP_SOCKET_OFFSET, ip, &port);
    toe_fill_sockaddr(name, namelen, ip, port);
    errno = 0;
    return 0;
}

int __wrap_lwip_setsockopt(int s, int level, int optname, const void *optval, socklen_t optlen)
{
    int toe_fd = s - LWIP_SOCKET_OFFSET;
    if (optval == NULL) { errno = EFAULT; return -1; }

    if (level == SOL_SOCKET) {
        switch (optname) {
        case SO_REUSEADDR:
        case SO_BROADCAST:
            errno = 0; return 0;                     /* harmless no-op */
        case SO_BINDTODEVICE:
            /* Pinning a socket to a netif is meaningless here: the chip IS the
             * interface, so every TOE socket is already bound to it. Accepting
             * the option lets portable code (e.g. the DHCP client in
             * examples/dhcp_dns) set it unconditionally on both backends. */
            errno = 0; return 0;
        case SO_KEEPALIVE:
            if (wiztoe_setsockopt(toe_fd, WIZTOE_OPT_KEEPALIVE, optval, optlen) < 0) {
                errno = EINVAL; return -1;
            }
            errno = 0; return 0;
        case SO_RCVTIMEO:
        case SO_SNDTIMEO: {
            const struct timeval *tv = (const struct timeval *)optval;
            uint32_t ms;
            wiztoe_opt_t o = (optname == SO_RCVTIMEO) ? WIZTOE_OPT_RCVTIMEO_MS
                                                      : WIZTOE_OPT_SNDTIMEO_MS;
            if (optlen < (socklen_t)sizeof(struct timeval)) { errno = EINVAL; return -1; }
            ms = (uint32_t)((tv->tv_sec * 1000) + (tv->tv_usec / 1000));
            if (wiztoe_setsockopt(toe_fd, o, &ms, sizeof(ms)) < 0) { errno = EINVAL; return -1; }
            errno = 0; return 0;
        }
        default: break;
        }
    } else if (level == IPPROTO_TCP) {
        wiztoe_opt_t o;
        if (optname == TCP_NODELAY)       o = WIZTOE_OPT_NODELAY;
        else if (optname == TCP_KEEPIDLE) o = WIZTOE_OPT_KEEPIDLE;
        else { errno = ENOPROTOOPT; return -1; }
        if (wiztoe_setsockopt(toe_fd, o, optval, optlen) < 0) { errno = EINVAL; return -1; }
        errno = 0; return 0;
    } else if (level == IPPROTO_IP) {
        wiztoe_opt_t o;
        if (optname == IP_ADD_MEMBERSHIP || optname == IP_DROP_MEMBERSHIP) {
            /* Not mapped on purpose. The chip filters the group in hardware and
             * latches the group's MAC when the socket opens, so a join arriving
             * after bind() can only be applied by closing and reopening the
             * hardware socket -- which is a decision about the application's
             * traffic, not something a setsockopt() should do behind its back.
             * examples/udp_multicast performs the reopen itself; see the join
             * seam there. Reporting it unsupported keeps a caller that expects
             * ordinary LwIP IGMP from believing it got it. */
            errno = ENOPROTOOPT;
            return -1;
        }
        if (optname == IP_MULTICAST_TTL || optname == IP_MULTICAST_IF ||
            optname == IP_MULTICAST_LOOP) {
            /* The chip has no equivalent knob; accepting these keeps portable
             * multicast code from failing on an option it sets defensively. */
            errno = 0; return 0;
        }
        if (optname == IP_TTL)      o = WIZTOE_OPT_TTL;
        else if (optname == IP_TOS) o = WIZTOE_OPT_TOS;
        else { errno = ENOPROTOOPT; return -1; }
        if (wiztoe_setsockopt(toe_fd, o, optval, optlen) < 0) { errno = EINVAL; return -1; }
        errno = 0; return 0;
    }
    errno = ENOPROTOOPT;
    return -1;
}

int __wrap_lwip_getsockopt(int s, int level, int optname, void *optval, socklen_t *optlen)
{
    int toe_fd = s - LWIP_SOCKET_OFFSET;
    if (optval == NULL || optlen == NULL) { errno = EFAULT; return -1; }

    if (level == SOL_SOCKET) {
        switch (optname) {
        case SO_ERROR:
        case SO_TYPE:
        case SO_RCVBUF:
        case SO_SNDBUF: {
            wiztoe_opt_t o = (optname == SO_ERROR)  ? WIZTOE_OPT_ERROR
                           : (optname == SO_TYPE)   ? WIZTOE_OPT_TYPE
                           : (optname == SO_RCVBUF) ? WIZTOE_OPT_RCVBUF
                                                    : WIZTOE_OPT_SNDBUF;
            size_t sz = (size_t)*optlen;
            if (*optlen < (socklen_t)sizeof(int)) { errno = EINVAL; return -1; }
            if (wiztoe_getsockopt(toe_fd, o, optval, &sz) < 0) { errno = EINVAL; return -1; }
            *optlen = (socklen_t)sz;
            errno = 0; return 0;
        }
        case SO_RCVTIMEO:
        case SO_SNDTIMEO: {
            uint32_t ms = 0; size_t sz = sizeof(ms);
            struct timeval *tv;
            wiztoe_opt_t o = (optname == SO_RCVTIMEO) ? WIZTOE_OPT_RCVTIMEO_MS
                                                      : WIZTOE_OPT_SNDTIMEO_MS;
            if (*optlen < (socklen_t)sizeof(struct timeval)) { errno = EINVAL; return -1; }
            if (wiztoe_getsockopt(toe_fd, o, &ms, &sz) < 0) { errno = EINVAL; return -1; }
            tv = (struct timeval *)optval;
            tv->tv_sec = (long)(ms / 1000);
            tv->tv_usec = (long)((ms % 1000) * 1000);
            *optlen = sizeof(struct timeval);
            errno = 0; return 0;
        }
        default: break;
        }
    } else if (level == IPPROTO_IP) {
        if (optname == IP_TTL || optname == IP_TOS) {
            wiztoe_opt_t o = (optname == IP_TTL) ? WIZTOE_OPT_TTL : WIZTOE_OPT_TOS;
            size_t sz = (size_t)*optlen;
            if (*optlen < (socklen_t)sizeof(int)) { errno = EINVAL; return -1; }
            if (wiztoe_getsockopt(toe_fd, o, optval, &sz) < 0) { errno = EINVAL; return -1; }
            *optlen = (socklen_t)sz;
            errno = 0; return 0;
        }
    }
    errno = ENOPROTOOPT;
    return -1;
}

/* ---------------------------------------------------------------------------
 * read / write / readv / writev / fcntl / shutdown / getpeername
 *
 * These reach a TOE fd through two routes, and both end at lwip_*, which is why
 * wrapping the lwip_* symbol is enough for either:
 *   - the VFS (newlib read()/write()/fcntl() -> vfs_lwip.c -> lwip_read/...)
 *   - a direct ::lwip_read()/::lwip_write() call, which some socket layers make
 *     to skip the VFS indirection.
 * Without these wraps the calls reach the real lwIP, which has no socket for a
 * TOE fd and fails with EBADF -- a failure that only shows up at run time.
 * ------------------------------------------------------------------------- */

ssize_t __wrap_lwip_read(int s, void *mem, size_t len)
{
    /* POSIX: read(fd, buf, n) on a socket == recv(fd, buf, n, 0). */
    return __wrap_lwip_recv(s, mem, len, 0);
}

ssize_t __wrap_lwip_write(int s, const void *data, size_t size)
{
    /* POSIX: write(fd, buf, n) on a socket == send(fd, buf, n, 0). */
    return __wrap_lwip_send(s, data, size, 0);
}

ssize_t __wrap_lwip_readv(int s, const struct iovec *iov, int iovcnt)
{
    if (iov == NULL || iovcnt <= 0) { errno = EINVAL; return -1; }

    int toe_fd = s - LWIP_SOCKET_OFFSET;
    ssize_t total = 0;

    for (int i = 0; i < iovcnt; i++) {
        if (iov[i].iov_len == 0)
            continue;

        /* Only the first buffer may wait (and only if the socket is blocking).
         * Later buffers are filled solely from data already in the chip's RX
         * buffer, so a scatter read can never block after it has data in hand.
         * Returning less than the total is a short read, which POSIX allows on
         * a stream socket. */
        if (total > 0 && wiztoe_available(toe_fd) <= 0)
            break;

        int n = wiztoe_recv(toe_fd, iov[i].iov_base, iov[i].iov_len);
        if (toe_is_wouldblock(n)) {
            if (total > 0)
                break;                       /* report what we already read */
            errno = EWOULDBLOCK;
            return -1;
        }
        if (n < 0) {
            if (total > 0)
                break;
            errno = EIO;
            return -1;
        }
        total += n;
        if ((size_t)n < iov[i].iov_len)
            break;                           /* short fill: nothing more waiting */
    }

    errno = 0;
    return total;
}

ssize_t __wrap_lwip_writev(int s, const struct iovec *iov, int iovcnt)
{
    if (iov == NULL || iovcnt <= 0) { errno = EINVAL; return -1; }

    int toe_fd = s - LWIP_SOCKET_OFFSET;
    ssize_t total = 0;

    for (int i = 0; i < iovcnt; i++) {
        if (iov[i].iov_len == 0)
            continue;

        int n = wiztoe_send(toe_fd, iov[i].iov_base, iov[i].iov_len);
        if (toe_is_wouldblock(n)) {
            if (total > 0)
                break;                       /* partial gather write */
            errno = EWOULDBLOCK;
            return -1;
        }
        if (n < 0) {
            if (total > 0)
                break;
            errno = EIO;
            return -1;
        }
        total += n;
        if ((size_t)n < iov[i].iov_len)
            break;                           /* short write: stop, caller resumes */
    }

    errno = 0;
    return total;
}

int __wrap_lwip_fcntl(int s, int cmd, int val)
{
    int toe_fd = s - LWIP_SOCKET_OFFSET;
    int nb;

    switch (cmd) {
    case F_GETFL:
        nb = wiztoe_get_nonblocking(toe_fd);
        if (nb < 0) { errno = EBADF; return -1; }
        errno = 0;
        return nb ? O_NONBLOCK : 0;

    case F_SETFL:
        /* O_NONBLOCK is the only flag the chip can honour. Anything else is
         * ignored rather than rejected, matching lwIP's own lwip_fcntl. */
        if (wiztoe_set_nonblocking(toe_fd, (val & O_NONBLOCK) != 0) < 0) {
            errno = EBADF;
            return -1;
        }
        errno = 0;
        return 0;

    default:
        /* Not silently succeeding: a caller asking for F_DUPFD or file locks
         * must find out that it did not happen. */
        errno = EINVAL;
        return -1;
    }
}

int __wrap_lwip_shutdown(int s, int how)
{
    int shut_rd = (how == SHUT_RD) || (how == SHUT_RDWR);
    int shut_wr = (how == SHUT_WR) || (how == SHUT_RDWR);

    if (!shut_rd && !shut_wr) { errno = EINVAL; return -1; }

    if (wiztoe_shutdown(s - LWIP_SOCKET_OFFSET, shut_rd, shut_wr) < 0) {
        errno = ENOTCONN;
        return -1;
    }
    errno = 0;
    return 0;
}

int __wrap_lwip_getpeername(int s, struct sockaddr *name, socklen_t *namelen)
{
    /* Reuses the existing wiztoe_peer(); no new peer bookkeeping is introduced. */
    uint8_t ip[4]; uint16_t port;
    wiztoe_peer(s - LWIP_SOCKET_OFFSET, ip, &port);
    toe_fill_sockaddr(name, namelen, ip, port);
    errno = 0;
    return 0;
}
