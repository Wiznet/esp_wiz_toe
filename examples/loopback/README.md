# How to Test Loopback Example

## Step 1: Prepare software

The following serial terminal program and TCP/UDP test tool are required for the Loopback example test, download and install from below links.

- [Tera Term][link-tera_term]
- [Hercules][link-hercules]

## Step 2: Prepare hardware

1. Connect the WIZnet Ethernet chip (W5500 or W6300) to the ESP32-S3 board over SPI, following the pin table in [Step 3](#step-3-setup-loopback-example).
2. Connect an Ethernet cable from the module's RJ45 port to your PC or network.
3. Connect the ESP32-S3 board to your PC with a USB cable.

![][link-hardware]

## Step 3: Setup Loopback Example

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

All example settings live in `examples/loopback/inc/net_config.h`; `main.c` assembles them into the `wiz_NetInfo` it hands to `wiznet_net_init()`, which applies it to the chip with `wizchip_setnetinfo()`.

```cpp
#define NET_MAC_ADDR          {0x00, 0x08, 0xDC, 0x12, 0x34, 0x56}  /* WIZnet OUI */
#define NET_IP_ADDR           {192, 168, 11, 2}
#define NET_SUBNET_MASK       {255, 255, 255, 0}
#define NET_GATEWAY           {192, 168, 11, 1}
#define NET_DNS_ADDR          {8, 8, 8, 8}
```

### Wi-Fi configuration

The example runs the same echo server on the Wi-Fi STA alongside Ethernet, so fill in your AP credentials in the same file:

```cpp
#define WIFI_SSID             "your-ssid"
#define WIFI_PASS             "your-password"
```

### Loopback configuration

Ports, the TCP-client destination, and the buffer size are in `net_config.h` as well. Each interface listens on its own port so that the shared LwIP stack has no bind clash.

```cpp
#define LOOPBACK_PORT         5000              /* Ethernet echo port */
#define WIFI_LOOPBACK_PORT    5001              /* Wi-Fi echo port    */
#define LOOPBACK_TARGET_IP    "192.168.11.100"  /* TCP-client mode destination */
#define LOOPBACK_TARGET_PORT  5000
#define LOOPBACK_BUF_SIZE     2048
```

The loopback variant itself is the compile-time switch `LOOPBACK_MODE` in `examples/loopback/src/loopback.c`. It defaults to the TCP server and applies to **both** interfaces.

```cpp
#define LOOPBACK_TCP_SERVER   0
#define LOOPBACK_TCP_CLIENT   1
#define LOOPBACK_UDP          2

#ifndef LOOPBACK_MODE
#define LOOPBACK_MODE         LOOPBACK_TCP_SERVER
#endif
```

Edit that default, or override it from the build without touching the source by adding `-DLOOPBACK_MODE=1` (TCP client) or `-DLOOPBACK_MODE=2` (UDP) to the `target_compile_options()` in `examples/loopback/main/CMakeLists.txt`.

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

If flashing succeeds, the assigned IP and the TCP server socket open logs appear in the terminal. Every line from the echo engine is prefixed with the interface label (`eth` or `wifi`).

```
I (522) wiztoe_net: TOE up: 192.168.11.2 (WIZnet hardware TCP/IP)
I (525) loopback: [eth] waiting for link...
I (527) loopback: [eth] loopback: TCP SERVER on port 5000
I (528) loopback: [eth] TCP server listening on port 5000
```

With Wi-Fi configured, the second server appears on port 5001 once the STA has an address:

```
I (xxxxx) loopback: [wifi] TCP server listening on port 5001
```

![][link-run_socket_open]

Open Hercules, select the **TCP Client** tab, enter the device IP `192.168.11.2` and port `5000`, then connect.
![][link-run_hercules]

After connecting, send any data from Hercules. The device echoes the same data back, confirming the loopback works.
![][link-run_loopback]

## Appendix

- **TCP client / UDP variants:** Set `LOOPBACK_MODE` to `1` or `2` to test the other loopback modes. In TCP client mode both interfaces connect out to a PC TCP server at `LOOPBACK_TARGET_IP:LOOPBACK_TARGET_PORT` (`192.168.11.100:5000`) and echo whatever it sends; in UDP mode open a Hercules UDP socket to the device IP on port `5000` (Ethernet) or `5001` (Wi-Fi).
- **Two interfaces, one engine:** `main.c` calls `loopback_start()` twice with identical arguments except the label, socket vtable, port and readiness predicate. Ethernet uses `loopback_lwip_ops` (plain `lwip_*`, which `-Wl,--wrap` redirects to the chip's hardware sockets when `CONFIG_WSM_DRIVER_SOCKET_WRAP` is on); Wi-Fi uses `wifi_loopback_ops`, which binds `__real_lwip_*` so it always reaches the real software LwIP. The echo logic in `src/loopback.c` contains no `#if` for the backend.
- **W6300 QSPI mode:** Quad mode (4-bit) requires the extra D2/D3 lines wired and selected in `Component config -> WIZnet WSM Driver -> W6300 QSPI mode`. Single mode uses the same 4-wire wiring as W5500.

<!-- Link -->
[link-tera_term]: https://osdn.net/projects/ttssh2/releases/
[link-hercules]: https://www.hw-group.com/software/hercules-setup-utility

[link-hardware]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/loopback/hardware.png
[link-config_main]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/loopback/config_main.png
[link-config_component]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/loopback/config_component.png
[link-config_wiz_toe]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/loopback/config_wiz_toe.png

[link-build_log]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/loopback/build_log.png
[link-run_socket_open]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/loopback/run_socket_open.png
[link-run_hercules]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/loopback/run_hercules.png
[link-run_loopback]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/loopback/run_loopback.png
