/* vt_expr.c —— v5「计算」步骤的表达式引擎（vt_expr_check / vt_expr_eval）。
 *
 * 语法与语义逐字照 docs/OPS_PLAN_V5.md §2（递归下降解析 + 现解析现求值）：
 *   expr := term { ("+"|"-") term }；term := unary { ("*"|"/") unary }；unary := ["-"] unary | primary；
 *   primary := number | var | slot | func "(" args ")" | "(" expr ")"；
 *   var = tdx/tdy/tux/tuy/tms；slot = r1..r4；func = atan2/sin/cos/abs/min/max/sqrt；
 *   number = 十进制整数 [ "." 十进制小数 ]（必须数字开头：`.5`、`1.` 都非法）；空白忽略；全小写精确匹配。
 * 语义：+ - * / 为 double 运算（除零 → 错）；atan2(y,x) 返回**度**、范围 (-180,180]、`(0,0)` = 0；
 *   sin/cos 参数为度；abs 绝对值；min/max 两参数；sqrt(x<0) → 错；参数个数不符 → 错；
 *   一元负号允许（含 `--x`，不做特判）；结果必须为**有限数**（inf/NaN → 错）。
 * 边界：长度 ≤ 63 字符；括号嵌套 ≤ 8 层；token 数 ≤ 128（词法口径：数字/标识符/运算符/括号/逗号各算 1 个）。
 *   token 上限**先于长度检查**执行 —— 63 字符内 token 最多 63 个、到不了 128，所以该上限实际只对
 *   >63 字符的输入生效（防御式兜底）；这样三条边界都「可触发、各有 why」。
 * 错误口径：vt_expr_check 只解析 + **静态常量折叠**（变量/槽视为未知值并传播）——
 *   「除零 / 负数开方 / 结果非有限」只在**完全由常量决定**时被编辑期拦下（不会误拒 `1/tdx` 这类）；
 *   vt_expr_eval 用真实值求值，运行时同一套判定兜底（spec §5「编辑期已拦，这里是防御」）。
 *
 * 守卫：整文件在 #ifdef VT_UI 内 —— 默认（无面板）构建里本文件是空 TU（build.sh 用通配把 src 下的 .c 一起链）。
 * 零核心依赖：只 include 标准头 + 自家头，宿主 gcc 可直接编译（宿主单测见 build/test_vt_expr.c，不入库）。
 */
#ifdef VT_UI

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "vt_expr.h"

#define VT_EXPR_LEN_MAX   63    /* 表达式长度上限（字符；spec §2） */
#define VT_EXPR_DEPTH_MAX 8     /* 括号嵌套上限（spec §2） */
#define VT_EXPR_TOK_MAX   128   /* token 数上限（spec §2；计数口径见文件头） */
#define VT_EXPR_PI 3.14159265358979323846   /* 自备 π：M_PI 在宿主 / NDK 的可见性开关不一致 */

/* 内部错误码（对外映射见两个入口函数）。 */
#define XE_BAD    1     /* 表达式错（词法/语法/除零/域错/非有限/超限） */
#define XE_NOVAR  2     /* 变量无值 */
#define XE_NOSLOT 3     /* 结果无值 */

/* token 种类。 */
enum {
    TOK_END = 0, TOK_NUM, TOK_NAME, TOK_PLUS, TOK_MINUS, TOK_STAR, TOK_SLASH, TOK_LP, TOK_RP, TOK_COMMA
};

/* 名字类别（白名单；全小写精确匹配 —— `TDX` 也走「未知名字」）。 */
enum { NAME_UNKNOWN = 0, NAME_VAR, NAME_SLOT, NAME_FUNC };

/* 函数下标（识别为 NAME_FUNC 后的 idx）。 */
enum { XF_ATAN2 = 0, XF_SIN, XF_COS, XF_ABS, XF_MIN, XF_MAX, XF_SQRT };

