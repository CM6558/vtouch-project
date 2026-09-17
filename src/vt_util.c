/* vt_util.c（§2 小工具） —— 模块地图见 vt_internal.h；私有状态就近放 static，共享状态走 g。 */
#include "vt_internal.h"
#include <signal.h>

/* 墙钟换算的锚点（纳米整数；wall_clock_anchor 写、wall_ms_from_mono 读；启动后不再变） */
static uint64_t g_mono0 = 0, g_real0 = 0;
/**
 * (vtouch-doc: detect_logical_size)
 * @brief 自动探测"逻辑坐标空间"尺寸 —— 问框架（`wm size`）；不传 -w/-h 时用。
 * @param   w       成功时写入宽度（逻辑像素）
 * @param   h       成功时写入高度
 * @param   src     成功时写入来源说明（给日志用，可传 NULL）
 * @return  0 成功；-1 拿不到（调用方报错，要求显式 -w/-h）。
 * @note    逻辑尺寸是这套系统的坐标契约（区域表/事件/脚本坐标全在同一空间），必须
 *          **固定、不随屏幕旋转变**，所以把 wm 的答案归一化成竖屏（短边当宽）。
 *          只认这一个来源：`wm size` 是 AOSP 公开命令（/system/bin/wm → cmd window），
 *          和脚本看到的 device.width/height 同源，且天然覆盖 wm size 覆盖值/屏幕缩放。
 *          内核 sysfs（/sys/class/drm、fb0）**故意不用**：实测本机 card0-DP-1 报
 *          2560x5120（外接屏连接器抢答），拿它当坐标空间会全线错位 —— 宁可启动失败
 *          让人显式 -w/-h，也不要静默用错的坐标空间。
 *          输出形如 "Physical size: 1440x3168"；设过覆盖时最后一行是 "Override size: ..."，
 *          因此取**最后**一个 WxH。要起一次 app_process，约 0.3s，带 2s 超时保护。
 */
/* SIGALRM 只置位：读管道时被打断（无 SA_RESTART）就放弃探测，不让启动卡住。 */
static volatile sig_atomic_t g_probe_alarm = 0;
static void probe_alarm(int sig) { (void)sig; g_probe_alarm = 1; }

static void normalize_portrait(int *w, int *h)
{
    if (*w > *h) { int t = *w; *w = *h; *h = t; }
}

int detect_logical_size(int *w, int *h, const char **src)
{
    struct sigaction sa, old;
    FILE *f;
    char buf[512];
    int a = 0, b = 0, got = 0;

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = probe_alarm;
    sigemptyset(&sa.sa_mask);              /* 故意不设 SA_RESTART：让 fgets 可被打断 */
    sigaction(SIGALRM, &sa, &old);
    g_probe_alarm = 0;
    alarm(2);
    f = popen("wm size 2>/dev/null", "r");
    if (f) {
        while (fgets(buf, sizeof buf, f)) {
            char *p = buf;
            while ((p = strchr(p, 'x')) != NULL) {
                int x = 0, y = 0;
                char *q = p;
                while (q > buf && q[-1] >= '0' && q[-1] <= '9') q--;   /* 'x' 前是宽 */
                if (p[1] >= '0' && p[1] <= '9'
                    && sscanf(q, "%dx%d", &x, &y) == 2 && x >= 2 && y >= 2) {
                    a = x; b = y; got = 1;   /* 取最后一个：有 Override 时它才是生效值 */
                }
                p++;
            }
            if (g_probe_alarm) break;
        }
        pclose(f);
    }
    alarm(0);
    sigaction(SIGALRM, &old, NULL);
    if (!got) return -1;
    *w = a; *h = b;
    normalize_portrait(w, h);
    if (src) *src = "框架 wm size";
    return 0;
}
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
 * (vtouch-doc: wall_clock_anchor)
 * @brief 锚定「单调钟 ↔ 墙钟」的偏移（启动时调一次，供 wall_ms_from_mono 换算）。
 * @note  为什么要两套钟：事件时间戳必须用单调钟（CLOCK_MONOTONIC 不会被 NTP/时区调整拽回去），
 *        但脚本那边要的是能和 Date.now() 直接比的墙钟。锚一次偏移就能同时满足两边，
 *        而且换出来的是**事件本身**发生的时刻（不是「谁处理它的时刻」，队列延迟不算进去）。
 */
void wall_clock_anchor(void)
{
    struct timespec m, r;
    clock_gettime(CLOCK_MONOTONIC, &m);
    clock_gettime(CLOCK_REALTIME, &r);
    g_mono0 = (uint64_t)m.tv_sec * 1000000000ull + (uint64_t)m.tv_nsec;
    g_real0 = (uint64_t)r.tv_sec * 1000000000ull + (uint64_t)r.tv_nsec;
}
/**
 * (vtouch-doc: wall_ms_from_mono)
 * @brief 把单调钟纳秒换成墙钟毫秒（与 Date.now() 同基准，可直接比大小/做差）。
 * @param   mono_ns  事件时间戳（now_ns() / 按下时刻那种）
 * @return  自 epoch 起的墙钟毫秒。
 * @note    前提是 wall_clock_anchor() 已在启动时调过；事件时间戳总是晚于锚点，
 *          所以这里的减法不会下溢。
 */
uint64_t wall_ms_from_mono(uint64_t mono_ns)
{
    return (g_real0 + (mono_ns - g_mono0)) / 1000000ull;
}
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
