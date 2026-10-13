# ESP32-S3 雲盤 U 盤

[English](README.md) | **繁體中文**

把 ESP32-S3 插到電腦上，它被識別成一個普通 U 盤；每一次扇區讀寫都經 WiFi + HTTPS
轉發到你自己服務器上的一個鏡像文件。電腦端不用裝任何驅動或客戶端。

```
電腦 ──USB(MSC)──▶ ESP32-S3 ──WiFi/HTTPS──▶ 反向代理(如 Tailscale Funnel) ──▶ clouddisk_server.py ──▶ disk.img
       讀寫扇區      3MB 讀緩存                                            127.0.0.1:8090
                    64KB 寫合併
```

- 固件：ESP-IDF v5.3 純 C，直接用 TinyUSB（不經 esp_tinyusb 的 MSC 層，因為要自己處理扇區）
- 服務端：Python 標準庫單文件，按 512 字節扇區讀寫一個鏡像文件，Bearer token 鑒權
- 塊設備層可替換：同一份固件在沒有服務器配置時編成本地模式（SD 卡直通，沒卡就用 PSRAM 內存盤）

## 它不是什麼

U 盤協議（USB MSC）傳的是**扇區**，不是文件：電腦自己在上面跑 FAT/exFAT。所以

- 服務器上看到的是**一個鏡像文件**，不是一個能直接瀏覽的目錄。要看裡面的文件，先在電腦端彈出，
  再在服務器上 `sudo mount -o loop,ro disk.img /mnt/x`。**電腦掛著的時候不要在服務器端掛載**，兩邊同時寫必然損壞文件系統。
- 速度受限於 S3 的 USB 全速（12Mbps）和網絡往返，大文件約 260 KB/s（1GB 約 65 分鐘），適合存文檔照片，不適合大量小文件或大視頻。

## 硬件

開發和實測用的是 Freenove ESP32-S3-WROOM CAM（16MB Flash、8MB 八線 PSRAM、兩個 Type-C）。
其他 S3 板子只要滿足這兩點就行，引腳在 `firmware/common/board_common/include/board_pins.h` 改：

- **原生 USB 口**（GPIO19/20）引出到 USB 座子——U 盤就從這個口出來
- **PSRAM**——讀緩存、寫緩衝、TLS 都放在 PSRAM

另一個 Type-C（CH343 串口）可選，只用來看串口日誌；不接也行，見下文「看日誌」。

## 快速開始

### 1. 服務端

任何能跑 Python 3 的 Linux 機器。鏡像是稀疏文件，沒寫過的部分不佔磁盤。

```bash
mkdir -p ~/clouddisk && chmod 700 ~/clouddisk
cp server/clouddisk_server.py ~/clouddisk/
truncate -s 1G ~/clouddisk/disk.img
(umask 077; python3 -c "import secrets;print(secrets.token_urlsafe(32))" > ~/clouddisk/token)

# systemd：先把 clouddisk.service 裡的 YOUR_USER 換掉
sudo cp server/clouddisk.service /etc/systemd/system/
sudo systemctl enable --now clouddisk
```