/* 解析 / 求值上下文（一次调用一份，栈上；check 与 eval 共用）。 */
struct ex_ctx {
    const char *p;        /* 词法游标（下一个待读字符） */
    int tok;              /* 当前 token（TOK_*） */
    double num;           /* TOK_NUM 的值 */
    const char *name;     /* TOK_NAME 的起点（非 NUL 结尾，配 namelen 用） */
    int namelen;
    int depth;            /* 当前括号嵌套深度（解析期） */
    int tcount;           /* 已产出 token 数（词法上限） */
    int mode_eval;        /* 1 = 真实求值（eval）；0 = 静态（check：变量/槽 = 未知值） */
    int known;            /* 当前子表达式的值是否已知（静态模式的未知传播） */
    double v;             /* 当前子表达式值（known 为真时有效） */
    int err;              /* 0 = 未错；XE_* */
    char *why;            /* 原因缓冲（check 用；eval 传 NULL） */
    size_t whycap;
    const int *trig_vals; /* eval：触发数据（可 NULL = 全无值） */
    unsigned trig_mask;
    const double *slots;  /* eval：结果槽（可 NULL = 全未写） */
    unsigned slot_mask;
};

/* ---- 词法与上下文 ---- */

/* 记下**第一个**错误（后到的不覆盖先到的）：err 置码、why 填短中文（why 可 NULL / 0 容）。 */
static void ex_fail(struct ex_ctx *c, int code, const char *msg)
{
    size_t n;

    if (c->err) return;
    c->err = code;
    if (!c->why || !c->whycap) return;
    n = strlen(msg);
    if (n > c->whycap - 1) n = c->whycap - 1;
    memcpy(c->why, msg, n);
    c->why[n] = '\0';
}

/* 词法：跳空白（空格/Tab/CR/LF）读出一个 token；空白忽略、全小写精确匹配（spec §2）。
 * 数字必须数字开头（`.5` 非法）；`1.` / `1.2.3` 里的裸点 → `非法数字`；token 超 128 → `表达式过长`。
 * 出错时 err 置位、tok 置 END（各调用点统一靠 err 判断，不再看 tok）。 */
static void ex_tok(struct ex_ctx *c)
{
    const char *p = c->p;

    c->num = 0.0;
    c->name = NULL;
    c->namelen = 0;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;

    if (*p == '\0') { c->p = p; c->tok = TOK_END; return; }

    if (*p >= '0' && *p <= '9') {
        double v = 0.0;
        while (*p >= '0' && *p <= '9') { v = v * 10.0 + (double)(*p - '0'); p++; }
        if (*p == '.') {
            double scale;
            p++;
            if (!(*p >= '0' && *p <= '9')) { c->p = p; ex_fail(c, XE_BAD, "非法数字"); c->tok = TOK_END; return; }
            scale = 0.1;
            while (*p >= '0' && *p <= '9') { v += (double)(*p - '0') * scale; scale *= 0.1; p++; }
        }
        c->p = p;
        c->num = v;
        c->tok = TOK_NUM;
    } else if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || *p == '_') {
        const char *s = p;
        while ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_') p++;
        c->p = p;
        c->name = s;
        c->namelen = (int)(p - s);
        c->tok = TOK_NAME;
    } else if (*p == '+') { c->p = p + 1; c->tok = TOK_PLUS; }
    else if (*p == '-') { c->p = p + 1; c->tok = TOK_MINUS; }
    else if (*p == '*') { c->p = p + 1; c->tok = TOK_STAR; }
    else if (*p == '/') { c->p = p + 1; c->tok = TOK_SLASH; }
    else if (*p == '(') { c->p = p + 1; c->tok = TOK_LP; }
    else if (*p == ')') { c->p = p + 1; c->tok = TOK_RP; }
    else if (*p == ',') { c->p = p + 1; c->tok = TOK_COMMA; }
    else if (*p == '.') { c->p = p + 1; ex_fail(c, XE_BAD, "非法数字"); c->tok = TOK_END; return; }
    else { c->p = p + 1; ex_fail(c, XE_BAD, "非法字符"); c->tok = TOK_END; return; }

    /* token 上限（先于长度检查的口径见文件头）：超限即停，后面的字符不再看。 */
    if (++c->tcount > VT_EXPR_TOK_MAX) { ex_fail(c, XE_BAD, "表达式过长"); c->tok = TOK_END; }
}

