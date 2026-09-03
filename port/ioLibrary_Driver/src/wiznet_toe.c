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
 *
 * Descriptor / hardware socket separation
 * ---------------------------------------
 * A descriptor (the value callers pass around as an fd) and a W5500 hardware
 * socket number are two different things:
 *
 *     g_desc[fd].sn  ->  hardware socket, or -1 when the descriptor holds none
 *     g_sn_owner[sn] ->  owning descriptor, TOE_SN_FREE, or TOE_SN_RAW
 *
 * They used to be the same integer. That made BSD accept() semantics
 * impossible: the listener's hardware socket IS the one that becomes
 * ESTABLISHED, so with fd == sn there was nowhere to put the listener
 * afterwards and accept() had to hand the listener itself back to the caller.
 * With the indirection the listener is simply relocated -- the connection keeps
 * the hardware socket it was established on, and the listener re-opens on a
 * free one with the same port. The W5500 supports several sockets listening on
 * one port and demultiplexes by 4-tuple; that was measured on real hardware
 * before this was written (docs/test-results.md, STEP 6-pre).
 */
#include <string.h>

#include "wizchip_conf.h"
#include "socket.h"            /* ioLibrary socket API (hardware sockets) */

#include "wiznet_toe.h"
#include "toe_port.h"          /* toe_yield_1ms(), toe_time_us() */

#ifndef WIZTOE_MAX_SOCK
#define WIZTOE_MAX_SOCK _WIZCHIP_SOCK_NUM_   /* 8 on W5500 */
#endif

/* One descriptor per hardware socket is enough: a descriptor is only useful
 * while it owns one (listener + N connections <= WIZTOE_MAX_SOCK), and the fd
 * numbers must stay inside the VFS-routed range the wrap layer maps them into. */
#ifndef WIZTOE_MAX_DESC
#define WIZTOE_MAX_DESC WIZTOE_MAX_SOCK
#endif

/* g_sn_owner values that are not a descriptor index. */
#define TOE_SN_FREE (-1)
#define TOE_SN_RAW  (-2)   /* handed out by wiztoe_socket_reserve(): no descriptor */

typedef struct {
    uint8_t used;
    int8_t  sn;            /* bound hardware socket, -1 = none */
    uint8_t is_udp;
    uint8_t opened;
    uint8_t listening;
    uint8_t nodelay;
    uint8_t nonblocking;   /* POSIX O_NONBLOCK */
    uint8_t rd_shutdown;   /* SHUT_RD: report EOF instead of buffered data */
    uint16_t port;
    uint32_t rcv_timeout_ms;
    uint32_t snd_timeout_ms;
    uint8_t  dst_ip[4];
    uint16_t dst_port;
    uint8_t  connected;
} toe_desc_t;

static toe_desc_t g_desc[WIZTOE_MAX_DESC];
static int8_t     g_sn_owner[WIZTOE_MAX_SOCK];
static uint8_t    g_tables_init;

static void toe_tcp_disconnect_if_connected(int fd);
static int  toe_listener_rearm(int fd);

/* Zero-initialised statics would read as "descriptor 0 owns every hardware
 * socket" and "every descriptor is bound to sn 0", so both tables need an
 * explicit first touch. Done lazily from the allocation entry points, which is
 * the only way into the rest of the API. */
static void toe_tables_init(void)
{
    if (g_tables_init)
        return;
    for (int i = 0; i < WIZTOE_MAX_SOCK; i++)
        g_sn_owner[i] = TOE_SN_FREE;
    for (int i = 0; i < WIZTOE_MAX_DESC; i++)
        g_desc[i].sn = -1;
    g_tables_init = 1;
}

/* Free a descriptor. Never plain memset(): sn == 0 is a valid hardware socket,
 * so the cleared descriptor has to be put back to "holds none" explicitly. */
static void toe_desc_clear(int fd)
{
    memset(&g_desc[fd], 0, sizeof(g_desc[fd]));
    g_desc[fd].sn = -1;
}

static int toe_fd_valid(int fd)
{
    return (fd >= 0) && (fd < WIZTOE_MAX_DESC) && g_desc[fd].used;
}

/* Valid AND holding a hardware socket. Every function that touches the chip
 * must use this, not toe_fd_valid(): a listener that could not be re-armed is
 * a live descriptor with sn == -1, and (uint8_t)-1 would address socket 255. */
