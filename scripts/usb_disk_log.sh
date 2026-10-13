#!/usr/bin/env bash
# 經 USB 讀 usb_disk 固件最近 4KB 日誌（廠商 SCSI 命令 FC 'L''O''G'），不用串口、不用網絡。
# 走 /dev/sgN（SCSI generic）而不是 /dev/sdX：沒有介質時 sdX 打不開，sg 可以。
set -euo pipefail
disk="$(lsblk -dno NAME,MODEL | awk '$2=="Cloud" {print $1; exit}')"
[ -n "$disk" ] || { echo "找不到 Cloud Disk（U 盤沒 attach 到 WSL？）"; exit 1; }
sg="/dev/$(ls /sys/block/$disk/device/scsi_generic/)"
docker run --rm --privileged --device "$sg:$sg" alpine:latest sh -c \
    "apk add -q sg3_utils >/dev/null && sg_raw -n -r 4096 -o /tmp/log $sg fc 4c 4f 47 00 00 00 00 00 00 2>/dev/null; cat /tmp/log"
