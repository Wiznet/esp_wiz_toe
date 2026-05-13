#pragma once

#include "esp_err.h"
#include "esp_wiz_toe.h"

esp_err_t esp_wiz_toe_spi_init(const esp_wiz_toe_config_t *cfg);
esp_err_t esp_wiz_toe_spi_deinit(void);
esp_err_t esp_wiz_toe_spi_register_iolib_callbacks(void);
