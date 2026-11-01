package com.vtouch.editor;

import java.io.File;
import java.io.FileInputStream;
import java.util.ArrayList;
import java.util.List;

/**
 * VtToSteps（script→steps 投影器，V8 T6.1）离机核：
 * ① 往返金样——VtFromSteps 全部金样用例反向过一遍（translate → project → translate 逐字一致）；
 * ② 脚本先正例/规范化（缺省补显、显式默认折叠、德摩根展开、#模式 侧线）；③ 负样例（不可投影原因）；
 * ④ 字段级抽查（pack/取反/槽位序）；⑤ 真实产物扫描（out-mig / out-mig-zt 存在时全量往返 + [格式化] 恒等）；
 * ⑥ 存取/侧线/元数据行/引用改写/格式化（ScriptStore，v36 扩）；⑦ 帮助表（VtHelp，v37：覆盖/光标词/候选/抑制）。
 * 运行：java -cp . com.vtouch.editor.VtToStepsCheck（run.sh 已接入）。
 */
public final class VtToStepsCheck {

    private static int pass = 0, fail = 0;

    public static void main(String[] args) {
        /* ================= ① 往返金样（steps → script → project → 再翻译，逐字一致） ================= */

        rt("往返·基础步型",
           op(st(1, 540, 1584, 0, 0, 50, "", 0, 0, ""),
              st(3, 0, 0, 0, 0, 500, "", 0, 0, ""),
              st(2, 100, 200, 300, 400, 300, "", 30, 10, ""),
              st(4, 5, 6, 0, 0, 0, "", 0, 0, ""),
              st(5, 0, 0, 0, 0, 0, "", 0, 0, ""),
              st(1, -1, -2, 0, 0, -5, "", 0, 0, "")),
           "点按 540 1584 50\n" +
           "等待 500\n" +
           "滑动 100 200 300 400 时长=300 停1=30 停2=10\n" +
           "按下 5 6\n" +
           "弹起\n" +
           "点按 tdx tdy tms\n");

        rt("往返·计算步（表达式规范）",
           op(st(9, 0, 0, 0, 0, 0, "花费", 0, 0, "tdx+1"),
              st(9, 1, 0, 0, 0, 0, "e1", 0, 0, "tdx + cos(atan2(tuy-tdy, tux-tdx)) *200")),
           "花费 = tdx + 1\n" +
           "e1 = tdx + cos(atan2(tuy - tdy, tux - tdx)) * 200\n");

        rt("往返·四档继续/继续（空块）",
           op(c10(2, 2, 0, 0), tap()),
           "如果 找图(\"tm\") {\n}\n点按 1 2 50\n");
        rt("往返·四档继续/跳过（吸收）",
           op(c10(2, 1, 0, 0), tap()),
           "如果 找图(\"tm\") {\n    点按 1 2 50\n}\n");
        rt("往返·四档跳过/继续（!c 吸收）",
           op(c10(1, 2, 0, 0), tap()),
           "如果 !找图(\"tm\") {\n    点按 1 2 50\n}\n");
        rt("往返·四档跳过/跳过（X 死）",
           op(c10(1, 1, 0, 0), tap()),
           "如果 找图(\"tm\") {\n}\n");
        rt("往返·四档中止/继续",
           op(c10(0, 2, 0, 0), tap()),
           "如果 !找图(\"tm\") {\n    点按 1 2 50\n} 否则 {\n    终止 中止\n}\n");
        rt("往返·四档继续/中止",
           op(c10(2, 0, 0, 0), tap()),
           "如果 找图(\"tm\") {\n    点按 1 2 50\n} 否则 {\n    终止 中止\n}\n");
        rt("往返·四档中止/中止",
           op(c10(0, 0, 0, 0), tap()),
           "如果 找图(\"tm\") {\n    终止 中止\n} 否则 {\n    终止 中止\n}\n");
        rt("往返·四档跳转0=终止",
           op(c10(3, 2, 0, 0), tap()),
           "如果 !找图(\"tm\") {\n    点按 1 2 50\n} 否则 {\n    终止\n}\n");
        rt("往返·四档跳转 i+2 归一",
           op(c10(3, 2, 3, 0), tap(), st(3, 0, 0, 0, 0, 200, "", 0, 0, "")),
           "如果 !找图(\"tm\") {\n    点按 1 2 50\n}\n等待 200\n");
        rt("往返·四档吸收递归（跳过链嵌套）",
           op(c10(2, 1, 0, 0), st(6, 5, 6, 1, 2, 0, "r", 0, 0, ""), st(3, 0, 0, 0, 0, 200, "", 0, 0, ""),
              st(3, 0, 0, 0, 0, 300, "", 0, 0, "")),
           "如果 找图(\"tm\") {\n" +
           "    如果 在区域(\"r\", 5, 6) {\n" +
           "        等待 200\n" +
           "    }\n" +
           "}\n" +
           "等待 300\n");

        rt("往返·循环三模式",
           op(st(12, 0, 3, 0, 0, 0, "", 0, 0, ""), st(3, 0, 0, 0, 0, 100, "", 0, 0, ""), st(13, 0, 0, 0, 0, 0, "", 0, 0, ""),
              st(12, 1, 2000, 0, 0, 0, "", 0, 0, ""), st(13, 0, 0, 0, 0, 0, "", 0, 0, ""),
              st(12, 2, 0, 0, 0, 0, "", 0, 0, ""), st(13, 0, 0, 0, 0, 0, "", 0, 0, "")),
           "循环 3 {\n    等待 100\n}\n循环 时长=2000 {\n}\n循环 无限 {\n}\n");

        rt("往返·到条件（直到 + 取反）",
           op(st(12, 3, 1, 0, 0, 0, "s1", 0, 0, ""), st(3, 0, 0, 0, 0, 100, "", 0, 0, ""), st(13, 0, 0, 0, 0, 0, "", 0, 0, ""),
              st(12, 3, 0, 5, 6, 0, "r", 0, 1, ""), st(13, 0, 0, 0, 0, 0, "", 0, 0, "")),
           "直到 开关开(\"s1\") {\n    等待 100\n}\n直到 !在区域(\"r\", 5, 6) {\n}\n");

        rt("往返·子条件 DNF（且/或/取反）",
           op(st(14, 1, 0, 0, 0, 0, "s1", 0, 0, ""),
              st(19, 0, 1, 0, 0, 0, "s2", 0, 1, ""),
              st(19, 1, 1, 0, 0, 0, "s3", 0, 0, ""),
              st(3, 0, 0, 0, 0, 100, "", 0, 0, ""),
              st(16, 0, 0, 0, 0, 0, "", 0, 0, "")),
           "如果 开关开(\"s1\") && !开关开(\"s2\") || 开关开(\"s3\") {\n    等待 100\n}\n");

        rt("往返·变量比较（jt4 取反）",
           op(st(9, 0, 0, 0, 0, 0, "花费", 0, 0, "tdx+1"),
              st(14, 1, 0, 0, 0, 0, "s1", 0, 0, ""),
              st(19, 0, 4, -10, 100, 0, "", 0, 1, ""),
              st(15, 0, 0, 0, 0, 0, "", 0, 0, ""),
              st(3, 0, 0, 0, 0, 200, "", 0, 0, ""),
              st(16, 0, 0, 0, 0, 0, "", 0, 0, "")),
           "花费 = tdx + 1\n如果 开关开(\"s1\") && !(花费 > 100) {\n} 否则 {\n    等待 200\n}\n");

        rt("往返·跳出/继续/终止（层数）",
           op(st(12, 1, 5000, 0, 0, 0, "", 0, 0, ""),
              st(12, 0, 3, 0, 0, 0, "", 0, 0, ""),
              st(17, 0, 0, 0, 0, 0, "", 0, 0, ""),
              st(18, 2, 0, 0, 0, 0, "", 0, 0, ""),
              st(20, 1, 0, 0, 0, 0, "", 0, 0, ""),
              st(13, 0, 0, 0, 0, 0, "", 0, 0, ""),
              st(13, 0, 0, 0, 0, 0, "", 0, 0, ""),
              st(20, 0, 0, 0, 0, 0, "", 0, 0, "")),
           "循环 时长=5000 {\n    循环 3 {\n        跳出\n        继续 2\n        终止 中止\n    }\n}\n终止\n");

        rt("往返·提示/框区域/框矩形",
           op(st(21, 0, 0, 0, 0, 2000, "", 0, 0, "找到名刀{fx}"),
              st(21, 0, 0, 0, 0, 0, "", 0, 0, "结算 } 好 {{ 字面"),
              st(22, 0, 0, 0, 0, 500, "r2", 0, 0, ""),
              st(23, 100, 200, 300, 400, 0, "", 0, 0, "")),
           "提示 \"找到名刀{fx}\" 时长=2000\n" +
           "提示 \"结算 }} 好 {{ 字面\"\n" +
           "框区域 \"r2\" 时长=500\n" +
           "框矩形 100 200 300 400\n");

        rt("往返·找色点/找色（pack）",
           op(st(11, 0, (0xC81E1E << 8) | 5, 2, 2, 0, "", 0, 0, ""),
              st(11, 1, 0, 2, 2, 0, "pts", 0, 0, ""),
              st(14, 3, 0, 0, 0, 0, "", (0xC81E1E << 8) | 8, 0, ""),
              st(16, 0, 0, 0, 0, 0, "", 0, 0, "")),
           "如果 找色点(0xC81E1E, 容差=5) {\n}\n如果 找色(\"pts\") {\n}\n如果 找色点(0xC81E1E) {\n}\n");

        rt("往返·跳转块内展开",
           op(st(12, 2, 0, 0, 0, 0, "", 0, 0, ""),
              st(8, 1, 0, 0, 0, 0, "", 0, 0, ""),
              st(13, 0, 0, 0, 0, 0, "", 0, 0, ""),
              st(12, 0, 3, 0, 0, 0, "", 0, 0, ""),
              st(8, 7, 0, 0, 0, 0, "", 0, 0, ""),
              st(13, 0, 0, 0, 0, 0, "", 0, 0, ""),
              st(3, 0, 0, 0, 0, 100, "", 0, 0, "")),
           "循环 无限 {\n    继续\n}\n循环 3 {\n    跳出\n}\n等待 100\n");

        rt("往返·真实 op1（13 步全链）",
           op(st(14, 0, -3, -4, 0, 0, "c1", 0, 0, ""),
              st(15, 0, 0, 0, 0, 0, "", 0, 0, ""),
              st(12, 1, 2000, 0, 0, 0, "", 0, 0, ""),
              st(14, 3, 1, 0, 0, 0, "lb1", 0, 0, ""),
              st(9, 0, 0, 0, 0, 0, "e1", 0, 0, "tdx + cos(atan2(tuy-tdy, tux-tdx)) *200"),
              st(9, 1, 0, 0, 0, 0, "e2", 0, 0, "tdy + sin(atan2(tuy-tdy, tux-tdx)) *200"),
              st(2, -1, -2, -10, -11, 100, "", 30, 0, ""),
              st(1, 154, 1978, 0, 0, 50, "", 0, 0, ""),
              st(17, 0, 0, 0, 0, 0, "", 0, 0, ""),
              st(16, 0, 0, 0, 0, 0, "", 0, 0, ""),
              st(3, 0, 0, 0, 0, 50, "", 0, 0, ""),
              st(13, 0, 0, 0, 0, 0, "", 0, 0, ""),
              st(16, 0, 0, 0, 0, 0, "", 0, 0, "")),
           "如果 在区域(\"c1\", tux, tuy) {\n" +
           "} 否则 {\n" +
           "    循环 时长=2000 {\n" +
           "        如果 找色(\"lb1\") {\n" +
           "            e1 = tdx + cos(atan2(tuy - tdy, tux - tdx)) * 200\n" +
           "            e2 = tdy + sin(atan2(tuy - tdy, tux - tdx)) * 200\n" +
           "            滑动 tdx tdy e1 e2 时长=100 停1=30\n" +
           "            点按 154 1978 50\n" +
           "            跳出\n" +
           "        }\n" +
           "        等待 50\n" +
           "    }\n" +
           "}\n");

        /* ================= ② 脚本先正例 / 规范化 ================= */
        {
            String op1 = "如果 在区域(\"c1\", tux, tuy) {\n" +
                "} 否则 {\n" +
                "    循环 时长=2000 {\n" +
                "        如果 找色(\"lb1\") {\n" +
                "            e1 = tdx + cos(atan2(tuy - tdy, tux - tdx)) * 200\n" +
                "            e2 = tdy + sin(atan2(tuy - tdy, tux - tdx)) * 200\n" +
                "            滑动 tdx tdy e1 e2 时长=100 停1=30\n" +
                "            点按 154 1978 50\n" +
                "            跳出\n" +
                "        }\n" +
                "        等待 50\n" +
                "    }\n" +
                "}\n";
            posx("脚本先·真实 op1 逐字稳定", op1);
        }
        norm("规范化·点按缺省补显", "点按 100 200\n", "点按 100 200 50\n");
        norm("规范化·滑动时长默认 300", "滑动 100 200 300 400\n", "滑动 100 200 300 400 时长=300\n");
        norm("规范化·找图显式默认折叠", "如果 找图(\"tm\", 阈值=8) {\n}\n", "如果 找图(\"tm\") {\n}\n");
        norm("规范化·德摩根展开", "如果 !(开关开(\"s1\") && 开关开(\"s2\")) {\n}\n",
             "如果 !开关开(\"s1\") || !开关开(\"s2\") {\n}\n");
        posx("脚本先·组合式规范形态",
             "如果 开关开(\"s1\") && !开关开(\"s2\") || 开关开(\"s3\") {\n    等待 100\n}\n");
        posx("脚本先·直到规范形态", "直到 开关开(\"s1\") {\n    等待 100\n}\n");
        {
            VtToSteps.R r = VtToSteps.project("#模式 脚本\n等待 100\n");
            if (r.err == null && "等待 100\n".equals(r.src) && r.exact) ok("脚本先·#模式 侧线放行");
            else bad0("脚本先·#模式 侧线放行", "err=" + r.err + " src=" + esc(r.src) + " exact=" + r.exact);
        }

        /* ================= ③ 负样例 ================= */
        neg("负·跳转（兼容语法）", "等待 100\n跳转 第3行\n等待 200\n等待 300\n", "跳转");
        neg("负·函数/返回", "函数 f() {\n    返回 1\n}\n", "函数");
        neg("负·表定义", "表 t[8]\n", "表定义");
        neg("负·当循环", "当 开关开(\"s1\") {\n}\n", "「当」");
        neg("负·for 计数循环", "循环 i 从 1 到 3 {\n}\n", "暂不支持块视图");
        neg("负·用户注释", "# 说明\n等待 100\n", "含注释");
        neg("负·负数字面量", "点按 -5 10 50\n", "负数字面量");
        neg("负·小数", "点按 1.5 10 50\n", "小数");
        neg("负·复合字段", "等待 tdx + 1\n", "字段超出块视图支持");
        neg("负·表达式%（超出 v5 域）", "a = 5 % 2\n", "表达式超出块视图支持");
        neg("负·字符串表达式", "a = \"x\"\n", "表达式超出块视图支持");
        neg("负·颜色越界", "如果 找色点(0xFFFFFFF) {\n}\n", "颜色越界");
        neg("负·变量名超长", "一二三四五六七八九十甲乙丙丁戊己 = 1\n", "变量名超长");
        neg("负·空脚本", "", "没有任何步骤");
        neg("负·循环变量计数", "循环 tdx {\n}\n", "该字段不支持变量");
        neg("负·提示插值非变量", "循环 3 {\n    提示 \"第 {轮} 轮\"\n}\n", "提示占位变量无效");
        neg("负·子条件超过 4 条",
            "如果 开关开(\"s1\") && 开关开(\"s2\") && 开关开(\"s3\") && 开关开(\"s4\") && 开关开(\"s5\") {\n}\n",
            "组合条件超过");
        {
            StringBuilder b = new StringBuilder();
            for (int i = 0; i < 17; i++) b.append("v").append(i).append(" = 1\n");
            neg("负·变量数超过 16", b.toString(), "变量数超过");
        }
        {
            StringBuilder b = new StringBuilder();
            for (int i = 0; i < 9; i++) b.append("如果 开关开(\"s1\") {\n");
            b.append("    等待 1\n");
            for (int i = 0; i < 9; i++) b.append("}\n");
            neg("负·嵌套超过 8 层", b.toString(), "嵌套超过");
        }

        /* ================= ④ 字段级抽查 ================= */
        {
            VtToSteps.R r = VtToSteps.project("如果 找色点(0xC81E1E, 容差=5) {\n}\n");
            if (r.err == null && r.op.steps.size() == 2
                    && r.op.steps.get(0).type == 14 && r.op.steps.get(0).a1 == 3
                    && r.op.steps.get(0).j1 == ((0xC81E1E << 8) | 5)) ok("抽查·找色点 pack=j1");
            else bad0("抽查·找色点 pack=j1", r.err != null ? r.err : "字段不符");
        }
        {
            VtToSteps.R r = VtToSteps.project("直到 开关开(\"s1\") {\n    等待 100\n}\n");
            Ops.Step h = r.op == null ? null : r.op.steps.get(0);
            if (r.err == null && h != null && h.type == 12 && h.a1 == 3 && h.a2 == 1 && "s1".equals(h.ref) && h.j2 == 0)
                ok("抽查·到条件头字段");
            else bad0("抽查·到条件头字段", r.err != null ? r.err : "字段不符");
        }
        {
            VtToSteps.R r = VtToSteps.project("a = 1\n如果 !(a > 100) {\n}\n");
            Ops.Step h = r.op == null ? null : r.op.steps.get(1);
            if (r.err == null && h != null && h.type == 14 && h.a1 == 4 && h.a2 == -10 && h.a3 == 100 && h.a4 == 1)
                ok("抽查·变量比较头（jt4/取反/编码）");
            else bad0("抽查·变量比较头（jt4/取反/编码）", r.err != null ? r.err : "字段不符");
        }
        {
            String op1 = "如果 在区域(\"c1\", tux, tuy) {\n} 否则 {\n    循环 时长=2000 {\n        如果 找色(\"lb1\") {\n" +
                "            e1 = tdx + 1\n            e2 = tdy + 1\n            滑动 tdx tdy e1 e2 时长=100 停1=30\n" +
                "            点按 154 1978 50\n            跳出\n        }\n        等待 50\n    }\n}\n";
            VtToSteps.R r = VtToSteps.project(op1);
            Ops.Step c1 = r.op == null ? null : r.op.steps.get(4);
            Ops.Step sw = r.op == null ? null : r.op.steps.get(6);
            if (r.err == null && c1 != null && c1.a1 == 0 && "e1".equals(c1.ref)
                    && sw != null && sw.a3 == -10 && sw.a4 == -11)
                ok("抽查·槽位序（e1=slot0/e2=slot1/-10/-11）");
            else bad0("抽查·槽位序（e1=slot0/e2=slot1/-10/-11）", r.err != null ? r.err : "字段不符");
        }
        {
            VtToSteps.R r = VtToSteps.project("提示 \"找到名刀{fx}\" 时长=2000\n");
            Ops.Step t = r.op == null ? null : r.op.steps.get(0);
            if (r.err == null && t != null && t.type == 21 && "找到名刀{fx}".equals(t.expr) && t.ms == 2000)
                ok("抽查·提示模板与时长");
            else bad0("抽查·提示模板与时长", r.err == null ? "字段不符" : r.err);
        }

        /* ================= ⑥ 存取/侧线/元数据行/引用改写（ScriptStore） ================= */
        {
            String w = ScriptStore.withPragma("等待 100\n", "块");
            if ("#模式 块\n等待 100\n".equals(w) && "块".equals(ScriptStore.modeOf(w))) ok("存取·侧线写入/读取（块）");
            else bad0("存取·侧线写入/读取（块）", esc(w));
            String w2 = ScriptStore.withPragma(w, "脚本");
            if ("#模式 脚本\n等待 100\n".equals(w2) && "脚本".equals(ScriptStore.modeOf(w2))) ok("存取·侧线切换（脚本）");
            else bad0("存取·侧线切换（脚本）", esc(w2));
            if (ScriptStore.modeOf("等待 100\n") == null) ok("存取·侧线缺省为空");
            else bad0("存取·侧线缺省为空", "非 null");
        }
        {
            ScriptStore.Save s = ScriptStore.prepare("等待 100\n", "脚本");
            if (s.err == null && s.bytes != null && s.bytes.length > 0
                    && "#模式 脚本\n等待 100\n".equals(s.src)) ok("存取·prepare 编译（含侧线）");
            else bad0("存取·prepare 编译（含侧线）", s.err != null ? s.err : "字段不符");
            if (s.err == null) {
                OpsV8.Rec r = new OpsV8.Rec();
                r.name = "op1"; r.gate = "-"; r.autooff = 0;
                r.hash = OpsV8.fnv8(s.bytes); r.size = s.bytes.length;
                if (OpsV8.validRec(r) == null && OpsV8.checkArtifact(r, s.bytes) == null) ok("存取·产物校验自洽（validRec/checkArtifact）");
                else bad0("存取·产物校验自洽（validRec/checkArtifact）", "校验不过");
            } else {
                bad0("存取·产物校验自洽（validRec/checkArtifact）", "前项已败");
            }
            ScriptStore.Save e1 = ScriptStore.prepare("循环 {\n", "脚本");
            if (e1.err != null && e1.line > 0) ok("存取·prepare 错误行号");
            else bad0("存取·prepare 错误行号", "err=" + e1.err + " line=" + e1.line);
            ScriptStore.Save e2 = ScriptStore.prepare("", "块");
            if (e2.err != null && e2.err.contains("没有任何语句")) ok("存取·空脚本拒绝");
            else bad0("存取·空脚本拒绝", "err=" + e2.err);
        }
        {
            String conf = "#vtouch-ops v8\n" +
                "op op1 gate r1 autooff 1 prog 4de7db61 500\n" +
                "op op2 gate - autooff 0 prog aabbccdd 700\n";
            OpsV8.Rec r = new OpsV8.Rec();
            r.name = "op1"; r.gate = "r1"; r.autooff = 1; r.hash = "11112222"; r.size = 612;
            String out = ScriptStore.updateMetaLine(conf, r);
            String expect = "#vtouch-ops v8\n" +
                "op op1 gate r1 autooff 1 prog 11112222 612\n" +
                "op op2 gate - autooff 0 prog aabbccdd 700\n";
            if (expect.equals(out)) ok("存取·元数据行级替换（其余逐字节）");
            else bad0("存取·元数据行级替换（其余逐字节）", esc(out));
            r.name = "op3"; r.gate = "-"; r.autooff = 0; r.hash = "33334444"; r.size = 100; r.extra = "xx yy";
            String out2 = ScriptStore.updateMetaLine(conf, r);
            if (out2.endsWith("op op3 gate - autooff 0 prog 33334444 100 xx yy\n") && out2.startsWith(conf)) ok("存取·元数据行追加（extra 保留）");
            else bad0("存取·元数据行追加（extra 保留）", esc(out2));
        }
        {
            String op1 = "如果 在区域(\"c1\", tux, tuy) {\n} 否则 {\n    循环 时长=2000 {\n        如果 找色(\"lb1\") {\n" +
                "            e1 = tdx + 1\n            等待 50\n        }\n        等待 50\n    }\n}\n";
            java.util.List<String> refs = ScriptStore.regionRefs(op1);
            if (refs.contains("c1") && !refs.contains("lb1")) ok("引用·区域扫描（c1 收 / 点集名不收）");
            else bad0("引用·区域扫描（c1 收 / 点集名不收）", "refs=" + refs);
            java.util.Map<String, String> ren = new java.util.HashMap<String, String>();
            ren.put("c1", "zz1");
            String mapped = ScriptStore.remapRegions(op1, ren);
            if (mapped != null && mapped.contains("在区域(\"zz1\"") && !mapped.contains("\"c1\"")) ok("引用·改名改写（规范重排）");
            else bad0("引用·改名改写（规范重排）", esc(mapped));
        }
        {
            /* v36：格式化深核（规范重排 + 注释归位 + 幂等 + 侧线 + 报行 + 扫描） */
            String messy = "等待 100\n  如果 开关开(\"s1\") {\n点按 1 2\n# 说明\n}\n";
            ScriptStore.Fmt f1 = ScriptStore.format(messy);
            String want1 = "等待 100\n如果 开关开(\"s1\") {\n    点按 1 2\n    # 说明\n}\n";
            if (f1.err == null && want1.equals(f1.src)) ok("格式化·规范重排 + 注释随语句");
            else bad0("格式化·规范重排 + 注释随语句", f1.err != null ? f1.err : esc(f1.src));
            ScriptStore.Fmt f2 = ScriptStore.format(f1.src == null ? "" : f1.src);
            if (f2.err == null && f1.src != null && f1.src.equals(f2.src)) ok("格式化·幂等");
            else bad0("格式化·幂等", f2.err != null ? f2.err : esc(f2.src));
            ScriptStore.Fmt f3 = ScriptStore.format("循环 3 {\n等待 1\n# 尾\n}\n# 外\n等待 2\n");
            String want3 = "循环 3 {\n    等待 1\n    # 尾\n}\n# 外\n等待 2\n";
            if (f3.err == null && want3.equals(f3.src)) ok("格式化·块尾注释归位（尾内/外分界）");
            else bad0("格式化·块尾注释归位（尾内/外分界）", f3.err != null ? f3.err : esc(f3.src));
            ScriptStore.Fmt f4 = ScriptStore.format("#模式 脚本\n点按 9 9\n");
            if (f4.err == null && "#模式 脚本\n点按 9 9\n".equals(f4.src)) ok("格式化·侧线保留（纯排版不改参）");
            else bad0("格式化·侧线保留（纯排版不改参）", f4.err != null ? f4.err : esc(f4.src));
            ScriptStore.Fmt f5 = ScriptStore.format("循环 {\n");
            if (f5.err != null && f5.line == 1) ok("格式化·语法错报行");
            else bad0("格式化·语法错报行", "err=" + f5.err + " line=" + f5.line);
            List<VtPrint.Cmt> cs = ScriptStore.comments("等待 1\n\"# 串里\"\n# 真注\n");
            if (cs.size() == 1 && cs.get(0).line == 3 && cs.get(0).text.trim().equals("# 真注")
                    && ScriptStore.comments("点按 1 1 50 # 行尾\n").size() == 1) ok("格式化·注释扫描（串内不算/行尾算）");
            else bad0("格式化·注释扫描（串内不算/行尾算）", "cs=" + cs.size());
        }
        {
            /* v37：帮助表核（覆盖 + 光标词/前缀候选/行首回退/串注释抑制 + 补全） */
            int kmiss = 0;
            for (String w : VtTok.KW_NAMES) if (VtHelp.of(w) == null) kmiss++;
            int vmiss = 0;
            String[] pv = { "tdx", "tdy", "tux", "tuy", "tms", "fx", "fy" };
            for (String w : pv) if (VtHelp.of(w) == null) vmiss++;
            if (kmiss == 0 && vmiss == 0) ok("帮助·覆盖（保留字 44 + 预定义 7 全有）");
            else bad0("帮助·覆盖（保留字 44 + 预定义 7 全有）", "kw缺=" + kmiss + " 变量缺=" + vmiss);
            VtHelp.Hint h1 = VtHelp.hint("等待 100\n", 2);
            if (h1 != null && h1.exact != null && h1.exact.name.equals("等待") && h1.cands == null) ok("帮助·光标词精确（等待）");
            else bad0("帮助·光标词精确（等待）", h1 == null ? "null" : String.valueOf(h1.exact));
            VtHelp.Hint h2 = VtHelp.hint("找", 1);
            if (h2 != null && h2.exact == null && h2.cands != null && h2.cands.size() == 3) ok("帮助·前缀候选（找→找图/找色/找色点）");
            else bad0("帮助·前缀候选（找→找图/找色/找色点）", h2 == null || h2.cands == null ? "null" : "cands=" + h2.cands.size());
            VtHelp.Hint h3 = VtHelp.hint("提示 \"等待\n", 6);
            if (h3 == null) ok("帮助·串内抑制");
            else bad0("帮助·串内抑制", "非 null");
            VtHelp.Hint h4 = VtHelp.hint("点按 1 2 # 等待\n", 11);
            if (h4 == null) ok("帮助·注释内抑制");
            else bad0("帮助·注释内抑制", "非 null");
            VtHelp.Hint h5 = VtHelp.hint("  等待 100\n", 8);
            if (h5 != null && h5.exact != null && h5.exact.name.equals("等待")) ok("帮助·行首回退（光标在参数上）");
            else bad0("帮助·行首回退（光标在参数上）", h5 == null ? "null" : String.valueOf(h5.exact));
            VtHelp.Hint h6 = VtHelp.hint("#模式 脚本\n", 4);
            if (h6 == null) ok("帮助·侧线行抑制");
            else bad0("帮助·侧线行抑制", "非 null");
            VtHelp.Hint h7 = VtHelp.hint("圆", 1);
            if (h7 == null || h7.cands == null || h7.cands.isEmpty()) ok("帮助·无候选回落（圆→无）");
            else bad0("帮助·无候选回落（圆→无）", "cands=" + h7.cands.size());
        }

        /* ================= ⑤ 真实产物扫描（out-mig / out-mig-zt） ================= */
        scanReal("out-mig");
        scanReal("out-mig-zt");

        System.out.println("VtToStepsCheck: 通过 " + pass + " · 失败 " + fail);
        if (fail > 0) System.exit(1);
    }

