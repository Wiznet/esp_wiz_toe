/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * DHCP client + DNS resolver, as component API.
 *
 * The protocol engine used to live in examples/dhcp_dns. It is library code, not
 * example code: it speaks RFC 2131 and RFC 1035 over the BSD socket vtable in
 * net_sock_ops.h, so the TOE/LwIP choice is made by the LINKER and the same
 * source is correct on both backends -- there is no #if in the engine.
 *
 *   net_eth_ops  = plain lwip_*  -> --wrap sends them to the W5500 hardware
 *                                   sockets (TOE), or they are software LwIP
 *                                   over esp_eth (ETH backend).
 *
 * Because the engine opens its sockets through that vtable, the DHCP socket is
 * allocated by the SAME allocator as every application socket
 * (wiztoe_socket()), so it can never collide with one. That is the reason this
 * engine is preferred over the ioLibrary DHCP/DNS clients, which take a raw
 * hardware socket NUMBER (DHCP_init(uint8_t s, ...)) and open it behind the
 * allocator's back.
 *
 * What stayed in the example is policy: whether to run this from a FreeRTOS
 * task or poll it from an application main loop, which hostname to resolve and
 * when, and how to log.
 *
 * POLLING vs BLOCKING -- read this before choosing recv_timeout_ms
 * ---------------------------------------------------------------
 * wsm_dhcp_poll() waits for a datagram with the socket's receive timeout. A
 * dedicated task can afford to block there; an application main loop cannot.
 *   recv_timeout_ms > 0 : blocking socket, SO_RCVTIMEO. One poll can occupy the
 *                         caller for up to recv_timeout_ms.
 *   recv_timeout_ms = 0 : O_NONBLOCK. Poll returns immediately when nothing has
 *                         arrived. This is what a main-loop caller wants.
 * The engine needs no other change for either mode: it already treats a failed
 * receive as "nothing yet" and returns WSM_DHCP_PENDING.
 *
 * Note 0 means non-blocking HERE, not "wait forever" -- do not pass 0 straight
 * through to SO_RCVTIMEO, where the TOE reads it as an infinite wait.
 */
#ifndef WSM_DHCP_H
#define WSM_DHCP_H

#include "sdkconfig.h"

/* The implementation is built only for TOE + socket wrap (see CMakeLists.txt):
 * the engine's sockets are ordinary lwip_* calls that have to land on the TOE
 * allocator. Including this header in any other configuration would compile and
 * then fail at link with an undefined reference, which is a poor way to learn
 * it. Say so here instead.
 *
 * With the esp_eth backend the interface is a real esp_netif, so use ESP-IDF's
 * own DHCP client (esp_netif_dhcpc_start) and resolver rather than this one. */
#if !CONFIG_WSM_DRIVER_SOCKET_WRAP
#error "wsm_dhcp.h needs CONFIG_WSM_DRIVER_SOCKET_WRAP (TOE backend). " \n       "On the esp_eth backend use esp_netif_dhcpc_start() instead."
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "net_sock_ops.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Defaults. Every one of them is overridable per client; they are the values
 * examples/dhcp_dns used as compile-time constants. */
#define WSM_DHCP_BUF_SIZE          (1024 * 2)  /* shared DHCP/DNS message buffer */
#define WSM_DHCP_XMIT_TRIES        4           /* transmits before a round fails */
#define WSM_DHCP_XMIT_INTERVAL_MS  2000        /* base retransmit spacing (x1, x2, ...) */
#define WSM_DHCP_STEP_PACKETS      4           /* datagrams handled per poll */
#define WSM_DHCP_DEFAULT_RENEW_S   1800        /* T1 when the server sends no lease time */
#define WSM_DNS_TIMEOUT_MS         3000        /* wait for the A record */

/* Outcome of one poll of the DHCP client. */
typedef enum {
    WSM_DHCP_PENDING = 0,   /* still negotiating -- keep polling */
    WSM_DHCP_LEASED,        /* address in hand; `info` has been filled in */
    WSM_DHCP_FAILED,        /* a full retransmission round expired */
    WSM_DHCP_CONFLICT,      /* the offered address is already in use */
} wsm_dhcp_status_t;

/* Leased IPv4 identity, in the byte-array form wiz_NetInfo uses. */
typedef struct {
    uint8_t  ip[4];
    uint8_t  sn[4];
    uint8_t  gw[4];
    uint8_t  dns[4];
    uint32_t lease_s;       /* 0 when the server sends no lease-time option */
} wsm_dhcp_netinfo_t;

