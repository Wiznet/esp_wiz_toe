#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_log.h"
#include "esp_wiz_toe.h"
#include "esp_wiz_toe/Ethernet/socket.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "wizchip_conf.h"

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
static uint16_t s_any_port = 50000;

static int32_t do_retransmit(uint8_t sn, uint8_t *buf, uint16_t buf_size)
{
    // State check
    uint8_t state = getSn_SR(sn);
    if (state != SOCK_ESTABLISHED && state != SOCK_CLOSE_WAIT) {
        return 1;
    }

    uint16_t rx_size = getSn_RX_RSR(sn);
    if (rx_size == 0) {
        return 1;
    }
    if (rx_size > buf_size) {
        rx_size = buf_size;
    }

    // Read directly from RX buffer
    wiz_recv_data(sn, buf, rx_size);
    setSn_CR(sn, Sn_CR_RECV);
    // Wait for command to complete (avoid WDT)
    while (getSn_CR(sn)) {
        vTaskDelay(1);
    }

    // Send loop (keep existing send logic)
    uint16_t sent = 0;
    uint32_t send_busy_count = 0;
    while (sent < rx_size) {
        int32_t sret = send(sn, buf + sent, rx_size - sent);
        if (sret == SOCK_BUSY) {
            send_busy_count++;
            if ((send_busy_count % 100U) == 0U) {
                ESP_LOGW(TAG, "send busy progress=%u/%u", (unsigned)sent, (unsigned)rx_size);
            }
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        if (sret < 0) {
            ESP_LOGW(TAG, "send failed ret=%ld progress=%u/%u", (long)sret, (unsigned)sent, (unsigned)rx_size);
            (void)close(sn);
            return sret;
        }
        sent += (uint16_t)sret;
    }

    return 1;
}

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

static void tcp_client_task(void *arg)
{
    (void)arg;
    int32_t rc = 0;
    uint32_t connect_busy_count = 0;

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


        switch (getSn_SR(EXAMPLE_SOCKET_NUM)) {
        case SOCK_ESTABLISHED:
            connect_busy_count = 0;
            if (getSn_IR(EXAMPLE_SOCKET_NUM) & Sn_IR_CON) {
                setSn_IR(EXAMPLE_SOCKET_NUM, Sn_IR_CON);
                ESP_LOGI(TAG, "Connected to %u.%u.%u.%u:%u",
                         EXAMPLE_SERVER_IP[0], EXAMPLE_SERVER_IP[1], EXAMPLE_SERVER_IP[2], EXAMPLE_SERVER_IP[3],
                         (unsigned)EXAMPLE_SERVER_PORT);
            }
            // 논블로킹 모드에서 RX 버퍼에 데이터가 있을 때만 do_retransmit 호출
            if (getSn_RX_RSR(EXAMPLE_SOCKET_NUM) > 0) {
                rc = do_retransmit(EXAMPLE_SOCKET_NUM, s_loopback_buf, EXAMPLE_LOOPBACK_BUF_SIZE);
            }
            break;

        case SOCK_CLOSE_WAIT:
            // 논블로킹 모드에서 RX 버퍼에 데이터가 있을 때만 do_retransmit 호출
            if (getSn_RX_RSR(EXAMPLE_SOCKET_NUM) > 0) {
                rc = do_retransmit(EXAMPLE_SOCKET_NUM, s_loopback_buf, EXAMPLE_LOOPBACK_BUF_SIZE);
            }
            if (rc >= 0) {
                rc = disconnect(EXAMPLE_SOCKET_NUM);
                if (rc == SOCK_OK) {
                    ESP_LOGI(TAG, "Socket disconnected");
                    rc = 1;
                }
            }
            break;

        case SOCK_INIT:
            rc = connect(EXAMPLE_SOCKET_NUM, (uint8_t *)EXAMPLE_SERVER_IP, EXAMPLE_SERVER_PORT);
            if (rc == SOCK_BUSY) {
                connect_busy_count++;
                if ((connect_busy_count % 200U) == 0U) {
                    ESP_LOGI(TAG, "Connecting to %u.%u.%u.%u:%u...",
                             EXAMPLE_SERVER_IP[0], EXAMPLE_SERVER_IP[1], EXAMPLE_SERVER_IP[2], EXAMPLE_SERVER_IP[3],
                             (unsigned)EXAMPLE_SERVER_PORT);
                }
                rc = 1;
            } else {
                connect_busy_count = 0;
            }
            break;

        case SOCK_CLOSED:
            connect_busy_count = 0;
            (void)close(EXAMPLE_SOCKET_NUM);
            // Open socket in non-blocking mode (SF_IO_NONBLOCK)
            rc = socket(EXAMPLE_SOCKET_NUM, Sn_MR_TCP, s_any_port++, SF_IO_NONBLOCK);
            if (rc == EXAMPLE_SOCKET_NUM) {
                ESP_LOGI(TAG, "Client socket opened (non-blocking)");
                rc = 1;
            }
            if (s_any_port == 0xFFFFU) {
                s_any_port = 50000;
            }
            break;

        default:
            rc = 1;
            break;
        }

        if (rc < 0) {
            ESP_LOGW(TAG, "tcp_client_task step failed: %ld", (long)rc);
            vTaskDelay(pdMS_TO_TICKS(100));
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}


void app_main(void)
{
    xTaskCreate(tcp_client_task, "tcp_client_task", 8192, NULL, 5, NULL);
}
