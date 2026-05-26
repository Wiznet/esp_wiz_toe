
# TCP Client Example

This example initializes the SPI port layer, configures W5500 through ioLibrary, and runs a TCP client loopback using a non-blocking socket and direct RX buffer access (wiz_recv_data).

**Key points:**
- The socket is opened in non-blocking mode (SF_IO_NONBLOCK) to avoid WDT during connect.
- Data reception does not use the standard recv() API, but instead reads directly from the RX buffer using wiz_recv_data and setSn_CR(Sn_CR_RECV).
- This approach avoids the ioLibrary's non-blocking recv() busy loop issue and prevents WDT resets.

**Note:**
- The application is responsible for checking socket state and RX buffer size before reading.

## Configuration

### Chip Configuration
```bash
idf.py set-target esp32s3
idf.py menuconfig
```

Select the Component config option in menuconfig.
![][link-config_main]

Select the WIZnet TOE Component option under Component config.
![][link-config_component]

Configure the WIZnet chip, SPI host number, clock, pins, and socket buffer size settings as shown below. In this example, SPI2 of the ESP32-S3 was used.
![][link-config_wiz_toe]

| W5500 | ESP32S3 Pin |
|----------|----------|
| MISO | 13 |
| MOSI | 11 |
| SCLK | 12 |
| CS | 10 |
| RESET | 9 |
| INT | 14 |

### Network Configuration
Configure the network settings in the `example/tcp_client/main/main.c file`.
```bash
static const uint8_t EXAMPLE_SERVER_IP[4] = {192, 168, 11, 100};
static const uint16_t EXAMPLE_SERVER_PORT = 5000;
```

## Build
```bash
idf.py build
```
![][link-build_log]

## Run

```bash
idf.py -p <PORT> flash monitor
```
If flashing succeeds, you should see socket open logs in the terminal as shown below.
![][link-run_socket_open]

Open a TCP server in the Hercules tool.
![][link-run_tcp_server]

After successfully connecting to the TCP server, send data from the Hercules tool. You can verify that the transmitted data is received exactly as sent.
![][link-run_loopback]

<!--
Link
-->
[link-config_main]: https://raw.githubusercontent.com/Wiznet/esp_wiz_toe/main/static/image/tcp_client/config_main.png
[link-config_component]: https://raw.githubusercontent.com/Wiznet/esp_wiz_toe/main/static/image/tcp_client/config_component.png
[link-config_wiz_toe]: https://raw.githubusercontent.com/Wiznet/esp_wiz_toe/main/static/image/tcp_client/config_wiz_toe.png
[link-config_wiz_toe]: https://raw.githubusercontent.com/Wiznet/esp_wiz_toe/main/static/image/tcp_client/build_log.png

[link-build_log]: https://raw.githubusercontent.com/Wiznet/esp_wiz_toe/main/static/image/tcp_client/build_log.png
[link-run_socket_open]: https://raw.githubusercontent.com/Wiznet/esp_wiz_toe/main/static/image/tcp_client/run_socket_open.png
[link-run_tcp_server]: https://raw.githubusercontent.com/Wiznet/esp_wiz_toe/main/static/image/tcp_client/run_tcp_server.png
[link-run_loopback]: https://raw.githubusercontent.com/Wiznet/esp_wiz_toe/main/static/image/tcp_client/run_loopback.png
