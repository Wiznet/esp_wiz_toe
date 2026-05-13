#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_wiz_toe.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define EXAMPLE_USE_DHCP            1
#define EXAMPLE_SOCKET_NUM          0
#define EXAMPLE_SERVER_PORT         5000
#define EXAMPLE_LOCAL_PORT          50000
#define EXAMPLE_CONNECT_TIMEOUT_MS  5000
#define EXAMPLE_IO_TIMEOUT_MS       5000

static const char *TAG = "tcp_client_example";
static const uint8_t EXAMPLE_SERVER_IP[4] = {192, 168, 1, 10};
#if !EXAMPLE_USE_DHCP
static const uint8_t EXAMPLE_STATIC_IP[4] = {192, 168, 1, 50};
static const uint8_t EXAMPLE_STATIC_NETMASK[4] = {255, 255, 255, 0};
static const uint8_t EXAMPLE_STATIC_GATEWAY[4] = {192, 168, 1, 1};
static const uint8_t EXAMPLE_STATIC_DNS[4] = {8, 8, 8, 8};
#endif

static void fill_default_config(esp_wiz_toe_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));

    cfg->spi.host_id = SPI2_HOST;
    cfg->spi.clock_hz = 20 * 1000 * 1000;
    cfg->spi.pin_miso = -1;
    cfg->spi.pin_mosi = -1;
    cfg->spi.pin_sclk = -1;
    cfg->spi.pin_cs = -1;
    cfg->spi.pin_int = -1;
    cfg->spi.pin_rst = -1;

    cfg->mac.mac[0] = 0x02;
    cfg->mac.mac[1] = 0x00;
    cfg->mac.mac[2] = 0x00;
    cfg->mac.mac[3] = 0x00;
    cfg->mac.mac[4] = 0x55;
    cfg->mac.mac[5] = 0x01;

#if EXAMPLE_USE_DHCP
    cfg->ip_mode = ESP_WIZ_TOE_IP_MODE_DHCP;
#else
    cfg->ip_mode = ESP_WIZ_TOE_IP_MODE_STATIC;
    memcpy(cfg->static_ip.ip, EXAMPLE_STATIC_IP, sizeof(cfg->static_ip.ip));
    memcpy(cfg->static_ip.netmask, EXAMPLE_STATIC_NETMASK, sizeof(cfg->static_ip.netmask));
    memcpy(cfg->static_ip.gateway, EXAMPLE_STATIC_GATEWAY, sizeof(cfg->static_ip.gateway));
    memcpy(cfg->static_ip.dns, EXAMPLE_STATIC_DNS, sizeof(cfg->static_ip.dns));
#endif

    cfg->timeout.connect_timeout_ms = EXAMPLE_CONNECT_TIMEOUT_MS;
    cfg->timeout.send_timeout_ms = EXAMPLE_IO_TIMEOUT_MS;
    cfg->timeout.recv_timeout_ms = EXAMPLE_IO_TIMEOUT_MS;
}

static void tcp_client_task(void *arg)
{
    (void)arg;

    esp_wiz_toe_config_t cfg;
    fill_default_config(&cfg);

    ESP_LOGI(TAG, "Initializing W5500 TOE");
    ESP_ERROR_CHECK(esp_wiz_toe_init(&cfg));

#if EXAMPLE_USE_DHCP
    ESP_LOGI(TAG, "Starting DHCP");
    ESP_ERROR_CHECK(esp_wiz_toe_dhcp_start(15000));
#endif

    while (true) {
        bool link_up = false;
        if (esp_wiz_toe_is_link_up(&link_up) != ESP_OK || !link_up) {
            ESP_LOGI(TAG, "PHY link down, retrying...");
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        esp_err_t ret = esp_wiz_toe_socket(
            EXAMPLE_SOCKET_NUM,
            ESP_WIZ_TOE_PROTO_TCP,
            EXAMPLE_LOCAL_PORT,
            0,
            EXAMPLE_CONNECT_TIMEOUT_MS);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Socket open failed: %s", esp_err_to_name(ret));
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        ret = esp_wiz_toe_connect(
            EXAMPLE_SOCKET_NUM,
            EXAMPLE_SERVER_IP,
            EXAMPLE_SERVER_PORT,
            EXAMPLE_CONNECT_TIMEOUT_MS);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Connect failed: %s", esp_err_to_name(ret));
            (void)esp_wiz_toe_close(EXAMPLE_SOCKET_NUM);
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        const char *msg = "hello from esp32s3";
        size_t sent_len = 0;
        ret = esp_wiz_toe_send(
            EXAMPLE_SOCKET_NUM,
            msg,
            strlen(msg),
            EXAMPLE_IO_TIMEOUT_MS,
            &sent_len);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Send failed: %s", esp_err_to_name(ret));
            (void)esp_wiz_toe_close(EXAMPLE_SOCKET_NUM);
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        ESP_LOGI(TAG, "Sent %u bytes", (unsigned)sent_len);

        uint8_t rx_buf[128];
        size_t recv_len = 0;
        ret = esp_wiz_toe_recv(
            EXAMPLE_SOCKET_NUM,
            rx_buf,
            sizeof(rx_buf) - 1,
            EXAMPLE_IO_TIMEOUT_MS,
            &recv_len);
        if (ret == ESP_OK) {
            rx_buf[recv_len] = '\0';
            ESP_LOGI(TAG, "Received (%u bytes): %s", (unsigned)recv_len, (char *)rx_buf);
        } else {
            ESP_LOGW(TAG, "Receive failed: %s", esp_err_to_name(ret));
        }

        (void)esp_wiz_toe_close(EXAMPLE_SOCKET_NUM);
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}

void app_main(void)
{
    xTaskCreate(tcp_client_task, "tcp_client_task", 4096, NULL, 5, NULL);
}
