#include "wsm_driver.h"

#include <stdint.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "toe_port.h"   /* neutral yield/time helpers used by wiznet_toe.c */

#if __has_include("wizchip_conf.h")
#include "wizchip_conf.h"
#define WSM_DRIVER_HAS_IOLIB 1
#else
#define WSM_DRIVER_HAS_IOLIB 0
#endif

/* W6300 uses QSPI framing (opcode + 16-bit address + dummy + data) instead of
 * the W5500 byte/burst callback interface. The defined(W6300) guard keeps the
 * comparison safe when ioLibrary headers are absent. */
#if WSM_DRIVER_HAS_IOLIB && defined(W6300) && (_WIZCHIP_ == W6300)
#define WSM_DRIVER_USE_QSPI 1
#else
#define WSM_DRIVER_USE_QSPI 0
#endif

static const char *TAG = "wsm_driver_spi";

/* Round a sub-tick millisecond delay up to one tick. pdMS_TO_TICKS() truncates,
 * so at the ESP-IDF default CONFIG_FREERTOS_HZ=100 anything below 10 ms becomes
 * vTaskDelay(0) -- a bare taskYIELD() that never blocks. In the TOE poll loops
 * (wiztoe_accept/recv) that starves the lower-priority idle task and trips the
 * task watchdog, so every delay here must be at least 1 tick regardless of the
 * tick rate the surrounding project happens to use. */
#define WSM_DRIVER_DELAY_TICKS(ms) \
    (pdMS_TO_TICKS(ms) > 0 ? pdMS_TO_TICKS(ms) : (TickType_t)1)

/* ---- neutral port helpers (toe_port.h) ----
 * Provided here (the TOE SPI transport TU) so wiznet_toe.c stays free of any
 * FreeRTOS/esp_timer headers. Both TUs are TOE-only, so these are always linked
 * exactly when needed and never pulled into the esp_eth backend. */
void toe_yield_1ms(void)   { vTaskDelay(WSM_DRIVER_DELAY_TICKS(1)); }
uint32_t toe_time_us(void) { return (uint32_t)esp_timer_get_time(); }

#ifdef CONFIG_WSM_DRIVER_SPI_HOST
#define WSM_DRIVER_DEF_SPI_HOST ((spi_host_device_t)CONFIG_WSM_DRIVER_SPI_HOST)
#else
#define WSM_DRIVER_DEF_SPI_HOST SPI2_HOST
#endif

#ifdef CONFIG_WSM_DRIVER_SPI_CLOCK_HZ
#define WSM_DRIVER_DEF_SPI_CLOCK_HZ CONFIG_WSM_DRIVER_SPI_CLOCK_HZ
#else
#define WSM_DRIVER_DEF_SPI_CLOCK_HZ (20 * 1000 * 1000)
#endif

#ifdef CONFIG_WSM_DRIVER_PIN_MISO
#define WSM_DRIVER_DEF_SPI_MISO_PIN ((gpio_num_t)CONFIG_WSM_DRIVER_PIN_MISO)
#else
#define WSM_DRIVER_DEF_SPI_MISO_PIN GPIO_NUM_13
#endif

#ifdef CONFIG_WSM_DRIVER_PIN_MOSI
#define WSM_DRIVER_DEF_SPI_MOSI_PIN ((gpio_num_t)CONFIG_WSM_DRIVER_PIN_MOSI)
#else
#define WSM_DRIVER_DEF_SPI_MOSI_PIN GPIO_NUM_11
#endif

#ifdef CONFIG_WSM_DRIVER_PIN_SCLK
#define WSM_DRIVER_DEF_SPI_CLK_PIN ((gpio_num_t)CONFIG_WSM_DRIVER_PIN_SCLK)
#else
#define WSM_DRIVER_DEF_SPI_CLK_PIN GPIO_NUM_12
#endif

#ifdef CONFIG_WSM_DRIVER_PIN_CS
#define WSM_DRIVER_DEF_SPI_CS_PIN ((gpio_num_t)CONFIG_WSM_DRIVER_PIN_CS)
#else
#define WSM_DRIVER_DEF_SPI_CS_PIN GPIO_NUM_10
#endif

#ifdef CONFIG_WSM_DRIVER_PIN_RST
#define WSM_DRIVER_DEF_SPI_RST_PIN ((gpio_num_t)CONFIG_WSM_DRIVER_PIN_RST)
#else
#define WSM_DRIVER_DEF_SPI_RST_PIN GPIO_NUM_9
#endif

