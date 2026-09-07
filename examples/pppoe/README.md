# How to Test PPPoE Example

## Step 1: Prepare software

The following serial terminal program and a PPPoE server (access concentrator) are required for the PPPoE example test, download and install from below links.

- [Tera Term][link-tera_term]

A PPPoE server / access concentrator is also required (for example an ISP DSL modem, a router with a PPPoE server, or a PC running a PPPoE server). You must have a valid PPPoE account (username and password) to authenticate the session.

## Step 2: Prepare hardware

1. Connect the WIZnet Ethernet chip (W5500) to the ESP32-S3 board over SPI, following the pin table in [Step 3](#step-3-setup-pppoe-example).
2. Connect an Ethernet cable from the module's RJ45 port to your PPPoE server / access concentrator.
3. Connect the ESP32-S3 board to your PC with a USB cable.

![][link-hardware]

## Step 3: Setup PPPoE Example

> **This example is W5500 only.** The vendored PPPoE driver uses W5500 PPPoE registers that do not exist on the W6300, and the example will fail to compile (`#error`) on any other chip. The shipped `sdkconfig.defaults` names the W5500 board explicitly (`CONFIG_WSM_DRIVER_BOARD_W5500_DEVKIT=y`), so it builds as shipped -- just **do not switch to a W6300 board** under `Component config -> WIZnet WSM Driver -> Board`.

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

Keep an **ESP32-W5500** board selected, and check the per-socket buffer size. The chip, SPI host, clock, and pins all follow the selected board automatically. In this example, SPI2 of the ESP32-S3 is used at 33 MHz.
![][link-config_wiz_toe]

**W5500 wiring (standard SPI)**

| W5500 | ESP32-S3 Pin |
|-------|--------------|
| MISO  | 13 |
| MOSI  | 11 |
| SCLK  | 12 |
| CS    | 10 |
| RESET | 9  |
| INT   | 14 |

### Network configuration

Configure the network settings in the `examples/pppoe/main/main.c` file. These are the static values applied to the chip before the PPPoE session; the IP is reassigned by the PPPoE server once authentication succeeds.

```cpp
static const wiz_NetInfo g_net_info = {
    .mac = {0x00, 0x08, 0xDC, 0x12, 0x34, 0x56}, // MAC address
    .ip  = {192, 168, 11, 2},                    // IP address
    .sn  = {255, 255, 255, 0},                   // Subnet Mask
    .gw  = {192, 168, 11, 1},                    // Gateway
    .dns = {8, 8, 8, 8},                         // DNS server
};
```

### PPPoE account configuration

Set your PPPoE username and password in `examples/pppoe/main/main.c`. The example ships with placeholder credentials (`W5100S` / `WIZnet`) — replace them with the real account from your PPPoE server and update the matching length fields.

```cpp
uint8_t pppoe_id[6] = "W5100S";
uint8_t pppoe_id_len = 6;
uint8_t pppoe_pw[6] = "WIZnet";
uint8_t pppoe_pw_len = 6;
```

The session is established by `ppp_start()`, which performs PPPoE discovery (PADI/PADO/PADR/PADS), authenticates via PAP/CHAP using the credentials above, and then negotiates IPCP to obtain an IP address.

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

If flashing succeeds, the example prints its banner and then starts the PPPoE session. The result is verified entirely in the **serial monitor**.

```
wiznet chip PPPOE example.
```

![][link-run_start]

As `ppp_start()` runs, the discovery and authentication stages proceed (PADI -> PADO -> PADR -> PADS), followed by LCP/PAP-or-CHAP authentication and IPCP address negotiation. On success the assigned IP and the post-PPPoE network configuration are printed:

```
<<<< PPPoE Success >>>>
Assigned IP address : x.x.x.x

==================================================
    AFTER PPPoE, Net Configuration Information
==================================================
MAC address  : 0:8:dc:12:34:56
SUBNET MASK  : 255.255.255.255
G/W IP ADDRESS : x.x.x.x
SOURCE IP ADDRESS : x.x.x.x
```

![][link-run_success]

If the session cannot be established (wrong credentials, no PPPoE server reachable, or the retry count is exceeded), the example prints a failure line instead:

```
<<<< PPPoE Failed >>>>
```

![][link-run_failed]

## Appendix

- **Credentials must be real:** the built-in `W5100S` / `WIZnet` values are placeholders. PAP/CHAP authentication will fail unless `pppoe_id` / `pppoe_pw` (and their length fields) match an account configured on your PPPoE server.
- **Retry behavior:** `ppp_start()` is retried in a loop until it succeeds or `pppoe_retry_count` exceeds `PPP_MAX_RETRY_COUNT`, after which the example reports `PPPoE Failed`.
- **W5500 only:** porting to W6300 would require rewriting the PPPoE register access in the vendored `PPPoE.c` (W6300 uses a different register map: PSIDR/PHAR/NETMR2). Do not select a W6300 board for this example.

<!-- Link -->
[link-tera_term]: https://osdn.net/projects/ttssh2/releases/

[link-hardware]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/pppoe/hardware.png
[link-config_main]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/pppoe/config_main.png
[link-config_component]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/pppoe/config_component.png
[link-config_wiz_toe]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/pppoe/config_wiz_toe.png

[link-build_log]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/pppoe/build_log.png
[link-run_start]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/pppoe/run_start.png
[link-run_success]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/pppoe/run_success.png
[link-run_failed]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/pppoe/run_failed.png
