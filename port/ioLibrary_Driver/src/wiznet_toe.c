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

/* How long close() may spend waiting for a graceful TCP shutdown before it
 * gives up and tears the socket down locally.
 *
 * A FIN/ACK on a healthy LAN completes in well under a millisecond, so this is
 * a safety net rather than a budget. It has to stay far below ESP-IDF's task
 * watchdog (5 s by default), because this wait happens on whatever task called
 * close() -- for ESPHome that is the main loop. */
#ifndef WIZTOE_DISCONNECT_TIMEOUT_MS
#define WIZTOE_DISCONNECT_TIMEOUT_MS 250u
#endif

/* toe_listener_reopen() returns this instead of an error when the chip has no
 * source address yet: the socket cannot be opened, but nothing is wrong and the
 * descriptor keeps its hardware socket so a later attempt can succeed. Callers
 * test for < 0, so a deferral never looks like a failure. */
#define WIZTOE_DEFER (1)

static void toe_tcp_disconnect_bounded(int fd);
static int  toe_listener_rearm(int fd);
static int  toe_listener_reopen(int fd);

/* Has `timeout_ms` of wall time passed since `started`?
 *
 * The waits below used to count loop iterations and treat one as a millisecond.
 * They are not: toe_yield_1ms() lands on a tick boundary and each turn also
 * costs two SPI register reads, so a 2 s SO_RCVTIMEO expired appreciably later
 * than 2 s -- which matters, because 2 s is exactly what ESPHome's OTA asks for
 * and the task watchdog is only 5 s.
 *
 * timeout_ms == 0 means "no timeout" at every layer involved -- POSIX for
 * SO_RCVTIMEO/SO_SNDTIMEO, and ioLibrary when SF_IO_NONBLOCK is unset -- so it
 * never expires here either. Unsigned subtraction, so the microsecond counter
 * wrapping (~71 min) is harmless. */
static int toe_deadline_passed(uint32_t started, uint32_t timeout_ms)
{
    if (timeout_ms == 0)
        return 0;
    return (uint32_t)(toe_time_us() - started) >= timeout_ms * 1000u;
}

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

/* The one sanctioned way out of the descriptor space. Everything inside this
 * file uses toe_sn(); this exists so a caller that must reach a chip register
 * does not have to guess the mapping, which stopped being fd == sn when accept()
 * started relocating listeners. */
int wiztoe_sn_of_fd(int fd)
{
    if (!toe_fd_valid(fd))
        return -1;
    return g_desc[fd].sn;          /* -1 when the descriptor holds no socket */
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
        toe_tcp_disconnect_bounded(fd);
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

    /* Succeeds even with no address yet: accept() opens the socket once one
     * arrives. Failing here would be permanent for a server that only listens
     * at start-up, and listening before an address exists is ordinary POSIX. */
    g_desc[fd].listening = 1;
    if (toe_listener_reopen(fd) < 0)
    {
        g_desc[fd].listening = 0;
        return -1;
    }
    return 0;
}

/* Does the chip hold a source address yet?
 *
 * ioLibrary's socket() refuses to open a TCP socket while SIPR is zero
 * (Ethernet/socket.c), so every listen() fails until an address exists. With
 * DHCP that is not an error, just "not yet": the address arrives seconds after
 * boot, long after a server has asked to listen. Read the same register
 * ioLibrary reads, so the two never disagree. */
static int toe_chip_has_ip(void)
{
    uint32_t sipr = 0;
    getSIPR((uint8_t *)&sipr);
    return sipr != 0;
}

/* Re-open the hardware socket this listener already owns and put it back in
 * LISTEN. ioLibrary's socket() issues CLOSE before OPEN, so this is valid from
 * ANY socket state, and listen() requires exactly the SOCK_INIT that socket()
 * leaves behind. Returns WIZTOE_DEFER while the chip has no address; only -1
 * means the socket is unusable. */
