#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
sync_auto.py — 一键把本地改动同步到 GitHub（网页提交，零 git push、零 MCP 依赖）

设计目标（对应历史教训）:
  * 不干扰正在使用的 MCP chrome-devtools 服务 / WebStorm CDP / 用户日常 Chrome:
      本脚本启动【独立】Chrome 实例（专用 user-data-dir + 独立端口 9334），
      与 38433(MCP)/9222/9223(WebStorm) 等端口完全隔离，关闭时不留进程。
  * 不需要 git push: 复用 sync_web.py 的 --json 清单，逐文件走 GitHub 网页编辑提交。
  * 首次运行需在专用浏览器窗口手动登录一次 GitHub，之后登录态持久。

架构（零 session 依赖）:
  每个操作页通过 HTTP /json/new 创建独立标签页，并直连该 page 的 WebSocket
  （page 级连接天然是该 target 的会话，无需 Target.attachToTarget / sessionId），
  与其它任何 CDP 客户端（MCP/WebStorm）完全隔离，互不干扰。

用法:
  1. python scripts/sync_web.py --json D:\\sync-manifest.json --no-open   # 生成清单
  2. python scripts/sync_auto.py --manifest D:\\sync-manifest.json        # 一键提交
     可选: --port 9334 / --profile D:\\MYP\\.github-sync-profile / --keep-browser

流程（每文件）:
  编辑页/new 页 -> CDP 注入内容(execCommand 全选替换, 已验证可靠)
  -> 点 Commit changes... -> 填提交信息 -> 确认 -> 验证 URL 跳转到 blob/tree。

