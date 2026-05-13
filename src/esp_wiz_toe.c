#include "esp_wiz_toe_internal.h"

#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/task.h"

#if __has_include("wizchip_conf.h")
#include "wizchip_conf.h"
#define ESP_WIZ_TOE_HAS_IOLIB 1
#else
#define ESP_WIZ_TOE_HAS_IOLIB 0
#endif

static const char *TAG = "esp_wiz_toe";

#ifdef CONFIG_ESP_WIZ_TOE_TX_BUF_KB
#define ESP_WIZ_TOE_SOCKET_TX_BUF_KB CONFIG_ESP_WIZ_TOE_TX_BUF_KB
#else
#define ESP_WIZ_TOE_SOCKET_TX_BUF_KB 2
#endif

#ifdef CONFIG_ESP_WIZ_TOE_RX_BUF_KB
#define ESP_WIZ_TOE_SOCKET_RX_BUF_KB CONFIG_ESP_WIZ_TOE_RX_BUF_KB
#else
#define ESP_WIZ_TOE_SOCKET_RX_BUF_KB 2
#endif

static esp_wiz_toe_context_t s_ctx;

static bool mac_is_zero(const uint8_t mac[6])
{
    return mac[0] == 0 && mac[1] == 0 && mac[2] == 0 && mac[3] == 0 && mac[4] == 0 && mac[5] == 0;
}

static esp_err_t hw_reset_sequence(void)
{
    if (s_ctx.cfg.spi.pin_rst < 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    gpio_set_level(s_ctx.cfg.spi.pin_rst, 0);
    vTaskDelay(pdMS_TO_TICKS(2));
    gpio_set_level(s_ctx.cfg.spi.pin_rst, 1);
    vTaskDelay(pdMS_TO_TICKS(150));

    return ESP_OK;
}

#if ESP_WIZ_TOE_HAS_IOLIB
static void fill_netinfo_from_config(wiz_NetInfo *net)
{
    memset(net, 0, sizeof(*net));

    memcpy(net->mac, s_ctx.cfg.mac.mac, sizeof(net->mac));
    net->dhcp = (s_ctx.cfg.ip_mode == ESP_WIZ_TOE_IP_MODE_DHCP) ? NETINFO_DHCP : NETINFO_STATIC;

    if (net->dhcp == NETINFO_STATIC) {
        memcpy(net->ip, s_ctx.cfg.static_ip.ip, sizeof(net->ip));
        memcpy(net->sn, s_ctx.cfg.static_ip.netmask, sizeof(net->sn));
        memcpy(net->gw, s_ctx.cfg.static_ip.gateway, sizeof(net->gw));
        memcpy(net->dns, s_ctx.cfg.static_ip.dns, sizeof(net->dns));
    }
}
#endif

esp_wiz_toe_context_t *esp_wiz_toe_get_context(void)
{
    return &s_ctx;
}

esp_err_t esp_wiz_toe_init(const esp_wiz_toe_config_t *config)
{
    esp_err_t ret = ESP_OK;

    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, TAG, "config is NULL");

    if (s_ctx.initialized) {
        ESP_LOGW(TAG, "already initialized");
        return ESP_OK;
    }

    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.cfg = *config;

    if (mac_is_zero(s_ctx.cfg.mac.mac)) {
        // Locally-administered default MAC for scaffold bring-up.
        static const uint8_t default_mac[6] = {0x02, 0x00, 0x00, 0x00, 0x55, 0x00};
        memcpy(s_ctx.cfg.mac.mac, default_mac, sizeof(default_mac));
    }

    ESP_GOTO_ON_ERROR(esp_wiz_toe_port_init(&s_ctx.cfg), fail, TAG, "port init failed");
    ESP_GOTO_ON_ERROR(esp_wiz_toe_port_register_iolib_callbacks(), fail, TAG, "SPI callback registration failed");
    ESP_GOTO_ON_ERROR(hw_reset_sequence(), fail, TAG, "hardware reset failed");

#if ESP_WIZ_TOE_HAS_IOLIB
    uint8_t tx_buf[8] = {
        ESP_WIZ_TOE_SOCKET_TX_BUF_KB, ESP_WIZ_TOE_SOCKET_TX_BUF_KB,
        ESP_WIZ_TOE_SOCKET_TX_BUF_KB, ESP_WIZ_TOE_SOCKET_TX_BUF_KB,
        ESP_WIZ_TOE_SOCKET_TX_BUF_KB, ESP_WIZ_TOE_SOCKET_TX_BUF_KB,
        ESP_WIZ_TOE_SOCKET_TX_BUF_KB, ESP_WIZ_TOE_SOCKET_TX_BUF_KB,
    };
    uint8_t rx_buf[8] = {
        ESP_WIZ_TOE_SOCKET_RX_BUF_KB, ESP_WIZ_TOE_SOCKET_RX_BUF_KB,
        ESP_WIZ_TOE_SOCKET_RX_BUF_KB, ESP_WIZ_TOE_SOCKET_RX_BUF_KB,
        ESP_WIZ_TOE_SOCKET_RX_BUF_KB, ESP_WIZ_TOE_SOCKET_RX_BUF_KB,
        ESP_WIZ_TOE_SOCKET_RX_BUF_KB, ESP_WIZ_TOE_SOCKET_RX_BUF_KB,
    };
    wiz_NetInfo netinfo = {0};
    wiz_NetInfo netinfo_readback = {0};

    ESP_GOTO_ON_FALSE(wizchip_init(tx_buf, rx_buf) == 0, ESP_FAIL, fail, TAG, "wizchip_init failed");

    fill_netinfo_from_config(&netinfo);
    wizchip_setnetinfo(&netinfo);
    wizchip_getnetinfo(&netinfo_readback);

    ESP_LOGI(TAG,
             "netinfo mac=%02x:%02x:%02x:%02x:%02x:%02x ip=%u.%u.%u.%u",
             netinfo_readback.mac[0], netinfo_readback.mac[1], netinfo_readback.mac[2],
             netinfo_readback.mac[3], netinfo_readback.mac[4], netinfo_readback.mac[5],
             netinfo_readback.ip[0], netinfo_readback.ip[1], netinfo_readback.ip[2], netinfo_readback.ip[3]);

    {
        bool link_up = esp_wiz_toe_port_link_is_up();
        ESP_LOGI(TAG, "PHY link: %s", link_up ? "UP" : "DOWN");
    }
#else
    ret = ESP_ERR_NOT_SUPPORTED;
    goto fail;
#endif
    s_ctx.initialized = true;
    ESP_LOGI(TAG, "W5500 TOE initialized");

    return ESP_OK;

fail:
    (void)esp_wiz_toe_port_deinit();
    memset(&s_ctx, 0, sizeof(s_ctx));
    return ret;
}

