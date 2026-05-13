# TCP Server Example

This example starts a TCP server, waits for a client, receives data, echoes it back, and continues listening in a loop.

## Build

```bash
idf.py set-target esp32s3
idf.py build
```

## Network configuration

Edit `main/main.c`:

- `EXAMPLE_USE_DHCP`: `1` for DHCP, `0` for static IP
- Static values: `EXAMPLE_STATIC_IP`, `EXAMPLE_STATIC_NETMASK`, `EXAMPLE_STATIC_GATEWAY`, `EXAMPLE_STATIC_DNS`
- Listen port: `EXAMPLE_LISTEN_PORT`

The project uses the local component path via `EXTRA_COMPONENT_DIRS ../../`.

## Run

```bash
idf.py -p <PORT> flash monitor
```

## Test with netcat

On PC (client side):

```bash
nc <ESP_IP> 5000
```

Type text and press Enter. The server echoes the same payload.

Expected behavior:

1. ESP initializes W5500 TOE
2. DHCP or static IP is applied
3. TCP listen starts
4. Client connection is accepted
5. Data is received
6. Data is echoed
7. On disconnect/error, socket closes and server keeps listening