依赖: 仅 Python 3 标准库 + 本机 Chrome。
"""
import argparse
import base64
import json
import os
import socket
import subprocess
import sys
import time
import urllib.request
import urllib.parse

# ---------------- 配置 ----------------
DEFAULT_PORT = 9334
DEFAULT_PROFILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".github-sync-profile")
CHROME_EXE = r"C:\Program Files\Google\Chrome\Application\chrome.exe"


# ---------------- 极简 WebSocket + CDP (page 级, 纯标准库) ----------------
class Page:
    """一个独立标签页: 经 /json/new 创建, 直连其 WebSocket, 无需 sessionId."""

    def __init__(self, port, target_id, sock):
        self.port = port
        self.target_id = target_id
        self.sock = sock
        self.buf = b""
        self.msg_id = 0

    # -- WS 层 --
    def _read_exact(self, n):
        while len(self.buf) < n:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise RuntimeError("WS 连接关闭")
            self.buf += chunk
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def _recv_msg(self):
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
                raise RuntimeError("WS 关闭帧")
            if opcode == 9:
                self._send(0xA, payload)
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
            # binary/未知: 忽略

    def _send(self, opcode, payload: bytes):
        head = bytes([0x80 | opcode])
        n = len(payload)
        if n < 126:
            head += bytes([0x80 | n])
        elif n < 65536:
            head += bytes([0x80 | 126]) + n.to_bytes(2, "big")
        else:
            head += bytes([0x80 | 127]) + n.to_bytes(8, "big")
        mask = os.urandom(4)
        masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
        self.sock.sendall(head + mask + masked)

    # -- CDP 层 --
    def call(self, method, params=None):
        self.msg_id += 1
        mid = self.msg_id
        self._send(1, json.dumps({"id": mid, "method": method, "params": params or {}}).encode("utf-8"))
        deadline = time.time() + 30
        while time.time() < deadline:
            frame = self._recv_msg()
            if frame.get("id") == mid:
                if "error" in frame:
                    raise RuntimeError(f"CDP {method} 失败: {frame['error']}")
                return frame.get("result", {})
        raise RuntimeError(f"CDP {method} 超时")

    def eval(self, expr, await_promise=False):
        r = self.call("Runtime.evaluate",
                      {"expression": expr, "returnByValue": True,
                       "awaitPromise": await_promise, "userGesture": True})
        if "exceptionDetails" in r:
            raise RuntimeError(f"页面 JS 异常: {r['exceptionDetails'].get('text', '')}")
        return r.get("result", {}).get("value")

    def close(self):
        try:
            self.sock.close()
        except Exception:
            pass
        try:
            urllib.request.urlopen(f"http://127.0.0.1:{self.port}/json/close/{self.target_id}", timeout=3)
        except Exception:
            pass


def http_json(port, method, path):
    req = urllib.request.Request(f"http://127.0.0.1:{port}{path}", method=method)
    with urllib.request.urlopen(req, timeout=10) as r:
        data = r.read()
        return json.loads(data.decode("utf-8")) if data else None


def new_page(port, url):
    """HTTP 创建标签页并直连其 WebSocket; 返回 Page."""
    info = http_json(port, "PUT", f"/json/new?url={urllib.parse.quote(url, safe='')}")
    ws_url = info["webSocketDebuggerUrl"]
    target_id = info["id"]
    u = urllib.parse.urlparse(ws_url)
    sock = socket.create_connection(("127.0.0.1", port), timeout=10)
    key = base64.b64encode(os.urandom(16)).decode()
    path = u.path + (("?" + u.query) if u.query else "")
    sock.sendall((f"GET {path} HTTP/1.1\r\n"
                  f"Host: 127.0.0.1:{port}\r\n"
                  "Upgrade: websocket\r\nConnection: Upgrade\r\n"
                  f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n").encode())
    resp = b""
    while b"\r\n\r\n" not in resp:
        chunk = sock.recv(4096)
        if not chunk:
            raise RuntimeError("WS 握手失败")
        resp += chunk
    if b"101" not in resp.split(b"\r\n", 1)[0]:
        raise RuntimeError(f"WS 握手失败: {resp[:120]!r}")
    return Page(port, target_id, sock)


# ---------------- 浏览器管理 ----------------
def is_port_open(port):
    try:
        s = socket.create_connection(("127.0.0.1", port), timeout=1.5)
        s.close()
        return True
    except OSError:
        return False


def ensure_browser(port, profile):
    """若 port 无 Chrome 则启动独立实例; 返回 (是否新启动, chrome_pid)."""
    started, pid = False, None
    if not is_port_open(port):
        os.makedirs(profile, exist_ok=True)
        p = subprocess.Popen([
            CHROME_EXE,
            f"--remote-debugging-port={port}",
            f"--user-data-dir={profile}",
            "--no-first-run", "--no-default-browser-check",
            "about:blank",
        ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        pid = p.pid
        started = True
        for _ in range(60):
            if is_port_open(port):
                break
            time.sleep(0.5)
        else:
            raise RuntimeError("Chrome 独立实例启动失败(端口未就绪)")
        time.sleep(2)
    return started, pid


def check_login(port, timeout=120):
    """开新页访问 github.com 检查登录态; 返回用户名或 None."""
    pg = new_page(port, "https://github.com/")
    try:
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                pg.call("Page.enable")
                user = pg.eval(
                    "document.querySelector('meta[name=\"user-login\"]')?.content || ''")
                if user:
                    return user
            except Exception:
                pass
            time.sleep(2)
        return None
    finally:
        pg.close()


# ---------------- 提交单个文件 ----------------
def commit_file(port, path, content, is_new, branch="master", repo="CM6558/vtouch-project"):
    base = f"https://github.com/{repo}"
    url = f"{base}/new/{branch}?filename={path}" if is_new else f"{base}/edit/{branch}/{path}"
    pg = new_page(port, url)
    try:
        # 1) 等编辑器出现 (page 级 WS: 页面事件不自动派发, 直接轮询 evaluate)
        deadline = time.time() + 40
        while time.time() < deadline:
            try:
                if pg.eval("!!document.querySelector('.cm-content')"):
                    break
            except Exception:
                pass
            time.sleep(0.5)
        else:
            raise RuntimeError("编辑器加载超时")

        # 2) 全选 + execCommand 注入内容 (对 CM6 contenteditable 可靠)
        payload = json.dumps(content, ensure_ascii=False)
        r = pg.eval(f"""
(() => {{
  const ce = document.querySelector('.cm-content');
  if (!ce) return 'no-editor';
  ce.focus();
  const r = document.createRange(); r.selectNodeContents(ce);
  const sel = window.getSelection(); sel.removeAllRanges(); sel.addRange(r);
  const s = {payload};
  document.execCommand('insertText', false, s);
  return 'inj:' + ce.innerText.length;
}})();
""")
        if r == "no-editor":
            raise RuntimeError("编辑器未找到")
        time.sleep(0.8)

        # 3) 点 Commit changes... (等按钮启用)
        deadline = time.time() + 25
        clicked = False
        while time.time() < deadline:
            ok = pg.eval("""
