#!/usr/bin/env python3
"""CI 一致性门：断言"发出去的 SDK 与示例确实来自本次构建的最新代码"。

用法:
    python3 scripts/ci_check.py [--core build/vtouchd_ui] [--sdk build/vtouch_onefile.js]
                                [--example clients/example.js] [--source clients/vtouch.js]
                                [--allow-core <别的核心，可重复>]

检查内容：
  ① SDK 是自包含的（内嵌负载非空）；
  ①c **内嵌负载的 md5 必须等于 --core 那份**；不等时可以显式 --allow-core <路径> 放行，
     放行后 ② 门的重生成基准换成**那一份**（默认行为不变：不传 --allow-core 时只看 --core）；
  ② **把 SDK 按 pack_client.py 的规则从当前源码 + 本次构建的核心重新生成一遍，与磁盘上的 SDK 逐字节比对**
     —— 通过就说明"这份 SDK = 这份源码 + 这个二进制"，不存在旧副本混充；
  ③ 示例里的 require 指向单文件客户端，且示例用到的每个 vt.<名字> 都存在于 SDK 导出的 API 里；
  ④ 打印清单（名字 / 大小 / md5），供 job summary 和 artifact 附带。

退出码：0 全过；1 有失败（CI 直接红）。

为什么这么写：新鲜度不靠人记得跑打包脚本，靠这条门 ——
源码或核心一变，旧 SDK 立刻对不上，CI 就红。
"""
import argparse
import base64
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
    ap.add_argument("--sdk", default="build/vtouch_onefile.js")
    ap.add_argument("--example", default="clients/example.js")
    ap.add_argument("--source", default="clients/vtouch.js")
    ap.add_argument("--allow-core", action="append", default=[], metavar="PATH",
                    help="额外允许的核心（可重复）：SDK 内嵌负载的 md5 命中它时不算错，"
                         "且 ② 门的逐字节重生成以**那一份**做基准（默认只用 --core）")
    a = ap.parse_args()

    root = pathlib.Path(__file__).resolve().parent.parent
    core = root / a.core
    sdk = root / a.sdk
    example = root / a.example
    source = root / a.source

    print("== 输入 ==")
    for p in (core, sdk, example, source):
        if not p.is_file():
            print("  FAIL  缺文件 %s%s" % (p, "（先跑构建与 scripts/pack_client.py）" if p == sdk else ""))
            FAILS.append("缺文件 %s" % p)
    if FAILS:
        print("\n结果：失败 %d" % len(FAILS))
        return 1

    data = core.read_bytes()
    core_md5 = hashlib.md5(data).hexdigest()
    print("  %-28s %9d 字节  md5=%s" % (core.name, len(data), core_md5))

    sdk_text = sdk.read_text(encoding="utf-8")
    print("  %-28s %9d 字节  md5=%s" % (sdk.name, sdk.stat().st_size, md5_of(sdk)))
    print("  %-28s %9d 字节  md5=%s" % (example.name, example.stat().st_size, md5_of(example)))
    print("  %-28s %9d 字节  md5=%s" % (source.name, source.stat().st_size, md5_of(source)))

    print("\n== ① SDK 自包含 ==")
    m = re.search(r'var PAYLOAD = (null|"([A-Za-z0-9+/=]*)")', sdk_text)
    payload_md5 = None
    if not m:
        bad("SDK 里找不到 PAYLOAD（不是 pack_client.py 的产物？）")
    elif m.group(1) == "null":
        bad("SDK 的 PAYLOAD 是 null —— 这是源码态，不是自包含单文件")
    else:
        ok("内嵌负载非空（%d 字符 base64）" % len(m.group(2)))
        try:
            payload_md5 = hashlib.md5(base64.b64decode(m.group(2))).hexdigest()
        except Exception:
            payload_md5 = None
        if payload_md5 is None:
            bad("内嵌负载不是合法 base64")
        else:
            ok("内嵌负载 md5=%s" % payload_md5)

    # ①c：内嵌负载到底是不是「声明的核心」那一份。对不上 = 产物与声明不符（不是代码坏了），
    #      要么重跑 scripts/pack_client.py，要么显式 --allow-core <路径> 放行。
    #      命中 allow-core 时，② 门的逐字节重生成用**那一份**做基准。
    basis, basis_data, basis_md5 = core, data, core_md5
    if payload_md5 is not None:
        if payload_md5 == core_md5:
            ok("内嵌负载 == --core %s（md5=%s）" % (a.core, core_md5))
        else:
            hit = None
            for extra in a.allow_core:
                p = root / extra
                if not p.is_file():
                    print("  --    允许核心 %s 不存在" % extra)
                elif md5_of(p) == payload_md5:
                    hit = p
                    break
                else:
                    print("  --    允许核心 %s 对不上（md5=%s）" % (extra, md5_of(p)))
            if hit is None:
                bad("SDK 内嵌负载 md5=%s 既不是 --core %s（md5=%s），也不在 --allow-core 列表里 "
                    "—— 磁盘上的 SDK 内嵌的不是声明的核心（判红 ≠ 代码坏了）"
                    % (payload_md5, a.core, core_md5))
            else:
                basis = hit
                basis_data = hit.read_bytes()
                basis_md5 = payload_md5
                ok("内嵌负载命中 --allow-core %s（md5=%s）→ ② 门以它为基准"
                   % (hit.relative_to(root).as_posix(), basis_md5))

    print("\n== ①b 面板必须是「接核心」模式（real），不能是 stub ==")
    panel = root / "build/ui/libtestimgui.so"
    if not panel.is_file():
        bad("缺 %s（先跑 VTOUCH_UI_CORE=real scripts/build_ui.sh）" % panel.relative_to(root).as_posix())
    else:
        blob = panel.read_bytes()
        real_mark = "已接核心".encode("utf-8")
        stub_mark = "demo_rect".encode("utf-8")          # ui_stubs.c 里的桩数据，real 模式不该有
        if real_mark in blob and stub_mark not in blob:
            ok("%s 是 real 模式（含「已接核心」、不含桩数据）" % panel.name)
        else:
            bad("%s 是 stub 模式（VTOUCH_UI_CORE=real 没生效）—— 这种 SDK 的面板不接核心、看不到真实状态"
                % panel.name)

    print("\n== ② SDK = 当前源码 + 本次构建的核心（逐字节重生成比对）==")
    want = source.read_text(encoding="utf-8")
    b64 = base64.b64encode(basis_data).decode("ascii")
    subs = [
        (r'var PAYLOAD = null;[ \t]*/\* <<PAYLOAD>> \*/',
         'var PAYLOAD = "%s";   /* <<PAYLOAD>> 内嵌核心 base64（pack_client.py 填） */' % b64),
        (r'var PAYLOAD_MD5 = null;[ \t]*/\* <<PAYLOAD_MD5>> \*/',
         'var PAYLOAD_MD5 = "%s";   /* <<PAYLOAD_MD5>> */' % basis_md5),
        (r'var PAYLOAD_SIZE = 0;[ \t]*/\* <<PAYLOAD_SIZE>> \*/',
         'var PAYLOAD_SIZE = %d;   /* <<PAYLOAD_SIZE>> */' % len(basis_data)),
    ]
    for pat, rep in subs:
        want, n = re.subn(pat, lambda _m: rep, want, count=1)
        if n != 1:
            bad("源码 %s 里找不到占位符 %s（pack_client.py 与源码不同步）" % (a.source, pat[:28]))
            return 1
    if want == sdk_text:
        ok("与「当前源码 + 当前核心」重新生成的 SDK 逐字节一致")
    else:
        bad("SDK 与重新生成的结果不一致 —— 磁盘上的 SDK 不是用当前源码/当前核心生成的（重跑 scripts/pack_client.py）")

    print("\n== ③ 示例与 SDK 的 API 对齐 ==")
    ex = example.read_text(encoding="utf-8")
    m = re.search(r'require\(\s*"([^"]+)"\s*\)', ex)
    if not m:
        bad("示例里没有 require(...)")
    elif m.group(1).endswith("/vtouch.js"):
        ok("示例 require 指向单文件客户端：%s" % m.group(1))
    else:
        bad("示例 require 指向 %s（应为 …/vtouch.js）" % m.group(1))

    exp = re.search(r'module\.exports = \{(.*?)\};', sdk_text, re.S)
    if not exp:
        bad("SDK 里找不到 module.exports")
        exported = set()
    else:
        exported = set(re.findall(r'(\w+)\s*:', exp.group(1)))
        ok("SDK 导出 %d 个名字：%s" % (len(exported), ", ".join(sorted(exported))))

    used = set(re.findall(r'vt\.([A-Za-z_]\w*)', ex))
    missing = sorted(used - exported)
    if missing:
        bad("示例用到但 SDK 没导出：%s" % ", ".join(missing))
    else:
        ok("示例用到的 %d 个 API 全部存在：%s" % (len(used), ", ".join(sorted(used))))

    print("\n== ④ 清单 ==")
    for p in (sdk, example, basis):
        print("  %s  %s  %d" % (md5_of(p), p.relative_to(root).as_posix(), p.stat().st_size))

    print("\n结果：通过 %d，失败 %d" % (len(OKS), len(FAILS)))
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
