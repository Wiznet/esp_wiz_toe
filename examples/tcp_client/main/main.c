#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_log.h"
#include "esp_wiz_toe.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "wizchip_conf.h"
#include "Application/loopback/loopback.h"

#define EXAMPLE_SOCKET_NUM 0
#define EXAMPLE_IO_TIMEOUT_MS 5000
#define EXAMPLE_LOOPBACK_BUF_SIZE (1024 * 2)

#ifdef CONFIG_ESP_WIZ_TOE_TX_BUF_KB
#define EXAMPLE_TX_BUF_KB CONFIG_ESP_WIZ_TOE_TX_BUF_KB
#else
#define EXAMPLE_TX_BUF_KB 2
#endif

#ifdef CONFIG_ESP_WIZ_TOE_RX_BUF_KB
#define EXAMPLE_RX_BUF_KB CONFIG_ESP_WIZ_TOE_RX_BUF_KB
#else
#define EXAMPLE_RX_BUF_KB 2
#endif

static const char *TAG = "tcp_client_example";
static const uint8_t EXAMPLE_SERVER_IP[4] = {192, 168, 11, 100};
static const uint16_t EXAMPLE_SERVER_PORT = 5000;

// Keep large networking buffers out of task stack.
static esp_wiz_toe_spi_config_t s_spi_cfg;
static uint8_t s_tx_buf[8];
static uint8_t s_rx_buf[8];
static uint8_t s_loopback_buf[EXAMPLE_LOOPBACK_BUF_SIZE];
static bool s_link_up = false;
static bool s_link_was_up = false;

static const wiz_NetInfo s_net_info = {
    .mac = {0x00, 0x08, 0xDC, 0x12, 0x34, 0x56},
    .ip = {192, 168, 11, 2},
    .sn = {255, 255, 255, 0},
    .gw = {192, 168, 11, 1},
    .dns = {8, 8, 8, 8},
    .dhcp = NETINFO_STATIC,
};

static void fill_spi_config(esp_wiz_toe_spi_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));

    cfg->host_id = (spi_host_device_t)CONFIG_ESP_WIZ_TOE_SPI_HOST;
    cfg->clock_hz = CONFIG_ESP_WIZ_TOE_SPI_CLOCK_HZ;
    cfg->pin_miso = (gpio_num_t)CONFIG_ESP_WIZ_TOE_PIN_MISO;
    cfg->pin_mosi = (gpio_num_t)CONFIG_ESP_WIZ_TOE_PIN_MOSI;
    cfg->pin_sclk = (gpio_num_t)CONFIG_ESP_WIZ_TOE_PIN_SCLK;
    cfg->pin_cs = (gpio_num_t)CONFIG_ESP_WIZ_TOE_PIN_CS;
    cfg->pin_int = (gpio_num_t)CONFIG_ESP_WIZ_TOE_PIN_INT;
    cfg->pin_rst = (gpio_num_t)CONFIG_ESP_WIZ_TOE_PIN_RST;
    cfg->lock_timeout_ms = EXAMPLE_IO_TIMEOUT_MS;
}

static void app_task(void *arg)
{
    (void)arg;

    s_tx_buf[0] = EXAMPLE_TX_BUF_KB;
    s_tx_buf[1] = EXAMPLE_TX_BUF_KB;
    s_tx_buf[2] = EXAMPLE_TX_BUF_KB;
    s_tx_buf[3] = EXAMPLE_TX_BUF_KB;
    s_tx_buf[4] = EXAMPLE_TX_BUF_KB;
    s_tx_buf[5] = EXAMPLE_TX_BUF_KB;
    s_tx_buf[6] = EXAMPLE_TX_BUF_KB;
    s_tx_buf[7] = EXAMPLE_TX_BUF_KB;

    s_rx_buf[0] = EXAMPLE_RX_BUF_KB;
    s_rx_buf[1] = EXAMPLE_RX_BUF_KB;
    s_rx_buf[2] = EXAMPLE_RX_BUF_KB;
    s_rx_buf[3] = EXAMPLE_RX_BUF_KB;
    s_rx_buf[4] = EXAMPLE_RX_BUF_KB;
    s_rx_buf[5] = EXAMPLE_RX_BUF_KB;
    s_rx_buf[6] = EXAMPLE_RX_BUF_KB;
    s_rx_buf[7] = EXAMPLE_RX_BUF_KB;

    fill_spi_config(&s_spi_cfg);

    ESP_ERROR_CHECK(esp_wiz_toe_spi_init(&s_spi_cfg));
    ESP_ERROR_CHECK(esp_wiz_toe_spi_register_iolib_callbacks());
    ESP_ERROR_CHECK(esp_wiz_toe_spi_reset());
    ESP_ERROR_CHECK(esp_wiz_toe_spi_wizchip_check());

    if (wizchip_init(s_tx_buf, s_rx_buf) != 0) {
        ESP_LOGE(TAG, "wizchip_init failed");
        vTaskDelete(NULL);
        return;
    }

    wizchip_setnetinfo((wiz_NetInfo *)&s_net_info);

    do {
        if (esp_wiz_toe_spi_link_is_up(&s_link_up) != ESP_OK) {
            ESP_LOGW(TAG, "PHY link state read failed, retrying...");
            s_link_up = false;
        } else if (!s_link_up) {
            ESP_LOGI(TAG, "PHY link down, waiting...");
        }

        if (!s_link_up) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    } while (!s_link_up);
    ESP_LOGI(TAG, "PHY link up");
    s_link_was_up = true;

    while (true) {
        if (esp_wiz_toe_spi_link_is_up(&s_link_up) != ESP_OK) {
            ESP_LOGW(TAG, "PHY link state read failed, retrying...");
            s_link_up = false;
        }

        if (!s_link_up) {
            s_link_was_up = false;
            ESP_LOGI(TAG, "PHY link down, waiting...");
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        if (!s_link_was_up) {
            ESP_LOGI(TAG, "PHY link up");
            s_link_was_up = true;
        }

        int32_t rc = loopback_tcpc(EXAMPLE_SOCKET_NUM,
                                   s_loopback_buf,
                                   (uint8_t *)EXAMPLE_SERVER_IP,
                                   EXAMPLE_SERVER_PORT);
        if (rc < 0) {
            ESP_LOGW(TAG, "loopback_tcpc failed: %ld", (long)rc);
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}


void app_main(void)
{
    xTaskCreate(app_task, "tcp_client_task", 8192, NULL, 5, NULL);
}
