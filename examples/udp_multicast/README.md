# How to Test UDP Multicast Example

> **Verified on both chips.** This example was run on a W6300 (QSPI) and on a W5500 (standard SPI, XIAO ESP32-S3), over Ethernet and Wi-Fi in each case.

## Step 1: Prepare software

The following serial terminal program and UDP test tool are required for the UDP Multicast example test, download and install from below links.

- [Tera Term][link-tera_term]
- [Hercules][link-hercules]

## Step 2: Prepare hardware

1. Connect the WIZnet Ethernet chip (W5500 or W6300) to the ESP32-S3 board over SPI, following the pin table in [Step 3](#step-3-setup-udp-multicast-example).
2. Connect an Ethernet cable from the module's RJ45 port to your PC or network.
3. Connect the ESP32-S3 board to your PC with a USB cable.

![][link-hardware]

## Step 3: Setup UDP Multicast Example

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

Choose the WIZnet chip, and check the per-socket buffer size. SPI host, clock, and pins follow the selected chip automatically. In this example, SPI2 of the ESP32-S3 is used at 33 MHz.
![][link-config_wiz_toe]

> This example ships with **W6300** selected by default (`sdkconfig.defaults`). Switch to W5500 under `Component config -> WIZnet WSM Driver -> WIZnet chip` if needed.

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

Network identity, the group and the ports live in `examples/udp_multicast/inc/net_config.h`, the same way as in `examples/loopback`:

```cpp
#define NET_MAC_ADDR          {0x00, 0x08, 0xDC, 0x12, 0x34, 0x56}
#define NET_IP_ADDR           {192, 168, 11, 2}
#define NET_SUBNET_MASK       {255, 255, 255, 0}
#define NET_GATEWAY           {192, 168, 11, 1}
#define NET_DNS_ADDR          {8, 8, 8, 8}

#define WIFI_SSID             ""      /* empty -> Ethernet only */
#define WIFI_PASS             ""

#define MCAST_GROUP_IP        "224.0.0.5"
#define MCAST_GROUP_PORT      30000   /* Ethernet */
#define WIFI_MCAST_GROUP_PORT 30001   /* Wi-Fi    */
#define MCAST_BUF_SIZE        2048
```

`main.c` assembles a `wiz_NetInfo` from these and hands it to `wiznet_net_init()`, which applies it to the chip with `wizchip_setnetinfo()`.

`224.0.0.5` is the OSPF All-SPF-Routers group, the same one the WIZnet-PICO-C example uses. Any address in `224.0.0.0/4` works.

### How the group is joined

Everything the receive engine does — `socket()`, `bind()`, `recvfrom()` — is plain BSD through the vtable. Joining is the one step the two backends cannot express the same way, so it arrives as a function pointer and `main.c` picks the right one:

```cpp
#if CONFIG_WSM_DRIVER_SOCKET_WRAP
#define ETH_JOIN  mcast_join_toe      /* WIZnet hardware sockets */
#else
#define ETH_JOIN  mcast_join_bsd      /* esp_eth backend: software LwIP */
#endif

mcast_rx_start("eth",  &net_eth_ops,  ETH_JOIN,        ...);
mcast_rx_start("wifi", &net_wifi_ops, mcast_join_bsd,  ...);
```

**`mcast_join_bsd()`** is the ordinary one — bind, then ask:

```cpp
struct ip_mreq mreq = {
    .imr_multiaddr.s_addr = inet_addr(group),
    .imr_interface.s_addr = htonl(INADDR_ANY),
};
o->setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));
```

`struct ip_mreq` carries no port, so the group port is the port you bound — which is exactly what binding to a group's port means in BSD.

**`mcast_join_toe()`** cannot use that call, because the chip filters the group **in hardware** and derives the multicast MAC for `Sn_DHAR` from `Sn_DIPR` at the moment the socket opens. The group has to be in the registers *before* the open, and `bind()` has already opened it, so the join closes the hardware socket and reopens it with `Sn_MR_MULTI`:

```cpp
close(sn);
setSn_DHAR(sn, mac);          /* 01:00:5E + low 23 bits of the group (RFC 1112) */
setSn_DIPR(sn, group);
setSn_DPORT(sn, port);
socket(sn, Sn_MR_UDP, port, SF_MULTI_ENABLE);
```

Datagrams arriving during the reopen are lost. That is acceptable here: a join happens once at start-up, before any traffic is expected.

There is no `close()` before those register writes, and that is deliberate. ioLibrary's `socket()` closes the socket itself before reopening it, so one is not needed — and calling `close()` from this file would not reach ioLibrary's anyway. The component compiles its own sources with `-Dclose=wiz_close` so that ioLibrary's `close(uint8_t)` stops hijacking newlib's POSIX `close(int)`, and that definition does not extend to the example. A bare `close(sn)` here resolves to POSIX `close()`, which closes file **descriptor** `sn` — 0 being stdin.

That was in this file until a W5500 run found it. On a UART console the stray close of fd 0 passed unnoticed and multicast worked anyway, because `socket()`'s own close was doing the real work. On USB Serial/JTAG, where fds 0-2 are the console, it took the console down mid-join and the example looked like it had hung.

`SF_MULTI_ENABLE` belongs in the **flag** argument, not the protocol argument — `socket()` validates it there, and OR-ing it into the protocol gives `0x82`, which matches no protocol case at all.

### The one non-standard thing in this example

`mcast_join_toe()` is handed a BSD `fd` and needs the chip's socket number. It used to derive one from the other arithmetically:

```
fd == hardware socket number + LWIP_SOCKET_OFFSET     // no longer true
```

That was an internal rule of `wsm_driver`, never a published contract, and it **no longer holds**. Since `accept()` gained BSD semantics a listener hands its hardware socket to the accepted connection and relocates onto a free one, so descriptors and chip sockets drift apart; a raw socket reserved for ioLibrary's `DHCP_run()` shifts them too. The subtraction would still compile and would quietly address someone else's socket.

So the driver is asked instead:

```c
int sn = wiztoe_sn_of_fd(fd - mcast_lwip_socket_offset());
```

`wiztoe_sn_of_fd()` returns the hardware socket currently behind a descriptor, or -1 when there is none. The value is valid only until the next call that can move sockets around (`accept`, `close`, `listen`), so it is read immediately before use and never cached.

This whole path holds **only when `WSM_DRIVER_SOCKET_WRAP` is enabled**. With the esp_eth backend the same vtable is software LwIP: `fd` is a genuine LwIP socket with no hardware socket behind it at all, and `wiznet_toe.c` is not even built. That is what the `#if` in `main.c` is for.

The answer is still checked against the chip before anything is written. `bind()` has just opened this socket for UDP, so the chip must agree it is in `SOCK_UDP`:

```
E (xxx) mcast_join: socket N is not open for UDP (Sn_SR=0x..) — refusing to
        reopen it; it is not the socket behind this fd
```

Losing multicast is a far better outcome than silently reopening someone else's socket.

The alternative was to keep this in the component, where `setsockopt(IP_ADD_MEMBERSHIP)` could hide it and the example would be pure BSD. It was moved out deliberately: closing and reopening a socket is a decision about the application's own traffic, not something a `setsockopt()` should do behind the caller's back. `wiztoe_wrap.c` now answers `ENOPROTOOPT` for `IP_ADD_MEMBERSHIP` rather than pretending to support it.

### Architecture

Same layout as `examples/loopback`:

| Path | Role |
|------|------|
| `inc/net_config.h` | network identity, group, ports, buffer size |
| `inc/mcast_rx.h` · `src/mcast_rx.c` | backend-neutral receiver (BSD sockets via a vtable) |
| `inc/mcast_join.h` | the join seam: one function pointer, two implementations |
| `src/mcast_join_bsd.c` | join through LwIP — Wi-Fi, and Ethernet on esp_eth |
| `src/mcast_join_toe.c` | join by reopening the WIZnet hardware socket |
| `main/main.c` | orchestration only: bring interfaces up, start the tasks |

`mcast_rx.c` includes lwIP; `mcast_join_toe.c` includes ioLibrary's `socket.h`. Those two must never meet in one translation unit — both declare `close()`, with different signatures — which is why the join lives in its own files and `LWIP_SOCKET_OFFSET` reaches `mcast_join_toe.c` through a function rather than the macro.

### Running on Wi-Fi at the same time (optional)

Fill in `WIFI_SSID` and the same receiver also comes up on a Wi-Fi STA:

```cpp
mcast_rx_start("eth",  &net_eth_ops,  ETH_JOIN,       MCAST_GROUP_IP, MCAST_GROUP_PORT,      wiznet_net_is_up);
mcast_rx_start("wifi", &net_wifi_ops, mcast_join_bsd, MCAST_GROUP_IP, WIFI_MCAST_GROUP_PORT, wifi_net_is_up);
```

The two use different ports because with `SOCKET_WRAP=0` (esp_eth backend) they share one LwIP stack, where the same port would clash on bind. Leave `WIFI_SSID` empty when committing.

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

If flashing succeeds, the assigned IP and the joined multicast group appear in the terminal.

```
I (522) wiztoe_net: TOE up: 192.168.11.2 (WIZnet hardware TCP/IP)
I (525) mcast_rx: [eth] waiting for link...
I (528) mcast_rx: [eth] listening to 224.0.0.5:30000
```

With Wi-Fi configured, the second receiver appears once DHCP has assigned an address:

```
I (181336) esp_netif_handlers: sta ip: 192.168.11.7, mask: 255.255.255.0, gw: 192.168.11.1
I (181337) wifi: got IP 192.168.11.7
I (181424) mcast_rx: [wifi] listening to 224.0.0.5:30001
```

That timestamp is not a typo — see [DHCP can take minutes on some access points](#dhcp-can-take-minutes-on-some-access-points) below. The Ethernet receiver is unaffected and starts within a second.

![][link-run_socket_open]

Open Hercules and select the **UDP** tab. Set the module IP to the multicast group `224.0.0.5`, set both **Port** and **Local port** to `30000`, then click **Listen** so Hercules joins the same group. Make sure your PC's active network adapter is on the same subnet (`192.168.11.x`) as the device.
![][link-run_hercules]

Send any data from Hercules to the multicast group. The device receives the datagram and prints it in the serial monitor, confirming multicast reception works.

```
I (xxxxx) mcast_rx: [eth] 5 bytes from 192.168.11.4: hello
```

This example only receives. It never answers, so Hercules' *Received data* pane stays empty apart from its own socket messages — the serial monitor is the only place a result shows up.

With both interfaces up, the same sender reaches both receivers on their own ports:

```
# to 224.0.0.5:30000
I (745573) mcast_rx: [eth]  57 bytes from 192.168.11.4: 123123123123123...

# to 224.0.0.5:30001
I (811824) mcast_rx: [wifi] 50 bytes from 192.168.11.4: mcast test #1 from 192.168.11.4 to 224.0.0.5:30001
I (813065) mcast_rx: [wifi] 50 bytes from 192.168.11.4: mcast test #2 from 192.168.11.4 to 224.0.0.5:30001
```

The Ethernet line comes off the W6300's hardware group filter, the Wi-Fi line off LwIP IGMP, and `mcast_rx.c` is identical for both.

![][link-run_multicast]

To check that the hardware filter really is filtering, send to a *different* group (say `224.0.0.9`) on the same port — the device should stay silent.

### DHCP can take minutes on some access points

With Wi-Fi configured, the station associates in well under a second but the DHCP lease can take anywhere from two seconds to three minutes. It always arrives eventually, and Ethernet is never affected. This is not specific to multicast — the other Wi-Fi examples behave the same way on the same AP.

The cause is Wi-Fi power save meeting an access point that buffers broadcasts poorly. IDF defaults to `WIFI_PS_MIN_MODEM`, where the station only wakes for DTIM beacons, and a DHCP OFFER is a broadcast: the AP has to hold it for sleeping stations and release it at the next DTIM. One AP here (DTIM 3, 102.4 ms beacons) frequently did not, and the lease arrived only when a retry happened to line up. Measured over five boots each, with association and the block-ack completing inside a second either way:

| | DHCP lease arrives at |
|---|---|
| `WIFI_PS_MIN_MODEM` (default) | 2.5 s, 29 s, 82 s, 86 s, 181 s |
| `WIFI_PS_NONE` | 2.36 s, 2.35 s, 2.16 s, 2.14 s, 2.20 s |

If you hit it, turn power save off after `wifi_net_init()`:

```cpp
#include "esp_wifi.h"

wifi_net_init(WIFI_SSID, WIFI_PASS);
esp_wifi_set_ps(WIFI_PS_NONE);
```

It is left in the example rather than the component on purpose: whether it is needed depends on the access point, most do not need it, and the radio costs real power in a product. A static Wi-Fi address avoids the problem entirely.

Two explanations that were tested and ruled out. It is **not** the two interfaces sharing a subnet — moving the Ethernet netif to 192.168.20.0/24 changed nothing (90.8 s and 29.3 s on the first two boots). And it is **not** antenna or signal: RSSI measured -9 to -28 dBm throughout, unicast traffic was never affected, broadcasts go out at the lowest basic rate and are more robust than unicast anyway, and disabling power save fixed it with the antenna untouched.

### If nothing arrives: check which adapter Windows sends multicast on

Multicast has no destination host to route toward, so Windows picks the egress adapter purely by interface metric — and a VirtualBox, VMware, WSL or Hyper-V virtual adapter usually has a *lower* metric than your real NIC, which silently wins. Hercules has no way to choose the interface, so the datagram leaves on the virtual adapter and the device never sees it. The device is fine; the packet never reached the wire.

Check the ordering:

```powershell
Get-NetRoute -DestinationPrefix '224.0.0.0/4' |
    Select-Object InterfaceAlias, InterfaceMetric | Sort-Object InterfaceMetric
```

If your Ethernet is not first, give it a lower metric (needs an elevated shell):

```powershell
Set-NetIPInterface -InterfaceIndex <your-ethernet-index> -InterfaceMetric 5
# undo later with: Set-NetIPInterface -InterfaceIndex <idx> -AutomaticMetric Enabled
```

To sidestep the whole issue, send from PowerShell instead — binding the socket to the wired address forces the interface:

```powershell
$local = New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Parse("192.168.11.4"), 0)
$c = New-Object System.Net.Sockets.UdpClient($local)
$b = [System.Text.Encoding]::ASCII.GetBytes("hello multicast")
$c.Send($b, $b.Length, "224.0.0.5", 30000)
$c.Close()
```

Wireshark on the wired adapter with the filter `ip.addr == 224.0.0.5` settles it either way: you should see the device's own IGMPv2 Membership Reports (proof the chip joined) alongside your outgoing datagrams.

## Appendix

- **Multicast group and IGMP:** Any host on the same network that sends to this group:port is received, and multiple receivers can join the same group at once. On the WIZnet chip the filtering happens in hardware, so multicast traffic for other groups never wakes the MCU. On the Wi-Fi side LwIP does the filtering and sends IGMP membership reports, which needs `CONFIG_LWIP_IGMP=y` (pinned in `sdkconfig.defaults`).
- **W6300 QSPI mode:** Quad mode (4-bit) requires the extra D2/D3 lines wired and selected in `Component config -> WIZnet WSM Driver -> W6300 QSPI mode`. Single mode uses the same 4-wire wiring as W5500.

<!-- Link -->
[link-tera_term]: https://osdn.net/projects/ttssh2/releases/
[link-hercules]: https://www.hw-group.com/software/hercules-setup-utility

[link-hardware]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/udp_multicast/hardware.png
[link-config_main]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/udp_multicast/config_main.png
[link-config_component]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/udp_multicast/config_component.png
[link-config_wiz_toe]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/udp_multicast/config_wiz_toe.png

[link-build_log]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/udp_multicast/build_log.png
[link-run_socket_open]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/udp_multicast/run_socket_open.png
[link-run_hercules]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/udp_multicast/run_hercules.png
[link-run_multicast]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/udp_multicast/run_multicast.png
