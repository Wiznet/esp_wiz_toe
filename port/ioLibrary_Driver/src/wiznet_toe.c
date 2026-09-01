/*
 * Copyright (c) 2024 WIZnet Co.,Ltd
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * WIZnet TOE backend implementation (see wiznet_toe.h). Ported from
 * WIZnet-PICO-LWIP-TOE-C (port/lwip/wiznet_toe.c) to ESP-IDF:
 *   - sleep_ms(1)   -> toe_yield_1ms()   (FreeRTOS vTaskDelay, see toe_port.h)
 *   - time_us_32()  -> toe_time_us()     (esp_timer)
 *
 * This is the ONLY TU (besides ioLibrary itself) that talks to the ioLibrary
 * driver, whose socket()/listen()/connect()/send()/recv()/close() names clash
 * with POSIX/newlib. To avoid a duplicate/override of the POSIX `close` symbol
 * at link, this TU and the ioLibrary sources are compiled with the identifiers
 * renamed (-Dsocket=wiz_socket -Dclose=wiz_close ... in CMake); the source
 * below still reads with the ioLibrary names. It includes NO FreeRTOS/POSIX
 * headers, only <string.h> + ioLibrary + toe_port.h.
 */
#include <string.h>

#include "wizchip_conf.h"
#include "socket.h"            /* ioLibrary socket API (hardware sockets) */

#include "wiznet_toe.h"
#include "toe_port.h"          /* toe_yield_1ms(), toe_time_us() */

#ifndef WIZTOE_MAX_SOCK
#define WIZTOE_MAX_SOCK _WIZCHIP_SOCK_NUM_   /* 8 on W5500 */
#endif

typedef struct {
    uint8_t used;
    uint8_t is_udp;
    uint8_t opened;
    uint8_t listening;
    uint8_t accepted;
    uint8_t nodelay;
    uint8_t nonblocking;   /* POSIX O_NONBLOCK */
    uint8_t rd_shutdown;   /* SHUT_RD: report EOF instead of buffered data */
    uint16_t port;
    uint32_t rcv_timeout_ms;
    uint32_t snd_timeout_ms;
    uint8_t  dst_ip[4];
    uint16_t dst_port;
    uint8_t  connected;
} toe_sock_t;

static toe_sock_t g_toe[WIZTOE_MAX_SOCK];

static void toe_tcp_disconnect_if_connected(int fd);

static int toe_fd_valid(int fd)
{
    return (fd >= 0) && (fd < WIZTOE_MAX_SOCK) && g_toe[fd].used;
}

static uint8_t toe_open_flag(int fd)
{
    return g_toe[fd].nodelay ? SF_TCP_NODELAY : 0;
}

/* Open the hardware socket for a UDP fd.
 *
 * A single place for the UDP open so callers that need the socket reopened --
 * examples/udp_multicast reopens it with Sn_MR_MULTI to join a group -- can see
 * exactly what the plain open does. */
static int toe_open_udp(int fd)
{
    if (socket((uint8_t)fd, Sn_MR_UDP, g_toe[fd].port, 0) != fd)
        return -1;

    g_toe[fd].opened = 1;
    return 0;
}

void wiztoe_network_init(const uint8_t ip[4], const uint8_t mask[4],
                         const uint8_t gw[4], const uint8_t mac[6])
{
    wiz_NetInfo ni;
    memset(&ni, 0, sizeof(ni));
    memcpy(ni.mac, mac, 6);
    memcpy(ni.ip, ip, 4);
    memcpy(ni.sn, mask, 4);
    memcpy(ni.gw, gw, 4);
    ni.dhcp = NETINFO_STATIC;
#if (_WIZCHIP_ > W5500)
    {
        uint8_t syslock = SYS_NET_LOCK;
        ctlwizchip(CW_SYS_UNLOCK, &syslock);
    }
#endif
    ctlnetwork(CN_SET_NETINFO, (void *)&ni);
}