服務只聽 `127.0.0.1:8090`，前面需要一個 **帶有效證書的 HTTPS** 入口（固件用 ESP-IDF 內置的公共 CA 庫驗證證書）。
不想開公網端口的話，[Tailscale Funnel](https://tailscale.com/kb/1223/funnel) 是最省事的：

```bash
sudo tailscale funnel --bg 8090        # 得到 https://<機器名>.<tailnet>.ts.net/
```

第一次開 Funnel 要在 tailnet 管理頁授權，證書簽發約需一分鐘，這期間 TLS 握手會失敗。

接口（全部要帶 `Authorization: Bearer <token>`）：

| 請求 | 作用 |
|---|---|
| `GET /info` | 盤大小和讀寫統計 |
| `GET /blk/<lba>?n=<扇區數>` | 讀扇區（單次最多 256 個 = 128KB） |
| `PUT /blk/<lba>` | 寫扇區，請求體長度須是 512 的整數倍 |
| `POST /sync` | fsync 鏡像 |
| `POST /log` | 板子回傳的日誌，追加到 `~/clouddisk/esp.log` |

### 2. 固件

需要 [ESP-IDF v5.3](https://docs.espressif.com/projects/esp-idf/en/v5.3/esp32s3/get-started/)。

```bash
cp secrets.env.example secrets.env     # 填 WiFi、服務器地址和 token
python3 scripts/gen_secrets.py         # 生成兩個頭文件（都在 .gitignore 裡）

cd firmware/usb_disk
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyACM0 flash           # 首次燒錄：插原生 USB 口，按住 BOOT 再按一下 RST 進下載模式
```

**首次燒錄後拔插一次**才會跑新固件（用按鍵進的下載模式，esptool 的復位會回到下載模式）。
之後原生口就是 U 盤了，再燒錄用 `scripts/usb_disk_flash.sh`，見下文。

`CLOUDDISK_URL` 留空時固件編成本地模式（SD 卡直通，沒卡用 6MB PSRAM 內存盤），可以先驗證 USB 這一層。

### 3. 使用

插上電腦，聯網成功後出現一個 1GB 的盤（聯網前顯示為「無介質」，就緒後固件會自動重新枚舉一次）。
第一次用先格式化：Windows 直接在資源管理器裡格式化；Linux `mkfs.vfat -F 32 /dev/sdX`，
掛載時加 `-o iocharset=utf8` 才能用中文文件名。

**拔之前先「彈出」**：寫入有最多 300ms 的回寫延遲，彈出時固件會把緩衝全部推上服務器。

## 開發輔助

這兩個腳本假定 U 盤在 Linux 上可見（原生 Linux，或 WSL 經 `usbipd attach --auto-attach`）。
沒有 root 時借 `docker run --privileged` 發 SCSI 命令。

| 腳本 | 作用 |
|---|---|
| `scripts/usb_disk_flash.sh` | 給 U 盤發廠商 SCSI 命令 `FB "BOOT"`，板子自己切回下載模式；燒完自動回到 U 盤。**不用按鍵** |
| `scripts/usb_disk_log.sh` | 發 `FC "LOG"` 讀最近 4KB 日誌。不用串口，WiFi 沒連上時也能看 |

聯網後日誌還會每 2 秒 POST 到服務器的 `esp.log`；固件每 10 秒打印一行吞吐、緩存命中率、請求重試和最慢請求。

## 實測

ESP32-S3 經 WPA2-Enterprise WiFi（RSSI 約 −50 dBm），透過 Tailscale Funnel 連到往返幾十毫秒的雲主機。

| 場景 | 結果 |
|---|---|
| 50MB 文件寫入 / 冷讀 | 249 KB/s / 255 KB/s，sha256 一致 |
| 200MB 文件寫入 / 冷讀 | 261 KB/s / 262 KB/s（歷時 26 分鐘，WiFi 斷線、請求重試、寫入丟失均為 0），電腦端和服務器端鏡像 sha256 均一致 |
| 16MB 裸設備冷讀 | 129 KB/s |
| 200 個 4KB 小文件寫入 | 31.8 s（小文件要反覆改 FAT 表，慢得多） |
| 1GB `mkfs.vfat -F 32` | 41 s |
| 207 個文件寫入→卸載→冷讀校驗→服務器端掛鏡像校驗 | 全部一致 |
| 本地 PSRAM 內存盤（不經網絡） | 讀 837 KB/s，寫 1.1 MB/s（USB 全速上限） |

## 限制和風險

- **寫回緩存**：寫入回調返回成功時數據可能還在板子內存裡（最多 300ms，最多 64KB）。這段時間斷電或斷網會丟數據。
- **單連接串行**：一次只有一個 HTTP 請求在路上，延遲直接決定吞吐。提速方向：更大的預讀塊、順序讀時後台預取、TinyUSB 異步 IO。
- **容量**：固件只實現 READ(10)/WRITE(10)，盤最大 2TB；FAT32 單文件最大 4GB，要更大就在電腦端格式化成 exFAT（板子只轉發扇區，不關心文件系統）。
- **安全**：服務端只靠一個 Bearer token，錯誤 token 會延遲 1 秒。token 編譯進固件，拿到板子的人能讀出來。企業網模式不驗證 RADIUS 服務器證書。
- **USB VID/PID**：用的是 Espressif 的 VID `0x303A`，只適合自用和測試，對外發布產品需要自己的 VID/PID。

## 踩過的坑

1. **MSC 單次傳輸緩衝不能超過 4KB**。設成 16KB 時，16KB 及以上的寫會卡死，主機 121 秒超時後把盤下線。S3 的 DWC2 單次傳輸包數有上限（64 字節一包）。
2. **esp_tinyusb 的 MSC 層自帶強符號 SCSI 回調**，只支持 SPI Flash / SD，所以這裡直接用 TinyUSB，自己配 PHY、描述符和回調。
3. **用 BOOT+RST 進的下載模式，esptool 燒完的 hard_reset 會回到下載模式**（ROM 報 `boot:0x21 DOWNLOAD`），只能拔插。用軟件（RTC 強制下載標誌）進的下載模式沒有這個問題。
4. **軟件進下載模式前必須先 `tud_disconnect()` 停 0.5 秒**。否則主機不知道設備拔了，不會去枚舉新出現的下載口。
5. **網絡盤開機時沒有介質，Linux 枚舉後就停止輪詢**（dmesg `Media removed, stopped polling`），之後介質就緒它也不知道。固件在盤從無到有時斷開重連一次 USB。
6. **WiFi 配置結構要先清零再拷 SSID**。按另一個網絡初始化後只拷短 SSID，會帶上原 SSID 的尾巴，結果掃描看得到 AP，連接卻一直 `reason=201`（找不到 AP）。
7. **TCP 窗口要放大**（`sdkconfig.defaults` 裡的 LWIP 項）。默認 5.7KB 的窗口在幾十毫秒的往返下會把吞吐壓到百來 KB/s 以下。
8. **電腦 USB 口供電偏弱時 WiFi 會不穩**：峰值電流造成瞬間壓降，表現為板子自報信號很好、網絡側卻丟包。換好一點的線或帶電源的 Hub。

## 目錄

```
firmware/usb_disk/          固件（TinyUSB MSC + 塊設備後端）
  main/usb_msc.c            描述符、SCSI 回調、軟件進下載模式、介質就緒重新枚舉
  main/blockdev_http.c      網絡後端：讀緩存、寫合併、keep-alive HTTPS
  main/blockdev_sd.c        SD 卡直通
  main/blockdev_ram.c       PSRAM 內存盤
  main/remote_log.c         日誌回傳 + 經 USB 讀日誌
firmware/common/board_common/  WiFi（含 WPA2-Enterprise）、引腳定義
server/                     扇區服務 + systemd 單元
scripts/                    憑據生成、免按鍵燒錄、讀日誌
```

## 授權

[MIT](LICENSE)
