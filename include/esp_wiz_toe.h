#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ESP_WIZ_TOE_IP_MODE_STATIC = 0,
    ESP_WIZ_TOE_IP_MODE_DHCP,
} esp_wiz_toe_ip_mode_t;

typedef enum {
    ESP_WIZ_TOE_LINK_STATUS_DOWN = 0,
    ESP_WIZ_TOE_LINK_STATUS_UP,
    ESP_WIZ_TOE_LINK_STATUS_UNKNOWN,
} esp_wiz_toe_link_status_t;

typedef enum {
    ESP_WIZ_TOE_PROTO_TCP = 0x01,
    ESP_WIZ_TOE_PROTO_UDP = 0x02,
} esp_wiz_toe_protocol_t;

typedef struct {
    spi_host_device_t host_id;
    int clock_hz;
    gpio_num_t pin_miso;
    gpio_num_t pin_mosi;
    gpio_num_t pin_sclk;
    gpio_num_t pin_cs;
    gpio_num_t pin_rst;
    gpio_num_t pin_int;
} esp_wiz_toe_spi_config_t;

typedef struct {
    uint8_t mac[6];
} esp_wiz_toe_mac_config_t;

typedef struct {
    uint8_t ip[4];
    uint8_t netmask[4];
    uint8_t gateway[4];
    uint8_t dns[4];
} esp_wiz_toe_static_ip_config_t;

typedef struct {
    uint32_t connect_timeout_ms;
    uint32_t send_timeout_ms;
    uint32_t recv_timeout_ms;
} esp_wiz_toe_timeout_config_t;

typedef struct {
    esp_wiz_toe_spi_config_t spi;
    esp_wiz_toe_mac_config_t mac;
    esp_wiz_toe_ip_mode_t ip_mode;
    esp_wiz_toe_static_ip_config_t static_ip;
    esp_wiz_toe_timeout_config_t timeout;
} esp_wiz_toe_config_t;

esp_err_t esp_wiz_toe_init(const esp_wiz_toe_config_t *config);
esp_err_t esp_wiz_toe_deinit(void);
esp_err_t esp_wiz_toe_reset(void);
bool esp_wiz_toe_is_ready(void);
esp_err_t esp_wiz_toe_is_link_up(bool *is_up);
esp_err_t esp_wiz_toe_get_link_status(esp_wiz_toe_link_status_t *status);

esp_err_t esp_wiz_toe_socket(uint8_t sn,
                             uint8_t protocol,
                             uint16_t local_port,
                             uint8_t flag,
                             uint32_t timeout_ms);
esp_err_t esp_wiz_toe_connect(uint8_t sn,
                              const uint8_t remote_ip[4],
                              uint16_t remote_port,
                              uint32_t timeout_ms);
esp_err_t esp_wiz_toe_listen(uint8_t sn, uint32_t timeout_ms);

esp_err_t esp_wiz_toe_tcp_connect(uint8_t sn,
                                  const uint8_t remote_ip[4],
                                  uint16_t remote_port,
                                  uint16_t local_port,
                                  uint32_t timeout_ms);
esp_err_t esp_wiz_toe_tcp_listen(uint8_t sn, uint16_t local_port, uint32_t timeout_ms);
esp_err_t esp_wiz_toe_tcp_accept_wait(uint8_t sn, uint32_t timeout_ms);
esp_err_t esp_wiz_toe_udp_open(uint8_t sn, uint16_t local_port, uint32_t timeout_ms);
esp_err_t esp_wiz_toe_udp_sendto(uint8_t sn,
                                 const void *data,
                                 size_t len,
                                 const uint8_t dest_ip[4],
                                 uint16_t dest_port,
                                 uint32_t timeout_ms,
                                 size_t *sent_len);
esp_err_t esp_wiz_toe_udp_recvfrom(uint8_t sn,
                                   void *buf,
                                   size_t len,
                                   uint32_t timeout_ms,
                                   size_t *recv_len,
                                   uint8_t src_ip[4],
                                   uint16_t *src_port);
esp_err_t esp_wiz_toe_dhcp_start(uint32_t timeout_ms);
esp_err_t esp_wiz_toe_dhcp_stop(void);
esp_err_t esp_wiz_toe_dns_resolve(const char *hostname, uint8_t out_ip[4], uint32_t timeout_ms);
esp_err_t esp_wiz_toe_send(uint8_t sn,
                           const void *data,
                           size_t len,
                           uint32_t timeout_ms,
                           size_t *sent_len);
esp_err_t esp_wiz_toe_recv(uint8_t sn,
                           void *buf,
                           size_t len,
                           uint32_t timeout_ms,
                           size_t *recv_len);
esp_err_t esp_wiz_toe_close(uint8_t sn);

#ifdef __cplusplus
}
#endif
