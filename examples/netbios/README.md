# How to Test NetBIOS Example

## Step 1: Prepare software

The following serial terminal program is required for the NetBIOS example test, download and install from below links.

- [Tera Term][link-tera_term]

The PC-side test uses `examples/netbios/test.py`, a self-contained NetBIOS name lookup script that ships with this example. It needs **Python 3.7 or later** and nothing else — it uses only the standard library, so there is nothing to `pip install`. The ESP-IDF environment already provides a suitable Python.

Windows users can additionally use the built-in `ping` and `nbtstat` commands, but those depend on the OS NetBIOS resolver being enabled; `test.py` does not and works the same on Windows, Linux and macOS.

## Step 2: Prepare hardware

1. Connect the WIZnet Ethernet chip (W5500 or W6300) to the ESP32-S3 board over SPI, following the pin table in [Step 3](#step-3-setup-netbios-example).
2. Connect an Ethernet cable from the module's RJ45 port to your PC or network.
3. Connect the ESP32-S3 board to your PC with a USB cable.

![][link-hardware]

## Step 3: Setup NetBIOS Example

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

Under `Component config -> WIZnet WSM Driver -> Network backend` there are two
choices, and the same `src/netbios.c` runs on either:

| Backend | What carries the traffic | Which symbols the responder calls |
|---------|--------------------------|-----------------------------------|
| **TOE (hardware TCP/IP)** *(default)* | the chip's own TCP/IP stack | `__wrap_lwip_*` — `-Wl,--wrap` redirects the engine's `lwip_*` calls to the hardware sockets (`wiztoe_wrap.c`) |
| **esp_eth MACRAW + software LwIP** | the ESP32-S3's LwIP stack, chip as a MAC | `lwip_*` — no wrap, the calls run over software LwIP |

`netbios.c` has no `#if` for this. It calls sockets through the component's
`net_sock_ops_t` vtable and the LINKER picks the target; the Wi-Fi vtable
(`net_wifi_ops`) binds `__real_lwip_*` when the wrap is active, so Wi-Fi always
reaches the real software stack.

### Network and NetBIOS configuration

All of the example's settings live in `examples/netbios/inc/net_config.h`.

```c
/* ---- static network identity (wsm_driver style: wiz_NetInfo byte arrays) ---- */
#define NET_MAC_ADDR          {0x00, 0x08, 0xDC, 0x12, 0x34, 0x56}  /* WIZnet OUI */
#define NET_IP_ADDR           {192, 168, 11, 2}
#define NET_SUBNET_MASK       {255, 255, 255, 0}
#define NET_GATEWAY           {192, 168, 11, 1}
#define NET_DNS_ADDR          {8, 8, 8, 8}

/* ---- Wi-Fi STA config (leave WIFI_SSID empty to run Ethernet-only) ---- */
#define WIFI_SSID             "your-ssid"
#define WIFI_PASS             "your-password"

/* ---- NetBIOS responder config ---- */
#define NETBIOS_NAME          "WIZNET"        /* Ethernet (WIZnet chip) */
#define WIFI_NETBIOS_NAME     "WIZNETWIFI"    /* Wi-Fi STA */
#define NETBIOS_PORT          137             /* NetBIOS name service (UDP) */
#define NETBIOS_BUF_SIZE      512             /* max NetBIOS datagram handled */
#define NETBIOS_RECV_TIMEOUT_MS (5 * 1000)    /* bounds recvfrom() on a quiet network */
#define NETBIOS_NAME_TTL      10              /* TTL (s) advertised in the response */
```

The board answers name queries for `NETBIOS_NAME` on Ethernet and, when Wi-Fi is
configured, for `WIFI_NETBIOS_NAME` on the Wi-Fi STA. The two names must differ:
NetBIOS names have to be unique on the segment, so a host that can see both
interfaces does not get two answers for one name. Names are case-insensitive and
at most 15 characters.

### Source layout

| File | Role |
|------|------|
| `main/main.c` | orchestration only: bring both interfaces up, start one responder task per interface |
| `src/netbios.c` | the backend-neutral responder — every network call goes through `ops->sock->…` |
| `src/eth_netbios.c` | Ethernet hooks: the address to answer with (`wizchip_getnetinfo` on TOE, `esp_netif` on ETH) |
| `src/netif_netbios.c` | Wi-Fi hooks (`esp_netif`), shared with the ETH backend |
| `inc/net_config.h` | all example configuration |
| `test.py` | PC-side NetBIOS name lookup used in [Step 5](#step-5-upload-and-run) |

The ioLibrary socket API (`socket(sn, Sn_MR_UDP, …)` plus the `getSn_SR()` state
machine the original example used) is not used: it drives the chip's socket
registers directly, so `--wrap` has nothing to intercept and one source could not
serve both backends.

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

If flashing succeeds, the assigned IP appears and the responder binds UDP port 137.

```
I (xxx) netbios: [eth] waiting for link...
I (xxx) netbios: [eth] NetBIOS responder for "WIZNET" on UDP port 137
```

![][link-run_socket_open]

### Look the name up with `test.py`

On a PC connected to the same network, run the lookup script that ships with this example. `-U` is the address to send the query to (the device), and the positional argument is the NetBIOS name to ask for:

```bash
cd examples/netbios
python test.py -U 192.168.11.2 WIZNET
```

The script sends one NBNS name query to UDP port 137 and prints the address the device answered with, its name type, and its node type:

```
192.168.11.2 WIZNET<00> UNIQUE B-node
```

Seeing the device IP here confirms the NetBIOS responder works. If the name does not belong to this interface the device stays silent and the script reports a timeout instead:

```
Lookup failed: No response from 192.168.11.2:137 within 2s. Attempt 2/2
```

Useful options:

| Option | Meaning |
|--------|---------|
| `-U`, `--server` | IP address the query is sent to (required) |
| `--suffix` | NetBIOS service suffix, in hex. Default `00` (workstation) |
| `--timeout` | Seconds to wait for each response. Default `2` |
| `--retries` | Maximum number of requests. Default `2` |

With Wi-Fi configured, query the Wi-Fi responder the same way — its own name, at the address the STA was given:

```bash
python test.py -U 192.168.11.7 WIZNETWIFI
```

When a name query arrives, the serial monitor logs the name that was asked for and the address that was answered with:

```
I (xxx) netbios: [eth] 50 bytes from 192.168.11.100:54321
I (xxx) netbios: [eth] name query for "WIZNET" (we are "WIZNET")
I (xxx) netbios: [eth] "WIZNET" -> 192.168.11.2
I (xxx) netbios: [eth] answered 62/62 bytes
```

![][link-run_serial]

### Windows built-in commands (alternative)

On Windows the same responder can be reached through the OS NetBIOS resolver. Open a Command Prompt and run:

```bash
ping WIZNET
```

The name resolves to the device IP `192.168.11.2` and replies are returned.
![][link-run_ping]

Or query the NetBIOS name table for the device IP directly:

```bash
nbtstat -A 192.168.11.2
```

The device's registered name `WIZNET` is listed in the returned name table.
![][link-run_nbtstat]

Both commands depend on NetBIOS over TCP/IP being enabled on the PC's adapter, so if they turn up nothing while `test.py` succeeds, the PC — not the device — is what is filtering the lookup.

## Appendix

- **NetBIOS name:** The board responds only to the name in `NETBIOS_NAME` (`WIZNET`), and to `WIFI_NETBIOS_NAME` on the Wi-Fi STA when one is configured. NetBIOS names are case-insensitive and limited to 15 characters.
- **Wi-Fi is optional:** with `WIFI_SSID` empty the example runs Ethernet-only and the Wi-Fi responder is never started.
- **Same subnet required:** NetBIOS name service uses UDP broadcast on port 137, so the PC and the device must be on the same local subnet (`192.168.11.x` here). Name resolution does not cross routers.
- **`test.py` queries by address, not by broadcast:** it sends the name query straight to the address given with `-U`, so the device IP has to be known up front (it is `NET_IP_ADDR`, `192.168.11.2`, unless you changed it). That is also why it works where `ping <name>` does not — no OS-level NetBIOS resolver is involved. The device answers a unicast query exactly as it answers a broadcast one.
- **What the reply says:** the responder always answers `UNIQUE` / `B-node` (name flags `0x0000`) with a TTL of `NETBIOS_NAME_TTL` seconds, and echoes back whichever suffix was asked for — so `--suffix 20` resolves to the same address as the default `00`.
- **W6300 QSPI mode:** Quad mode (4-bit) requires the extra D2/D3 lines wired and selected in `Component config -> WIZnet WSM Driver -> W6300 QSPI mode`. Single mode uses the same 4-wire wiring as W5500.

<!-- Link -->
[link-tera_term]: https://osdn.net/projects/ttssh2/releases/

[link-hardware]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/netbios/hardware.png
[link-config_main]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/netbios/config_main.png
[link-config_component]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/netbios/config_component.png
[link-config_wiz_toe]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/netbios/config_wiz_toe.png

[link-build_log]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/netbios/build_log.png
[link-run_socket_open]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/netbios/run_socket_open.png
[link-run_ping]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/netbios/run_ping.png
[link-run_serial]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/netbios/run_serial.png
[link-run_nbtstat]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/netbios/run_nbtstat.png
