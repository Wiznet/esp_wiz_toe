#include "esp_wiz_toe_spi.h"

#include <stdint.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_wiz_toe_internal.h"
#include "freertos/task.h"

#if __has_include("wizchip_conf.h")
#include "wizchip_conf.h"
#define ESP_WIZ_TOE_HAS_IOLIB 1
#else
#define ESP_WIZ_TOE_HAS_IOLIB 0
#endif

static const char *TAG = "esp_wiz_toe_spi";

#define ESP_WIZ_TOE_DEF_SPI_CLK_PIN  GPIO_NUM_12
#define ESP_WIZ_TOE_DEF_SPI_CS_PIN   GPIO_NUM_10
#define ESP_WIZ_TOE_DEF_SPI_MOSI_PIN GPIO_NUM_11
#define ESP_WIZ_TOE_DEF_SPI_MISO_PIN GPIO_NUM_13
#define ESP_WIZ_TOE_DEF_SPI_INT_PIN  GPIO_NUM_9
#define ESP_WIZ_TOE_DEF_SPI_RST_PIN  GPIO_NUM_7

#define ESP_WIZ_TOE_DEF_SPI_HOST     SPI2_HOST
#define ESP_WIZ_TOE_DEF_SPI_CLOCK_HZ (20 * 1000 * 1000)
#define ESP_WIZ_TOE_DEF_SPI_TIMEOUT_MS 1000U

static inline esp_wiz_toe_context_t *ctx_get(void)
{
    return esp_wiz_toe_get_context();
}

static gpio_num_t resolve_pin(gpio_num_t configured, gpio_num_t fallback)
{
    return configured >= 0 ? configured : fallback;
}

static TickType_t get_spi_wait_ticks(const esp_wiz_toe_context_t *ctx)
{
    uint32_t timeout_ms = ctx->cfg.timeout.send_timeout_ms;

    if (timeout_ms == 0) {
        timeout_ms = ESP_WIZ_TOE_DEF_SPI_TIMEOUT_MS;
    }
    if (timeout_ms == UINT32_MAX) {
        return portMAX_DELAY;
    }
    return pdMS_TO_TICKS(timeout_ms);
}

static void apply_spi_defaults(esp_wiz_toe_context_t *ctx)
{
    if (ctx->cfg.spi.host_id != SPI2_HOST && ctx->cfg.spi.host_id != SPI3_HOST) {
        ctx->cfg.spi.host_id = ESP_WIZ_TOE_DEF_SPI_HOST;
    }
    if (ctx->cfg.spi.clock_hz <= 0) {
        ctx->cfg.spi.clock_hz = ESP_WIZ_TOE_DEF_SPI_CLOCK_HZ;
    }

    ctx->cfg.spi.pin_sclk = resolve_pin(ctx->cfg.spi.pin_sclk, ESP_WIZ_TOE_DEF_SPI_CLK_PIN);
    ctx->cfg.spi.pin_cs = resolve_pin(ctx->cfg.spi.pin_cs, ESP_WIZ_TOE_DEF_SPI_CS_PIN);
    ctx->cfg.spi.pin_mosi = resolve_pin(ctx->cfg.spi.pin_mosi, ESP_WIZ_TOE_DEF_SPI_MOSI_PIN);
    ctx->cfg.spi.pin_miso = resolve_pin(ctx->cfg.spi.pin_miso, ESP_WIZ_TOE_DEF_SPI_MISO_PIN);
    ctx->cfg.spi.pin_int = resolve_pin(ctx->cfg.spi.pin_int, ESP_WIZ_TOE_DEF_SPI_INT_PIN);
    ctx->cfg.spi.pin_rst = resolve_pin(ctx->cfg.spi.pin_rst, ESP_WIZ_TOE_DEF_SPI_RST_PIN);
}

