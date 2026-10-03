/* vt_expr.h —— v5「计算」步骤的表达式引擎接口（校验 / 求值）。
 *
 * 零核心依赖：只依赖标准头（<stddef.h> 的 size_t），宿主 gcc 可直接编译 —— 宿主单测口径见
 * build/test_vt_expr.c（不入库）。实现（src/vt_expr.c）整体在 #ifdef VT_UI 内：默认（无面板）
 * 构建里编成空 TU。本头不加 VT_UI 守卫（纯声明；谁需要谁包含）。
 *
 * 语法与语义逐字照 docs/OPS_PLAN_V5.md §2：递归下降；+ - * /；atan2/sin/cos/abs/min/max/sqrt
 * （三角函数按度、atan2(0,0)=0）；小数；空白忽略；全小写精确匹配；长度 ≤ 63、括号嵌套 ≤ 8、
 * token ≤ 128；除零/域错/非有限/参数个数不符 → 表达式错。
 */
#ifndef VT_EXPR_H
#define VT_EXPR_H

#include <stddef.h>

/* vt_expr_eval 返回码（vt_expr_check 只回 0/非 0）。 */
#define VT_EXPR_OK        0
#define VT_EXPR_NO_VAR   (-1)   /* 变量无值（触发数据 mask 缺位） */
#define VT_EXPR_NO_SLOT  (-2)   /* 结果无值（槽未写） */
#define VT_EXPR_BAD      (-3)   /* 表达式错（词法/语法/除零/域错/非有限/超限） */

/* 校验表达式：0 = 合法；非 0 = 非法，why 填短中文原因（供面板显示/核心拒收日志）。 (vtouch-doc: vt_expr_check) */
int vt_expr_check(const char *s, char *why, size_t whycap);
/* 求值：trig_vals[0..4] = tdx,tdy,tux,tuy,tms；trig_mask 位 0..4 同序（1=有值）。
 * slots[0..3] = r1..r4；slot_mask 位 0..3（1=已写）。out 收 double（仅返回 OK 时有意义；出错不写）。
 * 返回 VT_EXPR_OK / NO_VAR / NO_SLOT / BAD。 (vtouch-doc: vt_expr_eval) */
int vt_expr_eval(const char *s, const int trig_vals[5], unsigned trig_mask,
                 const double slots[4], unsigned slot_mask, double *out);

#endif /* VT_EXPR_H */
