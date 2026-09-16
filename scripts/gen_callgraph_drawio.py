#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""生成 draw.io 调用图的**图数据 JSON**（喂给 drawio-skill 的 autolayout.py → .drawio）。

与 gen_callgraph.py 同源同规则：函数与调用边现取 src/*.c，作用文案取 scripts/funcdoc_data.py 的 @brief；
节点 = 函数（两行：函数名 + 作用），容器 = 模块（group = 文件名），边 = 调用，多次调用标 n。

用法：python scripts/gen_callgraph_drawio.py [out.json]
默认输出 build/diagrams/vtouch-callgraph-graph.json（草稿位，被 .gitignore）。
"""
import collections
import io
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from funcdoc_data import DOCS
import gen_callgraph as G

FILL = {"read": ("#eff6ff", "#3b82f6"), "control": ("#faf5ff", "#8b5cf6"),
        "write": ("#f0fdf4", "#10b981"), "data": ("#fff7ed", "#f97316"),
        "neutral": ("#f8fafc", "#94a3b8")}
GROUPS = {
    "vtouchd.c": ("进程 / 主循环", "control"),
    "vt_input.c": ("物理输入 / 合并设备", "read"),
    "vt_ws.c": ("WebSocket 协议 / 命令族", "control"),
    "vt_frame.c": ("组帧与合帧", "write"),
    "vt_queue.c": ("事件队列 / 出站队列", "data"),
    "vt_region.c": ("区域表 / 五事件", "read"),
    "vt_util.c": ("小工具", "neutral"),
}


def esc(s):
    return (s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;"))


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "diagrams", "vtouch-callgraph-graph.json")
    funcs, edges = G.extract()
    nodes, groups_seen = [], []
    for m in G.FILES:
        if m in GROUPS and not any(g[0] == m for g in groups_seen):
            groups_seen.append((m, GROUPS[m][0]))
    for n in sorted(funcs, key=lambda x: (G.FILES.index(funcs[x]["mod"]), funcs[x]["line"])):
        m = funcs[n]["mod"]
        fill, stroke = FILL[GROUPS.get(m, ("", "neutral"))[1]]
        brief = G.short(DOCS.get(n, {}), 34)
        nodes.append({
            "id": n,
            "label": "<b>%s()</b><br><span style='font-size:10px'>%s</span>" % (esc(n), esc(brief)),
            "style": "rounded=1;whiteSpace=wrap;html=1;arcSize=8;fillColor=%s;strokeColor=%s;"
                     "fontSize=12;spacing=4;verticalAlign=middle;" % (fill, stroke),
            "width": 240, "height": 62,
            "group": m,
            "groupLabel": "%s · %s" % (m, GROUPS.get(m, ("", ""))[0]),
        })
    es = []
    for (a, b), cnt in sorted(edges.items()):
        e = {"source": a, "target": b}
        if cnt > 1:
            e["label"] = "%d 处" % cnt
        es.append(e)
    # ranksep/nodesep 加大层/点间距：不加时布局会出现 3 条「边穿过节点」，加上后为 0（validate.py 可查）
    graph = {"direction": "TB", "ranksep": 1.6, "nodesep": 0.7, "nodes": nodes, "edges": es}
    io.open(out, "w", encoding="utf-8").write(json.dumps(graph, ensure_ascii=False, indent=1) + "\n")
    print("节点 %d · 边 %d · 容器 %d → %s" % (len(nodes), len(es), len(groups_seen), os.path.relpath(out, ROOT)))


if __name__ == "__main__":
    main()
