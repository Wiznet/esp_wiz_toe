# How to Test UDP Example

> **Verified on both chips.** This example was run on a W6300 (QSPI) and on a W5500 (standard SPI, XIAO ESP32-S3), over Ethernet and Wi-Fi in each case.

## Step 1: Prepare software

The following serial terminal program and UDP test tool are required for the UDP example test, download and install from below links.

- [Tera Term][link-tera_term]
- [Hercules][link-hercules]

## Step 2: Prepare hardware

1. Connect the WIZnet Ethernet chip (W5500 or W6300) to the ESP32-S3 board over SPI, following the pin table in [Step 3](#step-3-setup-udp-example).
2. Connect an Ethernet cable from the module's RJ45 port to your PC or network.
3. Connect the ESP32-S3 board to your PC with a USB cable.

![][link-hardware]

## Step 3: Setup UDP Example

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

Network identity and ports live in `examples/udp/inc/net_config.h`, the same way as
in `examples/loopback`:

```cpp
#define NET_MAC_ADDR          {0x00, 0x08, 0xDC, 0x12, 0x34, 0x56}
#define NET_IP_ADDR           {192, 168, 11, 2}
#define NET_SUBNET_MASK       {255, 255, 255, 0}
#define NET_GATEWAY           {192, 168, 11, 1}
#define NET_DNS_ADDR          {8, 8, 8, 8}

#define WIFI_SSID             ""      /* empty -> Ethernet only */
#define WIFI_PASS             ""

#define UDP_ECHO_PORT             5000    /* Ethernet, server role */
#define UDP_ECHO_CLIENT_PORT      50000   /* Ethernet, client role */
#define WIFI_UDP_ECHO_PORT        5001    /* Wi-Fi, server role    */
#define WIFI_UDP_ECHO_CLIENT_PORT 50001   /* Wi-Fi, client role    */
#define UDP_ECHO_BUF_SIZE         2048
```

`main.c` assembles a `wiz_NetInfo` from these and hands it to `wiznet_net_init()`,
which applies it to the chip with `wizchip_setnetinfo()`.

### Running on Wi-Fi at the same time (optional)

Fill in `WIFI_SSID` and the same echo engine also comes up on a Wi-Fi STA, as a
sibling task at the same level as the Ethernet one:

```cpp
udp_echo_start("eth",  &net_eth_ops,  UDP_ECHO_BIND_PORT,      wiznet_net_is_up);
udp_echo_start("wifi", &net_wifi_ops, WIFI_UDP_ECHO_BIND_PORT, wifi_net_is_up);
```

The two interfaces use different ports because with `SOCKET_WRAP=0` (esp_eth
backend) they share one LwIP stack, where identical ports would clash on bind.

Wait for this line before testing the Wi-Fi side — the socket only opens once DHCP
has assigned an address:

```
I (xxxx) wifi: got IP 192.168.11.7
I (xxxx) udp_echo: [wifi] UDP echo on port 5001
```

Leave `WIFI_SSID` empty when committing; an empty SSID skips Wi-Fi entirely so the
example still builds and runs for everyone else.

### UDP role configuration

Select the role in menuconfig under **UDP Example Configuration -> UDP role**
(`EXAMPLE_UDP_ROLE`):

- **UDP server** — `EXAMPLE_UDP_SERVER` (default): binds `UDP_ECHO_PORT` (5000)
  and echoes every datagram back to its sender.
- **UDP client** — `EXAMPLE_UDP_CLIENT`: binds `UDP_ECHO_CLIENT_PORT` (50000),
  echoes the same way, and additionally logs each received payload.

Both roles are echo responders — they never initiate. That matches the ioLibrary
reference this example is ported from, where `loopback_udpc()` likewise only
opens an ephemeral port and replies; the difference between the two roles is the
local port and the logging.

### Architecture

Same layout as `examples/loopback`:

| Path | Role |
|------|------|
| `inc/net_config.h` | network identity, ports, buffer size |
| `inc/udp_echo.h` | engine API |
| `src/udp_echo.c` | backend-neutral echo engine (BSD sockets via a vtable) |
| `main/main.c` | orchestration only: bring the chip up, start the task |

The engine calls BSD sockets through the component's `net_sock_ops_t` vtable and
is handed `net_eth_ops` — the plain `lwip_*` entry points, which the `wsm_driver`
component redirects to the WIZnet hardware sockets at link time via `-Wl,--wrap`
(`CONFIG_WSM_DRIVER_SOCKET_WRAP`). Running the same engine on Wi-Fi as well is a
second `udp_echo_start()` call with `net_wifi_ops`.

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

If flashing succeeds, the assigned IP and the UDP server start logs appear in the terminal.

```
I (361) wiztoe_net: TOE up: 192.168.11.2 (WIZnet hardware TCP/IP)
I (362) udp_echo: [eth] waiting for link...
I (365) udp_echo: [eth] UDP echo on port 5000
```

![][link-run_socket_open]

Open Hercules, select the **UDP** tab, set the **Module IP** to the device IP `192.168.11.2` with **Port** `5000`, set a **Local Port**, then send a UDP datagram.
![][link-run_hercules]

The device echoes the same datagram back to your PC, confirming the UDP loopback works.
![][link-run_loopback]

## Appendix

- **UDP client role:** Select `EXAMPLE_UDP_CLIENT` in menuconfig. The device then binds port `50000` instead of `5000` and logs each datagram it echoes. Point Hercules at `192.168.11.2:50000` to exercise it.
- **W6300 QSPI mode:** Quad mode (4-bit) requires the extra D2/D3 lines wired and selected in `Component config -> WIZnet WSM Driver -> W6300 QSPI mode`. Single mode uses the same 4-wire wiring as W5500.

<!-- Link -->
[link-tera_term]: https://osdn.net/projects/ttssh2/releases/
[link-hercules]: https://www.hw-group.com/software/hercules-setup-utility

[link-hardware]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/udp/hardware.png
[link-config_main]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/udp/config_main.png
[link-config_component]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/udp/config_component.png
[link-config_wiz_toe]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/udp/config_wiz_toe.png

[link-build_log]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/udp/build_log.png
[link-run_socket_open]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/udp/run_socket_open.png
[link-run_hercules]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/udp/run_hercules.png
[link-run_loopback]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/udp/run_loopback.png