    /* ================= 工具 ================= */

    private static final String R5 = "/data/data/com.termux/files/home/vtouch-shots/v8t5";

    /** 已知负例夹具（历次验收拒收矩阵的产物）：预期=投影仍拒（若哪天能投影了反而是告警）。 */
    private static final String[] NEG_FIXTURES = { "ztc3.vts", "ztc12.vts", "ztd4.vts", "zte2.vts" };

    private static void scanReal(String dir) {
        File root = new File(R5, dir);
        if (!root.isDirectory()) { System.out.println("[SKIP] 真实产物扫描（" + dir + " 目录不存在）"); return; }
        List<File> fs = new ArrayList<File>();
        collect(root, fs);
        int n = 0, ng = 0, bad = 0;
        for (File f : fs) {
            String content = readAll(f);
            if (content == null) continue;
            VtToSteps.R r = VtToSteps.project(content);
            boolean neg = false;
            for (String x : NEG_FIXTURES) if (x.equals(f.getName())) neg = true;
            if (neg) {
                if (r.err != null) { ng++; continue; }
                bad0("真实·负例夹具 " + f.getName() + "（" + dir + "）", "预期拒绝但通过了投影"); bad++;
                continue;
            }
            if (r.err != null) { bad0("真实·" + f.getName() + "（" + dir + "）", "投影拒：" + r.err); bad++; continue; }
            if (!r.exact) { bad0("真实·" + f.getName() + "（" + dir + "）", "非规范往返（exact=false）"); bad++; continue; }
            if (!content.equals(r.src)) { bad0("真实·" + f.getName() + "（" + dir + "）", "往返不一致"); bad++; continue; }
            ScriptStore.Fmt ff = ScriptStore.format(content);      // v36：规范产物 [格式化] 恒等
            if (ff.err != null || !content.equals(ff.src)) {
                bad0("真实·" + f.getName() + "（" + dir + "）", "格式化非恒等" + (ff.err != null ? "：" + ff.err : ""));
                bad++;
                continue;
            }
            n++;
        }
        if (bad == 0) ok("真实产物往返（" + dir + " · 正例 " + n + " 件全绿 + 负例 " + ng + " 件仍拒）");
    }

