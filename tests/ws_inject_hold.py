#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""ws_inject_hold.py —— 保持一根虚拟触点（做「身份两段」对照测试用），同时打印服务端推来的 pev/region_ev。

和 ws_smoke.py 的分工：smoke 验协议面（每条命令的回包），这个脚本负责「持续按住」——
因为它按住的时候你在屏幕上按物理手指，合并设备流里才会同时出现虚拟 id 与物理 id，
再用 tests/id_split_check.py 断死「两段不重叠」。

为什么是「保持 + 小幅移动」而不是「按下不动」：stationary 触摸会被系统判成长按（可能弹出菜单），
小幅移动既避免长按，又顺带测到 move 路径；身份（oslot/oid）在这期间必须一直不变。

用法:
    adb forward tcp:27183 tcp:27183
    python build/_inject_hold.py --hold 120 --slot 0 --x 720 --y 1584
"""
import argparse
import base64
import hashlib
import math
import os
import socket
import struct
import sys
import time

GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


class WS:
    def __init__(self, host, port, timeout=3.0):
        self.s = socket.create_connection((host, port), timeout)
        self.s.settimeout(timeout)
        key = base64.b64encode(os.urandom(16)).decode()
        req = ("GET / HTTP/1.1\r\nHost: %s:%d\r\nUpgrade: websocket\r\n"
               "Connection: Upgrade\r\nSec-WebSocket-Key: %s\r\n"
               "Sec-WebSocket-Version: 13\r\n\r\n" % (host, port, key))
        self.s.sendall(req.encode())
        buf = b""
        while b"\r\n\r\n" not in buf:
            chunk = self.s.recv(1)
            if not chunk:
                raise RuntimeError("握手时连接断开")
            buf += chunk
        head = buf.decode("latin1")
        got = ""
        for line in head.split("\r\n"):
            if line.lower().startswith("sec-websocket-accept"):
                got = line.split(":", 1)[1].strip()
        want = base64.b64encode(hashlib.sha1((key + GUID).encode()).digest()).decode()
        if got != want:
            raise RuntimeError("accept 不匹配: %r != %r" % (got, want))
        self.buf = b""

    def send(self, text):
        data = text.encode()
        mask = os.urandom(4)
        n = len(data)
        head = bytearray([0x81])
        if n < 126:
            head.append(0x80 | n)
        else:
            head.append(0x80 | 126)
            head += struct.pack(">H", n)
        masked = bytes(b ^ mask[i & 3] for i, b in enumerate(data))
        self.s.sendall(bytes(head) + mask + masked)

    def recv(self, timeout=0.2):
        """返回一行文本；没有新消息返回 ""；连接关闭返回 None。
        半包不算错误：已到的字节留在 self.buf 里，下次调用接着读（timeout 只是「暂时没有」）。"""
        self.s.settimeout(timeout)
        try:
            while len(self.buf) < 2:
                chunk = self.s.recv(4096)
                if not chunk:
                    return None
                self.buf += chunk
            b0, b1 = self.buf[0], self.buf[1]
            ln = b1 & 0x7F
            off = 2
            if ln == 126:
                while len(self.buf) < 4:
                    chunk = self.s.recv(4096)
                    if not chunk:
                        return None
                    self.buf += chunk
                ln = struct.unpack(">H", self.buf[2:4])[0]
                off = 4
            while len(self.buf) < off + ln:
                chunk = self.s.recv(4096)
                if not chunk:
                    return None
                self.buf += chunk
        except (socket.timeout, TimeoutError):
            return ""
        payload = self.buf[off:off + ln]
        self.buf = self.buf[off + ln:]
        op = b0 & 0x0F
        if op == 0x8:
            return None
        return payload.decode("utf-8", "replace")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=27183)
    ap.add_argument("--slot", type=int, default=0)
    ap.add_argument("--x", type=int, default=720)
    ap.add_argument("--y", type=int, default=1584)
    ap.add_argument("--hold", type=float, default=120.0, help="保持多少秒（期间小幅移动）")
    ap.add_argument("--radius", type=int, default=40)
    args = ap.parse_args()

    print("[inject] 连接 ws://%s:%d" % (args.host, args.port), flush=True)
    w = WS(args.host, args.port)
    for cmd in ("res", "sub all"):
        w.send(cmd)
        print("[inject] > %-10s < %r" % (cmd, w.recv(1.0)), flush=True)

    w.send("down %d %d %d" % (args.slot, args.x, args.y))
    resp = w.recv(1.0)
    print("[inject] > down %d %d %d  < %r" % (args.slot, args.x, args.y, resp), flush=True)
    if resp != "ok":
        print("[inject] 注入失败，退出", flush=True)
        return 1

    t0 = time.time()
    pushes = []
    i = 0
    while time.time() - t0 < args.hold:
        ang = (i % 24) * math.pi / 12.0
        px = int(args.x + args.radius * math.cos(ang))
        py = int(args.y + args.radius * math.sin(ang))
        w.send("move %d %d %d" % (args.slot, px, py))
        r = w.recv(0.3)
        if isinstance(r, str) and r and r != "ok" and not r.startswith("pev") and not r.startswith("region_ev"):
            print("[inject] > move < %r（异常）" % r, flush=True)
        # 把服务端推来的事件读出来（pev 会暴露物理槽号）
        while True:
            m = w.recv(0.05)
            if not m:
                break
            if m and m != "ok":
                pushes.append(m)
                if len(pushes) <= 40:
                    print("[inject] 推送 < %s" % m, flush=True)
        i += 1
        time.sleep(0.25)

    w.send("up %d" % args.slot)
    print("[inject] > up %d  < %r" % (args.slot, w.recv(1.0)), flush=True)
    print("[inject] 共收到 %d 条推送（前 40 条见上）" % len(pushes), flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
