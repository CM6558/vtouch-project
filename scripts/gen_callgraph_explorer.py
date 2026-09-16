#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""生成「可交互阅读版」调用图的数据（喂给 scripts/callgraph_html_template.html）。

数据全部现取：函数与调用边从 src/*.c 解析，作用/参数/返回/注意取自 scripts/funcdoc_data.py
（与函数文档、与三张静态图、与 .drawio 版完全同一份来源）。

除了节点/边，还**从图里搜出几条真实链路**（每一步都断言是图上真实存在的调用边），
供页面上的「跟着链路读」播放器逐步高亮 —— 链路不是手编的，是 BFS/最长路搜出来的。

用法：python scripts/gen_callgraph_explorer.py
产物：docs/diagrams/vtouch-callgraph-interactive.html
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

TPL = os.path.join(HERE, "callgraph_html_template.html")
OUT = os.path.join(ROOT, "docs", "diagrams", "vtouch-callgraph-interactive.html")
MOD_TITLE = {
    "vtouchd.c": "进程 / 主循环 / 收尾",
    "vt_input.c": "物理输入 + 合并设备",
    "vt_ws.c": "WebSocket 协议 + 命令族",
    "vt_frame.c": "组帧与合帧",
    "vt_queue.c": "事件队列 / 出站队列",
    "vt_region.c": "区域表 + 五事件",
    "vt_util.c": "小工具",
}
THREADS = {"region_thread_main": "区域线程", "region_apply": "区域线程", "region_ev_send": "区域线程",
           "on_signal": "信号处理器"}
ENTRIES = {"main": "进程入口", "region_thread_main": "线程入口", "on_signal": "信号处理器",
           "physical_events": "poll 回调（触摸设备）", "client_frame": "poll 回调（客户端）",
           "vtouch_poll_step": "poll 回调"}
LIBKEYWORDS = {"if", "for", "while", "switch", "return", "sizeof", "typeof"}


def bodies():
    """每个函数的函数体（去注释），用来统计它用到的系统/库调用。"""
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


def longest_path(nexts, start, target=None, max_len=10, budget=400000):
    """从 start 出发沿 nexts 搜最长简单路径（带环保护 + 搜索预算）。返回 [start, ...]。

    链路不手编：搜到什么就是什么；调用方下面还会断言每一对都是真实边。
    """
    best, n = [start], [0]

    def dfs(node, path, seen):
        n[0] += 1
        if n[0] > budget:
            return
        if target is None or node == target:
            if len(path) > len(best):
                best[:] = path
        if len(path) >= max_len:
            return
        for nxt in sorted(nexts.get(node, ())):
            if nxt not in seen:
                dfs(nxt, path + [nxt], seen | {nxt})

    dfs(start, [start], {start})
    return list(best) if (target is None or best[-1] == target) else [start]


def main():
    funcs, edge_cnt = G.extract()
    names = set(funcs)
    body = bodies()
    out_edges = collections.defaultdict(set)
    for (a, b) in edge_cnt:
        out_edges[a].add(b)

    nodes = []
    for n in sorted(funcs, key=lambda x: (G.FILES.index(funcs[x]["mod"]), funcs[x]["line"])):
        d = DOCS.get(n, {})
        libc = sorted(set(mm.group(1) for mm in re.finditer(r"\b([a-z_][a-z0-9_]*)\s*\(", body.get(n, ""))
                          if mm.group(1) not in names and mm.group(1) != n and mm.group(1) not in LIBKEYWORDS))
        nodes.append({
            "id": n, "mod": funcs[n]["mod"], "line": funcs[n]["line"],
            "brief": (d.get("brief") or "").strip(), "params": d.get("params") or [],
            "ret": d.get("ret") or "", "note": d.get("note") or "",
            "thread": THREADS.get(n, "主线程"), "entry": ENTRIES.get(n, ""),
            "out": sum(1 for (a, b) in edge_cnt if a == n),
            "in": sum(1 for (a, b) in edge_cnt if b == n),
            "libc": libc,
        })

    # 链路 = 从入口点搜出的**最长真实调用路径**（不手编；下面断言每一对都是图上真实边）。
    # 注意：数据流跨线程的那一跳（vtq_push 入队 → 区域线程 vtq_pop 取出）在调用图上没有边，
    # 所以「物理侧」和「区域侧」是两条链，各自的 desc 里说明队列边界。
    in_edges = collections.defaultdict(set)
    for (a, b) in edge_cnt:
        in_edges[b].add(a)
    # 每条链给「起点 + 有意义的终点」，搜 start→target 的**最长真实路径**（终点不可达就退化成单点，下面断言能兜住）。
    chains_spec = [
        ("① 物理手指：读到一帧 → 写进 uinput", "physical_events", "uinput_writev_retry", "out", 8,
         "物理设备报告一帧 → 解析进 phys[] → 合成一帧 → 一次 writev 提交"),
        ("② 帧边界：状态变化怎么入队", "enqueue_phys_changes", "queue_drop_log", "out", 8,
         "一帧提交之后，跟上一帧快照比出 down/up/move，换算成逻辑坐标入队（队满的日志也在这条上）"),
        ("③ 区域线程：取事件 → 五事件判定 → 推送", "region_thread_main", "outq_push", "out", 8,
         "区域线程取事件、按 down/enter/move/exit/up 判定、把结果封成 WS 文本帧入队"),
        ("④ 出站：刷给客户端 & 失败收摊", "drop_client", None, "in", 8,
         "谁把队列刷给客户端；flush 出错时谁负责关连接、抬触点、清订阅（这条往上走：外层调用者 → drop_client）"),
        ("⑤ 命令注入：一行命令 → 一帧进 uinput", "client_frame", "uinput_writev_retry", "out", 9,
         "客户端一行文本 → 解命令行 → 改虚拟触点/提交帧 → 写进 uinput"),
        ("⑥ 区域订阅：region 命令 → 建表", "client_frame", "region_add", "out", 8,
         "region add/clear/list 这条命令怎么落到区域表上"),
    ]
    chains = []
    for title, start, target, direction, cap, desc in chains_spec:
        if direction == "out":
            path = longest_path(out_edges, start, target, max_len=cap)
        else:
            rev = longest_path(in_edges, start, None, max_len=cap)   # 沿「谁调它」往上走
            path = list(reversed(rev))                                # 翻成调用顺序：外层调用者 → … → start
        for a, b in zip(path, path[1:]):
            assert b in out_edges[a], "链路 %s 里的 %s→%s 不是真实调用边" % (title, a, b)
        chains.append({"title": title, "desc": desc, "path": path})

    data = {
        "nodes": nodes,
        "edges": [{"a": a, "b": b, "n": c} for (a, b), c in sorted(edge_cnt.items())],
        "mods": G.FILES,
        "modTitle": MOD_TITLE,
        "chains": chains,
    }
    tpl = io.open(TPL, encoding="utf-8").read()
    head, tail = tpl.split("__DATA__")
    html = head + json.dumps(data, ensure_ascii=False) + tail
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    io.open(OUT, "w", encoding="utf-8").write(html)
    print("节点 %d · 边 %d · 链路 %d 条（%s）→ %s（%d bytes）" %
          (len(nodes), len(data["edges"]), len(chains),
           ", ".join("%s %d 步" % (c["title"], len(c["path"]) - 1) for c in chains),
           os.path.relpath(OUT, ROOT), len(html)))


if __name__ == "__main__":
    main()