/* 标识符白名单：精确匹配（长度 + 逐字节）。命中出类别与下标；否则 NAME_UNKNOWN。 */
static int ex_name_kind(const char *s, int n, int *idx)
{
    if (n == 3 && !memcmp(s, "tdx", 3)) { *idx = 0; return NAME_VAR; }
    if (n == 3 && !memcmp(s, "tdy", 3)) { *idx = 1; return NAME_VAR; }
    if (n == 3 && !memcmp(s, "tux", 3)) { *idx = 2; return NAME_VAR; }
    if (n == 3 && !memcmp(s, "tuy", 3)) { *idx = 3; return NAME_VAR; }
    if (n == 3 && !memcmp(s, "tms", 3)) { *idx = 4; return NAME_VAR; }
    if (n == 2 && !memcmp(s, "r1", 2)) { *idx = 0; return NAME_SLOT; }
    if (n == 2 && !memcmp(s, "r2", 2)) { *idx = 1; return NAME_SLOT; }
    if (n == 2 && !memcmp(s, "r3", 2)) { *idx = 2; return NAME_SLOT; }
    if (n == 2 && !memcmp(s, "r4", 2)) { *idx = 3; return NAME_SLOT; }
    if (n == 5 && !memcmp(s, "atan2", 5)) { *idx = XF_ATAN2; return NAME_FUNC; }
    if (n == 3 && !memcmp(s, "sin", 3)) { *idx = XF_SIN; return NAME_FUNC; }
    if (n == 3 && !memcmp(s, "cos", 3)) { *idx = XF_COS; return NAME_FUNC; }
    if (n == 3 && !memcmp(s, "abs", 3)) { *idx = XF_ABS; return NAME_FUNC; }
    if (n == 3 && !memcmp(s, "min", 3)) { *idx = XF_MIN; return NAME_FUNC; }
    if (n == 3 && !memcmp(s, "max", 3)) { *idx = XF_MAX; return NAME_FUNC; }
    if (n == 4 && !memcmp(s, "sqrt", 4)) { *idx = XF_SQRT; return NAME_FUNC; }
    return NAME_UNKNOWN;
}

/* ---- 递归下降 ---- */

static void ex_expr(struct ex_ctx *c);
static void ex_unary(struct ex_ctx *c);

/* 消费一个 '('：深度 +1 并查上限（超 8 层 → `嵌套过深`）；失败返回 0。 */
static int ex_open(struct ex_ctx *c)
{
    c->depth++;
    if (c->depth > VT_EXPR_DEPTH_MAX) { ex_fail(c, XE_BAD, "嵌套过深"); return 0; }
    ex_tok(c);
    return !c->err;
}

/* 期望当前是 ')'：是则消费（深度 -1）；否则 `括号不配对`。 */
static int ex_close(struct ex_ctx *c)
{
    if (c->err) return 0;
    if (c->tok != TOK_RP) { ex_fail(c, XE_BAD, "括号不配对"); return 0; }
    c->depth--;
    ex_tok(c);
    return !c->err;
}

/* 函数调用（名字已识别，f 为 XF_*）：`func "(" args ")"` —— 解析期收参数（最多存前 2 个）、
 * 收尾查 arity（atan2/min/max 须 2 个、其余须 1 个），再按函数语义算值；静态模式下只在
 * 参数全已知时算（否则标记未知），域错（负数开方）只要参数已知就先拦。 */
