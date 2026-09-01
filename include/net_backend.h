/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * WIZnet TOE backend bring-up (see src/net_backend_toe.c).
 *
 * Config conventions follow wsm_driver:
 *   - pins / SPI -> wsm_driver_spi_config_t built from Kconfig (CONFIG_WSM_DRIVER_*)
 *   - network    -> wiz_NetInfo + wizchip_setnetinfo() (ioLibrary standard)
 */
#ifndef NET_BACKEND_H
#define NET_BACKEND_H

#include <stdbool.h>

#include "esp_err.h"
#include "wizchip_conf.h"   /* wiz_NetInfo */
#include "wsm_driver.h"     /* wsm_driver_spi_config_t */

/* Isolation layer: wizchip_conf.h -> W5500/w5500.h:920,925 defines SOCK_STREAM
 * and SOCK_DGRAM as ioLibrary's Sn_MR protocol values (0x01 / 0x02), while
 * lwip/sockets.h:197,198 defines the same names as POSIX socket types (1 / 2).
 * Neither header guards its definition, so any TU that sees both gets a
 * -Werror redefinition error (the numeric values happen to match; the conflict
 * is purely at the preprocessor level).
 *
 * This component's public API is meant to be used alongside lwIP BSD sockets,
 * so the ioLibrary aliases are dropped here and the lwIP definitions are left
 * to win. Nothing in this project needs the aliases: code that talks to the
 * hardware sockets spells the protocol out as Sn_MR_TCP / Sn_MR_UDP (see
 * port/ioLibrary_Driver/src/wiznet_toe.c). W6300/w6300.h does not define them
 * at all, which is why only the W5500 build ever hit this.
 *
 * ORDERING: include this header (or any other ioLibrary header) BEFORE
 * lwip/sockets.h. In the reverse order w5500.h redefines lwIP's macros before
 * this point is reached and the error comes back. */
#undef SOCK_STREAM
#undef SOCK_DGRAM

#ifdef __cplusplus
extern "C" {
#endif

/* Bring up lwIP (shadow esp_netif holding the IPv4 identity) + the WIZnet chip
 * over SPI (pins/SPI from Kconfig), then apply the network identity to the
 * chip's hardware TCP/IP stack via wizchip_setnetinfo(net_info).
 * Blocks only for chip init, not for link. */
void wiznet_net_init(const wiz_NetInfo *net_info);

/* True once bring-up has completed. */
bool wiznet_net_is_up(void);

/* Lower-level TOE bring-up, for applications that already own their network
 * lifecycle (an existing esp_netif, event loop, or SPI bus).
 *
 * Does exactly the chip half of wiznet_net_init():
 *     SPI transport -> ioLibrary callbacks -> reset -> version check
 *     -> wizchip_init() -> wizchip_setnetinfo(net_info)
 *
 * and deliberately does NOT touch anything the application may own:
 * no esp_netif_init(), no esp_event_loop_create_default(), no esp_netif_new(),
 * no DHCP and no DNS. The caller supplies the wiring through spi_cfg instead of
 * the CONFIG_WSM_DRIVER_PIN_* Kconfig values, and decides via
 * spi_cfg->bus_initialized_by_caller whether the SPI bus is initialized here.
 *
 * wiznet_net_init() is unchanged and still available for standalone apps; it is
 * now implemented on top of this function.
 *
 * @return ESP_OK on success, otherwise the first failing step's error. */
esp_err_t wiznet_toe_bringup(const wsm_driver_spi_config_t *spi_cfg,
                             const wiz_NetInfo *net_info);

#ifdef __cplusplus
}
#endif

#endif /* NET_BACKEND_H */
