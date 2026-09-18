/* ui_glue.c —— 面板侧胶水：把「老面板的进程内 12 个 C 接口」接到核心的共享内存上。
 *
 * 面板代码（src-ui/vtouch_ui.cpp）**一行不改**：它照旧调这 12 个函数，这里换掉实现。
 * 数据来源与去向（契约见 src/vt_shm.h、docs/UI_INTEGRATION.md §4）：
 *   物理触点 / 区域表 / 逻辑尺寸 ← 区 A（**只读**映射；对它写 = SIGSEGV，只死面板）
 *   区域编辑                   → 区 B 的编辑邮箱（核心 8ms 内吃掉，按 region_add/del/rename/clear 语义生效）
 *   面板矩形                   → 区 B（逆变换回竖屏逻辑坐标；核心据此吞触摸）
 *   事件流                     ← 区 C 事件环（与"脚本有没有订阅"无关）
 *   心跳                       → 头里 ui_hb；核心心跳停滞 → poll_step 返回 -1，面板自杀退出
 *
 * 四条踩过的坑（都别忘）：
 *   1) vtouch_set_hooks **按值拷贝**：调用方传的是栈上临时量，存指针会悬空 → 实测 blr 到野地址 SIGBUS；
 *   2) 「退出」按钮 = 停**引擎**（旧时代面板就是引擎）→ 写 stop_req + 给父进程 SIGTERM；
 *      但只认共享内存里记的 core_pid 且必须等于当前 getppid()：核心先死时面板会被 reparent 到 init，
 *      那时 kill(getppid()) 就是 kill init；
 *   3) 面板矩形推给核心前必须做 p2c 的**逆变换**（核心只有竖屏逻辑坐标一套），漏了就是"旋转后吞错位置"的静默 bug；
 *   4) 编译要带 -DVT_UI -DVT_UI_PANEL（否则会链到核心侧那半边）。
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* vt_internal.h 里带着核心自己的 vtouch_poll_step(void) 原型；面板这边要提供的却是老面板的接口
 * vtouch_poll_step(int timeout_ms)（名字必须一致才能不改面板代码）→ 包含期把核心那个原型改名掉。 */
#define vtouch_poll_step vt_core_poll_step_proto
#include "../src/vt_internal.h"     /* 带出 struct vt_state / g / g_ptr / vt_shm.h / raw_to_logical 等只读辅助 */
#undef vtouch_poll_step

/* 与 vtouch_ui.cpp 逐字一致（布局必须相同） */
struct vtouch_hooks {
    void (*event)(const char *line);
    void (*region_changed)(void);
    int  (*consume)(int lx, int ly);
    int  (*ui)(int want);
};

static struct vt_shm_header *Hh;
static struct vt_state      *S;
static struct vt_shm_b      *B;
static struct vt_shm_c      *C;
static struct vtouch_hooks   HK;          /* 按值保存（见文件头坑 1） */
static int                   HK_ok;
static uint32_t              glue_seq;
static char                  snap[MAX_REGIONS][REGION_ID_MAX + 1];
static int                   snap_n = -1;

/* ---------- 内部工具 ---------- */

/* p2c 的逆（vtouch_ui.cpp:193-201）：当前屏坐标 → 竖屏逻辑坐标 */
static int c2p_x(int rot, int ox, int oy, int W, int H)
{
    (void)H;
    switch (rot) {
    case 1:  return W - 1 - oy;      /* p2c: ox=y,     oy=W-1-x → x=W-1-oy, y=ox     */
    case 3:  return oy;              /* p2c: ox=H-1-y, oy=x     → x=oy,     y=H-1-ox */
    case 2:  return W - 1 - ox;
    default: return ox;
    }
}
static int c2p_y(int rot, int ox, int oy, int W, int H)
{
    (void)W;
    switch (rot) {
    case 1:  return ox;
    case 3:  return H - 1 - ox;
    case 2:  return H - 1 - oy;
    default: return oy;
    }
}

static void glue_watch_table(void)
{
    int i, n, changed = 0;
    if (!S) return;
    n = S->region_count;
    if (n < 0) n = 0;
    if (n > MAX_REGIONS) n = MAX_REGIONS;
    if (n != snap_n) changed = 1;
    else for (i = 0; i < n; i++)
        if (strncmp(snap[i], S->regions[i].id, REGION_ID_MAX) != 0) { changed = 1; break; }
    if (!changed) return;
    snap_n = n;
    for (i = 0; i < n; i++) snprintf(snap[i], sizeof snap[i], "%s", S->regions[i].id);
    if (HK_ok && HK.region_changed) HK.region_changed();     /* 面板据此清失效引用 + 重画 + 置落盘 */
}

static int glue_find(const char *id)
{
    int i, n;
    if (!S || !id || !*id) return -1;
    n = S->region_count;
    for (i = 0; i < n && i < MAX_REGIONS; i++)
        if (strncmp(S->regions[i].id, id, REGION_ID_MAX) == 0) return i;
    return -1;
}

