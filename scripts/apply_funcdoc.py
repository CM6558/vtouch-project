# -*- coding: utf-8 -*-
"""把 build/_funcdoc.py 的文档注入 src/*.c（定义处完整 Doxygen 块）与 src/vt_internal.h（原型处一句话）。

两个要点（都是踩过的坑）：
1) 插入点必须在**原文件行号**上一次性算完，再自底向上插入 —— 边算边插会让后面行号漂移，文档插到别的函数头上；
2) 全程用 str 处理，只在读写文件时做编码转换，别让字节/字符串判断混用（会直接抛 TypeError）。
幂等：同一函数上方已有它的 @brief 就跳过，可重复执行。
"""
import io, os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SRC = os.path.join(ROOT, "src")
sys.path.insert(0, HERE)
from funcdoc_data import DOCS

C_FILES = ["vt_util.c", "vt_queue.c", "vt_region.c", "vt_input.c", "vt_frame.c", "vt_ws.c", "vtouchd.c"]

def read_lines(path):
    text = io.open(path, "rb").read().decode("utf-8")
    return text, text.split("\n")

def write_lines(path, lines):
    io.open(path, "wb").write("\n".join(lines).encode("utf-8"))

MARK = "(vtouch-doc: %s)"

def doc_block(name):
    d = DOCS[name]
    out = ["/**", " * " + MARK % name, " * @brief " + d["brief"]]
    for p, desc in d.get("params", []):
        out.append(" * @param   %-8s %s" % (p, desc))
    if d.get("ret"):
        out.append(" * @return  " + d["ret"])
    if d.get("note"):
        out.append(" * @note    " + d["note"])
    out.append(" */")
    return out

def first_sentence(s):
    for sep in ("。", "；"):
        i = s.find(sep)
        if 0 < i <= 72:
            return s[: i + 1]
    for sep in ("：", "，", "、"):
        i = s.find(sep)
        if 0 < i <= 72:
            return s[: i + 1]
    return s if len(s) <= 74 else s[:72] + "…"

def up_to_comment_start(lines, i):
    """从定义行 i 向上跨过紧贴的注释/空行，返回该注释块起始行；没有则返回 i。"""
    j = i - 1
    while j >= 0 and lines[j].strip() == "":
        j -= 1
    if j < 0:
        return i
    s = lines[j].strip()
    if s.endswith("*/"):
        k = j
        while k >= 0 and not lines[k].lstrip().startswith("/*"):
            k -= 1
        return k if k >= 0 else i
    if s.startswith("/*"):
        return j
    k = j
    while k >= 0 and lines[k].strip().startswith("//"):
        k -= 1
    return k + 1 if k + 1 <= j else i

def comment_region_start(lines, i):
    """定义行上方连续注释/空行区的起始行（跨过任意长度，别用固定窗口）。"""
    j = i
    while j > 0:
        s = lines[j - 1].strip()
        if s == "" or s.startswith("//") or s == "/*" or s.startswith("*") or s.endswith("*/") or s.startswith("/*"):
            j -= 1
        else:
            break
    return j

def already_documented(lines, i, name):
    region = "\n".join(lines[comment_region_start(lines, i) : i])
    return (MARK % name) in region

def is_definition(line):
    if not line or line[0] in " \t/*#})":
        return None
    if line.rstrip().endswith(";"):
        return None
    m = re.match(r"^[A-Za-z_][\w \t\*]*?\b([A-Za-z_]\w*)\s*\(", line)
    return m.group(1) if m else None

total_added = 0
for fn in C_FILES:
    path = os.path.join(SRC, fn)
    text, lines = read_lines(path)
    ins, names = [], []
    for idx, line in enumerate(lines):
        name = is_definition(line)
        if not name or name not in DOCS or name in names:
            continue
        names.append(name)
        if already_documented(lines, idx, name):
            continue
        ins.append((comment_region_start(lines, idx), doc_block(name)))
    for at, blk in sorted(ins, key=lambda x: -x[0]):
        lines[at:at] = blk
    write_lines(path, lines)
    total_added += len(ins)
    print("  %-12s 覆盖函数 %2d 个，本次写入 %2d 个" % (fn, len(names), len(ins)))

# 头文件原型：一句话（先把上一版生成的清掉，避免截断残留）
path = os.path.join(SRC, "vt_internal.h")
text, lines = read_lines(path)
stale = 0
for idx, line in enumerate(lines):
    if idx == 0 or not line or line[0] in " \t" or not line.rstrip().endswith(";"):
        continue
    m = re.match(r"^[A-Za-z_][\w \t\*]*?\b([A-Za-z_]\w*)\s*\(", line)
    if not m or m.group(1) not in DOCS:
        continue
    name = m.group(1)
    prev = lines[idx - 1]
    if prev.startswith("/* ") and prev.endswith("*/"):
        body = prev[3:-2].strip()
        if MARK % name not in prev:
            lines[idx - 1] = ""
            stale += 1
print("  头文件清理旧一句话：%d 条" % stale)
ins = []
for idx, line in enumerate(lines):
    if not line or line[0] in " \t" or not line.rstrip().endswith(";"):
        continue
    m = re.match(r"^[A-Za-z_][\w \t\*]*?\b([A-Za-z_]\w*)\s*\(", line)
    if not m or m.group(1) not in DOCS:
        continue
    name = m.group(1)
    if already_documented(lines, idx, name):
        continue
    ins.append((idx, ["/* " + first_sentence(DOCS[name]["brief"]) + " " + MARK % name + " */"]))
for at, blk in sorted(ins, key=lambda x: -x[0]):
    lines[at:at] = blk
write_lines(path, lines)
print("  %-12s 原型一句话 本次写入 %2d 个" % ("vt_internal.h", len(ins)))
print("合计写入文档：%d 处（数据表共 %d 个函数）" % (total_added + len(ins), len(DOCS)))

# 覆盖核对
miss = []
for fn in C_FILES:
    text, lines = read_lines(os.path.join(SRC, fn))
    for idx, line in enumerate(lines):
        name = is_definition(line)
        if not name or name not in DOCS:
            continue
        if (MARK % name) not in "\n".join(lines[comment_region_start(lines, idx) : idx]):
            miss.append("%s:%d %s" % (fn, idx + 1, name))
print("没有文档的函数：", miss if miss else "无 ✓")
