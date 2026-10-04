/* vt_expr.h —— v5「计算」步骤的表达式引擎接口（校验 / 求值；v10 起操作数 = 变量）。
 *
 * 零核心依赖：只依赖标准头（<stddef.h> 的 size_t），宿主 gcc 可直接编译 —— 宿主单测口径见
 * build/test_vt_expr.c（不入库）。实现（src/vt_expr.c）整体在 #ifdef VT_UI 内：默认（无面板）
 * 构建里编成空 TU。本头不加 VT_UI 守卫（纯声明；谁需要谁包含）。
 *
 * 语法与语义逐字照 docs/OPS_PLAN_V5.md §2（递归下降；+ - * /；atan2/sin/cos/abs/min/max/sqrt
 * （三角函数按度、atan2(0,0)=0）；小数；空白忽略；全小写精确匹配；长度 ≤ 63、括号嵌套 ≤ 8、
 * token ≤ 128；除零/域错/非有限/参数个数不符 → 表达式错）。
 * v10 变量空间（spec EDITOR_V2 §Task 7.5）：预置 tdx/tdy/tux/tuy/tms（触发数据）+ fx/fy
 * （最近一次找图/找色命中坐标）+ 自定义命名变量（名字表随调用传入；[A-Za-z_][A-Za-z0-9_]*、
 * 1..15）。引用未写的触发数据 → 变量无值；引用未写的 fx/fy / 自定义变量 → 结果无值。
 */
#ifndef VT_EXPR_H
#define VT_EXPR_H

#include <stddef.h>

/* vt_expr_eval 返回码（vt_expr_check 只回 0/非 0）。 */
#define VT_EXPR_OK        0
#define VT_EXPR_NO_VAR   (-1)   /* 变量无值（触发数据 mask 缺位） */
#define VT_EXPR_NO_SLOT  (-2)   /* 结果无值（fx/fy / 自定义变量未写） */
#define VT_EXPR_BAD      (-3)   /* 表达式错（词法/语法/除零/域错/非有限/超限/未知名字） */

/* 求值变量快照（调用方组好；引擎只读）：触发数据 + fx/fy + 自定义命名变量。 */
struct vt_expr_env {
    const int *trig_vals;      /* tdx,tdy,tux,tuy,tms（可 NULL = 全无值） */
    unsigned   trig_mask;      /* 位 0..4 同序（1 = 有值） */
    double     fx, fy;         /* 最近一次找图/找色命中坐标（res_mask 位 0/1 指示已写） */
    unsigned   res_mask;       /* 位 0 = fx 已写、位 1 = fy 已写 */
    const char (*names)[16];   /* 自定义变量名表（≤16 条；空名 = 空槽；可 NULL = 无自定义） */
    const double *vals;        /* 自定义变量值（与 names 同下标） */
    unsigned   var_mask;       /* 位 0..15 = 已写 */
    int        nnames;         /* names 条数（0..16） */
};

/* 校验表达式：0 = 合法；非 0 = 非法，why 填短中文原因（供面板显示/核心拒收日志）。
 * names/nnames = 自定义变量名表（名字不在表里且不是预置名/函数 → `未知名字`）。 (vtouch-doc: vt_expr_check) */
int vt_expr_check(const char *s, const char (*names)[16], int nnames, char *why, size_t whycap);
/* 求值：env = 变量快照（触发数据 / fx/fy / 自定义变量）。out 收 double（仅返回 OK 时有意义；出错不写）。
 * 返回 VT_EXPR_OK / NO_VAR / NO_SLOT / BAD。 (vtouch-doc: vt_expr_eval) */
int vt_expr_eval(const char *s, const struct vt_expr_env *env, double *out);

#endif /* VT_EXPR_H */
