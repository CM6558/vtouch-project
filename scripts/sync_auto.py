#!/usr/bin/env python3

# -*- coding: utf-8 -*-

"""  

sync_auto.py — 一键把本地改动同步到 GitHub（浏览器扩展 WS 通道, 零 git push / 零 MCP / 零 CDP 端口）



架构:

  本脚本在 0.0.0.0:9336 开极简 WebSocket server。

  Chrome 扩展 (extension/sync-ext) 的 offscreen 文档（常驻, 不受 SW 休眠影响）

  通过 WS 连接本 server —— 扩展请求强制走代理隧道, 故用本机局域网 IP

  (10.164.120.30, 代理放行内网) 而非 127.0.0.1 (代理 CONNECT 拦截)。

  offscreen 收任务后经 chrome.runtime 转发 SW 执行 GitHub 编辑页提交,

  登录态天然可用 (用户已登录的 Chrome)。



用法（每次同步）:

  1. python scripts/sync_web.py --json D:\\sync-manifest.json --no-open   # 生成清单

  2. python scripts/sync_auto.py --manifest D:\\sync-manifest.json        # 一键提交



前置（一次性）:

  chrome://extensions -> 开发者模式 -> 加载已解压的扩展 -> 选 extension/sync-ext

  （若本机局域网 IP 变化, 改 extension/sync-ext/{manifest.json,offscreen.js} 的 10.164.120.30）



依赖: 仅 Python 3 标准库 + Chrome + 已加载扩展。

"""

import argparse

import base64

import hashlib

import json

import os

import socket

import sys

import time



WS_PORT = 9336

REPO_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

DEFAULT_MANIFEST = os.path.join(os.path.dirname(REPO_DIR), "sync-manifest.json")





# ---------------- 极简 WebSocket SERVER (纯标准库) ----------------

class WSServer:

    """只服务一个扩展客户端 (断线重连时 accept 新连接)."""



    def __init__(self, port):

        self.port = port

        self.sock = None

        self.conn = None

        self.buf = b""



    def listen(self):

        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)

        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)

        # 绑 0.0.0.0: 扩展经代理 CONNECT 隧道连本机局域网 IP 时, 目的地址是本机网卡 IP

        self.sock.bind(("0.0.0.0", self.port))

        self.sock.listen(1)

        self.sock.settimeout(180)



    def accept(self, timeout=90):

        deadline = time.time() + timeout

        while time.time() < deadline:

            try:

                conn, _ = self.sock.accept()

            except socket.timeout:

                raise RuntimeError("等待扩展连接超时 (请确认扩展已加载, chrome://extensions)")

            conn.settimeout(60)

            req = b""

            while b"\r\n\r\n" not in req:

                chunk = conn.recv(4096)

                if not chunk:

                    break

                req += chunk

            key = None

            for line in req.decode("utf-8", "replace").split("\r\n"):

                if line.lower().startswith("sec-websocket-key:"):

                    key = line.split(":", 1)[1].strip()

            if not key:

                conn.close()

                continue

            accept = base64.b64encode(

                hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest()

            ).decode()

            conn.sendall((

                "HTTP/1.1 101 Switching Protocols\r\n"

                "Upgrade: websocket\r\n"

                "Connection: Upgrade\r\n"

                f"Sec-WebSocket-Accept: {accept}\r\n\r\n"

            ).encode())

            self.conn = conn

            return True

        raise RuntimeError("等待扩展连接超时")



    def _read_exact(self, n):

        """确保 self.buf 至少有 n 字节 (只填充, 不移出)."""

        while len(self.buf) < n:

            chunk = self.conn.recv(65536)

            if not chunk:

                raise RuntimeError("扩展连接关闭")

            self.buf += chunk



    def recv_msg(self):

        fragment = b""

        while True:

            if len(self.buf) < 2:

                self._read_exact(2 - len(self.buf))

            b0, b1 = self.buf[0], self.buf[1]

            fin = b0 & 0x80

            opcode = b0 & 0x0F

            masked = b1 & 0x80

            ln = b1 & 0x7F

            off = 2

            if ln == 126:

                self._read_exact(off + 2 - len(self.buf))

                ln = int.from_bytes(self.buf[off:off + 2], "big")

                off += 2

            elif ln == 127:

                self._read_exact(off + 8 - len(self.buf))

                ln = int.from_bytes(self.buf[off:off + 8], "big")

                off += 8

            mlen = 4 if masked else 0

            self._read_exact(off + mlen + ln - len(self.buf))

            if masked:

                mask = self.buf[off:off + 4]

                payload = bytes(b ^ mask[i % 4] for i, b in enumerate(self.buf[off + 4:off + 4 + ln]))

            else:

                payload = self.buf[off:off + ln]

            self.buf = self.buf[off + mlen + ln:]

            if opcode == 8:

                raise RuntimeError("扩展关闭连接")

            if opcode == 9:

                self.send_msg({"__pong": True})

                continue

            if opcode == 0:

                fragment += payload

                if fin:

                    return json.loads(fragment.decode("utf-8"))

                continue

            if not fin:

                fragment = payload

                continue

            if opcode == 1:

                return json.loads(payload.decode("utf-8"))



    def send_msg(self, obj):

        payload = json.dumps(obj, ensure_ascii=False).encode("utf-8")

        n = len(payload)

        head = bytes([0x81])

        if n < 126:

            head += bytes([n])

        elif n < 65536:

            head += bytes([126]) + n.to_bytes(2, "big")

        else:

            head += bytes([127]) + n.to_bytes(8, "big")

        self.conn.sendall(head + payload)



    def close(self):

        try:

            if self.conn:

                self.conn.close()

        except Exception:

            pass

        try:

            if self.sock:

                self.sock.close()

        except Exception:

            pass



    def rpc(self, msg, timeout=240):

        if "id" not in msg:

            msg["id"] = int(time.time() * 1000) % 1000000

        self.send_msg(msg)

        deadline = time.time() + timeout

        while time.time() < deadline:

            try:

                resp = self.recv_msg()

            except socket.timeout:

                continue

            if resp.get("__pong"):

                continue

            if resp.get("id") == msg["id"]:

                return resp

        raise RuntimeError("扩展响应超时")





