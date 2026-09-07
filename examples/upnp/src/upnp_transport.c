/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * BSD implementation of the UPnP transport seam (see upnp_transport.h).
 *
 * This is the only file in the example that includes lwIP. Sockets are reached
 * through the component's net_sock_ops_t vtable, so the client runs on the
 * WIZnet hardware sockets or on the software LwIP the Wi-Fi netif is attached
 * to, chosen by which vtable main.c hands over.
 */
#include <errno.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"    /* vTaskDelay in the send retry */
#include "esp_log.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"

#include "net_sock_ops.h"
#include "upnp_transport.h"

static const char *TAG = "upnp_tx";

static const net_sock_ops_t *s_ops;

void upnp_transport_bind(const void *ops)
{
    s_ops = (const net_sock_ops_t *)ops;
}

/* A blocking send() that reports EAGAIN/EWOULDBLOCK has not failed: the TOE
 * says this while the chip has an unacknowledged transmission in flight
 * (SOCK_BUSY), and LwIP says it for an elapsed SO_SNDTIMEO. Both want a
 * retry, so a request is not abandoned over a condition that clears itself.
 *
 * Returns the number of bytes written, or -1 on a genuine failure. */
static int send_all(int fd, const char *buf, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        int n = s_ops->send(fd, buf + sent, len - sent, 0);
        if (n > 0) {
            sent += (size_t)n;
            continue;
        }
        if (n < 0 && (errno == EWOULDBLOCK || errno == EAGAIN)) {
            vTaskDelay(1);
            continue;
        }
        return -1;                      /* 0 = peer gone, <0 = real error */
    }
    return (int)sent;
}

static int set_timeout(int fd, uint32_t ms)
{
    struct timeval tv = {
        .tv_sec  = ms / 1000,
        .tv_usec = (ms % 1000) * 1000,
    };
    return s_ops->setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

int upnp_transport_ssdp(const char *group_ip, uint16_t group_port,
                        uint16_t local_port, const char *msg,
                        char *resp, size_t resp_size, uint32_t timeout_ms)
{
    if (s_ops == NULL) {
        ESP_LOGE(TAG, "transport used before bind");
        return -1;
    }

    int fd = s_ops->socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        ESP_LOGE(TAG, "socket() failed: errno %d", errno);
        return -1;
    }

    struct sockaddr_in local = {
        .sin_family = AF_INET,
        .sin_port = htons(local_port),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (s_ops->bind(fd, (struct sockaddr *)&local, sizeof(local)) < 0) {
        ESP_LOGE(TAG, "bind(%u) failed: errno %d", local_port, errno);
        s_ops->close(fd);
        return -1;
    }
    set_timeout(fd, timeout_ms);

    /* TTL 1 keeps the search on the local segment, which is where the IGD is.
     * Ignored by the TOE path, which has no software multicast layer. */
    int ttl = 1;
    s_ops->setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));

    struct sockaddr_in dst = {
        .sin_family = AF_INET,
        .sin_port = htons(group_port),
        .sin_addr.s_addr = inet_addr(group_ip),
    };
    int n = s_ops->sendto(fd, msg, strlen(msg), 0,
                          (struct sockaddr *)&dst, sizeof(dst));
    if (n < 0) {
        ESP_LOGE(TAG, "M-SEARCH to %s:%u failed: errno %d",
                 group_ip, group_port, errno);
        s_ops->close(fd);
        return -1;
    }

    struct sockaddr_in src;
    socklen_t sl = sizeof(src);
    n = s_ops->recvfrom(fd, resp, resp_size - 1, 0,
                        (struct sockaddr *)&src, &sl);
    s_ops->close(fd);

    if (n <= 0) {
        return 0;                       /* nobody answered in time */
    }
    resp[n] = '\0';
    ESP_LOGI(TAG, "SSDP reply from %s (%d bytes)", inet_ntoa(src.sin_addr), n);
    return n;
}

int upnp_transport_http(const char *ip, uint16_t port, const char *request,
                        char *resp, size_t resp_size, uint32_t timeout_ms)
{
    if (s_ops == NULL) {
        ESP_LOGE(TAG, "transport used before bind");
        return -1;
    }

    int fd = s_ops->socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        ESP_LOGE(TAG, "socket() failed: errno %d", errno);
        return -1;
    }

    struct sockaddr_in dst = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = inet_addr(ip),
    };
    if (s_ops->connect(fd, (struct sockaddr *)&dst, sizeof(dst)) < 0) {
        ESP_LOGE(TAG, "connect(%s:%u) failed: errno %d", ip, port, errno);
        s_ops->close(fd);
        return -1;
    }
    set_timeout(fd, timeout_ms);

    if (send_all(fd, request, strlen(request)) < 0) {
        ESP_LOGE(TAG, "send failed: errno %d", errno);
        s_ops->close(fd);
        return -1;
    }

    /* Read until the router closes the connection. UPnP answers are a few KB
     * at most and the IGD does not keep the socket alive between actions. */
    size_t total = 0;
    while (total < resp_size - 1) {
        int n = s_ops->recv(fd, resp + total, resp_size - 1 - total, 0);
        if (n <= 0) {
            break;                      /* peer closed, or the timeout fired */
        }
        total += n;
    }
    s_ops->close(fd);

    resp[total] = '\0';
    return (int)total;
}

int upnp_transport_listen(uint16_t port)
{
    if (s_ops == NULL) {
        ESP_LOGE(TAG, "transport used before bind");
        return -1;
    }

    int fd = s_ops->socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        ESP_LOGE(TAG, "socket() failed: errno %d", errno);
        return -1;
    }

    struct sockaddr_in local = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (s_ops->bind(fd, (struct sockaddr *)&local, sizeof(local)) < 0 ||
        s_ops->listen(fd, 1) < 0) {
        ESP_LOGE(TAG, "listen(%u) failed: errno %d", port, errno);
        s_ops->close(fd);
        return -1;
    }
    return fd;
}

int upnp_transport_accept(int listen_fd, uint32_t timeout_ms)
{
    set_timeout(listen_fd, timeout_ms);

    struct sockaddr_in peer;
    socklen_t sl = sizeof(peer);
    int fd = s_ops->accept(listen_fd, (struct sockaddr *)&peer, &sl);
    if (fd < 0) {
        return -1;                      /* nothing connected in time */
    }
    ESP_LOGI(TAG, "eventing connection from %s", inet_ntoa(peer.sin_addr));
    return fd;
}

int upnp_transport_recv(int fd, char *buf, size_t size, uint32_t timeout_ms)
{
    set_timeout(fd, timeout_ms);

    int n = s_ops->recv(fd, buf, size - 1, 0);
    if (n <= 0) {
        return n < 0 ? -1 : 0;
    }
    buf[n] = '\0';
    return n;
}

int upnp_transport_send(int fd, const char *buf, size_t len)
{
    return send_all(fd, buf, len);
}

void upnp_transport_close(int fd)
{
    if (fd >= 0) {
        s_ops->close(fd);
    }
}