esp_err_t esp_wiz_toe_deinit(void)
{
    if (!s_ctx.initialized) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(esp_wiz_toe_port_deinit(), TAG, "port deinit failed");
    memset(&s_ctx, 0, sizeof(s_ctx));
    return ESP_OK;
}

bool esp_wiz_toe_is_ready(void)
{
    return s_ctx.initialized;
}

esp_err_t esp_wiz_toe_reset(void)
{
    ESP_RETURN_ON_FALSE(s_ctx.initialized, ESP_ERR_INVALID_STATE, TAG, "component not initialized");
    return hw_reset_sequence();
}

esp_err_t esp_wiz_toe_is_link_up(bool *is_up)
{
    ESP_RETURN_ON_FALSE(is_up != NULL, ESP_ERR_INVALID_ARG, TAG, "is_up is NULL");
    ESP_RETURN_ON_FALSE(s_ctx.initialized, ESP_ERR_INVALID_STATE, TAG, "component not initialized");

    *is_up = esp_wiz_toe_port_link_is_up();
    return ESP_OK;
}

esp_err_t esp_wiz_toe_get_link_status(esp_wiz_toe_link_status_t *status)
{
    bool is_up = false;

    ESP_RETURN_ON_FALSE(status != NULL, ESP_ERR_INVALID_ARG, TAG, "status is NULL");

    esp_err_t ret = esp_wiz_toe_is_link_up(&is_up);
    if (ret != ESP_OK) {
        *status = ESP_WIZ_TOE_LINK_STATUS_UNKNOWN;
        return ret;
    }

    *status = is_up ? ESP_WIZ_TOE_LINK_STATUS_UP : ESP_WIZ_TOE_LINK_STATUS_DOWN;
    return ESP_OK;
}
