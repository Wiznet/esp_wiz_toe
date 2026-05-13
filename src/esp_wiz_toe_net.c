#include "esp_wiz_toe_internal.h"

#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/task.h"

#if __has_include("dhcp.h")
#include "dhcp.h"
#define ESP_WIZ_TOE_HAS_DHCP 1
#elif __has_include("DHCP.h")
#include "DHCP.h"
#define ESP_WIZ_TOE_HAS_DHCP 1
#else
#define ESP_WIZ_TOE_HAS_DHCP 0
#endif

#if __has_include("dns.h")
#include "dns.h"
#define ESP_WIZ_TOE_HAS_DNS 1
#elif __has_include("DNS.h")
#include "DNS.h"
#define ESP_WIZ_TOE_HAS_DNS 1
#else
#define ESP_WIZ_TOE_HAS_DNS 0
#endif

#if __has_include("wizchip_conf.h")
#include "wizchip_conf.h"
#define ESP_WIZ_TOE_HAS_WIZCHIP 1
#else
#define ESP_WIZ_TOE_HAS_WIZCHIP 0
#endif

#define ESP_WIZ_TOE_DHCP_SOCKET_NUM 7
#define ESP_WIZ_TOE_DNS_SOCKET_NUM 6
#define ESP_WIZ_TOE_DHCP_BUF_SIZE 548
#define ESP_WIZ_TOE_DNS_BUF_SIZE 512

static const char *TAG = "esp_wiz_toe_net";

static uint8_t s_dhcp_buf[ESP_WIZ_TOE_DHCP_BUF_SIZE];
static uint8_t s_dns_buf[ESP_WIZ_TOE_DNS_BUF_SIZE];
static bool s_dhcp_active;
static bool s_dhcp_dns_valid;
static uint8_t s_dhcp_dns[4];

static uint32_t normalize_timeout_ms(uint32_t timeout_ms)
{
    if (timeout_ms == UINT32_MAX) {
        return timeout_ms;
    }
    if (timeout_ms == 0) {
        uint32_t cfg_timeout = esp_wiz_toe_get_context()->cfg.timeout.connect_timeout_ms;
        return cfg_timeout == 0 ? 10000U : cfg_timeout;
    }
    return timeout_ms;
}

static bool timeout_expired(TickType_t start, uint32_t timeout_ms)
{
    if (timeout_ms == UINT32_MAX) {
        return false;
    }
    return (xTaskGetTickCount() - start) >= pdMS_TO_TICKS(timeout_ms);
}

