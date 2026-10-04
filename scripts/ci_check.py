#!/usr/bin/env python3
"""CI 一致性门：断言构建产物齐全、面板是「接核心」real 模式、su 脚本内嵌的是本次核心，并打印清单。

用法:
    python3 scripts/ci_check.py [--core build/vtouchd_ui] [--core-default build/vtouchd]
                                [--panel build/ui/libtestimgui.so] [--su build/vtouch.sh]

检查内容：
  ① 四个产物文件存在且非空（带面板核心 / 默认核心 / 面板 so / su 自包含脚本）；
  ② 面板必须是「接核心」模式（real），不能是 ui_stubs.c 的桩版；
  ②b su 脚本内嵌负载的 md5/大小必须等于 --core 那一份（换核心没重打包 → 判红）；
  ③ 打印清单（名字 / 大小 / md5），供 job summary 与 artifact 附带。

退出码：0 全过；1 有失败（CI 直接红）。

历史：本门曾负责「SDK（单文件 JS 客户端）按当前源码逐字节重生成比对」——
旧客户端通道于 2026-10-05 全退役后，那组检查随客户端一并移除（见 git 历史）。
"""
import argparse
import hashlib
import pathlib
import re
import sys

FAILS = []
OKS = []


def ok(msg):
    OKS.append(msg)
    print("  ok    " + msg)


def bad(msg):
    FAILS.append(msg)
    print("  FAIL  " + msg)


def md5_of(path):
    return hashlib.md5(path.read_bytes()).hexdigest()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--core", default="build/vtouchd_ui")
    ap.add_argument("--core-default", default="build/vtouchd")
    ap.add_argument("--panel", default="build/ui/libtestimgui.so")
    ap.add_argument("--su", default="build/vtouch.sh")
    a = ap.parse_args()

    root = pathlib.Path(__file__).resolve().parent.parent
    core = root / a.core
    core_d = root / a.core_default
    panel = root / a.panel
    su = root / a.su

    print("== ① 产物齐全 ==")
    for p, how in ((core, "先跑 sh scripts/build.sh ui"), (core_d, "先跑 sh scripts/build.sh"),
                   (panel, "先跑 VTOUCH_UI_CORE=real sh scripts/build_ui.sh"),
                   (su, "先跑 python scripts/pack_su.py")):
        if not p.is_file() or p.stat().st_size == 0:
            bad("缺 %s（%s）" % (p.relative_to(root).as_posix(), how))
        else:
            ok("%s 存在（%d 字节）" % (p.relative_to(root).as_posix(), p.stat().st_size))
    if FAILS:
        print("\n结果：失败 %d" % len(FAILS))
        return 1

    print("\n== ② 面板必须是「接核心」模式（real），不能是 stub ==")
    blob = panel.read_bytes()
    real_mark = "已接核心".encode("utf-8")
    stub_mark = "demo_rect".encode("utf-8")          # ui_stubs.c 里的桩数据，real 模式不该有
    if real_mark in blob and stub_mark not in blob:
        ok("%s 是 real 模式（含「已接核心」、不含桩数据）" % panel.name)
    else:
        bad("%s 是 stub 模式（VTOUCH_UI_CORE=real 没生效）—— 这种面板不接核心、看不到真实状态"
            % panel.name)

    print("\n== ②b su 脚本内嵌负载 == %s ==" % a.core)
    txt = su.read_text(encoding="utf-8")
    core_md5 = md5_of(core)
    m = re.search(r'PAYLOAD_MD5="([0-9a-f]{32})"', txt)
    if not m:
        bad("su 脚本里找不到 PAYLOAD_MD5（不是 pack_su.py 的产物？）")
    elif m.group(1) != core_md5:
        bad("su 内嵌负载 md5=%s ≠ %s（md5=%s）—— 换核心后没重跑 scripts/pack_su.py"
            % (m.group(1), a.core, core_md5))
    else:
        ok("su 内嵌负载 md5 == %s（%s）" % (a.core, core_md5))
    m2 = re.search(r'PAYLOAD_SIZE=(\d+)', txt)
    if not m2:
        bad("su 脚本里找不到 PAYLOAD_SIZE")
    elif int(m2.group(1)) != core.stat().st_size:
        bad("su 内嵌负载大小=%s ≠ %s（%d）" % (m2.group(1), a.core, core.stat().st_size))
    else:
        ok("su 内嵌负载大小 == %s（%d 字节）" % (a.core, core.stat().st_size))

    print("\n== ③ 清单 ==")
    for p in (core, core_d, panel, su):
        print("  %s  %s  %d" % (md5_of(p), p.relative_to(root).as_posix(), p.stat().st_size))

    print("\n结果：通过 %d，失败 %d" % (len(OKS), len(FAILS)))
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
