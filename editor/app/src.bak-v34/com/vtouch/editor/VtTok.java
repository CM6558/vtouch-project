package com.vtouch.editor;

import java.util.List;

/** vt-script 词法 token（T1.1）。规范：docs/VT_SCRIPT_V1_SPEC.md §2（修订 r2）。
 *  纯 Java、零 Android 依赖（离机核 ~/vtchk-host/VtScriptCheck）。
 *  r2：选项键（次数/时长/…）与 `从/到` 降为**语境词**——词法层 = 普通 IDENT，
 *  解析层按 `键=值` 键位 / `循环 i 从 a 到 b` 位置识别（见 OPT_KEYS）。 */
public final class VtTok {
    /* token 类别 */
    public static final int EOF = 0, SEP = 1, NUM = 2, STR = 3, IDENT = 4, KW = 5, OP = 6;

    /* 关键字子号（= KW_NAMES 下标+1；解析器直接引用这些常量） */
    public static final int K_DIANAN = 1, K_HUADONG = 2, K_DENGDAI = 3, K_ANXIA = 4, K_TANQI = 5,
            K_RUGUO = 6, K_FOUZE = 7, K_XUNHUAN = 8, K_DANG = 9, K_ZHIDAO = 10,
            K_TIAOCHU = 11, K_JIXU = 12, K_ZHONGZHI = 13, K_FANHUI = 14, K_HANSHU = 15,
            K_BIAO = 16, K_WUXIAN = 17, K_TIAOZHUAN = 18, K_JIESHU = 19,
            K_TISHI = 20, K_KUANGQUYU = 21, K_KUANGJUXING = 22,
            K_ZHAOTU = 23, K_ZHAOSE = 24, K_ZHAOSEDIAN = 25, K_ZAIQUYU = 26, K_KAIGUANKAI = 27,
            K_LUN = 28, K_ZHEN = 29, K_JIA = 30,
            K_LEN = 31, K_STR = 32, K_NUM = 33, K_RAND = 34, K_FLOOR = 35, K_CEIL = 36, K_ROUND = 37,
            K_ABS = 38, K_MIN = 39, K_MAX = 40, K_SQRT = 41, K_SIN = 42, K_COS = 43, K_ATAN2 = 44;

    /** 保留字全表（44 词；顺序 = 子号-1；与规范 §2 修订 r2 逐字一致）。 */
    public static final String[] KW_NAMES = {
            "点按", "滑动", "等待", "按下", "弹起",
            "如果", "否则", "循环", "当", "直到",
            "跳出", "继续", "终止", "返回", "函数",
            "表", "无限", "跳转", "结束",
            "提示", "框区域", "框矩形",
            "找图", "找色", "找色点", "在区域", "开关开",
            "轮", "真", "假",
            "len", "str", "num", "rand", "floor", "ceil", "round", "abs", "min", "max", "sqrt", "sin", "cos", "atan2"
    };

    /** 语境词：命名参数键（解析层在 `键=值` 键位识别；词法层 = IDENT，可作普通名字）。
     *  另：循环计数词 `从` / `到` 同为语境词（仅 `循环 i 从 a 到 b` 里成对出现时有语法义）。 */
    public static final String[] OPT_KEYS = {
            "时长", "停1", "停2", "阈值", "持续", "色校", "区域", "模式", "容差", "次数"
    };

    public int kind;
    public String text = "";       // KW/OP/IDENT/NUM 原文；SEP = "\n" 或 ";"
    public int line, col;          // 1 起
    public double num;             // NUM 值
    public int kw;                 // KW 子号（K_*；0 = 无）
    public String raw;             // STR：引号内原文（未解码）
    public List<Object> segs;      // STR：String 字面段 / Interp 插值段

    /** STR 插值段：{名} 或 {名[下标]}（下标 = 数字或变量名）。 */
    public static final class Interp {
        public String name;
        public boolean hasIdx;
        public boolean idxNum;
        public double idx;
        public String idxName;
        public int line, col;
    }

    public static int kwOf(String s) {
        for (int i = 0; i < KW_NAMES.length; i++) if (KW_NAMES[i].equals(s)) return i + 1;
        return 0;
    }

    public static boolean isOptKey(String s) {
        for (int i = 0; i < OPT_KEYS.length; i++) if (OPT_KEYS[i].equals(s)) return true;
        return false;
    }

    public boolean isKw(int k) { return kind == KW && kw == k; }
    public boolean isOp(String o) { return kind == OP && text.equals(o); }

    /** 诊断签名（测试断言用）：K:点按 / I:x / N:100 / S:"…" / O:+ / SEP / EOF。 */
    public String sig() {
        switch (kind) {
            case SEP: return "SEP";
            case EOF: return "EOF";
            case KW: return "K:" + text;
            case IDENT: return "I:" + text;
            case NUM: return "N:" + text;
            case STR: return "S:\"" + raw + "\"";
            default: return "O:" + text;
        }
    }
}
