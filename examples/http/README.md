# How to Test HTTP Server Example

## Step 1: Prepare software

The following serial terminal program and web browser are required for the HTTP Server example test, download and install from below links.

- [Tera Term][link-tera_term]
- A web browser (e.g. Chrome, Edge, or Firefox)

## Step 2: Prepare hardware

1. Connect the WIZnet Ethernet chip (W5500 or W6300) to the ESP32-S3 board over SPI, following the pin table in [Step 3](#step-3-setup-http-server-example).
2. Connect an Ethernet cable from the module's RJ45 port to your PC or network.
3. Connect the ESP32-S3 board to your PC with a USB cable.

![][link-hardware]

## Step 3: Setup HTTP Server Example

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

### Network backend

`Component config -> WIZnet WSM Driver -> Network backend` picks which stack carries the traffic. **The example source does not change** — the choice is made by the linker:

| menuconfig choice | What `src/http_server.c`'s `ops->socket()` / `ops->recv()` / `ops->send()` resolve to |
|-------------------|-------------------------------------------------------------------------------|
| **TOE (hardware TCP/IP)** *(default)* | `__wrap_lwip_*` — `-Wl,--wrap` redirects lwIP's BSD entry points to the WIZnet chip's hardware sockets (`port/ioLibrary_Driver/src/wiztoe_wrap.c`) |
| **esp_eth MACRAW + software LwIP** | `lwip_*` — the chip runs as a plain SPI Ethernet MAC and the ESP32-S3's software LwIP owns TCP/IP |

The engine never contains an `#if`: it is handed the component's `net_eth_ops` vtable (plain `lwip_*`, which the linker aims at whichever backend is selected), and Wi-Fi is handed `net_wifi_ops`, which binds `__real_lwip_*` when the wrap is active so Wi-Fi always reaches the real software LwIP.

The ioLibrary `httpServer` (`Internet/httpServer`) is **not** used. It drives the chip's socket registers directly, so `--wrap` has nothing to intercept and one source could not serve both backends — `src/http_server.c` speaks HTTP/1.1 over `ops->recv()` / `ops->send()` instead.

### Network configuration

Network identity and ports live in `examples/http/inc/net_config.h`, the same way as in `examples/loopback` and `examples/dhcp_dns`:

```cpp
#define NET_MAC_ADDR          {0x00, 0x08, 0xDC, 0x12, 0x34, 0x56}
#define NET_IP_ADDR           {192, 168, 11, 2}
#define NET_SUBNET_MASK       {255, 255, 255, 0}
#define NET_GATEWAY           {192, 168, 11, 1}
#define NET_DNS_ADDR          {8, 8, 8, 8}

#define WIFI_SSID             "your-ssid"      /* leave empty -> Ethernet only */
#define WIFI_PASS             "your-password"

#define HTTP_PORT             80      /* Ethernet */
#define WIFI_HTTP_PORT        8080    /* Wi-Fi    */
#define HTTP_RECV_TIMEOUT_MS  (10 * 1000)
#define HTTP_BUF_SIZE         2048
```

`main.c` assembles a `wiz_NetInfo` from these and hands it to `wiznet_net_init()`, which applies it to the chip with `wizchip_setnetinfo()`.

### Web page

The page served to the browser is defined in `examples/http/inc/web_page.h` and answered for `/` and `/index.html`.

```cpp
#define index_page  "<!DOCTYPE html>"\
                    "<html lang=\"en\">"\
                    "<head>"\
                        "<meta charset=\"UTF-8\">"\
                        "<title>HTTP Server Example</title>"\
                    "</head>"\
                    "<body>"\
                        "<h1>Hello, World!</h1>"\
                    "</body>"\
                    "</html>"
```

### Running on Wi-Fi at the same time (optional)

`net_config.h` ships with the `WIFI_SSID` / `WIFI_PASS` placeholders. Replace them with your AP credentials and the same HTTP server also comes up on a Wi-Fi STA, as a sibling task at the same level as the Ethernet one:

```cpp
http_server_start("eth",  &net_eth_ops,  HTTP_PORT,      wiznet_net_is_up);
http_server_start("wifi", &net_wifi_ops, WIFI_HTTP_PORT, wifi_net_is_up);
```

Each interface gets its own task and its own buffer, so nothing is shared. The two use different ports because with the esp_eth backend they share one LwIP stack, where identical ports would clash on bind.

Clear `WIFI_SSID` to `""` when committing; an empty SSID skips Wi-Fi entirely so the example still builds and runs Ethernet-only for everyone else.

### Architecture

Same layout as `examples/loopback` and `examples/dhcp_dns`:

| Path | Role |
|------|------|
| `inc/net_config.h` | network identity, ports, timeouts, buffer size |
| `inc/web_page.h` | the page served as `/` and `/index.html` |
| `inc/http_server.h` | engine API |
| `src/http_server.c` | backend-neutral HTTP/1.1 server (BSD sockets via a vtable) |
| `main/main.c` | orchestration only: bring interfaces up, start the tasks |

Compared with the ioLibrary version this was ported from:

- `httpServer_run()`'s `Sn_SR` state machine (`SOCK_ESTABLISHED` / `SOCK_CLOSE_WAIT` / `SOCK_CLOSED` plus the manual re-listen) over a fixed list of hardware sockets collapses into a plain `accept()` loop;
- `httpServer_time_handler()` and its 1-second `esp_timer` disappear — the session timeout is just `SO_RCVTIMEO` on the socket;
- the `httpParser` / `httpUtil` content registry disappears too, since this example serves exactly one page.

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
I (525) http: [eth] waiting for link...
I (527) http: [eth] HTTP server listening on port 80
```

With Wi-Fi configured, the second server appears once DHCP has assigned an address:

```
I (xxxxx) wifi: got IP 192.168.11.7
I (xxxxx) http: [wifi] HTTP server listening on port 8080
```

![][link-run_socket_open]

Open a web browser and enter the device URL `http://192.168.11.2/` in the address bar (or `http://192.168.11.7:8080/` for the Wi-Fi server).
![][link-run_browser]

The served web page appears, showing the **Hello, World!** heading from `web_page.h`, and each request is logged:

```
I (xxxxx) http: [eth] GET /
I (xxxxx) http: [eth] GET /favicon.ico
```

![][link-run_webpage]

## Appendix

- **One connection at a time:** Each interface runs a single listener and serves connections sequentially, closing after every response (`Connection: close`). That is a choice this example makes to keep the engine small, not a limit of the chip: `accept()` has BSD semantics on the hardware sockets too, so the listener survives each connection and several could be open at once, up to the chip's eight hardware sockets (a listener that is serving a client occupies two of them). The ioLibrary version spread `httpServer_run()` across 4 hardware sockets instead. To serve concurrently here, hand each accepted fd to its own task rather than calling `serve_client()` inline.
- **Requests handled:** `GET` and `HEAD` for `/` and `/index.html` (a query string is ignored). Anything else gets `404 Not Found`; other methods get `501 Not Implemented`. Requests whose headers exceed `HTTP_BUF_SIZE` are truncated.
- **Session timeout:** `SO_RCVTIMEO` (`HTTP_RECV_TIMEOUT_MS`) bounds both `accept()` and `recv()`, so a task that never sees a client — or a client that connects and then goes silent — keeps looping instead of wedging.
- **1 ms tick:** `sdkconfig.defaults` sets `CONFIG_FREERTOS_HZ=1000`. The TOE poll loops (`wiztoe_accept` / `wiztoe_recv`) yield in 1 ms steps and count those steps for `SO_RCVTIMEO`; at the IDF default of 100 Hz a 1 ms delay truncates to 0 ticks, which busy-waits and starves the idle task.
- **W6300 QSPI mode:** Quad mode (4-bit) requires the extra D2/D3 lines wired and selected in `Component config -> WIZnet WSM Driver -> W6300 QSPI mode`. Single mode uses the same 4-wire wiring as W5500.

<!-- Link -->
[link-tera_term]: https://osdn.net/projects/ttssh2/releases/

[link-hardware]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/http/hardware.png
[link-config_main]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/http/config_main.png
[link-config_component]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/http/config_component.png
[link-config_wiz_toe]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/http/config_wiz_toe.png

[link-build_log]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/http/build_log.png
[link-run_socket_open]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/http/run_socket_open.png
[link-run_browser]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/http/run_browser.png
[link-run_webpage]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/http/run_webpage.png
