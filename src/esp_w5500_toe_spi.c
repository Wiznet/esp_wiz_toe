/**
 * @file esp_w5500_toe_spi.c
 * @brief Low-level SPI driver shim for W5500.
 *
 * This file implements the byte-level SPI read/write callbacks required by
 * the WIZnet ioLibrary (reg_wizchip_spi_cbfunc) and the CS assert/deassert
 * callbacks (reg_wizchip_cs_cbfunc).
 *
 * The SPI device is managed entirely here; neither esp_eth nor lwIP is
 * involved.
 *
 * NOTE: ioLibrary's default byte-at-a-time callbacks are used for clarity.
 *       For higher throughput, replace with the burst-mode callbacks
 *       (reg_wizchip_spiburst_cbfunc) and use spi_device_transmit() with
 *       DMA-capable buffers.
 */

#include "esp_w5500_toe_spi.h"

#include "esp_log.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"

static const char *TAG = "w5500_spi";

/* -------------------------------------------------------------------------
 * Module-level state
 * ---------------------------------------------------------------------- */

static spi_device_handle_t s_spi_dev = NULL;
static int                 s_pin_cs  = -1;

/* -------------------------------------------------------------------------
 * Public: init / deinit
 * ---------------------------------------------------------------------- */

esp_err_t esp_w5500_toe_spi_init(const esp_w5500_toe_spi_config_t *cfg)
{
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    s_pin_cs = cfg->pin_cs;

    /* Configure CS GPIO as plain output (we drive it manually so that
       ioLibrary's CS callbacks stay in full control of framing).           */
    gpio_config_t cs_io = {
        .pin_bit_mask = (1ULL << cfg->pin_cs),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&cs_io), TAG, "CS GPIO config failed");
    gpio_set_level(cfg->pin_cs, 1);   /* deassert on init */

    /* SPI bus init ------------------------------------------------------- */
    spi_bus_config_t bus_cfg = {
        .mosi_io_num     = cfg->pin_mosi,
        .miso_io_num     = cfg->pin_miso,
        .sclk_io_num     = cfg->pin_sclk,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = 0,  /* default */
    };

    esp_err_t ret = spi_bus_initialize(cfg->spi_host, &bus_cfg,
                                       SPI_DMA_CH_AUTO);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        /* ESP_ERR_INVALID_STATE means bus already initialised – tolerate   */
        ESP_LOGE(TAG, "spi_bus_initialize: %s", esp_err_to_name(ret));
        return ret;
    }

    /* SPI device --------------------------------------------------------- */
    spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = cfg->clock_speed_mhz * 1000 * 1000,
        .mode           = 0,          /* W5500: CPOL=0, CPHA=0              */
        .spics_io_num   = -1,         /* CS managed manually via callbacks  */
        .queue_size     = 1,
        .flags          = 0,
    };

    ret = spi_bus_add_device(cfg->spi_host, &dev_cfg, &s_spi_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_add_device: %s", esp_err_to_name(ret));
        spi_bus_free(cfg->spi_host);
        return ret;
    }

    ESP_LOGI(TAG, "SPI init OK  host=%d  clk=%d MHz  MOSI=%d MISO=%d SCLK=%d CS=%d",
             cfg->spi_host, cfg->clock_speed_mhz,
             cfg->pin_mosi, cfg->pin_miso, cfg->pin_sclk, cfg->pin_cs);
    return ESP_OK;
}

esp_err_t esp_w5500_toe_spi_deinit(void)
{
    if (s_spi_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    spi_bus_remove_device(s_spi_dev);
    s_spi_dev = NULL;

    if (s_pin_cs >= 0) {
        gpio_reset_pin(s_pin_cs);
        s_pin_cs = -1;
    }

    return ESP_OK;
}

/* -------------------------------------------------------------------------
 * ioLibrary CS callbacks
 * ---------------------------------------------------------------------- */

void esp_w5500_toe_spi_cs_assert(void)
{
    gpio_set_level(s_pin_cs, 0);
}

void esp_w5500_toe_spi_cs_deassert(void)
{
    gpio_set_level(s_pin_cs, 1);
}

/* -------------------------------------------------------------------------
 * ioLibrary byte-level SPI callbacks
 *
 * ioLibrary drives the W5500 protocol framing (address phase + data phase)
 * through repeated calls to these two functions.  Each call triggers one
 * spi_device_polling_transmit() for simplicity.
 *
 * TODO: replace with burst callbacks (reg_wizchip_spiburst_cbfunc) and
 *       DMA transfers for production-quality throughput.
 * ---------------------------------------------------------------------- */

uint8_t esp_w5500_toe_spi_read_byte(void)
{
    if (s_spi_dev == NULL) {
        ESP_LOGE(TAG, "read_byte: SPI not initialised");
        return 0xFF;
    }

    uint8_t rx = 0;
    spi_transaction_t t = {
        .length    = 8,
        .rxlength  = 8,
        .rx_buffer = &rx,
        .tx_buffer = NULL,
        .flags     = SPI_TRANS_USE_RXDATA,
    };
    /* Use polling mode to avoid FreeRTOS overhead for single bytes        */
    spi_device_polling_transmit(s_spi_dev, &t);
    return rx;
}

void esp_w5500_toe_spi_write_byte(uint8_t byte)
{
    if (s_spi_dev == NULL) {
        ESP_LOGE(TAG, "write_byte: SPI not initialised");
        return;
    }

    spi_transaction_t t = {
        .length    = 8,
        .tx_buffer = &byte,
        .rx_buffer = NULL,
        .flags     = SPI_TRANS_USE_TXDATA,
    };
    spi_device_polling_transmit(s_spi_dev, &t);
}
