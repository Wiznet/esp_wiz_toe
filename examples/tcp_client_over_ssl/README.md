# How to Test TCP Client over SSL Example

## Step 1: Prepare software

The following serial terminal program and SSL test server are required for the TCP Client over SSL example test, download and install from below links.

- [Tera Term][link-tera_term]
- [OpenSSL][link-openssl]

## Step 2: Prepare hardware

1. Connect the WIZnet Ethernet chip (W5500 or W6300) to the ESP32-S3 board over SPI, following the pin table in [Step 3](#step-3-setup-tcp-client-over-ssl-example).
2. Connect an Ethernet cable from the module's RJ45 port to your PC or network.
3. Connect the ESP32-S3 board to your PC with a USB cable.

![][link-hardware]

## Step 3: Setup TCP Client over SSL Example

### Chip and SPI configuration

Set the target and open menuconfig:

```bash
idf.py set-target esp32s3
idf.py menuconfig
```

Select **Component config**.
![][link-config_main]

Select **WIZnet WSM Driver** under Component config.
![][link-config_component]

Choose the board, and check the per-socket buffer size. The chip, SPI host, clock, and pins all follow the selected board automatically. In this example, SPI2 of the ESP32-S3 is used at 33 MHz.
![][link-config_wiz_toe]

> This example ships with the **ESP32-W5500-Dev-kit** board, which is the component Kconfig default -- `sdkconfig.defaults` no longer names a chip. Pick a different board under `Component config -> WIZnet WSM Driver -> Board`: the board fixes the chip and the SPI pins together, and the chip is separately selectable only on `Board -> Custom`.

**W5500 wiring (standard SPI)**

| W5500 | ESP32-S3 Pin |
|-------|--------------|
| MISO  | 13 |
| MOSI  | 11 |
| SCLK  | 12 |
| CS    | 10 |
| RESET | 9  |
| INT   | 14 |

**W6300 wiring (QSPI)**

| W6300   | ESP32-S3 Pin |
|---------|--------------|
| D0 (MOSI) | 11 |
| D1 (MISO) | 13 |
| D2 (IO2)  | 14 *(Quad mode only)* |
| D3 (IO3)  | 9  *(Quad mode only)* |
| SCLK    | 12 |
| CS      | 10 |
| RESET   | 21 |
| INT     | 8  |

### Network configuration

All example settings live in `examples/tcp_client_over_ssl/inc/net_config.h`. The SPI wiring is **not** here — it comes from the component Kconfig shown above.

```cpp
#define NET_MAC_ADDR    {0x00, 0x08, 0xDC, 0x12, 0x34, 0x56}  // MAC address
#define NET_IP_ADDR     {192, 168, 11, 2}                     // IP address
#define NET_SUBNET_MASK {255, 255, 255, 0}                    // Subnet Mask
#define NET_GATEWAY     {192, 168, 11, 1}                     // Gateway
#define NET_DNS_ADDR    {8, 8, 8, 8}                          // DNS server
```

### Wi-Fi configuration

This example runs a TLS client **over the WIZnet chip and over Wi-Fi at the same time**, both against the same server, so fill in your AP credentials in the same file:

```cpp
#define WIFI_SSID "your-ssid"
#define WIFI_PASS "your-password"
```

Leaving the placeholders in place is harmless: the Wi-Fi session simply keeps retrying and the Ethernet one is unaffected.

### SSL server configuration

The target server is in the same `net_config.h`. Make sure it matches the IP of the PC running the OpenSSL test server.

```cpp
#define SSL_TARGET_IP       "192.168.11.4"
#define SSL_TARGET_PORT     443

#define SSL_RECV_TIMEOUT_MS (1000 * 10)
#define SSL_RETRY_DELAY_MS  (1000 * 5)   // wait before reconnecting
#define SSL_HELLO_MSG       " W5x00 TCP over SSL test\n"
```

Both interfaces connect from ephemeral local ports, so the two sessions never collide. The server will see **two** clients.

## Step 4: Build

After completing the setup, build the project.

```bash
idf.py build
```

![][link-build_log]

## Step 5: Upload and Run

Flash the firmware and open the serial monitor. Replace the port with your board's serial port.

```bash
idf.py -p COMx flash monitor
```

On Linux/macOS:

```bash
idf.py -p /dev/ttyUSB0 flash monitor
```

Before flashing, start an SSL test server on your PC with OpenSSL. Generate a self-signed certificate (if you do not already have one), then run `s_server` on port 443:

```bash
openssl req -x509 -newkey rsa:2048 -keyout server.key -out server.crt -days 365 -nodes
openssl s_server -accept 443 -cert server.crt -key server.key
```

When the device boots, the assigned IP and the connection progress appear in the terminal, once per interface.

```
wiztoe_net: TOE up: 192.168.11.2 (WIZnet hardware TCP/IP)
ssl_client: [eth] connecting to 192.168.11.4:443
ssl_client: [eth] TCP connected, starting TLS handshake
ssl_client: [eth] TLS ok [ Ciphersuite: ... ]
wifi: got IP 192.168.0.42
ssl_client: [wifi] connecting to 192.168.11.4:443
ssl_client: [wifi] TLS ok [ Ciphersuite: ... ]
```

![][link-run_socket_open]

After the TCP socket connects, the device runs the mbedTLS handshake against the OpenSSL server. On success it prints the negotiated ciphersuite and sends a test message to the server.
![][link-run_handshake]

The `openssl s_server` console shows the incoming connection and receives the test string `W5x00 TCP over SSL test`. Type any text in the `s_server` console to send it back; the device prints whatever the server sends.
![][link-run_ssl]

> `openssl s_server` serves **one connection at a time**. With both interfaces enabled you have two clients, so the second one waits and retries every `SSL_RETRY_DELAY_MS` until the first session ends. Run a second `s_server` on another port, or disable one interface (see below), if you want both connected at once.

## Appendix

- **Certificate verification disabled:** For a dependency-free demo, the client sets `MBEDTLS_SSL_VERIFY_NONE`, so the server certificate is not checked. This lets the handshake succeed against a self-signed OpenSSL certificate. For production, load a CA certificate with `mbedtls_x509_crt_parse`, set `mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_REQUIRED)`, and provide it via `mbedtls_ssl_conf_ca_chain`.
- **mbedTLS:** provided by ESP-IDF, which also supplies the entropy source, so the example sets no RNG callback of its own (the original WIZnet-PICO-C example had to, and used a weak `rand()`).
- **How one client drives two interfaces:** the TLS logic in `src/ssl_client.c` calls BSD sockets through a vtable, and mbedTLS's BIO is wired to that vtable. For Ethernet it is the plain `lwip_*` set, which the `wsm_driver` component redirects to the chip's hardware sockets at link time (`-Wl,--wrap`, `CONFIG_WSM_DRIVER_SOCKET_WRAP`); for Wi-Fi it is the un-wrapped `__real_lwip_*` set in `src/wifi_ssl_client.c`. The mbedTLS context and config are per task, since two sessions cannot share one `mbedtls_ssl_context`.
- **Timeout and reconnect:** TLS reads use `SSL_RECV_TIMEOUT_MS`, implemented with `SO_RCVTIMEO` on the socket (the TOE has no `select()`). After a session ends or fails, the client waits `SSL_RETRY_DELAY_MS` and reconnects. If the handshake never succeeds, confirm the server IP/port and that `s_server` is listening.
- **W6300 and `SF_FORCE_ARP`:** the original version of this example opened its hardware socket with `SF_FORCE_ARP` and exited on the first failed connect. That flag is not reachable through the BSD socket API, and it is not needed: the `tcp_client` example connects on the same chip with open flags `0`, and recovers from a failed connect by retrying from its `SOCK_CLOSED` state. This client retries the same way, so a lost first SYN costs one `SSL_RETRY_DELAY_MS` rather than the session.
- **Ethernet only:** remove the `ssl_client_start("wifi", ...)` call (and `wifi_net_init`) from `main/main.c`.
- **W6300 QSPI mode:** Quad mode (4-bit) requires the extra D2/D3 lines wired and selected in `Component config -> WIZnet WSM Driver -> W6300 QSPI mode`. Single mode uses the same 4-wire wiring as W5500.

<!-- Link -->
[link-tera_term]: https://osdn.net/projects/ttssh2/releases/
[link-openssl]: https://www.openssl.org/source/

[link-hardware]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/tcp_client_over_ssl/hardware.png
[link-config_main]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/tcp_client_over_ssl/config_main.png
[link-config_component]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/tcp_client_over_ssl/config_component.png
[link-config_wiz_toe]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/tcp_client_over_ssl/config_wiz_toe.png

[link-build_log]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/tcp_client_over_ssl/build_log.png
[link-run_socket_open]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/tcp_client_over_ssl/run_socket_open.png
[link-run_handshake]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/tcp_client_over_ssl/run_handshake.png
[link-run_ssl]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/tcp_client_over_ssl/run_ssl.png