/*
 * The two operations a BSD socket cannot perform, so they stay per-interface:
 * reading the interface identity, and installing a lease INTO the stack that
 * owns the interface (wizchip_setnetinfo() on TOE, esp_netif_set_ip_info() on a
 * LwIP netif). Everything else the engine does goes through `sock`.
 */
typedef struct {
    const net_sock_ops_t *sock;

    /* Fills `mac` with the chaddr to advertise and `ifname` with the LwIP netif
     * name to pin the socket to (empty when the stack has no such concept, as
     * on the TOE where the chip IS the interface). Also clears the interface's
     * current address so DISCOVER goes out from 0.0.0.0 as RFC 2131 wants, and
     * stops any DHCP client the stack runs by itself. */
    void (*prepare)(uint8_t mac[6], char *ifname, size_t ifname_len);

    /* Install a fresh lease into the stack that owns this interface. */
    void (*apply_lease)(const wsm_dhcp_netinfo_t *info);
} wsm_dhcp_ops_t;

/* WIZnet chip ops: identity from the chip's own registers (TOE backend).
 * Only defined when the TOE backend is selected. */
extern const wsm_dhcp_ops_t wsm_dhcp_eth_ops;

typedef struct wsm_dhcp_client wsm_dhcp_client_t;

typedef struct {
    /* NULL -> &wsm_dhcp_eth_ops. */
    const wsm_dhcp_ops_t *ops;
    /* 0 -> O_NONBLOCK (see the header comment). */
    uint32_t recv_timeout_ms;
    /* 0 -> WSM_DHCP_STEP_PACKETS. */
    uint8_t  step_packets;
    /* 0 -> WSM_DHCP_XMIT_TRIES. */
    uint8_t  xmit_tries;
    /* 0 -> WSM_DHCP_XMIT_INTERVAL_MS. */
    uint32_t xmit_interval_ms;
} wsm_dhcp_config_t;

/* Allocate a client. `cfg` may be NULL for all defaults. The client owns a
 * WSM_DHCP_BUF_SIZE buffer, so it is heap-allocated rather than a caller stack
 * object. No socket is opened yet. */
esp_err_t wsm_dhcp_client_create(const wsm_dhcp_config_t *cfg, wsm_dhcp_client_t **out);
void      wsm_dhcp_client_destroy(wsm_dhcp_client_t *c);

/* prepare() + open the UDP socket on port 68 and arm the first DISCOVER.
 * Call only once the link is up. Consumes ONE hardware socket until stop(). */
esp_err_t wsm_dhcp_start(wsm_dhcp_client_t *c);

/* One poll. Drives DISCOVER/OFFER/REQUEST/ACK and, once bound, the T1 renewal.
 * Safe to call every main-loop iteration when recv_timeout_ms is 0.
 * `info` is written only when the return value is WSM_DHCP_LEASED. */
wsm_dhcp_status_t wsm_dhcp_poll(wsm_dhcp_client_t *c, wsm_dhcp_netinfo_t *info);

/* Close the socket and forget the lease. Releases the hardware socket. */
void wsm_dhcp_stop(wsm_dhcp_client_t *c);

/* True once a lease has been obtained and not since stopped. */
bool wsm_dhcp_is_bound(const wsm_dhcp_client_t *c);

/* The current lease. ESP_ERR_INVALID_STATE before the first ACK. */
esp_err_t wsm_dhcp_get_netinfo(const wsm_dhcp_client_t *c, wsm_dhcp_netinfo_t *out);

/*
 * One-shot A-record lookup. Opens a UDP socket, sends the query, waits up to
 * timeout_ms for the answer, and closes the socket on every path -- so it
 * consumes one hardware socket only for the duration of the call.
 *
 * BLOCKS for up to timeout_ms. Do not call it from a latency-sensitive main
 * loop; use a task, or accept the stall.
 *
 * `sock`   : NULL -> &net_eth_ops.
 * `server` : the DNS server, in the same byte order as wsm_dhcp_netinfo_t.dns.
 * `timeout_ms` : 0 -> WSM_DNS_TIMEOUT_MS.
 */
esp_err_t wsm_dns_resolve(const net_sock_ops_t *sock, const uint8_t server[4],
                          const char *hostname, uint32_t timeout_ms, uint8_t out_ip[4]);

#ifdef __cplusplus
}
#endif

#endif /* WSM_DHCP_H */