    private static void collect(File d, List<File> out) {
        File[] cs = d.listFiles();
        if (cs == null) return;
        for (File f : cs) {
            if (f.isDirectory()) collect(f, out);
            else if (f.getName().endsWith(".vts")) out.add(f);
        }
    }

    private static String readAll(File f) {
        try {
            FileInputStream in = new FileInputStream(f);
            byte[] b = new byte[(int) f.length()];
            int off = 0;
            while (off < b.length) {
                int k = in.read(b, off, b.length - off);
                if (k < 0) break;
                off += k;
            }
            in.close();
            return new String(b, 0, off, "UTF-8");
        } catch (Exception e) {
            return null;
        }
    }

    private static String esc(String s) { return s == null ? "null" : s.replace("\n", "\\n"); }

    /** 往返金样：steps → script → project → 再翻译；三段一致 + 金样逐字。 */
    private static void rt(String name, Ops.Op o, String expect) {
        VtFromSteps.R r1 = VtFromSteps.translate(o);
        if (r1.src == null) { bad0(name, "翻译失败 " + r1.problems); return; }
        if (expect != null && !expect.equals(r1.src)) { bad0(name, "金样漂移\n--- 期望 ---\n" + expect + "--- 实际 ---\n" + r1.src); return; }
        VtToSteps.R p = VtToSteps.project(r1.src);
        if (p.err != null) { bad0(name, "投影拒：" + p.err); return; }
        if (!p.exact) { bad0(name, "exact=false（金样应逐字稳定）"); return; }
        if (!r1.src.equals(p.src)) { bad0(name, "往返不一致\n--- 金样 ---\n" + r1.src + "--- 重翻译 ---\n" + p.src); return; }
        ok(name);
    }

