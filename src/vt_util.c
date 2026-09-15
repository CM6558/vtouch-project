/* vt_util.c（§2 小工具） —— 模块地图见 vt_internal.h；私有状态就近放 static，共享状态走 g。 */
#include "vt_internal.h"

int parse_long(const char *s, long lo, long hi, int *out)
{
    char *e; long v;
    if (!s || !*s) return -1;
    errno = 0; v = strtol(s, &e, 10);
    if (errno || *e || v < lo || v > hi) return -1;
    *out = (int)v; return 0;
}

int bit(const unsigned long *b, int n)
{
    return (int)((b[(unsigned)n / (8 * sizeof(unsigned long))] >> ((unsigned)n % (8 * sizeof(unsigned long)))) & 1UL);
}

/* 逻辑坐标（竖屏，脚本用的那一套）→ 内核 raw 轴值 */
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

/* raw -> logical：把物理触点从内核 raw 轴值换算回脚本坐标（pev / 区域判定用）。 */
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
uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
