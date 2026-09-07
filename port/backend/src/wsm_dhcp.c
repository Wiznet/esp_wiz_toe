/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * DHCP client + DNS resolver (see include/wsm_dhcp.h for the contract).
 *
 * The protocol engine below was promoted from examples/dhcp_dns: it is library
 * code, not example code. What stayed in the example is policy -- running it
 * from a task, choosing a hostname, log formatting.
 *
 * Two things changed on the way in, both so a POLLED caller (an application
 * main loop, not a dedicated task) can use it:
 *
 *   1. the receive wait is per-client instead of a compile-time constant, and
 *      0 now means O_NONBLOCK rather than "wait forever" (sock_set_recv_mode);
 *   2. retransmit counts/intervals are per-client too.
 *
 * The engine logic itself is unchanged: it already returned WSM_DHCP_PENDING
 * when a receive produced nothing, which is exactly what a non-blocking socket
 * reports.
 *
 * Sockets come from the net_sock_ops_t vtable, so on the TOE backend they are
 * allocated by wiztoe_socket() -- the same allocator every application socket
 * uses. That is what keeps the DHCP socket from colliding with an application
 * one, and it is why the ioLibrary DHCP/DNS clients are NOT used here: those
 * take a raw hardware socket NUMBER and open it behind the allocator's back.
 */
#include "sdkconfig.h"

/* ORDER MATTERS -- ioLibrary BEFORE lwIP. wizchip_conf.h -> w5500.h defines
 * SOCK_STREAM/SOCK_DGRAM as the Sn_MR protocol values, unguarded; net_backend.h
 * drops those aliases so the lwIP/POSIX definitions win, but only if it is
 * reached first. */
#if !defined(CONFIG_WSM_DRIVER_BACKEND_ETH)
#include "net_backend.h"    /* wizchip_conf.h: wiz_NetInfo, wizchip_get/setnetinfo */
#endif

#include <fcntl.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"

#include "lwip/sockets.h"

#include "wsm_dhcp.h"

static const char *TAG = "wsm_dhcp";

/* ========================================================================== */
/* WIZnet chip identity ops (TOE backend)                                      */
/* ========================================================================== */

#if !defined(CONFIG_WSM_DRIVER_BACKEND_ETH)

static void eth_prepare(uint8_t mac[6], char *ifname, size_t ifname_len)
{
    /* The MAC is never leased; it is whatever bring-up wrote. Reading the whole
     * struct back also preserves the W6300 ipmode field. */
    wiz_NetInfo ni;
    wizchip_getnetinfo(&ni);
    memcpy(mac, ni.mac, 6);

    /* Drop whatever address the chip was seeded with so DISCOVER goes out from
     * 0.0.0.0, as RFC 2131 requires. */
    memset(ni.ip, 0, sizeof(ni.ip));
    memset(ni.sn, 0, sizeof(ni.sn));
    memset(ni.gw, 0, sizeof(ni.gw));
    ni.dhcp = NETINFO_DHCP;
    wizchip_setnetinfo(&ni);

    /* No LwIP netif to pin the socket to -- the chip IS the interface. */
    if (ifname_len > 0) {
        ifname[0] = '\0';
    }
}

static void eth_apply_lease(const wsm_dhcp_netinfo_t *info)
{
    wiz_NetInfo ni;
    wizchip_getnetinfo(&ni);            /* keeps the MAC (+ the W6300 ipmode) */

    memcpy(ni.ip,  info->ip,  sizeof(ni.ip));
    memcpy(ni.sn,  info->sn,  sizeof(ni.sn));
    memcpy(ni.gw,  info->gw,  sizeof(ni.gw));
    memcpy(ni.dns, info->dns, sizeof(ni.dns));
    ni.dhcp = NETINFO_DHCP;

    wizchip_setnetinfo(&ni);            /* the chip's TCP/IP now owns the lease */

    ESP_LOGI(TAG, "lease applied to the chip: %u.%u.%u.%u",
             info->ip[0], info->ip[1], info->ip[2], info->ip[3]);
}

const wsm_dhcp_ops_t wsm_dhcp_eth_ops = {
    .sock        = &net_eth_ops,
    .prepare     = eth_prepare,
    .apply_lease = eth_apply_lease,
};

#endif /* !CONFIG_WSM_DRIVER_BACKEND_ETH */

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

struct wsm_dhcp_client {
    const wsm_dhcp_ops_t *ops;
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
    wsm_dhcp_netinfo_t    lease;
    bool                  bound;            /* a lease has been applied at least once */
    uint32_t              recv_timeout_ms;  /* 0 = O_NONBLOCK; see wsm_dhcp.h */
    uint8_t               step_packets;
    uint8_t               xmit_tries;
    uint32_t              xmit_interval_ms;
    uint8_t               buf[WSM_DHCP_BUF_SIZE];
};

