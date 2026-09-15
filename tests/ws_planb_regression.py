#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""ws_planb_regression.py —— Plan B 引擎的 AVD/真机回归（主机侧，只依赖标准库 + adb）。

对应 VTOUCH_ARCH_PLAN.md §8 步骤4 的四件：
  T1 命令面（含 region/sub/unsub 的错路径）      ← ws_smoke.py 的超集
  T2 订阅语义（不 sub 不推；sub phys/region/all 分道）
  T3 五事件示例（物理手指进出区域 → down/enter/move/exit/up）
  T4 回触示例（注入虚拟触点 → **不产生** pev/region_ev，不自激）
  T5 ws_kick（新连接踢旧连接，被踢方订阅清零）

前置：
  1) 设备上 vtouchd 已在跑（scripts/deploy.sh start），且已 `adb forward tcp:27183 tcp:27183`
  2) python tests/ws_planb_regression.py            # 退出码 0 = 全过
  3) 物理用例会往 daemon 抓着的触摸节点写事件（等价于真有手指在动）：
     - 需要 root（脚本默认走 `su -c`，AVD userdebug 可加 --no-su）
     - **跑之前别碰屏幕**：伪造手指与真人共用内核槽位
     - 无 root / 不想动物理链路时加 --skip-phys（跳过 T3，T4/T5 仍跑）

用法:
  python tests/ws_planb_regression.py                  # 全跑
  python tests/ws_planb_regression.py --skip-phys      # 不动物理触摸
  python tests/ws_planb_regression.py --dev /dev/input/event1 --scale 16   # 手工指定设备节点/换算
