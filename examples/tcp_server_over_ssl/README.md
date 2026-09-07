# How to Test TCP Server over SSL Example

> **Verified on both chips.** This example was run on a W6300 (QSPI) and on a W5500 (standard SPI, XIAO ESP32-S3), over Ethernet and Wi-Fi in each case.

## Step 1: Prepare software

The following serial terminal program and SSL test client are required for the TCP Server over SSL example test, download and install from below links.

- [Tera Term][link-tera_term]
- [OpenSSL][link-openssl]

## Step 2: Prepare hardware

1. Connect the WIZnet Ethernet chip (W5500 or W6300) to the ESP32-S3 board over SPI, following the pin table in [Step 3](#step-3-setup-tcp-server-over-ssl-example).
2. Connect an Ethernet cable from the module's RJ45 port to your PC or network.
3. Connect the ESP32-S3 board to your PC with a USB cable.

![][link-hardware]

## Step 3: Setup TCP Server over SSL Example

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

Network identity, ports and TLS timings live in `examples/tcp_server_over_ssl/inc/net_config.h`, the same way as in `examples/loopback`:

```cpp
#define NET_MAC_ADDR          {0x00, 0x08, 0xDC, 0x12, 0x34, 0x56}
#define NET_IP_ADDR           {192, 168, 11, 2}
#define NET_SUBNET_MASK       {255, 255, 255, 0}
#define NET_GATEWAY           {192, 168, 11, 1}
#define NET_DNS_ADDR          {8, 8, 8, 8}

#define WIFI_SSID             ""      /* empty -> Ethernet only */
#define WIFI_PASS             ""

#define SSL_ECHO_PORT         443     /* Ethernet */
#define WIFI_SSL_ECHO_PORT    8443    /* Wi-Fi    */
#define SSL_ECHO_TIMEOUT_MS   (10 * 1000)
#define SSL_ECHO_BUF_SIZE     2048
#define SSL_ECHO_BANNER       "WIZnet TOE SSL server ready\r\n"
```

`main.c` assembles a `wiz_NetInfo` from these and hands it to `wiznet_net_init()`, which applies it to the chip with `wizchip_setnetinfo()`.

### Server credentials

The server presents the bundled mbedTLS test certificate/key (`inc/ssl_credentials.h` — `SSL_SERVER_CRT_PEM` / `SSL_SERVER_KEY_PEM`, CN=`localhost`, RSA 2048). These are demo credentials — replace them with your own certificate and key for production.

### Running on Wi-Fi at the same time (optional)

Fill in `WIFI_SSID` and the same TLS server also comes up on a Wi-Fi STA, as a sibling task at the same level as the Ethernet one:

```cpp
ssl_echo_start("eth",  &net_eth_ops,  SSL_ECHO_PORT,      wiznet_net_is_up);
ssl_echo_start("wifi", &net_wifi_ops, WIFI_SSL_ECHO_PORT, wifi_net_is_up);
```

Each interface gets its own task **and its own complete mbedTLS state** (context, config, parsed certificate and key), so both can handshake at the same time without sharing anything. The two use different ports because with `SOCKET_WRAP=0` (esp_eth backend) they share one LwIP stack, where identical ports would clash on bind.

Leave `WIFI_SSID` empty when committing; an empty SSID skips Wi-Fi entirely so the example still builds and runs for everyone else.

### Architecture

Same layout as `examples/loopback`:

| Path | Role |
|------|------|
| `inc/net_config.h` | network identity, ports, TLS timings |
| `inc/ssl_credentials.h` | demo server certificate and key |
| `inc/ssl_echo.h` | engine API |
| `src/ssl_echo.c` | backend-neutral TLS echo server (BSD sockets via a vtable) |
| `main/main.c` | orchestration only: bring interfaces up, start the tasks |

The engine calls BSD sockets through the component's `net_sock_ops_t` vtable, and mbedTLS reaches the socket through a BIO bound to that vtable — so no TLS code contains an `#if WSM_DRIVER_SOCKET_WRAP`.

Compared with the ioLibrary version this was ported from, the `Sn_SR` state machine (`SOCK_ESTABLISHED` / `SOCK_CLOSE_WAIT` / `SOCK_CLOSED` plus the manual re-listen) collapses into a plain `accept()` loop, and the `getsockopt(SO_RECVBUF)` poll before every read disappears because `SO_RCVTIMEO` lets `mbedtls_ssl_read()` block with a bound. Both backends report a receive timeout as `-1`/`EWOULDBLOCK`, so one BIO covers the WIZnet hardware sockets and the software LwIP alike.

### mbedTLS 4.0 note

ESP-IDF v6.0 ships **mbedTLS 4.0**, which removed static-RSA key exchange. The `MBEDTLS_TLS_RSA_WITH_AES_*` ciphersuites pinned by the WIZnet-PICO-C reference no longer exist, `mbedtls_ssl_conf_rng()` is gone (entropy now comes from PSA Crypto, which ESP-IDF initializes at startup), and `mbedtls_pk_parse_key()` lost its trailing RNG arguments. This example offers **ECDHE-RSA** suites instead: the same RSA certificate, ephemeral key agreement, and every modern client supports them.

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

When the device boots, the assigned IP and the listening log appear in the terminal.

```
I (522) wiztoe_net: TOE up: 192.168.11.2 (WIZnet hardware TCP/IP)
I (525) ssl_echo: [eth] waiting for link...
I (527) ssl_echo: [eth] SSL server listening on port 443
```

With Wi-Fi configured, the second server appears once DHCP has assigned an address:

```
I (xxxxx) wifi: got IP 192.168.11.7
I (xxxxx) ssl_echo: [wifi] SSL server listening on port 8443
```

![][link-run_socket_open]

From your PC (on the same `192.168.11.x` network), connect with the OpenSSL test client:

```bash
openssl s_client -connect 192.168.11.2:443            # Ethernet
openssl s_client -connect 192.168.11.7:8443           # Wi-Fi
```

To pin the ciphersuites the server offers, force TLS 1.2 with ECDHE-RSA:

```bash
openssl s_client -connect 192.168.11.2:443 -tls1_2 \
    -cipher 'ECDHE-RSA-AES128-GCM-SHA256:ECDHE-RSA-AES256-GCM-SHA384:ECDHE-RSA-AES128-SHA256'
```

On connection the device completes the TLS handshake, prints the negotiated ciphersuite, and sends a greeting. Anything you type into the `s_client` console is echoed back:

```
I (xxxxx) ssl_echo: [eth] client connected
I (xxxxx) ssl_echo: [eth] handshake OK, ciphersuite TLS-ECDHE-RSA-WITH-AES-128-GCM-SHA256
I (xxxxx) ssl_echo: [eth] received: hello
```

![][link-run_ssl]

The `s_client` console shows the server certificate (CN=localhost), the greeting `WIZnet TOE SSL server ready`, and your echoed input. Closing the client logs `[eth] client disconnected` and the device accepts the next connection.

> The certificate is self-signed by the PolarSSL test CA, so `s_client` reports `verify error:num=20` / `self-signed certificate`. That is expected — the server runs with `MBEDTLS_SSL_VERIFY_NONE` and the handshake still completes.

## Appendix

- **Client authentication disabled:** For a dependency-free demo, the server sets `MBEDTLS_SSL_VERIFY_NONE`, so client certificates are not requested/checked. For production, require client certs with `mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_REQUIRED)` and a CA via `mbedtls_ssl_conf_ca_chain`.
- **Server certificate:** The bundled certificate/key come from the mbedTLS test suite (`server2-sha256.crt` / `server2.key`). Browsers and verifying clients will reject it (untrusted CA, CN=localhost); use `openssl s_client` which does not verify by default, or supply your own trusted certificate.
- **Ciphersuites:** The server restricts to RSA key-exchange suites (`TLS-RSA-WITH-AES-256-CBC-SHA256`, `-AES-128-GCM-SHA256`, `-AES-128-CBC-SHA256`), matching the WIZnet-PICO-C reference. These require `CONFIG_MBEDTLS_KEY_EXCHANGE_RSA` (enabled by default and pinned in `sdkconfig.defaults`). If the client negotiates TLS 1.3, that legacy list does not apply and the RSA certificate is used for a TLS 1.3 handshake instead.
- **mbedTLS and RNG:** mbedTLS is provided by ESP-IDF. The RNG callback is backed by the ESP hardware RNG via `esp_fill_random`, replacing the weak `rand()` used in the original WIZnet-PICO-C example.
- **W6300 QSPI mode:** Quad mode (4-bit) requires the extra D2/D3 lines wired and selected in `Component config -> WIZnet WSM Driver -> W6300 QSPI mode`. Single mode uses the same 4-wire wiring as W5500.

<!-- Link -->
[link-tera_term]: https://osdn.net/projects/ttssh2/releases/
[link-openssl]: https://www.openssl.org/source/

[link-hardware]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/tcp_server_over_ssl/hardware.png
[link-config_main]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/tcp_server_over_ssl/config_main.png
[link-config_component]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/tcp_server_over_ssl/config_component.png
[link-config_wiz_toe]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/tcp_server_over_ssl/config_wiz_toe.png

[link-build_log]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/tcp_server_over_ssl/build_log.png
[link-run_socket_open]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/tcp_server_over_ssl/run_socket_open.png
[link-run_ssl]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/tcp_server_over_ssl/run_ssl.png
