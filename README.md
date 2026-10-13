# ESP32-S3 Cloud USB Drive

**English** | [繁體中文](README.zh-TW.md)

Plug an ESP32-S3 into a computer and it shows up as an ordinary USB flash drive. Every sector
read and write is forwarded over WiFi + HTTPS to a disk image on your own server. Nothing needs
to be installed on the computer.

```
Computer ──USB (MSC)──▶ ESP32-S3 ──WiFi/HTTPS──▶ reverse proxy (e.g. Tailscale Funnel) ──▶ clouddisk_server.py ──▶ disk.img
          sector I/O    3MB read cache                                                   127.0.0.1:8090
                        64KB write coalescing
```

- Firmware: ESP-IDF v5.3, plain C, using TinyUSB directly (not esp_tinyusb's MSC layer, because the sectors are handled here)
- Server: a single Python file, standard library only, serving a disk image in 512-byte sectors behind a Bearer token
- Pluggable block layer: without a server configured, the same firmware builds in local mode (SD card passthrough, or a PSRAM RAM disk if there is no card)

## What it is not

USB Mass Storage carries **sectors**, not files — the computer runs FAT/exFAT on top. Therefore:

- The server holds **a disk image**, not a browsable directory. To look at the files, eject the drive on the computer first, then on the server run `sudo mount -o loop,ro disk.img /mnt/x`. **Never mount it on the server while the computer has it mounted** — two writers will corrupt the filesystem.
- Throughput is bounded by the S3's Full-Speed USB (12 Mbit/s) and by network round trips: about 260 KB/s for large files (roughly 65 minutes per GB). Fine for documents and photos, not for many small files or large videos.

## Hardware

Developed and measured on a Freenove ESP32-S3-WROOM CAM (16MB flash, 8MB octal PSRAM, two USB-C ports).
Any S3 board works if it has both of the following; adjust pins in `firmware/common/board_common/include/board_pins.h`:

- **Native USB** (GPIO19/20) wired to a USB connector — this is where the drive appears
- **PSRAM** — the read cache, write buffer and TLS buffers live there

The second USB-C port (CH343 UART) is optional and only used for the serial console. You can do without it; see "Reading logs" below.

## Quick start

### 1. Server

Any Linux machine with Python 3. The image is a sparse file, so unwritten space costs no disk.

```bash
mkdir -p ~/clouddisk && chmod 700 ~/clouddisk
cp server/clouddisk_server.py ~/clouddisk/
truncate -s 1G ~/clouddisk/disk.img
(umask 077; python3 -c "import secrets;print(secrets.token_urlsafe(32))" > ~/clouddisk/token)

# systemd: replace YOUR_USER in clouddisk.service first
sudo cp server/clouddisk.service /etc/systemd/system/
sudo systemctl enable --now clouddisk
```

The service only listens on `127.0.0.1:8090` and needs an **HTTPS front end with a valid certificate**
(the firmware verifies it against ESP-IDF's built-in public CA bundle).
If you don't want to open a public port, [Tailscale Funnel](https://tailscale.com/kb/1223/funnel) is the easiest option:

```bash
sudo tailscale funnel --bg 8090        # gives you https://<machine>.<tailnet>.ts.net/
```

The first time, Funnel has to be approved in the tailnet admin console, and the certificate takes about a minute to be issued; TLS handshakes fail until then.

API (every request needs `Authorization: Bearer <token>`):

| Request | Purpose |
|---|---|
| `GET /info` | Disk size and I/O counters |
| `GET /blk/<lba>?n=<sectors>` | Read sectors (at most 256 = 128KB per request) |
| `PUT /blk/<lba>` | Write sectors; body length must be a multiple of 512 |
| `POST /sync` | fsync the image |
| `POST /log` | Log lines from the board, appended to `~/clouddisk/esp.log` |

### 2. Firmware

Requires [ESP-IDF v5.3](https://docs.espressif.com/projects/esp-idf/en/v5.3/esp32s3/get-started/).

```bash
cp secrets.env.example secrets.env     # fill in WiFi, server URL and token
python3 scripts/gen_secrets.py         # generates two headers (both git-ignored)

cd firmware/usb_disk
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyACM0 flash           # first flash: native USB port, hold BOOT and tap RST to enter download mode
```

**Unplug and replug once after the first flash** to start the new firmware (when download mode was entered with the BOOT button, esptool's reset lands back in download mode).
From then on the native port is the USB drive; reflash with `scripts/usb_disk_flash.sh` as described below.

If `CLOUDDISK_URL` is left empty, the firmware builds in local mode (SD card passthrough, or a 6MB PSRAM RAM disk without a card), which is handy for validating the USB layer first.

### 3. Use

Plug it in. Once the network is up, a 1GB drive appears (before that it shows as "no media"; the firmware re-enumerates once when the disk becomes ready).
Format it the first time: in Windows Explorer, or on Linux with `mkfs.vfat -F 32 /dev/sdX`.
On Linux, mount with `-o iocharset=utf8` if you need non-ASCII file names.

**Eject before unplugging**: writes are held back for up to 300ms, and ejecting makes the firmware push everything to the server.

## Development helpers

Both scripts expect the drive to be visible on Linux (native Linux, or WSL via `usbipd attach --auto-attach`).
Without root they use `docker run --privileged` to send the SCSI commands.

| Script | Purpose |
|---|---|
| `scripts/usb_disk_flash.sh` | Sends the vendor SCSI command `FB "BOOT"`, so the board switches itself into download mode; after flashing it comes back as the drive. **No buttons needed** |
| `scripts/usb_disk_log.sh` | Sends `FC "LOG"` to read the last 4KB of log. No serial cable needed, and it works even when WiFi is down |

Once online, the board also POSTs its log to the server's `esp.log` every 2 seconds, and prints a line every 10 seconds with throughput, cache hit rate, request retries and the slowest request.

## Measurements

ESP32-S3 on a WPA2-Enterprise WiFi network (RSSI about −50 dBm), connecting through Tailscale Funnel to a cloud VM a few tens of milliseconds away.

| Scenario | Result |
|---|---|
| 50MB file write / cold read | 249 KB/s / 255 KB/s, sha256 matches |
| 200MB file write / cold read | 261 KB/s / 262 KB/s (26 minutes, no WiFi drops, request retries or lost writes), sha256 matches on the computer and in the server-side image |
| 16MB raw device cold read | 129 KB/s |
| Writing 200 × 4KB files | 31.8 s (small files keep rewriting the FAT, so they are much slower) |
| `mkfs.vfat -F 32` on 1GB | 41 s |
| 207 files: write → unmount → cold read and verify → mount the image on the server and verify | all match |
| Local PSRAM RAM disk (no network) | read 837 KB/s, write 1.1 MB/s (the Full-Speed USB ceiling) |

## Limitations and risks

- **Write-back cache**: when a write callback returns success, the data may still be in the board's RAM (up to 300ms, up to 64KB). Losing power or network in that window loses the data.
- **One request at a time**: only one HTTP request is in flight, so latency sets the throughput. Ways to speed it up: larger read-ahead blocks, background prefetch for sequential reads, TinyUSB async I/O.
- **Capacity**: only READ(10)/WRITE(10) are implemented, so the disk can be up to 2TB. FAT32 caps a single file at 4GB; for larger files, format the drive as exFAT on the computer (the board only forwards sectors and doesn't care about the filesystem).
- **Security**: the server is protected by a single Bearer token, with a 1-second delay on a wrong token. The token is compiled into the firmware, so anyone holding the board can extract it. Enterprise WiFi mode does not validate the RADIUS server certificate.
- **USB VID/PID**: uses Espressif's VID `0x303A`, which is only acceptable for personal use and testing. A product needs its own VID/PID.

## Lessons learned

1. **The MSC transfer buffer must not exceed 4KB.** At 16KB, writes of 16KB or more hung, and the host took the disk offline after a 121-second timeout. The S3's DWC2 limits the packet count per transfer (64 bytes per packet).
2. **esp_tinyusb's MSC layer defines strong SCSI callbacks** and only supports SPI flash / SD, so this project uses TinyUSB directly with its own PHY setup, descriptors and callbacks.
3. **After entering download mode with BOOT+RST, esptool's hard_reset after flashing lands back in download mode** (the ROM reports `boot:0x21 DOWNLOAD`); only a power cycle helps. Download mode entered from software (the RTC force-download flag) doesn't have this problem.
4. **Call `tud_disconnect()` and wait 0.5s before switching to download mode from software.** Otherwise the host never notices the device left and never enumerates the new download port.
5. **A network disk has no media at boot, and Linux stops polling after enumeration** (dmesg: `Media removed, stopped polling`), so it never notices when the media becomes ready. The firmware disconnects and reconnects USB once when the disk goes from absent to present.
6. **Zero the WiFi config struct before copying the SSID.** After initialising it for another network, copying a short SSID leaves the old SSID's tail behind: the AP shows up in scans but every connection fails with `reason=201` (AP not found).
7. **Enlarge the TCP window** (the LWIP entries in `sdkconfig.defaults`). The default 5.7KB window caps throughput at around a hundred KB/s or less with tens of milliseconds of round trip.
8. **Weak USB power from the computer makes WiFi unreliable**: current peaks cause brief voltage drops, so the board reports a good signal while the network side sees packet loss. Use a better cable or a powered hub.

## Layout

```
firmware/usb_disk/             Firmware (TinyUSB MSC + block device backends)
  main/usb_msc.c               Descriptors, SCSI callbacks, software download mode, re-enumeration when media appears
  main/blockdev_http.c         Network backend: read cache, write coalescing, keep-alive HTTPS
  main/blockdev_sd.c           SD card passthrough
  main/blockdev_ram.c          PSRAM RAM disk
  main/remote_log.c            Log upload + reading the log over USB
firmware/common/board_common/  WiFi (including WPA2-Enterprise), pin definitions
server/                        Sector server + systemd unit
scripts/                       Secret generation, button-free flashing, log reader
```

Code comments are in Chinese.

## License

[MIT](LICENSE)
