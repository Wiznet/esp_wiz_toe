/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * Network seam for the UPnP client.
 *
 * upnp_core.c reaches the network exclusively through this header and knows
 * nothing about sockets, lwIP, or the WIZnet chip. Only upnp_transport.c
 * includes lwIP.
 *
 * That also settled a collision the original had: UPnP.c carried its own
 * inet_addr / inet_ntoa / htons / htonl / ntohs / ntohl with signatures that
 * disagree with lwIP's. Because this seam takes addresses as strings, none of
 * them are reachable any more and they are gone rather than renamed.
 *
 * The seam sits at the level of whole exchanges rather than individual socket
 * calls. The original code opened a socket, spun on getSn_SR() until the chip
 * reported SOCK_INIT, connected, spun again for SOCK_ESTABLISHED, sent, polled
 * the receive register, and closed -- six chip-specific steps for what is one
 * request/response. None of those states exist in BSD sockets, so mapping the
 * calls one-to-one would have meant inventing them. Collapsing each exchange
 * into a single entry point removes them instead.
 *
 * `ops` is deliberately a const void * rather than a net_sock_ops_t *: that
 * type is an anonymous typedef with no struct tag, so it cannot be forward
 * declared, and naming it here would drag the component's headers -- and lwIP
 * behind them -- back into upnp_core.c.
 */
#ifndef UPNP_TRANSPORT_H
#define UPNP_TRANSPORT_H

#include <stddef.h>
#include <stdint.h>

/* Select the socket vtable every later call uses. Pass &net_eth_ops for the
 * WIZnet hardware sockets or &net_wifi_ops for the software LwIP stack. */
void upnp_transport_bind(const void *ops);

/*
 * Send one SSDP M-SEARCH to the multicast group and return the first reply.
 *
 * The reply is unicast back to the port we sent from, so one socket serves
 * both directions. Returns the number of bytes received, 0 on timeout, or -1
 * if the socket could not be set up.
 */
int upnp_transport_ssdp(const char *group_ip, uint16_t group_port,
                        uint16_t local_port, const char *msg,
                        char *resp, size_t resp_size, uint32_t timeout_ms);

/*
 * Run one HTTP request against the IGD and collect the response.
 *
 * Keeps reading until the peer closes, the buffer fills, or the timeout
 * expires -- UPnP responses are small and the router closes after each one.
 * Returns bytes received, 0 if nothing arrived, -1 on connect/send failure.
 */
int upnp_transport_http(const char *ip, uint16_t port, const char *request,
                        char *resp, size_t resp_size, uint32_t timeout_ms);

/*
 * Eventing callback server. The IGD connects back to the address advertised in
 * the SUBSCRIBE header and POSTs a notification.
 *
 * upnp_transport_listen() returns a listening fd, or -1.
 * upnp_transport_accept() returns a connected fd, or -1 if nothing connected
 * before the timeout (and on error -- the caller retries either way). It does
 * NOT use 0 as the "nothing yet" sentinel: 0 is a perfectly good fd when
 * LWIP_SOCKET_OFFSET is 0, and treating it as a miss would drop a connection.
 */
int  upnp_transport_listen(uint16_t port);
int  upnp_transport_accept(int listen_fd, uint32_t timeout_ms);
int  upnp_transport_recv(int fd, char *buf, size_t size, uint32_t timeout_ms);
int  upnp_transport_send(int fd, const char *buf, size_t len);
void upnp_transport_close(int fd);

#endif /* UPNP_TRANSPORT_H */
