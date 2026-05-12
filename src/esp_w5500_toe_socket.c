/**
 * @file esp_w5500_toe_socket.c
 * @brief Thin wrappers around ioLibrary socket API for W5500 TOE.
 *
 * W5500 hardware provides 8 independent socket channels.  Each channel runs
 * its own TCP/IP engine autonomously – no host-side TCP/IP stack (lwIP) is
 * involved.
 *
 * The functions here:
 *   1. Validate arguments against Kconfig limits.
 *   2. Call the corresponding ioLibrary socket() / connect() / close() /
 *      disconnect() functions.
 *   3. Map ioLibrary error codes to esp_err_t values.
 *
 * Actual data transfer (send / recv / recvfrom / sendto) is intentionally
 * left for the application layer to call via the ioLibrary socket API
 * directly, or via higher-level helpers added in future.
 */

#include "esp_w5500_toe_socket.h"
#include "esp_w5500_toe.h"

#include "esp_log.h"
#include "socket.h"    /* ioLibrary socket API */

static const char *TAG = "w5500_socket";

/* -------------------------------------------------------------------------
 * Internal helpers
 * ---------------------------------------------------------------------- */

static bool _sn_valid(uint8_t sn)
{
    if (sn >= CONFIG_W5500_TOE_MAX_SOCKETS) {
        ESP_LOGE(TAG, "Socket %u out of range (max %d)",
                 sn, CONFIG_W5500_TOE_MAX_SOCKETS);
        return false;
    }
    return true;
}

/* Map ioLibrary negative return codes to esp_err_t */
static esp_err_t _iolib_err(int rc, const char *fn, uint8_t sn)
{
    if (rc == SOCK_OK) {
        return ESP_OK;
    }
    ESP_LOGE(TAG, "%s(sn=%u) failed, ioLib rc=%d", fn, sn, rc);
    switch (rc) {
        case SOCKERR_SOCKNUM:   return ESP_ERR_INVALID_ARG;
        case SOCKERR_SOCKMODE:  return ESP_ERR_INVALID_STATE;
        case SOCKERR_TIMEOUT:   return ESP_ERR_TIMEOUT;
        default:                return ESP_FAIL;
    }
}

/* -------------------------------------------------------------------------
 * Public socket helpers (implementations of esp_w5500_toe.h API)
 * ---------------------------------------------------------------------- */

esp_err_t esp_w5500_toe_tcp_open(uint8_t sn, uint16_t port)
{
    if (!_sn_valid(sn)) return ESP_ERR_INVALID_ARG;
    if (!esp_w5500_toe_is_ready()) return ESP_ERR_INVALID_STATE;

    ESP_LOGD(TAG, "TCP open sn=%u port=%u", sn, port);
    int rc = socket(sn, Sn_MR_TCP, port, SF_TCP_NODELAY);
    return _iolib_err(rc, "socket(TCP)", sn);
}

esp_err_t esp_w5500_toe_tcp_connect(uint8_t sn,
                                    const uint8_t dest_ip[4],
                                    uint16_t dest_port)
{
    if (!_sn_valid(sn)) return ESP_ERR_INVALID_ARG;
    if (!esp_w5500_toe_is_ready()) return ESP_ERR_INVALID_STATE;
    if (dest_ip == NULL) return ESP_ERR_INVALID_ARG;

    ESP_LOGD(TAG, "TCP connect sn=%u dst=%d.%d.%d.%d:%u",
             sn,
             dest_ip[0], dest_ip[1], dest_ip[2], dest_ip[3],
             dest_port);

    /* ioLibrary connect() takes a non-const pointer – cast is safe here   */
    int rc = connect(sn, (uint8_t *)dest_ip, dest_port);
    return _iolib_err(rc, "connect", sn);
}

esp_err_t esp_w5500_toe_udp_open(uint8_t sn, uint16_t port)
{
    if (!_sn_valid(sn)) return ESP_ERR_INVALID_ARG;
    if (!esp_w5500_toe_is_ready()) return ESP_ERR_INVALID_STATE;

    ESP_LOGD(TAG, "UDP open sn=%u port=%u", sn, port);
    int rc = socket(sn, Sn_MR_UDP, port, 0);
    return _iolib_err(rc, "socket(UDP)", sn);
}

esp_err_t esp_w5500_toe_socket_close(uint8_t sn)
{
    if (!_sn_valid(sn)) return ESP_ERR_INVALID_ARG;

    ESP_LOGD(TAG, "Close sn=%u", sn);

    /* Graceful TCP disconnect before close where applicable               */
    uint8_t sr = getSn_SR(sn);
    if (sr == SOCK_ESTABLISHED || sr == SOCK_CLOSE_WAIT) {
        disconnect(sn);
    }

    int rc = close(sn);
    return _iolib_err(rc, "close", sn);
}

/* -------------------------------------------------------------------------
 * Internal socket helper implementations (called from esp_w5500_toe.h API)
 * These are aliases so that the public header and the internal header both
 * resolve to the same functions via the linker.
 * ---------------------------------------------------------------------- */

esp_err_t w5500_toe_socket_tcp_open(uint8_t sn, uint16_t port)
{
    return esp_w5500_toe_tcp_open(sn, port);
}

esp_err_t w5500_toe_socket_tcp_connect(uint8_t sn,
                                       const uint8_t dest_ip[4],
                                       uint16_t dest_port)
{
    return esp_w5500_toe_tcp_connect(sn, dest_ip, dest_port);
}

esp_err_t w5500_toe_socket_udp_open(uint8_t sn, uint16_t port)
{
    return esp_w5500_toe_udp_open(sn, port);
}

esp_err_t w5500_toe_socket_close(uint8_t sn)
{
    return esp_w5500_toe_socket_close(sn);
}
