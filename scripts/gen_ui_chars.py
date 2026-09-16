#!/usr/bin/env python3
"""gen_ui_chars.py — 生成面板字形表（build/ui/ui_chars.h）。

为什么需要它：面板以前是 AddFontFromFileTTF(..., GetGlyphRangesChineseFull())，
也就是把 2 万多个汉字在 44px 与 30px 两档全部光栅化——真机实测这一步 ~860ms
（面板进程启动到首帧的最大一块），而且 4096² 图集装不下，后面的字会被静默丢掉。

面板实际会画的汉字只有源码里写死的那些（区域 id 限定 [A-Za-z0-9_-]，事件日志行
是纯 ASCII，没有用户输入的任意文本），所以按源码里出现过的字符建字形表即可：
字数随文案变化（按上面规则扫源码，别在注释/文档里写死具体数字 —— 会漂）；这个量级下
图集一拍即合，烘图 ~10ms 级。「字符集封闭」这条前提现在由 core 兜底：
region_add / region_rename 拒绝非 [A-Za-z0-9_-] 的 id（回 err region），
否则脚本经 WS 推一个怪 id 进来，面板只会画出方框。

生成的 k_ui_chars 以 UTF-8 字节写进 C 字符串；ImGui 的 AddText 按 UTF-8 解码。
改文案不用管——build_ui.sh 每次编译前重跑本脚本，表永远是最新的。

用法:
    python scripts/gen_ui_chars.py --out build/ui/ui_chars.h   # 生成（默认路径）
    python scripts/gen_ui_chars.py --check --out <path>        # 只比对，不一致退出码 1
"""
from __future__ import annotations

import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOURCES = ["src-ui/vtouch_ui.cpp", "src-ui/VTouchUI.java"]
DEFAULT_OUT = os.path.join(ROOT, "build", "ui", "ui_chars.h")


def collect_chars() -> str:
    chars: set[str] = set()
    for rel in SOURCES:
        path = os.path.join(ROOT, rel)
        if not os.path.exists(path):
            print("缺 %s" % path)
            continue
        with open(path, "r", encoding="utf-8") as f:
            for ch in f.read():
                if ord(ch) > 0x7F:
                    chars.add(ch)
    return "".join(sorted(chars, key=ord))


def render(chars: str) -> str:
    esc = chars.replace("\\", "\\\\").replace('"', '\\"')
    return (
        "/* 自动生成，勿手改：python scripts/gen_ui_chars.py --out build/ui/ui_chars.h\n"
        " *\n"
        " * 内容 = 面板源码（%s）里出现过的全部非 ASCII 字符，也就是面板会画的全部字形。\n"
        " * 面板 id 限 [A-Za-z0-9_-]、日志行纯 ASCII，没有用户任意输入，故字形集合是封闭的。\n"
        " * 用 ChineseFull 会把 2 万+ 汉字 × 44px/30px 两档全烘一遍（真机 ~860ms，图集还装不下）。\n"
        " */\n"
        "static const char k_ui_chars[] =\n    \"%s\";\n" % (", ".join(SOURCES), esc)
    )


def main() -> int:
    out = DEFAULT_OUT
    check = "--check" in sys.argv
    argv = sys.argv[1:]
    for i, a in enumerate(argv):
        if a == "--out" and i + 1 < len(argv):
            out = argv[i + 1]
    text = render(collect_chars())
    if check:
        if not os.path.exists(out):
            print("字形表不存在: %s（先跑不带 --check 的生成）" % out)
            return 1
        old = open(out, "r", encoding="utf-8").read()
        if old != text:
            print("字形表过期: %s ≠ 由源码重新生成的结果 —— 重跑 python scripts/gen_ui_chars.py" % out)
            return 1
        print("字形表最新: %s" % out)
        return 0
    os.makedirs(os.path.dirname(out), exist_ok=True)
    newline = "\n"
    with open(out, "w", encoding="utf-8", newline=newline) as f:
        f.write(text)
    print("字形表: %s (%d 字)" % (out, len(collect_chars())))
    return 0


if __name__ == "__main__":
    sys.exit(main())
