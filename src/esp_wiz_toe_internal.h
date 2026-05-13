#pragma once

#include <stdbool.h>

#include "driver/spi_master.h"
#include "esp_wiz_toe.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

typedef struct {
    bool initialized;
    spi_device_handle_t spi_dev;
    SemaphoreHandle_t lock;
    bool cs_active;
    esp_wiz_toe_config_t cfg;
} esp_wiz_toe_context_t;

esp_wiz_toe_context_t *esp_wiz_toe_get_context(void);
esp_err_t esp_wiz_toe_port_init(const esp_wiz_toe_config_t *cfg);
esp_err_t esp_wiz_toe_port_deinit(void);
esp_err_t esp_wiz_toe_port_register_iolib_callbacks(void);
bool esp_wiz_toe_port_link_is_up(void);
