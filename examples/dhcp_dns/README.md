# How to Test DHCP & DNS Example

## Step 1: Prepare software

The following serial terminal program and DHCP-enabled network are required for the DHCP & DNS example test, download and install from below links.

- [Tera Term][link-tera_term]

## Step 2: Prepare hardware

1. Connect the WIZnet Ethernet chip (W5500 or W6300) to the ESP32-S3 board over SPI, following the pin table in [Step 3](#step-3-setup-dhcp--dns-example).
2. Connect an Ethernet cable from the module's RJ45 port to a network with a DHCP server (e.g. your router).
3. Connect the ESP32-S3 board to your PC with a USB cable.

![][link-hardware]

## Step 3: Setup DHCP & DNS Example

### Chip and SPI configuration

Set the target and open menuconfig:

```bash
idf.py set-target esp32s3
idf.py menuconfig
```

Select **Component config**.
![][link-config_main]

Select **WIZnet WSM Driver** under Component config.
![][link-config_component]

Choose the board, and check the per-socket buffer size. The chip, SPI host, clock, and pins all follow the selected board automatically. In this example, SPI2 of the ESP32-S3 is used at 33 MHz.
![][link-config_wiz_toe]

> This example ships with the **ESP32-W5500-Dev-kit** board, which is the component Kconfig default -- `sdkconfig.defaults` no longer names a chip. Pick a different board under `Component config -> WIZnet WSM Driver -> Board`: the board fixes the chip and the SPI pins together, and the chip is separately selectable only on `Board -> Custom`.

**W5500 wiring (standard SPI)**

| W5500 | ESP32-S3 Pin |
|-------|--------------|
| MISO  | 13 |
| MOSI  | 11 |
| SCLK  | 12 |
| CS    | 10 |
| RESET | 9  |
| INT   | 14 |

**W6300 wiring (QSPI)**

| W6300   | ESP32-S3 Pin |
|---------|--------------|
| D0 (MOSI) | 11 |
| D1 (MISO) | 13 |
| D2 (IO2)  | 14 *(Quad mode only)* |
| D3 (IO3)  | 9  *(Quad mode only)* |
| SCLK    | 12 |
| CS      | 10 |
| RESET   | 21 |
| INT     | 8  |

### Network configuration

All example settings live in `examples/dhcp_dns/inc/net_config.h`; `main.c` assembles them into the `wiz_NetInfo` it hands to `wiznet_net_init()`. The `.dhcp` field is set to `NETINFO_DHCP`, so the `ip`/`sn`/`gw`/`dns` values below are only what the chip carries until the first lease arrives — the actual address comes from the DHCP server at runtime. The MAC is never leased and is always taken from here.

```cpp
#define NET_MAC_ADDR          {0x00, 0x08, 0xDC, 0x12, 0x34, 0x56}  /* WIZnet OUI */
#define NET_IP_ADDR           {192, 168, 11, 2}
#define NET_SUBNET_MASK       {255, 255, 255, 0}
#define NET_GATEWAY           {192, 168, 11, 1}
#define NET_DNS_ADDR          {8, 8, 8, 8}
```

### Wi-Fi configuration

The example runs the same DHCP-then-DNS sequence on the Wi-Fi STA interface alongside Ethernet, so fill in your AP credentials in the same file:

```cpp
#define WIFI_SSID             "your-ssid"
#define WIFI_PASS             "your-password"
```

### DHCP & DNS configuration

The hostname to resolve, the retry budgets and the retransmission schedule are in `net_config.h` as well. No hardware socket is pinned here: `dhcp_dns.c` calls `ops->sock->socket()`, which under the TOE `--wrap` allocates a free hardware socket the same way the loopback example does.

```cpp
#define DHCP_DNS_DOMAIN       "www.wiznet.io"   /* hostname resolved on each interface */
#define DHCP_DNS_RETRY_COUNT  5
#define DNS_RETRY_COUNT       5

#define DHCP_XMIT_TRIES       4                 /* transmits before a round fails */
#define DHCP_XMIT_INTERVAL_MS 2000              /* base retransmit spacing (x1, x2, x3, ...) */
#define DHCP_RECV_TIMEOUT_MS  500
#define DNS_RECV_TIMEOUT_MS   3000
```

### Source layout

| File | Role |
|------|------|
| `main/main.c` | brings up both stacks, then starts one task per interface |
| `inc/net_config.h` | network identity, Wi-Fi credentials, DHCP/DNS settings |
| `src/dhcp_dns.c` | the DHCP + DNS protocol over a BSD socket vtable, and the engine |
| `src/eth_dhcp_dns.c` | Ethernet MAC/lease hooks — `wizchip_*` (TOE) or esp_netif (ETH) |
| `src/netif_dhcp_dns.c` | esp_netif MAC/lease hooks + the Wi-Fi socket vtable |

## Step 4: Build

After completing the setup, build the project.

```bash
idf.py build
```

![][link-build_log]

## Step 5: Upload and Run

Flash the firmware and open the serial monitor. Replace the port with your board's serial port.

```bash
idf.py -p COMx flash monitor
```

On Linux/macOS:

```bash
idf.py -p /dev/ttyUSB0 flash monitor
```

If flashing succeeds, each interface starts its DHCP client and prints the leased network information in the terminal. Every line is prefixed with the interface label (`eth` or `wifi`), and the two tasks run independently, so their lines interleave.

```
I (1234) dhcp_dns: [eth] DHCP client running
I (5678) dhcp_dns: [eth] DHCP success
I (5679) dhcp_dns: [eth] ip  : 192.168.11.100
I (5680) dhcp_dns: [eth] sn  : 255.255.255.0
I (5681) dhcp_dns: [eth] gw  : 192.168.11.1
I (5682) dhcp_dns: [eth] dns : 8.8.8.8
I (5683) dhcp_dns: [eth] DHCP leased time : 7200 seconds
```

The leased IP (`ip`), subnet, gateway, and DNS values come from your DHCP server, so they will differ from the example above.

![][link-run_dhcp]

After the IP is leased, each interface resolves the target hostname over DNS and prints the result.

```
I (6789) dhcp_dns: [eth] DNS success
I (6790) dhcp_dns: [eth] target domain : www.wiznet.io
I (6791) dhcp_dns: [eth] IP of target domain : 211.244.224.36
```

Seeing the `target domain` and its resolved IP confirms that both DHCP and DNS work. The resolved IP depends on current DNS records and may differ from the value above.

![][link-run_dns]

## Appendix

- **The backend switch is a link-time decision.** `src/dhcp_dns.c` speaks RFC 2131 (DHCP) and RFC 1035 (DNS) directly over a BSD socket vtable, exactly like `examples/loopback`:

  ```c
  ops->sock->sendto(fd, msg, len, 0, &broadcast, sizeof(broadcast));
  ```

  With `CONFIG_WSM_DRIVER_BACKEND_TOE` those `lwip_*` symbols are redirected by `-Wl,--wrap` to `__wrap_lwip_sendto` → the chip's hardware sockets; with `CONFIG_WSM_DRIVER_BACKEND_ETH` they are the plain software-LwIP `lwip_sendto`. **One source, no `#if`** — the linker picks. The ioLibrary `DHCP_run()`/`DNS_run()` clients are not used and do not end up in the binary; they could not serve both backends because they bypass `lwip_*` entirely and talk to the chip's registers, leaving `--wrap` nothing to intercept.
- **Two interfaces, one engine:** `main.c` calls `dhcp_dns_start()` twice with identical arguments except the label, vtable and readiness predicate. Both run the same protocol code; the vtable only supplies the socket symbols plus the two operations a socket cannot perform — reading the interface MAC and installing a lease.
- **What stays per-backend, and why:** `dhcp_dns_ops_t` has just two hooks. `prepare()` reports the chaddr and the LwIP netif name, and stops whatever DHCP client the stack runs on its own. `apply_lease()` installs the result — `wizchip_setnetinfo()` on TOE, `esp_netif_set_ip_info()` on LwIP. There is no socket call for either.
- **Sharing UDP port 68:** on the ETH backend both interfaces live on one LwIP stack, so both DHCP sockets bind port 68 with `SO_REUSEADDR` and each sees the other's broadcasts. Every reply is checked against the transaction ID *and* the `chaddr`, so the two clients never consume each other's leases. `SO_BINDTODEVICE` additionally pins each socket to its own netif, so a `255.255.255.255` send leaves through the right interface instead of `netif_default`. The TOE `--wrap` accepts that option as a no-op — the chip *is* the interface.
- **DHCP/DNS retry:** one round is `DHCP_XMIT_TRIES` transmits with a linear backoff; when a round expires the engine logs `DHCP timeout occurred and retry N` and counts it against `DHCP_DNS_RETRY_COUNT`. After the last retry that interface's task logs `DHCP failed` / `DNS failed` and exits, leaving the other interface running. Check that the Ethernet cable is connected to a DHCP-enabled network.
- **Renewal** restarts from `DHCPDISCOVER` at T1 (half the lease) rather than unicasting a `DHCPREQUEST` in RENEWING state — servers hand back the same address. **Duplicate-address detection is not implemented**: RFC 2131 specifies an ARP probe, and there is no portable way to send one through a BSD socket, so `DHCP_DNS_CONFLICT` is never reported.
- **Wi-Fi lease timing:** `wifi_net_is_up()` only returns true after `IP_EVENT_STA_GOT_IP`, so Wi-Fi already holds an esp_netif lease when the task starts. `prepare()` then stops that client and this example leases the address again itself, so the Wi-Fi side takes a few seconds longer than it used to.
- **ETH backend:** with `CONFIG_WSM_DRIVER_BACKEND_ETH` the chip runs as an esp_eth MACRAW MAC and Ethernet uses the same esp_netif hooks Wi-Fi does — only the netif key differs. `main.c` and the protocol code are unchanged.
- **W6300 QSPI mode:** Quad mode (4-bit) requires the extra D2/D3 lines wired and selected in `Component config -> WIZnet WSM Driver -> W6300 QSPI mode`. Single mode uses the same 4-wire wiring as W5500.

<!-- Link -->
[link-tera_term]: https://osdn.net/projects/ttssh2/releases/

[link-hardware]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/dhcp_dns/hardware.png
[link-config_main]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/dhcp_dns/config_main.png
[link-config_component]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/dhcp_dns/config_component.png
[link-config_wiz_toe]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/dhcp_dns/config_wiz_toe.png

[link-build_log]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/dhcp_dns/build_log.png
[link-run_dhcp]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/dhcp_dns/run_dhcp.png
[link-run_dns]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/dhcp_dns/run_dns.png
