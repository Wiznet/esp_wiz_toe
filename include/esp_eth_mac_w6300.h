/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * esp_eth MAC driver for the WIZnet W6300 used as a plain SPI/QSPI Ethernet MAC
 * (MACRAW on hardware socket 0) with the software LwIP stack on the ESP32-S3 —
 * the W6300 counterpart of esp_eth_mac_w5500.h.
 *
 * The chip's own hardware TCP/IP (TOE) is NOT used in this path; select
 * CONFIG_WSM_DRIVER_BACKEND_TOE for that.
 *
 * The API mirrors the W5500 driver so net_backend_eth.c differs only in which
 * *_new_* function it calls. The QSPI framing (opcode + 16-bit address + dummy
 * + data, single or quad per CONFIG_WSM_DRIVER_QSPI_*) is handled internally,
 * so the SPI device MUST be created half-duplex — see the note on spi_devcfg.
 */

#pragma once

#include "esp_eth_com.h"
#include "esp_eth_mac_spi.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief W6300 specific configuration
 *
 */
typedef struct {
    int int_gpio_num;                                   /*!< Interrupt GPIO number, set -1 to not use interrupt and to poll rx status periodically */
    uint32_t poll_period_ms;                            /*!< Period in ms to poll rx status when interrupt mode is not used */
    spi_host_device_t spi_host_id;                      /*!< SPI peripheral (this field is invalid when custom SPI driver is defined) */
    spi_device_interface_config_t *spi_devcfg;          /*!< SPI device configuration. MUST have SPI_DEVICE_HALFDUPLEX set: the W6300
                                                             QSPI frame is phased (opcode / address / dummy / data), which the ESP32
                                                             SPI master can only emit in half-duplex mode. command_bits /
                                                             address_bits are filled in by the driver when left at 0.
                                                             (this field is invalid when custom SPI driver is defined) */
    eth_spi_custom_driver_config_t custom_spi_driver;   /*!< Custom SPI driver definitions */
} eth_w6300_config_t;

/**
 * @brief Default W6300 specific configuration
 *
 */
#define ETH_W6300_DEFAULT_CONFIG(spi_host, spi_devcfg_p) \
    {                                           \
        .int_gpio_num = 8,                      \
        .poll_period_ms = 0,                    \
        .spi_host_id = spi_host,                \
        .spi_devcfg = spi_devcfg_p,             \
        .custom_spi_driver = ETH_DEFAULT_SPI,   \
    }

/**
* @brief Create W6300 Ethernet MAC instance
*
* @param w6300_config: W6300 specific configuration
* @param mac_config: Ethernet MAC configuration
*
* @return
*      - instance: create MAC instance successfully
*      - NULL: create MAC instance failed because some error occurred
*/
esp_eth_mac_t *esp_eth_mac_new_w6300(const eth_w6300_config_t *w6300_config, const eth_mac_config_t *mac_config);

#ifdef __cplusplus
}
#endif