static int toe_fd_ready(int fd)
{
    return toe_fd_valid(fd) && g_desc[fd].sn >= 0;
}

/* The ONLY place a descriptor is turned into a hardware socket number. Keeping
 * it to one accessor is what makes "no (uint8_t)fd left anywhere" checkable. */
static uint8_t toe_sn(int fd)
{
    return (uint8_t)g_desc[fd].sn;
}

static int toe_desc_alloc(void)
{
    for (int fd = 0; fd < WIZTOE_MAX_DESC; fd++)
    {
        if (!g_desc[fd].used)
        {
            toe_desc_clear(fd);
            g_desc[fd].used = 1;
            return fd;
        }
    }
    return -1;
}

/* Claim a free hardware socket for `fd`. Does not touch the chip. */
static int toe_sn_alloc(int fd)
{
    for (int sn = 0; sn < WIZTOE_MAX_SOCK; sn++)
    {
        if (g_sn_owner[sn] == TOE_SN_FREE)
        {
            g_sn_owner[sn] = (int8_t)fd;
            return sn;
        }
    }
    return -1;
}

static void toe_sn_release(int sn)
{
    if (sn >= 0 && sn < WIZTOE_MAX_SOCK)
        g_sn_owner[sn] = TOE_SN_FREE;
}

/* A listener that had to give up its hardware socket takes one as soon as one
 * frees up. Called from close(), which is the only way a socket comes back. */
static void toe_rearm_pending_listeners(void)
{
    for (int fd = 0; fd < WIZTOE_MAX_DESC; fd++)
    {
        if (g_desc[fd].used && g_desc[fd].listening && g_desc[fd].sn < 0)
            (void)toe_listener_rearm(fd);
    }
}

static uint8_t toe_open_flag(int fd)
{
    return g_desc[fd].nodelay ? SF_TCP_NODELAY : 0;
}

/* Open the hardware socket for a UDP fd.
 *
 * A single place for the UDP open so callers that need the socket reopened --
 * examples/udp_multicast reopens it with Sn_MR_MULTI to join a group -- can see
 * exactly what the plain open does. */
static int toe_open_udp(int fd)
{
    uint8_t sn = toe_sn(fd);
    if (socket(sn, Sn_MR_UDP, g_desc[fd].port, 0) != sn)
        return -1;

    g_desc[fd].opened = 1;
    return 0;
}

void wiztoe_network_init(const uint8_t ip[4], const uint8_t mask[4],
                         const uint8_t gw[4], const uint8_t mac[6])
{
    wiz_NetInfo ni;
    toe_tables_init();
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

    toe_tables_init();

    if (type != 1 /* SOCK_STREAM */ && type != 2 /* SOCK_DGRAM */)
        return -1;

    int fd = toe_desc_alloc();
    if (fd < 0)
        return -1;

    /* The hardware socket is claimed here rather than at bind/listen/connect so
     * that exhaustion is still reported by socket(), the way it always was. */
    int sn = toe_sn_alloc(fd);
    if (sn < 0)
    {
        toe_desc_clear(fd);
        return -1;
    }

    g_desc[fd].sn = (int8_t)sn;
    g_desc[fd].is_udp = (type == 2);
    return fd;
}

int wiztoe_is_udp(int fd)
{
    return toe_fd_valid(fd) && g_desc[fd].is_udp;
}

int wiztoe_set_nonblocking(int fd, int enable)
{
    if (!toe_fd_valid(fd))
        return -1;

    g_desc[fd].nonblocking = enable ? 1 : 0;
    return 0;
}

int wiztoe_get_nonblocking(int fd)
{
    if (!toe_fd_valid(fd))
        return -1;

    return g_desc[fd].nonblocking ? 1 : 0;
}

int wiztoe_available(int fd)
{
    if (!toe_fd_valid(fd))
        return -1;
    if (!toe_fd_ready(fd) || !g_desc[fd].opened)
        return 0;

    return (int)getSn_RX_RSR(toe_sn(fd));
}

