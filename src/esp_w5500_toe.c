/**
 * @file esp_w5500_toe.c
 * @brief W5500 TOE component – lifecycle and chip initialisation.
 *
 * Responsibilities of this file:
 *   - Hardware reset sequence
 *   - SPI init (delegates to esp_w5500_toe_spi.c)
 *   - wizchip_conf registration (critical section / SPI select callbacks)
 *   - wizchip_init() with per-socket buffer sizes
 *   - wizchip_setnetinfo() with the caller-supplied network config
 *   - Component state tracking
 *
 * This component does NOT use esp_eth MAC RAW driver or lwIP netif.
 * All TCP/IP processing is offloaded to W5500 hardware.
 */

#include "esp_w5500_toe.h"
#include "esp_w5500_toe_spi.h"    /* internal SPI layer                    */
#include "esp_w5500_toe_socket.h" /* internal socket helpers               */

#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* ioLibrary */
#include "wizchip_conf.h"
#include "w5500.h"

static const char *TAG = "w5500_toe";

/* Component-level state -------------------------------------------------- */

static bool s_initialized = false;
static esp_w5500_toe_config_t s_cfg;

/* -------------------------------------------------------------------------
 * ioLibrary callback shims (registered via wizchip_conf)
 * ---------------------------------------------------------------------- */

static void _cs_select(void)
{
    esp_w5500_toe_spi_cs_assert();
}

static void _cs_deselect(void)
{
    esp_w5500_toe_spi_cs_deassert();
}

static uint8_t _spi_read_byte(void)
{
    return esp_w5500_toe_spi_read_byte();
}

static void _spi_write_byte(uint8_t byte)
{
    esp_w5500_toe_spi_write_byte(byte);
}

/* -------------------------------------------------------------------------
 * Internal helpers
 * ---------------------------------------------------------------------- */

static void _hw_reset(int pin_rst)
{
    if (pin_rst < 0) {
        ESP_LOGW(TAG, "RST pin not configured – skipping hardware reset");
        return;
    }

    gpio_config_t io_cfg = {
        .pin_bit_mask = (1ULL << pin_rst),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_cfg));

    gpio_set_level(pin_rst, 0);
    vTaskDelay(pdMS_TO_TICKS(10));   /* W5500 datasheet: ≥500 µs assert      */
    gpio_set_level(pin_rst, 1);
    vTaskDelay(pdMS_TO_TICKS(50));   /* PLL lock + internal init             */

    ESP_LOGD(TAG, "Hardware reset complete (GPIO %d)", pin_rst);
}

static void _build_netinfo(const esp_w5500_toe_net_config_t *net,
                           wiz_NetInfo *ni)
{
    for (int i = 0; i < 6; i++) ni->mac[i]     = net->mac[i];
    for (int i = 0; i < 4; i++) ni->ip[i]      = net->ip[i];
    for (int i = 0; i < 4; i++) ni->sn[i]      = net->subnet[i];
    for (int i = 0; i < 4; i++) ni->gw[i]      = net->gateway[i];
    for (int i = 0; i < 4; i++) ni->dns[i]     = net->dns[i];
    ni->dhcp = NETINFO_STATIC;       /* DHCP path not yet implemented        */
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

esp_err_t esp_w5500_toe_init(const esp_w5500_toe_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_initialized) {
        ESP_LOGW(TAG, "Already initialised – call deinit first");
        return ESP_ERR_INVALID_STATE;
    }

    s_cfg = *config;

    /* 1. Hardware reset -------------------------------------------------- */
    _hw_reset(config->spi.pin_rst);

    /* 2. SPI bus & device init ------------------------------------------- */
    esp_err_t ret = esp_w5500_toe_spi_init(&config->spi);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 3. Register ioLibrary callbacks ------------------------------------ */
    reg_wizchip_cs_cbfunc(_cs_select, _cs_deselect);
    reg_wizchip_spi_cbfunc(_spi_read_byte, _spi_write_byte);

    /* 4. Initialise chip & socket buffers -------------------------------- */
    /* Each element: TX[sn] and RX[sn] buffer size in KB (must be power-of-2,
       total ≤ 16 KB each direction).                                        */
    uint8_t tx_bufs[8] = {
        CONFIG_W5500_TOE_SOCKET_TX_BUF_KB,
        CONFIG_W5500_TOE_SOCKET_TX_BUF_KB,
        CONFIG_W5500_TOE_SOCKET_TX_BUF_KB,
        CONFIG_W5500_TOE_SOCKET_TX_BUF_KB,
        CONFIG_W5500_TOE_SOCKET_TX_BUF_KB,
        CONFIG_W5500_TOE_SOCKET_TX_BUF_KB,
        CONFIG_W5500_TOE_SOCKET_TX_BUF_KB,
        CONFIG_W5500_TOE_SOCKET_TX_BUF_KB,
    };
    uint8_t rx_bufs[8] = {
        CONFIG_W5500_TOE_SOCKET_RX_BUF_KB,
        CONFIG_W5500_TOE_SOCKET_RX_BUF_KB,
        CONFIG_W5500_TOE_SOCKET_RX_BUF_KB,
        CONFIG_W5500_TOE_SOCKET_RX_BUF_KB,
        CONFIG_W5500_TOE_SOCKET_RX_BUF_KB,
        CONFIG_W5500_TOE_SOCKET_RX_BUF_KB,
        CONFIG_W5500_TOE_SOCKET_RX_BUF_KB,
        CONFIG_W5500_TOE_SOCKET_RX_BUF_KB,
    };

    if (wizchip_init(tx_bufs, rx_bufs) != 0) {
        ESP_LOGE(TAG, "wizchip_init() failed – check SPI wiring");
        esp_w5500_toe_spi_deinit();
        return ESP_FAIL;
    }

    /* 5. Set network information ----------------------------------------- */
    wiz_NetInfo ni = {0};
    _build_netinfo(&config->net, &ni);
    wizchip_setnetinfo(&ni);

    /* 6. Log current config for debugging -------------------------------- */
    wiz_NetInfo read_back = {0};
    wizchip_getnetinfo(&read_back);
    ESP_LOGI(TAG, "W5500 ready  MAC=%02X:%02X:%02X:%02X:%02X:%02X",
             read_back.mac[0], read_back.mac[1], read_back.mac[2],
             read_back.mac[3], read_back.mac[4], read_back.mac[5]);
    ESP_LOGI(TAG, "  IP=%d.%d.%d.%d  GW=%d.%d.%d.%d",
             read_back.ip[0],  read_back.ip[1],
             read_back.ip[2],  read_back.ip[3],
             read_back.gw[0],  read_back.gw[1],
             read_back.gw[2],  read_back.gw[3]);

    s_initialized = true;
    return ESP_OK;
}

esp_err_t esp_w5500_toe_deinit(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_w5500_toe_spi_deinit();

    if (s_cfg.spi.pin_rst >= 0) {
        gpio_reset_pin(s_cfg.spi.pin_rst);
    }

    s_initialized = false;
    ESP_LOGI(TAG, "De-initialised");
    return ESP_OK;
}

bool esp_w5500_toe_is_ready(void)
{
    return s_initialized;
}
