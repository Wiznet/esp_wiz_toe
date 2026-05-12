#pragma once
/* Internal SPI layer – not part of the public component API */

#include <stdint.h>
#include "esp_err.h"
#include "esp_w5500_toe.h"   /* for esp_w5500_toe_spi_config_t */

esp_err_t esp_w5500_toe_spi_init(const esp_w5500_toe_spi_config_t *spi_cfg);
esp_err_t esp_w5500_toe_spi_deinit(void);

void    esp_w5500_toe_spi_cs_assert(void);
void    esp_w5500_toe_spi_cs_deassert(void);
uint8_t esp_w5500_toe_spi_read_byte(void);
void    esp_w5500_toe_spi_write_byte(uint8_t byte);
