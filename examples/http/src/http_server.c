/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * HTTP/1.1 server engine. Serves the same page as the ioLibrary version this
 * example was ported from, but written against the BSD socket API and reached
 * through a vtable, so the engine is backend-neutral (see http_server.h).
 *
 * The port from the ioLibrary httpServer is mostly a simplification:
 *   - httpServer_run()'s Sn_SR state machine (SOCK_ESTABLISHED / CLOSE_WAIT /
 *     CLOSED plus the manual re-listen) over a fixed list of hardware sockets
 *     collapses into a plain accept() loop;
 *   - httpServer_time_handler() and its 1-second esp_timer disappear: the
 *     session timeout is just SO_RCVTIMEO on the socket;
 *   - the httpParser/httpUtil content registry disappears too — this example
 *     serves exactly one page, so the request line is matched directly.
 * Every response says "Connection: close", so one request is one connection.
 * That is a choice this example makes, not a limit of the chip: accept() has
 * BSD semantics on the TOE too, so the listener survives each connection and
 * several can be open at once, up to the chip's eight hardware sockets.
 */
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "http_server.h"
#include "web_page.h"       /* index_page */
#include "net_config.h"     /* HTTP_BUF_SIZE, HTTP_RECV_TIMEOUT_MS */

static const char *TAG = "http";

/* Everything one server instance owns. One of these per interface, so the
 * Ethernet and Wi-Fi servers share no state at all. */
typedef struct {
    const char           *name;
    const net_sock_ops_t *ops;
    uint16_t              port;
    bool                (*is_up)(void);
} http_ctx_t;

/* ---- socket helpers ------------------------------------------------------ */

