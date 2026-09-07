/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * TLS echo server. Same behaviour as the ioLibrary version this example was
 * ported from, but written against the BSD socket API and reached through a
 * vtable, so the engine is backend-neutral (see ssl_echo.h).
 *
 * The port from the ioLibrary version is mostly a simplification:
 *   - the Sn_SR state machine (SOCK_ESTABLISHED / CLOSE_WAIT / CLOSED, plus the
 *     manual re-listen) collapses into a plain accept() loop;
 *   - the getsockopt(SO_RECVBUF) poll before every read disappears, because
 *     SO_RCVTIMEO lets mbedtls_ssl_read() block with a bound instead of spinning.
 * Both backends report a receive timeout as -1/EWOULDBLOCK, so one BIO covers
 * the WIZnet hardware sockets and the software LwIP alike.
 */
#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "mbedtls/ssl.h"
#include "mbedtls/pk.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/error.h"

#include "ssl_echo.h"
#include "ssl_credentials.h"
#include "net_config.h"     /* SSL_ECHO_BUF_SIZE, SSL_ECHO_TIMEOUT_MS, banner */

static const char *TAG = "ssl_echo";

/* ECDHE-RSA ciphersuites, which the RSA-2048 demo certificate can serve.
 *
 * The WIZnet-PICO-C reference pins the static-RSA suites
 * (MBEDTLS_TLS_RSA_WITH_AES_*), but ESP-IDF v6.0 ships mbedTLS 4.0, which
 * REMOVED static-RSA key exchange along with those constants — the reference
 * list no longer compiles. ECDHE-RSA is the direct replacement: same RSA
 * certificate, ephemeral key agreement, and every modern client offers it. */
static const int g_ciphersuites[] = {
    MBEDTLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
    MBEDTLS_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384,
    MBEDTLS_TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA256,
    0
};

/* Everything one server instance owns. One of these per interface, so the
 * Ethernet and Wi-Fi servers never share mbedTLS state. */
typedef struct {
    const char           *name;
    const net_sock_ops_t *ops;
    uint16_t              port;
    bool                (*is_up)(void);

    int                   client_fd;   /* BIO context: the accepted socket */
    mbedtls_ssl_context   ssl;
    mbedtls_ssl_config    conf;
    mbedtls_x509_crt      cert;
    mbedtls_pk_context    key;
} ssl_echo_ctx_t;

/* ---- mbedTLS glue -------------------------------------------------------- */

/* WANT_WRITE, not an error, when the send would have blocked: the TOE reports
 * -1/EWOULDBLOCK while the chip has an unacknowledged transmission in flight
 * (SOCK_BUSY), and LwIP does the same on an elapsed SO_SNDTIMEO. mbedTLS
 * retries the record on WANT_WRITE; INTERNAL_ERROR would drop the session over
 * a condition that clears itself in milliseconds. */
static int bio_send(void *p, const unsigned char *buf, size_t len)
{
    ssl_echo_ctx_t *c = (ssl_echo_ctx_t *)p;
    int n = c->ops->send(c->client_fd, buf, len, 0);
    if (n < 0) {
        if (errno == EWOULDBLOCK || errno == EAGAIN) {
            return MBEDTLS_ERR_SSL_WANT_WRITE;
        }
        return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    }
    return n;
}

/* A receive timeout arrives as -1/EWOULDBLOCK from both the TOE wrap and the
 * software LwIP, so the two map onto MBEDTLS_ERR_SSL_TIMEOUT identically. */
