# wsm_driver

ESP-IDF component that provides a W5500 SPI port layer for direct WIZnet ioLibrary usage.

## Overview

This component is intentionally minimal.
It only handles:

- SPI transport setup for W5500
- ioLibrary callback registration (`reg_wizchip_*_cbfunc`)
- Hardware reset pin control
- PHY link state query

Application code uses ioLibrary APIs directly (`wizchip_init`, `socket`, `connect`, `send`, `recv`, `sendto`, `recvfrom`, DHCP/DNS APIs).

## Requirements

- **ESP-IDF v6.0 or later.** This is a hard minimum, not a recommendation — the
  component declares `idf >= 6.0.1` in `idf_component.yml` and the public header
  fails the build with an `#error` on anything older. Reasons:
  - the esp_eth MAC callbacks used by the ETH backend (`add_mac_filter`,
    `rm_mac_filter`, `set_all_multicast`) are part of `esp_eth_mac_t` from v5.5 on;
  - the TLS examples target mbedTLS 4.0, which ships with v6.0 and removed the
    static-RSA ciphersuites, `mbedtls_ssl_conf_rng()` and the RNG arguments of
    `mbedtls_pk_parse_key()` used by older code.

  Building on v5.x is not supported and will not be patched around.

## Supported ESP-IDF targets

- esp32s3

## Supported WIZnet TOE chips

- W5500 (standard SPI)
- W6300 (QSPI: single 1-bit or quad 4-bit mode)

Other WIZnet chips may be added later.

### W6300 notes

W6300 uses QSPI framing (opcode + 16-bit address + dummy clocks + data) instead
of the W5500 SPI byte/burst interface. The port layer drives it with the
ESP32-S3 SPI master in half-duplex mode.

- **Single mode** (default): uses the same 4-wire wiring as W5500
  (MOSI=IO0, MISO=IO1, SCLK, CS).
- **Quad mode**: requires two extra data lines (IO2/IO3) wired to the chip and
  selected via `Component config -> WIZnet WSM Driver -> W6300 QSPI mode`.

Select a W6300 board in menuconfig: `Component config -> WIZnet WSM Driver -> Board -> ESP32-W6300-Dev-kit` (or `ESP32-W6300-SoM`). The board is what picks the chip; the chip itself is only separately selectable on `Board -> Custom`.

## Network backend

`Component config -> WIZnet WSM Driver -> Network backend` picks who owns the
TCP/IP stack. Both options work with either chip.

- **TOE (hardware TCP/IP)** — the chip runs the stack (ioLibrary). Apps either
  call the ioLibrary socket API directly, or enable
  `Route BSD sockets to the WIZnet TOE hardware sockets (--wrap)` to drive the
  chip's 8 hardware sockets through standard BSD socket calls. IPv4 TCP/UDP only;
  no `select`/`poll`.
- **esp_eth MACRAW + software LwIP** — the chip is a plain Ethernet MAC
  (MACRAW on hardware socket 0) and the ESP32-S3's LwIP owns the stack, so the
  full socket API is available (`select`/`poll`, non-blocking, TLS, IPv6, no
  8-socket limit) at the cost of moving every packet across the SPI/QSPI bus.

Each chip has its own esp_eth MAC/PHY pair, selected automatically by the chip
choice:

| Chip  | MAC / PHY                                | Transport                     |
|-------|------------------------------------------|-------------------------------|
| W5500 | `esp_eth_mac_w5500.c` / `esp_eth_phy_w5500.c` | full-duplex SPI, VDM frame |
| W6300 | `esp_eth_mac_w6300.c` / `esp_eth_phy_w6300.c` | half-duplex QSPI, single or quad |

The W6300 pair is specific to this component. Beyond the different frame format
it also has to unlock the chip's `CHPLCKR`/`NETLCKR`/`PHYLCKR` register groups
after every reset (the W6300 boots with them locked, so `SYCR0`, `SHAR` and
`PHYCR0/1` are otherwise unwritable) and it uses `PHYSR` + `PHYCR0/1` instead of
the W5500's single `PHYCFGR`, whose speed/duplex bits have the opposite polarity.

## Public API

Header: `include/wsm_driver.h`

- `wsm_driver_spi_init`
- `wsm_driver_spi_deinit`
- `wsm_driver_spi_register_iolib_callbacks`
- `wsm_driver_spi_reset`
- `wsm_driver_spi_wizchip_check`
- `wsm_driver_spi_link_is_up`

## Quick start (clone & build)

The repository root is a component; the buildable projects are the examples.

```bash
git clone --recursive https://github.com/Wiznet/wsm_driver.git
cd wsm_driver
idf.py -C examples/loopback build        # -C selects the project directory
```

For VSCode, open `wsm_driver.code-workspace` (File -> Open Workspace from
File). The workspace registers each example as a folder, so the ESP-IDF
extension works as usual — run `ESP-IDF: Pick a Workspace Folder`, choose an
example, then use the normal build/flash/monitor buttons.

## Usage flow

1. Fill `wsm_driver_spi_config_t`
2. Call `wsm_driver_spi_init()`
3. Call `wsm_driver_spi_register_iolib_callbacks()`
4. Call `wsm_driver_spi_reset()`
5. Call ioLibrary init (`wizchip_init`) and network setup (`wizchip_setnetinfo`)
6. Use ioLibrary socket APIs directly