static void sock_set_rcvtimeo(const net_sock_ops_t *ops, int fd, uint32_t ms)
{
    struct timeval tv = {
        .tv_sec  = (long)(ms / 1000),
        .tv_usec = (long)((ms % 1000) * 1000),
    };
    ops->setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

/* send() may take less than asked on either backend, so loop.
 *
 * EAGAIN/EWOULDBLOCK from a BLOCKING send() is not a failure and must not abort
 * the response. On the TOE it means the chip has not yet acknowledged the
 * previous transmission (SOCK_BUSY); on LwIP a socket carrying SO_SNDTIMEO
 * reports an elapsed timeout the same way. Both mean "try again in a moment". */
static bool send_all(const net_sock_ops_t *ops, int fd, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    size_t off = 0;

    while (off < len) {
        int w = ops->send(fd, p + off, len - off, 0);
        if (w > 0) {
            off += (size_t)w;
            continue;
        }
        if (w < 0 && (errno == EWOULDBLOCK || errno == EAGAIN)) {
            vTaskDelay(1);
            continue;
        }
        return false;                   /* 0 = peer gone, <0 = real error */
    }
    return true;
}

/* ---- request / response -------------------------------------------------- */

/*
 * Read until the end of the request headers ("\r\n\r\n"). The body, if any, is
 * ignored: this server only answers GET and HEAD. Returns the number of bytes
 * in `buf` (NUL-terminated), or -1 if the client went away or timed out first.
 */
static int recv_request(const net_sock_ops_t *ops, int fd, char *buf, int cap)
{
    int len = 0;

    while (len < cap - 1) {
        int n = ops->recv(fd, buf + len, cap - 1 - len, 0);
        if (n <= 0) {
            return -1;                  /* EOF, SO_RCVTIMEO, or a real error */
        }
        len += n;
        buf[len] = '\0';
        if (strstr(buf, "\r\n\r\n") != NULL) {
            return len;
        }
    }
    return len;                         /* headers longer than the buffer */
}

/* Copy the first two whitespace-delimited tokens of the request line — the
 * method and the target — out of `req`. Returns false if it is not a request
 * line at all. */
static bool parse_request_line(const char *req, char *method, size_t method_cap,
                               char *target, size_t target_cap)
{
    size_t i = 0;

    while (req[i] != '\0' && req[i] != ' ' && req[i] != '\r' && req[i] != '\n') {
        if (i < method_cap - 1) {
            method[i] = req[i];
        }
        i++;
    }
    method[(i < method_cap - 1) ? i : method_cap - 1] = '\0';
    if (req[i] != ' ') {
        return false;
    }
    i++;

    size_t j = 0;
    while (req[i] != '\0' && req[i] != ' ' && req[i] != '\r' && req[i] != '\n') {
        if (j < target_cap - 1) {
            target[j] = req[i];
        }
        i++;
        j++;
    }
    target[(j < target_cap - 1) ? j : target_cap - 1] = '\0';
    return target[0] != '\0';
}

/*
 * Send a complete response. `body` may be NULL (HEAD), in which case only the
 * headers go out — Content-Length still describes the body that a GET would
 * have returned, as RFC 9110 requires.
 */
static void send_response(http_ctx_t *c, int fd, char *hdr, int hdr_cap,
                          const char *status, const char *ctype,
                          const char *body, size_t body_len)
{
    int n = snprintf(hdr, hdr_cap,
                     "HTTP/1.1 %s\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %u\r\n"
                     "Connection: close\r\n"
                     "\r\n",
                     status, ctype, (unsigned)body_len);
    if (n < 0 || n >= hdr_cap) {
        return;                         /* cannot happen with these fixed strings */
    }

    if (!send_all(c->ops, fd, hdr, (size_t)n)) {
        ESP_LOGW(TAG, "[%s] send headers failed: errno %d", c->name, errno);
        return;
    }
    if (body != NULL && body_len > 0 && !send_all(c->ops, fd, body, body_len)) {
        ESP_LOGW(TAG, "[%s] send body failed: errno %d", c->name, errno);
    }
}

/* Only "/" and "/index.html" exist here — the single page the example serves.
 * A query string is ignored, the way a real server would strip it. */
static bool target_is_index(const char *target)
{
    size_t n = strcspn(target, "?");

    return (n == 1 && target[0] == '/') ||
           (n == 11 && strncmp(target, "/index.html", 11) == 0);
}

/* Serve one accepted client: read the request, answer it, return. The caller
 * closes the socket — every response carries "Connection: close". */
static void serve_client(http_ctx_t *c, int fd, char *buf, int cap)
{
    static const char *ctype_html = "text/html";
    const size_t page_len = strlen(index_page);

    int len = recv_request(c->ops, fd, buf, cap);
    if (len < 0) {
        ESP_LOGW(TAG, "[%s] no request received", c->name);
        return;
    }

    char method[8];
    char target[128];
    if (!parse_request_line(buf, method, sizeof(method), target, sizeof(target))) {
        ESP_LOGW(TAG, "[%s] malformed request line", c->name);
        send_response(c, fd, buf, cap, "400 Bad Request", ctype_html, "", 0);
        return;
    }
    ESP_LOGI(TAG, "[%s] %s %s", c->name, method, target);

    bool is_get  = (strcmp(method, "GET") == 0);
    bool is_head = (strcmp(method, "HEAD") == 0);

    /* buf held the request; from here on it is scratch for the response header,
     * so nothing is read out of it after this point. */
    if (!is_get && !is_head) {
        send_response(c, fd, buf, cap, "501 Not Implemented", ctype_html, "", 0);
    } else if (!target_is_index(target)) {
        static const char *not_found =
            "<!DOCTYPE html><html><body><h1>404 Not Found</h1></body></html>";
        send_response(c, fd, buf, cap, "404 Not Found", ctype_html,
                      is_head ? NULL : not_found, strlen(not_found));
    } else {
        send_response(c, fd, buf, cap, "200 OK", ctype_html,
                      is_head ? NULL : index_page, page_len);
    }
}

/* ---- server -------------------------------------------------------------- */

static void http_serve(http_ctx_t *c, char *buf, int cap)
{
    const net_sock_ops_t *ops = c->ops;

    int lsock = ops->socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (lsock < 0) {
        ESP_LOGE(TAG, "[%s] socket() failed: errno %d", c->name, errno);
        return;
    }
    int opt = 1;
    ops->setsockopt(lsock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    ops->setsockopt(lsock, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
    /* Bounds accept() as well as recv(). The TOE applies SO_RCVTIMEO to accept()
     * too, and hands the value on to the accepted socket; on LwIP the accepted
     * socket inherits it. Re-applied below either way, so neither is assumed. */
    sock_set_rcvtimeo(ops, lsock, HTTP_RECV_TIMEOUT_MS);

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(c->port),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (ops->bind(lsock, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        ops->listen(lsock, 1) < 0) {
        ESP_LOGE(TAG, "[%s] bind/listen failed: errno %d", c->name, errno);
        ops->close(lsock);
        return;
    }
    ESP_LOGI(TAG, "[%s] HTTP server listening on port %d", c->name, c->port);

    while (1) {
        struct sockaddr_in src;
        socklen_t sl = sizeof(src);
        int fd = ops->accept(lsock, (struct sockaddr *)&src, &sl);
        if (fd < 0) {
            continue;               /* accept timeout, or a transient error */
        }
        sock_set_rcvtimeo(ops, fd, HTTP_RECV_TIMEOUT_MS);

        serve_client(c, fd, buf, cap);

        /* Closes the connection only; lsock keeps listening on both backends.
         * On the TOE this also returns a hardware socket to the pool, which is
         * what lets a listener still waiting for one arm itself. */
        ops->close(fd);
    }
}

/* ---- task launcher: same shape as examples/loopback ---------------------- */

static void http_task(void *arg)
{
    http_ctx_t *c = (http_ctx_t *)arg;

    char *buf = malloc(HTTP_BUF_SIZE);
    if (buf == NULL) {
        ESP_LOGE(TAG, "[%s] out of memory for %d-byte buffer", c->name, HTTP_BUF_SIZE);
        free(c);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "[%s] waiting for link...", c->name);
    while (!c->is_up()) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    http_serve(c, buf, HTTP_BUF_SIZE);

    free(buf);       /* http_serve only returns on a fatal setup error */
    free(c);
    vTaskDelete(NULL);
}

void http_server_start(const char *name, const net_sock_ops_t *ops,
                       uint16_t port, bool (*is_up)(void))
{
    http_ctx_t *c = calloc(1, sizeof(*c));
    if (c == NULL) {
        ESP_LOGE(TAG, "[%s] out of memory", name);
        return;
    }
    c->name = name;
    c->ops = ops;
    c->port = port;
    c->is_up = is_up;

    if (xTaskCreate(http_task, name, 4096, c, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "[%s] xTaskCreate failed", name);
        free(c);
    }
}
