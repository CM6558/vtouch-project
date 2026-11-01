#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""v13 方案数据迁移：旧式「守卫 / 持续查找」改写为块结构（对照 vtouch 契约 v13）。

step 行格式（v4/v5）：step <type> <a1> <a2> <a3> <a4> <ms> <ref> <j1> <j2> <expr>   （11 词）
  split 后索引： [0]=step [1]=type [2]=a1 [3]=a2 [4]=a3 [5]=a4 [6]=ms [7]=ref [8]=j1 [9]=j2 [10]=expr

规则（用户已拍板，2026-10-09）：
  R1 守卫：op 首个 step 为区域判断（type 6）且 不成立=继续(2)、成立=中止(0)、无跳转 → 改写为
     条件块【如果（同判定）〔空〕否则〔其余步骤〕】＝ 首位替换为 14/15、末尾追加 16。
  R2 持续查找：视觉步（10/11）ms>0 → 改写为
     循环·时长 ms【如果（同查找 命中）{原后续步骤 + 17 跳出}16 等待50】13（超时未命中 = 无动作，等效中止）。
  R3 格式升级：文件头 v3/v4 → v5；v3 行（10 词）补 expr 列 `-`。
  R4 体检（不改，只报告）：op 内用 -6/-7（fx/fy）但其前（含改写后）无视觉步 → 运行必挂警告。

用法：
  python3 migrate_scheme_v13.py <file.conf>            # 干跑：输出 .new + 报告
  python3 migrate_scheme_v13.py --apply <file.conf>    # 写入（先备份 .bak-migrate-<ts>）
