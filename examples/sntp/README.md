# How to Test SNTP Example

## Step 1: Prepare software

The following serial terminal program is required for the SNTP example test, download and install from the below link.

- [Tera Term][link-tera_term]

## Step 2: Prepare hardware

1. Connect the WIZnet Ethernet chip (W5500 or W6300) to the ESP32-S3 board over SPI, following the pin table in [Step 3](#step-3-setup-sntp-example).
2. Connect an Ethernet cable from the module's RJ45 port to your network so the device can reach the internet (gateway and DNS must be valid).
3. Connect the ESP32-S3 board to your PC with a USB cable.

![][link-hardware]

## Step 3: Setup SNTP Example

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

All example settings live in `examples/sntp/inc/net_config.h`. The SPI wiring is **not** here — it comes from the component Kconfig shown above. The gateway must point to a working internet route so the device can reach the SNTP server.

```cpp
#define NET_MAC_ADDR    {0x00, 0x08, 0xDC, 0x12, 0x34, 0x56}  // MAC address
#define NET_IP_ADDR     {192, 168, 11, 2}                     // IP address
#define NET_SUBNET_MASK {255, 255, 255, 0}                    // Subnet Mask
#define NET_GATEWAY     {192, 168, 11, 1}                     // Gateway
#define NET_DNS_ADDR    {8, 8, 8, 8}                          // DNS server
```

### Wi-Fi configuration

This example queries the time **over the WIZnet chip and over Wi-Fi at the same time**, so fill in your AP credentials in the same file:

```cpp
#define WIFI_SSID "your-ssid"
#define WIFI_PASS "your-password"
```

Leaving the placeholders in place is harmless: the Wi-Fi query simply fails after its retries and the Ethernet one is unaffected.

### SNTP configuration

The server, local ports and timezone are in the same `net_config.h`. By default the device queries `time.google.com` (`216.239.35.0`) with the offset set to Korea (UTC+9).

```cpp
#define SNTP_SERVER_IP        "216.239.35.0"   // time.google.com
#define SNTP_SERVER_PORT      123
#define SNTP_LOCAL_PORT       5000             // Ethernet local UDP port
#define WIFI_SNTP_LOCAL_PORT  5001             // Wi-Fi local UDP port

#define SNTP_TIMEOUT_MS       (1000 * 10)      // per attempt
#define SNTP_RETRY_COUNT      3

#define SNTP_TZ_OFFSET_MIN    (9 * 60)         // UTC+9 (Korea)
```

The server is given as an IPv4 literal — this client does no name resolution, so DNS is not a second thing that can fail during bring-up. `SNTP_TZ_OFFSET_MIN` is plain minutes: use `(-5 * 60)` for UTC-5, `(5 * 60 + 30)` for UTC+5:30, and so on. (The original example used ioLibrary's `TIMEZONE` index code instead; `40` there meant UTC+9.)

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

If flashing succeeds, the assigned IP appears in the terminal.

```
wiztoe_net: TOE up: 192.168.11.2 (WIZnet hardware TCP/IP)
```

![][link-run_socket_open]

Each interface then queries the server and prints the current date and time, with `SNTP_TZ_OFFSET_MIN` already applied. The two lines should agree.

```
sntp: [eth] querying 216.239.35.0 (attempt 1/3)
sntp: [eth] 2026-06-23 14:05:30 (UTC+9:00)
sntp: [wifi] querying 216.239.35.0 (attempt 1/3)
sntp: [wifi] 2026-06-23 14:05:31 (UTC+9:00)
```

![][link-run_time]

No separate PC tool is needed — the result is verified entirely in the serial monitor.

## Appendix

- **SNTP failed:** If `SNTP failed after 3 attempts` is printed, the device could not reach the server within `SNTP_TIMEOUT_MS` per attempt. Check the cable and that `NET_GATEWAY` provides a valid internet route (for the `[wifi]` line, that your AP does).
- **Timezone:** `SNTP_TZ_OFFSET_MIN` is a plain minute offset applied to the UTC the server returns. Adjust it for your region.
- **How one client drives two interfaces:** the query in `src/sntp_client.c` calls BSD sockets through a vtable. For Ethernet that vtable is the plain `lwip_*` set, which the `wsm_driver` component redirects to the chip's hardware sockets at link time (`-Wl,--wrap`, `CONFIG_WSM_DRIVER_SOCKET_WRAP`); for Wi-Fi it is the un-wrapped `__real_lwip_*` set in `src/wifi_sntp.c`, which reaches the software LwIP stack. The application code is identical for both.
- **Why not ioLibrary's `SNTP_run()`:** that implementation drives a hardware socket number directly, so it never passes through the BSD socket layer the wrap intercepts and cannot run on Wi-Fi at all. The NTP exchange is one 48-byte datagram each way, so this example does it over plain BSD UDP instead. It also keeps ioLibrary's `sntp.h` out of the include path, where it collides with lwIP's header of the same name.
- **Ethernet only:** remove the `sntp_client_start("wifi", ...)` call (and `wifi_net_init`) from `main/main.c`.
- **W6300 QSPI mode:** Quad mode (4-bit) requires the extra D2/D3 lines wired and selected in `Component config -> WIZnet WSM Driver -> W6300 QSPI mode`. Single mode uses the same 4-wire wiring as W5500.

<!-- Link -->
[link-tera_term]: https://osdn.net/projects/ttssh2/releases/

[link-hardware]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/sntp/hardware.png
[link-config_main]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/sntp/config_main.png
[link-config_component]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/sntp/config_component.png
[link-config_wiz_toe]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/sntp/config_wiz_toe.png

[link-build_log]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/sntp/build_log.png
[link-run_socket_open]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/sntp/run_socket_open.png
[link-run_time]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/sntp/run_time.png