typedef struct wsm_dhcp_client dhcp_client_t;

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

/* Pin a socket to one netif so that a 255.255.255.255 send leaves through THIS
 * interface (LwIP would otherwise route broadcasts to netif_default) and so that
 * incoming broadcasts from the other interface are filtered out. Best effort:
 * the TOE has no netif to name, and its --wrap accepts the option as a no-op. */
static void sock_bind_iface(const net_sock_ops_t *sk, int fd, const char *ifname)
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

/* How the engine waits for a datagram.
 *   ms > 0 : blocking socket with SO_RCVTIMEO -- fine inside a dedicated task.
 *   ms = 0 : O_NONBLOCK -- what a polled main loop needs. Deliberately NOT
 *            SO_RCVTIMEO {0,0}, which the TOE reads as "wait forever". */
static void sock_set_recv_mode(const net_sock_ops_t *sk, int fd, uint32_t ms)
{
    if (ms == 0) {
        if (sk->fcntl != NULL) {
            sk->fcntl(fd, F_SETFL, O_NONBLOCK);
        }
        return;
    }
    struct timeval tv = { .tv_sec = (time_t)(ms / 1000), .tv_usec = (suseconds_t)((ms % 1000) * 1000) };
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
    const net_sock_ops_t *sk = c->ops->sock;

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
    sock_set_recv_mode(sk, fd, c->recv_timeout_ms);
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
                          wsm_dhcp_netinfo_t *out)
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
 * One poll. Runs up to step_packets turns so a DISCOVER/OFFER/REQUEST/ACK
 * exchange can complete without waiting for the caller's next tick.
 */
static wsm_dhcp_status_t dhcp_step(dhcp_client_t *c, wsm_dhcp_netinfo_t *info)
{
    for (int turn = 0; turn < c->step_packets; turn++) {
        int64_t now = esp_timer_get_time();

        if (c->state == ST_BOUND) {
            if (now < c->renew_at_us) {
                *info = c->lease;
                return WSM_DHCP_LEASED;
            }
            dhcp_enter_selecting(c);                 /* T1 reached — renew */
        }

        if (now >= c->next_tx_us) {
            if (c->tries >= c->xmit_tries) {
                dhcp_enter_selecting(c);
                return WSM_DHCP_FAILED;              /* the engine counts it */
            }
            c->tries++;
            dhcp_send(c, (c->state == ST_SELECTING) ? DHCPDISCOVER : DHCPREQUEST);
            c->next_tx_us = now + (int64_t)c->xmit_interval_ms * 1000 * c->tries;
        }

        int n = c->ops->sock->recvfrom(c->fd, c->buf, sizeof(c->buf), 0, NULL, NULL);
        if (n <= 0) {
            return WSM_DHCP_PENDING;                 /* SO_RCVTIMEO expired */
        }

        wsm_dhcp_netinfo_t got;
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
            c->bound = true;

            uint32_t t1 = c->lease.lease_s ? (c->lease.lease_s / 2) : WSM_DHCP_DEFAULT_RENEW_S;
            c->renew_at_us = esp_timer_get_time() + (int64_t)t1 * 1000000;

            *info = c->lease;
            return WSM_DHCP_LEASED;
        }
    }
    return WSM_DHCP_PENDING;
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
static bool dns_resolve(const net_sock_ops_t *sk, const uint8_t server[4],
                        const char *domain, uint32_t timeout_ms, uint8_t out_ip[4])
{
    /* One resolver at a time. A per-call 512 B stack buffer would be fine too;
     * static keeps wsm_dns_resolve() callable from a small task stack. */
    static uint8_t dnsbuf[512];

    if ((server[0] | server[1] | server[2] | server[3]) == 0) {
        ESP_LOGW(TAG, "no DNS server configured");
        return false;
    }

    int fd = sk->socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        return false;
    }
    sock_set_recv_mode(sk, fd, timeout_ms);

    uint16_t id = (uint16_t)esp_random();
    uint8_t *p = dnsbuf;

    p[0] = (uint8_t)(id >> 8); p[1] = (uint8_t)id;
    p[2] = 0x01; p[3] = 0x00;                   /* standard query, recursion desired */
    p[4] = 0;    p[5] = 1;                      /* QDCOUNT = 1 */
    memset(p + 6, 0, 6);                        /* AN/NS/AR COUNT = 0 */

    int nl = dns_encode_name(p + DNS_HEADER_LEN, sizeof(dnsbuf) - DNS_HEADER_LEN - 4, domain);
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
        int r = sk->recvfrom(fd, dnsbuf, sizeof(dnsbuf), 0, NULL, NULL);
        if (r > 0) {
            ok = dns_parse(dnsbuf, (size_t)r, id, out_ip);
        }
    }
    sk->close(fd);        /* every path closes: no hardware socket leak */
    return ok;
}

