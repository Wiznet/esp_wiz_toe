/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * MQTT 3.1.1 client engine. Speaks the wire format directly over the BSD socket
 * API, reached through a vtable, so the engine is backend-neutral (see mqtt.h).
 *
 * The port from the ioLibrary MQTT client is mostly a subtraction:
 *   - mqtt_interface.c's Network struct (mqttread/mqttwrite bound to a hardware
 *     socket number) disappears: the connection is a plain fd from ops->socket();
 *   - MQTTClient.c's 1 ms MilliTimer_Handler esp_timer tick disappears too — the
 *     keep-alive deadlines are compared against esp_timer_get_time() when the
 *     loop comes round, and recv() is bounded by SO_RCVTIMEO instead of being
 *     polled;
 *   - the MQTTPacket serializer/deserializer collapses into the encode/decode
 *     helpers below, because this client only needs CONNECT, SUBSCRIBE, PUBLISH
 *     at QoS 0, PINGREQ and their acknowledgements.
 *
 * Only QoS 0 is published. A subscription is requested at QoS 0, so the broker
 * may not grant more; an inbound QoS 1 publish is still acknowledged in case a
 * broker ignores that.
 */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"          /* esp_timer_get_time (keep-alive deadlines) */
#include "esp_netif.h"          /* esp_ip4addr_aton (broker address parse) */

#include "mqtt.h"
#include "net_config.h"         /* MQTT_BUF_SIZE, MQTT_POLL_MS, ... */

static const char *TAG = "mqtt";

/* ---- MQTT 3.1.1 control packet types (MQTT-2.2.1) ------------------------ */
#define MQTT_PKT_CONNECT      1
#define MQTT_PKT_CONNACK      2
#define MQTT_PKT_PUBLISH      3
#define MQTT_PKT_PUBACK       4
#define MQTT_PKT_SUBSCRIBE    8
#define MQTT_PKT_SUBACK       9
#define MQTT_PKT_UNSUBACK    11
#define MQTT_PKT_PINGREQ     12
#define MQTT_PKT_PINGRESP    13

#define MQTT_PROTOCOL_LEVEL   0x04      /* 3.1.1 */

/* CONNECT flag bits (MQTT-3.1.2.3) */
#define CONNECT_CLEAN_SESSION 0x02
#define CONNECT_PASSWORD      0x40
#define CONNECT_USERNAME      0x80

#define SUBACK_FAILURE        0x80      /* granted-QoS byte meaning "refused" */

/* Longest fixed header this client emits: type byte + 4 remaining-length bytes. */
#define MQTT_FIXED_HDR_MAX    5

/* Everything one client instance owns. One of these per interface, so the
 * Ethernet and Wi-Fi clients share no state at all. */
typedef struct {
    const char           *name;
    const net_sock_ops_t *ops;
    mqtt_config_t         cfg;      /* copied; the strings inside are not */
    bool                (*is_up)(void);

    int       fd;
    uint16_t  next_pkt_id;
    uint32_t  last_tx_ms;           /* drives PINGREQ */
    uint32_t  last_rx_ms;           /* drives the "broker went silent" check */
    uint8_t  *txbuf;                /* packet under construction */
    uint8_t  *rxbuf;                /* body of the packet just read */
    size_t    cap;                  /* capacity of each buffer */
} mqtt_ctx_t;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* ---- wire encoding ------------------------------------------------------- */

/* Remaining Length is a 1..4 byte base-128 varint (MQTT-2.2.3). */
static size_t enc_remaining_len(uint8_t *p, size_t len)
{
    size_t i = 0;

    do {
        uint8_t b = (uint8_t)(len % 128);
        len /= 128;
        if (len > 0) {
            b |= 0x80;
        }
        p[i++] = b;
    } while (len > 0);
    return i;
}

/* UTF-8 string: 2-byte big-endian length, then the bytes (MQTT-1.5.3). */
static size_t enc_str(uint8_t *p, const char *s)
{
    size_t n = strlen(s);

    p[0] = (uint8_t)(n >> 8);
    p[1] = (uint8_t)(n & 0xFF);
    memcpy(p + 2, s, n);
    return n + 2;
}

