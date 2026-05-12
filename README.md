# esp_w5500_toe

ESP-IDF v5.4.x component for the WIZnet W5500 using its built-in **TCP/IP Offload Engine (TOE)**.

---

## Architecture

```
Application
    │
    ▼
esp_w5500_toe API  ←──  ioLibrary socket() / send() / recv()
    │                   (W5500 hardware TCP/IP – Sn_MR_TCP / Sn_MR_UDP)
    ▼
esp_w5500_toe_spi  ──►  ESP32-S3 SPI master (driver/spi_master.h)
    │
    ▼
W5500 chip  (SPI)
```

### What this component is NOT

| What you might expect          | What this component does instead                       |
|-------------------------------|--------------------------------------------------------|
| `esp_eth` MAC RAW driver       | **Not used.** W5500 is driven directly via SPI.        |
| lwIP `netif` / `esp_netif`     | **Not used.** TCP/IP runs entirely inside W5500.       |
| Host-side TCP/IP stack         | **Not used.** All socket state lives in W5500 hardware.|
| DHCP client (esp-idf/lwIP)     | **Not used.** Static IP only (DHCP reserved for future).|

The W5500 contains its own hardwired TCP/IP stack with 8 independent hardware
socket channels.  Each socket channel handles its own TCP state machine,
retransmission, checksum generation, etc. autonomously.  The ESP32-S3 only
needs to read/write payload data over SPI.

---

## ioLibrary dependency

The [WIZnet ioLibrary_Driver](https://github.com/Wiznet/ioLibrary_Driver) is
included as a git submodule:

```
third_party/ioLibrary_Driver/
```

After cloning, run:

```bash
git submodule update --init --recursive
```

Only the following ioLibrary sources are compiled:

| File                                   | Purpose                           |
|----------------------------------------|-----------------------------------|
| `Ethernet/W5500/w5500.c`               | W5500 register-level driver       |
| `Ethernet/socket.c`                    | Hardware socket API               |
| `Ethernet/wizchip_conf.c`              | Chip config / callback glue       |

---

## Target

| Item          | Value              |
|---------------|--------------------|
| SoC           | ESP32-S3           |
| IDF version   | ≥ 5.4.0            |
| W5500 interface | SPI (full-duplex) |
| Max sockets   | 8 (configurable via Kconfig) |

---

## Quick start

```c
#include "esp_w5500_toe.h"

void app_main(void)
{
    esp_w5500_toe_config_t cfg = {
        .spi = {
            .spi_host       = SPI2_HOST,
            .pin_mosi       = 11,
            .pin_miso       = 13,
            .pin_sclk       = 12,
            .pin_cs         = 10,
            .pin_rst        = 9,
            .pin_int        = 8,
            .clock_speed_mhz = 40,
        },
        .net = {
            .mac     = {0x00, 0x08, 0xDC, 0x01, 0x02, 0x03},
            .ip      = {192, 168, 1, 100},
            .subnet  = {255, 255, 255, 0},
            .gateway = {192, 168, 1, 1},
            .dns     = {8, 8, 8, 8},
        },
    };

    ESP_ERROR_CHECK(esp_w5500_toe_init(&cfg));

    /* Open TCP socket on hardware socket 0, local port 5000 */
    ESP_ERROR_CHECK(esp_w5500_toe_tcp_open(0, 5000));

    uint8_t server_ip[] = {192, 168, 1, 10};
    ESP_ERROR_CHECK(esp_w5500_toe_tcp_connect(0, server_ip, 8080));

    /* Now call ioLibrary send()/recv() directly on socket 0 */
}
```

---

## Adding to your project

In your project `CMakeLists.txt`:

```cmake
set(EXTRA_COMPONENT_DIRS path/to/esp_w5500_toe)
```

Or add it as an IDF component inside the `components/` folder of your project.

---

## Kconfig options

All options live under `Component config → ESP W5500 TOE Component`:

| Symbol                            | Default | Description                         |
|-----------------------------------|---------|-------------------------------------|
| `W5500_TOE_SPI_HOST`              | 1       | SPI host (1=SPI2, 2=SPI3)           |
| `W5500_TOE_SPI_CLOCK_MHZ`         | 40      | SPI clock in MHz                    |
| `W5500_TOE_PIN_MOSI/MISO/SCLK/CS` | —       | GPIO assignments                    |
| `W5500_TOE_PIN_RST`               | 9       | Reset GPIO (-1 to disable)          |
| `W5500_TOE_PIN_INT`               | 8       | Interrupt GPIO (-1 to disable)      |
| `W5500_TOE_MAX_SOCKETS`           | 4       | Number of hardware sockets in use   |
| `W5500_TOE_SOCKET_TX_BUF_KB`      | 2       | TX buffer per socket (KB)           |
| `W5500_TOE_SOCKET_RX_BUF_KB`      | 2       | RX buffer per socket (KB)           |

---

## File layout

```
esp_w5500_toe/
├── CMakeLists.txt
├── idf_component.yml
├── Kconfig
├── README.md
├── include/
│   └── esp_w5500_toe.h          # Public API
├── src/
│   ├── esp_w5500_toe.c          # Lifecycle: init / deinit
│   ├── esp_w5500_toe_spi.c      # SPI master + ioLibrary SPI callbacks
│   ├── esp_w5500_toe_spi.h      # (internal)
│   ├── esp_w5500_toe_socket.c   # Socket open / connect / close helpers
│   └── esp_w5500_toe_socket.h   # (internal)
└── third_party/
    └── ioLibrary_Driver/        # git submodule
```

---

## License

MIT – see `LICENSE` file.  
WIZnet ioLibrary_Driver is licensed separately under its own MIT-style license.
