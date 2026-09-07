/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * Multicast group membership, split out of the receive engine.
 *
 * Joining is the one step in this example that cannot be written once for both
 * backends, because the two disagree about when the group has to be known:
 *
 *   - LwIP joins an already-bound socket. setsockopt(IP_ADD_MEMBERSHIP) is all
 *     it takes, and the IGMP report goes out from software.
 *   - The W6300 derives the group's multicast MAC for Sn_DHAR from Sn_DIPR at
 *     the moment the socket is opened, so the group must be in the registers
 *     BEFORE the open -- after bind() has already opened it. The only way to
 *     apply a join at that point is to close the hardware socket and reopen it
 *     with Sn_MR_MULTI set.
 *
 * Everything else -- socket, bind, recvfrom -- is plain BSD through the vtable
 * on both paths. Only this one call is backend-specific, which is why it is a
 * function pointer rather than an #if inside mcast_rx.c.
 *
 * Deliberately no component headers here: mcast_join_toe.c includes ioLibrary's
 * socket.h and mcast_join_bsd.c includes lwIP's, and those two must never meet
 * in one translation unit. `ops` is a const void * for the same reason.
 */
#ifndef MCAST_JOIN_H
#define MCAST_JOIN_H

#include <stdint.h>

/*
 * Join `group` on the socket `fd`, which is already bound to `port`.
 * Returns 0 on success, -1 on failure.
 *
 * ops is the net_sock_ops_t the caller is using, passed opaquely so an
 * implementation that needs BSD calls can use them.
 */
typedef int (*mcast_join_fn)(const void *ops, int fd,
                             const char *group, uint16_t port);

/* Software LwIP: one setsockopt. Use for Wi-Fi, and for Ethernet when the
 * esp_eth backend is selected (WSM_DRIVER_SOCKET_WRAP=0). */
int mcast_join_bsd(const void *ops, int fd, const char *group, uint16_t port);

/* WIZnet hardware sockets: reopen the chip socket with the group programmed.
 * Only valid when WSM_DRIVER_SOCKET_WRAP=1 -- see mcast_join_toe.c. */
int mcast_join_toe(const void *ops, int fd, const char *group, uint16_t port);

/*
 * LWIP_SOCKET_OFFSET, which mcast_join_toe() has to subtract from an fd to get
 * the driver's descriptor index. That index is NOT the chip's socket number;
 * mcast_join_toe() asks wiztoe_sn_of_fd() for that.
 *
 * It lives behind a function because the macro is only reachable through lwIP
 * headers, and those cannot be included next to ioLibrary's socket.h -- both
 * declare close(), with different signatures. So the file that already includes
 * lwIP reports the value, and the file that cannot include it asks.
 */
int mcast_lwip_socket_offset(void);

#endif /* MCAST_JOIN_H */