static size_t str_field_len(const char *s)
{
    return 2 + strlen(s);
}

/* ---- raw socket I/O ------------------------------------------------------ */

/* Both backends report a SO_RCVTIMEO expiry this way: -1 with EWOULDBLOCK (the
 * TOE maps its own WIZTOE_ERR_TIMEOUT to it in wiztoe_wrap.c). */
static bool errno_is_timeout(void)
{
    return errno == EWOULDBLOCK || errno == EAGAIN;
}

/* send() may take less than asked on either backend, so loop.
 *
 * errno_is_timeout() covers the send side too: on the TOE a blocking send()
 * reports -1/EWOULDBLOCK while the chip still has an unacknowledged
 * transmission in flight (SOCK_BUSY), and LwIP does the same for an elapsed
 * SO_SNDTIMEO. Neither is a broken connection, and treating them as one would
 * tear down the session and reconnect over a condition that clears itself. */
static bool send_all(mqtt_ctx_t *c, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    size_t off = 0;

    while (off < len) {
        int w = c->ops->send(c->fd, p + off, len - off, 0);
        if (w > 0) {
            off += (size_t)w;
            continue;
        }
        if (w < 0 && errno_is_timeout()) {
            vTaskDelay(1);
            continue;
        }
        ESP_LOGW(TAG, "[%s] send failed: errno %d", c->name, errno);
        return false;
    }
    c->last_tx_ms = now_ms();
    return true;
}

typedef enum {
    RD_OK      =  0,
    RD_TIMEOUT =  1,        /* SO_RCVTIMEO expired — the peer is just quiet */
    RD_ERROR   = -1,        /* EOF or a real error — the session is over */
} rd_t;

static rd_t recv_some(mqtt_ctx_t *c, void *buf, size_t len, int *out_n)
{
    int n = c->ops->recv(c->fd, buf, len, 0);

    if (n > 0) {
        *out_n = n;
        c->last_rx_ms = now_ms();
        return RD_OK;
    }
    if (n == 0) {
        return RD_ERROR;                    /* peer closed */
    }
    return errno_is_timeout() ? RD_TIMEOUT : RD_ERROR;
}

/* Read exactly `len` bytes. A quiet gap inside a packet is retried until
 * `timeout_ms` has elapsed: half a packet cannot be resumed later, so a timeout
 * here ends the session. */
static rd_t read_exact(mqtt_ctx_t *c, void *buf, size_t len, uint32_t timeout_ms)
{
    uint8_t *p = (uint8_t *)buf;
    size_t off = 0;
    uint32_t start = now_ms();

    while (off < len) {
        int n = 0;
        rd_t r = recv_some(c, p + off, len - off, &n);

        if (r == RD_ERROR) {
            return RD_ERROR;
        }
        if (r == RD_TIMEOUT) {
            if ((uint32_t)(now_ms() - start) >= timeout_ms) {
                return RD_TIMEOUT;
            }
            continue;
        }
        off += (size_t)n;
    }
    return RD_OK;
}

/* Throw away `len` bytes of a packet this client cannot hold. */
static rd_t drop_bytes(mqtt_ctx_t *c, size_t len)
{
    while (len > 0) {
        size_t chunk = (len < c->cap) ? len : c->cap;
        rd_t r = read_exact(c, c->rxbuf, chunk, MQTT_ACK_TIMEOUT_MS);

        if (r != RD_OK) {
            return r;
        }
        len -= chunk;
    }
    return RD_OK;
}

static int read_remaining_len(mqtt_ctx_t *c, size_t *out)
{
    size_t value = 0;
    size_t mult = 1;

    for (int i = 0; i < 4; i++) {          /* 4 bytes is the maximum (MQTT-2.2.3) */
        uint8_t b;
        if (read_exact(c, &b, 1, MQTT_ACK_TIMEOUT_MS) != RD_OK) {
            return -1;
        }
        value += (size_t)(b & 0x7F) * mult;
        if ((b & 0x80) == 0) {
            *out = value;
            return 0;
        }
        mult *= 128;
    }
    return -1;                              /* malformed */
}

