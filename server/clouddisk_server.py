#!/usr/bin/env python3
"""
雲盤扇區服務：把一個鏡像文件按 512 字節扇區暴露成 HTTP 接口，給 ESP32-S3 U 盤固件轉發讀寫。
只聽 127.0.0.1，對外經 Tailscale Funnel（HTTPS 終結在 Funnel）。只用標準庫。

  GET  /info                 -> {"sectors": N, "sector_size": 512}
  GET  /blk/<lba>?n=<count>  -> count 個扇區的原始字節
  PUT  /blk/<lba>            -> 寫入請求體（長度須為 512 的整數倍），回 204
  POST /sync                 -> fsync 鏡像文件，回 204
  POST /log                  -> 請求體（文本）追加到 <dir>/esp.log，回 204
                                板子只插了原生 USB 口時沒有串口，日誌靠這個回傳

所有請求都要帶 Authorization: Bearer <token>，token 在 <dir>/token。
"""
import argparse
import hmac
import json
import os
import re
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

SECTOR = 512
MAX_SECTORS = 256           # 單次最多 128KB，防止一個請求吃光內存
MAX_LOG = 16384
LOG_ROTATE = 5 << 20        # esp.log 超過 5MB 就改名成 .1 重新開始
BLK_RE = re.compile(r"^/blk/(\d+)$")


class Disk:
    def __init__(self, path):
        self.fd = os.open(path, os.O_RDWR)
        self.sectors = os.fstat(self.fd).st_size // SECTOR
        self.stats = {"read_bytes": 0, "write_bytes": 0, "reqs": 0}
        self.lock = threading.Lock()

    def check(self, lba, n):
        return 0 < n <= MAX_SECTORS and lba + n <= self.sectors


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"   # keep-alive：ESP32 一條 TLS 連接複用到底，省握手
    server_version = "clouddisk/0.1"

    def log_message(self, fmt, *args):   # 默認每個請求一行，扇區級訪問會刷屏
        pass

    def _auth(self):
        got = self.headers.get("Authorization", "")
        if hmac.compare_digest(got.encode(), self.server.auth.encode()):
            return True
        time.sleep(1)                     # 公網暴露，拖慢暴力猜 token
        # 請求體沒讀；繼續複用連接的話，它會被當成下一個請求的開頭，所以直接斷開
        self.close_connection = True
        self._reply(401, b"unauthorized\n")
        return False

    def _reply(self, code, body=b"", ctype="application/octet-stream"):
        self.send_response(code)
        if self.close_connection:
            self.send_header("Connection", "close")
        if code != 204:
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if body and code != 204:
            self.wfile.write(body)

    def _length(self, limit):
        """返回 (Content-Length, 錯誤碼)。格式不對回 400，超過 limit 回 413，都不讀請求體。"""
        raw = self.headers.get("Content-Length", "0").strip()
        if not raw.isdigit():
            return None, 400
        if int(raw) > limit:
            return None, 413
        return int(raw), None

    def _reject(self, code, msg):
        """沒法安全讀完請求體時用：回錯誤並斷開，保證不會錯位到下一個請求。"""
        self.close_connection = True
        self._reply(code, msg)

    def _lba(self):
        path, _, query = self.path.partition("?")
        m = BLK_RE.match(path)
        if not m:
            return None, None
        n = 1
        for kv in query.split("&"):
            if kv.startswith("n="):
                n = int(kv[2:]) if kv[2:].isdigit() else 0
        return int(m.group(1)), n

    def do_GET(self):
        if not self._auth():
            return
        disk = self.server.disk
        if self.path == "/info":
            body = json.dumps({"sectors": disk.sectors, "sector_size": SECTOR, **disk.stats}).encode()
            return self._reply(200, body, "application/json")
        lba, n = self._lba()
        if lba is None or not disk.check(lba, n):
            return self._reply(400, b"bad range\n")
        data = os.pread(disk.fd, n * SECTOR, lba * SECTOR)
        with disk.lock:
            disk.stats["read_bytes"] += len(data)
            disk.stats["reqs"] += 1
        self._reply(200, data)

    def do_PUT(self):
        if not self._auth():
            return
        disk = self.server.disk
        lba, _ = self._lba()
        length, _ = self._length(MAX_SECTORS * SECTOR)
        if lba is None or length is None or length % SECTOR or not disk.check(lba, length // SECTOR):
            return self._reject(400, b"bad range\n")
        data = self.rfile.read(length)
        if len(data) != length:
            # 客戶端中途斷了：半截扇區絕不能落盤
            return self._reject(400, b"truncated body\n")
        written = 0
        while written < length:
            n = os.pwrite(disk.fd, memoryview(data)[written:], lba * SECTOR + written)
            if n <= 0:
                return self._reject(500, b"short write\n")
            written += n
        with disk.lock:
            disk.stats["write_bytes"] += len(data)
            disk.stats["reqs"] += 1
        self._reply(204)

    def do_POST(self):
        if not self._auth():
            return
        length, err = self._length(MAX_LOG)
        if length is None:
            return self._reject(err, b"bad length\n")
        if self.path == "/sync":
            self.rfile.read(length)
            os.fsync(self.server.disk.fd)
            return self._reply(204)
        if self.path == "/log" and length > 0:
            body = self.rfile.read(length)
            if len(body) != length:
                return self._reject(400, b"truncated body\n")
            text = body.decode("utf-8", "replace")
            path = self.server.log_path
            with self.server.disk.lock:
                if os.path.exists(path) and os.path.getsize(path) > LOG_ROTATE:
                    os.replace(path, path + ".1")
                with open(path, "a") as f:
                    stamp = time.strftime("%H:%M:%S")
                    f.writelines(f"{stamp} {line}\n" for line in text.rstrip("\n").split("\n"))
            return self._reply(204)
        self.rfile.read(length)
        self._reject(404, b"not found\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", default=os.path.expanduser("~/clouddisk"))
    ap.add_argument("--port", type=int, default=8090)
    args = ap.parse_args()

    with open(os.path.join(args.dir, "token")) as f:
        token = f.read().strip()
    srv = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    srv.daemon_threads = True
    srv.auth = "Bearer " + token
    srv.disk = Disk(os.path.join(args.dir, "disk.img"))
    srv.log_path = os.path.join(args.dir, "esp.log")
    print(f"clouddisk: {srv.disk.sectors} 扇區（{srv.disk.sectors * SECTOR >> 20} MB），聽 127.0.0.1:{args.port}", flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    main()
