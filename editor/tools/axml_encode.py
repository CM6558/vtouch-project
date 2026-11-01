#!/usr/bin/env python3
"""文本 AndroidManifest.xml -> 二进制 AXML（Termux 无 aapt2 环境下手写）。

用法: axml_encode.py <in.xml> <out.bin>
支持: android 命名空间白名单属性 + 无命名空间属性(如 package)。
值类型: 字符串 / 整数(int(v,0)) / 布尔。
属性名放字符串池最前面并按资源ID升序排列（与 aapt 输出的 resmap 语义一致）。
"""

from __future__ import annotations

import struct
import sys
import xml.etree.ElementTree as ET

ANDROID_NS = "http://schemas.android.com/apk/res/android"

# framework 属性资源 ID（已用真机 APK 解析核对）
ATTR_IDS = {
    "theme": 0x01010000, "label": 0x01010001, "icon": 0x01010002,
    "name": 0x01010003, "enabled": 0x0101000E, "debuggable": 0x0101000F,
    "exported": 0x01010010, "process": 0x01010011, "launchMode": 0x0101001D,
    "screenOrientation": 0x0101001E, "minSdkVersion": 0x0101020C,
    "versionCode": 0x0101021B, "versionName": 0x0101021C,
    "targetSdkVersion": 0x01010270, "allowBackup": 0x01010280,
    "windowSoftInputMode": 0x0101022B,
    "foregroundServiceType": 0x01010599,
}
INT_ATTRS = {"minSdkVersion", "versionCode", "targetSdkVersion",
             "launchMode", "screenOrientation", "windowSoftInputMode"}
BOOL_ATTRS = {"debuggable", "exported", "enabled", "allowBackup"}
# foregroundServiceType 的旗标名 -> 整数（值来自 android.content.pm.ServiceInfo）
FGS_TYPE_FLAGS = {"dataSync": 1, "mediaPlayback": 2, "phoneCall": 4,
                  "location": 8, "connectedDevice": 16, "mediaProjection": 32,
                  "camera": 64, "microphone": 128, "health": 256,
                  "remoteMessaging": 512, "systemExempted": 1024,
                  "shortService": 2048, "specialUse": 0x40000000}


def u16(v):
    return struct.pack("<H", v & 0xFFFF)


def u32(v):
    return struct.pack("<I", v & 0xFFFFFFFF)


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


class AXML:
    def __init__(self, used_attrs):
        self.attr_list = sorted(used_attrs, key=lambda n: ATTR_IDS[n])
        self.resmap = [ATTR_IDS[n] for n in self.attr_list]
        self.rest = []
        self.rest_idx = {}
        self.uri_i = None
        self.prefix_i = None

    def sid(self, s):
        if s in ATTR_IDS and s in self.attr_list:
            return self.attr_list.index(s)
        if s in self.rest_idx:
            return len(self.attr_list) + self.rest_idx[s]
        self.rest_idx[s] = len(self.rest)
        self.rest.append(s)
        return len(self.attr_list) + self.rest_idx[s]

    def pool_strings(self):
        return self.attr_list + self.rest


def collect_used_attrs(root):
    used = set()

    def walk(el):
        for k in el.attrib:
            if k.startswith("{"):
                ns, nm = k[1:].split("}")
                if ns != ANDROID_NS:
                    raise SystemExit("不支持的命名空间: " + ns)
                used.add(nm)
        for c in el:
            walk(c)

    walk(root)
    return used


def encode(root):
    used = collect_used_attrs(root)
    for n in used:
        if n not in ATTR_IDS:
            raise SystemExit(f"属性 android:{n} 未登记资源ID，先在 ATTR_IDS 里加")
    ax = AXML(used)
    ax.prefix_i = ax.sid("android")
    ax.uri_i = ax.sid(ANDROID_NS)

    body = b""
    ns_body = u32(ax.prefix_i) + u32(ax.uri_i)
    # start namespace（XML 节点 chunk 的 headerSize 固定为 16，字段都在 contents 里）
    body += chunk(0x0100, u32(0xFFFFFFFF) + u32(0xFFFFFFFF), ns_body)

    counter = [1]

    def enc_el(el):
        tag = el.tag if not el.tag.startswith("{") else el.tag.split("}")[1]
        n_i = ax.sid(tag)
        attrs = []
        for k, v in el.attrib.items():
            if k.startswith("{"):
                _ns, an = k[1:].split("}")
                a_i = ax.sid(an)
                if an == "foregroundServiceType":
                    attrs.append((ax.uri_i, a_i, 0xFFFFFFFF, 0x10,
                                  FGS_TYPE_FLAGS.get(str(v).strip(), 0)))
                elif str(v).startswith("@0x"):
                    # 资源引用（TYPE_REFERENCE）：如 @0x7f010000（自建 ARSC 的图标条目）
                    attrs.append((ax.uri_i, a_i, 0xFFFFFFFF, 0x01, int(str(v)[3:], 16)))
                elif an in INT_ATTRS:
                    attrs.append((ax.uri_i, a_i, 0xFFFFFFFF, 0x10, int(v, 0)))
                elif an in BOOL_ATTRS:
                    attrs.append((ax.uri_i, a_i, 0xFFFFFFFF, 0x12,
                                  1 if str(v).lower() == "true" else 0))
                else:
                    vi = ax.sid(v)
                    attrs.append((ax.uri_i, a_i, vi, 0x03, vi))
            else:
                a_i = ax.sid(k)
                vi = ax.sid(v)
                attrs.append((0xFFFFFFFF, a_i, vi, 0x03, vi))
        # android 属性按 id 升序；无命名空间属性排最后
        def keyf(a):
            ns_i, name_i, _raw, _t, _d = a
            if ns_i == 0xFFFFFFFF:
                return (1, name_i)
            return (0, ATTR_IDS[ax.attr_list[name_i]])

        attrs.sort(key=keyf)
        counter[0] += 1
        ext = (u32(0xFFFFFFFF) + u32(n_i) + u16(0x14) + u16(0x14)
               + u16(len(attrs)) + u16(0) + u16(0) + u16(0))
        attr_bytes = b"".join(
            u32(a) + u32(b) + u32(c) + u16(8) + bytes([0, t]) + u32(d)
            for (a, b, c, t, d) in attrs)
        out = chunk(0x0102, u32(counter[0]) + u32(0xFFFFFFFF), ext + attr_bytes)
        for c in el:
            out += enc_el(c)
        counter[0] += 1
        out += chunk(0x0103, u32(counter[0]) + u32(0xFFFFFFFF),
                     u32(0xFFFFFFFF) + u32(n_i))
        return out

    body += enc_el(root)
    # end namespace
    body += chunk(0x0101, u32(0xFFFFFFFF) + u32(0xFFFFFFFF), ns_body)

    pool_bin = string_pool(ax.pool_strings())
    resmap_bin = chunk(0x0180, b"", b"".join(u32(i) for i in ax.resmap))
    rest = pool_bin + resmap_bin + body
    return u16(0x0003) + u16(8) + u32(8 + len(rest)) + rest


def main():
    src, dst = sys.argv[1], sys.argv[2]
    root = ET.parse(src).getroot()
    out = encode(root)
    with open(dst, "wb") as fp:
        fp.write(out)
    print(f"AXML 已生成: {dst}  {len(out)} bytes")


if __name__ == "__main__":
    main()
