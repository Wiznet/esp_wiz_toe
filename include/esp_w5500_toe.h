#pragma once

/**
 * @file esp_w5500_toe.h
 * @brief W5500 TOE (TCP/IP Offload Engine) public API for ESP32-S3.
 *
 * This component drives the WIZnet W5500 chip using its built-in hardware
 * TCP/IP stack (TOE).  It does NOT use:
 *   - esp_eth MAC RAW driver
 *   - lwIP network interface (netif)
 *
 * All socket operations are offloaded to W5500 hardware via SPI using the
 * WIZnet ioLibrary_Driver (third_party/ioLibrary_Driver).
 *
 * Typical usage:
 *   1. Populate esp_w5500_toe_config_t with SPI pins / IP settings.
 *   2. Call esp_w5500_toe_init().
 *   3. Use esp_w5500_toe_socket_*() API — or call ioLibrary socket() /
 *      connect() / send() / recv() functions directly.
 *   4. Call esp_w5500_toe_deinit() before power-down.
 */

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * Configuration structures
 * ---------------------------------------------------------------------- */

/**
 * @brief SPI bus and GPIO configuration for the W5500.
 */
typedef struct {
    int spi_host;       /**< SPI host: SPI2_HOST (1) or SPI3_HOST (2)        */
    int pin_mosi;       /**< MOSI GPIO number                                 */
    int pin_miso;       /**< MISO GPIO number                                 */
    int pin_sclk;       /**< SCLK GPIO number                                 */
    int pin_cs;         /**< Chip-select GPIO number (active low)             */
    int pin_rst;        /**< Reset GPIO number, or -1 if unused               */
    int pin_int;        /**< Interrupt GPIO number, or -1 if unused           */
    int clock_speed_mhz;/**< SPI clock frequency in MHz (max 80)             */
} esp_w5500_toe_spi_config_t;

/**
 * @brief Network identity configuration for the W5500.
 *
 * Passed verbatim to wizchip_setnetinfo() during init.
 */
typedef struct {
    uint8_t mac[6];     /**< MAC address (6 bytes)                            */
    uint8_t ip[4];      /**< Static IPv4 address                              */
    uint8_t subnet[4];  /**< Subnet mask                                      */
    uint8_t gateway[4]; /**< Default gateway                                  */
    uint8_t dns[4];     /**< Primary DNS server (informational)               */
    bool    use_dhcp;   /**< Reserved – DHCP not yet implemented              */
} esp_w5500_toe_net_config_t;

/**
 * @brief Top-level component configuration.
 */
typedef struct {
    esp_w5500_toe_spi_config_t spi;  /**< SPI / GPIO configuration           */
    esp_w5500_toe_net_config_t net;  /**< Network identity configuration     */
} esp_w5500_toe_config_t;

/* -------------------------------------------------------------------------
 * Component lifecycle
 * ---------------------------------------------------------------------- */

/**
 * @brief Initialise the W5500 TOE component.
 *
 * Performs hardware reset, SPI init, chip detection and network
 * configuration.  Must be called before any socket operations.
 *
 * @param config Pointer to a populated configuration structure.
 * @return ESP_OK on success, or an esp_err_t error code.
 */
esp_err_t esp_w5500_toe_init(const esp_w5500_toe_config_t *config);

/**
 * @brief De-initialise the W5500 TOE component.
 *
 * Releases the SPI device handle and GPIO resources.  All open sockets
 * must be closed by the caller before invoking this function.
 *
 * @return ESP_OK on success.
 */
esp_err_t esp_w5500_toe_deinit(void);

/**
 * @brief Return true if the component has been successfully initialised.
 */
bool esp_w5500_toe_is_ready(void);

/* -------------------------------------------------------------------------
 * Socket helpers (thin wrappers around ioLibrary socket API)
 * ---------------------------------------------------------------------- */

/**
 * @brief Open a TCP socket on the specified W5500 hardware socket number.
 *
 * @param sn       Hardware socket number (0 – W5500_TOE_MAX_SOCKETS-1).
 * @param port     Local TCP port to bind.
 * @return ESP_OK on success.
 */
esp_err_t esp_w5500_toe_tcp_open(uint8_t sn, uint16_t port);

/**
 * @brief Connect an open TCP socket to a remote host.
 *
 * @param sn       Hardware socket number.
 * @param dest_ip  Destination IPv4 address (4 bytes, big-endian).
 * @param dest_port Destination TCP port.
 * @return ESP_OK on success.
 */
esp_err_t esp_w5500_toe_tcp_connect(uint8_t sn,
                                    const uint8_t dest_ip[4],
                                    uint16_t dest_port);

/**
 * @brief Close and reset a hardware socket.
 *
 * @param sn Hardware socket number.
 * @return ESP_OK on success.
 */
esp_err_t esp_w5500_toe_socket_close(uint8_t sn);

/**
 * @brief Open a UDP socket on the specified W5500 hardware socket number.
 *
 * @param sn   Hardware socket number.
 * @param port Local UDP port to bind.
 * @return ESP_OK on success.
 */
esp_err_t esp_w5500_toe_udp_open(uint8_t sn, uint16_t port);

#ifdef __cplusplus
}
#endif
