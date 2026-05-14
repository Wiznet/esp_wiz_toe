#include "esp_wiz_toe.h"

#include <stdint.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#if __has_include("wizchip_conf.h")
#include "wizchip_conf.h"
#define ESP_WIZ_TOE_HAS_IOLIB 1
#else
#define ESP_WIZ_TOE_HAS_IOLIB 0
#endif

static const char *TAG = "esp_wiz_toe_spi";

#ifdef CONFIG_ESP_WIZ_TOE_SPI_HOST
#define ESP_WIZ_TOE_DEF_SPI_HOST ((spi_host_device_t)CONFIG_ESP_WIZ_TOE_SPI_HOST)
#else
#define ESP_WIZ_TOE_DEF_SPI_HOST SPI2_HOST
#endif

#ifdef CONFIG_ESP_WIZ_TOE_SPI_CLOCK_HZ
#define ESP_WIZ_TOE_DEF_SPI_CLOCK_HZ CONFIG_ESP_WIZ_TOE_SPI_CLOCK_HZ
#else
#define ESP_WIZ_TOE_DEF_SPI_CLOCK_HZ (20 * 1000 * 1000)
#endif

#ifdef CONFIG_ESP_WIZ_TOE_PIN_MISO
#define ESP_WIZ_TOE_DEF_SPI_MISO_PIN ((gpio_num_t)CONFIG_ESP_WIZ_TOE_PIN_MISO)
#else
#define ESP_WIZ_TOE_DEF_SPI_MISO_PIN GPIO_NUM_13
#endif

#ifdef CONFIG_ESP_WIZ_TOE_PIN_MOSI
#define ESP_WIZ_TOE_DEF_SPI_MOSI_PIN ((gpio_num_t)CONFIG_ESP_WIZ_TOE_PIN_MOSI)
#else
#define ESP_WIZ_TOE_DEF_SPI_MOSI_PIN GPIO_NUM_11
#endif

#ifdef CONFIG_ESP_WIZ_TOE_PIN_SCLK
#define ESP_WIZ_TOE_DEF_SPI_CLK_PIN ((gpio_num_t)CONFIG_ESP_WIZ_TOE_PIN_SCLK)
#else
#define ESP_WIZ_TOE_DEF_SPI_CLK_PIN GPIO_NUM_12
#endif

#ifdef CONFIG_ESP_WIZ_TOE_PIN_CS
#define ESP_WIZ_TOE_DEF_SPI_CS_PIN ((gpio_num_t)CONFIG_ESP_WIZ_TOE_PIN_CS)
#else
#define ESP_WIZ_TOE_DEF_SPI_CS_PIN GPIO_NUM_10
#endif

#ifdef CONFIG_ESP_WIZ_TOE_PIN_RST
#define ESP_WIZ_TOE_DEF_SPI_RST_PIN ((gpio_num_t)CONFIG_ESP_WIZ_TOE_PIN_RST)
#else
#define ESP_WIZ_TOE_DEF_SPI_RST_PIN GPIO_NUM_9
#endif

#ifdef CONFIG_ESP_WIZ_TOE_PIN_INT
#define ESP_WIZ_TOE_DEF_SPI_INT_PIN ((gpio_num_t)CONFIG_ESP_WIZ_TOE_PIN_INT)
#else
#define ESP_WIZ_TOE_DEF_SPI_INT_PIN GPIO_NUM_14
#endif

#define ESP_WIZ_TOE_DEF_SPI_TIMEOUT_MS 1000U

typedef struct {
    bool initialized;
    bool cs_active;
    spi_device_handle_t spi_dev;
    SemaphoreHandle_t lock;
    esp_wiz_toe_spi_config_t cfg;
} esp_wiz_toe_spi_context_t;

static esp_wiz_toe_spi_context_t s_ctx;

static gpio_num_t resolve_pin(gpio_num_t configured, gpio_num_t fallback)
{
    return configured >= 0 ? configured : fallback;
}