#ifdef CONFIG_WSM_DRIVER_PIN_INT
#define WSM_DRIVER_DEF_SPI_INT_PIN ((gpio_num_t)CONFIG_WSM_DRIVER_PIN_INT)
#else
#define WSM_DRIVER_DEF_SPI_INT_PIN GPIO_NUM_14
#endif

#ifdef CONFIG_WSM_DRIVER_PIN_IO2
#define WSM_DRIVER_DEF_SPI_IO2_PIN ((gpio_num_t)CONFIG_WSM_DRIVER_PIN_IO2)
#else
#define WSM_DRIVER_DEF_SPI_IO2_PIN GPIO_NUM_15
#endif

#ifdef CONFIG_WSM_DRIVER_PIN_IO3
#define WSM_DRIVER_DEF_SPI_IO3_PIN ((gpio_num_t)CONFIG_WSM_DRIVER_PIN_IO3)
#else
#define WSM_DRIVER_DEF_SPI_IO3_PIN GPIO_NUM_16
#endif

#define WSM_DRIVER_DEF_SPI_TIMEOUT_MS 1000U

typedef struct {
    bool initialized;
    bool cs_active;
    bool bus_owned;        /* true only if we called spi_bus_initialize() ourselves */
    spi_device_handle_t spi_dev;
    SemaphoreHandle_t lock;
    wsm_driver_spi_config_t cfg;
} wsm_driver_spi_context_t;

static wsm_driver_spi_context_t s_ctx;

static gpio_num_t resolve_pin(gpio_num_t configured, gpio_num_t fallback)
{
    return configured >= 0 ? configured : fallback;
}

static void apply_defaults(wsm_driver_spi_config_t *cfg)
{
    if (cfg->host_id != SPI2_HOST && cfg->host_id != SPI3_HOST) {
        cfg->host_id = WSM_DRIVER_DEF_SPI_HOST;
    }
    if (cfg->clock_hz <= 0) {
        cfg->clock_hz = WSM_DRIVER_DEF_SPI_CLOCK_HZ;
    }
    cfg->pin_sclk = resolve_pin(cfg->pin_sclk, WSM_DRIVER_DEF_SPI_CLK_PIN);
    cfg->pin_cs = resolve_pin(cfg->pin_cs, WSM_DRIVER_DEF_SPI_CS_PIN);
    cfg->pin_mosi = resolve_pin(cfg->pin_mosi, WSM_DRIVER_DEF_SPI_MOSI_PIN);
    cfg->pin_miso = resolve_pin(cfg->pin_miso, WSM_DRIVER_DEF_SPI_MISO_PIN);
    cfg->pin_int = resolve_pin(cfg->pin_int, WSM_DRIVER_DEF_SPI_INT_PIN);
    cfg->pin_rst = resolve_pin(cfg->pin_rst, WSM_DRIVER_DEF_SPI_RST_PIN);
    cfg->pin_io2 = resolve_pin(cfg->pin_io2, WSM_DRIVER_DEF_SPI_IO2_PIN);
    cfg->pin_io3 = resolve_pin(cfg->pin_io3, WSM_DRIVER_DEF_SPI_IO3_PIN);
    if (cfg->lock_timeout_ms == 0) {
        cfg->lock_timeout_ms = WSM_DRIVER_DEF_SPI_TIMEOUT_MS;
    }
}

static TickType_t get_wait_ticks(void)
{
    if (s_ctx.cfg.lock_timeout_ms == UINT32_MAX) {
        return portMAX_DELAY;
    }
    return pdMS_TO_TICKS(s_ctx.cfg.lock_timeout_ms);
}

#if !WSM_DRIVER_USE_QSPI
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
#endif /* !WSM_DRIVER_USE_QSPI */

#if WSM_DRIVER_HAS_IOLIB
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

#if !WSM_DRIVER_USE_QSPI
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
#endif /* !WSM_DRIVER_USE_QSPI */

#if WSM_DRIVER_USE_QSPI
/*
 * W6300 QSPI frame: opcode (8 bits, always 1-line) + 16-bit address + dummy
 * clocks + data. The dummy clocks (8 in single mode, 2 in quad mode) are
 * emitted by widening the address phase to 24 bits with a trailing zero byte,
 * which matches the official WIZnet-PICO-C reference bit-for-bit: it transmits
 * the dummy as a driven 0x00 byte after the address in both modes.
 *
 * Callbacks are invoked by w6300.c with CS already asserted (CS._select())
 * and the recursive lock held, so transactions go straight to the device.
 */
