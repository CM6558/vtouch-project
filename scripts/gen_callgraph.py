#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""生成 vtouchd 调用图的 fireworks JSON-IR（权威源 = build/diagrams/*.json）。

数据全部现从 src/*.c 提取（函数定义行 + 调用边），作用文案取 scripts/funcdoc_data.py 的 @brief，不手抄。

为什么是"三条主干链"而不是一张全图：这套 JSON-IR 是**流程/管线**模板，
渲染器给每个节点端口的可挂端点数有限（generate-from-template.py 的 PORT_CAPACITY），
真实调用图里 ev_add 入度 19、parse_long 17、bit 12 → 必然触发
"unresolved collinear overlap"（generate-from-template.py:2778-2780）。
所以按技能的分层规矩：每带 ≤5 个节点，按**角色**分带（不是按文件），
完整 62 函数 / 78 边那份用交互式 HTML（build/callgraph 那套）承载。

用法：python build/_gen_callgraph.py phys|ctrl|proto
"""
import io
import json
import os
import re
import sys
import collections

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
from funcdoc_data import DOCS

SRC = os.path.join(ROOT, "src")
FILES = ["vtouchd.c", "vt_input.c", "vt_ws.c", "vt_frame.c", "vt_queue.c", "vt_region.c", "vt_util.c"]
MOD_FLOW = {"vtouchd.c": "control", "vt_input.c": "read", "vt_ws.c": "control",
            "vt_frame.c": "write", "vt_queue.c": "data", "vt_region.c": "read", "vt_util.c": "neutral"}
PAL = {"read": ("#eff6ff", "#3b82f6"), "control": ("#faf5ff", "#8b5cf6"),
       "write": ("#f0fdf4", "#10b981"), "data": ("#fff7ed", "#f97316"),
       "neutral": ("#f8fafc", "#94a3b8"), "feedback": ("#fef2f2", "#ef4444")}

# 三条主干链：带 = 角色（不是文件），每带 ≤5 个节点
# 三张图的**节点清单**（并集 = src 全部 62 个函数，emit_frame/set_virtual/owner_reset 在两图各出现一次做交叉引用）；
# 带（band）不手写：按调用关系做**最长路径分层**自动生成 —— 拓扑上保证边只跨层，不出现同带内互调
# （渲染器对同带边无解，实测最小失败集 sha1_block→rol32 + sha1_block→be32 + websocket_handshake→sha1_final）。
SCOPES_MEMBERS = {
 # 节点清单按"模块 + 跨模块邻居"取，尽量把跨模块调用收进同一张图（画不到的边会在回报里逐条列）
 "phys": ("物理触摸链：物理帧 → 合帧 → 转发 → 入队 → 区域判定 → 出站（%d 个函数 · %d 条调用）",
          ["vt_input.c", "vt_frame.c", "vt_queue.c", "vt_region.c", "vt_util.c"], []),
 "ctrl": ("控制链：进程 / 主循环 / 注入（%d 个函数 · %d 条调用）",
          ["vtouchd.c", "vt_util.c"],
          ["emit_frame", "set_virtual", "owner_reset"]),
 "proto": ("WebSocket 协议链：握手 → 摘要 → 发送 → 解帧（%d 个函数 · %d 条调用）",
           ["vt_ws.c"], ["outq_push_text", "outq_flush", "region_add", "regions_clear"]),
 # 注：曾试过第四张「跨模块汇聚」（主循环/命令族 → 各模块入口，24 条跨模块边）——渲染器对
 # 7 源 → 17 目标的纯扇出无解：最窄与最宽走廊都试过，BRIDGE_BUDGET 始终 157~179 > 预算。
 # 这 24 条边只在交互版 HTML 里看得到（docs/diagrams/vtouch-callgraph-interactive.html）。
}
def strip_comments(s):
    s = re.sub(r"/\*.*?\*/", " ", s, flags=re.S)
    return re.sub(r"//[^\n]*", " ", s)


def extract():
    funcs, src = {}, {}
    for m in FILES:
        L = io.open(os.path.join(SRC, m), encoding="utf-8").read().split("\n")
        src[m] = L
        for i, l in enumerate(L):
            mm = re.search(r"\(vtouch-doc: (\w+)\)", l)
            if not mm:
                continue
            name = mm.group(1)
            for j in range(i, len(L)):
                d = re.match(r"^[A-Za-z_][\w \t\*]*?\b" + re.escape(name) + r"\s*\(", L[j])
                if d and not L[j].rstrip().endswith(";"):
                    end = j if L[j].rstrip().endswith("}") else next(k for k in range(j, len(L)) if L[k] == "}")
                    funcs[name] = {"name": name, "mod": m, "line": j + 1, "end": end + 1}
                    break
    names = set(funcs)
    edges = collections.Counter()
    for name, info in funcs.items():
        body = strip_comments("\n".join(src[info["mod"]][info["line"] - 1:info["end"]]))
        for cm in re.finditer(r"\b([a-z_][a-z0-9_]*)\s*\(", body):
            c = cm.group(1)
            if c != name and c in names:
                edges[(name, c)] += 1
    return funcs, edges


