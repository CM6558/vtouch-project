#!/usr/bin/env python3
"""verify_bundle.py — 校验 clients/vtouch_bundle.js 里嵌的东西对得上本次编译产物。

为什么需要它：bundle 是把二进制/dex/so 以 base64 内嵌进 JS 的，肉眼看不出来它到底带了哪一版。
CI 与本地重建后用同一份脚本断言「产物 = 本次源码编出来的东西」，防止发出去的包里
混着旧二进制（历史上就发生过：面板 dex 里残留过已删除类的 .class）。

用法:
    python scripts/verify_bundle.py                        # 只做自洽性检查（解开内嵌项、算 md5、语法门）
    python scripts/verify_bundle.py --bin build/vtouchd    # 还要求内嵌 daemon 与这个文件逐字节相同
    python scripts/verify_bundle.py --ui build/ui          # 还要求内嵌 dex/so 与这两个文件 md5 相同
    python scripts/verify_bundle.py --allow-headless       # 允许没有面板（x86_64 AVD 包）

退出码 0 = 全部通过；非 0 = 有断言失败（打印 FAIL 行）。
"""
from __future__ import annotations

import base64
import hashlib
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_BUNDLE = os.path.join(ROOT, "clients", "vtouch_bundle.js")

fails: list[str] = []
oks: list[str] = []


def ok(msg: str) -> None:
    oks.append(msg)
    print("  ok   " + msg)


def fail(msg: str) -> None:
    fails.append(msg)
    print("  FAIL " + msg)


def md5(b: bytes) -> str:
    return hashlib.md5(b).hexdigest()


def js_string_value(js: str, var: str) -> str | None:
    """取 `var <var> = "..." (+ "...")*;` 的拼接结果（bundle 里的 b64 是分行拼的）。"""
    m = re.search(r'var\s+%s\s*=\s*(.*?);\s*\n' % re.escape(var), js, re.S)
    if not m:
        return None
    return "".join(re.findall(r'"([^"]*)"', m.group(1)))


def js_int_value(js: str, var: str) -> int | None:
    m = re.search(r'var\s+%s\s*=\s*(\d+)\s*;' % re.escape(var), js)
    return int(m.group(1)) if m else None


