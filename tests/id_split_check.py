#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""id_split_check.py —— 校验「身份两段」在合并设备上确实成立（只吃标准库）。

校验的不变量（`src/vtouchd.c` §5 改后的设计）：
  1. 每个触点的 tracking id == 它的槽位号（物理段原样透传，虚拟段 = phys_slots + 客户端槽号）
  2. 物理段 id ∈ [0, phys_slots)，虚拟段 id ∈ [phys_slots, phys_slots + virt_slots)
  3. 任何时刻**没有两个触点共用同一个 id**（这是「不需要避让池」的全部前提）
  4. 所有 id ≤ 31（Android 的 pointer id 是 32 位 BitSet，超了会被框架改成自分配）
  5. （可选 --require-both）同一段流里既有物理触点又有虚拟触点

数据来源：合并设备（`touchpanel_vtouch`）的 getevent 抓包。抓法见 scripts/ondev-capture.sh。

用法:
    # 直接读设备上的抓包（需要 adb + root）
    python tests/id_split_check.py
    # 或喂一个已经拉下来的日志
    python tests/id_split_check.py build/_merged.log
    # 本地 log 但要求物理+虚拟都出现过
    python tests/id_split_check.py build/_merged.log --require-both
"""
import argparse
import re
import subprocess
import sys

TS = re.compile(r"^\[\s*([0-9.]+)\]")
EV = re.compile(r"(ABS_MT_SLOT|ABS_MT_TRACKING_ID)\s+(\S+)\s*$")
UP = (0xFFFFFFFF, -1)


def load(path):
    if path:
        return open(path, encoding="utf-8", errors="replace").read().splitlines()
    out = subprocess.run(
        ["adb", "shell", "su -c 'cat /data/local/tmp/merged.log'"],
        capture_output=True, text=True)
    if out.returncode != 0 or not out.stdout.strip():
        sys.exit("拉取 /data/local/tmp/merged.log 失败（设备在跑吗？抓过吗？）: %s" % out.stderr.strip())
    return out.stdout.splitlines()


def contacts(lines):
    """还原触点会话：返回 [(slot, id, down_ts, up_ts_or_None)]。"""
    cur, open_by_slot, out = None, {}, []
    for line in lines:
        tsm = TS.match(line)
        ts = float(tsm.group(1)) if tsm else None
        m = EV.search(line)
        if not m:
            continue
        kind, tok = m.group(1), m.group(2)
        if kind == "ABS_MT_SLOT":
            cur = int(tok, 16)
            continue
        if cur is None:
            continue                     # 日志从流中间开始时的无主事件
        v = int(tok, 16)
        if v in UP:
            s = open_by_slot.pop(cur, None)
            if s:
                s["up"] = ts
        else:
            if cur in open_by_slot:      # 同槽换 id（不该发生）
                open_by_slot[cur]["up"] = ts
            s = {"slot": cur, "id": v, "down": ts, "up": None}
            open_by_slot[cur] = s
            out.append(s)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log", nargs="?", help="本地日志；省略则从设备拉 /data/local/tmp/merged.log")
    ap.add_argument("--phys-slots", type=int, default=10, help="物理屏槽数（默认 10）")
    ap.add_argument("--virt-slots", type=int, default=10, help="虚拟槽数（默认 10）")
    ap.add_argument("--require-both", action="store_true", help="要求物理与虚拟触点都出现过")
    a = ap.parse_args()

    cs = contacts(load(a.log))
    if not cs:
        sys.exit("没解析出任何触点：抓包是空的（没人摸过合并设备？）")
    lo_v = a.phys_slots
    hi_v = a.phys_slots + a.virt_slots - 1
    fails = []

    print("触点会话 %d 个（phys_slots=%d → 物理段 0..%d，虚拟段 %d..%d）"
          % (len(cs), a.phys_slots, a.phys_slots - 1, lo_v, hi_v))
    for s in sorted(cs, key=lambda x: (x["down"] or 0)):
        dur = "%.1fs" % (s["up"] - s["down"]) if s["up"] and s["down"] else ("仍在按下" if not s["up"] else "?")
        print("  slot=%-3d id=%-3d %s" % (s["slot"], s["id"], dur))

    # 1. id == slot
    for s in cs:
        if s["id"] != s["slot"]:
            fails.append("id != slot：slot=%d 却发 id=%d" % (s["slot"], s["id"]))

    # 2. 两段范围
    phys = [s for s in cs if s["slot"] < a.phys_slots]
    virt = [s for s in cs if s["slot"] >= a.phys_slots]
    for s in phys:
        if not (0 <= s["id"] < a.phys_slots):
            fails.append("物理触点 id 越出物理段：%d" % s["id"])
    for s in virt:
        if not (lo_v <= s["id"] <= hi_v):
            fails.append("虚拟触点 id 越出虚拟段 [%d,%d]：%d" % (lo_v, hi_v, s["id"]))

    # 3. 任何时刻没有两个触点同 id
    for i in range(len(cs)):
        for j in range(i + 1, len(cs)):
            x, y = cs[i], cs[j]
            if x["id"] != y["id"]:
                continue
            if x["up"] is None or y["up"] is None:
                continue
            if min(x["up"], y["up"]) > max(x["down"], y["down"]):
                fails.append("同 id 同时按下：id=%d（slot %d 与 %d）" % (x["id"], x["slot"], y["slot"]))

    # 4. pointer id 天花板
    for s in cs:
        if s["id"] > 31:
            fails.append("id=%d 超过 Android pointer id 上限 31" % s["id"])

    # 5. 两段都出现（可选）
    if a.require_both and not (phys and virt):
        fails.append("要求物理与虚拟都出现，实际 物理 %d 个 / 虚拟 %d 个" % (len(phys), len(virt)))

    ids = sorted({s["id"] for s in cs})
    print("\n出现过的 id：%s（物理 %s / 虚拟 %s）"
          % (ids, sorted({s["id"] for s in phys}), sorted({s["id"] for s in virt})))
    if fails:
        print("\n结果：不通过")
        for f in fails:
            print("  FAIL %s" % f)
        return 1
    print("\n结果：通过（id==slot / 两段不重叠 / 无同 id 并发 / 都 ≤31%s）"
          % (" / 物理+虚拟都出现" if a.require_both else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
