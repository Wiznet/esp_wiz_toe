# How to Test Network Install Example

## Step 1: Prepare software

The following serial terminal program is required for the Network Install example test, download and install from the below link. This example is a bring-up / diagnostic check; verification is done entirely in the serial monitor, so no separate PC test tool is needed.

- [Tera Term][link-tera_term]

## Step 2: Prepare hardware

1. Connect the WIZnet Ethernet chip (W5500 or W6300) to the ESP32-S3 board over SPI, following the pin table in [Step 3](#step-3-setup-network-install-example).
2. Connect an Ethernet cable from the module's RJ45 port to your PC or network.
3. Connect the ESP32-S3 board to your PC with a USB cable.

![][link-hardware]

## Step 3: Setup Network Install Example

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

Configure the network settings in the `examples/network_install/main/main.c` file. The IP printed here is the address you will ping once the link is up.

```cpp
static const wiz_NetInfo g_net_info = {
    .mac = {0x00, 0x08, 0xDC, 0x12, 0x34, 0x56}, // MAC address
    .ip  = {192, 168, 11, 2},                    // IP address
    .sn  = {255, 255, 255, 0},                   // Subnet Mask
    .gw  = {192, 168, 11, 1},                    // Gateway
    .dns = {8, 8, 8, 8},                         // DNS server
};
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

The example polls the internal PHY link state every 500 ms. With the Ethernet cable connected, the link comes up and the negotiated speed/duplex and the IP to ping are printed:

```
Link OK of Internal PHY.
the 100 Mbtis speed of Internal PHY.
The Full-Duplex Mode of the Internal PHY.

Try ping the ip:192.168.11.2.
```

![][link-run_link_ok]

Now verify the link state changes live in the serial monitor. Unplug the Ethernet cable before flashing (or unplug it and reset the board). While the link is down, the device prints a stream of `0` characters as it polls. After about 10 failed polls (~5 s) it gives up and prints:

```
Link failed of Internal PHY.

Please check whether the network cable is loose or disconnected.
```

![][link-run_link_fail]

Plug the cable back in and reset the board: the link now comes up and you see the `Link OK of Internal PHY.` block again. Finally, from your PC, ping the device to confirm basic connectivity:

```bash
ping 192.168.11.2
```

![][link-run_ping]

## Appendix

- **Link poll behavior:** While the cable is disconnected the device prints the raw link status (`0`) on each 500 ms poll. It retries up to 10 times before declaring `Link failed of Internal PHY.`; reconnecting the cable and resetting the board restarts the check.
- **W6300 QSPI mode:** Quad mode (4-bit) requires the extra D2/D3 lines wired and selected in `Component config -> WIZnet WSM Driver -> W6300 QSPI mode`. Single mode uses the same 4-wire wiring as W5500.

<!-- Link -->
[link-tera_term]: https://osdn.net/projects/ttssh2/releases/

[link-hardware]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/network_install/hardware.png
[link-config_main]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/network_install/config_main.png
[link-config_component]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/network_install/config_component.png
[link-config_wiz_toe]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/network_install/config_wiz_toe.png

[link-build_log]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/network_install/build_log.png
[link-run_link_ok]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/network_install/run_link_ok.png
[link-run_link_fail]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/network_install/run_link_fail.png
[link-run_ping]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/network_install/run_ping.png
