# How to Test MQTT Example

## Step 1: Prepare software

The following serial terminal program and MQTT broker/client are required for the MQTT example test, download and install from below links.

- [Tera Term][link-tera_term]
- [mosquitto][link-mosquitto]
- [MQTTX][link-mqttx]

Run a broker (mosquitto) on your PC, and optionally use an MQTT client (MQTTX) to publish and subscribe alongside the device.

## Step 2: Prepare hardware

1. Connect the WIZnet Ethernet chip (W5500 or W6300) to the ESP32-S3 board over SPI, following the pin table in [Step 3](#step-3-setup-mqtt-example).
2. Connect an Ethernet cable from the module's RJ45 port to your PC or network.
3. Connect the ESP32-S3 board to your PC with a USB cable.

![][link-hardware]

## Step 3: Setup MQTT Example

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

| menuconfig choice | What `src/mqtt.c`'s `ops->socket()` / `ops->recv()` / `ops->send()` resolve to |
|-------------------|-------------------------------------------------------------------------------|
| **TOE (hardware TCP/IP)** *(default)* | `__wrap_lwip_*` — `-Wl,--wrap` redirects lwIP's BSD entry points to the WIZnet chip's hardware sockets (`port/ioLibrary_Driver/src/wiztoe_wrap.c`) |
| **esp_eth MACRAW + software LwIP** | `lwip_*` — the chip runs as a plain SPI Ethernet MAC and the ESP32-S3's software LwIP owns TCP/IP |

The engine never contains an `#if`: it is handed the component's `net_eth_ops` vtable (plain `lwip_*`, which the linker aims at whichever backend is selected), and Wi-Fi is handed `net_wifi_ops`, which binds `__real_lwip_*` when the wrap is active so Wi-Fi always reaches the real software LwIP.

The ioLibrary MQTT client (`Internet/MQTT`: `MQTTClient.c`, `mqtt_interface.c`) is **not** used. Its `Network` struct binds `mqttread`/`mqttwrite` to a hardware socket number and calls ioLibrary's own `socket()` / `send()` / `recv()`, so `--wrap` has nothing to intercept and one source could not serve both backends — `src/mqtt.c` speaks the MQTT 3.1.1 wire format over `ops->recv()` / `ops->send()` instead.

### Network configuration

Network identity, broker, and topics live in `examples/mqtt/inc/net_config.h`, the same way as in `examples/loopback`, `examples/dhcp_dns` and `examples/http`:

```cpp
#define NET_MAC_ADDR            {0x00, 0x08, 0xDC, 0x12, 0x34, 0x56}
#define NET_IP_ADDR             {192, 168, 11, 2}
#define NET_SUBNET_MASK         {255, 255, 255, 0}
#define NET_GATEWAY             {192, 168, 11, 1}
#define NET_DNS_ADDR            {8, 8, 8, 8}

#define WIFI_SSID               "your-ssid"      /* leave empty -> Ethernet only */
#define WIFI_PASS               "your-password"
```

`main.c` assembles a `wiz_NetInfo` from these and hands it to `wiznet_net_init()`, which applies it to the chip with `wizchip_setnetinfo()`.

### MQTT broker configuration

Point the broker address at the PC running mosquitto. The example connects on the standard MQTT port 1883.

```cpp
#define MQTT_BROKER_IP          "192.168.11.100"
#define MQTT_BROKER_PORT        1883
```

Credentials, client IDs, topics, and keep-alive are in the same file:

```cpp
#define MQTT_CLIENT_ID_ETH      "esp32s3-wiz-toe-eth"
#define MQTT_CLIENT_ID_WIFI     "esp32s3-wiz-toe-wifi"
#define MQTT_USERNAME           "wiznet"
#define MQTT_PASSWORD           "0123456789"
#define MQTT_KEEP_ALIVE_S       60

#define MQTT_PUBLISH_TOPIC_ETH    "publish_topic/eth"
#define MQTT_PUBLISH_TOPIC_WIFI   "publish_topic/wifi"
#define MQTT_PUBLISH_PAYLOAD_ETH  "Hello, World! from eth"
#define MQTT_PUBLISH_PAYLOAD_WIFI "Hello, World! from wifi"
#define MQTT_PUBLISH_PERIOD_MS    (10 * 1000)
#define MQTT_SUBSCRIBE_TOPIC_ETH  "subscribe_topic/eth"
#define MQTT_SUBSCRIBE_TOPIC_WIFI "subscribe_topic/wifi"
```

Topics are **per interface**, not per example. Sharing them would make the two publishes indistinguishable at the broker and would deliver every inbound message to both clients. The common prefix is kept so one wildcard still covers both: subscribe to `publish_topic/#` to watch them together, or to `publish_topic/eth` to watch one.

In publish mode the Ethernet client sends `Hello, World! from eth` to `publish_topic/eth` every 10 seconds, and the Wi-Fi client sends `Hello, World! from wifi` to `publish_topic/wifi`.

### MQTT mode

Select the MQTT mode in menuconfig under **MQTT Example Configuration -> MQTT mode**. The default is **Publish and subscribe**.

- **Publish only** — publish to `publish_topic/<interface>` every 10 seconds.
- **Subscribe only** — subscribe to `subscribe_topic/<interface>` and print arriving messages.
- **Publish and subscribe** — do both at once (default).

The mode is applied in `main.c` by filling in — or leaving out — `pub_topic` / `sub_topic` in the `mqtt_config_t`. A `NULL` topic is how the engine is told to skip that half of the job, so `src/mqtt.c` itself contains no `#if` for the mode either.

### Running on Wi-Fi at the same time (optional)

`net_config.h` ships with the `WIFI_SSID` / `WIFI_PASS` placeholders. Replace them with your AP credentials and the same MQTT client also comes up on a Wi-Fi STA, as a sibling task at the same level as the Ethernet one:

```cpp
mqtt_client_start("eth",  &net_eth_ops,  &cfg, wiznet_net_is_up);
cfg.client_id   = MQTT_CLIENT_ID_WIFI;
cfg.pub_topic   = MQTT_PUBLISH_TOPIC_WIFI;
cfg.pub_payload = MQTT_PUBLISH_PAYLOAD_WIFI;
cfg.sub_topic   = MQTT_SUBSCRIBE_TOPIC_WIFI;
mqtt_client_start("wifi", &net_wifi_ops, &cfg, wifi_net_is_up);
```

Each interface gets its own task, its own buffers, and its own broker session. Everything that identifies the client on the broker is swapped, not just the ID: the ID because a broker closes the older session when a second client connects with the same one, and the topics so the two publishes stay apart and an inbound message reaches only the interface it was addressed to. (In `main.c` the topic assignments sit under the same `#ifdef`s as the initializer, so a topic the MQTT mode leaves out stays `NULL` for both clients.)

Clear `WIFI_SSID` to `""` when committing; an empty SSID skips Wi-Fi entirely so the example still builds and runs Ethernet-only for everyone else.

### Architecture

Same layout as `examples/loopback`, `examples/dhcp_dns` and `examples/http`:

| Path | Role |
|------|------|
| `inc/net_config.h` | network identity, broker, credentials, topics, timeouts, buffer size |
| `inc/mqtt.h` | engine API (`mqtt_config_t`, `mqtt_client_start`) |
| `src/mqtt.c` | backend-neutral MQTT 3.1.1 client (BSD sockets via a vtable) |
| `main/Kconfig.projbuild` | the MQTT mode choice |
| `main/main.c` | orchestration only: bring interfaces up, start the tasks |

Compared with the ioLibrary version this was ported from:

- `mqtt_interface.c`'s `Network` struct (`mqttread` / `mqttwrite` bound to a hardware socket number) disappears — the connection is a plain fd from `ops->socket()`;
- `MQTTClient.c`'s 1 ms `MilliTimer_Handler()` `esp_timer` tick disappears too. Keep-alive deadlines are compared against `esp_timer_get_time()` when the loop comes round, and `recv()` is bounded by `SO_RCVTIMEO` instead of being polled;
- the `MQTTPacket` serializer/deserializer collapses into the encode/decode helpers in `src/mqtt.c`, because this client only needs CONNECT, SUBSCRIBE, PUBLISH at QoS 0, PINGREQ and their acknowledgements;
- the client protocol level moves from MQTT 3.1 (`MQTTVersion = 3`) to **MQTT 3.1.1** (protocol level 4), which is what current brokers expect.

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

Before flashing, make sure a broker is running. On the PC, start mosquitto so it listens on port 1883:

```bash
mosquitto -v
```

If flashing succeeds, the assigned IP, the broker connection, and the subscription appear in the terminal.

```
I (522) wiztoe_net: TOE up: 192.168.11.2 (WIZnet hardware TCP/IP)
I (525) mqtt: [eth] waiting for link...
I (642) mqtt: [eth] TCP connected to 192.168.11.100:1883
I (655) mqtt: [eth] MQTT connected as "esp32s3-wiz-toe-eth"
I (661) mqtt: [eth] subscribed to "subscribe_topic/eth"
```

With Wi-Fi configured, the second client appears once DHCP has assigned an address:

```
I (xxxxx) wifi: got IP 192.168.11.7
I (xxxxx) mqtt: [wifi] MQTT connected as "esp32s3-wiz-toe-wifi"
I (xxxxx) mqtt: [wifi] subscribed to "subscribe_topic/wifi"
```

![][link-run_socket_open]

In **Publish** mode, each client publishes to its own topic every 10 seconds and logs each send.

```
I (xxxxx) mqtt: [eth] published "Hello, World! from eth" to publish_topic/eth
I (xxxxx) mqtt: [wifi] published "Hello, World! from wifi" to publish_topic/wifi
```

Subscribe to those topics from your MQTT client to watch the messages arrive. Using MQTTX, create a connection to the broker, then subscribe to `publish_topic/#` to see both interfaces at once (or `publish_topic/eth` for just one).
![][link-run_subscribe]

In **Subscribe** mode, each client subscribes to its own topic. Publish to one of them from your MQTT client (or `mosquitto_pub`), and only that interface prints the topic and payload.

```bash
mosquitto_pub -h 192.168.11.100 -t subscribe_topic/eth -m "hello from PC"
```

```
I (xxxxx) mqtt: [eth] subscribe_topic/eth : hello from PC
```

![][link-run_publish]

## Appendix

- **MQTT mode:** `Publish and subscribe` (default) runs both paths at once, so each client publishes to `publish_topic/<interface>` while also receiving on `subscribe_topic/<interface>`. Change it under `MQTT Example Configuration -> MQTT mode`.
- **Authentication:** The example connects with username `wiznet` / password `0123456789`. If your broker enforces different credentials, update `MQTT_USERNAME` / `MQTT_PASSWORD` in `inc/net_config.h`. Leave `MQTT_USERNAME` empty for an anonymous broker — MQTT 3.1.1 forbids a password without a username, so the password is then dropped as well.
- **QoS:** Publishes go out at QoS 0 and the subscription is requested at QoS 0, so there is no delivery bookkeeping to keep. An inbound QoS 1 publish is still answered with a PUBACK; QoS 2 is not supported.
- **Keep-alive and reconnect:** A PINGREQ goes out after half of `MQTT_KEEP_ALIVE_S` of silence from this side. If the broker sends nothing for 1.5 keep-alive intervals the session is considered dead. Either way — dropped TCP, refused CONNECT, or a silent broker — the task closes the socket, waits `MQTT_RECONNECT_MS`, and builds the session again.
- **Responsiveness:** `SO_RCVTIMEO` is `MQTT_POLL_MS` (1 s). That is how often the loop gets a turn to publish, ping, or notice a dead link, and therefore also the granularity of `MQTT_PUBLISH_PERIOD_MS`.
- **Packet size:** `MQTT_BUF_SIZE` (2 KB) bounds both the packets this client builds and the ones it will inspect. A larger inbound publish is consumed and dropped with a warning, so the stream stays in sync.
- **1 ms tick:** `sdkconfig.defaults` sets `CONFIG_FREERTOS_HZ=1000`. The TOE poll loop (`wiztoe_recv`) yields in 1 ms steps and counts those steps for `SO_RCVTIMEO`; at the IDF default of 100 Hz a 1 ms delay truncates to 0 ticks, which busy-waits and starves the idle task.
- **W6300 QSPI mode:** Quad mode (4-bit) requires the extra D2/D3 lines wired and selected in `Component config -> WIZnet WSM Driver -> W6300 QSPI mode`. Single mode uses the same 4-wire wiring as W5500.

<!-- Link -->
[link-tera_term]: https://osdn.net/projects/ttssh2/releases/
[link-mosquitto]: https://mosquitto.org/download/
[link-mqttx]: https://mqttx.app/

[link-hardware]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/mqtt/hardware.png
[link-config_main]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/mqtt/config_main.png
[link-config_component]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/mqtt/config_component.png
[link-config_wiz_toe]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/mqtt/config_wiz_toe.png

[link-build_log]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/mqtt/build_log.png
[link-run_socket_open]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/mqtt/run_socket_open.png
[link-run_subscribe]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/mqtt/run_subscribe.png
[link-run_publish]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/mqtt/run_publish.png
