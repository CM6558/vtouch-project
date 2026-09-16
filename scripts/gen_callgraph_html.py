#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""生成「全量交互版」调用图 HTML（62 个函数 / 78 条调用边：悬停高亮、点击看函数文档、焦点模式）。

与三张静态图（scripts/gen_callgraph.py → fireworks JSON-IR → SVG/PNG）同一份数据源：
函数与调用边现从 src/*.c 提取，作用文案取 scripts/funcdoc_data.py 的 @brief；
版式与交互留在模板 scripts/callgraph_html_template.html（改样式只改模板，数据由本脚本注入）。

用法：python scripts/gen_callgraph_html.py
产物：docs/diagrams/vtouch-callgraph-interactive.html
"""
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

TPL = os.path.join(HERE, "callgraph_html_template.html")
OUT = os.path.join(ROOT, "docs", "diagrams", "vtouch-callgraph-interactive.html")
MOD_TITLE = {
    "vtouchd.c": "vtouchd.c · 进程与主循环",
    "vt_input.c": "vt_input.c · 物理输入 + 合并设备",
    "vt_ws.c": "vt_ws.c · WebSocket 协议 + 命令族",
    "vt_frame.c": "vt_frame.c · 组帧与合帧",
    "vt_queue.c": "vt_queue.c · 事件队列 / 出站队列",
    "vt_region.c": "vt_region.c · 区域表 + 五事件",
    "vt_util.c": "vt_util.c · 小工具",
}
THREADS = {"region_thread_main": "区域线程", "region_apply": "区域线程", "region_ev_send": "区域线程",
           "on_signal": "信号处理器"}
ENTRIES = {"main": "进程入口", "region_thread_main": "线程入口", "on_signal": "信号处理器",
           "physical_events": "poll 回调（触摸设备）", "client_frame": "poll 回调（客户端）",
           "vtouch_poll_step": "poll 回调"}


def bodies():
    """每个函数的函数体文本（去注释），用来统计它调到的系统/库函数。"""
    out = {}
    for m in G.FILES:
        L = io.open(os.path.join(ROOT, "src", m), encoding="utf-8").read().split("\n")
        for i, line in enumerate(L):
            mm = re.search(r"\(vtouch-doc: (\w+)\)", line)
            if not mm:
                continue
            name = mm.group(1)
            for j in range(i, len(L)):
                d = re.match(r"^[A-Za-z_][\w \t\*]*?\b" + re.escape(name) + r"\s*\(", L[j])
                if d and not L[j].rstrip().endswith(";"):
                    end = j if L[j].rstrip().endswith("}") else next(k for k in range(j, len(L)) if L[k] == "}")
                    out[name] = G.strip_comments("\n".join(L[j:end + 1]))
                    break
    return out


def build():
    funcs, edges = G.extract()
    body = bodies()
    names = set(funcs)
    nodes = []
    for n in sorted(funcs, key=lambda x: (G.FILES.index(funcs[x]["mod"]), funcs[x]["line"])):
        d = DOCS.get(n, {})
        libc = sorted(set(mm.group(1) for mm in re.finditer(r"\b([a-z_][a-z0-9_]*)\s*\(", body.get(n, ""))
                          if mm.group(1) not in names and mm.group(1) != n))
        nodes.append({"id": n, "mod": funcs[n]["mod"], "line": funcs[n]["line"],
                      "brief": (d.get("brief") or "").strip(), "params": d.get("params") or [],
                      "ret": d.get("ret") or "", "note": d.get("note") or "",
                      "thread": THREADS.get(n, "主线程"), "entry": ENTRIES.get(n, ""),
                      "out": sum(1 for (a, b) in edges if a == n), "in": sum(1 for (a, b) in edges if b == n),
                      "libc": libc})
    return {"nodes": nodes,
            "edges": [{"a": a, "b": b, "n": c} for (a, b), c in sorted(edges.items())],
            "mods": G.FILES, "modTitle": MOD_TITLE}


def main():
    data = build()
    tpl = io.open(TPL, encoding="utf-8").read()
    head, tail = tpl.split("__DATA__")
    html = head + json.dumps(data, ensure_ascii=False) + tail
    out = os.path.join(ROOT, "docs", "diagrams")
    os.makedirs(out, exist_ok=True)
    io.open(OUT, "w", encoding="utf-8").write(html)
    print("节点 %d · 边 %d → %s（%d bytes）" %
          (len(data["nodes"]), len(data["edges"]), os.path.relpath(OUT, ROOT), len(html)))


if __name__ == "__main__":
    main()
