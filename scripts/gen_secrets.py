#!/usr/bin/env python3
"""
從項目根目錄的 secrets.env 生成兩個頭文件（都在 .gitignore 裡）：
  firmware/common/board_common/include/wifi_secrets.h   WiFi
  firmware/usb_disk/main/clouddisk_secrets.h            雲盤地址、token、可選的企業網憑據
CLOUDDISK_URL 為空時刪掉 clouddisk_secrets.h，固件編成本地模式。
只打印各字段長度，不打印內容。
"""
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WIFI_H = os.path.join(ROOT, "firmware/common/board_common/include/wifi_secrets.h")
DISK_H = os.path.join(ROOT, "firmware/usb_disk/main/clouddisk_secrets.h")
# 用 __has_include("clouddisk_secrets.h") 決定模式的源文件。頭文件從無到有（或反過來）時
# ninja 的依賴記錄裡沒有它，不會重編這些文件，網絡模式會悄悄沿用本地模式的目標文件；
# 所以切換模式時更新它們的時間戳，逼構建系統重編。
MODE_SOURCES = [os.path.join(ROOT, "firmware/usb_disk/main", f) for f in ("main.c", "blockdev_http.c")]
HEADER = "/* 由 scripts/gen_secrets.py 從 secrets.env 生成，請勿手工編輯，勿提交 */\n#pragma once\n\n"


def load(path):
    env = {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            k, v = line.split("=", 1)
            env[k.strip()] = v.strip().strip('"')
    return env


def c_str(v):
    return '"' + v.replace("\\", "\\\\").replace('"', '\\"') + '"'


def write(path, defs):
    old = os.umask(0o077)
    try:
        with open(path, "w", encoding="utf-8") as f:
            f.write(HEADER)
            for k, v in defs:
                f.write(f"#define {k} {c_str(v)}\n")
    finally:
        os.umask(old)


def main():
    src = os.path.join(ROOT, "secrets.env")
    if not os.path.exists(src):
        sys.exit("沒有 secrets.env：先 cp secrets.env.example secrets.env 再填寫")
    env = load(src)
    eap = bool(env.get("EAP_SSID"))
    if not env.get("WIFI_SSID") and not eap:
        sys.exit("WIFI_SSID 和 EAP_SSID 至少填一個")

    # board_wifi.c 總要這兩個宏；只用企業網時填佔位值，不會被使用
    write(WIFI_H, [("WIFI_SSID", env.get("WIFI_SSID") or "unused"),
                   ("WIFI_PASSWORD", env.get("WIFI_PASSWORD", ""))])

    was_network = os.path.exists(DISK_H)
    if env.get("CLOUDDISK_URL"):
        if not env.get("CLOUDDISK_TOKEN"):
            sys.exit("填了 CLOUDDISK_URL 就必須填 CLOUDDISK_TOKEN")
        defs = [("CLOUDDISK_URL", env["CLOUDDISK_URL"].rstrip("/")),
                ("CLOUDDISK_TOKEN", env["CLOUDDISK_TOKEN"])]
        if eap:
            defs += [(f"CLOUDDISK_EAP_{k}", env.get(f"EAP_{k}", ""))
                     for k in ("SSID", "IDENTITY", "USERNAME", "PASSWORD")]
        write(DISK_H, defs)
        mode = "網絡模式（" + ("WPA2-Enterprise" if eap else "WPA2-Personal") + "）"
    else:
        if os.path.exists(DISK_H):
            os.remove(DISK_H)
        mode = "本地模式（SD 卡 / PSRAM 內存盤）"

    if was_network != os.path.exists(DISK_H):
        for f in MODE_SOURCES:
            os.utime(f)
        mode += "，已觸發相關源文件重編"

    lens = {k: len(v) for k, v in env.items() if v}
    print(f"已生成，固件將編成{mode}；字段長度: {lens}")


if __name__ == "__main__":
    main()
