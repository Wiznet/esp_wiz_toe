/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * Backend-neutral DHCP + DNS engine.
 *
 * Like loopback.c, every network call in this file goes through a BSD socket
 * vtable (dhcp_sock_ops_t), so ONE copy of the protocol logic drives both
 * backends and the choice is made at LINK time:
 *
 *   ops->sock->sendto(...)          TOE backend  -> __wrap_lwip_sendto -> wiztoe_sendto
 *                                   ETH backend  -> lwip_sendto (software LwIP)
 *
 * That is why the ioLibrary DHCP/DNS clients (DHCP_run()/DNS_run()) are NOT used
 * here: they bypass lwip_* entirely and speak to the chip's registers, so --wrap
 * has nothing to intercept and the same source could not serve both backends.
 * Instead this file implements the client side of RFC 2131 (DHCP) and the A-record
 * half of RFC 1035 (DNS) directly on top of UDP sockets.
 *
 * The component now ships the same engine as library code, in wsm_dhcp.h
 * (wsm_dhcp_client_create/start/poll, wsm_dns_resolve) -- prefer that in an
 * application. This example keeps its own copy on purpose: wsm_dhcp.c is built
 * only for TOE + socket wrap, while this example is meant to run unchanged on
 * the esp_eth backend and on Wi-Fi as well, which is exactly the comparison it
 * exists to show.
 *
 * Two things a socket cannot express stay in dhcp_dns_ops_t (see dhcp_dns.h):
 * prepare() supplies the chaddr / netif name, apply_lease() installs the result
 * into whichever stack owns the interface.
 *
 * Simplifications, deliberate for an example:
 *   - Renewal restarts from DHCPDISCOVER at T1 instead of unicasting a
 *     DHCPREQUEST in RENEWING state. Servers hand back the same address.
 *   - DHCP_DNS_CONFLICT is never returned: the RFC's duplicate-address check is
 *     an ARP probe, and there is no portable way to send one through a BSD
 *     socket. (The ioLibrary client could only do it by poking the chip.)
 */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"

#include "lwip/sockets.h"

#include "dhcp_dns.h"
#include "net_config.h"

static const char *TAG = "dhcp_dns";

/* Ethernet socket vtable: the plain lwIP BSD entry points. With
 * WSM_DRIVER_SOCKET_WRAP=1 these symbols are --wrap-redirected to the WIZnet
 * hardware sockets; with =0 they are software LwIP over esp_eth. Naming lwip_*
 * is correct in BOTH cases, so this needs no #if — writing __wrap_lwip_*
 * explicitly would instead fail to link when the wrap is off. (The Wi-Fi vtable,
 * which must bypass the wrap, is in netif_dhcp_dns.c.) */
const dhcp_sock_ops_t dhcp_lwip_ops = {
    .socket = lwip_socket,   .bind = lwip_bind,
    .sendto = lwip_sendto,   .recvfrom = lwip_recvfrom,
    .setsockopt = lwip_setsockopt,
    .close = lwip_close,
};

/* ========================================================================== */
/* RFC 2131 wire format                                                        */
/* ========================================================================== */

#define DHCP_CLIENT_PORT    68
#define DHCP_SERVER_PORT    67

#define DHCP_OP_REQUEST     1
#define DHCP_OP_REPLY       2
#define DHCP_HTYPE_ETHER    1
#define DHCP_HLEN_ETHER     6
#define DHCP_FLAG_BROADCAST 0x8000

/* Message types (option 53). */
#define DHCPDISCOVER        1
#define DHCPOFFER           2
#define DHCPREQUEST         3
#define DHCPACK             5
#define DHCPNAK             6

/* Options used here. */
#define OPT_PAD             0
#define OPT_SUBNET          1
#define OPT_ROUTER          3
#define OPT_DNS             6
#define OPT_REQUESTED_IP    50
#define OPT_LEASE_TIME      51
#define OPT_MSG_TYPE        53
#define OPT_SERVER_ID       54
#define OPT_PARAM_REQ       55
#define OPT_CLIENT_ID       61
#define OPT_END             255

