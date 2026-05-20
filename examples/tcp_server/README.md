
# TCP Server Example

This example initializes the SPI port layer, configures W5500 through ioLibrary, and runs a TCP server loopback using a non-blocking socket and direct RX buffer access (wiz_recv_data).

**Key points:**
- The socket is opened in non-blocking mode (SF_IO_NONBLOCK) to avoid WDT during accept/connect.
- Data reception does not use the standard recv() API, but instead reads directly from the RX buffer using wiz_recv_data and setSn_CR(Sn_CR_RECV).
- This approach avoids the ioLibrary's non-blocking recv() busy loop issue and prevents WDT resets.

**Note:**
- The application is responsible for checking socket state and RX buffer size before reading.
## Build

```bash
idf.py set-target esp32s3
idf.py build
```

## Configuration

- Listen port is configured in `main/main.c`:
  - `EXAMPLE_LISTEN_PORT`
- SPI host/clock/pins come from:
  - `Component config -> WIZnet TOE Component`

## Run

```bash
idf.py -p <PORT> flash monitor
```

## Test with netcat

On PC (client side):

```bash
nc <ESP_IP> 5000
```

Type text and press Enter. The server loopback handler echoes payload using direct RX buffer access.