static void wizchip_qspi_xfer(uint8_t opcode, uint16_t addr, const uint8_t *tx, uint8_t *rx, uint16_t len)
{
    if (len == 0) {
        return;
    }

    spi_transaction_ext_t t = {
        .base = {
            .flags = SPI_TRANS_VARIABLE_CMD | SPI_TRANS_VARIABLE_ADDR | SPI_TRANS_VARIABLE_DUMMY,
            .cmd = opcode,
            .addr = (uint32_t)addr << 8,
            .length = (tx != NULL) ? (size_t)len * 8 : 0,
            .rxlength = (rx != NULL) ? (size_t)len * 8 : 0,
            .tx_buffer = tx,
            .rx_buffer = rx,
        },
        .command_bits = 8,
        .address_bits = 24,
        .dummy_bits = 0,
    };

#ifdef CONFIG_WSM_DRIVER_QSPI_QUAD
    t.base.flags |= SPI_TRANS_MODE_QIO | SPI_TRANS_MULTILINE_ADDR;
#endif

    esp_err_t ret = spi_device_transmit(s_ctx.spi_dev, (spi_transaction_t *)&t);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "qspi xfer failed op=0x%02X addr=0x%04X len=%u (%s)",
                 opcode, addr, (unsigned)len, esp_err_to_name(ret));
    }
}

static void wizchip_qspi_read(uint8_t opcode, uint16_t addr, uint8_t *buf, uint16_t len)
{
    
    wizchip_qspi_xfer(opcode, addr, NULL, buf, len);
}

static void wizchip_qspi_write(uint8_t opcode, uint16_t addr, uint8_t *buf, uint16_t len)
{
    wizchip_qspi_xfer(opcode, addr, buf, NULL, len);
}
#endif /* WSM_DRIVER_USE_QSPI */

static void wizchip_critical_enter(void)
{
    (void)xSemaphoreTakeRecursive(s_ctx.lock, get_wait_ticks());
}

static void wizchip_critical_exit(void)
{
    (void)xSemaphoreGiveRecursive(s_ctx.lock);
}
#endif

esp_err_t wsm_driver_spi_init(const wsm_driver_spi_config_t *cfg)
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
        /* Must be -1, not left zero-initialised. spicommon_bus_initialize_io()
         * gates its octal (data4..data7) wiring on `flags & SPICOMMON_BUSFLAG_OCTAL`
         * rather than `(flags & OCTAL) == OCTAL`, and OCTAL is a superset of QUAD,
         * so a plain quad bus enters that branch too. Left at 0 these read as
         * GPIO0 -- a boot strapping pin -- and routing it to SPI panics the chip
         * with "Cache error / MMU entry fault". At -1 the branch skips them. */
        .data4_io_num = -1,
        .data5_io_num = -1,
        .data6_io_num = -1,
        .data7_io_num = -1,
        .max_transfer_sz = 2048,
    };
#if WSM_DRIVER_USE_QSPI
    // A single QSPI burst can span a full socket buffer; cover the largest
    // size selectable via Kconfig (16 KB) plus the frame header.
    buscfg.max_transfer_sz = 16 * 1024 + 8;
#ifdef CONFIG_WSM_DRIVER_QSPI_QUAD
    buscfg.quadwp_io_num = s_ctx.cfg.pin_io2;
    buscfg.quadhd_io_num = s_ctx.cfg.pin_io3;
    buscfg.flags = SPICOMMON_BUSFLAG_MASTER | SPICOMMON_BUSFLAG_QUAD;
