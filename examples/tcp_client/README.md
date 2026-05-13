# TCP Client Example

This example connects to a TCP server, sends a string, receives a response, prints logs, and closes the socket.

## Build

```bash
idf.py set-target esp32s3
idf.py build
```

## Network configuration

Edit `main/main.c`:

- `EXAMPLE_USE_DHCP`: `1` for DHCP, `0` for static IP
- Static values: `EXAMPLE_STATIC_IP`, `EXAMPLE_STATIC_NETMASK`, `EXAMPLE_STATIC_GATEWAY`, `EXAMPLE_STATIC_DNS`
- Server endpoint: `EXAMPLE_SERVER_IP`, `EXAMPLE_SERVER_PORT`

The project uses the local component path via `EXTRA_COMPONENT_DIRS ../../`.

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

1. ESP initializes W5500 TOE
2. DHCP or static IP is applied
3. ESP connects to server
4. ESP sends `hello from esp32s3`
5. ESP receives and logs response
6. Socket closes and retries