    /** 脚本先·逐字稳定（exact=true 且 src==输入）。 */
    private static void posx(String name, String script) {
        VtToSteps.R r = VtToSteps.project(script);
        if (r.err != null) { bad0(name, "投影拒：" + r.err); return; }
        if (!r.exact) { bad0(name, "exact=false"); return; }
        if (!script.equals(r.src)) { bad0(name, "src 不一致：" + esc(r.src)); return; }
        ok(name);
    }

    /** 规范化：可投影 + 输出等于规范化形态（允许 exact=false）。 */
    private static void norm(String name, String script, String expectSrc) {
        VtToSteps.R r = VtToSteps.project(script);
        if (r.err != null) { bad0(name, "投影拒：" + r.err); return; }
        if (!expectSrc.equals(r.src)) { bad0(name, "输出不一致\n--- 期望 ---\n" + expectSrc + "--- 实际 ---\n" + esc(r.src)); return; }
        ok(name);
    }

    private static void neg(String name, String script, String expectSubstr) {
        VtToSteps.R r = VtToSteps.project(script);
        if (r.err == null) { bad0(name, "期望拒绝但通过"); return; }
        if (r.err.contains(expectSubstr)) { ok(name); return; }
        bad0(name, "err 未含「" + expectSubstr + "」：" + r.err);
    }

    private static Ops.Step st(int type, int a1, int a2, int a3, int a4, int ms, String ref, int j1, int j2, String expr) {
        Ops.Step s = new Ops.Step();
        s.type = type; s.a1 = a1; s.a2 = a2; s.a3 = a3; s.a4 = a4; s.ms = ms;
        s.ref = ref; s.j1 = j1; s.j2 = j2; s.expr = expr;
        return s;
    }

    private static Ops.Step c10(int a4, int a3, int j1, int j2) {
        return st(10, 8, 0, a3, a4, 0, "tm", j1, j2, "");
    }

    private static Ops.Step tap() { return st(1, 1, 2, 0, 0, 50, "", 0, 0, ""); }

    private static Ops.Op op(Ops.Step... ss) {
        Ops.Op o = new Ops.Op();
        o.name = "op1";
        o.gate = "-";
        o.autooff = 0;
        for (Ops.Step s : ss) o.steps.add(s);
        return o;
    }

    private static void ok(String name) { pass++; System.out.println("[OK] " + name); }

    private static void bad0(String name, String why) {
        fail++;
        System.out.println("[FAIL] " + name + " —— " + why);
    }
}
