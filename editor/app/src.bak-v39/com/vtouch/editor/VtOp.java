package com.vtouch.editor;

/** VTBC v1 操作码（T2.2）。定义见 docs/VTBC_V1.md §3。纯 Java、零 Android 依赖。 */
public final class VtOp {
    private VtOp() { }

    /* 常量 / 变量 / 表 */
    public static final int KN = 0x01, KS = 0x02, LV = 0x03, SV = 0x04, LT = 0x05, ST = 0x06;
    /* 算术 / 逻辑 */
    public static final int ADD = 0x10, SUB = 0x11, MUL = 0x12, DIV = 0x13, MOD = 0x14,
            NEG = 0x15, NOT = 0x16, CMP = 0x17;
    /* 栈 */
    public static final int POP = 0x21;
    /* 分支 / 调用 */
    public static final int JMP = 0x30, JF = 0x31, JT = 0x32, CJMP = 0x33;
    public static final int CALL = 0x40, RET = 0x41, RETE = 0x42;
    /* 动作 / 展示 / 视觉 */
    public static final int TAP = 0x50, SWIPE = 0x51, WAIT = 0x52, DOWN = 0x53, UP = 0x54,
            TOAST = 0x55, HL = 0x56, BOX = 0x57;
    public static final int FINDIMG = 0x58, FINDCOLOR = 0x59, FINDDOT = 0x5A,
            INREGION = 0x5B, SWON = 0x5C, TERM = 0x5D;
    /* 时刻 */
    public static final int NOW = 0x60;
    /* 内建 */
    public static final int LEN = 0x70, STRN = 0x71, NUM = 0x72, RAND = 0x73, FLOOR = 0x74,
            CEIL = 0x75, ROUND = 0x76, ABS = 0x77, MIN = 0x78, MAX = 0x79, SQRT = 0x7A,
            SIN = 0x7B, COS = 0x7C, ATAN2 = 0x7D;

    /* CMP 子码 */
    public static final int C_EQ = 0, C_NE = 1, C_LT = 2, C_GT = 3, C_LE = 4, C_GE = 5;
}
