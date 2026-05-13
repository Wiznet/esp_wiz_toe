#include "esp_wiz_toe_internal.h"

#include <stdint.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/task.h"

#if __has_include("Ethernet/socket.h")
#include "Ethernet/socket.h"
#define ESP_WIZ_TOE_HAS_SOCKET_API 1
#else
#define ESP_WIZ_TOE_HAS_SOCKET_API 0
#endif

static const char *TAG = "esp_wiz_toe_socket";

/*
 * MVP: use one global mutex (component-wide recursive mutex) for all sockets.
 * Future extension: replace with dedicated socket manager and per-socket locks.
 */
static inline esp_wiz_toe_context_t *ctx_get(void)
{
    return esp_wiz_toe_get_context();
}

static TickType_t to_wait_ticks(uint32_t timeout_ms, uint32_t default_timeout_ms)
{
    if (timeout_ms == UINT32_MAX) {
        return portMAX_DELAY;
    }
    if (timeout_ms == 0) {
        timeout_ms = default_timeout_ms;
    }
    return pdMS_TO_TICKS(timeout_ms);
}

static bool timeout_expired(TickType_t start, uint32_t timeout_ms)
{
    if (timeout_ms == UINT32_MAX) {
        return false;
    }
    return (xTaskGetTickCount() - start) >= pdMS_TO_TICKS(timeout_ms);
}