int wiztoe_shutdown(int fd, int shut_rd, int shut_wr)
{
    if (!toe_fd_valid(fd))
        return -1;

    if (shut_rd)
    {
        /* The chip keeps filling its RX buffer regardless; all we can honour is
         * the promise that this side stops delivering data. */
        g_desc[fd].rd_shutdown = 1;
    }

    if (shut_wr && !g_desc[fd].is_udp && toe_fd_ready(fd))
    {
        /* Send FIN so the peer observes EOF. The fd stays allocated -- freeing
         * it is close()'s job, and a half-closed socket must still be readable. */
        toe_tcp_disconnect_if_connected(fd);
    }

    return 0;
}

int wiztoe_bind(int fd, uint16_t port)
{
    if (!toe_fd_ready(fd))
        return -1;

    g_desc[fd].port = port;

    if (g_desc[fd].is_udp)
    {
        if (toe_open_udp(fd) < 0)
            return -1;
    }
    return 0;
}

int wiztoe_listen(int fd, int backlog)
{
    (void)backlog;

    if (!toe_fd_ready(fd) || g_desc[fd].is_udp)
        return -1;

    uint8_t sn = toe_sn(fd);
    if (socket(sn, Sn_MR_TCP, g_desc[fd].port, toe_open_flag(fd)) != sn)
        return -1;
    g_desc[fd].opened = 1;

    if (listen(sn) != SOCK_OK)
        return -1;

    g_desc[fd].listening = 1;
    return 0;
}

/* Re-open the hardware socket this listener already owns and put it back in
 * LISTEN. ioLibrary's socket() issues CLOSE before OPEN, so this is valid from
 * ANY socket state, and listen() requires exactly the SOCK_INIT that socket()
 * leaves behind. */
static int toe_listener_reopen(int fd)
{
    uint8_t sn = toe_sn(fd);

    if (socket(sn, Sn_MR_TCP, g_desc[fd].port, toe_open_flag(fd)) != sn)
        return -1;
    g_desc[fd].opened = 1;

    if (listen(sn) != SOCK_OK)
        return -1;

    return 0;
}

/* Give up the hardware socket a listener is holding. Used when it can no longer
 * be driven back to LISTEN: the descriptor stays live but unarmed, and the next
 * poll (or a close() elsewhere) picks a different socket for it. */
static void toe_listener_disarm(int fd)
{
    if (g_desc[fd].sn >= 0)
        toe_sn_release(g_desc[fd].sn);
    g_desc[fd].sn = -1;
    g_desc[fd].opened = 0;
}

/* Put an unarmed listener back on the air: claim a free hardware socket and
 * open+listen it on the listener's port. Best effort -- with none free the
 * descriptor stays live but unarmed, and close() retries later. */
static int toe_listener_rearm(int fd)
{
    if (g_desc[fd].sn >= 0)
        return 0;

    int sn = toe_sn_alloc(fd);
    if (sn < 0)
        return -1;

    g_desc[fd].sn = (int8_t)sn;
    if (toe_listener_reopen(fd) < 0)
    {
        toe_listener_disarm(fd);
        return -1;
    }
    return 0;
}

/* The listener's hardware socket has reached ESTABLISHED. Hand that socket to a
 * new descriptor (the connection) and move the listener to a fresh one. */
static int toe_accept_established(int listen_fd)
{
    uint8_t sn = toe_sn(listen_fd);

    int cfd = toe_desc_alloc();
    if (cfd < 0)
    {
        /* No descriptor to hand out. The connection is established on the chip
         * and stays there; report "not now" rather than dropping it. */
        return WIZTOE_ERR_WOULDBLOCK;
    }

    g_desc[cfd].sn        = (int8_t)sn;
    g_desc[cfd].opened    = 1;
    g_desc[cfd].connected = 1;
    g_desc[cfd].port      = g_desc[listen_fd].port;
    /* Carried over because they describe the endpoint, not the listening role.
     * O_NONBLOCK deliberately is NOT: POSIX does not inherit it across accept(),
     * and callers set it on the accepted socket themselves. */
    g_desc[cfd].nodelay        = g_desc[listen_fd].nodelay;
    g_desc[cfd].rcv_timeout_ms = g_desc[listen_fd].rcv_timeout_ms;
    g_desc[cfd].snd_timeout_ms = g_desc[listen_fd].snd_timeout_ms;
    g_sn_owner[sn] = (int8_t)cfd;

    /* The listener no longer owns that socket; give it another one. */
    g_desc[listen_fd].sn = -1;
    g_desc[listen_fd].opened = 0;
    (void)toe_listener_rearm(listen_fd);

    return cfd;
}

