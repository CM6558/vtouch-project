#!/usr/bin/env python3
"""手写最小 resources.arsc：单包(0x7f) / 单类型(mipmap) / 单条目(ic_launcher)。

用法: arsc_encode.py <out.arsc>
口径：目标 Android 24+；默认配置（无密度限定）；条目值 = TYPE_STRING（APK 内文件路径
      "res/mipmap/ic_launcher.png"），清单里以 @0x7f010000 引用（axml_encode.py 支持 @0x 引用）。
"""
import struct
import sys


def u16(v): return struct.pack("<H", v & 0xFFFF)
def u32(v): return struct.pack("<I", v & 0xFFFFFFFF)
def u8(v): return struct.pack("<B", v & 0xFF)


def varint(v):
    if v > 0x7F:
        return bytes([(v >> 8) | 0x80, v & 0xFF])
    return bytes([v])


def chunk(typ, header_extra, body):
    hsize = 8 + len(header_extra)
    size = hsize + len(body)
    pad = (-size) % 4
    return u16(typ) + u16(hsize) + u32(size + pad) + header_extra + body + b"\x00" * pad


def string_pool(strings):
    offsets = []
    data = b""
    for s in strings:
        b = s.encode("utf-8")
        utf16_len = len(s.encode("utf-16-le")) // 2
        buf = varint(utf16_len) + varint(len(b)) + b + b"\x00"
        offsets.append(len(data))
        pad = (-len(buf)) % 4
        data += buf + b"\x00" * pad
    count = len(strings)
    strings_start = 28 + 4 * count
    header_extra = u32(count) + u32(0) + u32(0x100) + u32(strings_start) + u32(0)
    body = b"".join(u32(o) for o in offsets) + data
    return chunk(0x0001, header_extra, body)


def config_default():
    # ResTable_config（v1 口径 size=28）：仅置 size，其余全零 = 默认配置（无密度限定）
    return struct.pack("<IHH2s2sBBHBBBBHHHH",
                       28, 0, 0, b"\x00\x00", b"\x00\x00",
                       0, 0, 640, 0, 0, 0, 0, 0, 0, 0, 0)   # density=640（本机 4x）


def main():
    out_path = sys.argv[1]
    pkg = "com.vtouch.editor"
    type_name = "mipmap"
    key_name = "ic_launcher"
    file_path = "res/mipmap/ic_launcher.png"

    value_pool = string_pool([file_path])      # 全局值池：索引 0 = 图标文件路径
    type_pool = string_pool([type_name])
    key_pool = string_pool([key_name])

    # --- package 块（headerSize = 288 = 8 前缀 + id 4 + name 256 + 5 个偏移 20）---
    name = pkg.encode("utf-16-le")
    name += b"\x00" * (256 - len(name))
    key_off = 288 + len(type_pool)
    pkg_header_extra = (u32(0x7F) + name
                        + u32(288) + u32(1)          # typeStrings, lastPublicType
                        + u32(key_off) + u32(1)      # keyStrings, lastPublicKey
                        + u32(0))                    # typeIdOffset
    assert len(pkg_header_extra) == 280, len(pkg_header_extra)

    # --- TypeSpec（0x0202）+ Type（0x0201）---
    spec = chunk(0x0202, u8(1) + u8(0) + u16(0) + u32(1), u32(0))
    cfg = config_default()
    entry = (u16(8) + u16(0) + u32(0)                    # ResTable_entry: size=8, flags=0, key=0
             + u16(8) + u8(0) + u8(0x03) + u32(0))       # Res_value: size=8, res0=0, TYPE_STRING, data=0
    type_extra = (u8(1) + u8(0) + u16(0) + u32(1) + u32(48 + 4) + cfg)   # entriesStart = 48+偏移表(4)
    type_chunk = chunk(0x0201, type_extra, u32(0) + entry)

    pkg_chunk = chunk(0x0200, pkg_header_extra, type_pool + key_pool + spec + type_chunk)

    rest = value_pool + pkg_chunk
    out = u16(0x0002) + u16(12) + u32(12 + len(rest)) + u32(1) + rest
    with open(out_path, "wb") as fp:
        fp.write(out)
    print("ARSC 已生成: %s  %d bytes" % (out_path, len(out)))


if __name__ == "__main__":
    main()