/* 编辑到底成没成：核心不给逐条回执，就**回读区域表**判（旧接口的返回值面板在用：
 * 例如"区域数已达上限"的提示靠 region_add != 0 触发）。 */
static int glue_verify(uint32_t op, const char *id, const char *new_id)
{
    int i, j;
    if (!S) return -1;
    i = id ? glue_find(id) : -1;
    j = new_id ? glue_find(new_id) : -1;
    switch (op) {
    case VT_EDIT_ADD:    return (i >= 0) ? 0 : -1;
    case VT_EDIT_DEL:    return (i < 0) ? 0 : -1;
    case VT_EDIT_RENAME: return (j >= 0 && (i < 0 || i == j)) ? 0 : -1;
    case VT_EDIT_CLEAR:  return (S->region_count == 0) ? 0 : -1;
    default: break;
    }
    return -1;
}

/* 返回 0 = 核心确实生效了；-1 = 没生效（被核心拒了 / 超时） */
static int glue_post(uint32_t op, const char *id, const char *new_id,
                     int t, int a1, int a2, int a3, int a4, int en)
{
    struct vt_shm_edit e;
    int spins = 0;
    if (!B) return -1;
    memset(&e, 0, sizeof e);
    e.op = op; e.type = t; e.a1 = a1; e.a2 = a2; e.a3 = a3; e.a4 = a4; e.enabled = en;
    if (id) snprintf(e.id, sizeof e.id, "%s", id);
    if (new_id) snprintf(e.new_id, sizeof e.new_id, "%s", new_id);
    e.seq = ++glue_seq;
    /* 拖改的"直播写"是**绝对值**（每次都给完整几何），丢中间几次无害 → 不等，避免拖动手感被
     * 每帧 8ms 的等待拖住。只有"新建 / 删除 / 改名 / 清空"才必须等核心吃掉：丢一次就是真丢
     * （启动批量加载 regions.conf 就是这么丢过 2/3 条）。 */
    if (op == VT_EDIT_ADD && glue_find(id) >= 0) {
        vt_shm_post_edit(&e);
        return 0;
    }
    vt_shm_post_edit(&e);
    /* 邮箱是**单槽**的：不等核心吃掉就投下一条，前一条会被覆盖（启动批量加载 regions.conf 时
     * 实测 3 个区域只落地 1 个）。这里等一拍 —— 核心 8ms 一轮，实际通常 0~8ms。
     * 顺带让面板拿回"同步"语义：调用返回时核心已经生效，后面回读区域表不会看到旧值。 */
    while (B->edit_applied != e.seq && spins++ < 10000) usleep(100);   /* 上限 ~1s，防死等 */
    if (B->edit_applied != e.seq) {
        fprintf(stderr, "vtouch-ui: 编辑 seq=%u 超时未生效\n", e.seq);
        return -1;
    }
    return glue_verify(op, id, new_id);
}

/* ---------- 面板侧额外入口（vtouch_ui.cpp 只多调这两个） ---------- */

/**
 * (vtouch-doc: vtouch_ui_publish_rect)
 * @brief 把面板矩形（当前屏坐标）逆变换成竖屏逻辑坐标后推给核心，核心据此吞触摸。
 * @param   visible  面板当前是否可见（不可见 = 核心一律不吞）
 * @param   rot      当前显示方向（0..3）
 * @param   scr_w    当前屏宽（未用，留作诊断）
 * @param   scr_h    当前屏高（未用，留作诊断）
 * @param   x1,y1,x2,y2  面板矩形（当前屏坐标，任意两点）
 * @note    旋转后坐标系在换，核心拿到的是"发布中(奇数 seq)"就保守不吞 —— 宁放不吞。
 */
void vtouch_ui_publish_rect(int visible, int rot, int scr_w, int scr_h, int x1, int y1, int x2, int y2)
{
    int W, H, ax1, ay1, ax2, ay2, t;
    (void)scr_w; (void)scr_h;
    if (!S || !B) return;
    W = S->logical_width; H = S->logical_height;
    ax1 = c2p_x(rot, x1, y1, W, H); ay1 = c2p_y(rot, x1, y1, W, H);
    ax2 = c2p_x(rot, x2, y2, W, H); ay2 = c2p_y(rot, x2, y2, W, H);
    if (ax1 > ax2) { t = ax1; ax1 = ax2; ax2 = t; }
    if (ay1 > ay2) { t = ay1; ay1 = ay2; ay2 = t; }
    vt_shm_publish_rect(visible, rot, ax1, ay1, ax2, ay2);
}

/* ---------- 面板调的 12 个接口 ---------- */

void vtouch_set_hooks(const struct vtouch_hooks *h) { if (h) { HK = *h; HK_ok = 1; } }

int vtouch_phys_slots(void) { return S ? S->phys_slots : 0; }

