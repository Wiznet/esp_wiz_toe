/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * Backend-neutral multi-socket TCP echo server. Same observable behaviour as
 * the original WIZnet-PICO-C example (N listeners on consecutive ports, several
 * clients connected at once, data echoed back and printed with the peer
 * address), but the BSD socket calls go through a vtable (multi_socket_ops_t)
 * so the Ethernet (WIZnet chip) and Wi-Fi paths reuse one copy.
 *
 * Structure differs from the original in one respect: the original round-robins
 * over the hardware sockets from a single loop, polling Sn_SR. Here each
 * listener gets its own task and blocks in accept()/recv(), which is what the
 * BSD socket API offers on both stacks (the TOE has no select/poll). The wire
 * behaviour is the same.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "multi_socket.h"
#include "net_config.h"         /* MULTI_SOCKET_BUF_SIZE */

static const char *TAG = "multi_socket";

/* Standard lwIP BSD socket vtable (Ethernet). With WSM_DRIVER_SOCKET_WRAP=1 these
 * lwip_* symbols are --wrap-redirected to the WIZnet chip; with =0 they are
 * software LwIP over esp_eth. Correct either way, so no #if here. */
const multi_socket_ops_t multi_socket_lwip_ops = {
    .socket = lwip_socket,   .bind = lwip_bind,
    .listen = lwip_listen,   .accept = lwip_accept,
    .connect = lwip_connect, .recv = lwip_recv,
    .send = lwip_send,       .recvfrom = lwip_recvfrom,
    .sendto = lwip_sendto,   .setsockopt = lwip_setsockopt,
    .close = lwip_close,
};

/* Write the whole buffer, tolerating a short write.
 *
 * EAGAIN/EWOULDBLOCK from a BLOCKING send() is not an error and must not end
 * the connection. On the TOE it means the chip has not yet acknowledged the
 * previous transmission (SOCK_BUSY); on LwIP a socket with SO_SNDTIMEO set
 * reports an elapsed timeout the same way. Both are "try again in a moment".
 *
 * Returns true when every byte went out. */
static bool send_all(const multi_socket_ops_t *ops, int fd,
                     const uint8_t *buf, int len)
{
    int off = 0;
    while (off < len) {
        int sent = ops->send(fd, buf + off, len - off, 0);
        if (sent > 0) {
            off += sent;
            continue;
        }
        if (sent < 0 && (errno == EWOULDBLOCK || errno == EAGAIN)) {
            vTaskDelay(1);              /* let the chip drain, then retry */
            continue;
        }
        return false;                   /* 0 = peer gone, <0 = real error */
    }
    return true;
}

/* --------------------------------------------------------------------------
 * Per-listener echo task
 * ------------------------------------------------------------------------ */
typedef struct {
    const char                *name;
    const multi_socket_ops_t  *ops;
    int                        lfd;      /* listening socket, already bound */
    uint16_t                   port;
    int                        index;
} worker_ctx_t;

static void log_peer(const char *name, int index, const char *what,
                     const struct sockaddr_in *src)
{
    uint32_t a = ntohl(src->sin_addr.s_addr);
    ESP_LOGI(TAG, "[%s#%d] %s - %u.%u.%u.%u:%u", name, index, what,
             (unsigned)((a >> 24) & 0xFF), (unsigned)((a >> 16) & 0xFF),
             (unsigned)((a >> 8) & 0xFF),  (unsigned)(a & 0xFF),
             (unsigned)ntohs(src->sin_port));
}

