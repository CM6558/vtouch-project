#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
sync_auto.py — 一步静默后台同步: 本地改动 -> GitHub 网页提交 (零 git, 零 MCP, 零额外登录)

架构:
  本脚本扫描仓库受管文本文件, 与本地缓存 (sync-state.json) 比对 sha256,
  变化的文件经 Chrome 扩展通道逐个网页提交:
    Python WS server (0.0.0.0:9336) -> 扩展 offscreen 文档常驻持 WS (SW 休眠免疫)
    -> runtime.sendMessage 转发 SW -> executeScript(MAIN world) 编辑页注入
    -> CM6 -> Commit -> 验证跳转。登录态天然可用 (用户已登录的 Chrome)。

  * 完全不用 git: 无 fetch/diff/status/commit/push。
  * 幂等: 内容 sha256 未变则跳过 (不覆盖远端, 远端被手动改过也安全)。
  * 删除/二进制天然不处理 (远端残留无害; zip 等按扩展名排除)。

用法 (一步):
  python scripts/sync_auto.py                          # 前台, 打印进度
  pythonw scripts/sync_auto.py --silent                # 静默后台, 日志写 D:\\MYP\\sync-auto.log
  pythonw scripts/sync_auto.py --silent --watch 600    # 常驻, 每 10 分钟自动同步一次

前置 (一次性):
  chrome://extensions -> 开发者模式 -> 加载已解压的扩展 -> 选 extension/sync-ext
  (若本机局域网 IP 变化, 改 extension/sync-ext/{manifest.json,offscreen.js} 的 10.164.120.30)

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
REPO = "CM6558/vtouch-project"
BRANCH = "master"
OUT_DIR = os.path.dirname(REPO_DIR)          # D:\MYP
STATE_FILE = os.path.join(OUT_DIR, "sync-state.json")
LOCK_FILE = os.path.join(OUT_DIR, ".sync-auto.lock")
LOG_FILE = os.path.join(OUT_DIR, "sync-auto.log")

# 受管目录 (相对 REPO_DIR) 与排除项
MANAGED_DIRS = ["scripts", "extension", "src", "docs", "clients"]
MANAGED_FILES = ["README.md"]
TEXT_EXTS = {".py", ".js", ".java", ".xml", ".md", ".c", ".h", ".mk", ".json",
             ".html", ".txt", ".bat", ".yml", ".yaml", ".toml", ".cfg", ".sh",
             ".gradle", ".properties", ".css", ".ts"}
EXCLUDE_DIRS = {".git", "build", "__pycache__", ".github-sync-profile",
                "node_modules", "dist", ".idea", ".vscode"}
MAX_SIZE = 512 * 1024

SILENT = False


def log(msg, err=False):
    """silent 时写日志文件, 否则打印."""
    ts = time.strftime("%Y-%m-%d %H:%M:%S")
    if SILENT:
        try:
            with open(LOG_FILE, "a", encoding="utf-8") as f:
                f.write(f"[{ts}] {msg}\n")
        except Exception:
            pass
    else:
        print(msg, file=sys.stderr if err else sys.stdout, flush=True)


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def scan_repo():
    """扫描受管目录下文本文件, 返回 {relpath: sha256hex}."""
    files = {}
    roots = [os.path.join(REPO_DIR, d) for d in MANAGED_DIRS]
    roots += [os.path.join(REPO_DIR, f) for f in MANAGED_FILES]
    for root in roots:
        if os.path.isfile(root):
            rel = os.path.relpath(root, REPO_DIR).replace("\\", "/")
            files[rel] = sha256_file(root)
            continue
        if not os.path.isdir(root):
            continue
        for dirpath, dirnames, filenames in os.walk(root):
            dirnames[:] = [d for d in dirnames if d not in EXCLUDE_DIRS]
            for fn in filenames:
                ext = os.path.splitext(fn)[1].lower()
                if ext not in TEXT_EXTS:
                    continue
                fp = os.path.join(dirpath, fn)
                try:
                    if os.path.getsize(fp) > MAX_SIZE:
                        continue
                except OSError:
                    continue
                rel = os.path.relpath(fp, REPO_DIR).replace("\\", "/")
                files[rel] = sha256_file(fp)
    return files


def load_state():
    try:
        with open(STATE_FILE, encoding="utf-8") as f:
            return json.load(f)
    except Exception:
        return {}


def save_state(state):
    try:
        with open(STATE_FILE, "w", encoding="utf-8") as f:
            json.dump(state, f, ensure_ascii=False, indent=1)
    except Exception as e:
        log(f"[sync_auto] ✗ 缓存写入失败: {e}", err=True)


def acquire_lock():
    if os.path.exists(LOCK_FILE):
        try:
            pid = int(open(LOCK_FILE).read().strip())
            os.kill(pid, 0)  # 仅检测存活 (Windows 上多数情况抛异常)
            log(f"[sync_auto] 已有实例运行 (pid {pid}), 退出", err=True)
            sys.exit(2)
        except (ValueError, OSError):
            pass  # 锁文件残留 (进程已死), 覆盖
    with open(LOCK_FILE, "w") as f:
        f.write(str(os.getpid()))


def release_lock():
    try:
        os.remove(LOCK_FILE)
    except OSError:
        pass


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


