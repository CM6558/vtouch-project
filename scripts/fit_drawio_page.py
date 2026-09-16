#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把 .drawio 的页面尺寸贴合到图内容外框。

为什么必需：draw.io CLI 按**页面**裁剪导出，而 autolayout 产出的页面是 A4（850×1100），
62 节点 / 78 边的调用图内容有 8294×1536 —— 不贴合的话 PNG 只导出左上角一条（实测 2000×374）。

用法：python scripts/fit_drawio_page.py docs/diagrams/vtouch-callgraph.drawio [--margin 40]
"""
import io
import os
import re
import sys


def main():
    path = sys.argv[1]
    margin = 40
    if "--margin" in sys.argv:
        margin = int(sys.argv[sys.argv.index("--margin") + 1])
    t = io.open(path, encoding="utf-8").read()
    xs, ys = [], []
    for g in re.finditer(r"<mxGeometry([^>]*?)/?>", t):
        a = g.group(1)

        def num(k):
            m = re.search(r'\b%s="(-?[\d.]+)"' % k, a)
            return float(m.group(1)) if m else None

        x, y, w, h = num("x"), num("y"), num("width"), num("height")
        if x is not None and y is not None and w and h:
            xs.append(x + w)
            ys.append(y + h)
    if not xs:
        sys.exit("没解析到几何：%s" % path)
    w, h = int(max(xs) + margin), int(max(ys) + margin)
    t2 = re.sub(r'pageWidth="[\d.]+"', 'pageWidth="%d"' % w, t, count=1)
    t2 = re.sub(r'pageHeight="[\d.]+"', 'pageHeight="%d"' % h, t2, count=1)
    io.open(path, "w", encoding="utf-8").write(t2)
    print("页面贴合内容：%dx%d → %s" % (w, h, os.path.basename(path)))


if __name__ == "__main__":
    main()