static void ex_call(struct ex_ctx *c, int f)
{
    double av[2];
    int ak[2];
    int nargs = 0;

    av[0] = av[1] = 0.0;
    ak[0] = ak[1] = 0;

    ex_tok(c);                                   /* 消费函数名 */
    if (c->err) return;
    if (c->tok != TOK_LP) { ex_fail(c, XE_BAD, "括号不配对"); return; }
    if (!ex_open(c)) return;

    if (c->tok != TOK_RP) {
        for (;;) {
            ex_expr(c);
            if (c->err) return;
            if (nargs < 2) { av[nargs] = c->v; ak[nargs] = c->known; }
            nargs++;
            if (c->tok != TOK_COMMA) break;
            ex_tok(c);
            if (c->err) return;
        }
    }
    if (!ex_close(c)) return;

    if ((f == XF_ATAN2 || f == XF_MIN || f == XF_MAX) ? (nargs != 2) : (nargs != 1)) {
        ex_fail(c, XE_BAD, "参数个数不对");
        return;
    }

    switch (f) {
    case XF_ATAN2:                               /* 度；范围 (-180,180]；spec：atan2(0,0) = 0 */
        c->known = ak[0] && ak[1];
        if (c->known) {
            if (av[0] == 0.0 && av[1] == 0.0) c->v = 0.0;
            else c->v = atan2(av[0], av[1]) / VT_EXPR_PI * 180.0;
        }
        break;
    case XF_SIN:
        c->known = ak[0];
        if (c->known) c->v = sin(av[0] * (VT_EXPR_PI / 180.0));
        break;
    case XF_COS:
        c->known = ak[0];
        if (c->known) c->v = cos(av[0] * (VT_EXPR_PI / 180.0));
        break;
    case XF_ABS:
        c->known = ak[0];
        if (c->known) c->v = fabs(av[0]);
        break;
    case XF_MIN:
        c->known = ak[0] && ak[1];
        if (c->known) c->v = av[0] < av[1] ? av[0] : av[1];
        break;
    case XF_MAX:
        c->known = ak[0] && ak[1];
        if (c->known) c->v = av[0] > av[1] ? av[0] : av[1];
        break;
    default:                                     /* XF_SQRT */
        if (ak[0] && av[0] < 0.0) { ex_fail(c, XE_BAD, "负数开方"); return; }
        c->known = ak[0];
        if (c->known) c->v = sqrt(av[0]);
        break;
    }
    if (c->known && !isfinite(c->v)) ex_fail(c, XE_BAD, "结果非有限");
}

/* primary：数字 / 变量 / 槽 / 函数调用 / 括号表达式；其余（含 EOF、运算符、逗号、`)`）→ `缺少操作数`。
 * 变量与槽在静态模式（check）一律记「未知」—— 运行值不可知，不能拿试算值误拒合法表达式。 */
static void ex_primary(struct ex_ctx *c)
{
    int kind, idx;

    if (c->err) return;
    switch (c->tok) {
    case TOK_NUM:                                /* 数字字面量恒有限（≤63 位十进制 ≪ DBL_MAX） */
        c->v = c->num;
        c->known = 1;
        ex_tok(c);
        return;
    case TOK_NAME:
        kind = ex_name_kind(c->name, c->namelen, &idx);
        if (kind == NAME_VAR) {
            if (c->mode_eval) {
                if (!c->trig_vals || !(c->trig_mask & (1u << idx))) { ex_fail(c, XE_NOVAR, "变量无值"); return; }
                c->v = (double)c->trig_vals[idx];
            }
            c->known = c->mode_eval;
            ex_tok(c);
            return;
        }
        if (kind == NAME_SLOT) {
            if (c->mode_eval) {
                if (!c->slots || !(c->slot_mask & (1u << idx))) { ex_fail(c, XE_NOSLOT, "结果无值"); return; }
                c->v = c->slots[idx];
                if (!isfinite(c->v)) { ex_fail(c, XE_BAD, "结果非有限"); return; }
            }
            c->known = c->mode_eval;
            ex_tok(c);
            return;
        }
        if (kind == NAME_FUNC) { ex_call(c, idx); return; }
        ex_fail(c, XE_BAD, "未知名字");
        return;
    case TOK_LP:
        if (!ex_open(c)) return;
        ex_expr(c);
        if (c->err) return;
        ex_close(c);
        return;
    default:
        ex_fail(c, XE_BAD, "缺少操作数");
        return;
    }
}