/*
 * Read one control packet. Returns:
 *    1  packet read: *type / *flags set, body in c->rxbuf, length in *body_len
 *    0  nothing arrived within SO_RCVTIMEO (idle — not an error)
 *   -1  the session is over (EOF, error, or a malformed packet)
 */
static int read_packet(mqtt_ctx_t *c, uint8_t *type, uint8_t *flags, size_t *body_len)
{
    uint8_t hdr;
    size_t rem = 0;

    rd_t r = read_exact(c, &hdr, 1, 0);     /* one SO_RCVTIMEO window, no retry */
    if (r == RD_TIMEOUT) {
        return 0;
    }
    if (r == RD_ERROR) {
        return -1;
    }

    if (read_remaining_len(c, &rem) != 0) {
        ESP_LOGW(TAG, "[%s] malformed remaining length", c->name);
        return -1;
    }

    *type  = (uint8_t)(hdr >> 4);
    *flags = (uint8_t)(hdr & 0x0F);

    if (rem > c->cap) {
        /* Too big to inspect, but the stream stays in sync if it is consumed. */
        ESP_LOGW(TAG, "[%s] dropping %u-byte packet (buffer is %u)",
                 c->name, (unsigned)rem, (unsigned)c->cap);
        return (drop_bytes(c, rem) == RD_OK) ? 0 : -1;
    }
    if (rem > 0 && read_exact(c, c->rxbuf, rem, MQTT_ACK_TIMEOUT_MS) != RD_OK) {
        ESP_LOGW(TAG, "[%s] truncated packet body", c->name);
        return -1;
    }

    *body_len = rem;
    return 1;
}

/* ---- outgoing packets ---------------------------------------------------- */

static bool send_connect(mqtt_ctx_t *c)
{
    const mqtt_config_t *cfg = &c->cfg;
    bool has_user = (cfg->username != NULL) && (cfg->username[0] != '\0');
    /* A password without a username is forbidden (MQTT-3.1.2-22). */
    bool has_pass = has_user && (cfg->password != NULL) && (cfg->password[0] != '\0');

    size_t rem = 10                                     /* "MQTT" + level + flags + keep-alive */
               + str_field_len(cfg->client_id)
               + (has_user ? str_field_len(cfg->username) : 0)
               + (has_pass ? str_field_len(cfg->password) : 0);

    if (MQTT_FIXED_HDR_MAX + rem > c->cap) {
        ESP_LOGE(TAG, "[%s] CONNECT does not fit in %u bytes", c->name, (unsigned)c->cap);
        return false;
    }

    uint8_t *p = c->txbuf;
    *p++ = (uint8_t)(MQTT_PKT_CONNECT << 4);
    p += enc_remaining_len(p, rem);
    p += enc_str(p, "MQTT");
    *p++ = MQTT_PROTOCOL_LEVEL;
    *p++ = (uint8_t)(CONNECT_CLEAN_SESSION |
                     (has_user ? CONNECT_USERNAME : 0) |
                     (has_pass ? CONNECT_PASSWORD : 0));
    *p++ = (uint8_t)(cfg->keepalive_s >> 8);
    *p++ = (uint8_t)(cfg->keepalive_s & 0xFF);
    p += enc_str(p, cfg->client_id);
    if (has_user) {
        p += enc_str(p, cfg->username);
    }
    if (has_pass) {
        p += enc_str(p, cfg->password);
    }

    return send_all(c, c->txbuf, (size_t)(p - c->txbuf));
}

static bool send_subscribe(mqtt_ctx_t *c, uint16_t pkt_id)
{
    size_t rem = 2 + str_field_len(c->cfg.sub_topic) + 1;   /* id + filter + QoS */

    if (MQTT_FIXED_HDR_MAX + rem > c->cap) {
        ESP_LOGE(TAG, "[%s] SUBSCRIBE does not fit in %u bytes", c->name, (unsigned)c->cap);
        return false;
    }

    uint8_t *p = c->txbuf;
    *p++ = (uint8_t)((MQTT_PKT_SUBSCRIBE << 4) | 0x02);     /* reserved bits (MQTT-3.8.1-1) */
    p += enc_remaining_len(p, rem);
    *p++ = (uint8_t)(pkt_id >> 8);
    *p++ = (uint8_t)(pkt_id & 0xFF);
    p += enc_str(p, c->cfg.sub_topic);
    *p++ = 0;                                               /* requested QoS 0 */

    return send_all(c, c->txbuf, (size_t)(p - c->txbuf));
}

