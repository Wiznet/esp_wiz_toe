# esp_wiz_toe

ESP-IDF component for ESP32-S3 that controls WIZnet W5500 in TOE mode (socket offload) using WIZnet ioLibrary.

## Overview

`esp_wiz_toe` provides a thin, blocking-first wrapper over W5500 hardware sockets.
The component directly controls W5500 through SPI and ioLibrary callback binding.

## Supported chip

- ESP32-S3

## Supported WIZnet chip

- W5500

## Supported features

- TCP client
- TCP server (listen + accept wait)
- UDP send/recv
- DHCP (ioLibrary `Internet/DHCP`)
- DNS resolve (ioLibrary `Internet/DNS`)

## Unsupported features

- TLS/SSL offload or TLS wrapper
- lwIP socket compatibility layer
- `esp_eth` integration mode
- MAC RAW mode

## Architecture

```text
Application
    -> esp_wiz_toe public API
        -> socket/net thin wrappers
            -> ioLibrary (Ethernet + Internet)
                -> W5500 SPI callback layer
                    -> ESP-IDF SPI Master driver
```

Key source files:

- `src/esp_wiz_toe.c`: lifecycle, reset/init, netinfo setup
- `src/esp_wiz_toe_spi.c`: SPI transport and ioLibrary callback bridge
- `src/esp_wiz_toe_socket.c`: TCP/UDP thin wrappers
- `src/esp_wiz_toe_net.c`: DHCP/DNS blocking helpers

## Difference from esp_eth

- `esp_eth` typically integrates with ESP-IDF networking pipeline and can use MAC/PHY abstractions.
- `esp_wiz_toe` does not create or use `esp_eth` objects.
- W5500 socket engine is controlled directly through ioLibrary + SPI callbacks.

## Relationship with lwIP

- This component does not create `esp_netif` instances.
- This component does not use lwIP DHCP/DNS/socket APIs.
- Networking APIs are W5500/ioLibrary driven.

## ioLibrary submodule initialization

`ioLibrary_Driver` is expected under `third_party/ioLibrary_Driver`.

```bash
git submodule add https://github.com/Wiznet/ioLibrary_Driver.git third_party/ioLibrary_Driver
git submodule update --init --recursive
```

## Usage

Minimal flow:

1. Fill `esp_wiz_toe_config_t`
2. Call `esp_wiz_toe_init()`
3. Optional DHCP: `esp_wiz_toe_dhcp_start()`
4. Use TCP/UDP APIs
5. Call `esp_wiz_toe_deinit()`

Example API groups:

- Lifecycle: `esp_wiz_toe_init`, `esp_wiz_toe_deinit`, `esp_wiz_toe_reset`
- Link: `esp_wiz_toe_is_link_up`, `esp_wiz_toe_get_link_status`
- TCP: `esp_wiz_toe_tcp_connect`, `esp_wiz_toe_tcp_listen`, `esp_wiz_toe_tcp_accept_wait`, `esp_wiz_toe_send`, `esp_wiz_toe_recv`, `esp_wiz_toe_close`
- UDP: `esp_wiz_toe_udp_open`, `esp_wiz_toe_udp_sendto`, `esp_wiz_toe_udp_recvfrom`, `esp_wiz_toe_close`
- DHCP/DNS: `esp_wiz_toe_dhcp_start`, `esp_wiz_toe_dhcp_stop`, `esp_wiz_toe_dns_resolve`

## menuconfig options

`Component config -> WIZnet TOE Component`

- `ESP_WIZ_TOE_ENABLED`
- `ESP_WIZ_TOE_SPI_HOST`
- `ESP_WIZ_TOE_SPI_CLOCK_HZ`
- `ESP_WIZ_TOE_PIN_MISO`
- `ESP_WIZ_TOE_PIN_MOSI`
- `ESP_WIZ_TOE_PIN_SCLK`
- `ESP_WIZ_TOE_PIN_CS`
- `ESP_WIZ_TOE_PIN_RST`
- `ESP_WIZ_TOE_PIN_INT`
- `ESP_WIZ_TOE_RX_BUF_KB`
- `ESP_WIZ_TOE_TX_BUF_KB`

## Running examples

Available examples:

- `examples/tcp_client`
- `examples/tcp_server`
- `examples/udp_echo`

Example build/run:

```bash
cd examples/tcp_client
idf.py set-target esp32s3
idf.py build
idf.py -p <PORT> flash monitor
```

Netcat quick tests:

- TCP client target server on PC: `nc -l 5000`
- TCP server test from PC: `nc <ESP_IP> 5000`
- UDP test on PC: `nc -u -l 5001`

## Concurrency policy

- MVP uses one global recursive mutex for socket/net operations.
- This keeps implementation simple and safe for multi-task access.
- Future plan is per-socket locking via socket manager.

## Timeout policy

- APIs are blocking-first with polling loops.
- `timeout_ms` controls polling deadline.
- `UINT32_MAX` means wait forever.
- On timeout, APIs return `ESP_ERR_TIMEOUT`.

## License notes

- This component is MIT licensed (see `idf_component.yml` / repository license file).
- `ioLibrary_Driver` is a third-party dependency with its own license terms.
- Verify third-party license compatibility before distribution.

## Component Registry pre-publish checklist

- Set final `version` in `idf_component.yml`
- Set real `repository` URL in `idf_component.yml`
- Ensure `name` is unique/available in Espressif Component Registry
- Confirm `targets`, `license`, and `dependencies` are correct
- Verify examples build for ESP32-S3
- Verify submodule initialization instructions are accurate
- Run static checks and resolve warnings
- Tag release and publish release notes/changelog
