# How to Test TCP Server Multi Socket Example

## Step 1: Prepare software

The following serial terminal program and TCP/UDP test tool are required for the TCP Server Multi Socket example test, download and install from below links.

- [Tera Term][link-tera_term]
- [Hercules][link-hercules]

## Step 2: Prepare hardware

1. Connect the WIZnet Ethernet chip (W5500 or W6300) to the ESP32-S3 board over SPI, following the pin table in [Step 3](#step-3-setup-tcp-server-multi-socket-example).
2. Connect an Ethernet cable from the module's RJ45 port to your PC or network.
3. Connect the ESP32-S3 board to your PC with a USB cable.

![][link-hardware]

## Step 3: Setup TCP Server Multi Socket Example

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

Choose the WIZnet chip, and check the per-socket buffer size. SPI host, clock, and pins follow the selected chip automatically. In this example, SPI2 of the ESP32-S3 is used at 33 MHz.
![][link-config_wiz_toe]

> This example ships with **W6300** selected by default (`sdkconfig.defaults`). Switch to W5500 under `Component config -> WIZnet WSM Driver -> WIZnet chip` if needed.

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

All example settings live in `examples/tcp_server_multi_socket/inc/net_config.h`. The SPI wiring is **not** here — it comes from the component Kconfig shown above.

```cpp
#define NET_MAC_ADDR    {0x00, 0x08, 0xDC, 0x12, 0x34, 0x56}  // MAC address
#define NET_IP_ADDR     {192, 168, 11, 2}                     // IP address
#define NET_SUBNET_MASK {255, 255, 255, 0}                    // Subnet Mask
#define NET_GATEWAY     {192, 168, 11, 1}                     // Gateway
#define NET_DNS_ADDR    {8, 8, 8, 8}                          // DNS server
```

### Wi-Fi configuration

This example runs the **same echo server on the WIZnet chip and on Wi-Fi at the same time**, so fill in your AP credentials in the same file:

```cpp
#define WIFI_SSID "your-ssid"
#define WIFI_PASS "your-password"
```

Leaving the placeholders in place is harmless: the Wi-Fi side simply keeps retrying the connection and the Ethernet side is unaffected.

### Server port configuration

Each listener binds its own TCP port, `PORT_BASE + index`. The chip can in fact demultiplex several hardware sockets listening on one port by 4-tuple, so one port per listener is a presentation choice rather than a hardware limit — it keeps the log readable and matches the original WIZnet-PICO-C example.

```cpp
#define MULTI_SOCKET_PORT_BASE      5000   // Ethernet: 5000..5003
#define WIFI_MULTI_SOCKET_PORT_BASE 5100   // Wi-Fi:    5100..5107
#if CONFIG_WSM_DRIVER_SOCKET_WRAP
#define MULTI_SOCKET_COUNT          4      // TOE: listener + connection, x4 = 8
#else
#define MULTI_SOCKET_COUNT          8      // software LwIP: no chip budget
#endif
```

### How many listeners the TOE can afford

`accept()` has BSD semantics on the hardware sockets: it hands the established socket to the accepted connection and relocates the listener onto a free one. A listener that is serving a client therefore occupies **two** of the chip's eight sockets, and two of the driver's eight descriptors — the descriptors being the tighter limit, because `accept()` cannot allocate one for the new connection if every descriptor is already a listener.

So eight listeners cannot accept anything at all: all eight descriptors are spent before the first client arrives and every `accept()` returns `EWOULDBLOCK` forever. **Four** is the largest count where every listener can serve a client at the same time (4 listeners + 4 connections = 8).

This budget is TOE-only. With the esp_eth backend (`SOCKET_WRAP=0`) and on Wi-Fi these are ordinary LwIP sockets, so 8 still applies there. Each listener costs one task on **both** interfaces, so lower it if you are short on RAM. The Wi-Fi side needs one lwIP socket per listener plus one per accepted connection, which is why `sdkconfig.defaults` raises `CONFIG_LWIP_MAX_SOCKETS` to 16.

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

If flashing succeeds, the assigned IP and the multi-socket server startup log appear in the terminal. Both interfaces come up independently, so the Wi-Fi lines may appear after the Ethernet ones.

```
wiztoe_net: TOE up: 192.168.11.2 (WIZnet hardware TCP/IP)
multi_socket: [eth] waiting for link...
multi_socket: [eth#0] listening on port 5000
multi_socket: [eth#1] listening on port 5001
multi_socket: [eth] 8/8 listeners up on ports 5000-5007
wifi: got IP 192.168.0.42
multi_socket: [wifi] 8/8 listeners up on ports 5100-5107
```

![][link-run_socket_open]

Open Hercules, select the **TCP Client** tab, enter the device IP `192.168.11.2` and port `5000`, then connect. Open several Hercules windows (or several TCP Client tabs) and connect each one to a **different port** (`5000`, `5001`, `5002`, …) at the same IP to use multiple sockets at once.
![][link-run_hercules]

To test the Wi-Fi side as well, use the IP the `wifi: got IP` line reported and ports `5100`, `5101`, … instead.

As each client connects, the device prints the interface, listener index and peer address, for example:

```
multi_socket: [eth#0] connected - 192.168.11.100:50312
multi_socket: [eth#1] connected - 192.168.11.101:50315
```

Send data from each connected Hercules window. The device echoes the same data back to the sender independently, and logs the listener, port and message:

```
multi_socket: [eth#0] port 5000 message:hello
multi_socket: [eth#1] port 5001 message:world
```

![][link-run_loopback]

Confirm that every connection echoes its own data back without interfering with the others.

## Appendix

- **Number of simultaneous clients:** On Ethernet the device serves as many connections as the chip has hardware sockets (8). Each listener runs in its own task and blocks in `accept()`/`recv()`, so the connections do not interfere with each other. The Wi-Fi side serves the same number over the ESP32-S3's own LwIP stack.
- **How one server drives two interfaces:** the echo logic in `src/multi_socket.c` calls BSD sockets through a vtable. For Ethernet that vtable is the plain `lwip_*` set, which the `wsm_driver` component redirects to the chip's hardware sockets at link time (`-Wl,--wrap`, `CONFIG_WSM_DRIVER_SOCKET_WRAP`); for Wi-Fi it is the un-wrapped `__real_lwip_*` set in `src/wifi_multi_socket.c`, which reaches the software LwIP stack. The application code is identical for both.
- **Ethernet only:** remove the `multi_socket_start("wifi", ...)` call (and `wifi_net_init`) from `main/main.c`.
- **W6300 QSPI mode:** Quad mode (4-bit) requires the extra D2/D3 lines wired and selected in `Component config -> WIZnet WSM Driver -> W6300 QSPI mode`. Single mode uses the same 4-wire wiring as W5500.

<!-- Link -->
[link-tera_term]: https://osdn.net/projects/ttssh2/releases/
[link-hercules]: https://www.hw-group.com/software/hercules-setup-utility

[link-hardware]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/tcp_server_multi_socket/hardware.png
[link-config_main]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/tcp_server_multi_socket/config_main.png
[link-config_component]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/tcp_server_multi_socket/config_component.png
[link-config_wiz_toe]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/tcp_server_multi_socket/config_wiz_toe.png

[link-build_log]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/tcp_server_multi_socket/build_log.png
[link-run_socket_open]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/tcp_server_multi_socket/run_socket_open.png
[link-run_hercules]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/tcp_server_multi_socket/run_hercules.png
[link-run_loopback]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/tcp_server_multi_socket/run_loopback.png
