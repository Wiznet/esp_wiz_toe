/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * Backend-neutral loopback (echo) engine. Same logic as the original inline
 * routines in W5500_loopback.c, but the BSD socket calls go through a vtable
 * (loopback_ops_t) so the Ethernet (W5500) and Wi-Fi paths reuse one copy.
 */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_netif.h"          /* esp_ip4addr_aton (TCP-client target parse) */

#include "loopback.h"
#include "net_config.h"         /* LOOPBACK_BUF_SIZE */

/* ---- Loopback mode (override at build time with -DLOOPBACK_MODE=n) ---- */
#define LOOPBACK_TCP_SERVER   0
#define LOOPBACK_TCP_CLIENT   1
#define LOOPBACK_UDP          2

#ifndef LOOPBACK_MODE
#define LOOPBACK_MODE         LOOPBACK_TCP_SERVER
#endif

static const char *TAG = "loopback";

/* Echo `len` bytes back, tolerating short writes.
 *
 * EAGAIN/EWOULDBLOCK from a BLOCKING send() is not an error: on the TOE it
 * means the chip has not yet acknowledged the previous transmission
 * (SOCK_BUSY), and on LwIP it is an elapsed SO_SNDTIMEO. Retrying is what both
 * ask for; treating it as failure would drop the connection mid-echo.
 *
 * Returns true when every byte went out. */
static bool send_all(const loopback_ops_t *ops, int fd, const uint8_t *buf, int len)
{
    int off = 0;
    while (off < len) {
        int w = ops->send(fd, buf + off, len - off, 0);
        if (w > 0) {
            off += w;
            continue;
        }
        if (w < 0 && (errno == EWOULDBLOCK || errno == EAGAIN)) {
            vTaskDelay(1);
            continue;
        }
        return false;                  /* 0 = peer gone, <0 = real error */
    }
    return true;
}

/* Standard lwIP BSD socket vtable (Ethernet). With WSM_DRIVER_SOCKET_WRAP=1 these
 * lwip_* symbols are --wrap-redirected to the W5500; with =0 they are software
 * LwIP over esp_eth. Correct either way, so no #if here. Exposed so app_main
 * references it the same way as the
 * Wi-Fi vtable (both are module-provided const vtables taken by address). */
const loopback_ops_t loopback_lwip_ops = {
    .socket = lwip_socket,   .bind = lwip_bind,
    .listen = lwip_listen,   .accept = lwip_accept,
    .connect = lwip_connect, .recv = lwip_recv,
    .send = lwip_send,       .recvfrom = lwip_recvfrom,
    .sendto = lwip_sendto,   .setsockopt = lwip_setsockopt,
    .close = lwip_close,
};

#if (LOOPBACK_MODE == LOOPBACK_TCP_SERVER)
static void loopback_tcp_server(const char *tag, const loopback_ops_t *ops,
                                uint16_t port, uint8_t *buf, int buf_size)
{
    int lsock = ops->socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (lsock < 0) {
        ESP_LOGE(TAG, "[%s] socket() failed: errno %d", tag, errno);
        return;
    }
    int opt = 1;
    ops->setsockopt(lsock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (ops->bind(lsock, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        ops->listen(lsock, 1) < 0) {
        ESP_LOGE(TAG, "[%s] bind/listen failed: errno %d", tag, errno);
        ops->close(lsock);
        return;
    }
    ESP_LOGI(TAG, "[%s] TCP server listening on port %d", tag, port);

    while (1) {
        struct sockaddr_in src;
        socklen_t sl = sizeof(src);
        int c = ops->accept(lsock, (struct sockaddr *)&src, &sl);
        if (c < 0) {
            ESP_LOGE(TAG, "[%s] accept failed: errno %d", tag, errno);
            continue;
        }
        ESP_LOGI(TAG, "[%s] client connected", tag);
        while (1) {
            int n = ops->recv(c, buf, buf_size, 0);
            if (n <= 0) {
                break;
            }
            if (!send_all(ops, c, buf, n)) {  /* echo back */
                break;
            }
        }
        ESP_LOGI(TAG, "[%s] client disconnected", tag);
        ops->close(c);
    }
}
#endif /* LOOPBACK_TCP_SERVER */

#if (LOOPBACK_MODE == LOOPBACK_TCP_CLIENT)
static void loopback_tcp_client(const char *tag, const loopback_ops_t *ops,
                                const char *target_ip, uint16_t target_port,
                                uint8_t *buf, int buf_size)
{
    while (1) {
        int s = ops->socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s < 0) {
            ESP_LOGE(TAG, "[%s] socket() failed: errno %d", tag, errno);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        struct sockaddr_in dst = {
            .sin_family = AF_INET,
            .sin_port = htons(target_port),
        };
        dst.sin_addr.s_addr = esp_ip4addr_aton(target_ip);

        if (ops->connect(s, (struct sockaddr *)&dst, sizeof(dst)) == 0) {
            ESP_LOGI(TAG, "[%s] connected to %s:%d", tag, target_ip, target_port);
            while (1) {
                int n = ops->recv(s, buf, buf_size, 0);   /* echo whatever the peer sends */
                if (n <= 0) {
                    break;
                }
                if (!send_all(ops, s, buf, n)) {
                    break;
                }
            }
            ESP_LOGI(TAG, "[%s] connection closed", tag);
        } else {
            ESP_LOGW(TAG, "[%s] connect to %s:%d failed: errno %d", tag, target_ip, target_port, errno);
        }
        ops->close(s);
        vTaskDelay(pdMS_TO_TICKS(1000));             /* retry */
    }
}
#endif /* LOOPBACK_TCP_CLIENT */

#if (LOOPBACK_MODE == LOOPBACK_UDP)
static void loopback_udp(const char *tag, const loopback_ops_t *ops,
                         uint16_t port, uint8_t *buf, int buf_size)
{
    int s = ops->socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) {
        ESP_LOGE(TAG, "[%s] socket() failed: errno %d", tag, errno);
        return;
    }
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (ops->bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "[%s] bind failed: errno %d", tag, errno);
        ops->close(s);
        return;
    }
    ESP_LOGI(TAG, "[%s] UDP echo on port %d", tag, port);

    while (1) {
        struct sockaddr_in src;
        socklen_t sl = sizeof(src);
        int n = ops->recvfrom(s, buf, buf_size, 0, (struct sockaddr *)&src, &sl);
        if (n > 0) {
            ops->sendto(s, buf, n, 0, (struct sockaddr *)&src, sl);  /* echo back */
        }
    }
}
#endif /* LOOPBACK_UDP */