static void worker_task(void *arg)
{
    worker_ctx_t *w = (worker_ctx_t *)arg;
    const multi_socket_ops_t *ops = w->ops;

    /* +1 so the echoed payload can be NUL-terminated for the log line, same as
     * the original example's ETHERNET_BUF_MAX_SIZE + 1. */
    uint8_t *buf = malloc(MULTI_SOCKET_BUF_SIZE + 1);
    if (buf == NULL) {
        ESP_LOGE(TAG, "[%s#%d] out of memory for %d-byte buffer",
                 w->name, w->index, MULTI_SOCKET_BUF_SIZE);
        ops->close(w->lfd);
        free(w);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "[%s#%d] listening on port %u", w->name, w->index, (unsigned)w->port);

    while (1) {
        struct sockaddr_in src;
        socklen_t sl = sizeof(src);

        int c = ops->accept(w->lfd, (struct sockaddr *)&src, &sl);
        if (c < 0) {
            ESP_LOGE(TAG, "[%s#%d] accept failed: errno %d", w->name, w->index, errno);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        log_peer(w->name, w->index, "connected", &src);

        while (1) {
            int n = ops->recv(c, buf, MULTI_SOCKET_BUF_SIZE, 0);
            if (n <= 0) {
                break;
            }
            buf[n] = 0x00;

            if (!send_all(ops, c, buf, n)) {   /* echo back */
                break;
            }
            ESP_LOGI(TAG, "[%s#%d] port %u message:%s",
                     w->name, w->index, (unsigned)w->port, (char *)buf);
        }

        log_peer(w->name, w->index, "disconnected", &src);

        /* Closes the accepted connection only; w->lfd keeps listening on both
         * backends. On the TOE this also returns a hardware socket to the pool,
         * which is what lets a listener still waiting for one arm itself. */
        ops->close(c);
    }
}

/* --------------------------------------------------------------------------
 * Supervisor: waits for the link, then opens the listeners one at a time
 * ------------------------------------------------------------------------ */
typedef struct {
    const char               *name;
    const multi_socket_ops_t *ops;
    uint16_t                  port_base;
    int                       count;
    bool                    (*is_up)(void);
} supervisor_ctx_t;

static bool open_listener(const supervisor_ctx_t *s, int index)
{
    const multi_socket_ops_t *ops = s->ops;
    uint16_t port = (uint16_t)(s->port_base + index);

    int fd = ops->socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        ESP_LOGE(TAG, "[%s#%d] socket() failed: errno %d", s->name, index, errno);
        return false;
    }

    int opt = 1;
    ops->setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (ops->bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        ops->listen(fd, 1) < 0) {
        ESP_LOGE(TAG, "[%s#%d] bind/listen on port %u failed: errno %d",
                 s->name, index, (unsigned)port, errno);
        ops->close(fd);
        return false;
    }

    worker_ctx_t *w = malloc(sizeof(*w));
    if (w == NULL) {
        ESP_LOGE(TAG, "[%s#%d] out of memory", s->name, index);
        ops->close(fd);
        return false;
    }
    w->name  = s->name;
    w->ops   = ops;
    w->lfd   = fd;
    w->port  = port;
    w->index = index;

    char task_name[16];
    snprintf(task_name, sizeof(task_name), "%.7s_srv%d", s->name, index);

    if (xTaskCreate(worker_task, task_name, 3072, w, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "[%s#%d] xTaskCreate failed", s->name, index);
        ops->close(fd);
        free(w);
        return false;
    }
    return true;
}

static void supervisor_task(void *arg)
{
    supervisor_ctx_t *s = (supervisor_ctx_t *)arg;

    ESP_LOGI(TAG, "[%s] waiting for link...", s->name);
    while (!s->is_up()) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    int opened = 0;
    for (int i = 0; i < s->count; i++) {
        if (open_listener(s, i)) {
            opened++;
        }
    }

    ESP_LOGI(TAG, "[%s] %d/%d listeners up on ports %u-%u",
             s->name, opened, s->count,
             (unsigned)s->port_base, (unsigned)(s->port_base + s->count - 1));

    free(s);
    vTaskDelete(NULL);
}

void multi_socket_start(const char *name, const multi_socket_ops_t *ops,
                        uint16_t port_base, int count, bool (*is_up)(void))
{
    supervisor_ctx_t *s = malloc(sizeof(*s));
    if (s == NULL) {
        ESP_LOGE(TAG, "[%s] out of memory", name);
        return;
    }
    s->name      = name;
    s->ops       = ops;
    s->port_base = port_base;
    s->count     = count;
    s->is_up     = is_up;

    char task_name[16];
    snprintf(task_name, sizeof(task_name), "%.9s_sup", name);

    if (xTaskCreate(supervisor_task, task_name, 3072, s, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "[%s] xTaskCreate failed", name);
        free(s);
    }
}