/* ========================================================================== */
/* Public API                                                                  */
/* ========================================================================== */

static const wsm_dhcp_ops_t *default_ops(void)
{
#if !defined(CONFIG_WSM_DRIVER_BACKEND_ETH)
    return &wsm_dhcp_eth_ops;
#else
    return NULL;    /* the ETH backend must supply its own esp_netif ops */
#endif
}

esp_err_t wsm_dhcp_client_create(const wsm_dhcp_config_t *cfg, wsm_dhcp_client_t **out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = NULL;

    const wsm_dhcp_ops_t *ops = (cfg != NULL && cfg->ops != NULL) ? cfg->ops : default_ops();
    if (ops == NULL || ops->sock == NULL || ops->prepare == NULL || ops->apply_lease == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* The client carries a WSM_DHCP_BUF_SIZE buffer -- heap, not caller stack. */
    struct wsm_dhcp_client *c = calloc(1, sizeof(*c));
    if (c == NULL) {
        return ESP_ERR_NO_MEM;
    }

    c->ops              = ops;
    c->fd               = -1;
    c->recv_timeout_ms  = (cfg != NULL) ? cfg->recv_timeout_ms : 0;
    c->step_packets     = (cfg != NULL && cfg->step_packets) ? cfg->step_packets
                                                             : WSM_DHCP_STEP_PACKETS;
    c->xmit_tries       = (cfg != NULL && cfg->xmit_tries) ? cfg->xmit_tries
                                                           : WSM_DHCP_XMIT_TRIES;
    c->xmit_interval_ms = (cfg != NULL && cfg->xmit_interval_ms) ? cfg->xmit_interval_ms
                                                                 : WSM_DHCP_XMIT_INTERVAL_MS;

    *out = c;
    return ESP_OK;
}

void wsm_dhcp_client_destroy(wsm_dhcp_client_t *c)
{
    if (c == NULL) {
        return;
    }
    wsm_dhcp_stop(c);
    free(c);
}

esp_err_t wsm_dhcp_start(wsm_dhcp_client_t *c)
{
    if (c == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (c->fd >= 0) {
        return ESP_OK;                      /* already running */
    }

    c->ops->prepare(c->mac, c->ifname, sizeof(c->ifname));
    dhcp_enter_selecting(c);
    c->bound = false;

    if (!dhcp_open(c)) {
        ESP_LOGE(TAG, "could not open the DHCP socket");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "DHCP started (fd %d, %s)", c->fd,
             c->recv_timeout_ms ? "blocking" : "non-blocking");
    return ESP_OK;
}

wsm_dhcp_status_t wsm_dhcp_poll(wsm_dhcp_client_t *c, wsm_dhcp_netinfo_t *info)
{
    wsm_dhcp_netinfo_t scratch;

    if (c == NULL || c->fd < 0) {
        return WSM_DHCP_PENDING;
    }
    return dhcp_step(c, info != NULL ? info : &scratch);
}

void wsm_dhcp_stop(wsm_dhcp_client_t *c)
{
    if (c == NULL || c->fd < 0) {
        return;
    }
    c->ops->sock->close(c->fd);             /* releases the hardware socket */
    c->fd    = -1;
    c->state = ST_SELECTING;
    c->bound = false;
    memset(&c->lease, 0, sizeof(c->lease));
}

bool wsm_dhcp_is_bound(const wsm_dhcp_client_t *c)
{
    return c != NULL && c->bound;
}

esp_err_t wsm_dhcp_get_netinfo(const wsm_dhcp_client_t *c, wsm_dhcp_netinfo_t *out)
{
    if (c == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!c->bound) {
        return ESP_ERR_INVALID_STATE;
    }
    *out = c->lease;
    return ESP_OK;
}

esp_err_t wsm_dns_resolve(const net_sock_ops_t *sock, const uint8_t server[4],
                          const char *hostname, uint32_t timeout_ms, uint8_t out_ip[4])
{
    if (server == NULL || hostname == NULL || out_ip == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (sock == NULL) {
        sock = &net_eth_ops;
    }
    if (timeout_ms == 0) {
        timeout_ms = WSM_DNS_TIMEOUT_MS;
    }
    return dns_resolve(sock, server, hostname, timeout_ms, out_ip) ? ESP_OK : ESP_FAIL;
}
