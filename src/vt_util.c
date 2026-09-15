/* vt_util.c（§2 小工具） —— 模块地图见 vt_internal.h；私有状态就近放 static，共享状态走 g。 */
#include "vt_internal.h"
/**
 * (vtouch-doc: parse_long)
 * @brief 把字符串解析成 [lo, hi] 区间内的整数（命令参数解析用）。
 * @param   s        待解析文本
 * @param   lo       允许下界（含）
 * @param   hi       允许上界（含）
 * @param   out      成功时写入结果
 * @return  0 成功；-1 非数字、越界或带多余字符。
 * @note    范围检查就是协议的一部分：越界一律回 err，不静默截断。
 */
int parse_long(const char *s, long lo, long hi, int *out)
{
    char *e; long v;
    if (!s || !*s) return -1;
    errno = 0; v = strtol(s, &e, 10);
    if (errno || *e || v < lo || v > hi) return -1;
    *out = (int)v; return 0;
}
/**
 * (vtouch-doc: bit)
 * @brief 取位图（cap_* 那几张能力位图）里的第 n 位。
 * @param   b        位图数组
 * @param   n        位号
 * @return  非 0 表示该位置位。
 */
int bit(const unsigned long *b, int n)
{
    return (int)((b[(unsigned)n / (8 * sizeof(unsigned long))] >> ((unsigned)n % (8 * sizeof(unsigned long)))) & 1UL);
}
/**
 * (vtouch-doc: logical_to_raw)
 * @brief 逻辑坐标（设备像素）→ 触摸屏 raw 坐标。
 * @param   logical  逻辑值
 * @param   axis     0=X 1=Y
 * @param   raw      输出 raw 值
 * @return  0 成功；-1 轴非法或该轴量程为 0。
 *
 * 为什么这么写（原有注释，逐字保留）：
 *   逻辑坐标（竖屏，脚本用的那一套）→ 内核 raw 轴值
 */
int logical_to_raw(int logical, int axis, int *raw)
{
    int size = axis ? g.logical_height : g.logical_width;
    long span = (long)g.axmax[axis] - g.axmin[axis];
    long value;
    if (size < 2 || logical < 0 || logical >= size) return -1;
    value = (long)g.axmin[axis] + ((long)logical * span + (size - 1) / 2) / (size - 1);
    if (value < g.axmin[axis]) value = g.axmin[axis];
    if (value > g.axmax[axis]) value = g.axmax[axis];
    *raw = (int)value; return 0;
}
/**
 * (vtouch-doc: raw_to_logical)
 * @brief raw 坐标 → 逻辑坐标（转发区域事件时用）。
 * @param   raw      raw 值
 * @param   axis     0=X 1=Y
 * @param   logical  输出逻辑值
 * @return  0 成功；-1 轴非法或量程非法。
 *
 * 为什么这么写（原有注释，逐字保留）：
 *   raw -> logical：把物理触点从内核 raw 轴值换算回脚本坐标（区域判定用）。
 */
int raw_to_logical(int raw, int axis, int *logical)
{
    int size = axis ? g.logical_height : g.logical_width;
    long span = (long)g.axmax[axis] - g.axmin[axis];
    long v;
    if (size < 2 || span <= 0) return -1;
    if (raw < g.axmin[axis]) raw = g.axmin[axis];
    if (raw > g.axmax[axis]) raw = g.axmax[axis];
    v = ((long)(raw - g.axmin[axis]) * (size - 1) + span / 2) / span;
    if (v < 0) v = 0; if (v > size - 1) v = size - 1;
    *logical = (int)v; return 0;
}
/* ================= 转发引擎（方案 §4）：事件队列 / 区域线程 / 出站队列 =================
 * 目的（§1/§4）：把「判断（区域五事件）」和「推送（WS 写）」从触摸注入热路径里搬走。
 * 热路径只剩两件事：合成帧写 uinput（writev）+ push 队列（微秒级、永不阻塞、永不碰 socket）。
 */

/**
 * (vtouch-doc: now_ns)
 * @brief 单调时钟（纳秒），事件时间戳用。
 * @return  单调递增的纳秒数（CLOCK_MONOTONIC）。
 */
uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
