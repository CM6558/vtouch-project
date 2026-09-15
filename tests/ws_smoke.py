#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""ws_smoke.py —— 主机侧 smoke test（只依赖标准库）。

测什么：daemon 的 WS 握手 + 注入命令链路（不下发任何真实触摸之外的东西）。
前提：设备上 vtouchd 已在跑，且已 `adb forward tcp:27183 tcp:27183`。

用法:
    adb forward tcp:27183 tcp:27183
    python tests/ws_smoke.py            # 退出码 0 = 全过
    python tests/ws_smoke.py --host 127.0.0.1 --port 27183 --no-inject   # 只测握手/查询，不发触摸

注意：注入用例会真的往系统里放触点（down→up 全程 ~几十毫秒），先在桌面/空白处跑。
"""
import argparse
import base64
import hashlib
import os
import socket
import struct
import sys

GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
oks, fails = [], []


def ok(msg):
    oks.append(msg)
    print("  ok   %s" % msg)


def bad(msg):
    fails.append(msg)
    print("  FAIL %s" % msg)


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
        if "101" not in head.split("\r\n")[0]:
            raise RuntimeError("不是 101：%s" % head.split("\r\n")[0])
        want = base64.b64encode(hashlib.sha1((key + GUID).encode()).digest()).decode()
        got = ""
        for line in head.split("\r\n"):
            if line.lower().startswith("sec-websocket-accept"):
                got = line.split(":", 1)[1].strip()
        if got != want:
            raise RuntimeError("accept 不匹配（服务端 %r vs 期望 %r）" % (got, want))
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

    def recv(self, timeout=1.0):
        self.s.settimeout(timeout)
        try:
            while len(self.buf) < 2:
                chunk = self.s.recv(4096)
                if not chunk:                      # EOF：服务端直接关连接（不空转）
                    raise RuntimeError("服务端关闭连接（EOF）")
                self.buf += chunk
            b0, b1 = self.buf[0], self.buf[1]
            ln = b1 & 127
            off = 2
            if ln == 126:
                while len(self.buf) < 4:
                    chunk = self.s.recv(4096)
                    if not chunk:
                        raise RuntimeError("服务端关闭连接（EOF）")
                    self.buf += chunk
                ln = struct.unpack(">H", self.buf[2:4])[0]
                off = 4
            while len(self.buf) < off + ln:
                chunk = self.s.recv(4096)
                if not chunk:
                    raise RuntimeError("服务端关闭连接（EOF）")
                self.buf += chunk
            if (b0 & 15) == 8:
                raise RuntimeError("服务端关闭连接")
            payload = self.buf[off:off + ln]
            self.buf = self.buf[off + ln:]
            return payload.decode("utf-8", "replace")
        except socket.timeout:
            return None

    def cmd(self, line, timeout=1.0):
        self.send(line)
        return self.recv(timeout)

    def close(self):
        try:
            self.s.close()
        except Exception:
            pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=27183)
    ap.add_argument("--no-inject", action="store_true", help="不下发 down/move/up（只测握手与查询）")
    a = ap.parse_args()

    print("连接 ws://%s:%d" % (a.host, a.port))
    try:
        ws = WS(a.host, a.port)
    except Exception as e:
        bad("握手失败: %s（daemon 在跑吗？adb forward 做了吗？）" % e)
        return 1
    ok("握手成功（101 + Sec-WebSocket-Accept 校验通过）")

    r = ws.cmd("ping")
    ok("ping → %r" % r) if r == "pong" else bad("ping 应答异常: %r" % r)

    r = ws.cmd("res")
    if r and r.startswith("res "):
        ok("res → %s" % r)
        try:
            lw, lh = int(r.split()[1]), int(r.split()[2])
        except Exception:
            lw, lh = 0, 0
    else:
        bad("res 应答异常: %r" % r)
        lw, lh = 0, 0

    r = ws.cmd("bogus")
    ok("未知命令 → %r" % r) if (r or "").startswith("err") else bad("未知命令应答异常: %r" % r)

    r = ws.cmd("point 0 down 10 10")
    ok("帧外 point 被拒 → %r" % r) if (r or "").startswith("err") else bad("帧外 point 竟然通过: %r" % r)

    if not a.no_inject and lw > 10 and lh > 10:
        cx, cy = lw // 2, lh // 2
        seq = [("down 0 %d %d" % (cx, cy), "ok"),
               ("move 0 %d %d" % (cx + 5, cy + 5), "ok"),
               ("up 0", "ok"),
               ("down 99 %d %d" % (cx, cy), "err"),
               ("up 0", "err")]
        for line, want in seq:
            r = ws.cmd(line)
            got = "ok" if (r or "").startswith("ok") else "err"
            (ok if got == want else bad)("%s → %r（期望 %s）" % (line, r, want))
        r = ws.cmd("begin_frame")
        (ok if (r or "").startswith("ok") else bad)("begin_frame → %r" % r)
        r = ws.cmd("point 0 down %d %d" % (cx, cy))
        (ok if (r or "").startswith("ok") else bad)("point 0 down → %r" % r)
        r = ws.cmd("point 1 down %d %d" % (cx + 40, cy))
        (ok if (r or "").startswith("ok") else bad)("point 1 down（同帧第二指）→ %r" % r)
        r = ws.cmd("point 0 move %d %d" % (cx + 1, cy + 1))
        (ok if (r or "").startswith("err") else bad)("同帧重复 slot 被拒 → %r" % r)
        r = ws.cmd("end_frame")
        (ok if (r or "").startswith("ok") else bad)("end_frame → %r" % r)
        for s in (0, 1):
            ws.cmd("up %d" % s)

    ws.close()
    print("\n结果: %d ok / %d fail" % (len(oks), len(fails)))
    if fails:
        print("smoke 未通过")
        return 1
    print("smoke 通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
