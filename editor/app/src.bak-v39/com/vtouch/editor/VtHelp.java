package com.vtouch.editor;

import java.util.ArrayList;
import java.util.List;

/**
 * 编辑器输入提示表（v37/v37b）：命令 → 作用 + 参数 + **插入模板**（按规范 §3/§4 抄录；改语言先改本表）。
 * 纯逻辑零 Android 依赖（离机核 VtToStepsCheck ⑦）。
 * 规则：光标左邻词精确=命令 → 显示；前缀命中 → 候选补全；否则回退行首词；串/注释内抑制。
 * 模板（v37b）：补全/命令键 = 插入参数骨架；首占位选中（可直接键入替换）；块级模板带缩进体与收尾括号，
 * 缩进随当前行（模板内 `\u0001` = 缩进前缀占位，仅出现在选中位之后，不影响 sel 偏移）。
 */
public final class VtHelp {

    public static final class H {
        public final String name;   // 词
        public final String what;   // 作用（一句话）
        public final String args;   // 参数 / 语法形态
        public final String tpl;    // 插入模板（\u0001 = 行缩进前缀）
        public final int selA, selB; // 模板内选中区间（-1 = 无选中，光标落尾）
        H(String n, String w, String a, String t, int sa, int sb) {
            name = n; what = w; args = a; tpl = t; selA = sa; selB = sb;
        }
    }