static int bio_recv_timeout(void *p, unsigned char *buf, size_t len, uint32_t timeout_ms)
{
    ssl_echo_ctx_t *c = (ssl_echo_ctx_t *)p;
    struct timeval tv = {
        .tv_sec  = timeout_ms / 1000,
        .tv_usec = (timeout_ms % 1000) * 1000,
    };
    c->ops->setsockopt(c->client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    int n = c->ops->recv(c->client_fd, buf, len, 0);
    if (n < 0) {
        if (errno == EWOULDBLOCK || errno == EAGAIN) {
            return MBEDTLS_ERR_SSL_TIMEOUT;
        }
        return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    }
    if (n == 0) {
        return MBEDTLS_ERR_SSL_CONN_EOF;
    }
    return n;
}

static int bio_recv(void *p, unsigned char *buf, size_t len)
{
    return bio_recv_timeout(p, buf, len, SSL_ECHO_TIMEOUT_MS);
}

/* Parse the demo credentials and build the server config. Done once per task. */
static int tls_setup(ssl_echo_ctx_t *c)
{
    int ret;

    mbedtls_ssl_init(&c->ssl);
    mbedtls_ssl_config_init(&c->conf);
    mbedtls_x509_crt_init(&c->cert);
    mbedtls_pk_init(&c->key);

    if ((ret = mbedtls_ssl_config_defaults(&c->conf, MBEDTLS_SSL_IS_SERVER,
                                           MBEDTLS_SSL_TRANSPORT_STREAM,
                                           MBEDTLS_SSL_PRESET_DEFAULT)) != 0) {
        ESP_LOGE(TAG, "[%s] ssl_config_defaults: -0x%x", c->name, (unsigned)-ret);
        return ret;
    }

    mbedtls_ssl_conf_authmode(&c->conf, MBEDTLS_SSL_VERIFY_NONE);
    /* No mbedtls_ssl_conf_rng() here: mbedTLS 4.0 removed it and draws entropy
     * from PSA Crypto instead, which ESP-IDF initializes during startup
     * (__esp_system_init_fn_mbedtls_psa_crypto_init_fn). */
    mbedtls_ssl_conf_ciphersuites(&c->conf, g_ciphersuites);
    mbedtls_ssl_conf_read_timeout(&c->conf, SSL_ECHO_TIMEOUT_MS);

    if ((ret = mbedtls_x509_crt_parse(&c->cert, SSL_SERVER_CRT_PEM,
                                      sizeof(SSL_SERVER_CRT_PEM))) < 0) {
        ESP_LOGE(TAG, "[%s] x509_crt_parse: -0x%x", c->name, (unsigned)-ret);
        return ret;
    }
    /* mbedTLS 4.0 dropped the trailing f_rng/p_rng pair from this call. */
    if ((ret = mbedtls_pk_parse_key(&c->key, SSL_SERVER_KEY_PEM, sizeof(SSL_SERVER_KEY_PEM),
                                    NULL, 0)) != 0) {
        ESP_LOGE(TAG, "[%s] pk_parse_key: -0x%x", c->name, (unsigned)-ret);
        return ret;
    }
    if ((ret = mbedtls_ssl_conf_own_cert(&c->conf, &c->cert, &c->key)) != 0) {
        ESP_LOGE(TAG, "[%s] conf_own_cert: -0x%x", c->name, (unsigned)-ret);
        return ret;
    }
    if ((ret = mbedtls_ssl_setup(&c->ssl, &c->conf)) != 0) {
        ESP_LOGE(TAG, "[%s] ssl_setup: -0x%x", c->name, (unsigned)-ret);
        return ret;
    }
    return 0;
}

/* ---- server ------------------------------------------------------------- */

/* Serve one accepted client until it goes away. */
static void serve_client(ssl_echo_ctx_t *c, uint8_t *buf, int buf_size)
{
    int ret;

    /* Reuse the same ssl context for every client, as the reference does. */
    if ((ret = mbedtls_ssl_session_reset(&c->ssl)) != 0) {
        ESP_LOGE(TAG, "[%s] session_reset: -0x%x", c->name, (unsigned)-ret);
        return;
    }
    mbedtls_ssl_set_bio(&c->ssl, c, bio_send, bio_recv, bio_recv_timeout);

    do {
        ret = mbedtls_ssl_handshake(&c->ssl);
    } while (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE);

    if (ret != 0) {
        char err[128];
        mbedtls_strerror(ret, err, sizeof(err));
        ESP_LOGW(TAG, "[%s] handshake failed -0x%x (%s)", c->name, (unsigned)-ret, err);
        return;
    }
    ESP_LOGI(TAG, "[%s] handshake OK, ciphersuite %s", c->name,
             mbedtls_ssl_get_ciphersuite(&c->ssl));

    const char *banner = SSL_ECHO_BANNER;
    mbedtls_ssl_write(&c->ssl, (const unsigned char *)banner, strlen(banner));

    while (1) {
        ret = mbedtls_ssl_read(&c->ssl, buf, buf_size - 1);
        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE ||
            ret == MBEDTLS_ERR_SSL_TIMEOUT) {
            continue;              /* idle client — keep the session open */
        }
        if (ret <= 0) {
            if (ret != MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY && ret != MBEDTLS_ERR_SSL_CONN_EOF) {
                ESP_LOGW(TAG, "[%s] ssl_read: -0x%x", c->name, (unsigned)-ret);
            }
            break;
        }

        buf[ret] = '\0';
        ESP_LOGI(TAG, "[%s] received: %s", c->name, (const char *)buf);

        int off = 0;
        while (off < ret) {         /* echo back, handle partial writes */
            int w = mbedtls_ssl_write(&c->ssl, buf + off, ret - off);
            if (w == MBEDTLS_ERR_SSL_WANT_READ || w == MBEDTLS_ERR_SSL_WANT_WRITE) {
                continue;
            }
            if (w < 0) {
                ESP_LOGW(TAG, "[%s] ssl_write: -0x%x", c->name, (unsigned)-w);
                return;
            }
            off += w;
        }
    }
}

