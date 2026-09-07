/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * TCP server multi-socket example configuration.
 *
 * Follows wsm_driver's config conventions:
 *   - SPI / pin wiring is NOT configured here. It comes from the component
 *     Kconfig (menuconfig -> Component config -> WIZnet WSM Driver) and is
 *     applied by net_backend_toe.c via wsm_driver_spi_config_t.
 *   - The network identity is expressed as wiz_NetInfo fields (byte arrays);
 *     main.c assembles a wiz_NetInfo from these macros and hands it to
 *     wiznet_net_init(), which applies it with wizchip_setnetinfo().
 */
#ifndef NET_CONFIG_H
#define NET_CONFIG_H

#include "sdkconfig.h"      /* CONFIG_WSM_DRIVER_SOCKET_WRAP */

/* ---- static network identity (wsm_driver style: wiz_NetInfo byte arrays) ---- */
#define NET_MAC_ADDR          {0x00, 0x08, 0xDC, 0x12, 0x34, 0x56}  /* WIZnet OUI */
#define NET_IP_ADDR           {192, 168, 11, 2}
#define NET_SUBNET_MASK       {255, 255, 255, 0}
#define NET_GATEWAY           {192, 168, 11, 1}
#define NET_DNS_ADDR          {8, 8, 8, 8}

/* ---- Wi-Fi STA config (fill in your AP credentials) ---- */
#define WIFI_SSID             "your-ssid"
#define WIFI_PASS             "your-password"

/* ---- multi-socket server config ----
 *
 * ONE PORT PER SOCKET, as in the original WIZnet-PICO-C example: listener i
 * binds PORT_BASE + i. The chip can in fact demultiplex several hardware
 * sockets listening on one port by 4-tuple, but one port per listener is what
 * the original does and it keeps the log readable. The Wi-Fi side uses its own
 * base so the two never clash when they share one LwIP stack (SOCKET_WRAP=0).
 *
 * HOW MANY LISTENERS THE TOE CAN AFFORD
 *
 * accept() has BSD semantics: it hands the established hardware socket to the
 * accepted connection and relocates the listener onto a free one, so a listener
 * that is serving a client occupies TWO of the chip's eight sockets (and two of
 * the driver's eight descriptors, which is the tighter limit -- accept() cannot
 * allocate a descriptor for the connection if every one is already a listener).
 *
 * Eight listeners therefore cannot accept anything at all: 8 descriptors are
 * spent before the first client arrives, and every accept() returns EWOULDBLOCK
 * forever. Four is the largest count where all listeners can serve a client at
 * the same time (4 listeners + 4 connections = 8).
 *
 * This is a TOE-only budget. With the esp_eth backend (SOCKET_WRAP=0) and on
 * Wi-Fi these are ordinary LwIP sockets, so the original 8 still applies there.
 * Each listener costs one task on both interfaces; lower this if RAM is tight.
 */
#define MULTI_SOCKET_PORT_BASE       5000   /* Ethernet: 5000..5000+COUNT-1 */
#define WIFI_MULTI_SOCKET_PORT_BASE  5100   /* Wi-Fi:    5100..5100+COUNT-1 */
#if CONFIG_WSM_DRIVER_SOCKET_WRAP
#define MULTI_SOCKET_COUNT           4      /* listener + connection, x4 = 8 */
#else
#define MULTI_SOCKET_COUNT           8      /* software LwIP: no chip budget */
#endif
#define MULTI_SOCKET_BUF_SIZE        2048

#endif /* NET_CONFIG_H */