int wiztoe_accept(int fd)
{
    if (!toe_fd_valid(fd) || !g_desc[fd].listening)
        return -1;

    uint32_t waited = 0;
    for (;;)
    {
        if (g_desc[fd].sn < 0)
        {
            /* Unarmed: every hardware socket was taken when this listener last
             * needed one. Nothing can arrive until one is returned. */
            (void)toe_listener_rearm(fd);
        }
        else
        {
            /* A whitelist, deliberately. The chip drives Sn_SR on its own, and
             * the previous "handle ESTABLISHED and CLOSED, ignore the rest"
             * shape meant any state nobody had thought of parked the listener
             * for good -- a peer that closed before this call reached it left
             * the socket in SOCK_CLOSE_WAIT, which accepts no further SYN and
             * never returns to LISTEN by itself. Every state that is not
             * usable-as-a-listener now ends in a re-open. */
            switch (getSn_SR(toe_sn(fd)))
            {
            case SOCK_ESTABLISHED:
            case SOCK_CLOSE_WAIT:
                /* CLOSE_WAIT is a half-close, not a dead socket: the peer sent
                 * FIN, but anything it sent before that is still in the RX
                 * buffer and the chip can still transmit (w5500.h, Sn_SR docs).
                 * So it is handed over exactly like ESTABLISHED -- recv()
                 * drains it and then reports EOF, and close() answers the FIN.
                 * Discarding it here would drop a connection BSD delivers, and
                 * dropping it silently is what used to wedge the listener. */
                return toe_accept_established(fd);

            case SOCK_LISTEN:
                break;                         /* armed and idle: the normal case */

            case SOCK_SYNRECV:
                /* Handshake in flight. Re-opening now would kill a connection
                 * that is still being established. */
                break;

            case SOCK_INIT:
                /* Opened, but the LISTEN command did not take. listen() wants
                 * precisely this state, so retry just that. */
                if (listen(toe_sn(fd)) != SOCK_OK)
                    toe_listener_disarm(fd);
                break;

            default:
                /* SOCK_CLOSED, the closing states (FIN_WAIT / CLOSING /
                 * TIME_WAIT / LAST_ACK, which only leave on a chip timeout), a
                 * stale non-TCP mode, or a value not in the datasheet. None can
                 * accept a SYN; none recover into LISTEN unaided. */
                if (toe_listener_reopen(fd) < 0)
                    toe_listener_disarm(fd);
                break;
            }
        }
        /* Re-armed above if needed, so the listener is live either way; report
         * "nothing pending" rather than waiting for a client to show up. */
        if (g_desc[fd].nonblocking)
            return WIZTOE_ERR_WOULDBLOCK;
        if (g_desc[fd].rcv_timeout_ms && ++waited >= g_desc[fd].rcv_timeout_ms)
            return WIZTOE_ERR_TIMEOUT;
        toe_yield_1ms();
    }
}

int wiztoe_connect(int fd, const uint8_t ip[4], uint16_t port)
{
    if (!toe_fd_ready(fd))
        return -1;

    if (g_desc[fd].is_udp)
    {
        memcpy(g_desc[fd].dst_ip, ip, 4);
        g_desc[fd].dst_port = port;
        g_desc[fd].connected = 1;
        return 0;
    }

    /* Randomized ephemeral local port to avoid TIME_WAIT 4-tuple reuse after a
     * reset (ioLibrary's static sock_any_port restarts at 0xC000 each boot). */
    uint16_t lport = g_desc[fd].port;
    if (lport == 0)
    {
        lport = (uint16_t)(0xC000u + (toe_time_us() % 0x3FF0u));
        g_desc[fd].port = lport;
    }
    uint8_t sn = toe_sn(fd);
    if (socket(sn, Sn_MR_TCP, lport, toe_open_flag(fd)) != sn)
        return -1;
    g_desc[fd].opened = 1;

    return (connect(sn, (uint8_t *)ip, port) == SOCK_OK) ? 0 : -1;
}

