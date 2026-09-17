#!/usr/bin/env python3
"""把核心二进制 base64 内嵌进 clients/vtouch.js，生成**自包含单文件**客户端。

用法：
    python scripts/pack_client.py                        # build/vtouchd_ui → build/vtouch_onefile.js
    python scripts/pack_client.py <核心二进制> <输出路径>

产物在 build/ 下（不入库）。生成物 = 可读源码 + 一行 base64 负载 + md5/长度常量；
require 时若设备上没有该二进制或版本不对，会自己写进去并校验 md5。
"""
import base64, hashlib, pathlib, re, sys

root = pathlib.Path(__file__).resolve().parent.parent
src = root / "clients" / "vtouch.js"
bin_path = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else root / "build" / "vtouchd_ui"
out = pathlib.Path(sys.argv[2]) if len(sys.argv) > 2 else root / "build" / "vtouch_onefile.js"

if not src.is_file():
    sys.exit("pack_client: 找不到源码 %s" % src)
if not bin_path.is_file():
    sys.exit("pack_client: 找不到核心二进制 %s（先 sh scripts/build.sh ui）" % bin_path)

data = bin_path.read_bytes()
md5 = hashlib.md5(data).hexdigest()
b64 = base64.b64encode(data).decode("ascii")
js = src.read_text(encoding="utf-8")


def sub1(pattern, replacement, text, what):
    new, n = re.subn(pattern, lambda m: replacement, text, count=1)
    if n != 1:
        sys.exit("pack_client: 源码里找不到占位符（%s）—— 是不是已经被打包过了？" % what)
    return new


js = sub1(r'var PAYLOAD = null;[ \t]*/\* <<PAYLOAD>> \*/',
          'var PAYLOAD = "%s";   /* <<PAYLOAD>> 内嵌核心 base64（pack_client.py 填） */' % b64, js, "PAYLOAD")
js = sub1(r'var PAYLOAD_MD5 = null;[ \t]*/\* <<PAYLOAD_MD5>> \*/',
          'var PAYLOAD_MD5 = "%s";   /* <<PAYLOAD_MD5>> */' % md5, js, "PAYLOAD_MD5")
js = sub1(r'var PAYLOAD_SIZE = 0;[ \t]*/\* <<PAYLOAD_SIZE>> \*/',
          'var PAYLOAD_SIZE = %d;   /* <<PAYLOAD_SIZE>> */' % len(data), js, "PAYLOAD_SIZE")

out.parent.mkdir(parents=True, exist_ok=True)
out.write_text(js, encoding="utf-8", newline="\n")
print("已生成 %s" % out)
print("  内嵌 %s：%d 字节，md5=%s" % (bin_path.name, len(data), md5))
print("  产物大小：%.2f MB（JS 源码 %.0f KB + base64 负载）" % (out.stat().st_size / 1048576.0, src.stat().st_size / 1024.0))
print("  推到设备：adb push %s /sdcard/vtouch.js" % out)
