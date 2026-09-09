#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
build_sdk.py — SDK 单源化构建：从 scripts/vtouch-sdk.src.js 生成三种分发形态。

  scripts/vtouch-sdk.src.js      <- 唯一可读主源 (改 SDK 只改这里)
  ├─ clients/plugins/vtouch.js            (项目插件: module.exports = VTouch)
  └─ clients/plugin-apk/assets/vtouch/index.js  (APK 胶水层: function(plugin) 导出)

用法: python scripts/build_sdk.py [--check]   (--check 仅验证语法, 不写文件)
"""
import argparse
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "scripts" / "vtouch-sdk.src.js"


# ---- 安全压缩: token 级保留必需空格, 保留字符串/注释安全 ----
def minify(src: str) -> str:
    tokens = []
    i, n = 0, len(src)
    WORD_START = set("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ_$")
    WORD_CHAR = WORD_START | set("0123456789")
    while i < n:
        c = src[i]
        if c in " \t\r\n":
            i += 1
            continue
        if c in ('"', "'", "`"):
            j = i + 1
            while j < n:
                if src[j] == "\\":
                    j += 2
                    continue
                if src[j] == c:
                    break
                j += 1
            tokens.append(("str", src[i:j + 1]))
            i = j + 1
            continue
        if c == "/" and i + 1 < n and src[i + 1] == "/":
            while i < n and src[i] != "\n":
                i += 1
            continue
        if c == "/" and i + 1 < n and src[i + 1] == "*":
            j = src.find("*/", i + 2)
            i = j + 2 if j >= 0 else n
            continue
        if c in WORD_START:
            j = i + 1
            while j < n and src[j] in WORD_CHAR:
                j += 1
            tokens.append(("word", src[i:j]))
            i = j
            continue
        if c.isdigit():
            j = i + 1
            while j < n and (src[j].isalnum() or src[j] in "._"):
                j += 1
            tokens.append(("num", src[i:j]))
            i = j
            continue
        j = i + 1
        while j < n and src[j] not in WORD_START and src[j] not in ' \t\r\n"\'`' and not src[j].isdigit():
            j += 1
        tokens.append(("op", src[i:j]))
        i = j
    out, prev_word = [], False
    for kind, val in tokens:
        is_word = kind in ("word", "num")
        if is_word and prev_word:
            out.append(" ")
        out.append(val)
        prev_word = is_word
    return "".join(out)


def check_js(path: Path) -> bool:
    r = subprocess.run(["node", "--check", str(path)], capture_output=True, text=True)
    if r.returncode != 0:
        print(f"  [FAIL] 语法错误 {path.name}:\n{r.stderr.strip()}")
        return False
    print(f"  [OK] {path.name}")
    return True


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true", help="仅验证当前分发文件语法, 不重建")
    args = ap.parse_args()

    if args.check:
        ok = all(check_js(p) for p in [
            ROOT / "clients" / "plugins" / "vtouch.js",
            ROOT / "clients" / "plugin-apk" / "assets" / "vtouch" / "index.js",
        ])
        return 0 if ok else 1

    sdk = minify(SRC.read_text(encoding="utf-8"))

    # 1) 项目插件
    (ROOT / "clients" / "plugins" / "vtouch.js").write_text(
        '"use strict";\n' + sdk +
        '\n/* AutoJs6 项目插件导出：加载后返回 VTouch 构造函数（Finger 挂在 VTouch.Finger 上）。 */\n'
        'module.exports = VTouch;\nVTouch.Finger = Finger;\nVTouch.VERSION = "2.0.0-plugin";\n',
        encoding="utf-8", newline="\n")

    # 2) APK 胶水层
    (ROOT / "clients" / "plugin-apk" / "assets" / "vtouch" / "index.js").write_text(
        "/*\n"
        " * VTouch 应用插件胶水层 (AutoJs6 plugins.load('org.vtouch.plugin') 入口)。\n"
        " * 由 scripts/build_sdk.py 从 scripts/vtouch-sdk.src.js 生成, 勿手改。\n"
        " * v2: 插件 Java API 优先 (startVTouchService/stopVTouchService/isServiceReady),\n"
        " *     失败自动回退 SDK 内 shell 实现。\n"
        " */\nmodule.exports = function (plugin) {\n"
        + sdk +
        "\n"
        "    if (plugin && typeof plugin.startVTouchService === 'function') {\n"
        "        var __origStart = VTouch.prototype.startService;\n"
        "        VTouch.prototype.startService = function () {\n"
        "            if (plugin.isServiceReady && plugin.isServiceReady()) { this.serviceStarted = true; return this; }\n"
        "            var ok = false;\n"
        "            try { ok = plugin.startVTouchService(); } catch (e) { ok = false; }\n"
        "            if (!ok) return __origStart.call(this);\n"
        "            this.serviceStarted = true;\n"
        "            return this;\n"
        "        };\n"
        "        var __origStop = VTouch.prototype.stopService;\n"
        "        VTouch.prototype.stopService = function () {\n"
        "            try { plugin.stopVTouchService(); } catch (e) {}\n"
        "            this.serviceStarted = false;\n"
        "            return this;\n"
        "        };\n"
        "    }\n"
        "    VTouch.VERSION = \"2.0.0-plugin\";\n"
        "    VTouch.Finger = Finger;\n"
        "    return VTouch;\n"
        "};\n",
        encoding="utf-8", newline="\n")

    print("生成完成: clients/plugins/vtouch.js, clients/plugin-apk/assets/vtouch/index.js")
    return 0


if __name__ == "__main__":
    sys.exit(main())