def main() -> int:
    argv = sys.argv[1:]
    bundle = DEFAULT_BUNDLE
    bin_path = ui_dir = None
    allow_headless = False
    i = 0
    while i < len(argv):
        a = argv[i]
        if a == "--bundle":
            bundle = argv[i + 1]; i += 2; continue
        if a == "--bin":
            bin_path = argv[i + 1]; i += 2; continue
        if a == "--ui":
            ui_dir = argv[i + 1]; i += 2; continue
        if a == "--allow-headless":
            allow_headless = True; i += 1; continue
        print("未知参数: " + a); return 2

    print("校验 %s" % bundle)
    if not os.path.exists(bundle):
        print("  FAIL bundle 不存在"); return 1
    raw_js = open(bundle, "rb").read()
    js = raw_js.decode("utf-8", "ignore")
    print("  bundle 大小 %d B，md5 %s" % (len(raw_js), md5(raw_js)))

    # ---- 结构：必备段落都在
    for var in ("VTOUCH_BIN_SIZE", "VTOUCH_BIN_B64", "VTOUCH_UI_DEX_SIZE", "VTOUCH_UI_SO_SIZE",
                "VTOUCH_UI_SO_GZ_B64", "vtouchOnRegion"):
        if var in js:
            ok("含 %s" % var)
        else:
            fail("缺 %s（bundle 不完整）" % var)

    # ---- 内嵌 daemon：解 b64 → 字节数、md5，可再与现编对比
    b64 = js_string_value(js, "VTOUCH_BIN_B64")
    declared = js_int_value(js, "VTOUCH_BIN_SIZE")
    if not b64:
        fail("VTOUCH_BIN_B64 解不出来")
    else:
        try:
            bin_raw = base64.b64decode(b64)
        except Exception as e:  # noqa: BLE001
            bin_raw = b""
            fail("VTOUCH_BIN_B64 不是合法 base64: %s" % e)
        if bin_raw:
            if declared is not None and len(bin_raw) != declared:
                fail("daemon 实际 %d B ≠ 声明 %d B" % (len(bin_raw), declared))
            else:
                ok("内嵌 daemon %d B md5=%s" % (len(bin_raw), md5(bin_raw)))
            if not bin_raw.startswith(b"\x7fELF"):
                fail("内嵌 daemon 不是 ELF")
            if bin_path:
                if not os.path.exists(bin_path):
                    fail("--bin 给的文件不存在: %s" % bin_path)
                else:
                    ref = open(bin_path, "rb").read()
                    if md5(ref) == md5(bin_raw):
                        ok("内嵌 daemon 与 %s 逐字节一致（%s）" % (bin_path, md5(ref)))
                    else:
                        fail("内嵌 daemon ≠ %s（包内 %s vs 现编 %s）—— 包是旧的"
                             % (bin_path, md5(bin_raw), md5(ref)))

    # ---- 内嵌面板：md5 声明值 + 可选与现编产物对比
    dex_md5 = re.search(r'VTOUCH_UI_DEX_MD5\s*=\s*"([0-9a-f]*)"', js)
    so_md5 = re.search(r'VTOUCH_UI_SO_MD5\s*=\s*"([0-9a-f]*)"', js)
    so_gz_b64 = js_string_value(js, "VTOUCH_UI_SO_GZ_B64") or ""
    has_panel = bool(so_gz_b64)
    if has_panel:
        ok("内嵌面板 dex md5=%s so md5=%s" % (dex_md5.group(1) if dex_md5 else "?",
                                             so_md5.group(1) if so_md5 else "?"))
        if so_md5:
            try:
                import gzip
                so_raw = gzip.decompress(base64.b64decode(so_gz_b64))
                if md5(so_raw) == so_md5.group(1):
                    ok("内嵌 so gz 解出来与声明 md5 一致（%d B）" % len(so_raw))
                else:
                    fail("内嵌 so 解出的 md5 %s ≠ 声明 %s" % (md5(so_raw), so_md5.group(1)))
            except Exception as e:  # noqa: BLE001
                fail("内嵌 so 解不开: %s" % e)
    elif allow_headless:
        ok("无面板（headless，允许）")
    else:
        fail("没有内嵌面板，但没给 --allow-headless —— 这个包不能上真机跑 uiStart()")

    if ui_dir and has_panel:
        d = os.path.join(ui_dir, "classes.dex")
        s = os.path.join(ui_dir, "libtestimgui.so")
        for path, declared_md5 in ((d, dex_md5.group(1) if dex_md5 else None),
                                   (s, so_md5.group(1) if so_md5 else None)):
            if not os.path.exists(path):
                fail("--ui 下缺 %s" % path)
                continue
            actual = md5(open(path, "rb").read())
            if declared_md5 and actual == declared_md5:
                ok("内嵌 %s 与 %s 一致（%s）" % (os.path.basename(path), path, actual))
            else:
                fail("包内 %s md5 %s ≠ 现编 %s —— 包是旧的"
                     % (os.path.basename(path), declared_md5, actual))

    # ---- 语法门（有 node 就跑）
    node = subprocess.run(["node", "--version"], capture_output=True, text=True, shell=False).returncode == 0
    if node:
        r = subprocess.run(["node", "--check", bundle], capture_output=True, text=True)
        if r.returncode == 0:
            ok("node --check 通过")
        else:
            fail("node --check 失败: %s" % (r.stderr.strip()[:200]))
    else:
        print("  skip 未找到 node，跳过语法门")

    print("\n结果: %d ok / %d fail" % (len(oks), len(fails)))
    if fails:
        print("校验未通过：产物与本次源码/构建不一致，别发这个包。")
        return 1
    print("校验通过：包内所有内嵌项都与本次构建产物一致。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