def short(doc, budget=32):
    """节点副标题 = 该函数的作用：优先取 @brief 冒号前的半句（够长时才是「作用」，
    例如「把 phys[]/virt[] 合成一帧并提交：待抬 → …」→ 取前句）；前句太短（如「命令族」）才取后句。"""
    b = (doc.get("brief") or "").strip()
    if "：" in b:
        head, tail = b.split("：", 1)
        b = head if len(head) >= 10 else tail
    b = re.sub(r"（§[^）]*）|\(§[^)]*\)|（见[^）]*）|（不推客户端）", "", b).strip("。；，, ")
    if len(b) <= budget:
        return b
    cut = max(b.rfind(s, 0, budget) for s in "，。、；,)")
    return (b[:cut] if cut > budget * 0.5 else b[:budget]) + "…"


def main(scope):
    members = SCOPES_MEMBERS[scope]
    only_from = None
    chunk_size = 2
    if isinstance(members[-1], dict):          # 可选：{"only_from": [...]}
        title, files, extra = members[0], members[1], members[2]
        only_from = members[-1].get("only_from")
        chunk_size = members[-1].get("chunk", 2)
        geom = {k: members[-1][k] for k in ("cw", "box_w", "node_w") if k in members[-1]}
    else:
        title, files, extra = members
    spec = {"file": "vtouch-callgraph-%s.json" % scope,
            "subtitle": "带 = 调用深度层（按调用关系自动分层，箭头只跨层）；箭头 = 调用方向；"
                        "节点副标题 = 该函数的作用（取自代码 @brief）"}
    if isinstance(members[-1], dict):
        spec.update({k: members[-1][k] for k in ("cw", "box_w", "node_w") if k in members[-1]})
    funcs, edges_all = extract()
    names = set([n for n in funcs if funcs[n]["mod"] in files] + [n for n in extra if n in funcs])
    assert all(n in funcs for n in names), "清单里有不存在的函数"
    inner = [(a, b) for (a, b) in edges_all if a in names and b in names
             and (only_from is None or a in only_from)]
    # 最长路径分层（带环保护）：edge a→b 表示 b 在 a 的下一层
    callers = collections.defaultdict(list)
    for a, b in inner:
        callers[b].append(a)
    layer, visiting = {}, set()
    def lay(n, depth=0):
        if n in layer:
            return layer[n]
        if n in visiting or depth > 40:
            return 0
        visiting.add(n)
        v = 0 if not callers[n] else 1 + max(lay(c, depth + 1) for c in callers[n])
        visiting.discard(n)
        layer[n] = v
        return v
    for n in names:
        lay(n)
    # 同层节点按主导模块带标签；同层超过 6 个时拆成相邻带（拆开后仍不同带）
    bydom = collections.defaultdict(list)
    for n in names:
        bydom[layer[n]].append(n)
    bands, band_of = [], {}
    for L in sorted(bydom):
        ns = sorted(bydom[L], key=lambda n: (funcs[n]["mod"], funcs[n]["line"]))
        step = chunk_size if chunk_size > 0 else max(1, len(ns))
        for chunk in range(0, len(ns), step):
            grp = ns[chunk:chunk + step]
            mod = collections.Counter(funcs[n]["mod"] for n in grp).most_common(1)[0][0]
            # 带标签保持短（容器标题是路由障碍物，SKILL.md:53）：层号 + 模块短名，同层多带用序号区分
            lab = "层 %d · %s%s" % (L, mod.replace(".c", ""),
                                     "" if chunk == 0 else "（%d）" % (chunk // max(1, chunk_size) + 1))
            bands.append((lab, MOD_FLOW.get(mod, "neutral"), grp))
            for n in grp:
                band_of[n] = len(bands) - 1
    spec = dict(spec, title=title, bands=bands)
    same_band = set()
    edges = collections.Counter()
    for k, v in edges_all.items():
        if k[0] not in names or k[1] not in names:
            continue
        if only_from is not None and k[0] not in only_from:
            continue
        if band_of[k[0]] == band_of[k[1]]:
            same_band.add(k)        # 同带内互调：渲染器无解（实测 e7+e0+e1 必挂），按技能规则不画箭头
            continue
        edges[k] = v
    if same_band:
        print("同带互调（不画箭头，节点仍在上图）：", ", ".join("%s→%s" % k for k in sorted(same_band)))

    # 带内顺序：重心法 3 遍（父/子位置平均），减少交叉
    order = {bi: [n for n in spec["bands"][bi][2]] for bi in range(len(spec["bands"]))}
    pos = {n: i for bi in order for i, n in enumerate(order[bi])}
    for _ in range(3):
        for bi in order:
            def bary(n):
                nb = [pos[a] for (a, b) in edges if b == n] + [pos[b] for (a, b) in edges if a == n]
                return (sum(nb) / len(nb)) if nb else pos[n] + 0.5
            order[bi] = sorted(order[bi], key=lambda n: (bary(n), pos[n]))
            for i, n in enumerate(order[bi]):
                pos[n] = i

    # 技能 SKILL.md:25 的已验证几何：容器 x=100..800、节点 x=150 宽 580、行高 56、行距 20、头 40、下留 18；
    # 画布右侧多留走廊（800..1240）给跨带边专用 —— 每条边一条专属走廊 x，否则共线（SKILL.md:54）。
    # 几何可按图覆盖（扇出图需要更宽的走廊：走廊宽 = CW - BOX_W，走廊越宽，每条边的专属 x 越容易拉开 → 交叉越少）
    CW, BOX_X, BOX_W = spec.get("cw", 1240), 100, spec.get("box_w", 700)
    NODE_X, NODE_W, NODE_H, ROW_GAP, HEAD, BOTPAD, BAND_GAP, PADY = \
        150, spec.get("node_w", 580), 56, 20, 40, 18, 60, 104
    RAIL_X0, RAIL_DX = 830, 16
    containers, nodes, arrows = [], [], []
    y = PADY
    for bi, (lab, flow, members) in enumerate(spec["bands"]):
        ns = order[bi]
        ys, hh = [], []
        for n in ns:                      # 度自适应高度：出/入度高的节点加高，端口才挂得下端点
            deg = max(sum(1 for (a, b) in edges if a == n), sum(1 for (a, b) in edges if b == n))
            hh.append(NODE_H + 12 * max(0, deg - 4))
        h = HEAD + sum(hh) + max(0, len(ns) - 1) * ROW_GAP + BOTPAD
        containers.append({"id": "band%d" % bi, "x": BOX_X, "y": y, "width": BOX_W, "height": h,
                           "label": lab, "stroke": "#dbe5f1", "fill": "none"})
        yy = y + HEAD
        for i, n in enumerate(ns):
            ys.append(yy)
            fill, stroke = PAL[flow]
            nodes.append({"id": n, "kind": "rect", "x": NODE_X, "y": yy,
                          "width": NODE_W, "height": hh[i], "label": n + "()",
                          "sublabel": "%s · %s:%d" % (short(DOCS.get(n, {})), funcs[n]["mod"], funcs[n]["line"]),
                          "fill": fill, "stroke": stroke, "flat": True})
            yy += hh[i] + ROW_GAP
        y += h + BAND_GAP
    band_y = {bi: c["y"] for bi, c in enumerate(containers)}
    band_h = {bi: c["height"] for bi, c in enumerate(containers)}
    for i, ((a, b), cnt) in enumerate(sorted(edges.items())):
        bi, bj = band_of[a], band_of[b]
        flo = spec["bands"][max(bi, bj)][1]
        if flo == "neutral":
            flo = spec["bands"][min(bi, bj)][1]
        # 经验（实测对照 B/C/D/E 四组）：显式 route_points 会让渲染器报
        # "unresolved collinear overlap"，即使路点在容器外、单独渲染也挂；
        # 显式端口或全默认都能过。所以这里一律只给 source/target/flow，路由交给路由器。
        arrows.append({"id": "e%d" % i, "source": a, "target": b, "flow": flo})
    total_h = y + 110
    doc = {"schema_version": 1, "mode": "data-flow", "template_type": "data-flow", "style": 1,
           "width": CW, "height": total_h,
           "title": spec["title"] % (len(names), len(arrows)), "subtitle": spec["subtitle"],
           # 调用图的交叉数由拓扑决定（standard 档上限 8 条过不去），只放宽这一项 + 拉伸比，
           # 其余仍按 standard（折数/标签间距/最小段长等）。渲染器从 JSON 的 composition 读档位：
           # generate-from-template.py:3007-3009 → composition_quality.PROFILES。
           "composition": {"profile": "standard", "max_bridged_crossings": 60, "max_total_bends": 160,
                           "max_route_stretch": 8.0},
           "containers": containers, "nodes": nodes, "arrows": arrows,
           "legend_orientation": "horizontal", "legend_x": BOX_X, "legend_y": y + 16, "legend_locked": True,
           "legend": [{"flow": f, "label": l} for f, l in
                      [("read", "物理读取 / 区域判定"), ("control", "进程 / 协议控制"), ("write", "组帧 / 发送"),
                       ("data", "队列 / 解帧"), ("neutral", "纯工具")]],
           "footer": "源：src/*.c（函数与调用边现取）· 作用文案见 scripts/funcdoc_data.py · 带内顺序按重心法排",
           "footer_x": BOX_X, "footer_y": total_h - 20}
    out = os.path.join(ROOT, "docs", "diagrams")   # 入库路径：图鉴随仓库走（build/ 被 .gitignore）
    os.makedirs(out, exist_ok=True)
    io.open(os.path.join(out, spec["file"]), "w", encoding="utf-8").write(json.dumps(doc, ensure_ascii=False, indent=2) + "\n")
    big = max(len(order[bi]) for bi in order)
    print("%-34s 节点 %2d · 边 %2d · 每带最多 %d 节点 · 画布 %dx%d" %
          (spec["file"], len(nodes), len(arrows), big, CW, total_h))


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "phys")