/* Fixed-format part: 236 bytes of BOOTP header + the 4-byte magic cookie. */
#define DHCP_FIXED_LEN      240
#define DHCP_OFF_XID        4
#define DHCP_OFF_FLAGS      10
#define DHCP_OFF_YIADDR     16
#define DHCP_OFF_CHADDR     28
/* Pad short messages out to the 300-byte BOOTP minimum some servers insist on. */
#define DHCP_MIN_LEN        300

/* ========================================================================== */
/* Client state                                                                */
/* ========================================================================== */

typedef enum {
    ST_SELECTING = 0,   /* DHCPDISCOVER sent, waiting for a DHCPOFFER */
    ST_REQUESTING,      /* DHCPREQUEST sent, waiting for a DHCPACK */
    ST_BOUND,           /* leased; idle until T1 */
} dhcp_state_t;

typedef struct {
    const dhcp_dns_ops_t *ops;
    int                   fd;
    dhcp_state_t          state;
    uint32_t              xid;
    uint8_t               mac[6];
    char                  ifname[8];      /* "" when the stack has no netif name */
    uint8_t               server_id[4];
    uint8_t               offered[4];
    int                   tries;          /* retransmissions in the current round */
    int64_t               next_tx_us;
    int64_t               renew_at_us;
    dhcp_dns_netinfo_t    lease;
    uint8_t               buf[DHCP_DNS_BUF_SIZE];
} dhcp_client_t;

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

/* Pin a socket to one netif so that a 255.255.255.255 send leaves through THIS
 * interface (LwIP would otherwise route broadcasts to netif_default) and so that
 * incoming broadcasts from the other interface are filtered out. Best effort:
 * the TOE has no netif to name, and its --wrap accepts the option as a no-op. */
static void sock_bind_iface(const dhcp_sock_ops_t *sk, int fd, const char *ifname)
{
    if (ifname == NULL || ifname[0] == '\0') {
        return;
    }
    struct ifreq ifr = {0};
    size_t n = strlen(ifname);
    if (n > sizeof(ifr.ifr_name) - 1) {
        n = sizeof(ifr.ifr_name) - 1;       /* ifr_name is IFNAMSIZ, "st1"-sized */
    }
    memcpy(ifr.ifr_name, ifname, n);
    sk->setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, &ifr, sizeof(ifr));
}

