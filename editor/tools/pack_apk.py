#!/usr/bin/env python3
"""打包未签名 APK：AndroidManifest.xml + resources.arsc + classes.dex + 额外文件。

用法: pack_apk.py <manifest.bin> <classes.dex> <out.apk> [apk内路径=本地文件 ...]
约定：AndroidManifest.xml / resources.arsc 及图标等资源用 STORED（资源惯例）。
"""

import os
import sys
import zipfile

FIXED_TS = (2026, 1, 1, 0, 0, 0)


def add(z, apk_path, src_path, stored):
    zi = zipfile.ZipInfo(apk_path, FIXED_TS)
    zi.compress_type = zipfile.ZIP_STORED if stored else zipfile.ZIP_DEFLATED
    with open(src_path, "rb") as fp:
        z.writestr(zi, fp.read())


def main():
    manifest, dex, out = sys.argv[1], sys.argv[2], sys.argv[3]
    extras = []
    for a in sys.argv[4:]:
        k, v = a.split("=", 1)
        extras.append((k, v))
    with zipfile.ZipFile(out, "w") as z:
        add(z, "AndroidManifest.xml", manifest, True)
        for k, v in extras:
            if k == "resources.arsc":
                add(z, "resources.arsc", v, True)
        add(z, "classes.dex", dex, False)
        for k, v in extras:
            if k == "resources.arsc":
                continue
            add(z, k, v, True)
    print("APK(未签名) 已打包: %s  %d bytes" % (out, os.path.getsize(out)))


if __name__ == "__main__":
    main()