    /** 表序 = 候选补全展示序。 */
    private static final H[] TAB = {
        // ---- 语句（§3）----
        new H("点按", "点击屏幕坐标", "点按 x y [ms]（默认 50）", "点按 100 200 50", 3, 6),
        new H("滑动", "从一点滑到另一点", "滑动 x1 y1 x2 y2 [时长= 停1= 停2=]（时长默认 300）", "滑动 100 200 300 400", 3, 6),
        new H("等待", "暂停指定毫秒", "等待 ms", "等待 100", 3, 6),
        new H("按下", "按下指针（不抬起）", "按下 x y", "按下 100 200", 3, 6),
        new H("弹起", "抬起指针", "弹起", "弹起", -1, -1),
        new H("如果", "条件为真时执行块", "如果 条件 { … } [否则 { … }]", "如果 真 {\n\u0001    \n\u0001}", 3, 4),
        new H("否则", "条件为假时执行块", "否则 { … }（紧配 如果）", "否则 {\n\u0001    \n\u0001}", -1, -1),
        new H("循环", "重复执行块", "循环 3 / 次数=3 / 时长=ms / 无限 / i 从 1 到 10", "循环 3 {\n\u0001    \n\u0001}", 3, 4),
        new H("当", "条件真时反复执行（先判）", "当 条件 { … }", "当 真 {\n\u0001    \n\u0001}", 2, 3),
        new H("直到", "条件真时停（先判）", "直到 条件 { … }", "直到 真 {\n\u0001    \n\u0001}", 3, 4),
        new H("跳出", "跳出循环", "跳出 [层数]（默认 1，≤8）", "跳出", -1, -1),
        new H("继续", "跳过本轮", "继续 [层数]（默认 1）", "继续", -1, -1),
        new H("终止", "结束操作", "终止 / 终止 中止", "终止", -1, -1),
        new H("返回", "返回函数结果", "返回 [表达式]（仅函数内）", "返回", -1, -1),
        new H("函数", "定义函数（仅顶层）", "函数 名(参数,…) { … }", "函数 名() {\n\u0001    \n\u0001}", 3, 4),
        new H("表", "定义表（起跑清零）", "表 名[64]（1..4096 项）", "表 名[64]", 2, 3),
        new H("无限", "无限循环模式词", "循环 无限 { … }", "无限", -1, -1),
        new H("跳转", "跳到第 N 行（兼容）", "跳转 第N行 / 跳转 结束", "跳转 结束", -1, -1),
        new H("结束", "跳转收尾标记", "跳转 结束", "结束", -1, -1),
        new H("提示", "屏幕提示文字（{名} 插值）", "提示 表达式 [时长=ms]（默认 2000）", "提示 \"\"", 3, 3),
        new H("框区域", "高亮某个区域", "框区域 \"区域id\" [时长=ms]（默认 3000）", "框区域 \"\"", 4, 4),
        new H("框矩形", "高亮矩形范围", "框矩形 x1 y1 x2 y2 [时长=ms]（默认 3000）", "框矩形 100 200 300 400", 4, 7),
        // ---- 视觉/判定（§4）----
        new H("找图", "找图，命中→1 并写 fx/fy", "找图(\"模板\"[, 区域= 阈值=8 色校= 持续=])", "找图(\"\")", 3, 3),
        new H("找色", "多点找色，命中→1", "找色(\"点集\"[, 区域=…])", "找色(\"\")", 3, 3),
        new H("找色点", "单点找色，命中→1", "找色点(0xC81E1E[, 容差=8, 区域=…])", "找色点(0xC81E1E)", 4, 12),
        new H("在区域", "点在某区域内→1", "在区域(\"区域id\", x, y)", "在区域(\"\", 100, 200)", 4, 4),
        new H("开关开", "开关为开→1", "开关开(\"开关id\")", "开关开(\"\")", 4, 4),
        new H("轮", "当前循环序号（0 起）", "轮（循环外引用报错）", "轮", -1, -1),
        new H("真", "真值 1", "真", "真", -1, -1),
        new H("假", "假值 0", "假", "假", -1, -1),
        // ---- 内建函数 ----
        new H("len", "字符串长度", "len(串)", "len()", 4, 4),
        new H("str", "转字符串", "str(值)", "str()", 4, 4),
        new H("num", "转数值", "num(串)", "num()", 4, 4),
        new H("rand", "随机整数（含端点）", "rand(a, b)", "rand(0, 100)", 5, 6),
        new H("floor", "向下取整", "floor(x)", "floor()", 6, 6),
        new H("ceil", "向上取整", "ceil(x)", "ceil()", 5, 5),
        new H("round", "四舍五入取整", "round(x)", "round()", 6, 6),
        new H("abs", "绝对值", "abs(x)", "abs()", 4, 4),
        new H("min", "较小值", "min(a, b)", "min(0, 0)", 4, 5),
        new H("max", "较大值", "max(a, b)", "max(0, 0)", 4, 5),
        new H("sqrt", "平方根", "sqrt(x)", "sqrt()", 5, 5),
        new H("sin", "正弦（度）", "sin(角)", "sin()", 4, 4),
        new H("cos", "余弦（度）", "cos(角)", "cos()", 4, 4),
        new H("atan2", "反正切（度）", "atan2(y, x)", "atan2(0, 0)", 6, 7),
        // ---- 预定义变量（§5）----
        new H("tdx", "触发按下点 x", "tdx", "tdx", -1, -1),
        new H("tdy", "触发按下点 y", "tdy", "tdy", -1, -1),
        new H("tux", "触发抬起点 x", "tux", "tux", -1, -1),
        new H("tuy", "触发抬起点 y", "tuy", "tuy", -1, -1),
        new H("tms", "触发时刻（毫秒）", "tms", "tms", -1, -1),
        new H("fx", "视觉命中 x（找图写）", "fx", "fx", -1, -1),
        new H("fy", "视觉命中 y（找图写）", "fy", "fy", -1, -1),
        // ---- 语境词 / 参数键（§1/§3）----
        new H("时长", "命名参数：毫秒时长", "时长=300", "时长=300", 3, 6),
        new H("停1", "命名参数：滑动起始停（ms）", "停1=0", "停1=0", 3, 4),
        new H("停2", "命名参数：滑动结束停（ms）", "停2=0", "停2=0", 3, 4),
        new H("阈值", "命名参数：找图阈值", "阈值=8", "阈值=8", 3, 4),
        new H("持续", "命名参数：持续查找（ms）", "持续=0", "持续=0", 3, 4),
        new H("色校", "命名参数：颜色校验 0|1", "色校=1", "色校=1", 3, 4),
        new H("区域", "命名参数：限定区域 id", "区域=\"r9\"", "区域=\"\"", 3, 3),
        new H("容差", "命名参数：找色点容差", "容差=8", "容差=8", 3, 4),
        new H("次数", "命名参数：循环次数", "次数=3", "次数=3", 3, 4),
        new H("从", "循环计数起点", "循环 i 从 1 到 10", "从", -1, -1),
        new H("到", "循环计数终点", "… 从 a 到 b", "到", -1, -1),
        new H("中止", "终止模式（中止而非完成）", "终止 中止", "中止", -1, -1),
    };

    private VtHelp() {}

