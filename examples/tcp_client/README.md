# TCP Client Example

This example initializes the SPI port layer, configures W5500 through ioLibrary, and runs TCP client loopback using `loopback_tcpc`.

## Build

```bash
idf.py set-target esp32s3
idf.py build
```

## Configuration

- Server endpoint is configured in `main/main.c`:
  - `EXAMPLE_SERVER_IP`
  - `EXAMPLE_SERVER_PORT`
- SPI host/clock/pins come from:
  - `Component config -> WIZnet TOE Component`

## Run

```bash
idf.py -p <PORT> flash monitor
```

## Test with netcat

On PC (server side):

```bash
nc -l 5000
```

Expected behavior:

1. ESP initializes SPI and W5500
2. Network info is applied
3. TCP client connects to server
4. Loopback handler echoes received payload