int wiztoe_socket(int domain, int type, int protocol)
{
    (void)domain;
    (void)protocol;

    if (type != 1 /* SOCK_STREAM */ && type != 2 /* SOCK_DGRAM */)
        return -1;

    for (int sn = 0; sn < WIZTOE_MAX_SOCK; sn++)
    {
        if (!g_toe[sn].used)
        {
            memset(&g_toe[sn], 0, sizeof(g_toe[sn]));
            g_toe[sn].used = 1;
            g_toe[sn].is_udp = (type == 2);
            return sn;                        /* fd == sn */
        }
    }
    return -1;
}

int wiztoe_is_udp(int fd)
{
    return toe_fd_valid(fd) && g_toe[fd].is_udp;
}

int wiztoe_set_nonblocking(int fd, int enable)
{
    if (!toe_fd_valid(fd))
        return -1;

    g_toe[fd].nonblocking = enable ? 1 : 0;
    return 0;
}

int wiztoe_get_nonblocking(int fd)
{
    if (!toe_fd_valid(fd))
        return -1;

    return g_toe[fd].nonblocking ? 1 : 0;
}

int wiztoe_available(int fd)
{
    if (!toe_fd_valid(fd))
        return -1;
    if (!g_toe[fd].opened)
        return 0;

    return (int)getSn_RX_RSR((uint8_t)fd);
}

int wiztoe_shutdown(int fd, int shut_rd, int shut_wr)
{
    if (!toe_fd_valid(fd))
        return -1;

    if (shut_rd)
    {
        /* The chip keeps filling its RX buffer regardless; all we can honour is
         * the promise that this side stops delivering data. */
        g_toe[fd].rd_shutdown = 1;
    }

    if (shut_wr && !g_toe[fd].is_udp)
    {
        /* Send FIN so the peer observes EOF. The fd stays allocated -- freeing
         * it is close()'s job, and a half-closed socket must still be readable. */
        toe_tcp_disconnect_if_connected(fd);
    }

    return 0;
}

int wiztoe_bind(int fd, uint16_t port)
{
    if (!toe_fd_valid(fd))
        return -1;

    g_toe[fd].port = port;

    if (g_toe[fd].is_udp)
    {
        if (toe_open_udp(fd) < 0)
            return -1;
    }
    return 0;
}

int wiztoe_listen(int fd, int backlog)
{
    (void)backlog;

    if (!toe_fd_valid(fd) || g_toe[fd].is_udp)
        return -1;

    if (socket((uint8_t)fd, Sn_MR_TCP, g_toe[fd].port, toe_open_flag(fd)) != fd)
        return -1;
    g_toe[fd].opened = 1;

    if (listen((uint8_t)fd) != SOCK_OK)
        return -1;

    g_toe[fd].listening = 1;
    return 0;
}

int wiztoe_accept(int fd)
{
    if (!toe_fd_valid(fd) || !g_toe[fd].listening)
        return -1;

    uint32_t waited = 0;
    for (;;)
    {
        uint8_t sr = getSn_SR((uint8_t)fd);

        if (sr == SOCK_ESTABLISHED)
        {
            g_toe[fd].accepted = 1;
            return fd;
        }
        if (sr == SOCK_CLOSED)
        {
            if (socket((uint8_t)fd, Sn_MR_TCP, g_toe[fd].port, toe_open_flag(fd)) != fd)
                return -1;
            if (listen((uint8_t)fd) != SOCK_OK)
                return -1;
        }
        /* Re-armed above if needed, so the listener is live either way; report
         * "nothing pending" rather than waiting for a client to show up. */
        if (g_toe[fd].nonblocking)
            return WIZTOE_ERR_WOULDBLOCK;
        if (g_toe[fd].rcv_timeout_ms && ++waited >= g_toe[fd].rcv_timeout_ms)
            return WIZTOE_ERR_TIMEOUT;
        toe_yield_1ms();
    }
}