int vtouch_phys_get(int i, int *down, int *lx, int *ly)
{
    int x, y;
    if (!S || !down || !lx || !ly || i < 0 || i >= S->phys_slots) return -1;
    x = S->phys[i].x; y = S->phys[i].y;
    *down = S->phys[i].down ? 1 : 0;
    /* 与旧核心**逐字同一契约**（backup src_vtouchd.c:913-915）：无论按下与否都返回当前（含抬起后的
     * 最后）位置。面板的"抬手提交拖改"就靠 up 那一帧的位置 —— 这里清零会把区域拖到 (0,0)：
     * 实测现象 = "拖完之后位置变了但不是我要的"。 */
    if (raw_to_logical(x, 0, lx) < 0 || raw_to_logical(y, 1, ly) < 0) return -1;
    return 0;
}

int vtouch_init(int argc, char **argv)
{
    const char *s;
    int fd;
    (void)argc; (void)argv;
    s = getenv("VTOUCH_SHM_FD");
    fd = (s && *s) ? atoi(s) : VT_SHM_FD;
    if (vt_shm_attach(fd) != 0) {
        fprintf(stderr, "vtouch-ui: 共享内存附着失败（fd=%d）—— 面板无法工作\n", fd);
        return -1;
    }
    S = vt_shm_state(); B = vt_shm_b(); C = vt_shm_c(); Hh = vt_shm_hdr();
    if (!S || !B || !C) return -1;
    fprintf(stderr, "vtouch-ui: 已接核心（逻辑 %dx%d core_pid=%d 面板 pid=%d）\n",
            S->logical_width, S->logical_height, Hh ? Hh->core_pid : -1, (int)getpid());
    return 0;
}

int vtouch_poll_step(int timeout_ms)
{
    char line[VT_RING_LINE];
    int waited = 0, slice = 5;
    if (timeout_ms <= 0) timeout_ms = 1;
    for (;;) {
        if (vt_shm_ui_tick() != 0) return -1;                /* 核心死了：别"看着正常其实全死" */
        while (vt_shm_ring_pop(line, sizeof line))
            if (HK_ok && HK.event) HK.event(line);           /* 事件环 → 面板的事件日志 */
        glue_watch_table();
        if (B && B->stop_req) return -1;                     /* 引擎要停 → 面板跟着收尾 */
        if (waited >= timeout_ms) break;
        usleep((useconds_t)((timeout_ms - waited > slice ? slice : timeout_ms - waited) * 1000));
        waited += slice;
    }
    return 0;
}

void vtouch_cleanup(void)
{
    pid_t pp = getppid();
    if (B) {
        vt_shm_publish_rect(0, 0, 0, 0, 0, 0);              /* 先声明"我不吞了"，手指立刻回系统 */
        B->stop_req = 1;
    }
    /* 「退出」= 停引擎。只杀真正的父进程（核心）：核心先死时面板会被 reparent 到 init，
     * 那时 kill(getppid()) 就是 kill init —— 所以必须用共享内存里记的 core_pid 校验。 */
    if (Hh && Hh->core_pid > 1 && Hh->core_pid == (int32_t)pp) kill(pp, SIGTERM);
}

int vtouch_region_count(void) { return S ? S->region_count : 0; }

int vtouch_get_region(int i, char *id, int idn, int *type, int *a1, int *a2, int *a3, int *a4, int *enabled)
{
    if (!S || !id || !type || !a1 || !a2 || !a3 || !a4 || !enabled) return -1;
    if (i < 0 || i >= S->region_count) return -1;
    snprintf(id, (size_t)idn, "%s", S->regions[i].id);
    *type = S->regions[i].type;
    *a1 = S->regions[i].a1; *a2 = S->regions[i].a2; *a3 = S->regions[i].a3; *a4 = S->regions[i].a4;
    *enabled = S->regions[i].enabled;
    return 0;
}

/* 区域的"开关样式"标记（核心侧 region mark <id> 1 设置；面板据此高亮——直接读共享内存里的同一份结构）。
 * 为什么单独一个取数口：vtouch_get_region 的签名被 6 处调用，为一个显示字段改签名不划算。 */
int vtouch_region_mark(int i)
{
    if (!S || i < 0 || i >= S->region_count) return 0;
    return S->regions[i].mark;
}

int vtouch_region_add(const char *id, int type, int a1, int a2, int a3, int a4, int enabled)
{
    return glue_post(VT_EDIT_ADD, id, NULL, type, a1, a2, a3, a4, enabled);
}

int vtouch_region_del(const char *id)
{
    return glue_post(VT_EDIT_DEL, id, NULL, 0, 0, 0, 0, 0, 0);
}

int vtouch_region_rename(const char *old_id, const char *new_id)
{
    return glue_post(VT_EDIT_RENAME, old_id, new_id, 0, 0, 0, 0, 0, 0);
}

void vtouch_region_clear(void)
{
    glue_post(VT_EDIT_CLEAR, NULL, NULL, 0, 0, 0, 0, 0, 0);
}