"""

import sys, os, shutil, datetime

S_LOOP, S_ENDLOOP, S_IF, S_ELSE, S_ENDIF, S_BREAK, S_CONT = 12, 13, 14, 15, 16, 17, 18
STRUCT = ("12", "13", "14", "15", "16", "17", "18")


def mk_step(type_, a1=0, a2=0, a3=0, a4=0, ms=0, ref="-", j1=0, j2=0, expr="-"):
    return ("step %s %s %s %s %s %s %s %s %s %s"
            % (type_, a1, a2, a3, a4, ms, ref, j1, j2, expr)).split(" ")


def is_guard(p):
    """旧式守卫：type6、不成立=继续(2，a3)、成立=中止(0，a4)、ms=0、无跳转（j1/j2=0）。"""
    return (len(p) >= 10 and p[1] == "6" and p[4] == "2" and p[5] == "0"
            and p[6] == "0" and p[8] == "0" and p[9] == "0")


def migrate(text, label, report):
    lines = [l.rstrip("\n") for l in text.split("\n")]
    if not lines or not lines[0].startswith("#vtouch-ops"):
        return None, "无版本头"
    ver = int(lines[0].split("v")[-1])
    out = ["#vtouch-ops v5"]
    if ver < 5:
        report.append("  [R3] 文件头 v%d → v5" % ver)
    i = 1
    while i < len(lines):
        l = lines[i].strip()
        if not l or l.startswith("#"):
            i += 1
            continue
        p = l.split()
        if p[0] == "op":
            steps = []
            j = i + 1
            while j < len(lines):
                q = lines[j].strip()
                if not q or q.startswith("#"):
                    j += 1
                    continue
                if q.split()[0] == "op":
                    break
                if q.split()[0] == "step":
                    sp = q.split()
                    if len(sp) == 10:                       # v3 行（无 expr 列）补 `-`
                        sp = sp + ["-"]
                    steps.append(sp)
                j += 1
            opname = p[1] if len(p) > 1 else "?"
            # ---- R4 体检（基于改写前的原结构判断 fx/fy 来源） ----
            has_vis = any(s[1] == "10" or s[1] == "11" for s in steps)
            uses_fx = any(("-6" in s[2:10] or "-7" in s[2:10])
                          for s in steps if s[1] not in STRUCT)
            if uses_fx and not has_vis:
                report.append("  [警告] op %s：用到 fx/fy（-6/-7）但全 op 无找图/找色步 → "
                              "运行时该步会「结果无值」中止" % opname)
                # ---- R5 修复：滑到 fx/fy 无来源 → 滑到抬起点（-3/-4；用户拍板 2026-10-09） ----
                fixed = 0
                for s in steps:
                    if s[1] == "2":                         # 滑动：a3/a4 = 终点 x/y
                        if s[4] == "-6":
                            s[4] = "-3"; fixed += 1
                        if s[5] == "-7":
                            s[5] = "-4"; fixed += 1
                if fixed:
                    report.append("  [R5] op %s：滑到 fx/fy（无来源）→ 改为滑到抬起点（-3/-4），共 %d 处" % (opname, fixed))
            # ---- R2 持续查找（从后往前替换全部；先做，R1 封尾在后） ----
            hits = [k for k, s in enumerate(steps)
                    if s[1] in ("10", "11") and s[6].isdigit() and int(s[6]) > 0]
            for k in sorted(hits, reverse=True):
                s = steps[k]
                ms = int(s[6])
                expr = s[10] if len(s) > 10 else "-"
                if s[1] == "11":                            # 找色：模式 a1；多点 pck=0、单点 pck=打包(a2)
                    jt, px, py = 3, s[2], 0
                    pck = s[3] if s[2] == "0" else 0
                else:                                       # 找图：阈值 a1 / 色校 a2
                    jt, px, py, pck = 2, s[2], s[3], 0
                cond = mk_step(S_IF, jt, px, py, 0, 0, s[7], pck, 0, expr)
                seg = ([mk_step(S_LOOP, 1, ms), cond] + steps[k + 1:]
                       + [mk_step(S_BREAK), mk_step(S_ENDIF), mk_step(3, 0, 0, 0, 0, 50),
                          mk_step(S_ENDLOOP)])
                steps = steps[:k] + seg
                report.append("  [R2] op %s：持续查找 %dms → 循环·时长%dms{ 如果(命中){原后续; 跳出} "
                              "等待50 }（超时=无动作，等效中止）" % (opname, ms, ms))
            # ---- R1 守卫（放最后：其 16 才能落到整体末尾） ----
            if steps and is_guard(steps[0]):
                g = steps[0]
                report.append("  [R1] op %s：守卫（区域判断 %s，成立中止/不成立继续）→ "
                              "条件块【如果〔空〕否则〔%d 步〕结束】" % (opname, g[7], len(steps) - 1))
                steps = ([mk_step(S_IF, 0, g[2], g[3], 0, 0, g[7], 0, 0, "-"),
                          mk_step(S_ELSE)] + steps[1:] + [mk_step(S_ENDIF)])
            out.append(" ".join(p))
            for s in steps:
                out.append(" ".join(s))
            i = j
        else:
            i += 1
    return "\n".join(out) + "\n", None


def main():
    args = sys.argv[1:]
    apply_ = "--apply" in args
    for fn in [a for a in args if a != "--apply"]:
        with open(fn, encoding="utf-8") as f:
            text = f.read()
        report = []
        new, err = migrate(text, fn, report)
        print("===== %s" % fn)
        for r in report:
            print(r)
        if err:
            print("  [跳过] %s" % err)
            continue
        if not report:
            print("  [无改动]（没有匹配的旧式写法）")
        if new == text:
            continue
        if apply_:
            ts = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
            shutil.copy2(fn, fn + ".bak-migrate-" + ts)
            with open(fn, "w", encoding="utf-8") as f:
                f.write(new)
            print("  [已写入]（备份 %s.bak-migrate-%s）" % (fn, ts))
        else:
            with open(fn + ".new", "w", encoding="utf-8") as f:
                f.write(new)
            print("  [干跑] 新内容 → %s.new" % fn)


if __name__ == "__main__":
    main()
