#!/usr/bin/env bash
# 燒錄 usb_disk：原生口被 U 盤佔用時，先發廠商 SCSI 命令讓板子自己進下載模式，不用按 BOOT。
# 用法： bash scripts/usb_disk_flash.sh
# 前提：U 盤已 attach 到 WSL（usbipd attach --wsl --busid <id> --auto-attach）。
# 沒有 sudo 也能發 SCSI 命令：借 docker 的 --privileged 容器做。
set -euo pipefail

PROJ_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PORT=/dev/ttyACM0

find_disk() {
    lsblk -dno NAME,MODEL | awk '$2=="Cloud" {print "/dev/"$1; exit}'
}

if [ ! -e "$PORT" ]; then
    disk="$(find_disk)"
    if [ -n "$disk" ]; then
        echo "==> 給 $disk 發「重啟進下載模式」命令"
        # FB 'B' 'O' 'O' 'T'，補齊 10 字節 CDB
        docker run --rm --privileged --device "$disk:$disk" alpine:latest sh -c \
            "apk add -q sg3_utils >/dev/null && sg_raw $disk fb 42 4f 4f 54 00 00 00 00 00" || true
    fi
    echo "==> 等下載口 $PORT 出現（最多 30 秒）"
    for _ in $(seq 30); do [ -e "$PORT" ] && break; sleep 1; done
    [ -e "$PORT" ] || { echo "沒等到下載口：按住 BOOT 再按一下 RST，然後重跑本腳本"; exit 1; }
    sleep 2
fi

if [ -n "${VIRTUAL_ENV:-}" ]; then
    PATH="$(echo "$PATH" | tr ':' '\n' | grep -vxF "$VIRTUAL_ENV/bin" | paste -sd:)"; export PATH; unset VIRTUAL_ENV
fi
# shellcheck disable=SC1091
. "${IDF_PATH:-$HOME/esp/esp-idf}/export.sh" >/dev/null
cd "$PROJ_ROOT/firmware/usb_disk"
idf.py build >/dev/null
idf.py -p "$PORT" flash | tail -3

# 經 usbipd 的 USB-Serial-JTAG 上，esptool 的 hard_reset / watchdog_reset / run 實測都回到下載模式
# （ROM 報 boot:0x21 DOWNLOAD，GPIO0 被看成低電平），只有斷電重上電能進 app
echo "==> 燒錄完成。請拔插一次原生 USB 口的線（斷電重啟），板子才會跑新固件"
echo "==> 等 U 盤重新出現（最多 120 秒）"
for _ in $(seq 120); do d="$(find_disk)"; [ -n "$d" ] && break; sleep 1; done
[ -n "${d:-}" ] && lsblk -o NAME,SIZE,TRAN,MODEL,SERIAL "$d" || echo "U 盤沒出現：確認 usbipd 有 --auto-attach 把它掛回 WSL"