static void apply_defaults(esp_wiz_toe_spi_config_t *cfg)
{
    if (cfg->host_id != SPI2_HOST && cfg->host_id != SPI3_HOST) {
        cfg->host_id = ESP_WIZ_TOE_DEF_SPI_HOST;
    }
    if (cfg->clock_hz <= 0) {
        cfg->clock_hz = ESP_WIZ_TOE_DEF_SPI_CLOCK_HZ;
    }
    cfg->pin_sclk = resolve_pin(cfg->pin_sclk, ESP_WIZ_TOE_DEF_SPI_CLK_PIN);
    cfg->pin_cs = resolve_pin(cfg->pin_cs, ESP_WIZ_TOE_DEF_SPI_CS_PIN);
    cfg->pin_mosi = resolve_pin(cfg->pin_mosi, ESP_WIZ_TOE_DEF_SPI_MOSI_PIN);
    cfg->pin_miso = resolve_pin(cfg->pin_miso, ESP_WIZ_TOE_DEF_SPI_MISO_PIN);
    cfg->pin_int = resolve_pin(cfg->pin_int, ESP_WIZ_TOE_DEF_SPI_INT_PIN);
    cfg->pin_rst = resolve_pin(cfg->pin_rst, ESP_WIZ_TOE_DEF_SPI_RST_PIN);
    if (cfg->lock_timeout_ms == 0) {
        cfg->lock_timeout_ms = ESP_WIZ_TOE_DEF_SPI_TIMEOUT_MS;
    }
}

static TickType_t get_wait_ticks(void)
{
    if (s_ctx.cfg.lock_timeout_ms == UINT32_MAX) {
        return portMAX_DELAY;
    }
    return pdMS_TO_TICKS(s_ctx.cfg.lock_timeout_ms);
}

