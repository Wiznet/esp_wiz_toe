# How to Test W6300 Loopback Example

## Step 1: Prepare software

The following serial terminal program and TCP/UDP test tool are required for the W6300 Loopback example test, download and install from below links.

- [Tera Term][link-tera_term]
- [Hercules][link-hercules]

## Step 2: Prepare hardware

1. Connect the WIZnet W6300 Ethernet chip to the ESP32-S3 board over QSPI, following the pin table in [Step 3](#step-3-setup-w6300-loopback-example).
2. Connect an Ethernet cable from the module's RJ45 port to your PC or network.
3. Connect the ESP32-S3 board to your PC with a USB cable.

![][link-hardware]

## Step 3: Setup W6300 Loopback Example

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

Check the per-socket buffer size. The chip, SPI host, clock, and pins all follow the selected board automatically. In this example, SPI2 of the ESP32-S3 is used at 33 MHz.
![][link-config_wiz_toe]

> This example is **pinned to the ESP32-W6300-Dev-kit** board (`sdkconfig.defaults` sets `CONFIG_WSM_DRIVER_BOARD_W6300_DEVKIT=y`), which is what selects the W6300 chip. Leave the board as it is under `Component config -> WIZnet WSM Driver -> Board`. You can choose between **Single** and **Quad** SPI mode under `Component config -> WIZnet WSM Driver -> W6300 QSPI mode`.

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

Single mode uses the D0/D1 lines as a standard 4-wire SPI bus; Quad mode additionally requires the D2/D3 lines wired and selected in menuconfig.

### Network configuration

Configure the network settings in the `examples/w6300_loopback/main/main.c` file.

```cpp
static const wiz_NetInfo s_net_info = {
    .mac = {0x00, 0x08, 0xDC, 0x12, 0x34, 0x56}, // MAC address
    .ip  = {192, 168, 11, 2},                    // IP address
    .sn  = {255, 255, 255, 0},                   // Subnet Mask
    .gw  = {192, 168, 11, 1},                    // Gateway
    .dns = {8, 8, 8, 8},                         // DNS server
    .ipmode = NETINFO_STATIC_ALL,
    .dhcp   = NETINFO_STATIC,
};
```

### Loopback configuration

This example runs a TCP listener and a UDP listener at the same time. The listen ports are set in `examples/w6300_loopback/main/main.c`.

```cpp
#define EXAMPLE_TCP_LISTEN_PORT 5000
#define EXAMPLE_UDP_LISTEN_PORT 5001
```

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

If flashing succeeds, the device brings up the PHY link and reports the TCP and UDP loopback ports in the terminal.

```
w6300_loopback_example: TCP loopback on port 5000, UDP loopback on port 5001
w6300_loopback_example: PHY link up
```

![][link-run_socket_open]

### TCP loopback

Open Hercules, select the **TCP Client** tab, enter the device IP `192.168.11.2` and port `5000`, then connect. After connecting, send any data; the device echoes the same data back, confirming the TCP loopback works.
![][link-run_tcp]

### UDP loopback

In Hercules, select the **UDP** tab, set the module IP `192.168.11.2` and port `5001` as the destination, and open the UDP socket. Send any data; the device echoes the same data back, confirming the UDP loopback works.
![][link-run_udp]

## Appendix

- **Two listeners at once:** The loopback task services the TCP socket (`EXAMPLE_TCP_SOCKET_NUM 0`) and the UDP socket (`EXAMPLE_UDP_SOCKET_NUM 1`) in the same loop, so you can test both ports without reflashing.
- **W6300 QSPI mode:** Quad mode (4-bit) requires the extra D2/D3 lines wired and selected in `Component config -> WIZnet WSM Driver -> W6300 QSPI mode`. Single mode uses only the D0/D1 lines as a standard 4-wire SPI bus.
- **Link status:** While the Ethernet cable is unplugged the device logs `PHY link down, waiting...` once per second and resumes automatically when the link returns.

<!-- Link -->
[link-tera_term]: https://osdn.net/projects/ttssh2/releases/
[link-hercules]: https://www.hw-group.com/software/hercules-setup-utility

[link-hardware]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/w6300_loopback/hardware.png
[link-config_main]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/w6300_loopback/config_main.png
[link-config_component]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/w6300_loopback/config_component.png
[link-config_wiz_toe]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/w6300_loopback/config_wiz_toe.png

[link-build_log]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/w6300_loopback/build_log.png
[link-run_socket_open]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/w6300_loopback/run_socket_open.png
[link-run_tcp]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/w6300_loopback/run_tcp.png
[link-run_udp]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/w6300_loopback/run_udp.png