int wiztoe_connect(int fd, const uint8_t ip[4], uint16_t port)
{
    if (!toe_fd_valid(fd))
        return -1;

    if (g_toe[fd].is_udp)
    {
        memcpy(g_toe[fd].dst_ip, ip, 4);
        g_toe[fd].dst_port = port;
        g_toe[fd].connected = 1;
        return 0;
    }

    /* Randomized ephemeral local port to avoid TIME_WAIT 4-tuple reuse after a
     * reset (ioLibrary's static sock_any_port restarts at 0xC000 each boot). */
    uint16_t lport = g_toe[fd].port;
    if (lport == 0)
    {
        lport = (uint16_t)(0xC000u + (toe_time_us() % 0x3FF0u));
        g_toe[fd].port = lport;
    }
    if (socket((uint8_t)fd, Sn_MR_TCP, lport, toe_open_flag(fd)) != fd)
        return -1;
    g_toe[fd].opened = 1;

    return (connect((uint8_t)fd, (uint8_t *)ip, port) == SOCK_OK) ? 0 : -1;
}

int wiztoe_send(int fd, const void *buf, size_t len)
{
    if (!toe_fd_valid(fd) || g_toe[fd].is_udp)
        return -1;
    if (len > 0xFFFF)
        len = 0xFFFF;

    if (g_toe[fd].nonblocking)
    {
        /* ioLibrary's send() spins in `while (len > freesize)` until the chip
         * drains, which is exactly the wait a non-blocking caller forbade.
         * Clamping to the free space keeps it on the `len <= freesize` path,
         * where it returns without waiting, and a short write is what POSIX
         * expects from a non-blocking stream send. */
        uint16_t freesize = getSn_TX_FSR((uint8_t)fd);
        if (freesize == 0)
            return WIZTOE_ERR_WOULDBLOCK;
        if (len > freesize)
            len = freesize;
    }

    int32_t n = send((uint8_t)fd, (uint8_t *)buf, (uint16_t)len);
    return (n < 0) ? -1 : (int)n;
}

int wiztoe_recv(int fd, void *buf, size_t len)
{
    if (!toe_fd_valid(fd) || g_toe[fd].is_udp)
        return -1;
    if (len > 0xFFFF)
        len = 0xFFFF;

    if (g_toe[fd].rd_shutdown)
        return 0;                              /* EOF after shutdown(SHUT_RD) */

    uint32_t waited = 0;
    for (;;)
    {
        if (getSn_RX_RSR((uint8_t)fd) > 0)
            break;
        if (getSn_SR((uint8_t)fd) != SOCK_ESTABLISHED)
            return 0;                          /* EOF */
        /* Checked after the state test so a closed connection still reports EOF
         * rather than EWOULDBLOCK -- a non-blocking reader must be able to see
         * the end of the stream. */
        if (g_toe[fd].nonblocking)
            return WIZTOE_ERR_WOULDBLOCK;
        if (g_toe[fd].rcv_timeout_ms)
        {
            if (++waited >= g_toe[fd].rcv_timeout_ms)
                return WIZTOE_ERR_TIMEOUT;
            toe_yield_1ms();
        }
        else
        {
            /* No SO_RCVTIMEO: still yield (unlike the Pico busy-poll) so the
             * ESP-IDF idle task / watchdog run. 1 ms tick (FREERTOS_HZ=1000). */
            toe_yield_1ms();
        }
    }

    int32_t n = recv((uint8_t)fd, (uint8_t *)buf, (uint16_t)len);
    if (n == SOCKERR_SOCKSTATUS || n == SOCKERR_SOCKCLOSED)
        return 0;                              /* EOF */
    return (n < 0) ? -1 : (int)n;
}

