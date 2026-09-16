#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""circle_drive.py —— 主机侧「持续画圆」注入器（只依赖标准库）。

干什么：连上设备上跑着的 vtouchd（loopback WS，经 adb forward），让**一个虚拟触点**
按住不放、以固定转速画圆 —— 用来观察「虚拟手指 + 我自己的物理手指同时用」的效果。

用法:
    adb forward tcp:27183 tcp:27183
    python tests/circle_drive.py                       # 默认：屏幕中心、半径 300、0.5 圈/秒、60 帧/秒
    python tests/circle_drive.py --px 720 --py 900 --radius 260 --rps 0.6 --fps 60
    python tests/circle_drive.py --duration 30         # 跑 30 秒自动收手（0 = 一直跑，Ctrl-C 停）
    python tests/circle_drive.py --sub                 # 顺带订阅 region 事件，每秒打印事件速率
    python tests/circle_drive.py --per-frame           # 用 begin_frame/point/end_frame 成帧发（对照用）

坐标是**逻辑坐标**（daemon 启动时按 `wm size` 归一化，本机 = 1440x3168 竖屏）。
收尾一定有 `up`：正常结束、Ctrl-C、异常退出都会把虚拟触点抬起来（不然屏幕上会留一个按住的点）。
"""
import argparse
import base64
import hashlib
import math
import os
import socket
import struct
import sys
import threading
import time

GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


class WS:
    """最小 WebSocket 客户端：握手 + 掩码发送 + 后台收包线程（daemon 会回 ok / region 事件，
    不收就会把出站队列顶满，最后被 daemon 踢掉）。"""

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
            c = self.s.recv(1)
            if not c:
                raise RuntimeError("握手时连接断开")
            buf += c
        head = buf.decode("latin1")
        if "101" not in head.split("\r\n")[0]:
            raise RuntimeError("不是 101：%s" % head.split("\r\n")[0])
        want = base64.b64encode(hashlib.sha1((key + GUID).encode()).digest()).decode()
        got = ""
        for line in head.split("\r\n"):
            if line.lower().startswith("sec-websocket-accept"):
                got = line.split(":", 1)[1].strip()
        if got != want:
            raise RuntimeError("accept 不匹配")
        self.buf, self.msgs, self.stop = b"", [], False
        self.t = threading.Thread(target=self._read_loop, daemon=True)
        self.t.start()

    def _read_loop(self):
        while not self.stop:
            try:
                self.s.settimeout(0.5)
                data = self.s.recv(65536)
            except socket.timeout:
                continue
            except OSError:
                return
            if not data:
                return
            self.buf += data
            while len(self.buf) >= 2:
                b1, b2 = self.buf[0], self.buf[1]
                ln, off = b2 & 0x7F, 2
                if ln == 126:
                    if len(self.buf) < 4:
                        break
                    ln, off = struct.unpack(">H", self.buf[2:4])[0], 4
                elif ln == 127:
                    if len(self.buf) < 10:
                        break
                    ln, off = struct.unpack(">Q", self.buf[2:10])[0], 10
                if len(self.buf) < off + ln:
                    break
                payload, self.buf = self.buf[off:off + ln], self.buf[off + ln:]
                self.msgs.append(payload.decode("utf-8", "replace"))

    def send(self, text):
        if self.stop:
            raise ConnectionError("连接已关闭")
        data = text.encode()
        mask = os.urandom(4)
        n = len(data)
        head = bytearray([0x81])
        if n < 126:
            head.append(0x80 | n)
        else:
            head.append(0x80 | 126)
            head += struct.pack(">H", n)
        self.s.sendall(bytes(head) + mask + bytes(b ^ mask[i & 3] for i, b in enumerate(data)))

    def close(self):
        self.stop = True
        try:
            self.s.close()
        except OSError:
            pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=27183)
    ap.add_argument("--slot", type=int, default=0, help="虚拟触点槽号（0 = 第一路虚拟触点）")
    ap.add_argument("--px", type=int, default=720, help="圆心 x（逻辑坐标）")
    ap.add_argument("--py", type=int, default=1500, help="圆心 y（逻辑坐标）")
    ap.add_argument("--radius", type=int, default=300)
    ap.add_argument("--rps", type=float, default=0.5, help="每秒转几圈")
    ap.add_argument("--fps", type=int, default=60)
    ap.add_argument("--duration", type=float, default=0, help="跑多少秒（0 = 一直跑到 Ctrl-C）")
    ap.add_argument("--sub", action="store_true", help="订阅 region 事件并每秒打印速率")
    ap.add_argument("--per-frame", action="store_true", help="用 begin_frame/point/end_frame 发（对照）")
    a = ap.parse_args()

    ws = WS(a.host, a.port)
    errs = [0]

    def cmd(text, wait=0.6, quiet=False):
        """发一条命令并**等它真正的回包**（"ok" / "err xxx" / 数据行）。
        只回显自己发了什么 = 自欺欺人：daemon 回 err 也看不出来。"""
        before = len(ws.msgs)
        ws.send(text)
        t0 = time.time()
        while time.time() - t0 < wait:
            if len(ws.msgs) > before:
                resp = ws.msgs[before]
                if resp.startswith("err"):
                    errs[0] += 1
                    print("  !! 设备回 %r（命令 %r）" % (resp, text))
                elif not quiet:
                    print("  %-28s → %s" % (text, resp))
                return resp
            time.sleep(0.01)
        print("  ?? 等不到回包：%r" % text)
        errs[0] += 1
        return ""

    print("ping →", cmd("ping"))
    print("res  →", cmd("res"))   # daemon 自报：逻辑尺寸 + raw 轴范围
    if a.sub:
        cmd("sub region")

    x0, y0 = int(a.px + a.radius), int(a.py)
    cmd("down %d %d %d" % (a.slot, x0, y0))
    print("画圆：圆心(%d,%d) 半径%d · %.2f 圈/秒 · %d 帧/秒 · %s" %
          (a.px, a.py, a.radius, a.rps, a.fps,
           ("%.0f 秒" % a.duration) if a.duration else "Ctrl-C 停"))

    t0, n, last, ev_last, ev_total = time.time(), 0, time.time(), 0, 0
    try:
        while True:
            t = time.time() - t0
            if a.duration and t >= a.duration:
                break
            ang = 2 * math.pi * a.rps * t
            x = int(a.px + a.radius * math.cos(ang))
            y = int(a.py + a.radius * math.sin(ang))
            if a.per_frame:
                ws.send("begin_frame")
                ws.send("point %d move %d %d" % (a.slot, x, y))
                ws.send("end_frame")
            else:
                ws.send("move %d %d %d" % (a.slot, x, y))
            n += 1
            if n % 60 == 0:
                now = time.time()
                if now - last >= 1.0:
                    rate = n / (now - t0)
                    ev = len([m for m in ws.msgs if m.startswith("region_ev")])
                    extra = ""
                    if a.sub:
                        ev_total += ev - ev_last
                        ev_last = ev
                        extra = " · region 事件 %d（本轮 %d）" % (ev_total, ev)
                    print("  %4.1fs  帧率 %.1f  当前点 (%d,%d)%s" % (t, rate, x, y, extra))
                    last = now
            # 按帧率节流
            want = t0 + n / float(a.fps)
            sleep = want - time.time()
            if sleep > 0:
                time.sleep(sleep)
    except KeyboardInterrupt:
        print("\n收到 Ctrl-C")
    except (ConnectionError, OSError) as e:
        # daemon 被停掉 / 客户端被踢：连接会直接断。这不是异常场景，干净收场即可
        # （虚拟触点由 daemon 的 owner_reset 或设备销毁负责抬起，不要再硬发命令）。
        print("\n连接断开（daemon 停了或被新客户端踢掉）：%s" % e)
        ws.stop = True
        print("虚拟触点由 daemon 侧释放；如需手动抬指，连上后发 reset")
        return 2
    finally:
        try:
            cmd("up %d" % a.slot)
        except (ConnectionError, OSError):
            pass
        print("抬起槽 %d（已释放虚拟触点）· 命令错误 %d 次" % (a.slot, errs[0]))
        time.sleep(0.2)
        ws.close()
        return 1 if errs[0] else 0


if __name__ == "__main__":
    sys.exit(main())
