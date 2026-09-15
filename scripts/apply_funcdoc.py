# -*- coding: utf-8 -*-
"""把 build/funcdoc 的文案写成 src/*.c 定义处的**单一** Doxygen 文档块（紧贴定义），并给 vt_internal.h 原型加一句话。

规则（就是「在函数定义处写清楚函数文档」的落地）：
  1) 每个函数定义正上方只有一个注释块 —— 文档块本身，中间不留空行；
  2) 文档块 = 机器标记 + @brief/@param/@return/@note + 原有的「为什么这么写」注释（逐字并入，作正文）；
  3) 段落横幅（`/* ---- §x ---- */` 这类）不并入、原地保留在文档块上方；
  4) 全程幂等：期望结果先算出来跟现状比，一样就不写（可反复跑）；
  5) 只在读写文件时做编码转换，行号一律基于原文件一次算完再自底向上改（否则插入会互相错位）。

用法：python scripts/apply_funcdoc.py [--check]   （--check 只报告不改）
"""
import io, os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SRC = os.path.join(ROOT, "src")
sys.path.insert(0, HERE)
from funcdoc_data import DOCS

C_FILES = ["vt_util.c", "vt_queue.c", "vt_region.c", "vt_input.c", "vt_frame.c", "vt_ws.c", "vtouchd.c"]
MARK = "(vtouch-doc: %s)"
CHECK_ONLY = "--check" in sys.argv

def read_lines(path):
    text = io.open(path, "rb").read().decode("utf-8")
    return text, text.split("\n")

def write_lines(path, lines):
    io.open(path, "wb").write("\n".join(lines).encode("utf-8"))

def is_definition(line):
    if not line or line[0] in " \t/*#})" or line.rstrip().endswith(";"):
        return None
    m = re.match(r"^[A-Za-z_][\w \t\*]*?\b([A-Za-z_]\w*)\s*\(", line)
    return m.group(1) if m else None

def region_start(lines, i):
    """定义行上方连续的注释/空行区起点。注意：只认**整行**注释 —— 行尾带注释的代码行
    （如 `static int x; /* ... */`）是代码，必须当作边界，否则文档会被插到文件头去。"""
    j = i
    while j > 0:
        s = lines[j - 1].lstrip()
        if s.strip() == "" or s.startswith("/*") or s.startswith("*") or s.startswith("//"):
            j -= 1
        else:
            break
    return j

def last_block_start(lines, i):
    """定义行上方**紧贴**的注释块起点：块内不许有空行（空行一出现就不再是同一块）。"""
    j = i
    while j > 0 and lines[j - 1].lstrip().startswith(("/*", "*", "//")):
        j -= 1
    return j

def cluster_start(lines, i):
    """定义上方「紧贴的那一簇」起点：从下往上连续的非横幅注释块（横幅是边界，原地不动）。
    替换区间就是 [簇起点, 定义行) —— 不能用「最后一个横幅之后」，横幅与文档块连在一起时会算出空区间，
    那就变成纯插入：旧块永远删不掉，每跑一遍叠一层（踩过）。"""
    jj = region_start(lines, i)
    blocks = split_blocks(lines, jj, i)
    k = i
    for a, b in reversed(blocks):
        if is_banner(lines[a:b]):
            break
        k = a
    return k

def inside_rationale(blk):
    """从已合并的文档块里把「为什么这么写」正文取回来（保证第二遍不丢内容）。"""
    out, inrat = [], False
    for x in blk:
        if x.strip() == "*/":
            break
        t = clean_line(x)                       # 先去掉行首的 ` *`，否则表头判断永远不成立
        if t.startswith("为什么这么写"):
            inrat = True
            continue
        if inrat and t:
            out.append(t)
    return out

def split_blocks(lines, a, b):
    """把 [a,b) 按空行切成若干块，返回 [(start,end), ...]。"""
    blocks, cur = [], None
    for k in range(a, b):
        if lines[k].strip() == "":
            if cur is not None:
                blocks.append((cur, k)); cur = None
        elif cur is None:
            cur = k
    if cur is not None:
        blocks.append((cur, b))
    return blocks

def is_banner(block_lines):
    s = "\n".join(block_lines)
    return bool(re.match(r"^\s*/\*+\s*[-=]{3,}", block_lines[0])) or "----" in block_lines[0] or "=====" in block_lines[0]

def clean_line(s):
    """去掉注释符与两侧空白，取出正文。"""
    s = s.strip()
    if s.startswith("/*"): s = s[2:]
    if s.startswith("*"): s = s[1:]
    if s.endswith("*/"): s = s[:-2]
    return s.strip()

def _norm(s):
    return re.sub(r"[\s：:（）()、,，。；;·\-—/]", "", s)

def is_restating_brief(txt, brief):
    """这行注释是不是在把 @brief 重说一遍（归一化去标点后互为子串）。"""
    if len(txt) != 1:
        return " ".join(txt).rstrip("。") == brief
    t, b = _norm(txt[0]), _norm(brief)
    return len(t) >= 6 and (t in b or b in t)

def is_doc_block(blk, name):
    return bool(blk) and blk[0].strip() == "/**" and (MARK % name) in "\n".join(blk)

def doc_block(name, rationale=()):
    d = DOCS[name]
    out = ["/**", " * " + MARK % name, " * @brief " + d["brief"]]
    for p, desc in d.get("params", []):
        out.append(" * @param   %-8s %s" % (p, desc))
    if d.get("ret"):
        out.append(" * @return  " + d["ret"])
    if d.get("note"):
        out.append(" * @note    " + d["note"])
    if rationale:
        out += [" *", " * 为什么这么写（原有注释，逐字保留）："]
        out += [" *   " + t for t in rationale]
    out.append(" */")
    return out

