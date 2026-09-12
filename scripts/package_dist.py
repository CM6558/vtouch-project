#!/usr/bin/env python3
"""package_dist.py — 把交付物打成一个「拿来就能用」的包（含文档与调用示例）。

产出 build/dist/vtouch-bundle-<版本>-arm64/
    vtouch_bundle.js    交付物本体（唯一需要推手机的文件）
    example.js          调用示例（clients/vtouch_region_min.js + 自动生成的头部横幅）
    README.md           包内说明（由 scripts/templates/dist-README.md 填入实际数字）
    md5.txt             各文件 md5（回读对账）
并打成同名 .zip，方便从 CI 下载后直接发出去。

用法:
    python scripts/package_dist.py                                   # 默认就够
    python scripts/package_dist.py --verify --bin build/vtouchd --ui build/ui
                                                                     # 先对账再打包（发版推荐）
    python scripts/package_dist.py --version 1.2.3                    # 自定义版本号（默认取 bundle md5 前 8 位）
"""
from __future__ import annotations

import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import zipfile
from datetime import datetime, timezone, timedelta

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TEMPLATE = os.path.join(ROOT, "scripts", "templates", "dist-README.md")
DEFAULT_BUNDLE = os.path.join(ROOT, "clients", "vtouch_bundle.js")
DEFAULT_EXAMPLE = os.path.join(ROOT, "clients", "vtouch_region_min.js")


def md5_file(p: str) -> str:
    h = hashlib.md5()
    with open(p, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def git_commit() -> str:
    try:
        r = subprocess.run(["git", "rev-parse", "--short", "HEAD"], cwd=ROOT,
                           capture_output=True, text=True)
        return r.stdout.strip() if r.returncode == 0 else ""
    except Exception:  # noqa: BLE001
        return ""


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--bundle", default=DEFAULT_BUNDLE)
    ap.add_argument("--example", default=DEFAULT_EXAMPLE)
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "dist"))
    ap.add_argument("--template", default=TEMPLATE)
    ap.add_argument("--version", default="")
    ap.add_argument("--verify", action="store_true", help="打包前先跑 scripts/verify_bundle.py")
    ap.add_argument("--bin", default="", help="配合 --verify：build/vtouchd")
    ap.add_argument("--ui", default="", help="配合 --verify：build/ui")
    ap.add_argument("--allow-headless", action="store_true")
    args = ap.parse_args()

    for p, what in ((args.bundle, "bundle"), (args.example, "调用示例"), (args.template, "README 模板")):
        if not os.path.exists(p):
            print("缺 %s: %s" % (what, p))
            return 1

    bundle_md5 = md5_file(args.bundle)
    bundle_size = os.path.getsize(args.bundle)
    version = args.version or bundle_md5[:8]

    if args.verify:
        cmd = [sys.executable, os.path.join(ROOT, "scripts", "verify_bundle.py"),
               "--bundle", args.bundle]
        if args.bin:
            cmd += ["--bin", args.bin]
        if args.ui:
            cmd += ["--ui", args.ui]
        if args.allow_headless:
            cmd += ["--allow-headless"]
        r = subprocess.run(cmd, cwd=ROOT)
        if r.returncode != 0:
            print("\n对账没过，拒绝打包（先修产物）。")
            return 1
        print()

    name = "vtouch-bundle-%s-arm64" % version
    dist = os.path.join(args.out, name)
    if os.path.exists(dist):
        shutil.rmtree(dist)
    os.makedirs(dist)

    # ① bundle 本体
    shutil.copy2(args.bundle, os.path.join(dist, "vtouch_bundle.js"))

    # ② 调用示例 = 源码示例 + 自动生成的头部（单一来源，别手抄）
    ex = open(args.example, encoding="utf-8").read()
    banner = (
        "/* ============================================================================\n"
        " * vtouch 调用示例（交付包自带）\n"
        " *\n"
        " * 来源：clients/%s（md5 %s）\n"
        " * 配套：同目录 vtouch_bundle.js（md5 %s，%d B）——推手机后 require 它。\n"
        " *\n"
        " * 用法：adb push vtouch_bundle.js /sdcard/vtouch_bundle.js\n"
        " *       adb push example.js /sdcard/vtouch_example.js\n"
        " *       在 AutoJs6 里运行 /sdcard/vtouch_example.js\n"
        " * 详见同目录 README.md。\n"
        " * ==========================================================================*/\n"
        % (os.path.basename(args.example), md5_file(args.example), bundle_md5, bundle_size)
    )
    open(os.path.join(dist, "example.js"), "w", encoding="utf-8").write(banner + ex)

    # ③ 包内 README（模板填数字）
    tpl = open(args.template, encoding="utf-8").read()
    now = datetime.now(timezone(timedelta(hours=8))).strftime("%Y-%m-%d %H:%M %z")
    commit = git_commit()
    readme = (tpl.replace("{{VERSION}}", version)
                 .replace("{{BUNDLE_MD5}}", bundle_md5)
                 .replace("{{BUNDLE_SIZE}}", str(bundle_size))
                 .replace("{{DATE}}", now)
                 .replace("{{COMMIT_LINE}}", ("，源码 commit `%s`" % commit) if commit else ""))
    assert "{{" not in readme, "模板还有没替换的占位符"
    open(os.path.join(dist, "README.md"), "w", encoding="utf-8").write(readme)

    # ④ md5 清单
    files = ["vtouch_bundle.js", "example.js", "README.md"]
    with open(os.path.join(dist, "md5.txt"), "w", encoding="utf-8", newline="\n") as f:
        for fn in files:
            f.write("%s  %s\n" % (md5_file(os.path.join(dist, fn)), fn))

    # ⑤ 打 zip（CI 下载/分发用）
    zpath = os.path.join(args.out, name + ".zip")
    with zipfile.ZipFile(zpath, "w", zipfile.ZIP_DEFLATED) as z:
        for fn in files + ["md5.txt"]:
            z.write(os.path.join(dist, fn), os.path.join(name, fn))

    print("交付包: %s" % dist)
    for fn in files + ["md5.txt"]:
        p = os.path.join(dist, fn)
        print("  %-20s %9d B  %s" % (fn, os.path.getsize(p), md5_file(p)))
    print("  zip: %s (%d B)" % (zpath, os.path.getsize(zpath)))
    print("\n把 vtouch_bundle.js + example.js 推 /sdcard/ 即可用；README.md 是本包说明。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
