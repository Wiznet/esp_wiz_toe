#pragma once
/* Internal socket helper declarations – not part of the public component API */

#include <stdint.h>
#include "esp_err.h"

esp_err_t w5500_toe_socket_tcp_open(uint8_t sn, uint16_t port);
esp_err_t w5500_toe_socket_tcp_connect(uint8_t sn,
                                       const uint8_t dest_ip[4],
                                       uint16_t dest_port);
esp_err_t w5500_toe_socket_udp_open(uint8_t sn, uint16_t port);
esp_err_t w5500_toe_socket_close(uint8_t sn);