static void loopback_run(const char *tag, const loopback_ops_t *ops,
                         uint16_t listen_port, const char *target_ip, uint16_t target_port,
                         uint8_t *buf, int buf_size)
{
#if   (LOOPBACK_MODE == LOOPBACK_TCP_SERVER)
    (void)target_ip; (void)target_port;
    ESP_LOGI(TAG, "[%s] loopback: TCP SERVER on port %d", tag, listen_port);
    loopback_tcp_server(tag, ops, listen_port, buf, buf_size);
#elif (LOOPBACK_MODE == LOOPBACK_TCP_CLIENT)
    (void)listen_port;
    ESP_LOGI(TAG, "[%s] loopback: TCP CLIENT -> %s:%d", tag, target_ip, target_port);
    loopback_tcp_client(tag, ops, target_ip, target_port, buf, buf_size);
#elif (LOOPBACK_MODE == LOOPBACK_UDP)
    (void)target_ip; (void)target_port;
    ESP_LOGI(TAG, "[%s] loopback: UDP on port %d", tag, listen_port);
    loopback_udp(tag, ops, listen_port, buf, buf_size);
#else
#error "Invalid LOOPBACK_MODE (expected 0=TCP_SERVER, 1=TCP_CLIENT, 2=UDP)"
#endif
}

/* --------------------------------------------------------------------------
 * Task launcher: Ethernet and Wi-Fi are both started this way (same level).
 * ------------------------------------------------------------------------ */
typedef struct {
    const char           *name;
    const loopback_ops_t *ops;
    uint16_t              listen_port;
    uint16_t              target_port;
    const char           *target_ip;
    bool                (*is_up)(void);
} loopback_ctx_t;

static void loopback_task(void *arg)
{
    loopback_ctx_t *c = (loopback_ctx_t *)arg;

    uint8_t *buf = malloc(LOOPBACK_BUF_SIZE);
    if (buf == NULL) {
        ESP_LOGE(TAG, "[%s] out of memory for %d-byte buffer", c->name, LOOPBACK_BUF_SIZE);
        free(c);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "[%s] waiting for link...", c->name);
    while (!c->is_up()) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    loopback_run(c->name, c->ops, c->listen_port, c->target_ip, c->target_port,
                 buf, LOOPBACK_BUF_SIZE);

    free(buf);       /* loopback_run only returns on a fatal setup error */
    free(c);
    vTaskDelete(NULL);
}

void loopback_start(const char *name, const loopback_ops_t *ops,
                    uint16_t listen_port, const char *target_ip, uint16_t target_port,
                    bool (*is_up)(void))
{
    loopback_ctx_t *c = malloc(sizeof(*c));
    if (c == NULL) {
        ESP_LOGE(TAG, "[%s] out of memory", name);
        return;
    }
    c->name = name;
    c->ops = ops;
    c->listen_port = listen_port;
    c->target_port = target_port;
    c->target_ip = target_ip;
    c->is_up = is_up;

    if (xTaskCreate(loopback_task, name, 4096, c, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "[%s] xTaskCreate failed", name);
        free(c);
    }
}
