/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * WIZnet TOE (W5500 / W6300) + Wi-Fi multi-socket TCP echo server demo.
 *
 * app_main brings up BOTH interfaces and starts a multi-socket echo server on
 * each as its OWN set of tasks, at the same level (see multi_socket_start):
 *   - Ethernet (WIZnet chip) on MULTI_SOCKET_PORT_BASE + 0..COUNT-1
 *   - Wi-Fi STA              on WIFI_MULTI_SOCKET_PORT_BASE + 0..COUNT-1
 * app_main itself just orchestrates: init both stacks, start both servers.
 *
 * One port per socket, as in the original WIZnet-PICO-C example. The chip can
 * in fact demultiplex several hardware sockets listening on one port by 4-tuple,
 * so this is a presentation choice rather than a hardware limit; consecutive
 * ports keep the log readable and match the original. See net_config.h for how
 * many listeners the TOE can afford.
 *
 * The server logic lives in the backend-neutral engine multi_socket.c; each
 * interface supplies a socket vtable. Ethernet uses the plain lwIP BSD entry
 * points, which the wsm_driver component redirects to the WIZnet hardware
 * sockets at link time via -Wl,--wrap (see wiztoe_wrap.c). The Wi-Fi path's
 * --wrap awareness is isolated to wifi_multi_socket.c.
 *
 * Config conventions follow wsm_driver:
 *   - SPI / pins  -> component Kconfig, applied by net_backend_toe.c.
 *   - network id  -> the wiz_NetInfo below (byte arrays from net_config.h),
 *                    applied by wiznet_net_init() -> wizchip_setnetinfo().
 */

#include <stdio.h>

#include "sdkconfig.h"
#include "wizchip_conf.h"       /* wiz_NetInfo, NETINFO_STATIC */

#include "net_backend.h"
#include "wifi_backend.h"
#include "net_config.h"
#include "multi_socket.h"

/* Network identity - wsm_driver style (wiz_NetInfo). Applied to the WIZnet
 * chip's hardware TCP/IP stack by wiznet_net_init() -> wizchip_setnetinfo(). */
static const wiz_NetInfo g_net_info = {
    .mac = NET_MAC_ADDR,
    .ip  = NET_IP_ADDR,
    .sn  = NET_SUBNET_MASK,
    .gw  = NET_GATEWAY,
    .dns = NET_DNS_ADDR,
#if _WIZCHIP_ > W5500
    .ipmode = NETINFO_STATIC_ALL,
#endif
    .dhcp = NETINFO_STATIC,
};

void app_main(void)
{
    /* Ethernet (WIZnet chip) first: it initializes esp_netif + the default event
     * loop that Wi-Fi then reuses, and applies g_net_info to the chip. */
    wiznet_net_init(&g_net_info);
    wifi_net_init(WIFI_SSID, WIFI_PASS);

    /* Start both servers; each waits for its own link, then opens its listeners.
     * Same call shape - only the label, vtable, port base and readiness
     * predicate differ. Ethernet uses the standard lwIP vtable (--wrap-redirected
     * to the WIZnet chip); Wi-Fi uses its own (__real_lwip_* to bypass the wrap). */
    multi_socket_start("eth",  &multi_socket_lwip_ops, MULTI_SOCKET_PORT_BASE,
                       MULTI_SOCKET_COUNT, wiznet_net_is_up);
    multi_socket_start("wifi", &wifi_multi_socket_ops, WIFI_MULTI_SOCKET_PORT_BASE,
                       MULTI_SOCKET_COUNT, wifi_net_is_up);
}
