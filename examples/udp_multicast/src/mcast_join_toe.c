/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * Multicast join for the WIZnet hardware sockets (see mcast_join.h).
 *
 * The chip derives the group's multicast MAC for Sn_DHAR from Sn_DIPR when the
 * socket is opened, so a group cannot be added to a socket that is already
 * running. bind() has opened it by the time a BSD join arrives, so the join
 * closes the hardware socket and reopens it with the group programmed and
 * Sn_MR_MULTI set. Datagrams arriving during the reopen are lost, which is
 * acceptable: a join happens once at start-up, before any traffic is expected.
 *
 * This is the one place in the example that steps outside the socket API. It
 * needs the chip's socket number, and what it is handed is a BSD fd.
 *
 * It used to derive one from the other arithmetically:
 *
 *     fd == hardware socket number + LWIP_SOCKET_OFFSET
 *
 * That was never a published contract, and it is no longer true. Since accept()
 * gained BSD semantics a listener hands its hardware socket to the accepted
 * connection and relocates onto a free one, so descriptors and chip sockets
 * drift apart; a raw socket reserved for ioLibrary's DHCP_run() shifts them
 * too. The subtraction would still compile and would quietly address someone
 * else's socket. So the driver is asked instead, through wiztoe_sn_of_fd(),
 * and the answer is still checked against the chip before anything is written.
 *
 * Two things this file must not do. It must not include any lwIP header --
 * ioLibrary's socket.h declares close() as int8_t(uint8_t) against POSIX's
 * int(int), and lwIP drags the POSIX one in, so the two cannot be seen
 * together. That is why the socket offset arrives through a function
 * (mcast_lwip_socket_offset()) instead of the macro. And it must not be used
 * when the wrap is off -- see the guard below.
 */
#include <stdio.h>

#include "sdkconfig.h"
#include "esp_log.h"

#include "mcast_join.h"

static const char *TAG = "mcast_join";

#if CONFIG_WSM_DRIVER_SOCKET_WRAP

#include "wsm_driver/Ethernet/socket.h"    /* socket(), setSn_*, getSn_SR */
#include "wiznet_toe.h"                     /* wiztoe_sn_of_fd()           */

/* Dotted quad to four bytes. Deliberately not inet_addr(): that would mean
 * including a network header, and both candidates are ruled out here. */
static int parse_group(const char *group, uint8_t out[4])
{
    unsigned a, b, c, d;
    if (sscanf(group, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return -1;
    if (a > 255 || b > 255 || c > 255 || d > 255) return -1;
    out[0] = (uint8_t)a; out[1] = (uint8_t)b;
    out[2] = (uint8_t)c; out[3] = (uint8_t)d;
    return 0;
}

int mcast_join_toe(const void *ops, int fd, const char *group, uint16_t port)
{
    (void)ops;                              /* reaches the chip directly */

    /* fd is a BSD fd; the driver indexes its descriptors from 0. */
    int sn = wiztoe_sn_of_fd(fd - mcast_lwip_socket_offset());
    if (sn < 0) {
        ESP_LOGE(TAG, "fd %d holds no hardware socket — cannot join a group on it",
                 fd);
        return -1;
    }

    /* Belt and braces. bind() has just opened this socket for UDP, so if sn really
     * is the chip socket behind `fd`, the chip agrees it is in SOCK_UDP. Anything
     * else means we are about to reopen a socket belonging to someone else, and
     * losing multicast is a far better outcome than that. */
    if (getSn_SR((uint8_t)sn) != SOCK_UDP) {
        ESP_LOGE(TAG, "socket %d is not open for UDP (Sn_SR=0x%02x) — refusing "
                      "to reopen it; it is not the socket behind this fd",
                 sn, getSn_SR((uint8_t)sn));
        return -1;
    }

    uint8_t g[4];
    if (parse_group(group, g) < 0) {
        ESP_LOGE(TAG, "cannot parse group \"%s\"", group);
        return -1;
    }

    /* Multicast MAC for an IPv4 group: 01:00:5E plus the low 23 bits of the
     * group address (RFC 1112). */
    uint8_t mac[6] = { 0x01, 0x00, 0x5E, (uint8_t)(g[1] & 0x7F), g[2], g[3] };

    /* Deliberately no close() here. ioLibrary's socket() closes the socket
     * itself before reopening it (Ethernet/socket.c), and calling close() from
     * this file would not reach ioLibrary's anyway: the component compiles its
     * own sources with -Dclose=wiz_close so that ioLibrary's close(uint8_t)
     * stops hijacking newlib's POSIX close(int), and that definition does not
     * extend to the example. A bare close(sn) here therefore resolves to POSIX
     * close(), which closes file descriptor `sn` -- 0 being stdin. On a UART
     * console that passed unnoticed; on USB Serial/JTAG it takes the console
     * down with it, which is how this was found.
     *
     * The registers are written before socket() and survive its internal close;
     * the chip latches the multicast MAC from them when the socket opens. */
    setSn_DHAR((uint8_t)sn, mac);
    setSn_DIPR((uint8_t)sn, g);
    setSn_DPORT((uint8_t)sn, port);

    /* Multicast belongs in the FLAG argument, not the protocol argument:
     * socket() validates it there (see ioLibrary socket.c, Sn_MR_UDP case), and
     * OR-ing it into the protocol yields 0x82, which matches no protocol case
     * at all -- the bug the component's unused helper used to have. */
    if (socket((uint8_t)sn, Sn_MR_UDP, port, SF_MULTI_ENABLE) != sn) {
        ESP_LOGE(TAG, "reopening socket %d with multicast failed", sn);
        return -1;
    }
    return 0;
}

#else /* !CONFIG_WSM_DRIVER_SOCKET_WRAP */

/*
 * With the esp_eth backend the wrap is off, so `fd` is a genuine LwIP socket and
 * there is no hardware socket behind it at all (wiznet_toe.c is not even built
 * in that configuration). main.c picks mcast_join_bsd() in that build and
 * never calls this; the stub exists so the link does not depend on that being
 * true, and says so loudly if it turns out not to be.
 */
int mcast_join_toe(const void *ops, int fd, const char *group, uint16_t port)
{
    (void)ops; (void)fd; (void)group; (void)port;
    ESP_LOGE(TAG, "mcast_join_toe() called with WSM_DRIVER_SOCKET_WRAP off — "
                  "use mcast_join_bsd() for the esp_eth backend");
    return -1;
}

#endif /* CONFIG_WSM_DRIVER_SOCKET_WRAP */
