#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""自动消解渲染器的路由冲突：哪条边报 collinear overlap / PORT_CAPACITY，就给那条边试端口组合，
成功后写回 JSON（JSON 是权威源，SVG 只是产物）。

机制（本轮实测）：这套 JSON-IR 的渲染器在"两条边抢同一条走廊"或"同带互调"时抛
unresolved collinear overlap；给冲突边加显式 source_port/target_port（right/left 组合）
即可让它绕开默认走廊。手写路点 route_points 反而必挂（实测 B/C/D/E 四组对照）。

用法：python build/_fix_ports.py <json> [<json> ...]
"""
import io
import json
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FW = r"C:/Users/21102/AppData/Local/hermes/skills/fireworks-tech-graph/scripts/fireworks.py"
VARIANTS = [("right", "left"), ("left", "right"), ("right", "top"), ("top", "left"),
            ("bottom", "right"), ("left", "bottom"), ("top", "right"), ("right", "bottom")]


def render(doc, path):
    tmp = os.path.join(ROOT, "build", "diagrams", os.path.basename(path) + ".probe.json")
    io.open(tmp, "w", encoding="utf-8").write(json.dumps(doc, ensure_ascii=False))
    return subprocess.run(["python", FW, "render", "data-flow", tmp, tmp.replace(".probe.json", ".probe.svg")],
                          capture_output=True, text=True, encoding="utf-8", errors="replace")


def fix(path):
    doc = json.loads(io.open(path, encoding="utf-8").read())
    edges = {a["id"]: a for a in doc["arrows"]}
    for attempt in range(1, 40):
        out = render(doc, path)
        if out.returncode == 0:
            io.open(path, "w", encoding="utf-8").write(json.dumps(doc, ensure_ascii=False, indent=2) + "\n")
            hinted = [a["id"] for a in doc["arrows"] if "source_port" in a]
            print("%-38s 尝试 %d 次后全过（%d 条边带端口提示：%s）" %
                  (os.path.basename(path), attempt, len(hinted), ",".join(hinted) or "无"))
            return True
        blob = (out.stdout or "") + (out.stderr or "")
        m = re.search(r"edge (\w+) has an unresolved collinear overlap", blob)
        if not m:
            print("%-38s 非路由类错误，停手：%s" % (os.path.basename(path), blob.strip()[-160:]))
            return False
        eid = m.group(1)
        if eid not in edges:
            print("%-38s 报错的边不在 JSON 里：%s" % (os.path.basename(path), eid))
            return False
        tried = edges[eid].setdefault("_tried", [])
        placed = False
        for sp, tp in VARIANTS:
            if [sp, tp] in tried:
                continue
            tried.append([sp, tp])
            edges[eid]["source_port"], edges[eid]["target_port"] = sp, tp
            print("  %s 冲突 → 给 %s 试端口 %s/%s" % (os.path.basename(path), eid, sp, tp))
            placed = True
            break
        if not placed:
            edges[eid].pop("source_port", None)
            edges[eid].pop("target_port", None)
            print("%-38s %s 的端口变体试尽，需要改布局（分层/带容量）" % (os.path.basename(path), eid))
            return False
    return False


if __name__ == "__main__":
    ok = [fix(p) for p in sys.argv[1:]]
    sys.exit(0 if all(ok) else 1)