/* unary：`["-"] unary | primary` —— 递归写法，`--x` 自然合法（spec：不做特判）。 */
static void ex_unary(struct ex_ctx *c)
{
    if (c->err) return;
    if (c->tok == TOK_MINUS) {
        ex_tok(c);
        if (c->err) return;
        ex_unary(c);
        if (c->err) return;
        if (c->known) c->v = -c->v;
        return;
    }
    ex_primary(c);
}

/* term：`unary { ("*"|"/") unary }`（左结合）；`/` 的除数为已知 0 → `除零`（静态/真实同口径）。 */
static void ex_term(struct ex_ctx *c)
{
    if (c->err) return;
    ex_unary(c);
    while (!c->err && (c->tok == TOK_STAR || c->tok == TOK_SLASH)) {
        int op = c->tok;
        int lk = c->known;
        double lv = c->v;

        ex_tok(c);
        if (c->err) return;
        ex_unary(c);
        if (c->err) return;
        if (op == TOK_SLASH) {
            if (c->known && c->v == 0.0) { ex_fail(c, XE_BAD, "除零"); return; }
            if (lk && c->known) {
                c->v = lv / c->v;
                if (!isfinite(c->v)) { ex_fail(c, XE_BAD, "结果非有限"); return; }
            } else {
                c->known = 0;
            }
        } else {
            if (lk && c->known) {
                c->v = lv * c->v;
                if (!isfinite(c->v)) { ex_fail(c, XE_BAD, "结果非有限"); return; }
            } else {
                c->known = 0;
            }
        }
    }
}

/* expr：`term { ("+"|"-") term }`（左结合）。 */
static void ex_expr(struct ex_ctx *c)
{
    if (c->err) return;
    ex_term(c);
    while (!c->err && (c->tok == TOK_PLUS || c->tok == TOK_MINUS)) {
        int op = c->tok;
        int lk = c->known;
        double lv = c->v;

        ex_tok(c);
        if (c->err) return;
        ex_term(c);
        if (c->err) return;
        if (lk && c->known) {
            c->v = (op == TOK_PLUS) ? lv + c->v : lv - c->v;
            if (!isfinite(c->v)) { ex_fail(c, XE_BAD, "结果非有限"); return; }
        } else {
            c->known = 0;
        }
    }
}

/* 主流程（check / eval 共用）：空 → 词法预扫（token 上限/非法字符/非法数字）→ 长度 → 递归下降解析。
 * 静态模式（mode_eval=0）做常量折叠 + 未知传播；真实模式取触发数据/槽值求值。
 * 返回 0 或 XE_*；成功时（仅真实模式）*out 收结果。 */