static int toe_listener_reopen(int fd)
{
    uint8_t sn = toe_sn(fd);

    if (!toe_chip_has_ip())
    {
        g_desc[fd].opened = 0;
        return WIZTOE_DEFER;
    }

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

    const uint32_t started = toe_time_us();
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
        if (toe_deadline_passed(started, g_desc[fd].rcv_timeout_ms))
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

    /* Wait for room in the chip's TX buffer HERE rather than inside ioLibrary.
     *
     * ioLibrary's send() ends in `while (len > freesize)` (socket.c) with no
     * timeout and -- unlike the receive path -- no yield either: a pure SPI
     * busy-poll that only leaves when the socket stops being ESTABLISHED. So a
     * peer that stops reading, or a link that dies, parks the caller there for
     * as long as the chip keeps the connection. SO_SNDTIMEO was accepted and
     * then ignored.
     *
     * Clamping len to the free space keeps send() on its `len <= freesize`
     * path, where it returns without waiting. The non-blocking case already
     * did this; the wait is now simply ours in every mode, which is what makes
     * SO_SNDTIMEO mean something. */
    const uint32_t started = toe_time_us();
    uint16_t freesize;

    for (;;)
    {
        uint8_t sr = getSn_SR(sn);
        if (sr != SOCK_ESTABLISHED && sr != SOCK_CLOSE_WAIT)
            return -1;                         /* connection gone */

        freesize = getSn_TX_FSR(sn);
        if (freesize > 0)
            break;

        if (g_desc[fd].nonblocking)
            return WIZTOE_ERR_WOULDBLOCK;
        if (toe_deadline_passed(started, g_desc[fd].snd_timeout_ms))
            return WIZTOE_ERR_WOULDBLOCK;      /* POSIX: nothing sent -> EAGAIN */
        toe_yield_1ms();
    }

    if (len > freesize)
        len = freesize;                        /* short write; see below */

    int32_t n = send(sn, (uint8_t *)buf, (uint16_t)len);

    /* SOCK_BUSY is 0 (socket.h), returned when a previous send has not been
     * acknowledged by the chip yet. Passing that through would look like "sent
     * zero bytes" to a POSIX caller, which is not a value send() may return for
     * a non-empty buffer -- writeall_() style loops read it as no progress.
     * It is a retry condition, so it is reported as one. */
    if (n == SOCK_BUSY)
        return WIZTOE_ERR_WOULDBLOCK;
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
    const uint32_t started = toe_time_us();
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
        if (toe_deadline_passed(started, g_desc[fd].rcv_timeout_ms))
            return WIZTOE_ERR_TIMEOUT;
        /* Yield rather than busy-poll (unlike the Pico original) so the idle
         * task runs. With no SO_RCVTIMEO this waits forever, by POSIX -- see
         * the contract note in wiznet_toe.h. */
        toe_yield_1ms();
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
    const uint32_t started = toe_time_us();
    for (;;)
    {
        if (getSn_RX_RSR(sn) > 0)
            break;
        if (getSn_SR(sn) != SOCK_UDP)
            return -1;
        if (g_desc[fd].nonblocking)
            return WIZTOE_ERR_WOULDBLOCK;
        if (toe_deadline_passed(started, g_desc[fd].rcv_timeout_ms))
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

/* PHY link state, straight from the chip. Read through ioLibrary so this TU
 * keeps its header isolation (it must not pull in ESP-IDF headers -- see the
 * note in toe_port.h). */
static int toe_link_is_up(void)
{
    uint8_t link = PHY_LINK_OFF;
    if (ctlwizchip(CW_GET_PHYLINK, (void *)&link) < 0)
        return 0;                              /* unreadable -> treat as down */
    return link == PHY_LINK_ON;
}

/* Graceful TCP shutdown that is guaranteed to return.
 *
 * ioLibrary's disconnect() cannot be used here. It ends in
 *
 *     while (getSn_SR(sn) != SOCK_CLOSED)     socket.c:502
 *
 * whose only exits are the peer acknowledging our FIN and the chip's own
 * retransmit timeout. Neither arrives while the cable is out: the FIN never
 * leaves the chip, so the timeout does not advance either. Measured on hardware
 * that spin held the caller for over 4 s and tripped the task watchdog, which
 * rebooted the device on nothing worse than someone unplugging a cable. It
 * takes no timeout argument, so it cannot be bounded from the outside -- the
 * DISCON sequence is reproduced here with a wait we own.
 *
 * (ioLibrary's non-blocking escape at socket.c:499 is deliberately not used:
 * it needs SF_IO_NONBLOCK in sock_io_mode, which is a global that also changes
 * send/recv/connect semantics, and it only makes disconnect() return SOCK_BUSY
 * -- handing the cleanup back to the caller rather than solving it.)
 *
 * The caller closes the socket afterwards either way, so every path here is
 * free to give up: Sn_CR_CLOSE is chip-local and completes with the link down. */
static void toe_tcp_disconnect_bounded(int fd)
{
    uint8_t sn = toe_sn(fd);
    uint8_t sr = getSn_SR(sn);

    if (sr != SOCK_ESTABLISHED && sr != SOCK_CLOSE_WAIT)
        return;                                /* nothing to shut down politely */

    /* Link already down: the FIN cannot be transmitted, so waiting for the
     * peer to acknowledge it is waiting for something that cannot happen. */
    if (!toe_link_is_up())
        return;

    setSn_CR(sn, Sn_CR_DISCON);
    while (getSn_CR(sn));                      /* command latch: chip-local */

    const uint32_t started = toe_time_us();
    const uint32_t limit_us = WIZTOE_DISCONNECT_TIMEOUT_MS * 1000u;

    for (;;)
    {
        if (getSn_SR(sn) == SOCK_CLOSED)
            return;                            /* peer answered: clean close */
        if (getSn_IR(sn) & Sn_IR_TIMEOUT)
            return;                            /* chip gave up first */
        /* Unsigned subtraction, so a wrap of the microsecond counter is fine. */
        if ((uint32_t)(toe_time_us() - started) >= limit_us)
            return;                            /* our own cap */
        if (!toe_link_is_up())
            return;                            /* cable pulled mid-handshake */
        toe_yield_1ms();
    }
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
            toe_tcp_disconnect_bounded(fd);
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
