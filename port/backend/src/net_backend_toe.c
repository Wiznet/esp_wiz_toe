/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * TOE backend (CONFIG_WSM_DRIVER_BACKEND_TOE) implementation of the
 * backend-neutral harness (net_backend.h). The W5500/W6300 runs the TCP/IP
 * stack in hardware (ioLibrary); no data flows through lwIP.
 *
 * Config conventions follow wsm_driver:
 *   - pins / SPI -> wsm_driver_spi_config_t built from Kconfig (CONFIG_WSM_DRIVER_*),
 *                   applied by wsm_driver_spi_init() (see wsm_driver.h).
 *   - network    -> the caller's wiz_NetInfo, applied with wizchip_setnetinfo().
 *
 * A SHADOW esp_netif holds the same IPv4 identity: esp_netif_init() also
 * registers the lwIP socket VFS fd-range, which is what makes close()/read()/
 * write() on the TOE fds dispatch to __wrap_lwip_* when CONFIG_WSM_DRIVER_
 * SOCKET_WRAP is enabled (see ioLibrary_Driver/wiztoe_wrap.c). No driver is
 * attached to the shadow netif and no data passes through it.
 *
 * Does NOT include the ioLibrary "socket.h" (only wizchip_conf.h), so there is
 * no socket()/close() name clash in this TU.
 */
#include <string.h>
#include <stdbool.h>

#include "sdkconfig.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"

#include "wizchip_conf.h"   /* wiz_NetInfo, wizchip_init, wizchip_setnetinfo */

#include "wsm_driver.h"    /* wsm_driver_spi_config_t + Kconfig SPI transport */
#include "net_backend.h"

static const char *TAG = "wiztoe_net";
static esp_netif_t *s_shadow;
static bool s_net_up;

/* Per-socket buffer sizes (KB) from Kconfig, applied to all 8 sockets. */
#ifdef CONFIG_WSM_DRIVER_TX_BUF_KB
#define TOE_TX_BUF_KB CONFIG_WSM_DRIVER_TX_BUF_KB
#else
#define TOE_TX_BUF_KB 2
#endif
#ifdef CONFIG_WSM_DRIVER_RX_BUF_KB
#define TOE_RX_BUF_KB CONFIG_WSM_DRIVER_RX_BUF_KB
#else
#define TOE_RX_BUF_KB 2
#endif

/* Fill the SPI/pin config from the component Kconfig (wsm_driver convention). */
static void fill_spi_config(wsm_driver_spi_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->host_id  = (spi_host_device_t)CONFIG_WSM_DRIVER_SPI_HOST;
    cfg->clock_hz = CONFIG_WSM_DRIVER_SPI_CLOCK_HZ;
    cfg->pin_miso = (gpio_num_t)CONFIG_WSM_DRIVER_PIN_MISO;
    cfg->pin_mosi = (gpio_num_t)CONFIG_WSM_DRIVER_PIN_MOSI;
    cfg->pin_sclk = (gpio_num_t)CONFIG_WSM_DRIVER_PIN_SCLK;
    cfg->pin_cs   = (gpio_num_t)CONFIG_WSM_DRIVER_PIN_CS;
    cfg->pin_rst  = (gpio_num_t)CONFIG_WSM_DRIVER_PIN_RST;
    cfg->pin_int  = (gpio_num_t)CONFIG_WSM_DRIVER_PIN_INT;
#if defined(CONFIG_WSM_DRIVER_QSPI_QUAD)
    cfg->pin_io2  = (gpio_num_t)CONFIG_WSM_DRIVER_PIN_IO2;
    cfg->pin_io3  = (gpio_num_t)CONFIG_WSM_DRIVER_PIN_IO3;
#else
    cfg->pin_io2  = (gpio_num_t)-1;
    cfg->pin_io3  = (gpio_num_t)-1;
#endif
}

/* esp_netif_init() / esp_event_loop_create_default() are process-wide singletons.
 * ESP_ERR_INVALID_STATE from either means "someone already did this", which is a
 * normal outcome -- not an error -- for a component that shares the application
 * with other stacks (Wi-Fi, mDNS, an existing esp_netif, ...). Treat it as
 * success and let real failures through. */