# ---------------- main ----------------

def main():

    ap = argparse.ArgumentParser(description="一键网页同步: 本地改动 -> GitHub (扩展 WS 通道)")

    ap.add_argument("--manifest", default=None, help="sync_web.py --json 清单路径")

    ap.add_argument("--port", type=int, default=WS_PORT)

    ap.add_argument("--only", default=None, help="只处理路径包含该子串的文件")

    ap.add_argument("--login-check", action="store_true", help="只检查登录态后退出")

    args = ap.parse_args()



    manifest_path = args.manifest or DEFAULT_MANIFEST

    if not os.path.exists(manifest_path):

        print(f"[sync_auto] ✗ 清单不存在: {manifest_path}\n"

              "  先运行: python scripts/sync_web.py --json <path> --no-open", file=sys.stderr)

        return 2

    m = json.load(open(manifest_path, encoding="utf-8"))

    repo = m.get("repo", "CM6558/vtouch-project")

    branch = m.get("branch", "master")

    updates = m.get("updates", [])

    deletes = m.get("deletes", [])

    binaries = m.get("binaries", [])

    print(f"[sync_auto] {repo} @ {branch}: 更新 {len(updates)} 删除 {len(deletes)} 二进制 {len(binaries)}")



    print(f"[sync_auto] 开启本地 WS 通道 0.0.0.0:{args.port} (等扩展连接, 最多 90s)...")

    srv = WSServer(args.port)

    srv.listen()

    try:

        user = ""

        while True:  # 断线自动重连 (扩展重载/代理隧道超时都会断)

            try:

                srv.accept(timeout=90)

            except RuntimeError as e:

                print(f"[sync_auto] ✗ {e}", file=sys.stderr)

                return 2

            print("[sync_auto] 扩展已连接 ✓")

            # check_login 可能因代理抖动返回空 (页面加载失败), 重试 4 次

            for attempt in range(4):

                try:

                    r = srv.rpc({"action": "check_login"}, timeout=90)

                    user = r.get("user", "")

                    if user:

                        break

                    print(f"[sync_auto] 登录检查为空 (第 {attempt + 1} 次), 重试...")

                except RuntimeError as e:

                    print(f"[sync_auto] 连接断开, 等待重连: {e}")

                    try:

                        srv.conn.close()

                    except Exception:

                        pass

                    srv.conn = None

                    srv.buf = b""

                    break  # 外层 while 会重新 accept

            if not user:

                if srv.conn is None:

                    continue  # 连接断了, 重新 accept

                print("[sync_auto] ✗ 多次检查 GitHub 未登录 (请确认 Chrome 已登录 github.com)", file=sys.stderr)

                return 2

            break  # 登录确认后不再重连

        print(f"[sync_auto] GitHub 登录: {user}")



        if args.login_check:

            return 0



        ok, fail = [], []

        for i, u in enumerate(updates, 1):

            path = u["path"]

            if args.only and args.only not in path:

                print(f"  [{i}/{len(updates)}] 跳过 {path} (--only 过滤)")

                continue

            print(f"  [{i}/{len(updates)}] {u['status']} {path} ...", end=" ", flush=True)

            try:

                r = srv.rpc({

                    "action": "commit",

                    "path": path,

                    "content": u["content"],

                    "is_new": u["status"] == "A",

                    "repo": repo,

                    "branch": branch,

                })

                if r.get("ok"):

                    ok.append(path)

                    print("✓")

                else:

                    fail.append((path, r.get("error", "?")))

                    print(f"✗ {r.get('error', '?')}")

            except Exception as e:

                fail.append((path, str(e)))

                print(f"✗ {e}")



        for d in deletes:

            print(f"  [del] {d['path']} — 需手动: https://github.com/{repo}/delete/{branch}/{d['path']}")

        for b in binaries:

            print(f"  [bin] {b['path']} — 跳过 (需手动网页上传)")



        print()

        print(f"=== 完成: 成功 {len(ok)} 失败 {len(fail)} ===")

        for p, e in fail:

            print(f"  ✗ {p}: {e}")

        return 0 if not fail else 1

    finally:

        srv.close()





if __name__ == "__main__":

    sys.exit(main())