## ioLibrary dependency

`ioLibrary_Driver` is expected under `third_party/ioLibrary_Driver`.

```bash
git submodule add https://github.com/Wiznet/ioLibrary_Driver.git third_party/ioLibrary_Driver
git submodule update --init --recursive
```

If that folder is missing, the component cannot provide ioLibrary callback binding.

## Examples

Available examples (ported from [WIZnet-PICO-C](https://github.com/WIZnet-ioNIC/WIZnet-PICO-C);
each works with W5500 or W6300 — switch the chip in menuconfig — except
`pppoe`, which is W5500-only):

- `examples/loopback` — TCP server loopback (TCP client / UDP via defines)
- `examples/tcp_server_multi_socket` — same port served on every hardware socket
- `examples/udp` — UDP echo server or client (menuconfig choice)
- `examples/udp_multicast` — UDP multicast receiver (224.0.0.5:30000)
- `examples/dhcp_dns` — DHCP address lease + DNS lookup
- `examples/sntp` — network time from time.google.com
- `examples/tftp` — TFTP client file read
- `examples/http` — HTTP web server on port 80
- `examples/mqtt` — MQTT publish / subscribe / both (menuconfig choice)
- `examples/netbios` — NetBIOS name service responder
- `examples/network_install` — PHY link / cable bring-up check
- `examples/pppoe` — PPPoE session establishment (**W5500 only**; the vendored
  PPPoE driver uses W5500 registers absent on W6300)
- `examples/upnp` — IGD discovery + port mapping via serial menu
- `examples/tcp_client_over_ssl` — TLS client over the WIZnet socket using
  mbedTLS (cert verification disabled for the demo)
- `examples/tcp_server_over_ssl` — TLS echo server over the WIZnet socket using
  mbedTLS (ECDHE-RSA suites; see the example README for the mbedTLS 4.0 notes)
- `examples/w6300_loopback` — W6300 reference (chip pinned to W6300)

Not ported from WIZnet-PICO-C: `can` (RP2040 PIO-specific) and `ftp` (FTP
module not present in the ioLibrary submodule).

Each example is an independent ESP-IDF project that uses ioLibrary APIs
directly after SPI port-layer initialization. Build from the example folder:

```bash
cd examples/loopback
idf.py build
```

The board and the per-socket RX/TX buffer size can be changed in menuconfig:

- `Component config -> WIZnet WSM Driver -> Board` (default: `ESP32-W5500-Dev-kit`)
- `Component config -> WIZnet WSM Driver -> Per-socket RX/TX buffer size (KB)`

The board decides the chip and the SPI host/clock/pins together, so those are derived and shown
read-only. Self-designed hardware picks `Board -> Custom`, which opens up the chip and the
GPIO/host/clock fields:

- `Component config -> WIZnet WSM Driver -> W6300` (chip; Custom only)
- `Component config -> WIZnet WSM Driver -> SPI host (2=SPI2, 3=SPI3)`
- `Component config -> WIZnet WSM Driver -> SPI clock (Hz)`
- `Component config -> WIZnet WSM Driver -> GPIO: MISO/MOSI/SCLK/CS/RESET/INT`

Example-specific endpoint values (static IP, ports, peer address) are kept in the
example sources for simplicity, not in menuconfig:

- `examples/<name>/inc/net_config.h` — every example except `w6300_loopback`,
  e.g. `examples/loopback/inc/net_config.h` (`LOOPBACK_PORT`,
  `LOOPBACK_TARGET_IP`, `LOOPBACK_TARGET_PORT`)
- `examples/w6300_loopback/main/main.c` (`EXAMPLE_TCP_LISTEN_PORT`, `EXAMPLE_UDP_LISTEN_PORT`)


## Using this component from ESP Component Registry

You can add this component to a new ESP-IDF project from the ESP Component Registry.

### 1. Create a new ESP-IDF project

```bash
idf.py create-project test_wsm_driver_production
cd test_wsm_driver_production
```
### 2. Set ESP-IDF target

```bash
idf.py set-target {target-chip}
Ex) idf.py set-target esp32s3
```

### 3. Add the component dependency

```bash
idf.py add-dependency "wiznet/wsm_driver=={version}"
Ex) idf.py add-dependency "wiznet/wsm_driver==1.0.0"
```
This command creates main/idf_component.yml automatically and adds the dependency.

After the dependency is resolved during build, the component will be downloaded under:

`managed_components/wiznet__wsm_driver/`

### 4. Add example code to your project

Copy the example application code from one of the component examples into your project main source file.

For example, you can refer to:

`managed_components/wiznet__wsm_driver/examples/loopback/main/main.c`
`managed_components/wiznet__wsm_driver/examples/tcp_server_multi_socket/main/main.c`
`managed_components/wiznet__wsm_driver/examples/w6300_loopback/main/main.c`

Then paste or adapt the code into your project source file, for example:

`main/test_wsm_driver_production.c`

### 5. Build & Flash
```bash
idf.py build
idf.py -p COM38 flash monitor
```

![][link-esp_idf_terminal]

![][link-hercules]

## License notes

- This component is MIT licensed.
- `ioLibrary_Driver` is a third-party dependency with its own license terms.


[link-esp_idf_terminal]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/esp_idf_terminal.png
[link-hercules]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/hercules.png