/* QoS 0: no packet identifier, no acknowledgement. */
static bool send_publish(mqtt_ctx_t *c)
{
    size_t payload_len = strlen(c->cfg.pub_payload);
    size_t rem = str_field_len(c->cfg.pub_topic) + payload_len;

    if (MQTT_FIXED_HDR_MAX + rem > c->cap) {
        ESP_LOGE(TAG, "[%s] PUBLISH does not fit in %u bytes", c->name, (unsigned)c->cap);
        return false;
    }

    uint8_t *p = c->txbuf;
    *p++ = (uint8_t)(MQTT_PKT_PUBLISH << 4);                /* dup=0, QoS=0, retain=0 */
    p += enc_remaining_len(p, rem);
    p += enc_str(p, c->cfg.pub_topic);
    memcpy(p, c->cfg.pub_payload, payload_len);
    p += payload_len;

    return send_all(c, c->txbuf, (size_t)(p - c->txbuf));
}

static bool send_puback(mqtt_ctx_t *c, uint16_t pkt_id)
{
    uint8_t pkt[4] = {
        (uint8_t)(MQTT_PKT_PUBACK << 4), 2,
        (uint8_t)(pkt_id >> 8), (uint8_t)(pkt_id & 0xFF),
    };

    return send_all(c, pkt, sizeof(pkt));
}

static bool send_pingreq(mqtt_ctx_t *c)
{
    uint8_t pkt[2] = { (uint8_t)(MQTT_PKT_PINGREQ << 4), 0 };

    return send_all(c, pkt, sizeof(pkt));
}

/* ---- incoming packets ---------------------------------------------------- */

/* PUBLISH body: topic name, packet identifier when QoS > 0, then the payload. */
static void handle_publish(mqtt_ctx_t *c, uint8_t flags, size_t len)
{
    const uint8_t *p = c->rxbuf;
    uint8_t qos = (uint8_t)((flags >> 1) & 0x03);
    size_t off;
    size_t topic_len;
    uint16_t pkt_id = 0;

    if (len < 2) {
        ESP_LOGW(TAG, "[%s] short PUBLISH", c->name);
        return;
    }
    topic_len = ((size_t)p[0] << 8) | p[1];
    off = 2 + topic_len;
    if (off > len) {
        ESP_LOGW(TAG, "[%s] PUBLISH topic runs past the packet", c->name);
        return;
    }
    if (qos > 0) {
        if (off + 2 > len) {
            ESP_LOGW(TAG, "[%s] PUBLISH is missing its packet id", c->name);
            return;
        }
        pkt_id = (uint16_t)(((uint16_t)p[off] << 8) | p[off + 1]);
        off += 2;
    }

    ESP_LOGI(TAG, "[%s] %.*s : %.*s", c->name,
             (int)topic_len, (const char *)(p + 2),
             (int)(len - off), (const char *)(p + off));

    /* We only ever ask for QoS 0, so this is belt and braces. QoS 2 would need
     * PUBREC/PUBREL/PUBCOMP and is not offered. */
    if (qos == 1) {
        send_puback(c, pkt_id);
    } else if (qos == 2) {
        ESP_LOGW(TAG, "[%s] ignoring a QoS 2 PUBLISH (unsupported)", c->name);
    }
}

/*
 * Read packets until `want` shows up. Publishes that arrive first (a broker may
 * deliver a retained message before the SUBACK) are handled on the way through.
 * Returns 0 with the body in c->rxbuf, or -1 on timeout / lost session.
 */
