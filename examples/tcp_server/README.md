# TCP Server Example

This example initializes the SPI port layer, configures W5500 through ioLibrary, and runs TCP server loopback using `loopback_tcps`.

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

Type text and press Enter. The server loopback handler echoes payload.