static void sock_set_rcvtimeo(const dhcp_sock_ops_t *sk, int fd, uint32_t ms)
{
    struct timeval tv = {0};
    tv.tv_sec  = (long)(ms / 1000);
    tv.tv_usec = (long)((ms % 1000) * 1000);
    sk->setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

/* ========================================================================== */
/* DHCP                                                                        */
/* ========================================================================== */

static void dhcp_enter_selecting(dhcp_client_t *c)
{
    c->state      = ST_SELECTING;
    c->tries      = 0;
    c->next_tx_us = 0;                  /* transmit on the next step */
    c->xid        = esp_random();
    memset(c->server_id, 0, sizeof(c->server_id));
    memset(c->offered, 0, sizeof(c->offered));
}

static bool dhcp_open(dhcp_client_t *c)
{
    const dhcp_sock_ops_t *sk = c->ops->sock;

    int fd = sk->socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        return false;
    }

    int one = 1;
    sk->setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
    /* Ethernet and Wi-Fi both want port 68. On one shared LwIP stack (ETH
     * backend) that is only legal with SO_REUSEADDR; each socket then sees the
     * other's broadcasts too, which the xid/chaddr check below discards. */
    sk->setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sock_set_rcvtimeo(sk, fd, DHCP_RECV_TIMEOUT_MS);
    sock_bind_iface(sk, fd, c->ifname);

    struct sockaddr_in me = {
        .sin_family = AF_INET,
        .sin_port   = htons(DHCP_CLIENT_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (sk->bind(fd, (struct sockaddr *)&me, sizeof(me)) < 0) {
        sk->close(fd);
        return false;
    }

    c->fd = fd;
    return true;
}

static size_t dhcp_build(dhcp_client_t *c, uint8_t type, uint8_t *p)
{
    memset(p, 0, DHCP_FIXED_LEN);

    p[0] = DHCP_OP_REQUEST;
    p[1] = DHCP_HTYPE_ETHER;
    p[2] = DHCP_HLEN_ETHER;
    p[3] = 0;                                    /* hops */

    p[DHCP_OFF_XID + 0] = (uint8_t)(c->xid >> 24);
    p[DHCP_OFF_XID + 1] = (uint8_t)(c->xid >> 16);
    p[DHCP_OFF_XID + 2] = (uint8_t)(c->xid >> 8);
    p[DHCP_OFF_XID + 3] = (uint8_t)(c->xid);

    /* Ask the server to broadcast its reply: until the lease is installed the
     * interface does not own the offered address, so a unicast to it would be
     * dropped (by the chip on TOE, by LwIP on ETH). */
    p[DHCP_OFF_FLAGS + 0] = (uint8_t)(DHCP_FLAG_BROADCAST >> 8);
    p[DHCP_OFF_FLAGS + 1] = (uint8_t)(DHCP_FLAG_BROADCAST & 0xFF);

    memcpy(p + DHCP_OFF_CHADDR, c->mac, 6);

    p[236] = 0x63; p[237] = 0x82; p[238] = 0x53; p[239] = 0x63;   /* magic cookie */

    size_t n = DHCP_FIXED_LEN;

    p[n++] = OPT_MSG_TYPE;  p[n++] = 1; p[n++] = type;

    p[n++] = OPT_CLIENT_ID; p[n++] = 7; p[n++] = DHCP_HTYPE_ETHER;
    memcpy(p + n, c->mac, 6); n += 6;

    if (type == DHCPREQUEST) {
        p[n++] = OPT_REQUESTED_IP; p[n++] = 4; memcpy(p + n, c->offered, 4);   n += 4;
        p[n++] = OPT_SERVER_ID;    p[n++] = 4; memcpy(p + n, c->server_id, 4); n += 4;
    }

    p[n++] = OPT_PARAM_REQ; p[n++] = 4;
    p[n++] = OPT_SUBNET; p[n++] = OPT_ROUTER; p[n++] = OPT_DNS; p[n++] = OPT_LEASE_TIME;

    p[n++] = OPT_END;

    while (n < DHCP_MIN_LEN) {
        p[n++] = OPT_PAD;
    }
    return n;
}

static void dhcp_send(dhcp_client_t *c, uint8_t type)
{
    size_t len = dhcp_build(c, type, c->buf);

    struct sockaddr_in to = {
        .sin_family = AF_INET,
        .sin_port   = htons(DHCP_SERVER_PORT),
        .sin_addr.s_addr = htonl(INADDR_BROADCAST),
    };
    c->ops->sock->sendto(c->fd, c->buf, len, 0, (struct sockaddr *)&to, sizeof(to));
}

/*
 * Returns the DHCP message type, or 0 if the datagram is not a reply to us.
 * The xid + chaddr check matters: with SO_REUSEADDR both interfaces' sockets
 * receive every port-68 broadcast on a shared LwIP stack.
 */
static uint8_t dhcp_parse(dhcp_client_t *c, const uint8_t *p, size_t len,
                          dhcp_dns_netinfo_t *out)
{
    if (len < DHCP_FIXED_LEN)                                       return 0;
    if (p[0] != DHCP_OP_REPLY)                                      return 0;
    if (be32(p + DHCP_OFF_XID) != c->xid)                           return 0;
    if (memcmp(p + DHCP_OFF_CHADDR, c->mac, 6) != 0)                return 0;
    if (p[236] != 0x63 || p[237] != 0x82 ||
        p[238] != 0x53 || p[239] != 0x63)                           return 0;

    uint8_t type = 0;
    memset(out, 0, sizeof(*out));
    memcpy(out->ip, p + DHCP_OFF_YIADDR, 4);                        /* yiaddr */

    size_t i = DHCP_FIXED_LEN;
    while (i < len) {
        uint8_t code = p[i++];
        if (code == OPT_PAD) {
            continue;
        }
        if (code == OPT_END || i >= len) {
            break;
        }
        uint8_t l = p[i++];
        if (i + l > len) {
            break;
        }
        switch (code) {
        case OPT_MSG_TYPE:   if (l >= 1) type = p[i];                     break;
        case OPT_SUBNET:     if (l >= 4) memcpy(out->sn,  p + i, 4);      break;
        case OPT_ROUTER:     if (l >= 4) memcpy(out->gw,  p + i, 4);      break;
        case OPT_DNS:        if (l >= 4) memcpy(out->dns, p + i, 4);      break;  /* first server */
        case OPT_LEASE_TIME: if (l >= 4) out->lease_s = be32(p + i);      break;
        case OPT_SERVER_ID:  if (l >= 4) memcpy(c->server_id, p + i, 4);  break;
        default: break;
        }
        i += l;
    }
    return type;
}

/*
 * One poll. Runs up to DHCP_STEP_PACKETS turns so a DISCOVER/OFFER/REQUEST/ACK
 * exchange can complete without waiting for the caller's next tick.
 */
static dhcp_dns_status_t dhcp_step(dhcp_client_t *c, dhcp_dns_netinfo_t *info)
{
    for (int turn = 0; turn < DHCP_STEP_PACKETS; turn++) {
        int64_t now = esp_timer_get_time();

        if (c->state == ST_BOUND) {
            if (now < c->renew_at_us) {
                *info = c->lease;
                return DHCP_DNS_LEASED;
            }
            dhcp_enter_selecting(c);                 /* T1 reached — renew */
        }

        if (now >= c->next_tx_us) {
            if (c->tries >= DHCP_XMIT_TRIES) {
                dhcp_enter_selecting(c);
                return DHCP_DNS_FAILED;              /* the engine counts it */
            }
            c->tries++;
            dhcp_send(c, (c->state == ST_SELECTING) ? DHCPDISCOVER : DHCPREQUEST);
            c->next_tx_us = now + (int64_t)DHCP_XMIT_INTERVAL_MS * 1000 * c->tries;
        }

        int n = c->ops->sock->recvfrom(c->fd, c->buf, sizeof(c->buf), 0, NULL, NULL);
        if (n <= 0) {
            return DHCP_DNS_PENDING;                 /* SO_RCVTIMEO expired */
        }

        dhcp_dns_netinfo_t got;
        uint8_t type = dhcp_parse(c, c->buf, (size_t)n, &got);
        if (type == 0) {
            continue;                                /* someone else's traffic */
        }

        if (type == DHCPNAK) {
            dhcp_enter_selecting(c);
            continue;
        }

        if (c->state == ST_SELECTING && type == DHCPOFFER) {
            memcpy(c->offered, got.ip, 4);
            c->state      = ST_REQUESTING;
            c->tries      = 0;
            c->next_tx_us = 0;                       /* REQUEST on the next turn */
            continue;
        }

        if (c->state == ST_REQUESTING && type == DHCPACK) {
            c->lease = got;
            c->ops->apply_lease(&c->lease);
            c->state = ST_BOUND;

            uint32_t t1 = c->lease.lease_s ? (c->lease.lease_s / 2) : DHCP_DEFAULT_RENEW_S;
            c->renew_at_us = esp_timer_get_time() + (int64_t)t1 * 1000000;

            *info = c->lease;
            return DHCP_DNS_LEASED;
        }
    }
    return DHCP_DNS_PENDING;
}

/* ========================================================================== */
/* DNS (RFC 1035 A query)                                                      */
/* ========================================================================== */

#define DNS_SERVER_PORT     53
#define DNS_TYPE_A          1
#define DNS_CLASS_IN        1
#define DNS_HEADER_LEN      12

/* Encode "www.wiznet.io" as 3www6wiznet2io0. Returns bytes written, or -1. */
static int dns_encode_name(uint8_t *p, size_t cap, const char *domain)
{
    size_t n = 0;
    const char *label = domain;

    for (;;) {
        const char *dot = strchr(label, '.');
        size_t l = dot ? (size_t)(dot - label) : strlen(label);
        if (l == 0 || l > 63 || n + 1 + l + 1 > cap) {
            return -1;
        }
        p[n++] = (uint8_t)l;
        memcpy(p + n, label, l);
        n += l;
        if (dot == NULL) {
            break;
        }
        label = dot + 1;
    }
    p[n++] = 0;
    return (int)n;
}

/* Advance past a (possibly compressed) name. Returns the next offset, or -1. */
static int dns_skip_name(const uint8_t *p, size_t len, size_t off)
{
    while (off < len) {
        uint8_t l = p[off];
        if (l == 0) {
            return (int)(off + 1);
        }
        if ((l & 0xC0) == 0xC0) {
            return (off + 2 <= len) ? (int)(off + 2) : -1;   /* pointer ends the name */
        }
        off += 1u + l;
    }
    return -1;
}

static bool dns_parse(const uint8_t *p, size_t len, uint16_t id, uint8_t out_ip[4])
{
    if (len < DNS_HEADER_LEN)                          return false;
    if ((((uint16_t)p[0] << 8) | p[1]) != id)          return false;
    if ((p[2] & 0x80) == 0)                            return false;   /* not a response */
    if ((p[3] & 0x0F) != 0)                            return false;   /* rcode */

    uint16_t qdcount = ((uint16_t)p[4] << 8) | p[5];
    uint16_t ancount = ((uint16_t)p[6] << 8) | p[7];

    int off = DNS_HEADER_LEN;
    for (uint16_t q = 0; q < qdcount; q++) {
        off = dns_skip_name(p, len, (size_t)off);
        if (off < 0 || (size_t)off + 4 > len) {
            return false;
        }
        off += 4;                                       /* QTYPE + QCLASS */
    }

    for (uint16_t a = 0; a < ancount; a++) {
        off = dns_skip_name(p, len, (size_t)off);
        if (off < 0 || (size_t)off + 10 > len) {
            return false;
        }
        uint16_t type   = ((uint16_t)p[off + 0] << 8) | p[off + 1];
        uint16_t cls    = ((uint16_t)p[off + 2] << 8) | p[off + 3];
        uint16_t rdlen  = ((uint16_t)p[off + 8] << 8) | p[off + 9];
        off += 10;
        if ((size_t)off + rdlen > len) {
            return false;
        }
        if (type == DNS_TYPE_A && cls == DNS_CLASS_IN && rdlen == 4) {
            memcpy(out_ip, p + off, 4);
            return true;
        }
        off += rdlen;                                   /* CNAME etc. — keep looking */
    }
    return false;
}

/*
 * Resolve `domain` by querying the leased DNS server directly over UDP. Both
 * backends come through here — on TOE the socket calls land on the chip, on ETH
 * they land on software LwIP. (getaddrinfo() would work on ETH but not on TOE:
 * it is not one of the --wrap'd symbols, so it would always reach LwIP.)
 */
static bool dns_resolve(dhcp_client_t *c, const uint8_t server[4],
                        const char *domain, uint8_t out_ip[4])
{
    const dhcp_sock_ops_t *sk = c->ops->sock;

    if ((server[0] | server[1] | server[2] | server[3]) == 0) {
        ESP_LOGW(TAG, "no DNS server in the lease");
        return false;
    }

    int fd = sk->socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        return false;
    }
    sock_set_rcvtimeo(sk, fd, DNS_RECV_TIMEOUT_MS);
    sock_bind_iface(sk, fd, c->ifname);

    uint16_t id = (uint16_t)esp_random();
    uint8_t *p = c->buf;

    p[0] = (uint8_t)(id >> 8); p[1] = (uint8_t)id;
    p[2] = 0x01; p[3] = 0x00;                   /* standard query, recursion desired */
    p[4] = 0;    p[5] = 1;                      /* QDCOUNT = 1 */
    memset(p + 6, 0, 6);                        /* AN/NS/AR COUNT = 0 */

    int nl = dns_encode_name(p + DNS_HEADER_LEN, sizeof(c->buf) - DNS_HEADER_LEN - 4, domain);
    if (nl < 0) {
        sk->close(fd);
        return false;
    }
    size_t n = DNS_HEADER_LEN + (size_t)nl;
    p[n++] = 0; p[n++] = DNS_TYPE_A;
    p[n++] = 0; p[n++] = DNS_CLASS_IN;

    struct sockaddr_in to = {
        .sin_family = AF_INET,
        .sin_port   = htons(DNS_SERVER_PORT),
    };
    memcpy(&to.sin_addr.s_addr, server, 4);

    bool ok = false;
    if (sk->sendto(fd, p, n, 0, (struct sockaddr *)&to, sizeof(to)) > 0) {
        int r = sk->recvfrom(fd, c->buf, sizeof(c->buf), 0, NULL, NULL);
        if (r > 0) {
            ok = dns_parse(c->buf, (size_t)r, id, out_ip);
        }
    }
    sk->close(fd);
    return ok;
}

/* ========================================================================== */
/* Engine                                                                      */
/* ========================================================================== */

static void print_netinfo(const char *name, const dhcp_dns_netinfo_t *n)
{
    ESP_LOGI(TAG, "[%s] ip  : %u.%u.%u.%u", name, n->ip[0], n->ip[1], n->ip[2], n->ip[3]);
    ESP_LOGI(TAG, "[%s] sn  : %u.%u.%u.%u", name, n->sn[0], n->sn[1], n->sn[2], n->sn[3]);
    ESP_LOGI(TAG, "[%s] gw  : %u.%u.%u.%u", name, n->gw[0], n->gw[1], n->gw[2], n->gw[3]);
    ESP_LOGI(TAG, "[%s] dns : %u.%u.%u.%u", name, n->dns[0], n->dns[1], n->dns[2], n->dns[3]);
    if (n->lease_s) {
        ESP_LOGI(TAG, "[%s] DHCP leased time : %u seconds", name, (unsigned)n->lease_s);
    }
}

/*
 * One pass of the example: DHCP until leased, then DNS once, then keep polling
 * DHCP forever so the lease is renewed. Returns only on a fatal condition (the
 * retry budget is spent, or the leased address conflicts), which ends the task.
 */
static void dhcp_dns_run(const char *name, dhcp_client_t *c, const char *domain)
{
    dhcp_dns_netinfo_t info = {0};
    uint8_t target_ip[4] = {0};
    uint8_t dhcp_retry = 0;
    uint8_t dns_retry = 0;
    bool leased = false;
    bool resolved = false;

    c->ops->prepare(c->mac, c->ifname, sizeof(c->ifname));
    dhcp_enter_selecting(c);

    if (!dhcp_open(c)) {
        ESP_LOGE(TAG, "[%s] cannot open the DHCP socket", name);
        return;
    }

    ESP_LOGI(TAG, "[%s] DHCP client running (%02x:%02x:%02x:%02x:%02x:%02x%s%s)", name,
             c->mac[0], c->mac[1], c->mac[2], c->mac[3], c->mac[4], c->mac[5],
             c->ifname[0] ? " on netif " : "", c->ifname);

    while (1) {
        switch (dhcp_step(c, &info)) {
        case DHCP_DNS_LEASED:
            if (!leased) {
                leased = true;
                dhcp_retry = 0;
                ESP_LOGI(TAG, "[%s] DHCP success", name);
                print_netinfo(name, &info);
            }
            break;

        case DHCP_DNS_FAILED:
            leased = false;
            dhcp_retry++;
            if (dhcp_retry > DHCP_DNS_RETRY_COUNT) {
                ESP_LOGE(TAG, "[%s] DHCP failed", name);
                c->ops->sock->close(c->fd);
                return;
            }
            ESP_LOGW(TAG, "[%s] DHCP timeout occurred and retry %u", name, dhcp_retry);
            break;

        case DHCP_DNS_CONFLICT:
            ESP_LOGE(TAG, "[%s] conflict IP from DHCP", name);
            c->ops->sock->close(c->fd);
            return;

        case DHCP_DNS_PENDING:
        default:
            break;
        }

        /* DNS runs once, after the first lease — it needs the leased server. */
        if (leased && !resolved) {
            if (dns_resolve(c, info.dns, domain, target_ip)) {
                resolved = true;
                ESP_LOGI(TAG, "[%s] DNS success", name);
                ESP_LOGI(TAG, "[%s] target domain : %s", name, domain);
                ESP_LOGI(TAG, "[%s] IP of target domain : %u.%u.%u.%u", name,
                         target_ip[0], target_ip[1], target_ip[2], target_ip[3]);
            } else {
                dns_retry++;
                if (dns_retry > DNS_RETRY_COUNT) {
                    ESP_LOGE(TAG, "[%s] DNS failed", name);
                    c->ops->sock->close(c->fd);
                    return;
                }
                ESP_LOGW(TAG, "[%s] DNS timeout occurred and retry %u", name, dns_retry);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* --------------------------------------------------------------------------
 * Task launcher: Ethernet and Wi-Fi are both started this way (same level).
 * ------------------------------------------------------------------------ */
typedef struct {
    const char           *name;
    const dhcp_dns_ops_t *ops;
    const char           *domain;
    bool                (*is_up)(void);
} dhcp_dns_ctx_t;

static void dhcp_dns_task(void *arg)
{
    dhcp_dns_ctx_t *ctx = (dhcp_dns_ctx_t *)arg;

    /* The client carries a DHCP_DNS_BUF_SIZE buffer — heap, not task stack. */
    dhcp_client_t *c = calloc(1, sizeof(*c));
    if (c == NULL) {
        ESP_LOGE(TAG, "[%s] out of memory for the DHCP client", ctx->name);
        free(ctx);
        vTaskDelete(NULL);
        return;
    }
    c->ops = ctx->ops;
    c->fd = -1;

    ESP_LOGI(TAG, "[%s] waiting for link...", ctx->name);
    while (!ctx->is_up()) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    dhcp_dns_run(ctx->name, c, ctx->domain);

    free(c);
    free(ctx);
    vTaskDelete(NULL);
}

void dhcp_dns_start(const char *name, const dhcp_dns_ops_t *ops,
                    const char *domain, bool (*is_up)(void))
{
    dhcp_dns_ctx_t *ctx = malloc(sizeof(*ctx));
    if (ctx == NULL) {
        ESP_LOGE(TAG, "[%s] out of memory", name);
        return;
    }
    ctx->name = name;
    ctx->ops = ops;
    ctx->domain = domain;
    ctx->is_up = is_up;

    if (xTaskCreate(dhcp_dns_task, name, 4096, ctx, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "[%s] xTaskCreate failed", name);
        free(ctx);
    }
}