static esp_err_t lock_net(uint32_t timeout_ms)
{
    esp_wiz_toe_context_t *ctx = esp_wiz_toe_get_context();
    TickType_t wait_ticks = timeout_ms == UINT32_MAX ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);

    ESP_RETURN_ON_FALSE(ctx->lock != NULL, ESP_ERR_INVALID_STATE, TAG, "net lock is not initialized");
    if (xSemaphoreTakeRecursive(ctx->lock, wait_ticks) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static void unlock_net(void)
{
    esp_wiz_toe_context_t *ctx = esp_wiz_toe_get_context();

    if (ctx->lock != NULL) {
        (void)xSemaphoreGiveRecursive(ctx->lock);
    }
}

static bool ip_is_zero(const uint8_t ip[4])
{
    return ip[0] == 0 && ip[1] == 0 && ip[2] == 0 && ip[3] == 0;
}

#if ESP_WIZ_TOE_HAS_DHCP
static bool dhcp_state_is_bound(uint8_t state)
{
#ifdef DHCP_IP_LEASED
    if (state == DHCP_IP_LEASED) {
        return true;
    }
#endif
#ifdef DHCP_IP_CHANGED
    if (state == DHCP_IP_CHANGED) {
        return true;
    }
#endif
#ifdef DHCP_IP_ASSIGN
    if (state == DHCP_IP_ASSIGN) {
        return true;
    }
#endif
    return false;
}

static bool dhcp_state_is_failed(uint8_t state)
{
#ifdef DHCP_FAILED
    if (state == DHCP_FAILED) {
        return true;
    }
#endif
#ifdef DHCP_IP_CONFLICT
    if (state == DHCP_IP_CONFLICT) {
        return true;
    }
#endif
    return false;
}
#endif

esp_err_t esp_wiz_toe_dhcp_start(uint32_t timeout_ms)
{
    ESP_RETURN_ON_FALSE(esp_wiz_toe_is_ready(), ESP_ERR_INVALID_STATE, TAG, "component not initialized");

#if !(ESP_WIZ_TOE_HAS_DHCP && ESP_WIZ_TOE_HAS_WIZCHIP)
    (void)timeout_ms;
    return ESP_ERR_NOT_SUPPORTED;
#else
    timeout_ms = normalize_timeout_ms(timeout_ms);
    TickType_t start = xTaskGetTickCount();

    if (s_dhcp_active) {
        return ESP_OK;
    }

    memset(s_dhcp_buf, 0, sizeof(s_dhcp_buf));
    ESP_RETURN_ON_ERROR(lock_net(timeout_ms), TAG, "net lock timeout");
    DHCP_init(ESP_WIZ_TOE_DHCP_SOCKET_NUM, s_dhcp_buf);
    unlock_net();

    while (true) {
        ESP_RETURN_ON_ERROR(lock_net(timeout_ms), TAG, "net lock timeout");
        uint8_t state = DHCP_run();
        unlock_net();

        if (dhcp_state_is_bound(state)) {
            uint8_t ip[4] = {0};
            uint8_t sn[4] = {0};
            uint8_t gw[4] = {0};
            uint8_t dns[4] = {0};
            wiz_NetInfo net = {0};

            getIPfromDHCP(ip);
            getSNfromDHCP(sn);
            getGWfromDHCP(gw);
            getDNSfromDHCP(dns);

            memcpy(net.mac, esp_wiz_toe_get_context()->cfg.mac.mac, sizeof(net.mac));
            memcpy(net.ip, ip, sizeof(net.ip));
            memcpy(net.sn, sn, sizeof(net.sn));
            memcpy(net.gw, gw, sizeof(net.gw));
            memcpy(net.dns, dns, sizeof(net.dns));
            net.dhcp = NETINFO_DHCP;

            ESP_RETURN_ON_ERROR(lock_net(timeout_ms), TAG, "net lock timeout");
            wizchip_setnetinfo(&net);
            unlock_net();
            s_dhcp_active = true;
            s_dhcp_dns_valid = !ip_is_zero(dns);
            if (s_dhcp_dns_valid) {
                memcpy(s_dhcp_dns, dns, sizeof(s_dhcp_dns));
            }

            ESP_LOGI(TAG, "DHCP leased IP: %u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
            return ESP_OK;
        }

        if (dhcp_state_is_failed(state)) {
            ESP_RETURN_ON_ERROR(lock_net(timeout_ms), TAG, "net lock timeout");
            DHCP_stop();
            unlock_net();
            return ESP_FAIL;
        }

        if (timeout_expired(start, timeout_ms)) {
            ESP_RETURN_ON_ERROR(lock_net(timeout_ms), TAG, "net lock timeout");
            DHCP_stop();
            unlock_net();
            return ESP_ERR_TIMEOUT;
        }

        vTaskDelay(pdMS_TO_TICKS(200));
    }
#endif
}

esp_err_t esp_wiz_toe_dhcp_stop(void)
{
#if !ESP_WIZ_TOE_HAS_DHCP
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (!s_dhcp_active) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(lock_net(1000U), TAG, "net lock timeout");
    DHCP_stop();
    unlock_net();
    s_dhcp_active = false;
    s_dhcp_dns_valid = false;
    memset(s_dhcp_dns, 0, sizeof(s_dhcp_dns));
    return ESP_OK;
#endif
}

esp_err_t esp_wiz_toe_dns_resolve(const char *hostname, uint8_t out_ip[4], uint32_t timeout_ms)
{
    ESP_RETURN_ON_FALSE(esp_wiz_toe_is_ready(), ESP_ERR_INVALID_STATE, TAG, "component not initialized");
    ESP_RETURN_ON_FALSE(hostname != NULL, ESP_ERR_INVALID_ARG, TAG, "hostname is NULL");
    ESP_RETURN_ON_FALSE(out_ip != NULL, ESP_ERR_INVALID_ARG, TAG, "out_ip is NULL");

#if !(ESP_WIZ_TOE_HAS_DNS)
    (void)timeout_ms;
    return ESP_ERR_NOT_SUPPORTED;
#else
    timeout_ms = normalize_timeout_ms(timeout_ms);
    uint8_t dns_server[4] = {0};
    uint8_t resolved[4] = {0};
    TickType_t start = xTaskGetTickCount();

    if (s_dhcp_dns_valid) {
        memcpy(dns_server, s_dhcp_dns, sizeof(dns_server));
    } else if (!ip_is_zero(esp_wiz_toe_get_context()->cfg.static_ip.dns)) {
        memcpy(dns_server, esp_wiz_toe_get_context()->cfg.static_ip.dns, sizeof(dns_server));
    } else {
        return ESP_ERR_INVALID_STATE;
    }

    memset(s_dns_buf, 0, sizeof(s_dns_buf));
    ESP_RETURN_ON_ERROR(lock_net(timeout_ms), TAG, "net lock timeout");
    DNS_init(ESP_WIZ_TOE_DNS_SOCKET_NUM, s_dns_buf);
    unlock_net();

    while (true) {
        ESP_RETURN_ON_ERROR(lock_net(timeout_ms), TAG, "net lock timeout");
        int8_t rc = DNS_run(dns_server, (uint8_t *)hostname, resolved);
        unlock_net();

        if (rc == 1) {
            memcpy(out_ip, resolved, 4);
            return ESP_OK;
        }
        if (rc < 0) {
            return ESP_FAIL;
        }
        if (timeout_expired(start, timeout_ms)) {
            return ESP_ERR_TIMEOUT;
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
#endif
}