static esp_err_t lock_socket(uint32_t timeout_ms)
{
    esp_wiz_toe_context_t *ctx = ctx_get();
    TickType_t wait_ticks = to_wait_ticks(timeout_ms, 1000U);

    ESP_RETURN_ON_FALSE(ctx->lock != NULL, ESP_ERR_INVALID_STATE, TAG, "socket lock is not initialized");
    if (xSemaphoreTakeRecursive(ctx->lock, wait_ticks) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static void unlock_socket(void)
{
    esp_wiz_toe_context_t *ctx = ctx_get();

    if (ctx->lock != NULL) {
        (void)xSemaphoreGiveRecursive(ctx->lock);
    }
}

static esp_err_t map_socket_error(int rc)
{
    if (rc >= 0) {
        return ESP_OK;
    }
#if ESP_WIZ_TOE_HAS_SOCKET_API
    if (rc == SOCK_BUSY) {
        return ESP_ERR_INVALID_STATE;
    }
#ifdef SOCKERR_TIMEOUT
    if (rc == SOCKERR_TIMEOUT) {
        return ESP_ERR_TIMEOUT;
    }
#endif
#ifdef SOCKERR_SOCKNUM
    if (rc == SOCKERR_SOCKNUM) {
        return ESP_ERR_INVALID_ARG;
    }
#endif
#ifdef SOCKERR_ARG
    if (rc == SOCKERR_ARG) {
        return ESP_ERR_INVALID_ARG;
    }
#endif
#endif
    return ESP_FAIL;
}

static esp_err_t wait_for_state(uint8_t sn, uint8_t expected_state, uint32_t timeout_ms)
{
#if ESP_WIZ_TOE_HAS_SOCKET_API
    TickType_t start = xTaskGetTickCount();

    while (true) {
        esp_err_t lock_ret = lock_socket(timeout_ms);
        if (lock_ret != ESP_OK) {
            return lock_ret;
        }
        uint8_t st = getSn_SR(sn);
        unlock_socket();

        if (st == expected_state) {
            return ESP_OK;
        }
        if (st == SOCK_CLOSED) {
            return ESP_FAIL;
        }
        if (timeout_expired(start, timeout_ms)) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
#else
    (void)sn;
    (void)expected_state;
    (void)timeout_ms;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t esp_wiz_toe_socket(uint8_t sn,
                             uint8_t protocol,
                             uint16_t local_port,
                             uint8_t flag,
                             uint32_t timeout_ms)
{
    ESP_RETURN_ON_FALSE(esp_wiz_toe_is_ready(), ESP_ERR_INVALID_STATE, TAG, "component not initialized");

#if ESP_WIZ_TOE_HAS_SOCKET_API
    esp_err_t ret = lock_socket(timeout_ms);
    if (ret != ESP_OK) {
        return ret;
    }

    int rc = socket(sn, protocol, local_port, flag);
    unlock_socket();

    return map_socket_error(rc);
#else
    (void)sn;
    (void)protocol;
    (void)local_port;
    (void)flag;
    (void)timeout_ms;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t esp_wiz_toe_connect(uint8_t sn,
                              const uint8_t remote_ip[4],
                              uint16_t remote_port,
                              uint32_t timeout_ms)
{
    ESP_RETURN_ON_FALSE(esp_wiz_toe_is_ready(), ESP_ERR_INVALID_STATE, TAG, "component not initialized");
    ESP_RETURN_ON_FALSE(remote_ip != NULL, ESP_ERR_INVALID_ARG, TAG, "remote_ip is NULL");

#if ESP_WIZ_TOE_HAS_SOCKET_API
    TickType_t start = xTaskGetTickCount();

    while (true) {
        esp_err_t ret = lock_socket(timeout_ms);
        if (ret != ESP_OK) {
            return ret;
        }
        int rc = connect(sn, (uint8_t *)remote_ip, remote_port);
        unlock_socket();

        if (rc >= 0) {
            break;
        }
        if (rc != SOCK_BUSY) {
            return map_socket_error(rc);
        }
        if (timeout_expired(start, timeout_ms)) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    return wait_for_state(sn, SOCK_ESTABLISHED, timeout_ms);
#else
    (void)sn;
    (void)remote_port;
    (void)timeout_ms;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t esp_wiz_toe_listen(uint8_t sn, uint32_t timeout_ms)
{
    ESP_RETURN_ON_FALSE(esp_wiz_toe_is_ready(), ESP_ERR_INVALID_STATE, TAG, "component not initialized");

#if ESP_WIZ_TOE_HAS_SOCKET_API
    esp_err_t ret = lock_socket(timeout_ms);
    if (ret != ESP_OK) {
        return ret;
    }

    int rc = listen(sn);
    unlock_socket();
    if (rc < 0) {
        return map_socket_error(rc);
    }

    return wait_for_state(sn, SOCK_LISTEN, timeout_ms);
#else
    (void)sn;
    (void)timeout_ms;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t esp_wiz_toe_tcp_connect(uint8_t sn,
                                  const uint8_t remote_ip[4],
                                  uint16_t remote_port,
                                  uint16_t local_port,
                                  uint32_t timeout_ms)
{
    ESP_RETURN_ON_FALSE(esp_wiz_toe_is_ready(), ESP_ERR_INVALID_STATE, TAG, "component not initialized");
    ESP_RETURN_ON_FALSE(remote_ip != NULL, ESP_ERR_INVALID_ARG, TAG, "remote_ip is NULL");

#if ESP_WIZ_TOE_HAS_SOCKET_API
    esp_err_t ret = esp_wiz_toe_socket(sn, Sn_MR_TCP, local_port, 0, timeout_ms);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = esp_wiz_toe_connect(sn, remote_ip, remote_port, timeout_ms);
    if (ret != ESP_OK) {
        (void)esp_wiz_toe_close(sn);
    }
    return ret;
#else
    (void)sn;
    (void)remote_port;
    (void)local_port;
    (void)timeout_ms;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t esp_wiz_toe_tcp_listen(uint8_t sn, uint16_t local_port, uint32_t timeout_ms)
{
    ESP_RETURN_ON_FALSE(esp_wiz_toe_is_ready(), ESP_ERR_INVALID_STATE, TAG, "component not initialized");

#if ESP_WIZ_TOE_HAS_SOCKET_API
    esp_err_t ret = esp_wiz_toe_socket(sn, Sn_MR_TCP, local_port, 0, timeout_ms);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = esp_wiz_toe_listen(sn, timeout_ms);
    if (ret != ESP_OK) {
        (void)esp_wiz_toe_close(sn);
    }
    return ret;
#else
    (void)sn;
    (void)local_port;
    (void)timeout_ms;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t esp_wiz_toe_tcp_accept_wait(uint8_t sn, uint32_t timeout_ms)
{
    ESP_RETURN_ON_FALSE(esp_wiz_toe_is_ready(), ESP_ERR_INVALID_STATE, TAG, "component not initialized");

#if ESP_WIZ_TOE_HAS_SOCKET_API
    TickType_t start = xTaskGetTickCount();

    while (true) {
        esp_err_t ret = lock_socket(timeout_ms);
        if (ret != ESP_OK) {
            return ret;
        }
        uint8_t st = getSn_SR(sn);
        unlock_socket();

        if (st == SOCK_ESTABLISHED || st == SOCK_CLOSE_WAIT) {
            return ESP_OK;
        }
        if (st != SOCK_LISTEN && st != SOCK_SYNSENT && st != SOCK_SYNRECV) {
            return ESP_FAIL;
        }
        if (timeout_expired(start, timeout_ms)) {
            return ESP_ERR_TIMEOUT;
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
#else
    (void)sn;
    (void)timeout_ms;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t esp_wiz_toe_udp_open(uint8_t sn, uint16_t local_port, uint32_t timeout_ms)
{
    ESP_RETURN_ON_FALSE(esp_wiz_toe_is_ready(), ESP_ERR_INVALID_STATE, TAG, "component not initialized");

#if ESP_WIZ_TOE_HAS_SOCKET_API
    esp_err_t ret = lock_socket(timeout_ms);
    if (ret != ESP_OK) {
        return ret;
    }

    int rc = socket(sn, Sn_MR_UDP, local_port, 0);
    unlock_socket();
    return map_socket_error(rc);
#else
    (void)sn;
    (void)local_port;
    (void)timeout_ms;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t esp_wiz_toe_udp_sendto(uint8_t sn,
                                 const void *data,
                                 size_t len,
                                 const uint8_t dest_ip[4],
                                 uint16_t dest_port,
                                 uint32_t timeout_ms,
                                 size_t *sent_len)
{
    ESP_RETURN_ON_FALSE(esp_wiz_toe_is_ready(), ESP_ERR_INVALID_STATE, TAG, "component not initialized");
    ESP_RETURN_ON_FALSE(data != NULL, ESP_ERR_INVALID_ARG, TAG, "data is NULL");
    ESP_RETURN_ON_FALSE(dest_ip != NULL, ESP_ERR_INVALID_ARG, TAG, "dest_ip is NULL");
    ESP_RETURN_ON_FALSE(len > 0, ESP_ERR_INVALID_ARG, TAG, "len must be > 0");
    ESP_RETURN_ON_FALSE(sent_len != NULL, ESP_ERR_INVALID_ARG, TAG, "sent_len is NULL");

    *sent_len = 0;

#if ESP_WIZ_TOE_HAS_SOCKET_API
    const uint8_t *ptr = (const uint8_t *)data;
    TickType_t start = xTaskGetTickCount();

    while (*sent_len < len) {
        esp_err_t ret = lock_socket(timeout_ms);
        if (ret != ESP_OK) {
            return ret;
        }

        int rc = sendto(sn,
                        (uint8_t *)(ptr + *sent_len),
                        (uint16_t)(len - *sent_len),
                        (uint8_t *)dest_ip,
                        dest_port);
        unlock_socket();

        if (rc > 0) {
            *sent_len += (size_t)rc;
            continue;
        }
        if (rc != SOCK_BUSY) {
            return map_socket_error(rc);
        }
        if (timeout_expired(start, timeout_ms)) {
            return ESP_ERR_TIMEOUT;
        }

        vTaskDelay(pdMS_TO_TICKS(5));
    }

    return ESP_OK;
#else
    (void)sn;
    (void)timeout_ms;
    (void)dest_port;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t esp_wiz_toe_udp_recvfrom(uint8_t sn,
                                   void *buf,
                                   size_t len,
                                   uint32_t timeout_ms,
                                   size_t *recv_len,
                                   uint8_t src_ip[4],
                                   uint16_t *src_port)
{
    ESP_RETURN_ON_FALSE(esp_wiz_toe_is_ready(), ESP_ERR_INVALID_STATE, TAG, "component not initialized");
    ESP_RETURN_ON_FALSE(buf != NULL, ESP_ERR_INVALID_ARG, TAG, "buf is NULL");
    ESP_RETURN_ON_FALSE(len > 0, ESP_ERR_INVALID_ARG, TAG, "len must be > 0");
    ESP_RETURN_ON_FALSE(recv_len != NULL, ESP_ERR_INVALID_ARG, TAG, "recv_len is NULL");

    *recv_len = 0;
    if (src_port != NULL) {
        *src_port = 0;
    }
    if (src_ip != NULL) {
        src_ip[0] = 0;
        src_ip[1] = 0;
        src_ip[2] = 0;
        src_ip[3] = 0;
    }

#if ESP_WIZ_TOE_HAS_SOCKET_API
    uint8_t remote_ip[4] = {0};
    uint16_t remote_port = 0;
    TickType_t start = xTaskGetTickCount();

    while (true) {
        esp_err_t ret = lock_socket(timeout_ms);
        if (ret != ESP_OK) {
            return ret;
        }

        int rc = recvfrom(sn, (uint8_t *)buf, (uint16_t)len, remote_ip, &remote_port);
        unlock_socket();

        if (rc > 0) {
            *recv_len = (size_t)rc;
            if (src_ip != NULL) {
                src_ip[0] = remote_ip[0];
                src_ip[1] = remote_ip[1];
                src_ip[2] = remote_ip[2];
                src_ip[3] = remote_ip[3];
            }
            if (src_port != NULL) {
                *src_port = remote_port;
            }
            return ESP_OK;
        }
        if (rc != SOCK_BUSY) {
            return map_socket_error(rc);
        }
        if (timeout_expired(start, timeout_ms)) {
            return ESP_ERR_TIMEOUT;
        }

        vTaskDelay(pdMS_TO_TICKS(5));
    }
#else
    (void)sn;
    (void)timeout_ms;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t esp_wiz_toe_send(uint8_t sn,
                           const void *data,
                           size_t len,
                           uint32_t timeout_ms,
                           size_t *sent_len)
{
    ESP_RETURN_ON_FALSE(esp_wiz_toe_is_ready(), ESP_ERR_INVALID_STATE, TAG, "component not initialized");
    ESP_RETURN_ON_FALSE(data != NULL, ESP_ERR_INVALID_ARG, TAG, "data is NULL");
    ESP_RETURN_ON_FALSE(len > 0, ESP_ERR_INVALID_ARG, TAG, "len must be > 0");
    ESP_RETURN_ON_FALSE(sent_len != NULL, ESP_ERR_INVALID_ARG, TAG, "sent_len is NULL");

    *sent_len = 0;

#if ESP_WIZ_TOE_HAS_SOCKET_API
    const uint8_t *ptr = (const uint8_t *)data;
    TickType_t start = xTaskGetTickCount();

    while (*sent_len < len) {
        esp_err_t ret = lock_socket(timeout_ms);
        if (ret != ESP_OK) {
            return ret;
        }

        uint8_t st = getSn_SR(sn);
        if (st != SOCK_ESTABLISHED && st != SOCK_CLOSE_WAIT) {
            unlock_socket();
            return ESP_ERR_INVALID_STATE;
        }

        int rc = send(sn, (uint8_t *)(ptr + *sent_len), (uint16_t)(len - *sent_len));
        unlock_socket();

        if (rc > 0) {
            *sent_len += (size_t)rc;
            continue;
        }
        if (rc != SOCK_BUSY) {
            return map_socket_error(rc);
        }
        if (timeout_expired(start, timeout_ms)) {
            return ESP_ERR_TIMEOUT;
        }

        vTaskDelay(pdMS_TO_TICKS(5));
    }

    return ESP_OK;
#else
    (void)sn;
    (void)timeout_ms;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t esp_wiz_toe_recv(uint8_t sn,
                           void *buf,
                           size_t len,
                           uint32_t timeout_ms,
                           size_t *recv_len)
{
    ESP_RETURN_ON_FALSE(esp_wiz_toe_is_ready(), ESP_ERR_INVALID_STATE, TAG, "component not initialized");
    ESP_RETURN_ON_FALSE(buf != NULL, ESP_ERR_INVALID_ARG, TAG, "buf is NULL");
    ESP_RETURN_ON_FALSE(len > 0, ESP_ERR_INVALID_ARG, TAG, "len must be > 0");
    ESP_RETURN_ON_FALSE(recv_len != NULL, ESP_ERR_INVALID_ARG, TAG, "recv_len is NULL");

    *recv_len = 0;

#if ESP_WIZ_TOE_HAS_SOCKET_API
    TickType_t start = xTaskGetTickCount();

    while (true) {
        esp_err_t ret = lock_socket(timeout_ms);
        if (ret != ESP_OK) {
            return ret;
        }

        uint8_t st = getSn_SR(sn);
        if (st != SOCK_ESTABLISHED && st != SOCK_CLOSE_WAIT) {
            unlock_socket();
            return ESP_ERR_INVALID_STATE;
        }

        int rc = recv(sn, (uint8_t *)buf, (uint16_t)len);
        unlock_socket();

        if (rc > 0) {
            *recv_len = (size_t)rc;
            return ESP_OK;
        }
        if (rc != SOCK_BUSY) {
            return map_socket_error(rc);
        }
        if (timeout_expired(start, timeout_ms)) {
            return ESP_ERR_TIMEOUT;
        }

        vTaskDelay(pdMS_TO_TICKS(5));
    }
#else
    (void)sn;
    (void)timeout_ms;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t esp_wiz_toe_close(uint8_t sn)
{
    ESP_RETURN_ON_FALSE(esp_wiz_toe_is_ready(), ESP_ERR_INVALID_STATE, TAG, "component not initialized");

#if ESP_WIZ_TOE_HAS_SOCKET_API
    esp_err_t ret = lock_socket(1000U);
    if (ret != ESP_OK) {
        return ret;
    }

    uint8_t st = getSn_SR(sn);
    if (st == SOCK_ESTABLISHED || st == SOCK_CLOSE_WAIT) {
        (void)disconnect(sn);
    }

    int rc = close(sn);
    unlock_socket();
    return map_socket_error(rc);
#else
    (void)sn;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}
