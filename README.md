# esp_wiz_toe

ESP-IDF component that provides a W5500 SPI port layer for direct WIZnet ioLibrary usage.

## Overview

This component is intentionally minimal.
It only handles:

- SPI transport setup for W5500
- ioLibrary callback registration (`reg_wizchip_*_cbfunc`)
- Hardware reset pin control
- PHY link state query

Application code uses ioLibrary APIs directly (`wizchip_init`, `socket`, `connect`, `send`, `recv`, `sendto`, `recvfrom`, DHCP/DNS APIs).

## Supported ESP-IDF targets

- esp32s3

## Supported WIZnet TOE chips

- W5500

Other WIZnet chips may be added later.

## Public API

Header: `include/esp_wiz_toe.h`

- `esp_wiz_toe_spi_init`
- `esp_wiz_toe_spi_deinit`
- `esp_wiz_toe_spi_register_iolib_callbacks`
- `esp_wiz_toe_spi_reset`
- `esp_wiz_toe_spi_wizchip_check`
- `esp_wiz_toe_spi_link_is_up`

## Usage flow

1. Fill `esp_wiz_toe_spi_config_t`
2. Call `esp_wiz_toe_spi_init()`
3. Call `esp_wiz_toe_spi_register_iolib_callbacks()`
4. Call `esp_wiz_toe_spi_reset()`
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

Available examples:

- `examples/tcp_client`
- `examples/tcp_server`

Each example uses ioLibrary APIs directly after SPI port-layer initialization.

SPI host/clock/pin defaults and per-socket RX/TX buffer size can be changed in menuconfig:

- `Component config -> WIZnet TOE Component -> WIZnet chip -> W5500`
- `Component config -> WIZnet TOE Component -> SPI host (2=SPI2, 3=SPI3)`
- `Component config -> WIZnet TOE Component -> SPI clock (Hz)`
- `Component config -> WIZnet TOE Component -> GPIO: MISO/MOSI/SCLK/CS/RESET/INT`
- `Component config -> WIZnet TOE Component -> Per-socket RX/TX buffer size (KB)`

Example-specific endpoint values are kept in each example source for simplicity:

- `examples/tcp_client/main/main.c` (`EXAMPLE_SERVER_IP`, `EXAMPLE_SERVER_PORT`)
- `examples/tcp_server/main/main.c` (`EXAMPLE_LISTEN_PORT`)


## Using this component from ESP Component Registry

You can add this component to a new ESP-IDF project from the ESP Component Registry.

### 1. Create a new ESP-IDF project

```bash
idf.py create-project test_esp_wiz_toe_production
cd test_esp_wiz_toe_production
```
### 2. Set ESP-IDF target

```bash
idf.py set-target {target-chip}
Ex) idf.py set-target esp32s3
```

### 3. Add the component dependency

```bash
idf.py add-dependency "wiznet/esp_wiz_toe=={version}"
Ex) idf.py add-dependency "wiznet/esp_wiz_toe==0.1.0-alpha.5"
```
This command creates main/idf_component.yml automatically and adds the dependency.

After the dependency is resolved during build, the component will be downloaded under:

`managed_components/wiznet__esp_wiz_toe/`

### 4. Add example code to your project

Copy the example application code from one of the component examples into your project main source file.

For example, you can refer to:

`managed_components/wiznet__esp_wiz_toe/examples/tcp_client/main/main.c`
`managed_components/wiznet__esp_wiz_toe/examples/tcp_server/main/main.c`


Then paste or adapt the code into your project source file, for example:

`main/test_esp_wiz_toe_production.c`

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


[link-esp_idf_terminal]: https://raw.githubusercontent.com/Wiznet/esp_wiz_toe/main/static/image/esp_idf_terminal.png
[link-hercules]: https://raw.githubusercontent.com/Wiznet/esp_wiz_toe/main/static/image/hercules.png
