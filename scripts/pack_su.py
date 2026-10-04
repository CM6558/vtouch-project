#!/usr/bin/env python3
"""把核心二进制 base64 内嵌进 scripts/vtouch.sh.in（模板），生成设备侧自包含入口 build/vtouch.sh。

用法：
    python scripts/pack_su.py                        # build/vtouchd_ui → build/vtouch.sh
    python scripts/pack_su.py <核心二进制> <输出路径>
"""
import base64, hashlib, pathlib, re, sys, textwrap

root = pathlib.Path(__file__).resolve().parent.parent
src = root / "scripts" / "vtouch.sh.in"
bin_path = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else root / "build" / "vtouchd_ui"
out = pathlib.Path(sys.argv[2]) if len(sys.argv) > 2 else root / "build" / "vtouch.sh"

if not src.is_file():
    sys.exit("pack_su: 找不到模板 %s" % src)
if not bin_path.is_file():
    sys.exit("pack_su: 找不到核心 %s（先 sh scripts/build.sh ui）" % bin_path)

data = bin_path.read_bytes()
md5 = hashlib.md5(data).hexdigest()
b64 = "\n".join(textwrap.wrap(base64.b64encode(data).decode("ascii"), 76))


def sub1(pattern, replacement, text, what):
    new, n = re.subn(pattern, lambda m: replacement, text, count=1)
    if n != 1:
        sys.exit("pack_su: 模板里找不到占位符（%s）—— 是不是已经被打包过？" % what)
    return new


sh = src.read_text(encoding="utf-8")
sh = sub1(r'<<PAYLOAD_MD5>>', md5, sh, "PAYLOAD_MD5")
sh = sub1(r'<<PAYLOAD_SIZE>>', str(len(data)), sh, "PAYLOAD_SIZE")
sh = sub1(r'<<PAYLOAD>>', b64, sh, "PAYLOAD")

# 自检：载荷解回来必须与源二进制逐字节一致（打包器自己的门）
back = base64.b64decode("".join(b64.split()))
if back != data:
    sys.exit("pack_su: 自检失败：载荷解码与源二进制不一致")

out.parent.mkdir(parents=True, exist_ok=True)
out.write_text(sh, encoding="utf-8", newline="\n")   # 设备侧脚本必须 LF
print("已生成 %s" % out)
print("  内嵌 %s：%d 字节，md5=%s" % (bin_path.name, len(data), md5))
print("  产物大小：%.2f MB" % (out.stat().st_size / 1048576.0))
print("  推到设备：adb push %s /sdcard/vtouch.sh" % out)