int wiztoe_sendto(int fd, const void *buf, size_t len,
                  const uint8_t ip[4], uint16_t port)
{
    if (!toe_fd_valid(fd) || !g_toe[fd].is_udp)
        return -1;
    if (len > 0xFFFF)
        len = 0xFFFF;

    if (!g_toe[fd].opened)
    {
        if (socket((uint8_t)fd, Sn_MR_UDP, g_toe[fd].port, 0) != fd)
            return -1;
        g_toe[fd].opened = 1;
    }

    int32_t n = sendto((uint8_t)fd, (uint8_t *)buf, (uint16_t)len,
                       (uint8_t *)ip, port);
    return (n < 0) ? -1 : (int)n;
}

int wiztoe_recvfrom(int fd, void *buf, size_t len, uint8_t ip[4], uint16_t *port)
{
    if (!toe_fd_valid(fd) || !g_toe[fd].is_udp || !g_toe[fd].opened)
        return -1;
    if (len > 0xFFFF)
        len = 0xFFFF;

    uint32_t waited = 0;
    for (;;)
    {
        if (getSn_RX_RSR((uint8_t)fd) > 0)
            break;
        if (getSn_SR((uint8_t)fd) != SOCK_UDP)
            return -1;
        if (g_toe[fd].nonblocking)
            return WIZTOE_ERR_WOULDBLOCK;
        if (g_toe[fd].rcv_timeout_ms && ++waited >= g_toe[fd].rcv_timeout_ms)
            return WIZTOE_ERR_TIMEOUT;
        toe_yield_1ms();
    }

    int32_t n = recvfrom((uint8_t)fd, (uint8_t *)buf, (uint16_t)len, ip, port);
    return (n < 0) ? -1 : (int)n;
}

void wiztoe_peer(int fd, uint8_t ip[4], uint16_t *port)
{
    if (!toe_fd_valid(fd))
    {
        memset(ip, 0, 4);
        *port = 0;
        return;
    }
    if (g_toe[fd].is_udp)
    {
        memcpy(ip, g_toe[fd].dst_ip, 4);
        *port = g_toe[fd].dst_port;
        return;
    }
    getSn_DIPR((uint8_t)fd, ip);
    *port = getSn_DPORT((uint8_t)fd);
}

void wiztoe_getsockname(int fd, uint8_t ip[4], uint16_t *port)
{
    wiz_NetInfo ni;
    if (!toe_fd_valid(fd))
    {
        memset(ip, 0, 4);
        *port = 0;
        return;
    }
    ctlnetwork(CN_GET_NETINFO, (void *)&ni);
    memcpy(ip, ni.ip, 4);
    *port = g_toe[fd].port;
}

void wiztoe_local_ip(uint8_t ip[4])
{
    wiz_NetInfo ni;
    ctlnetwork(CN_GET_NETINFO, (void *)&ni);
    memcpy(ip, ni.ip, 4);
}

void wiztoe_local_mac(uint8_t mac[6])
{
    wiz_NetInfo ni;
    ctlnetwork(CN_GET_NETINFO, (void *)&ni);
    memcpy(mac, ni.mac, 6);
}

int wiztoe_socket_reserve(void)
{
    for (int sn = 0; sn < WIZTOE_MAX_SOCK; sn++)
    {
        if (!g_toe[sn].used)
        {
            memset(&g_toe[sn], 0, sizeof(g_toe[sn]));
            g_toe[sn].used = 1;
            return sn;
        }
    }
    return -1;
}

void wiztoe_socket_release(int sn)
{
    if (sn >= 0 && sn < WIZTOE_MAX_SOCK)
    {
        close((uint8_t)sn);
        memset(&g_toe[sn], 0, sizeof(g_toe[sn]));
    }
}

static void toe_tcp_disconnect_if_connected(int fd)
{
    uint8_t sr = getSn_SR((uint8_t)fd);
    if (sr == SOCK_ESTABLISHED || sr == SOCK_CLOSE_WAIT)
        disconnect((uint8_t)fd);
}