def wanted_comment(lines, i, name):
    """定义行 i 上方**应该**是什么样：返回 (替换区间起点, 重建后的整段)。
    形态：段落横幅按原顺序留在上面（当章节头）→ 唯一一块文档块紧贴定义。
    重建是收敛的：第二遍算出来的东西与第一遍写下的完全一致（所以可反复跑）。
    不能只换「最后一个横幅之后」（横幅与文档块之间没空行时会算出空区间，旧块删不掉、每跑一遍叠一层）。"""
    jj = region_start(lines, i)
    while jj < i and lines[jj].strip() == "":        # 保留与上一块之间的空行
        jj += 1
    blocks = split_blocks(lines, jj, i)
    brief = DOCS[name]["brief"].rstrip("。")
    banners, rationale = [], []
    for a, b in blocks:
        if is_banner(lines[a:b]):
            banners.append([lines[k] for k in range(a, b)])
            continue
        txt = [clean_line(lines[k]) for k in range(a, b)]
        txt = [t for t in txt if t]
        if any(MARK % name in t for t in txt):       # 文档块：把已并入的正文取回来，别丢
            rationale += inside_rationale(lines[a:b])
        elif is_restating_brief(txt, brief):         # 与 @brief 重复的一行短注释：丢掉
            continue
        else:
            rationale += txt
    seen, ded = set(), []
    for t in rationale:
        if t not in seen:
            seen.add(t); ded.append(t)
    eol = "\r" if lines[i].endswith("\r") else ""      # 跟着原文件的行尾走，别混出 CRLF/LF
    new = []
    for bn in banners:
        new += bn + [eol]
    new += [x + eol for x in doc_block(name, ded)]
    return jj, new

def stale_block_edits(lines):
    """同名文档块只允许存在紧贴定义的那一份；其它位置上的（早期误插留下的孤儿块）删掉。"""
    edits = []
    for i, line in enumerate(lines):
        name = is_definition(line)
        if not name or name not in DOCS:
            continue
        keep_start = last_block_start(lines, i)
        for k, x in enumerate(lines):
            if (MARK % name) not in x or keep_start <= k < i:
                continue
            a = next((x for x in range(k, -1, -1) if lines[x].strip() == "/**"), None)
            b = next((x for x in range(k, len(lines)) if lines[x].strip() == "*/"), None)
            if a is None or b is None:
                continue
            edits.append((a, b + 1, []))
    return edits

def apply_c(fn):
    path = os.path.join(SRC, fn)
    text, lines = read_lines(path)
    edits, n_found = [], 0
    for i in range(len(lines)):
        name = is_definition(lines[i])
        if not name or name not in DOCS:
            continue
        n_found += 1
        at, want = wanted_comment(lines, i, name)
        if [x for x in lines[at:i]] == [x for x in want]:
            continue                                                 # 已经就是要的样子
        edits.append((at, i, want))
    if not CHECK_ONLY:
        for j, i, want in sorted(edits, key=lambda e: -e[0]):
            lines[j:i] = want
        # 再清一遍孤儿块（上一遍刚把文档块挪到定义处，这里删掉留在旧位置的那份）
        for a, b, _ in sorted(stale_block_edits(lines), key=lambda e: -e[0]):
            del lines[a:b]
            edits.append((a, b, []))
        write_lines(path, lines)
    return n_found, len(edits)

def apply_h():
    path = os.path.join(SRC, "vt_internal.h")
    text, lines = read_lines(path)
    edits = []
    for i, line in enumerate(lines):
        if not line or line[0] in " \t" or not line.rstrip().endswith(";"):
            continue
        m = re.match(r"^[A-Za-z_][\w \t\*]*?\b([A-Za-z_]\w*)\s*\(", line)
        if not m or m.group(1) not in DOCS:
            continue
        name = m.group(1)
        want = "/* " + first_sentence(DOCS[name]["brief"]) + " " + MARK % name + " */"
        prev = lines[i - 1] if i else ""
        if prev.strip() == want.strip():
            continue
        if MARK % name in prev:                                      # 旧版本一句话：换掉
            edits.append((i - 1, i, [want]))
        elif prev.lstrip().startswith("/*") and name in prev:
            continue                                                 # 本函数已有手写说明，尊重它
        else:
            edits.append((i, i, [want]))
    if not CHECK_ONLY:
        for a, b, want in sorted(edits, key=lambda e: -e[0]):
            lines[a:b] = want
        write_lines(path, lines)
    return len(edits)

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

tot_edit = 0
for fn in C_FILES:
    found, edited = apply_c(fn)
    tot_edit += edited
    print("  %-12s 函数 %2d 个，本次调整 %2d 处" % (fn, found, edited))
h = apply_h()
print("  %-12s 原型一句话 本次调整 %2d 处" % ("vt_internal.h", h))
print("%s：本次共调整 %d 处" % ("检查模式" if CHECK_ONLY else "已写入", tot_edit + h))

# 不变式：每个函数定义正上方必须紧贴一个带标记的文档块
bad = []
for fn in C_FILES:
    text, lines = read_lines(os.path.join(SRC, fn))
    for i, line in enumerate(lines):
        name = is_definition(line)
        if not name or name not in DOCS:
            continue
        j = last_block_start(lines, i)
        prev = lines[i - 1].strip() if i else ""
        blk = lines[j:i]
        if prev != "*/" or not blk or blk[0].strip() != "/**" or (MARK % name) not in "\n".join(blk):
            bad.append("%s:%d %s" % (fn, i + 1, name))
print("不变式（定义正上方紧贴唯一文档块）：", "全部满足 ✓" if not bad else bad)
