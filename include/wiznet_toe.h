/*
 * Copyright (c) 2024 WIZnet Co.,Ltd
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * WIZnet TOE (TCP Offload Engine / hardwired TCP/IP) backend — neutral API.
 * Ported from WIZnet-PICO-LWIP-TOE-C (port/lwip/wiznet_toe.h).
 *
 * IMPORTANT: plain C types ONLY (no ioLibrary, no lwIP headers) so this can be
 * included by the __wrap_lwip_* glue without colliding with the ioLibrary
 * socket()/recv()/... names. wiznet_toe.c is the only TU that includes the
 * ioLibrary headers.
 *
 * PUBLIC header (include/), not because applications are expected to call this
 * API -- ordinary code uses BSD sockets and the wrap routes them here -- but
 * because the collision-free construction above is exactly what a caller sitting
 * next to ioLibrary needs, and one such caller exists: a multicast join has to
 * reopen its own chip socket, which no setsockopt can express. See
 * wiztoe_sn_of_fd() and examples/udp_multicast.
 *
 * File descriptors are NOT hardware socket numbers. They were the same integer
 * until accept() gained BSD semantics: a listener now hands its hardware socket
 * to the accepted connection and relocates onto a free one, so the two spaces
 * moved apart and stay apart (a reserved raw socket, or a listener temporarily
 * without one, is enough to shift them even before any accept). The fd a caller
 * holds is a descriptor index plus LWIP_SOCKET_OFFSET; code that genuinely needs
 * the chip socket behind an fd must ask for it -- see wiztoe_sn_of_fd().
 */
#ifndef _WIZNET_TOE_H_
#define _WIZNET_TOE_H_

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Extra error return (besides -1): blocking call hit SO_RCVTIMEO.
 * The wrap layer maps this to errno EWOULDBLOCK. */
#define WIZTOE_ERR_TIMEOUT (-2)

/* Extra error return: the socket is non-blocking and the operation would have
 * blocked. Kept distinct from WIZTOE_ERR_TIMEOUT so callers can tell "you asked
 * not to wait" from "you waited and time ran out"; the wrap layer maps both to
 * errno EWOULDBLOCK, which is what POSIX reports for either. */
#define WIZTOE_ERR_WOULDBLOCK (-3)

/* Neutral option codes — the wrap layer maps (level, optname) to these. */
typedef enum {
    WIZTOE_OPT_KEEPALIVE,
    WIZTOE_OPT_KEEPIDLE,
    WIZTOE_OPT_NODELAY,
    WIZTOE_OPT_TTL,
    WIZTOE_OPT_TOS,
    WIZTOE_OPT_RCVTIMEO_MS,
    WIZTOE_OPT_SNDTIMEO_MS,
    WIZTOE_OPT_RCVBUF,
    WIZTOE_OPT_SNDBUF,
    WIZTOE_OPT_ERROR,
    WIZTOE_OPT_TYPE
} wiztoe_opt_t;

int  wiztoe_setsockopt(int fd, wiztoe_opt_t opt, const void *val, size_t len);
int  wiztoe_getsockopt(int fd, wiztoe_opt_t opt, void *val, size_t *len);

/* fd allocation / lifetime */
int  wiztoe_socket(int domain, int type, int protocol);   /* type 1=STREAM, 2=DGRAM */
int  wiztoe_close(int fd);

/* Non-blocking mode (POSIX O_NONBLOCK).
 *
 * Blocking (the default, enable == 0) keeps the historical behaviour: recv /
 * recvfrom / accept poll the chip until data or a connection arrives, bounded
 * only by SO_RCVTIMEO if one was set.
 *
 * Non-blocking (enable != 0) makes those three return WIZTOE_ERR_WOULDBLOCK
 * immediately instead of waiting, and makes send report WIZTOE_ERR_WOULDBLOCK
 * when the chip's TX buffer is full (a partially-filled buffer yields a short
 * write, as POSIX allows). This is what an event-loop application needs: it
 * polls many sockets from one thread and can never afford to block in one.
 *
 * NOTE: connect() is NOT affected -- see the limitation documented in
 * docs/wsm_driver_fix.md FIX-2 (ioLibrary signals a non-blocking connect with
 * SOCK_BUSY, whose value 0 is indistinguishable from a clean EOF on recv).
 *
 * @return 0 on success, -1 if fd is not a live socket. */
int  wiztoe_set_nonblocking(int fd, int enable);
int  wiztoe_get_nonblocking(int fd);      /* 1 = non-blocking, 0 = blocking, -1 = bad fd */

/* Bytes immediately readable from the chip's RX buffer (0 if none).
 * -1 if fd is not a live socket. Used by readv() to fill later iovec entries
 * only while data is already there, and available for a future FIONREAD. */
int  wiztoe_available(int fd);