static esp_err_t spi_transfer_locked(const uint8_t *tx, uint8_t *rx, size_t len)
{
    const TickType_t wait_ticks = get_wait_ticks();
    const bool auto_cs = !s_ctx.cs_active;

    if (len == 0) {
        return ESP_OK;
    }

    spi_transaction_t t = {
        .length = len * 8,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };

    if (!auto_cs) {
        // CS path already owns recursive lock and SPI bus.
        return spi_device_transmit(s_ctx.spi_dev, &t);
    }

    if (xSemaphoreTakeRecursive(s_ctx.lock, wait_ticks) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = spi_device_acquire_bus(s_ctx.spi_dev, portMAX_DELAY);
    if (ret != ESP_OK) {
        (void)xSemaphoreGiveRecursive(s_ctx.lock);
        return ret;
    }

    gpio_set_level(s_ctx.cfg.pin_cs, 0);
    ret = spi_device_transmit(s_ctx.spi_dev, &t);
    gpio_set_level(s_ctx.cfg.pin_cs, 1);

    spi_device_release_bus(s_ctx.spi_dev);
    (void)xSemaphoreGiveRecursive(s_ctx.lock);
    return ret;
}

#if ESP_WIZ_TOE_HAS_IOLIB
static void wizchip_cs_select(void)
{
    const TickType_t wait_ticks = get_wait_ticks();

    if (xSemaphoreTakeRecursive(s_ctx.lock, wait_ticks) != pdTRUE) {
        ESP_LOGE(TAG, "CS select lock timeout");
        return;
    }

    if (spi_device_acquire_bus(s_ctx.spi_dev, portMAX_DELAY) != ESP_OK) {
        ESP_LOGE(TAG, "CS select bus acquire timeout");
        (void)xSemaphoreGiveRecursive(s_ctx.lock);
        return;
    }

    gpio_set_level(s_ctx.cfg.pin_cs, 0);
    s_ctx.cs_active = true;
}

static void wizchip_cs_deselect(void)
{
    if (!s_ctx.cs_active) {
        return;
    }

    gpio_set_level(s_ctx.cfg.pin_cs, 1);
    s_ctx.cs_active = false;
    spi_device_release_bus(s_ctx.spi_dev);
    (void)xSemaphoreGiveRecursive(s_ctx.lock);
}

static uint8_t wizchip_spi_read_byte(void)
{
    const uint8_t tx = 0x00;
    uint8_t rx = 0;

    if (spi_transfer_locked(&tx, &rx, 1) != ESP_OK) {
        ESP_LOGE(TAG, "read byte failed");
    }
    return rx;
}

static void wizchip_spi_write_byte(uint8_t byte)
{
    if (spi_transfer_locked(&byte, NULL, 1) != ESP_OK) {
        ESP_LOGE(TAG, "write byte failed");
    }
}

static void wizchip_spi_read_burst(uint8_t *buf, uint16_t len)
{
    if (buf == NULL || len == 0) {
        return;
    }

    if (spi_transfer_locked(NULL, buf, len) != ESP_OK) {
        ESP_LOGE(TAG, "read burst failed len=%u", (unsigned)len);
    }
}

static void wizchip_spi_write_burst(uint8_t *buf, uint16_t len)
{
    if (buf == NULL || len == 0) {
        return;
    }

    if (spi_transfer_locked(buf, NULL, len) != ESP_OK) {
        ESP_LOGE(TAG, "write burst failed len=%u", (unsigned)len);
    }
}

#if _WIZCHIP_ == W6100
static void wizchip_spi_read_burst_6100(uint8_t *buf, datasize_t len)
{
    if (len <= 0) {
        return;
    }
    wizchip_spi_read_burst(buf, (uint16_t)len);
}

static void wizchip_spi_write_burst_6100(uint8_t *buf, datasize_t len)
{
    if (len <= 0) {
        return;
    }
    wizchip_spi_write_burst(buf, (uint16_t)len);
}
#endif

static void wizchip_critical_enter(void)
{
    (void)xSemaphoreTakeRecursive(s_ctx.lock, get_wait_ticks());
}

static void wizchip_critical_exit(void)
{
    (void)xSemaphoreGiveRecursive(s_ctx.lock);
}
#endif

esp_err_t esp_wiz_toe_spi_init(const esp_wiz_toe_spi_config_t *cfg)
{
    esp_err_t ret = ESP_OK;

    ESP_RETURN_ON_FALSE(cfg != NULL, ESP_ERR_INVALID_ARG, TAG, "cfg is NULL");

    if (s_ctx.initialized) {
        ESP_LOGW(TAG, "already initialized");
        return ESP_OK;
    }

    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.cfg = *cfg;
    apply_defaults(&s_ctx.cfg);

    s_ctx.lock = xSemaphoreCreateRecursiveMutex();
    ESP_RETURN_ON_FALSE(s_ctx.lock != NULL, ESP_ERR_NO_MEM, TAG, "failed to create SPI mutex");

    spi_bus_config_t buscfg = {
        .mosi_io_num = s_ctx.cfg.pin_mosi,
        .miso_io_num = s_ctx.cfg.pin_miso,
        .sclk_io_num = s_ctx.cfg.pin_sclk,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 2048,
    };
    ESP_GOTO_ON_ERROR(spi_bus_initialize(s_ctx.cfg.host_id, &buscfg, SPI_DMA_CH_AUTO), err, TAG, "spi_bus_initialize failed");

    spi_device_interface_config_t devcfg = {
        .mode = 0,
        .clock_speed_hz = s_ctx.cfg.clock_hz,
        .spics_io_num = -1,
        .queue_size = 1,
    };
    ESP_GOTO_ON_ERROR(spi_bus_add_device(s_ctx.cfg.host_id, &devcfg, &s_ctx.spi_dev), err_bus, TAG, "spi_bus_add_device failed");

    gpio_config_t out_cfg = {
        .pin_bit_mask = (1ULL << s_ctx.cfg.pin_cs) | (1ULL << s_ctx.cfg.pin_rst),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_GOTO_ON_ERROR(gpio_config(&out_cfg), err_dev, TAG, "gpio_config output failed");

    gpio_config_t int_cfg = {
        .pin_bit_mask = (1ULL << s_ctx.cfg.pin_int),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_GOTO_ON_ERROR(gpio_config(&int_cfg), err_dev, TAG, "gpio_config int failed");

    gpio_set_level(s_ctx.cfg.pin_cs, 1);
    gpio_set_level(s_ctx.cfg.pin_rst, 1);

    s_ctx.cs_active = false;
    s_ctx.initialized = true;

    ESP_LOGI(TAG, "SPI transport initialized host=%d clk=%d", (int)s_ctx.cfg.host_id, s_ctx.cfg.clock_hz);
    return ESP_OK;

err_dev:
    (void)spi_bus_remove_device(s_ctx.spi_dev);
    s_ctx.spi_dev = NULL;
err_bus:
    (void)spi_bus_free(s_ctx.cfg.host_id);
err:
    vSemaphoreDelete(s_ctx.lock);
    s_ctx.lock = NULL;
    memset(&s_ctx, 0, sizeof(s_ctx));
    return ret;
}

esp_err_t esp_wiz_toe_spi_deinit(void)
{
    if (!s_ctx.initialized) {
        return ESP_OK;
    }

    if (s_ctx.spi_dev != NULL) {
        (void)spi_bus_remove_device(s_ctx.spi_dev);
        s_ctx.spi_dev = NULL;
    }
    (void)spi_bus_free(s_ctx.cfg.host_id);

    if (s_ctx.lock != NULL) {
        vSemaphoreDelete(s_ctx.lock);
        s_ctx.lock = NULL;
    }

    memset(&s_ctx, 0, sizeof(s_ctx));
    return ESP_OK;
}

esp_err_t esp_wiz_toe_spi_register_iolib_callbacks(void)
{
    ESP_RETURN_ON_FALSE(s_ctx.initialized, ESP_ERR_INVALID_STATE, TAG, "SPI is not initialized");

#if ESP_WIZ_TOE_HAS_IOLIB
    reg_wizchip_cris_cbfunc(wizchip_critical_enter, wizchip_critical_exit);
    reg_wizchip_cs_cbfunc(wizchip_cs_select, wizchip_cs_deselect);
#if _WIZCHIP_ == W6100
    reg_wizchip_spi_cbfunc(
        wizchip_spi_read_byte,
        wizchip_spi_write_byte,
        wizchip_spi_read_burst_6100,
        wizchip_spi_write_burst_6100);
#else
    reg_wizchip_spi_cbfunc(wizchip_spi_read_byte, wizchip_spi_write_byte);
#endif
    reg_wizchip_spiburst_cbfunc(wizchip_spi_read_burst, wizchip_spi_write_burst);
    return ESP_OK;
#else
    ESP_LOGW(TAG, "ioLibrary headers not found; callback registration skipped");
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t esp_wiz_toe_spi_reset(void)
{
    ESP_RETURN_ON_FALSE(s_ctx.initialized, ESP_ERR_INVALID_STATE, TAG, "SPI is not initialized");

    if (s_ctx.cfg.pin_rst < 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    gpio_set_level(s_ctx.cfg.pin_rst, 0);
    vTaskDelay(pdMS_TO_TICKS(2));
    gpio_set_level(s_ctx.cfg.pin_rst, 1);
    vTaskDelay(pdMS_TO_TICKS(150));

    return ESP_OK;
}

esp_err_t esp_wiz_toe_spi_wizchip_check(void)
{
    ESP_RETURN_ON_FALSE(s_ctx.initialized, ESP_ERR_INVALID_STATE, TAG, "SPI is not initialized");

#if ESP_WIZ_TOE_HAS_IOLIB
#if (_WIZCHIP_ == W5100S)
    const uint8_t ver = getVER();
    if (ver != 0x51) {
        ESP_LOGE(TAG, "W5100S version mismatch: 0x%02X (expected 0x51)", ver);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "W5100S version check OK: 0x%02X", ver);
    return ESP_OK;
#elif (_WIZCHIP_ == W5500)
    const uint8_t ver = getVERSIONR();
    if (ver != 0x04) {
        ESP_LOGE(TAG, "W5500 version mismatch: 0x%02X (expected 0x04)", ver);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "W5500 version check OK: 0x%02X", ver);
    return ESP_OK;
#elif (_WIZCHIP_ == W6100)
    const uint16_t cid = getCIDR();
    if (cid != 0x6100) {
        ESP_LOGE(TAG, "W6100 CID mismatch: 0x%04X (expected 0x6100)", cid);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "W6100 CID check OK: 0x%04X", cid);
    return ESP_OK;
#elif (_WIZCHIP_ == W6300)
    const uint16_t cid = getCIDR();
    if (cid != 0x6300) {
        ESP_LOGE(TAG, "W6300 CID mismatch: 0x%04X (expected 0x6300)", cid);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "W6300 CID check OK: 0x%04X", cid);
    return ESP_OK;
#else
    uint8_t chip_id[7] = {0};
    if (ctlwizchip(CW_GET_ID, chip_id) != 0) {
        ESP_LOGE(TAG, "WIZchip ID read failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "WIZchip ID: %s", chip_id);
    return ESP_OK;
#endif
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t esp_wiz_toe_spi_link_is_up(bool *is_up)
{
    ESP_RETURN_ON_FALSE(is_up != NULL, ESP_ERR_INVALID_ARG, TAG, "is_up is NULL");
    ESP_RETURN_ON_FALSE(s_ctx.initialized, ESP_ERR_INVALID_STATE, TAG, "SPI is not initialized");

#if ESP_WIZ_TOE_HAS_IOLIB
    uint8_t link_state = 0;
    if (ctlwizchip(CW_GET_PHYLINK, (void *)&link_state) != 0) {
        *is_up = false;
        return ESP_FAIL;
    }
    *is_up = (link_state != 0);
    return ESP_OK;
#else
    *is_up = false;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}