"""
import argparse
import os
import re
import socket
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ws_smoke import WS            # 复用握手/收发（同目录，零额外依赖）

oks, fails = [], []


def ok(msg):
    oks.append(msg)
    print("  ok   %s" % msg)


def bad(msg):
    fails.append(msg)
    print("  FAIL %s" % msg)


def check(cond, msg, extra=None):
    (ok if cond else bad)(msg + ("" if cond or extra is None else "（%r）" % (extra,)))


def section(t):
    print("\n== %s" % t)


def sh(cmd, timeout=15):
    """在设备上跑一条命令（默认 su -c）。返回 (rc, stdout)。"""
    argv = ["adb", "shell"] + (["su", "-c", cmd] if not sh.no_su else [cmd])
    try:
        p = subprocess.run(argv, capture_output=True, timeout=timeout)
        return p.returncode, (p.stdout or b"").decode("utf-8", "replace")
    except Exception as e:
        return 1, str(e)


sh.no_su = False


def drain(ws, secs=0.25, keep=False):
    """收干这一段时间内的所有推送行（keep=True 时返回收到的行）。"""
    got, t0 = [], time.time()
    while time.time() - t0 < secs:
        line = ws.recv(0.1)
        if line is not None:
            got.append(line)
    return got if keep else None


def wait_for(ws, pred, timeout=2.0):
    """等一条满足 pred 的推送；返回 (行, 期间收到的全部行)。"""
    seen, t0 = [], time.time()
    while time.time() - t0 < timeout:
        line = ws.recv(0.15)
        if line is None:
            continue
        seen.append(line)
        if pred(line):
            return line, seen
    return None, seen


def is_closed(ws, timeout=1.5):
    """旧连接是否已被服务端关掉：EOF 或 close 帧都算，没动静 = 还活着。"""
    ws.s.settimeout(timeout)
    try:
        data = ws.s.recv(64)
    except socket.timeout:
        return False
    except Exception:
        return True
    if data == b"":
        return True
    return bool(data) and (data[0] & 15) == 8


def evs(lines, kind):
    """从推送行里挑出 pev / region_ev 并结构化。"""
    out = []
    for l in lines:
        parts = l.split()
        if not parts or parts[0] != kind:
            continue
        if kind == "pev" and len(parts) >= 5:
            out.append({"slot": int(parts[1]), "ev": parts[2], "x": int(parts[3]), "y": int(parts[4])})
        elif kind == "region_ev" and len(parts) >= 6:
            out.append({"id": parts[1], "ev": parts[2], "slot": int(parts[3]),
                        "x": int(parts[4]), "y": int(parts[5])})
    return out


# ---------------------------------------------------------------- 物理触摸驱动
class Phys:
    """用 fake_touch.sh 伪造物理手指（EVIOCGRAB 只挡读者不挡写者）。"""

    def __init__(self, dev, scale, remote="/data/local/tmp/fake_touch.sh"):
        self.dev, self.scale, self.remote = dev, scale, remote

    def ready(self):
        rc, out = sh("test -f %s && echo yes" % self.remote)
        if "yes" in out:
            return True
        path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fake_touch.sh")
        p = subprocess.run(["adb", "push", path, self.remote], capture_output=True)
        rc, out = sh("test -f %s && echo yes" % self.remote)
        return "yes" in out

    def _run(self, args):
        rc, out = sh("D=%s SCALE=%s sh %s %s" % (self.dev, self.scale, self.remote, args))
        return rc, out

    def down(self, slot, x, y):
        return self._run("down %d %d %d" % (slot, x, y))

    def move(self, slot, x, y):
        return self._run("move %d %d %d" % (slot, x, y))

    def up(self, slot):
        return self._run("up %d" % slot)


def discover_phys(ws, res_line):
    """从 daemon 日志拿 dev=，从 res 拿 raw 量程算 SCALE。返回 (dev, scale) 或 (None, None)。"""
    dev = None
    rc, out = sh("cat /data/local/tmp/vtouchd.log 2>/dev/null | grep -o 'dev=[^ ]*' | tail -1")
    m = re.search(r"dev=(\S+)", out or "")
    if m:
        dev = m.group(1)
    scale = None
    m = re.search(r"raw (\d+) (\d+) (\d+) (\d+)", res_line or "")
    if m and ws_lw:
        xmin, xmax = int(m.group(1)), int(m.group(2))
        span = xmax + 1 - xmin
        scale = max(1, int(round(float(span) / float(ws_lw))))
    return dev, scale


ws_lw = 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=27183)
    ap.add_argument("--dev", default=None, help="物理触摸节点（默认从 daemon 日志读 dev=）")
    ap.add_argument("--scale", type=int, default=0, help="逻辑坐标 → raw 轴的倍数（默认按 res 算）")
    ap.add_argument("--skip-phys", action="store_true", help="跳过需要伪造物理手指的用例")
    ap.add_argument("--no-su", action="store_true", help="不用 su（AVD adb root 时用）")
    a = ap.parse_args()
    sh.no_su = a.no_su

    print("连接 ws://%s:%d" % (a.host, a.port))
    try:
        ws = WS(a.host, a.port)
    except Exception as e:
        bad("握手失败: %s（vtouchd 在跑吗？adb forward 做了吗？）" % e)
        return 1
    ok("握手成功")

    # ---------------------------------------------------------------- T1 命令面
    section("T1 命令面：基础 + region/sub/unsub（含错路径）")
    r = ws.cmd("ping")
    check(r == "pong", "ping → pong", r)
    r = ws.cmd("res")
    check((r or "").startswith("res "), "res → 逻辑尺寸 + raw 量程", r)
    global ws_lw
    try:
        ws_lw = int(r.split()[1])
        ws_lh = int(r.split()[2])
    except Exception:
        ws_lw, ws_lh = 0, 0
    if ws_lw <= 10 or ws_lh <= 10:
        bad("拿不到逻辑尺寸，后面的区域用例没法算坐标")
        return 1

    for line, want in [("region", "err"), ("region bogus", "err"),
                       ("region add", "err"), ("region add r1 9 0 0 10 10 1", "err"),
                       ("region add r1 0 -1 0 10 10 1", "err"),
                       ("region add 0123456789abcdef 0 0 0 10 10 1", "err"),
                       ("region add r1 0 %d 0 10 10 1" % (ws_lw + 5), "err"),
                       ("region clear extra", "err"),
                       ("sub bogus", "err"), ("sub phys extra", "err"),
                       ("unsub extra", "err")]:
        r = ws.cmd(line)
        check((r or "").startswith(want), "%s → %s" % (line, want), r)

    for line in ("region clear", "region list"):
        r = ws.cmd(line)
        check((r or "").startswith(("ok", "end")), "%s → %r" % (line, r), r)

    x1, y1, x2, y2 = ws_lw // 4, ws_lh // 4, ws_lw * 3 // 4, ws_lh * 3 // 4
    r = ws.cmd("region add s3 0 %d %d %d %d 1" % (x1, y1, x2, y2))
    check((r or "").startswith("ok 1"), "region add（矩形）→ ok 1", r)
    r = ws.cmd("region add c1 1 %d %d %d 0 0 1" % (ws_lw // 2, ws_lh // 2, ws_lh // 8))
    check((r or "").startswith("ok 2"), "region add（圆形）→ ok 2", r)
    r = ws.cmd("region add s3 0 0 0 10 10 1")
    check((r or "").startswith("ok 2"), "同 id 重加 = 原地更新（总数不变）", r)
    r = ws.cmd("region list")
    check(r is not None and r.startswith("region s3") and "end 2" in r, "region list → 逐行 + end 2", r)
    r = ws.cmd("region clear")
    check((r or "").startswith("ok 0"), "region clear → ok 0", r)
    ws.cmd("region add s3 0 %d %d %d %d 1" % (x1, y1, x2, y2))   # 后面 T3/T4 用

    for line, want in [("sub", "ok"), ("sub phys", "ok"), ("sub region", "ok"), ("sub all", "ok")]:
        r = ws.cmd(line)
        check((r or "").startswith(want), "%s → %s" % (line, want), r)
    r = ws.cmd("unsub")
    check((r or "").startswith("ok"), "unsub → ok", r)

    # ---------------------------------------------------------------- T2 订阅语义
    section("T2 订阅语义：不 sub 不推；sub region 只给区域事件")
    ws.cmd("unsub")
    cx, cy = (x1 + x2) // 2, (y1 + y2) // 2
    ws.cmd("down 0 %d %d" % (cx, cy))
    ws.cmd("up 0")
    got = drain(ws, 0.3, keep=True)
    check(evs(got, "pev") == [] and evs(got, "region_ev") == [],
          "未订阅：注入不产生任何推送", got[:4])

    ws.cmd("sub region")
    ws.cmd("down 0 %d %d" % (cx, cy))
    ws.cmd("up 0")
    got = drain(ws, 0.3, keep=True)
    check(evs(got, "pev") == [], "sub region：虚拟触点不给 pev（pev 只报真手指）", got[:4])

    ws.cmd("sub all")
    ws.cmd("down 1 %d %d" % (cx, cy))
    ws.cmd("up 1")
    got = drain(ws, 0.3, keep=True)
    check(evs(got, "region_ev") == [], "虚拟触点不产生 region_ev（§4.4 防自激）", evs(got, "region_ev"))
    ws.cmd("unsub")

    # ---------------------------------------------------------------- T3 五事件
    phys = Phys(a.dev or "", a.scale or 16)
    dev, scale = (a.dev, a.scale) if (a.dev and a.scale) else discover_phys(ws, ws.cmd("res"))
    if a.dev:
        dev = a.dev
    if a.scale:
        scale = a.scale
    if a.skip_phys or not dev or not scale:
        print("  skip T3（--skip-phys，或拿不到 dev=/SCALE：dev=%r scale=%r）" % (dev, scale))
    elif not phys.ready():
        bad("T3 需要 %s 上的 fake_touch.sh，但推不上去（有 root 吗？）" % phys.remote)
    else:
        phys = Phys(dev, scale)
        section("T3 五事件：物理手指进出区域（dev=%s scale=%s）" % (dev, scale))
        ws.cmd("unsub")
        ws.cmd("sub all")
        drain(ws, 0.3)
        slot = 0
        phys.down(slot, cx, cy)
        line, seen = wait_for(ws, lambda l: l.startswith("pev "))
        check(line is not None, "物理按下 → pev down", seen)
        check(line is not None and line.split()[2] == "down", "pev 事件是 down", line)
        rline, seen2 = wait_for(ws, lambda l: l.startswith("region_ev "))
        check(rline is not None and rline.split()[2] == "down",
              "区域内按下 → region_ev down（带上 id/slot/逻辑坐标）", rline or seen2)
        if rline:
            p = rline.split()
            check(p[1] == "s3" and abs(int(p[4]) - cx) <= 8 and abs(int(p[5]) - cy) <= 8,
                  "region_ev 坐标是逻辑坐标且在按下点附近", rline)

        phys.move(slot, cx + 20, cy + 20)
        rline, seen3 = wait_for(ws, lambda l: l.startswith("region_ev ") and l.split()[2] == "move")
        check(rline is not None, "区域内移动 → region_ev move", seen3)

        phys.move(slot, 5, 5)
        rline, seen4 = wait_for(ws, lambda l: l.startswith("region_ev ") and l.split()[2] == "exit")
        check(rline is not None, "移出区域 → region_ev exit", seen4)

        phys.move(slot, cx, cy)
        rline, seen5 = wait_for(ws, lambda l: l.startswith("region_ev ") and l.split()[2] == "enter")
        check(rline is not None, "移回区域 → region_ev enter", seen5)

        phys.up(slot)
        rline, seen6 = wait_for(ws, lambda l: l.startswith("region_ev ") and l.split()[2] == "up")
        check(rline is not None, "抬起 → region_ev up", seen6)

        # 区域外按下不应该有 down（但要看到 pev 轨迹：广播与判定是两条路）
        phys.down(slot, 5, 5)
        got = drain(ws, 0.4, keep=True)
        phys.up(slot)
        drain(ws, 0.3)
        check(len(evs(got, "pev")) >= 1, "区域外按下仍有 pev 轨迹（广播不依赖区域）", evs(got, "pev"))
        check(evs(got, "region_ev") == [], "区域外按下没有 region_ev down", evs(got, "region_ev"))

        # ------------------------------------------------------------ T4 回触
        section("T4 回触示例：注入虚拟触点回触区域，不产生任何事件（不自激）")
        ws.cmd("unsub")
        ws.cmd("sub all")
        drain(ws, 0.3)
        phys.down(slot, cx, cy)
        wait_for(ws, lambda l: l.startswith("region_ev ") and l.split()[2] == "down")
        phys.up(slot)
        drain(ws, 0.3)
        ws.cmd("down 5 %d %d" % (cx, cy))          # 回触：另一路虚拟槽注入
        ws.cmd("move 5 %d %d" % (cx + 10, cy + 10))
        ws.cmd("up 5")
        got = drain(ws, 0.5, keep=True)
        check(evs(got, "pev") == [], "回触注入不回流 pev", evs(got, "pev"))
        check(evs(got, "region_ev") == [], "回触注入不产生 region_ev（不会连击）", evs(got, "region_ev"))
    ws.cmd("unsub")

    # ---------------------------------------------------------------- T5 ws_kick
    section("T5 ws_kick：新连接踢旧连接")
    a1 = WS(a.host, a.port)
    check(a1.cmd("ping") == "pong", "旧连接可用", None)
    a1.cmd("sub all")
    a2 = WS(a.host, a.port)
    check(a2.cmd("ping") == "pong", "新连接可用", None)
    check(is_closed(a1), "新连接上来后旧连接被关闭（EOF 或 close 帧）")
    a2.cmd("down 2 10 10")
    a2.cmd("up 2")
    got = drain(a2, 0.3, keep=True)
    check(evs(got, "pev") == [], "被踢方订阅清零：新连接不订阅就没推送", evs(got, "pev"))
    a2.close()

    ws.close()
    print("\n结果: %d ok / %d fail" % (len(oks), len(fails)))
    if fails:
        print("Plan B 回归未通过")
        return 1
    print("Plan B 回归通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