int wiztoe_close(int fd)
{
    if (!toe_fd_valid(fd))
        return -1;

    if (g_toe[fd].listening && g_toe[fd].accepted)
    {
        toe_tcp_disconnect_if_connected(fd);
        if (socket((uint8_t)fd, Sn_MR_TCP, g_toe[fd].port, toe_open_flag(fd)) != fd)
            return -1;
        if (listen((uint8_t)fd) != SOCK_OK)
            return -1;
        g_toe[fd].accepted = 0;
        /* The re-armed listener is a fresh connection, so a half-close from the
         * previous one must not carry over -- otherwise every later client would
         * see an immediate EOF. (nonblocking is a property the application set
         * on this fd and deliberately survives.) */
        g_toe[fd].rd_shutdown = 0;
        return 0;
    }

    if (g_toe[fd].opened && !g_toe[fd].is_udp)
        toe_tcp_disconnect_if_connected(fd);
    if (g_toe[fd].opened)
        close((uint8_t)fd);
    memset(&g_toe[fd], 0, sizeof(g_toe[fd]));
    return 0;
}

int wiztoe_setsockopt(int fd, wiztoe_opt_t opt, const void *val, size_t len)
{
    if (!toe_fd_valid(fd) || val == NULL || len == 0)
        return -1;

    int v = (len >= sizeof(int)) ? *(const int *)val : *(const uint8_t *)val;

    switch (opt)
    {
    case WIZTOE_OPT_KEEPALIVE:
        setSn_KPALVTR((uint8_t)fd, v ? 12 : 0);
        return 0;
    case WIZTOE_OPT_KEEPIDLE:
        if (v < 5) v = 5;
        if (v > 5 * 255) v = 5 * 255;
        setSn_KPALVTR((uint8_t)fd, (uint8_t)(v / 5));
        return 0;
    case WIZTOE_OPT_NODELAY:
        g_toe[fd].nodelay = (v != 0);
        return 0;
    case WIZTOE_OPT_TTL:
        setSn_TTL((uint8_t)fd, (uint8_t)v);
        return 0;
    case WIZTOE_OPT_TOS:
        setSn_TOS((uint8_t)fd, (uint8_t)v);
        return 0;
    case WIZTOE_OPT_RCVTIMEO_MS:
        if (len < sizeof(uint32_t)) return -1;
        g_toe[fd].rcv_timeout_ms = *(const uint32_t *)val;
        return 0;
    case WIZTOE_OPT_SNDTIMEO_MS:
        if (len < sizeof(uint32_t)) return -1;
        g_toe[fd].snd_timeout_ms = *(const uint32_t *)val;
        return 0;
    default:
        return -1;
    }
}

int wiztoe_getsockopt(int fd, wiztoe_opt_t opt, void *val, size_t *len)
{
    if (!toe_fd_valid(fd) || val == NULL || len == NULL || *len < sizeof(int))
        return -1;

    int *out = (int *)val;

    switch (opt)
    {
    case WIZTOE_OPT_ERROR:      *out = 0; break;
    case WIZTOE_OPT_TYPE:       *out = g_toe[fd].is_udp ? 2 : 1; break;
    case WIZTOE_OPT_RCVBUF:     *out = (int)getSn_RxMAX((uint8_t)fd); break;
    case WIZTOE_OPT_SNDBUF:     *out = (int)getSn_TxMAX((uint8_t)fd); break;
    case WIZTOE_OPT_TTL:        *out = (int)getSn_TTL((uint8_t)fd); break;
    case WIZTOE_OPT_TOS:        *out = (int)getSn_TOS((uint8_t)fd); break;
    case WIZTOE_OPT_RCVTIMEO_MS: *(uint32_t *)val = g_toe[fd].rcv_timeout_ms; break;
    case WIZTOE_OPT_SNDTIMEO_MS: *(uint32_t *)val = g_toe[fd].snd_timeout_ms; break;
    default: return -1;
    }
    *len = sizeof(int);
    return 0;
}