static esp_err_t spi_transfer_locked(const uint8_t *tx, uint8_t *rx, size_t len)
{
    esp_wiz_toe_context_t *ctx = ctx_get();
    const TickType_t wait_ticks = get_spi_wait_ticks(ctx);
    const bool auto_cs = !ctx->cs_active;
    esp_err_t ret = ESP_OK;

    if (len == 0) {
        return ESP_OK;
    }

    if (xSemaphoreTakeRecursive(ctx->lock, wait_ticks) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    ret = spi_device_acquire_bus(ctx->spi_dev, wait_ticks);
    if (ret != ESP_OK) {
        (void)xSemaphoreGiveRecursive(ctx->lock);
        return ret;
    }

    if (auto_cs) {
        gpio_set_level(ctx->cfg.spi.pin_cs, 0);
    }

    spi_transaction_t t = {
        .length = len * 8,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };

    ret = spi_device_transmit(ctx->spi_dev, &t);

    if (auto_cs) {
        gpio_set_level(ctx->cfg.spi.pin_cs, 1);
    }

    spi_device_release_bus(ctx->spi_dev);
    (void)xSemaphoreGiveRecursive(ctx->lock);
    return ret;
}

#if ESP_WIZ_TOE_HAS_IOLIB
/* MUST NOT be called from ISR context. Uses mutex and blocking SPI operations. */
static void wizchip_cs_select(void)
{
    esp_wiz_toe_context_t *ctx = ctx_get();
    const TickType_t wait_ticks = get_spi_wait_ticks(ctx);

    if (xSemaphoreTakeRecursive(ctx->lock, wait_ticks) != pdTRUE) {
        ESP_LOGE(TAG, "CS select lock timeout");
        return;
    }

    if (spi_device_acquire_bus(ctx->spi_dev, wait_ticks) != ESP_OK) {
        ESP_LOGE(TAG, "CS select bus acquire timeout");
        (void)xSemaphoreGiveRecursive(ctx->lock);
        return;
    }

    gpio_set_level(ctx->cfg.spi.pin_cs, 0);
    ctx->cs_active = true;
}

/* MUST NOT be called from ISR context. Uses mutex and blocking SPI operations. */
static void wizchip_cs_deselect(void)
{
    esp_wiz_toe_context_t *ctx = ctx_get();

    gpio_set_level(ctx->cfg.spi.pin_cs, 1);
    ctx->cs_active = false;
    spi_device_release_bus(ctx->spi_dev);
    (void)xSemaphoreGiveRecursive(ctx->lock);
}

/* MUST NOT be called from ISR context. Uses mutex and blocking SPI operations. */
static uint8_t wizchip_spi_read_byte(void)
{
    const uint8_t tx = 0x00;
    uint8_t rx = 0;

    if (spi_transfer_locked(&tx, &rx, 1) != ESP_OK) {
        ESP_LOGE(TAG, "read byte failed");
    }
    return rx;
}

/* MUST NOT be called from ISR context. Uses mutex and blocking SPI operations. */
static void wizchip_spi_write_byte(uint8_t byte)
{
    if (spi_transfer_locked(&byte, NULL, 1) != ESP_OK) {
        ESP_LOGE(TAG, "write byte failed");
    }
}

/* MUST NOT be called from ISR context. Uses mutex and blocking SPI operations. */
static void wizchip_spi_read_burst(uint8_t *buf, uint16_t len)
{
    if (buf == NULL || len == 0) {
        return;
    }

    if (spi_transfer_locked(NULL, buf, len) != ESP_OK) {
        ESP_LOGE(TAG, "read burst failed len=%u", (unsigned)len);
    }
}

/* MUST NOT be called from ISR context. Uses mutex and blocking SPI operations. */
static void wizchip_spi_write_burst(uint8_t *buf, uint16_t len)
{
    if (buf == NULL || len == 0) {
        return;
    }

    if (spi_transfer_locked(buf, NULL, len) != ESP_OK) {
        ESP_LOGE(TAG, "write burst failed len=%u", (unsigned)len);
    }
}

static void wizchip_critical_enter(void)
{
    esp_wiz_toe_context_t *ctx = ctx_get();
    const TickType_t wait_ticks = get_spi_wait_ticks(ctx);
    (void)xSemaphoreTakeRecursive(ctx->lock, wait_ticks);
}

static void wizchip_critical_exit(void)
{
    esp_wiz_toe_context_t *ctx = ctx_get();
    (void)xSemaphoreGiveRecursive(ctx->lock);
}
#endif

esp_err_t esp_wiz_toe_spi_init(const esp_wiz_toe_config_t *cfg)
{
    esp_wiz_toe_context_t *ctx = ctx_get();
    esp_err_t ret = ESP_OK;

    ESP_RETURN_ON_FALSE(cfg != NULL, ESP_ERR_INVALID_ARG, TAG, "cfg is NULL");

    apply_spi_defaults(ctx);

    ctx->lock = xSemaphoreCreateRecursiveMutex();
    ESP_RETURN_ON_FALSE(ctx->lock != NULL, ESP_ERR_NO_MEM, TAG, "failed to create SPI mutex");

    spi_bus_config_t buscfg = {
        .mosi_io_num = ctx->cfg.spi.pin_mosi,
        .miso_io_num = ctx->cfg.spi.pin_miso,
        .sclk_io_num = ctx->cfg.spi.pin_sclk,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 2048,
    };
    ESP_GOTO_ON_ERROR(spi_bus_initialize(ctx->cfg.spi.host_id, &buscfg, SPI_DMA_CH_AUTO), err, TAG, "spi_bus_initialize failed");

    spi_device_interface_config_t devcfg = {
        .mode = 0,
        .clock_speed_hz = ctx->cfg.spi.clock_hz,
        .spics_io_num = -1,
        .queue_size = 1,
    };
    ESP_GOTO_ON_ERROR(spi_bus_add_device(ctx->cfg.spi.host_id, &devcfg, &ctx->spi_dev), err_bus, TAG, "spi_bus_add_device failed");

    gpio_config_t out_cfg = {
        .pin_bit_mask = (1ULL << ctx->cfg.spi.pin_cs) | (1ULL << ctx->cfg.spi.pin_rst),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_GOTO_ON_ERROR(gpio_config(&out_cfg), err_dev, TAG, "gpio_config output failed");

    gpio_config_t int_cfg = {
        .pin_bit_mask = (1ULL << ctx->cfg.spi.pin_int),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_GOTO_ON_ERROR(gpio_config(&int_cfg), err_dev, TAG, "gpio_config int failed");

    gpio_set_level(ctx->cfg.spi.pin_cs, 1);
    gpio_set_level(ctx->cfg.spi.pin_rst, 1);

    ctx->cs_active = false;
    ESP_LOGI(TAG, "SPI transport initialized host=%d clk=%d", (int)ctx->cfg.spi.host_id, ctx->cfg.spi.clock_hz);
    return ESP_OK;

err_dev:
    (void)spi_bus_remove_device(ctx->spi_dev);
    ctx->spi_dev = NULL;
err_bus:
    (void)spi_bus_free(ctx->cfg.spi.host_id);
err:
    vSemaphoreDelete(ctx->lock);
    ctx->lock = NULL;
    return ret;
}

esp_err_t esp_wiz_toe_spi_deinit(void)
{
    esp_wiz_toe_context_t *ctx = ctx_get();

    if (ctx->spi_dev != NULL) {
        (void)spi_bus_remove_device(ctx->spi_dev);
        ctx->spi_dev = NULL;
    }
    (void)spi_bus_free(ctx->cfg.spi.host_id);

    if (ctx->lock != NULL) {
        vSemaphoreDelete(ctx->lock);
        ctx->lock = NULL;
    }

    ctx->cs_active = false;
    return ESP_OK;
}

esp_err_t esp_wiz_toe_spi_register_iolib_callbacks(void)
{
#if ESP_WIZ_TOE_HAS_IOLIB
    reg_wizchip_cris_cbfunc(wizchip_critical_enter, wizchip_critical_exit);
    reg_wizchip_cs_cbfunc(wizchip_cs_select, wizchip_cs_deselect);
    reg_wizchip_spi_cbfunc(wizchip_spi_read_byte, wizchip_spi_write_byte);
    reg_wizchip_spiburst_cbfunc(wizchip_spi_read_burst, wizchip_spi_write_burst);
    return ESP_OK;
#else
    ESP_LOGW(TAG, "ioLibrary headers not found; callback registration skipped");
    return ESP_ERR_NOT_SUPPORTED;
#endif
}