(() => {
  const b = Array.from(document.querySelectorAll('button'))
    .find(x => x.textContent.trim() === 'Commit changes...');
  if (b && !b.disabled) { b.click(); return 'clicked'; }
  return 'wait';
})();
""")
            if ok == "clicked":
                clicked = True
                break
            time.sleep(0.4)
        if not clicked:
            raise RuntimeError("Commit 按钮未就绪")
        time.sleep(0.6)

        # 4) 对话框: 填提交信息 + 确认
        msg = f"sync: {path}" if not is_new else f"feat: {path}"
        msg_js = json.dumps(msg, ensure_ascii=False)
        deadline = time.time() + 25
        done = False
        while time.time() < deadline:
            r = pg.eval(f"""
(() => {{
  const d = document.querySelector('[role="dialog"]');
  const inp = d && d.querySelector('input[placeholder^="Update"], input[placeholder^="Create"]');
  if (!inp) return 'wait';
  const setter = Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value').set;
  setter.call(inp, {msg_js});
  inp.dispatchEvent(new Event('input', {{ bubbles: true }}));
  const confirm = Array.from(d.querySelectorAll('button'))
    .find(x => x.textContent.trim() === 'Commit changes');
  if (confirm) {{ confirm.click(); return 'committed'; }}
  return 'no-confirm';
}})();
""")
            if r == "committed":
                done = True
                break
            if r == "no-confirm":
                raise RuntimeError("提交对话框无确认按钮")
            time.sleep(0.4)
        if not done:
            raise RuntimeError("提交对话框超时")

        # 5) 验证跳转到 blob/tree
        deadline = time.time() + 15
        while time.time() < deadline:
            href = pg.eval("location.href") or ""
            if "/blob/" in href or "/tree/" in href:
                return True, href
            time.sleep(0.8)
        raise RuntimeError(f"提交后未跳转 (当前: {(pg.eval('location.href') or '')[:100]})")
    finally:
        pg.close()


# ---------------- main ----------------
def main():
    ap = argparse.ArgumentParser(description="一键网页同步: 本地改动 -> GitHub (无 push / 无 MCP)")
    ap.add_argument("--manifest", required=True, help="sync_web.py --json 生成的清单路径")
    ap.add_argument("--port", type=int, default=DEFAULT_PORT)
    ap.add_argument("--profile", default=DEFAULT_PROFILE)
    ap.add_argument("--keep-browser", action="store_true", help="提交完成后不关闭独立 Chrome")
    ap.add_argument("--only", default=None, help="只处理路径包含该子串的文件")
    ap.add_argument("--login-timeout", type=int, default=120, help="等待手动登录秒数")
    args = ap.parse_args()

    m = json.load(open(args.manifest, encoding="utf-8"))
    repo = m.get("repo", "CM6558/vtouch-project")
    branch = m.get("branch", "master")
    updates = m.get("updates", [])
    deletes = m.get("deletes", [])
    binaries = m.get("binaries", [])
    print(f"[sync_auto] {repo} @ {branch}: 更新 {len(updates)} 删除 {len(deletes)} 二进制 {len(binaries)}")

    started, chrome_pid = ensure_browser(args.port, args.profile)
    print(f"[sync_auto] 独立 Chrome 就绪 (端口 {args.port}, 新启动={started})")

    # 登录检查
    user = check_login(args.port, timeout=args.login_timeout)
    if not user:
        print("[sync_auto] ✗ 未检测到 GitHub 登录。请在打开的独立 Chrome 窗口手动登录一次"
              "（登录态保存在专用 profile，下次无需再登录），然后重跑本脚本。")
        return 2
    print(f"[sync_auto] GitHub 登录: {user}")

    ok, fail = [], []
    for i, u in enumerate(updates, 1):
        path = u["path"]
        if args.only and args.only not in path:
            print(f"  [{i}/{len(updates)}] 跳过 {path} (--only 过滤)")
            continue
        print(f"  [{i}/{len(updates)}] {u['status']} {path} ...", end=" ", flush=True)
        try:
            _, href = commit_file(args.port, path, u["content"], u["status"] == "A",
                                  branch=branch, repo=repo)
            ok.append(path)
            print("✓")
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

    if not args.keep_browser and chrome_pid:
        try:
            subprocess.run(["taskkill", "/F", "/PID", str(chrome_pid)],
                           capture_output=True, timeout=10)
            print("[sync_auto] 独立 Chrome 已关闭")
        except Exception:
            pass
    return 0 if not fail else 1


if __name__ == "__main__":
    sys.exit(main())