int wiztoe_send(int fd, const void *buf, size_t len)
{
    if (!toe_fd_ready(fd) || g_desc[fd].is_udp)
        return -1;
    if (len > 0xFFFF)
        len = 0xFFFF;

    uint8_t sn = toe_sn(fd);

    if (g_desc[fd].nonblocking)
    {
        /* ioLibrary's send() spins in `while (len > freesize)` until the chip
         * drains, which is exactly the wait a non-blocking caller forbade.
         * Clamping to the free space keeps it on the `len <= freesize` path,
         * where it returns without waiting, and a short write is what POSIX
         * expects from a non-blocking stream send. */
        uint16_t freesize = getSn_TX_FSR(sn);
        if (freesize == 0)
            return WIZTOE_ERR_WOULDBLOCK;
        if (len > freesize)
            len = freesize;
    }

    int32_t n = send(sn, (uint8_t *)buf, (uint16_t)len);
    return (n < 0) ? -1 : (int)n;
}

int wiztoe_recv(int fd, void *buf, size_t len)
{
    if (!toe_fd_ready(fd) || g_desc[fd].is_udp)
        return -1;
    if (len > 0xFFFF)
        len = 0xFFFF;

    if (g_desc[fd].rd_shutdown)
        return 0;                              /* EOF after shutdown(SHUT_RD) */

    uint8_t sn = toe_sn(fd);
    uint32_t waited = 0;
    for (;;)
    {
        if (getSn_RX_RSR(sn) > 0)
            break;
        if (getSn_SR(sn) != SOCK_ESTABLISHED)
            return 0;                          /* EOF */
        /* Checked after the state test so a closed connection still reports EOF
         * rather than EWOULDBLOCK -- a non-blocking reader must be able to see
         * the end of the stream. */
        if (g_desc[fd].nonblocking)
            return WIZTOE_ERR_WOULDBLOCK;
        if (g_desc[fd].rcv_timeout_ms)
        {
            if (++waited >= g_desc[fd].rcv_timeout_ms)
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

    int32_t n = recv(sn, (uint8_t *)buf, (uint16_t)len);
    if (n == SOCKERR_SOCKSTATUS || n == SOCKERR_SOCKCLOSED)
        return 0;                              /* EOF */
    return (n < 0) ? -1 : (int)n;
}

int wiztoe_sendto(int fd, const void *buf, size_t len,
                  const uint8_t ip[4], uint16_t port)
{
    if (!toe_fd_ready(fd) || !g_desc[fd].is_udp)
        return -1;
    if (len > 0xFFFF)
        len = 0xFFFF;

    uint8_t sn = toe_sn(fd);

    if (!g_desc[fd].opened)
    {
        if (socket(sn, Sn_MR_UDP, g_desc[fd].port, 0) != sn)
            return -1;
        g_desc[fd].opened = 1;
    }

    int32_t n = sendto(sn, (uint8_t *)buf, (uint16_t)len,
                       (uint8_t *)ip, port);
    return (n < 0) ? -1 : (int)n;
}

int wiztoe_recvfrom(int fd, void *buf, size_t len, uint8_t ip[4], uint16_t *port)
{
    if (!toe_fd_ready(fd) || !g_desc[fd].is_udp || !g_desc[fd].opened)
        return -1;
    if (len > 0xFFFF)
        len = 0xFFFF;

    uint8_t sn = toe_sn(fd);
    uint32_t waited = 0;
    for (;;)
    {
        if (getSn_RX_RSR(sn) > 0)
            break;
        if (getSn_SR(sn) != SOCK_UDP)
            return -1;
        if (g_desc[fd].nonblocking)
            return WIZTOE_ERR_WOULDBLOCK;
        if (g_desc[fd].rcv_timeout_ms && ++waited >= g_desc[fd].rcv_timeout_ms)
            return WIZTOE_ERR_TIMEOUT;
        toe_yield_1ms();
    }

    int32_t n = recvfrom(sn, (uint8_t *)buf, (uint16_t)len, ip, port);
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
    if (g_desc[fd].is_udp)
    {
        memcpy(ip, g_desc[fd].dst_ip, 4);
        *port = g_desc[fd].dst_port;
        return;
    }
    if (!toe_fd_ready(fd))
    {
        memset(ip, 0, 4);
        *port = 0;
        return;
    }
    getSn_DIPR(toe_sn(fd), ip);
    *port = getSn_DPORT(toe_sn(fd));
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
    *port = g_desc[fd].port;
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
    toe_tables_init();
    for (int sn = 0; sn < WIZTOE_MAX_SOCK; sn++)
    {
        if (g_sn_owner[sn] == TOE_SN_FREE)
        {
            g_sn_owner[sn] = TOE_SN_RAW;
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
        toe_sn_release(sn);
        toe_rearm_pending_listeners();
    }
}

static void toe_tcp_disconnect_if_connected(int fd)
{
    uint8_t sn = toe_sn(fd);
    uint8_t sr = getSn_SR(sn);
    if (sr == SOCK_ESTABLISHED || sr == SOCK_CLOSE_WAIT)
        disconnect(sn);
}

int wiztoe_close(int fd)
{
    if (!toe_fd_valid(fd))
        return -1;

    /* One meaning only: this descriptor is finished. A listener used to be
     * re-armed in place here, because the connection and the listener shared an
     * fd; they no longer do, so close() just closes. */
    if (g_desc[fd].sn >= 0)
    {
        uint8_t sn = toe_sn(fd);
        if (!g_desc[fd].is_udp)
            toe_tcp_disconnect_if_connected(fd);
        if (g_desc[fd].opened)
            close(sn);
        toe_sn_release(sn);
    }
    toe_desc_clear(fd);

    /* A hardware socket just came back; a listener may be waiting for one. */
    toe_rearm_pending_listeners();
    return 0;
}

int wiztoe_setsockopt(int fd, wiztoe_opt_t opt, const void *val, size_t len)
{
    if (!toe_fd_valid(fd) || val == NULL || len == 0)
        return -1;

    int v = (len >= sizeof(int)) ? *(const int *)val : *(const uint8_t *)val;

    /* Options that only live in the descriptor work on an unarmed listener too;
     * the ones that write a chip register need a hardware socket. */
    switch (opt)
    {
    case WIZTOE_OPT_NODELAY:
        g_desc[fd].nodelay = (v != 0);
        return 0;
    case WIZTOE_OPT_RCVTIMEO_MS:
        if (len < sizeof(uint32_t)) return -1;
        g_desc[fd].rcv_timeout_ms = *(const uint32_t *)val;
        return 0;
    case WIZTOE_OPT_SNDTIMEO_MS:
        if (len < sizeof(uint32_t)) return -1;
        g_desc[fd].snd_timeout_ms = *(const uint32_t *)val;
        return 0;
    default:
        break;
    }

    if (!toe_fd_ready(fd))
        return -1;

    switch (opt)
    {
    case WIZTOE_OPT_KEEPALIVE:
        setSn_KPALVTR(toe_sn(fd), v ? 12 : 0);
        return 0;
    case WIZTOE_OPT_KEEPIDLE:
        if (v < 5) v = 5;
        if (v > 5 * 255) v = 5 * 255;
        setSn_KPALVTR(toe_sn(fd), (uint8_t)(v / 5));
        return 0;
    case WIZTOE_OPT_TTL:
        setSn_TTL(toe_sn(fd), (uint8_t)v);
        return 0;
    case WIZTOE_OPT_TOS:
        setSn_TOS(toe_sn(fd), (uint8_t)v);
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
    case WIZTOE_OPT_TYPE:       *out = g_desc[fd].is_udp ? 2 : 1; break;
    case WIZTOE_OPT_RCVTIMEO_MS: *(uint32_t *)val = g_desc[fd].rcv_timeout_ms; break;
    case WIZTOE_OPT_SNDTIMEO_MS: *(uint32_t *)val = g_desc[fd].snd_timeout_ms; break;
    case WIZTOE_OPT_RCVBUF:
        if (!toe_fd_ready(fd)) return -1;
        *out = (int)getSn_RxMAX(toe_sn(fd));
        break;
    case WIZTOE_OPT_SNDBUF:
        if (!toe_fd_ready(fd)) return -1;
        *out = (int)getSn_TxMAX(toe_sn(fd));
        break;
    case WIZTOE_OPT_TTL:
        if (!toe_fd_ready(fd)) return -1;
        *out = (int)getSn_TTL(toe_sn(fd));
        break;
    case WIZTOE_OPT_TOS:
        if (!toe_fd_ready(fd)) return -1;
        *out = (int)getSn_TOS(toe_sn(fd));
        break;
    default: return -1;
    }
    *len = sizeof(int);
    return 0;
}