static int ex_run(const char *s, int mode_eval, const int *trig_vals, unsigned trig_mask,
                  const double *slots, unsigned slot_mask, double *out, char *why, size_t whycap)
{
    struct ex_ctx c;

    memset(&c, 0, sizeof c);
    c.p = s ? s : "";
    c.mode_eval = mode_eval;
    c.why = why;
    c.whycap = whycap;
    c.trig_vals = trig_vals;
    c.trig_mask = trig_mask;
    c.slots = slots;
    c.slot_mask = slot_mask;

    if (!s || s[0] == '\0') { ex_fail(&c, XE_BAD, "表达式为空"); return c.err; }

    /* 1) 词法预扫（先于长度检查：让 token 上限对 >63 字符输入也可触发，口径见文件头）。 */
    for (;;) {
        ex_tok(&c);
        if (c.err) return c.err;
        if (c.tok == TOK_END) break;
    }
    if (c.tcount == 0) { ex_fail(&c, XE_BAD, "表达式为空"); return c.err; }
    /* 2) 长度。 */
    if (strlen(s) > VT_EXPR_LEN_MAX) { ex_fail(&c, XE_BAD, "超过 63 字符"); return c.err; }
    /* 3) 解析（游标回到串首）。 */
    c.p = s;
    c.tcount = 0;
    c.depth = 0;
    ex_tok(&c);
    if (!c.err) ex_expr(&c);
    if (!c.err && c.tok != TOK_END) ex_fail(&c, XE_BAD, c.tok == TOK_RP ? "括号不配对" : "多余字符");
    if (!c.err && out) *out = c.v;
    return c.err;
}

/**
 * (vtouch-doc: vt_expr_check)
 * @brief 校验表达式是否合法（面板编辑期与核心拒收共用的唯一实现）。
 * @param   s        表达式文本（可 NULL / 空）
 * @param   why      非法时写入短中文原因（可 NULL / 0 容）
 * @param   whycap   why 缓冲长度
 * @return  0 合法；-1 非法（why 已填原因；成功时 why 为空串）。
 * @note    语法与语义逐字照 spec §2（递归下降；+ - * /；atan2/sin/cos/abs/min/max/sqrt（三角函数按度、atan2(0,0)=0）；小数；空白忽略；全小写精确匹配）。边界：长度 ≤ 63、括号嵌套 ≤ 8、token ≤ 128（词法口径：数字/标识符/运算符/括号/逗号各 1 个；token 上限先于长度检查，三条边界都可触发、各有 why）。静态常量折叠：变量/槽视为未知值并传播 —— 「除零 / 负数开方 / 结果非有限」只在完全由常量决定时拦下（不会误拒 `1/tdx` 这类），运行期由 vt_expr_eval 同一套判定兜底。
 */
int vt_expr_check(const char *s, char *why, size_t whycap)
{
    if (why && whycap) why[0] = '\0';
    return ex_run(s, 0, NULL, 0, NULL, 0, NULL, why, whycap) ? -1 : 0;
}

/**
 * (vtouch-doc: vt_expr_eval)
 * @brief 求值表达式：触发数据 / 结果槽 / 字面量参与运算，返回 double。
 * @param   s        表达式文本
 * @param   trig_vals 触发数据 [tdx,tdy,tux,tuy,tms]（可 NULL = 全无值）
 * @param   trig_mask 位 0..4 同序，1=有值
 * @param   slots    结果槽 [r1..r4]（可 NULL = 全未写）
 * @param   slot_mask 位 0..3，1=已写
 * @param   out      输出（仅返回 OK 时有意义；出错不写）
 * @return  VT_EXPR_OK；VT_EXPR_NO_VAR（变量无值）/ VT_EXPR_NO_SLOT（结果无值）/ VT_EXPR_BAD（表达式错）。
 * @note    与 check 同一套解析与判定：语法错、除零、负数开方、结果非有限（inf/NaN）、超限都返回 BAD；引用 mask 缺位的触发数据 / 未写的槽返回 NO_VAR / NO_SLOT（按求值顺序，先遇到先报）。
 */
int vt_expr_eval(const char *s, const int trig_vals[5], unsigned trig_mask,
                 const double slots[4], unsigned slot_mask, double *out)
{
    int rc = ex_run(s, 1, trig_vals, trig_mask, slots, slot_mask, out, NULL, 0);

    if (rc == 0) return VT_EXPR_OK;
    if (rc == XE_NOVAR) return VT_EXPR_NO_VAR;
    if (rc == XE_NOSLOT) return VT_EXPR_NO_SLOT;
    return VT_EXPR_BAD;
}

#endif /* VT_UI */