static esp_err_t toe_ensure_lwip_core(void)
{
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_event_loop_create_default();
    if (err == ESP_ERR_INVALID_STATE) {
        ESP_LOGD(TAG, "default event loop already exists (kept)");
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_event_loop_create_default failed: %s", esp_err_to_name(err));
        return err;
    }
    return ESP_OK;
}

esp_err_t wiznet_toe_bringup(const wsm_driver_spi_config_t *spi_cfg,
                             const wiz_NetInfo *net_info)
{
    esp_err_t err;

    if (spi_cfg == NULL || net_info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* SPI transport. Whether the bus itself is initialized here or was already
     * brought up by the caller is decided by spi_cfg->bus_initialized_by_caller. */
    err = wsm_driver_spi_init(spi_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wsm_driver_spi_init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = wsm_driver_spi_register_iolib_callbacks();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register iolib callbacks failed: %s", esp_err_to_name(err));
        return err;
    }

    err = wsm_driver_spi_reset();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "chip reset failed: %s", esp_err_to_name(err));
        return err;
    }

    if (wsm_driver_spi_wizchip_check() != ESP_OK) {
        ESP_LOGW(TAG, "wizchip id/version check failed (continuing)");
    }

    /* Hardware TCP/IP engine: per-socket buffers from Kconfig (both symbols have
     * a prompt, so they are settable from sdkconfig without touching this code). */
    uint8_t tx[8], rx[8];
    memset(tx, TOE_TX_BUF_KB, sizeof(tx));
    memset(rx, TOE_RX_BUF_KB, sizeof(rx));
    if (wizchip_init(tx, rx) != 0) {
        ESP_LOGE(TAG, "wizchip_init failed");
        return ESP_FAIL;
    }

    /* The CHIP owns the IP in TOE mode. */
    wizchip_setnetinfo((wiz_NetInfo *)net_info);

    ESP_LOGI(TAG, "TOE chip up: %u.%u.%u.%u (WIZnet hardware TCP/IP)",
             net_info->ip[0], net_info->ip[1], net_info->ip[2], net_info->ip[3]);
    return ESP_OK;
}

void wiznet_net_init(const wiz_NetInfo *net_info)
{
    /* 1) lwIP core + default event loop -> registers the socket VFS fd-range
     *    (so close(fd) works, and reaches __wrap_lwip_close under SOCKET_WRAP). */
    ESP_ERROR_CHECK(toe_ensure_lwip_core());

    /* 2) shadow netif holding the IPv4 identity (no driver attached; no data). */
    esp_netif_inherent_config_t base = ESP_NETIF_INHERENT_DEFAULT_ETH();
    esp_netif_config_t netif_cfg = {
        .base = &base,
        .driver = NULL,
        .stack = ESP_NETIF_NETSTACK_DEFAULT_ETH,
    };
    s_shadow = esp_netif_new(&netif_cfg);
    if (s_shadow) {
        esp_netif_dhcpc_stop(s_shadow);
        esp_netif_ip_info_t ip = {0};
        ip.ip.addr      = ESP_IP4TOADDR(net_info->ip[0], net_info->ip[1], net_info->ip[2], net_info->ip[3]);
        ip.netmask.addr = ESP_IP4TOADDR(net_info->sn[0], net_info->sn[1], net_info->sn[2], net_info->sn[3]);
        ip.gw.addr      = ESP_IP4TOADDR(net_info->gw[0], net_info->gw[1], net_info->gw[2], net_info->gw[3]);
        esp_netif_set_ip_info(s_shadow, &ip);
    }

    /* 3) W5500/W6300 over SPI via ioLibrary, wired from Kconfig.
     *    Steps 3-5 are wiznet_toe_bringup(); this harness only supplies the
     *    Kconfig-derived SPI config so standalone apps keep working unchanged. */
    wsm_driver_spi_config_t spi;
    fill_spi_config(&spi);
    if (wiznet_toe_bringup(&spi, net_info) != ESP_OK) {
        return;
    }

    s_net_up = true;
}

bool wiznet_net_is_up(void)
{
    return s_net_up;
}