# ---------------- 同步主流程 ----------------
def sync_once(srv, args, repo=REPO, branch=BRANCH, accept_timeout=90):
    """扫描 + 提交变化文件, 返回 (ok_list, fail_list).
    accept_timeout: 等扩展连接秒数 (watch 模式用短值, Chrome 未开时快速跳过)."""
    files = scan_repo()
    state = load_state()
    # 首次运行 (缓存为空): 用 git 只读对比一次性预填 (视为已同步的文件记入缓存,
    # 与远端有差异的留待提交)。此后完全不用 git。
    if not state:
        import subprocess
        changed = set()
        try:
            subprocess.run(["git", "fetch", "origin"], cwd=REPO_DIR,
                           capture_output=True, timeout=120)
            out = subprocess.run(["git", "diff", "origin/master", "--name-only"],
                                 cwd=REPO_DIR, capture_output=True, text=True, timeout=60).stdout
            changed = {l.strip().replace("\\", "/") for l in out.splitlines() if l.strip()}
        except Exception as e:
            log(f"[sync_auto] 首次预填跳过 (git 不可用): {e}", err=True)
        for path in files:
            if path not in changed:
                state[path] = files[path]
        if state:
            save_state(state)
            log(f"[sync_auto] 首次预填: {len(state)} 文件视为已同步, "
                f"{len(changed)} 文件与远端有差异留待提交")
    to_submit = []
    for path in sorted(files):
        if state.get(path) != files[path]:
            if args.only and args.only not in path:
                continue
            to_submit.append(path)
    log(f"[sync_auto] {repo} @ {branch}: 扫描 {len(files)} 文件, 待提交 {len(to_submit)}")

    if not to_submit:
        log("[sync_auto] 无变化, 跳过")
        return [], []

    if srv.conn is None:
        srv.accept(timeout=accept_timeout)
        log("[sync_auto] 扩展已连接 ✓")
    # else: 复用上一轮连接 (offscreen 常驻, watch 模式连续轮次不重连)
    # check_login 可能因代理抖动返回空 (页面加载失败), 重试 4 次
    user = ""
    for attempt in range(4):
        try:
            r = srv.rpc({"action": "check_login"}, timeout=90)
            user = r.get("user", "")
            if user:
                break
            log(f"[sync_auto] 登录检查为空 (第 {attempt + 1} 次), 重试...")
        except RuntimeError as e:
            log(f"[sync_auto] 连接断开, 等待重连: {e}", err=True)
            try:
                srv.conn.close()
            except Exception:
                pass
            srv.conn = None
            srv.buf = b""
            srv.accept(timeout=90)
            log("[sync_auto] 扩展重连 ✓")
    if not user:
        raise RuntimeError("多次检查 GitHub 未登录 (请确认 Chrome 已登录 github.com)")
    log(f"[sync_auto] GitHub 登录: {user}")

    ok, fail = [], []
    for i, path in enumerate(to_submit, 1):
        abs_path = os.path.join(REPO_DIR, path.replace("/", os.sep))
        try:
            with open(abs_path, "r", encoding="utf-8") as f:
                content = f.read()
        except Exception as e:
            fail.append((path, f"读取失败: {e}"))
            continue
        log(f"  [{i}/{len(to_submit)}] {path} ...")
        try:
            r = srv.rpc({
                "action": "commit",
                "path": path,
                "content": content,
                "is_new": path not in state,   # 无缓存记录 -> 按新建处理 (远端可能已存在, 但提交页自动识别)
                "repo": repo,
                "branch": branch,
            })
            if r.get("ok"):
                ok.append(path)
                state[path] = files[path]      # 提交成功才更新缓存
                save_state(state)
                log(f"  ✓ {path}")
            else:
                err_msg = r.get("error", "?")
                # "Commit 按钮未就绪" = 内容与远端相同, 视为无变化跳过 (幂等)
                if "按钮未就绪" in err_msg:
                    ok.append(path)
                    state[path] = files[path]
                    save_state(state)
                    log(f"  ~ {path} (无变化, 跳过)")
                else:
                    fail.append((path, err_msg))
                    log(f"  ✗ {path}: {err_msg}", err=True)
        except Exception as e:
            fail.append((path, str(e)))
            log(f"  ✗ {path}: {e}", err=True)
    log(f"[sync_auto] 本轮完成: 成功 {len(ok)} 失败 {len(fail)}")
    return ok, fail


def main():
    global SILENT
    ap = argparse.ArgumentParser(description="一步网页同步: 本地改动 -> GitHub (免 git, 扩展 WS 通道)")
    ap.add_argument("--silent", action="store_true", help="静默后台 (日志写 sync-auto.log, 无输出)")
    ap.add_argument("--port", type=int, default=WS_PORT)
    ap.add_argument("--watch", type=int, default=0,
                    help="常驻模式: 每 N 秒自动同步一次 (0=单次后退出)")
    ap.add_argument("--only", default=None, help="只处理路径包含该子串的文件 (调试用)")
    ap.add_argument("--repo", default=REPO)
    ap.add_argument("--branch", default=BRANCH)
    args = ap.parse_args()
    SILENT = args.silent

    acquire_lock()
    try:
        srv = WSServer(args.port)
        srv.listen()
        try:
            if args.watch:
                log(f"[sync_auto] 常驻模式: 每 {args.watch}s 同步一次 (Ctrl+C 退出)")
                while True:
                    try:
                        # Chrome 未开时快速跳过 (20s 等待), 减少日志噪音
                        sync_once(srv, args, args.repo, args.branch,
                                  accept_timeout=20 if args.silent else 60)
                    except RuntimeError as e:
                        log(f"[sync_auto] ✗ {e}", err=True)
                        try:
                            srv.conn.close()
                        except Exception:
                            pass
                        srv.conn = None
                        srv.buf = b""
                    time.sleep(args.watch)
            else:
                ok, fail = sync_once(srv, args, args.repo, args.branch)
                return 0 if not fail else 1
        finally:
            srv.close()
    except RuntimeError as e:
        log(f"[sync_auto] ✗ {e}", err=True)
        return 2
    finally:
        release_lock()


if __name__ == "__main__":
    sys.exit(main())