/* Half-close.
 *
 * shut_wr: send a TCP FIN (ioLibrary disconnect()) so the peer sees EOF.
 * shut_rd: the W5500 has no way to refuse further RX, so this only records the
 *          intent locally -- subsequent recv() calls report EOF (0) rather than
 *          returning data that is already buffered.
 * The fd stays allocated either way; releasing it remains close()'s job.
 *
 * @return 0 on success, -1 if fd is not a live socket. */
int  wiztoe_shutdown(int fd, int shut_rd, int shut_wr);

/* TCP */
int  wiztoe_bind(int fd, uint16_t port);
int  wiztoe_listen(int fd, int backlog);
int  wiztoe_accept(int fd);   /* BSD semantics: returns a NEW fd for the connection;
                               * the listener fd keeps listening (relocated onto a
                               * free hardware socket with the same port).
                               * A peer that closed before this call reaches it is
                               * still delivered (half-closed: readable, then EOF),
                               * and any socket state that cannot serve as a
                               * listener is re-opened rather than waited on. */
int  wiztoe_connect(int fd, const uint8_t ip[4], uint16_t port);

/* Blocking behaviour of send/recv/recvfrom, in priority order:
 *
 *   O_NONBLOCK set            -> return WIZTOE_ERR_WOULDBLOCK immediately
 *   WIZTOE_OPT_{RCV,SND}TIMEO -> wait at most that many milliseconds of real
 *                                time, then WIZTOE_ERR_TIMEOUT (recv) or
 *                                WIZTOE_ERR_WOULDBLOCK (send, POSIX EAGAIN
 *                                for "blocked and sent nothing")
 *   timeout 0 (the default)   -> wait forever, as POSIX defines a zero
 *                                SO_RCVTIMEO/SO_SNDTIMEO and as ioLibrary
 *                                behaves without SF_IO_NONBLOCK
 *
 * CONTRACT: the wait yields to other tasks but does NOT feed the calling task's
 * watchdog. A blocking socket with no timeout must therefore be driven from a
 * task of its own, never from a watchdog-supervised event loop -- there, set
 * O_NONBLOCK or a timeout.
 *
 * send() may return less than `len`: the chip's TX buffer bounds one transfer,
 * and POSIX allows a partial count once SO_SNDTIMEO has elapsed. */
int  wiztoe_send(int fd, const void *buf, size_t len);
int  wiztoe_recv(int fd, void *buf, size_t len);           /* 0 = EOF */

/* UDP */
int  wiztoe_sendto(int fd, const void *buf, size_t len, const uint8_t ip[4], uint16_t port);
int  wiztoe_recvfrom(int fd, void *buf, size_t len, uint8_t ip[4], uint16_t *port);
/* No multicast join here. The chip latches the group's MAC when the socket
 * opens, so joining an already-bound socket means closing and reopening it --
 * a decision about the application's own traffic rather than something the
 * port layer should take on its behalf. examples/udp_multicast does the reopen
 * itself; see the join seam there, and wiztoe_sn_of_fd() below for the only
 * supported way to learn which chip socket an fd is sitting on. */

/* helpers */
int  wiztoe_is_udp(int fd);

/* The hardware socket currently behind `fd`, or -1 if the descriptor is invalid
 * or holds no socket (an unarmed listener does not).
 *
 * For the rare caller that must reach past the socket API to a chip register --
 * examples/udp_multicast reopens its socket with Sn_MR_MULTI to join a group,
 * which no setsockopt can express (see the note above). The value is valid only
 * until the next call that can move sockets around (accept, close, listen), so
 * read it immediately before use and never cache it.
 *
 * Pass a wiztoe fd, i.e. a BSD fd with LWIP_SOCKET_OFFSET already subtracted. */
int  wiztoe_sn_of_fd(int fd);
void wiztoe_peer(int fd, uint8_t ip[4], uint16_t *port);
void wiztoe_getsockname(int fd, uint8_t ip[4], uint16_t *port);
void wiztoe_local_ip(uint8_t ip[4]);
void wiztoe_local_mac(uint8_t mac[6]);

/* Raw hardware-socket reservation (for ioLibrary DHCP_run/DNS_run), which take a
 * socket NUMBER rather than an fd.
 *
 * The value returned here is a hardware socket number, NOT a descriptor: it must
 * be passed to ioLibrary, never to the wiztoe_* functions above. (It used to be
 * both, because descriptors and hardware sockets were the same integer.) */
int  wiztoe_socket_reserve(void);
void wiztoe_socket_release(int sn);

/* Configure the chip's own network identity (TOE: the CHIP owns the IP). */
void wiztoe_network_init(const uint8_t ip[4], const uint8_t mask[4],
                         const uint8_t gw[4], const uint8_t mac[6]);

#ifdef __cplusplus
}
#endif

#endif /* _WIZNET_TOE_H_ */
