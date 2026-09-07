# How to Test TFTP Example

> **Verified on both chips.** This example was run on a W6300 (QSPI) and on a W5500 (standard SPI, XIAO ESP32-S3), over Ethernet and Wi-Fi in each case.

## Step 1: Prepare software

The following serial terminal program and TFTP server are required for the TFTP example test, download and install from the links below.

- [Tera Term][link-tera_term]
- [Tftpd64][link-tftpd64]

> **Note:** Tftpd64 is a TFTP server. Do not confuse it with FileZilla (FTP) — TFTP and FTP are different protocols.

## Step 2: Prepare hardware

1. Connect the WIZnet Ethernet chip (W5500 or W6300) to the ESP32-S3 board over SPI, following the pin table in [Step 3](#step-3-setup-tftp-example).
2. Connect an Ethernet cable from the module's RJ45 port to your PC or network.
3. Connect the ESP32-S3 board to your PC with a USB cable.

![][link-hardware]

## Step 3: Setup TFTP Example

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

Network identity, the server and the file live in `examples/tftp/inc/net_config.h`, the same way as in `examples/loopback`:

```cpp
#define NET_MAC_ADDR          {0x00, 0x08, 0xDC, 0x12, 0x34, 0x56}
#define NET_IP_ADDR           {192, 168, 11, 2}
#define NET_SUBNET_MASK       {255, 255, 255, 0}
#define NET_GATEWAY           {192, 168, 11, 1}
#define NET_DNS_ADDR          {8, 8, 8, 8}

#define WIFI_SSID             ""      /* empty -> Ethernet only */
#define WIFI_PASS             ""

#define TFTP_SERVER_IP        "192.168.11.4"        /* your PC */
#define TFTP_FILE_NAME        "tftp_test_file.txt"
#define TFTP_PREVIEW_BYTES    64
```

`main.c` assembles a `wiz_NetInfo` from these and hands it to `wiznet_net_init()`, which applies it to the chip with `wizchip_setnetinfo()`. `TFTP_SERVER_IP` must be on the same subnet as the device.

The device acts as a TFTP **client**: it sends a read request and reports what came back.

### Architecture

Same layout as `examples/loopback`:

| Path | Role |
|------|------|
| `inc/net_config.h` | network identity, server, file name |
| `inc/tftp_client.h` | client API |
| `inc/tftp_core.h` · `src/tftp_core.c` | the ioLibrary TFTP protocol, carried here |
| `inc/tftp_transport.h` · `src/tftp_transport.c` | the network seam |
| `src/tftp_client.c` | drives the protocol, storage hook, retransmission tick |
| `main/main.c` | orchestration only: bring the interface up, start the task |

The protocol implementation is ioLibrary's, copied into the example rather than used from `third_party`, with its public symbols renamed `TFTP_*` → `tftpc_*` so it does not clash with the component's own copy. Only four functions were changed: it reaches the network exclusively through `tftp_transport.h`, whose BSD implementation calls the component's `net_sock_ops_t` vtable. That is also what keeps lwIP out of `tftp_core.c` — ioLibrary's `netutil.h` declares `inet_addr`, `htons`, `ntohs` and friends with different signatures from lwIP's, so the two must never be seen together.

To run the transfer over Wi-Fi instead, fill in `WIFI_SSID` and set `TFTP_OVER_WIFI` to 1 in `main.c`. Unlike the other converted examples this one does not run both interfaces at once: `tftp_core.c` keeps its state in globals, so two concurrent transfers would share it.

Both paths were verified against Tftpd64 on the same 192.168.11.0/24 LAN, and the same file arrives either way — the transport swap is the only difference:

```
# TFTP_OVER_WIFI 0 -- W6300 hardware sockets
I (433)  wiztoe_net: TOE up: 192.168.11.2 (WIZnet hardware TCP/IP)
I (434)  tftp: [eth] requesting "tftp_test_file.txt" from 192.168.11.4
>> TFTP RRQ : FileName(tftp_test_file.txt), Mode(octet)
>> TFTP RRQ : FileName(tftp_test_file.txt), Mode(octet)     <- ARP retransmission
I (6488) tftp: [eth] "tftp_test_file.txt" received: 1461 bytes in 3 blocks

# TFTP_OVER_WIFI 1 -- software LwIP on the Wi-Fi netif
I (2180) esp_netif_handlers: sta ip: 192.168.11.7, mask: 255.255.255.0, gw: 192.168.11.1
I (2234) tftp: [wifi] requesting "tftp_test_file.txt" from 192.168.11.4
I (2594) tftp: [wifi] "tftp_test_file.txt" received: 1461 bytes in 3 blocks
```

The doubled RRQ is the retransmission timer doing its job, and it is specific to the W6300: the chip resolves the server's MAC by ARP inside `sendto()`, and the first attempt to a cold peer times out. A W5500 on the same LAN sends the first RRQ straight through, so its Ethernet log shows only one. Wi-Fi never retries on either chip, because lwIP queues the packet behind its own ARP request instead of failing the send.

Note that the Wi-Fi station needs a route to the TFTP server. Here the AP bridges onto the same LAN as the PC, so the board's DHCP address (192.168.11.7) and the server (192.168.11.4) share a subnet. On an isolated AP, point `TFTP_SERVER_IP` at an address the station can actually reach.

### Fixes carried against the upstream implementation

Three defects in the ioLibrary original are corrected in this copy. All three are in `third_party` too, so they are worth reporting upstream.

- **Received data was discarded.** `save_data()` sits behind `#ifdef F_STORAGE`, which was never defined, so every block was acknowledged and thrown away — a transfer reported success without the file being looked at. `F_STORAGE` is enabled here and `tftp_client.c` implements the hook.
- **The receive error path was dead.** `tftpc_run()` declared `uint16_t len` and then tested `len < 0`, so the `-1` returned when no packet arrived became 65535 and a phantom 65 KB packet was handed to the parser. `len` is now `int`.
- **The retransmission timer never ran.** The tick that advances it lives in `data_process_count_handle()`, which nothing calls. `tftp_client.c` runs a 1 s `esp_timer` instead. This matters in practice: the first packet to a new peer usually fails while the chip resolves the peer's MAC by ARP, and without a working timer the client would wait forever.

## Step 4: Configure Tftpd64

Open **Tftpd64** and select the **Tftp Server** tab.

Configure the following items:

| Setting                            | Value                                                      |
| ---------------------------------- | ---------------------------------------------------------- |
| Current Directory / Base Directory | Folder that contains `tftp_test_file.txt` (e.g. `C:\tftp`) |
| Server interfaces                  | Your PC's Ethernet IP address (e.g. `192.168.11.4`)        |
| TFTP Security                      | **Standard** or **Read Only**                              |

Create the test file in the base directory. For example, create the following file:

```text
C:\tftp\tftp_test_file.txt
```

Any file content is fine.

> **Important:** The `Server interfaces` field must not be set to `::1` or `127.0.0.1`. Those are loopback interfaces and cannot be accessed by the WIZnet device. Select the actual PC Ethernet interface IP address, such as `192.168.11.4`.

> **Note:** If the device reports `File not found or No Access`, first check that the file is visible from Tftpd64 by pressing **Show Dir**. The file name must exactly match `TFTP_SERVER_FILE_NAME` in the source code.


## Step 5: Build

After completing the setup, build the project.

```bash
idf.py build
```

![][link-build_log]

## Step 6: Upload and Run

Flash the firmware and open the serial monitor. Replace the port with your board's serial port.

```bash
idf.py -p COMx flash monitor
```

On Linux/macOS:

```bash
idf.py -p /dev/ttyUSB0 flash monitor
```

When the device boots, the assigned IP appears, then it sends the read request to the server.

```
I (433) wiztoe_net: TOE up: 192.168.11.2 (WIZnet hardware TCP/IP)
I (433) tftp: [eth] waiting for link...
I (434) tftp: [eth] requesting "tftp_test_file.txt" from 192.168.11.4
```

The protocol trace and the result follow. The first block's contents are echoed so a
transfer is visibly a transfer, not just a success code.

```
>> TFTP RRQ : FileName(tftp_test_file.txt), Mode(octet)
<< TFTP_OACK :
>> TFTP ACK : Block Number(0)
<< TFTP_DATA : opcode(3), block_num(1)
I (6458) tftp: first block: "WIZnet wsm_driver TFTP test file  Transferred over th" ...
>> TFTP ACK : Block Number(1)
<< TFTP_DATA : opcode(3), block_num(2)
>> TFTP ACK : Block Number(2)
<< TFTP_DATA : opcode(3), block_num(3)
>> TFTP ACK : Block Number(3)
I (6488) tftp: [eth] "tftp_test_file.txt" received: 1461 bytes in 3 blocks
```

The byte and block counts should match what Tftpd64 reports in its **Tftp Server** tab.

> A gap of a second or two before the first `TFTP RRQ` is normal. The chip resolves
> the server's MAC with ARP inside `sendto()`, and the first packet to a new peer
> often times out before the reply arrives; the retransmission timer sends it again.

![][link-run_tftp_success]

## Troubleshooting

### `tftp read fail` — File not found or No Access

Check in order:

1. **File exists in the Tftpd64 directory**
   Confirm that `tftp_test_file.txt` exists in the folder configured as **Current Directory / Base Directory**.

2. **Check with Show Dir**
   Press **Show Dir** in Tftpd64. If `tftp_test_file.txt` does not appear there, Tftpd64 cannot access the file.

3. **Check the file extension**
   On Windows, file extensions may be hidden. Make sure the actual file name is not:

   ```text
   tftp_test_file.txt.txt
   ```

4. **Server interface**
   Tftpd64's **Server interfaces** must be set to the same PC IP address used by `TFTP_SERVER_IP` in the code.

5. **Another TFTP server may already own UDP 69**
   This is the one that looks exactly like a device-side bug and is not. If a second
   TFTP server is installed — OpenTFTPServer and similar ship as an auto-start Windows
   service — it takes port 69 first, Tftpd64 silently falls back to IPv6 only, and every
   request is answered by the *other* server, which has never heard of your file. The
   giveaway is that Tftpd64's **Server interfaces** keeps reverting to `::1`.

   ```powershell
   Get-NetUDPEndpoint -LocalPort 69 | ForEach-Object {
       "{0,-18} {1}" -f $_.LocalAddress, (Get-Process -Id $_.OwningProcess).Name
   }
   ```

   Anything other than Tftpd64 on your PC's address is the culprit. Stop it from an
   elevated shell, then restart Tftpd64:

   ```powershell
   Stop-Service <ServiceName>
   Set-Service <ServiceName> -StartupType Manual   # so it does not come back on reboot
   ```

6. **Confirm the server independently**
   Test the server without the device at all — `curl` on Windows 10+ speaks TFTP:

   ```powershell
   curl.exe -o out.txt tftp://192.168.11.4/tftp_test_file.txt
   ```

   If this fails, the problem is on the PC and nothing on the device will fix it.

   Example:

   ```cpp
   #define TFTP_SERVER_IP "192.168.11.4"
   ```

   Tftpd64 should also be bound to `192.168.11.4`.

5. **Check Tftpd64 Log Viewer**
   Open the **Log Viewer** tab and run the example again. If Tftpd64 receives the request but cannot open the file, it will report a file access or file not found message.

6. **Firewall / port conflict**
   If no request appears in the Tftpd64 log, check Windows Firewall or whether another TFTP server is already using UDP port 69.

   ```powershell
   netstat -ano | Select-String ":69 "
   ```

### `tftp read fail` — Timeout (no response)

- Check Windows Firewall: allow Tftpd64 on UDP port 69 (both inbound and outbound).
- Confirm the device and PC are on the same subnet.

## Appendix

- **TFTP server IP / file name:** `TFTP_SERVER_IP` must match the **Server interfaces** address configured in Tftpd64. `TFTP_SERVER_FILE_NAME` must exist in Tftpd64's **Base Directory**.
- **Retransmission timer:** An `esp_timer` fires `tftp_timeout_handler()` once per second to drive TFTP retransmission, so a dropped packet is retried automatically.
- **W6300 QSPI mode:** Quad mode (4-bit) requires the extra D2/D3 lines wired and selected in `Component config -> WIZnet WSM Driver -> W6300 QSPI mode`. Single mode uses the same 4-wire wiring as W5500.

<!-- Link -->
[link-tera_term]: https://osdn.net/projects/ttssh2/releases/
[link-tftpd64]: https://pjo2.github.io/tftpd64/

[link-hardware]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/tftp/hardware.png
[link-config_main]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/tftp/config_main.png
[link-config_component]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/tftp/config_component.png
[link-config_wiz_toe]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/tftp/config_wiz_toe.png

[link-build_log]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/tftp/build_log.png
[link-run_tftpd64]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/tftp/run_tftpd64.png
[link-run_tftp_success]: https://raw.githubusercontent.com/Wiznet/wsm_driver/main/static/image/tftp/run_tftp_success.png