    /** 精确查词。 */
    public static H of(String w) {
        if (w == null || w.length() == 0) return null;
        for (H h : TAB) if (h.name.equals(w)) return h;
        return null;
    }

    /** 前缀候选（不含全词本身；表序，上限 max）。 */
    public static List<H> complete(String p, int max) {
        List<H> out = new ArrayList<H>();
        if (p == null || p.length() == 0) return out;
        for (H h : TAB) {
            if (h.name.length() > p.length() && h.name.startsWith(p)) {
                out.add(h);
                if (out.size() >= max) break;
            }
        }
        return out;
    }

    /** 插入模板（已展开缩进）：text + 选中区间。 */
    public static final class Tpl {
        public String text;
        public int selA, selB;
    }

    /** 生成插入模板：`\u0001` 行前缀 → ind（当前行缩进）；无选中条目 → 光标落尾。 */
    public static Tpl template(H h, String ind) {
        Tpl t = new Tpl();
        t.text = h.tpl.replace("\u0001", ind == null ? "" : ind);
        if (h.selA >= 0 && h.selB >= h.selA) {
            t.selA = h.selA;
            t.selB = h.selB;
        } else {
            t.selA = t.selB = t.text.length();
        }
        return t;
    }

    /** 光标左邻词（词符 = 字母/数字/_/非 ASCII；其余为界）。 */
    public static String wordBefore(String text, int cur) {
        if (text == null) return "";
        if (cur < 0) cur = 0;
        if (cur > text.length()) cur = text.length();
        int i = cur;
        while (i > 0 && isWordChar(text.charAt(i - 1))) i--;
        return text.substring(i, cur);
    }

    /** 光标所在行（不含换行）。 */
    public static String lineOf(String text, int cur) {
        if (text == null) return "";
        if (cur < 0) cur = 0;
        if (cur > text.length()) cur = text.length();
        int a = text.lastIndexOf('\n', cur - 1) + 1;
        int b = text.indexOf('\n', cur);
        if (b < 0) b = text.length();
        if (a > b) a = b;
        return text.substring(a, b);
    }

    /** 光标所在行的缩进（前导空白）。 */
    public static String lineIndent(String text, int cur) {
        String l = lineOf(text, cur);
        int i = 0;
        while (i < l.length() && (l.charAt(i) == ' ' || l.charAt(i) == '\t')) i++;
        return l.substring(0, i);
    }

    /** 行首词（跳缩进；`#`/`"` 开头 → 空）。 */
    public static String headWord(String line) {
        if (line == null) return "";
        int i = 0;
        while (i < line.length() && (line.charAt(i) == ' ' || line.charAt(i) == '\t')) i++;
        if (i < line.length() && (line.charAt(i) == '#' || line.charAt(i) == '"')) return "";
        int st = i;
        while (i < line.length() && isWordChar(line.charAt(i))) i++;
        return line.substring(st, i);
    }

    /** 光标处提示：exact = 作用+参数；cands = 前缀候选；全无 → null。串/注释内抑制。 */
    public static final class Hint {
        public H exact;
        public List<H> cands;
    }

    public static Hint hint(String text, int cur) {
        if (text == null) return null;
        if (cur < 0) cur = 0;
        if (cur > text.length()) cur = text.length();
        int ls = text.lastIndexOf('\n', cur - 1) + 1;
        boolean inStr = false;
        for (int i = ls; i < cur; i++) {
            char ch = text.charAt(i);
            if (inStr && ch == '\\') { i++; continue; }
            if (ch == '"') inStr = !inStr;
            else if (ch == '#' && !inStr) return null;   // 注释内（含 #模式 行）
        }
        if (inStr) return null;                          // 串内
        String w = wordBefore(text, cur);
        boolean boundary = cur >= text.length() || !isWordChar(text.charAt(cur));
        if (w.length() > 0) {
            if (boundary) {
                H e = of(w);
                if (e != null) { Hint h = new Hint(); h.exact = e; return h; }
            }
            List<H> cs = complete(w, 8);
            if (!cs.isEmpty()) { Hint h = new Hint(); h.cands = cs; return h; }
        }
        String hw = headWord(lineOf(text, cur));
        if (hw.length() > 0) {
            H e = of(hw);
            if (e != null) { Hint h = new Hint(); h.exact = e; return h; }
        }
        return null;
    }

    private static boolean isWordChar(char ch) {
        return ch == '_' || Character.isLetterOrDigit(ch) || ch >= 128;
    }
}