static void ssl_echo_serve(ssl_echo_ctx_t *c, uint8_t *buf, int buf_size)
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
    ESP_LOGI(TAG, "[%s] SSL server listening on port %d", c->name, c->port);

    while (1) {
        struct sockaddr_in src;
        socklen_t sl = sizeof(src);
        int fd = ops->accept(lsock, (struct sockaddr *)&src, &sl);
        if (fd < 0) {
            continue;               /* accept timeout, or a transient error */
        }
        ESP_LOGI(TAG, "[%s] client connected", c->name);

        c->client_fd = fd;
        serve_client(c, buf, buf_size);

        mbedtls_ssl_close_notify(&c->ssl);
        ops->close(fd);
        ESP_LOGI(TAG, "[%s] client disconnected", c->name);
    }
}

/* ---- task launcher: same shape as examples/loopback ---------------------- */

static void ssl_echo_task(void *arg)
{
    ssl_echo_ctx_t *c = (ssl_echo_ctx_t *)arg;

    uint8_t *buf = malloc(SSL_ECHO_BUF_SIZE);
    if (buf == NULL) {
        ESP_LOGE(TAG, "[%s] out of memory for %d-byte buffer", c->name, SSL_ECHO_BUF_SIZE);
        goto done;
    }

    ESP_LOGI(TAG, "[%s] waiting for link...", c->name);
    while (!c->is_up()) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    if (tls_setup(c) == 0) {
        ssl_echo_serve(c, buf, SSL_ECHO_BUF_SIZE);
    }

    mbedtls_ssl_free(&c->ssl);
    mbedtls_ssl_config_free(&c->conf);
    mbedtls_x509_crt_free(&c->cert);
    mbedtls_pk_free(&c->key);
    free(buf);

done:
    free(c);
    vTaskDelete(NULL);
}

void ssl_echo_start(const char *name, const net_sock_ops_t *ops,
                    uint16_t port, bool (*is_up)(void))
{
    ssl_echo_ctx_t *c = calloc(1, sizeof(*c));
    if (c == NULL) {
        ESP_LOGE(TAG, "[%s] out of memory", name);
        return;
    }
    c->name = name;
    c->ops = ops;
    c->port = port;
    c->is_up = is_up;

    /* mbedTLS handshakes are stack-hungry: an RSA-2048 server needs well over
     * the default. The reference used 16 KB for its single task. */
    if (xTaskCreate(ssl_echo_task, name, 16384, c, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "[%s] xTaskCreate failed", name);
        free(c);
    }
}