#endif
#endif
    // Bus ownership is declared by the caller (see wsm_driver_spi_config_t).
    // When the application already initialized the bus we only add our device,
    // and we must not free the bus on deinit either.
    if (!s_ctx.cfg.bus_initialized_by_caller) {
        ESP_GOTO_ON_ERROR(spi_bus_initialize(s_ctx.cfg.host_id, &buscfg, SPI_DMA_CH_AUTO), err, TAG,
                          "spi_bus_initialize failed");
        s_ctx.bus_owned = true;
    } else {
        ESP_LOGD(TAG, "using caller-initialized SPI bus host=%d", (int)s_ctx.cfg.host_id);
    }

    spi_device_interface_config_t devcfg = {
        .mode = 0,
        .clock_speed_hz = s_ctx.cfg.clock_hz,
        .spics_io_num = -1,
        .queue_size = 1,
#if WSM_DRIVER_USE_QSPI
        // QSPI frames are phased (cmd/addr/dummy/data) -> half-duplex device.
        .flags = SPI_DEVICE_HALFDUPLEX,
#endif
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
    if (s_ctx.bus_owned) {
        (void)spi_bus_free(s_ctx.cfg.host_id);
    }
err:
    vSemaphoreDelete(s_ctx.lock);
    s_ctx.lock = NULL;
    memset(&s_ctx, 0, sizeof(s_ctx));
    return ret;
}

esp_err_t wsm_driver_spi_deinit(void)
{
    if (!s_ctx.initialized) {
        return ESP_OK;
    }

    if (s_ctx.spi_dev != NULL) {
        (void)spi_bus_remove_device(s_ctx.spi_dev);
        s_ctx.spi_dev = NULL;
    }
    /* Only release a bus we created; a caller-owned bus outlives this driver. */
    if (s_ctx.bus_owned) {
        (void)spi_bus_free(s_ctx.cfg.host_id);
    }

    if (s_ctx.lock != NULL) {
        vSemaphoreDelete(s_ctx.lock);
        s_ctx.lock = NULL;
    }

    memset(&s_ctx, 0, sizeof(s_ctx));
    return ESP_OK;
}

esp_err_t wsm_driver_spi_register_iolib_callbacks(void)
{
    ESP_RETURN_ON_FALSE(s_ctx.initialized, ESP_ERR_INVALID_STATE, TAG, "SPI is not initialized");

#if WSM_DRIVER_HAS_IOLIB
    reg_wizchip_cris_cbfunc(wizchip_critical_enter, wizchip_critical_exit);
    reg_wizchip_cs_cbfunc(wizchip_cs_select, wizchip_cs_deselect);
#if WSM_DRIVER_USE_QSPI
    // WIZCHIP.IF is a union: registering the SPI byte callbacks as well would
    // clobber the QSPI function pointers, so register only the QSPI pair.
    reg_wizchip_qspi_cbfunc(wizchip_qspi_read, wizchip_qspi_write);
#elif _WIZCHIP_ == W6100
    reg_wizchip_spi_cbfunc(
        wizchip_spi_read_byte,
        wizchip_spi_write_byte,
        wizchip_spi_read_burst_6100,
        wizchip_spi_write_burst_6100);
    reg_wizchip_spiburst_cbfunc(wizchip_spi_read_burst, wizchip_spi_write_burst);
#else
    reg_wizchip_spi_cbfunc(wizchip_spi_read_byte, wizchip_spi_write_byte);
    reg_wizchip_spiburst_cbfunc(wizchip_spi_read_burst, wizchip_spi_write_burst);
#endif
    return ESP_OK;
#else
    ESP_LOGW(TAG, "ioLibrary headers not found; callback registration skipped");
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t wsm_driver_spi_reset(void)
{
    ESP_RETURN_ON_FALSE(s_ctx.initialized, ESP_ERR_INVALID_STATE, TAG, "SPI is not initialized");

    if (s_ctx.cfg.pin_rst < 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    gpio_set_level(s_ctx.cfg.pin_rst, 0);
    vTaskDelay(WSM_DRIVER_DELAY_TICKS(2));
    gpio_set_level(s_ctx.cfg.pin_rst, 1);
    vTaskDelay(WSM_DRIVER_DELAY_TICKS(150));

    return ESP_OK;
}

esp_err_t wsm_driver_spi_wizchip_check(void)
{
    ESP_RETURN_ON_FALSE(s_ctx.initialized, ESP_ERR_INVALID_STATE, TAG, "SPI is not initialized");

#if WSM_DRIVER_HAS_IOLIB
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

esp_err_t wsm_driver_spi_link_is_up(bool *is_up)
{
    ESP_RETURN_ON_FALSE(is_up != NULL, ESP_ERR_INVALID_ARG, TAG, "is_up is NULL");
    ESP_RETURN_ON_FALSE(s_ctx.initialized, ESP_ERR_INVALID_STATE, TAG, "SPI is not initialized");

#if WSM_DRIVER_HAS_IOLIB
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