static int wait_packet(mqtt_ctx_t *c, uint8_t want, size_t *body_len, uint32_t timeout_ms)
{
    uint32_t start = now_ms();

    for (;;) {
        uint8_t type = 0, flags = 0;
        size_t len = 0;
        int r = read_packet(c, &type, &flags, &len);

        if (r < 0) {
            return -1;
        }
        if (r > 0) {
            if (type == want) {
                *body_len = len;
                return 0;
            }
            if (type == MQTT_PKT_PUBLISH) {
                handle_publish(c, flags, len);
            }
        }
        if ((uint32_t)(now_ms() - start) >= timeout_ms) {
            return -1;
        }
    }
}

/* ---- session ------------------------------------------------------------- */

static bool tcp_connect(mqtt_ctx_t *c)
{
    const net_sock_ops_t *ops = c->ops;
    struct timeval tv = {
        .tv_sec  = MQTT_POLL_MS / 1000,
        .tv_usec = (MQTT_POLL_MS % 1000) * 1000,
    };
    struct sockaddr_in dst = {
        .sin_family = AF_INET,
        .sin_port = htons(c->cfg.broker_port),
    };
    int opt = 1;

    c->fd = ops->socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (c->fd < 0) {
        ESP_LOGE(TAG, "[%s] socket() failed: errno %d", c->name, errno);
        return false;
    }
    /* Bounds recv() so the loop always comes back to publish, ping, and notice
     * a broker that stopped answering. */
    ops->setsockopt(c->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ops->setsockopt(c->fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

    dst.sin_addr.s_addr = esp_ip4addr_aton(c->cfg.broker_ip);
    if (ops->connect(c->fd, (struct sockaddr *)&dst, sizeof(dst)) != 0) {
        ESP_LOGW(TAG, "[%s] connect to %s:%u failed: errno %d",
                 c->name, c->cfg.broker_ip, (unsigned)c->cfg.broker_port, errno);
        ops->close(c->fd);
        c->fd = -1;
        return false;
    }
    return true;
}

/* Bring one session up and run it until the connection is lost. Returns when
 * the caller should reconnect. */
static void mqtt_session(mqtt_ctx_t *c)
{
    const mqtt_config_t *cfg = &c->cfg;
    uint32_t keepalive_ms = (uint32_t)cfg->keepalive_s * 1000;
    uint32_t last_pub_ms;
    size_t len = 0;

    if (!tcp_connect(c)) {
        return;
    }
    c->last_tx_ms = c->last_rx_ms = now_ms();
    ESP_LOGI(TAG, "[%s] TCP connected to %s:%u",
             c->name, cfg->broker_ip, (unsigned)cfg->broker_port);

    if (!send_connect(c)) {
        goto out;
    }
    if (wait_packet(c, MQTT_PKT_CONNACK, &len, MQTT_ACK_TIMEOUT_MS) != 0 || len < 2) {
        ESP_LOGE(TAG, "[%s] no CONNACK from the broker", c->name);
        goto out;
    }
    if (c->rxbuf[1] != 0) {
        /* 1 = bad protocol version, 2 = client id rejected, 4 = bad credentials,
         * 5 = not authorized (MQTT-3.2.2.3). */
        ESP_LOGE(TAG, "[%s] broker refused CONNECT: return code %u",
                 c->name, (unsigned)c->rxbuf[1]);
        goto out;
    }
    ESP_LOGI(TAG, "[%s] MQTT connected as \"%s\"", c->name, cfg->client_id);

    if (cfg->sub_topic != NULL) {
        uint16_t id = c->next_pkt_id++;

        if (c->next_pkt_id == 0) {
            c->next_pkt_id = 1;     /* 0 is not a valid packet identifier */
        }
        if (!send_subscribe(c, id)) {
            goto out;
        }
        if (wait_packet(c, MQTT_PKT_SUBACK, &len, MQTT_ACK_TIMEOUT_MS) != 0 || len < 3) {
            ESP_LOGE(TAG, "[%s] no SUBACK for \"%s\"", c->name, cfg->sub_topic);
            goto out;
        }
        if (c->rxbuf[2] == SUBACK_FAILURE) {
            ESP_LOGE(TAG, "[%s] broker refused the subscription to \"%s\"",
                     c->name, cfg->sub_topic);
            goto out;
        }
        ESP_LOGI(TAG, "[%s] subscribed to \"%s\"", c->name, cfg->sub_topic);
    }

    last_pub_ms = now_ms();

    while (1) {
        uint8_t type = 0, flags = 0;
        size_t n = 0;
        uint32_t now;

        int r = read_packet(c, &type, &flags, &n);
        if (r < 0) {
            ESP_LOGW(TAG, "[%s] connection lost", c->name);
            break;
        }
        if (r > 0) {
            switch (type) {
            case MQTT_PKT_PUBLISH:
                handle_publish(c, flags, n);
                break;
            case MQTT_PKT_PINGRESP:
            case MQTT_PKT_PUBACK:
            case MQTT_PKT_SUBACK:
            case MQTT_PKT_UNSUBACK:
                break;              /* liveness only — recorded in last_rx_ms */
            default:
                ESP_LOGD(TAG, "[%s] ignoring packet type %u", c->name, (unsigned)type);
                break;
            }
        }

        now = now_ms();

        if (cfg->pub_topic != NULL &&
            (uint32_t)(now - last_pub_ms) >= cfg->pub_period_ms) {
            last_pub_ms = now;
            if (!send_publish(c)) {
                break;
            }
            ESP_LOGI(TAG, "[%s] published \"%s\" to %s",
                     c->name, cfg->pub_payload, cfg->pub_topic);
        }

        if (keepalive_ms > 0) {
            /* Ping at half the interval so one lost PINGREQ still leaves room
             * for a second before the broker drops us. */
            if ((uint32_t)(now - c->last_tx_ms) >= keepalive_ms / 2 && !send_pingreq(c)) {
                break;
            }
            /* Nothing at all from the broker for 1.5 keep-alive intervals means
             * the connection is dead even though the socket has not said so. */
            if ((uint32_t)(now - c->last_rx_ms) >= keepalive_ms + keepalive_ms / 2) {
                ESP_LOGW(TAG, "[%s] broker silent for %u ms — reconnecting",
                         c->name, (unsigned)(now - c->last_rx_ms));
                break;
            }
        }
    }

out:
    c->ops->close(c->fd);
    c->fd = -1;
}

/* ---- task launcher: same shape as examples/http and examples/loopback ---- */

static void mqtt_task(void *arg)
{
    mqtt_ctx_t *c = (mqtt_ctx_t *)arg;

    c->txbuf = malloc(MQTT_BUF_SIZE);
    c->rxbuf = malloc(MQTT_BUF_SIZE);
    if (c->txbuf == NULL || c->rxbuf == NULL) {
        ESP_LOGE(TAG, "[%s] out of memory for two %d-byte buffers",
                 c->name, MQTT_BUF_SIZE);
        free(c->txbuf);
        free(c->rxbuf);
        free(c);
        vTaskDelete(NULL);
        return;
    }
    c->cap = MQTT_BUF_SIZE;

    ESP_LOGI(TAG, "[%s] waiting for link...", c->name);
    while (!c->is_up()) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    while (1) {
        mqtt_session(c);            /* returns only when the session is gone */
        vTaskDelay(pdMS_TO_TICKS(MQTT_RECONNECT_MS));
    }
}

void mqtt_client_start(const char *name, const net_sock_ops_t *ops,
                       const mqtt_config_t *cfg, bool (*is_up)(void))
{
    mqtt_ctx_t *c = calloc(1, sizeof(*c));

    if (c == NULL) {
        ESP_LOGE(TAG, "[%s] out of memory", name);
        return;
    }
    c->name        = name;
    c->ops         = ops;
    c->cfg         = *cfg;          /* by value; the strings inside are shared */
    c->is_up       = is_up;
    c->fd          = -1;
    c->next_pkt_id = 1;             /* 0 is not a valid packet identifier */

    if (xTaskCreate(mqtt_task, name, 4096, c, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "[%s] xTaskCreate failed", name);
        free(c);
    }
}
