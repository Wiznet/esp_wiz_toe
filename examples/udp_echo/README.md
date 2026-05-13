# UDP Echo Example

This example opens a UDP socket, sends a payload to a remote endpoint, waits for UDP data, and logs the received packet.

## Build

```bash
idf.py set-target esp32s3
idf.py build
```

## Network configuration

Edit `main/main.c`:

- `EXAMPLE_USE_DHCP`: `1` for DHCP, `0` for static IP
- Static values: `EXAMPLE_STATIC_IP`, `EXAMPLE_STATIC_NETMASK`, `EXAMPLE_STATIC_GATEWAY`, `EXAMPLE_STATIC_DNS`
- Remote endpoint: `EXAMPLE_REMOTE_IP`, `EXAMPLE_REMOTE_PORT`
- Local bind port: `EXAMPLE_LOCAL_PORT`

The project uses the local component path via `EXTRA_COMPONENT_DIRS ../../`.

## Run

```bash
idf.py -p <PORT> flash monitor
```

## Test with netcat

On PC (UDP listener):

```bash
nc -u -l 5001
```

Expected behavior:

1. ESP initializes W5500 TOE
2. DHCP or static IP is applied
3. UDP socket opens
4. ESP sends `hello from esp32s3`
5. Incoming UDP packet is logged with source IP/port
6. Socket closes and repeats
