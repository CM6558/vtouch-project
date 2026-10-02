/* ui_glue.c —— 面板侧胶水：把「老面板的进程内 12 个 C 接口」接到核心的共享内存上。
 *
 * 面板代码（src-ui/vtouch_ui.cpp）**一行不改**：它照旧调这 12 个函数，这里换掉实现；
 * T2.4 起本文件另提供操作 / 取点 / 绑定（读 + 写）的 17 个入口（面板 T2.5+ 逐字调用，都定义在文件末尾）。
 * 数据来源与去向（契约见 src/vt_shm.h、docs/UI_INTEGRATION.md §4）：
 *   物理触点 / 区域表 / 操作表 / 运行状态 ← 区 A（**只读**映射；对它写 = SIGSEGV，只死面板）
 *   区域 / 操作编辑             → 区 B 的编辑邮箱（**再写一字节到唤醒管道** ⇒ 核心立刻吃掉，不等它的
 *                                 poll 超时；按 region_add/del/rename/clear、vt_ops_* 语义生效）
 *   取点                       ↕ 区 B 的 pick_*（面板置模式，核心吞一次触摸回填逻辑坐标后自清）
 *   面板矩形                   → 区 B（逆变换回竖屏逻辑坐标；核心据此吞触摸）
 *   事件流                     ← 区 C 事件环（与"脚本有没有订阅"无关）
 *   心跳                       → 头里 ui_hb；核心心跳停滞（单调钟 3s）→ poll_step 返回 -1，面板自杀退出
 *   唤醒                       → VTOUCH_WAKE_FD（核心开的 pipe 写端）：投编辑/要停引擎时写 1 字节；
 *                                核心一死或一关读端，这里的写失败，退回"等下一轮 poll"的老路径。
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
static int                   W = -1;      /* 唤醒核心的管道写端（VTOUCH_WAKE_FD；-1 = 没有） */
static int                   pick_armed;  /* 取点（T2.8）：面板处于取点态（[取点] 已点、结果未取）。
                                           * take 的门：面板重启接旧核心时 pick_seq 可能非 0，
                                           * 不在取点态就不该把旧捕获吐出来（T2.4 递延①）。 */
static uint32_t              pick_last_seq;  /* take 的基线：只回报基线之后的捕获。进取点态时
                                              * 推进到当前 pick_seq（递延①的等价防护：否则面板重启
                                              * 接旧核心、pick_seq>0 时，点 [取点] 会在用户 tap 之前
                                              * 就把旧捕获吐出来 —— 凭空回填旧坐标）。 */
static char                  snap[MAX_REGIONS][REGION_ID_MAX + 1];
static int                   snap_n = -1;

/* ---------- 内部工具 ---------- */

/* 叫醒核心：写 1 字节。核心主循环 poll 这个管道，醒来第一件事就是吃编辑邮箱 / 看停引擎请求
 * ⇒ 面板的编辑从「等下一个 poll 超时」变成「立刻」，核心也就可以长睡（空闲唤醒 125 次/s → 0）。
 * 写失败不当错误：管道没建（老核心）或读端已关（核心在退出）时，退回「等下一轮」的老路径。 */
static void glue_wake(void)
{
    ssize_t r;
    if (W < 0) return;
    do { r = write(W, "w", 1); } while (r < 0 && errno == EINTR);
}

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
        glue_wake();
        return 0;
    }
    vt_shm_post_edit(&e);
    glue_wake();
    /* 邮箱是**单槽**的：不等核心吃掉就投下一条，前一条会被覆盖（启动批量加载 regions.conf 时
     * 实测 3 个区域只落地 1 个）。这里等一拍 —— 有唤醒管道时核心是**立刻**醒（通常 ~1ms 内生效），
     * 没有时退回「最多一个 poll 超时」。
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
    /* 唤醒核心的管道写端（核心传下来的固定 fd 号）。没有它也能跑：编辑退化成「等核心下一轮 poll」。 */
    s = getenv("VTOUCH_WAKE_FD");
    W = (s && *s) ? atoi(s) : -1;
    if (W >= 0 && fcntl(W, F_GETFD) < 0) {           /* 环境给了号但 fd 不在（老核心/被抢）→ 别乱写 */
        fprintf(stderr, "vtouch-ui: VTOUCH_WAKE_FD=%d 不是有效 fd（%s）→ 按无唤醒 fd 工作\n", W, strerror(errno));
        W = -1;
    }
    S = vt_shm_state(); B = vt_shm_b(); C = vt_shm_c(); Hh = vt_shm_hdr();
    if (!S || !B || !C) return -1;
    fprintf(stderr, "vtouch-ui: 已接核心（逻辑 %dx%d core_pid=%d 面板 pid=%d）\n",
            S->logical_width, S->logical_height, Hh ? Hh->core_pid : -1, (int)getpid());
    return 0;
}

/* 面板侧取点接口的前向声明（定义在文件末尾；poll_step 里先用到 take —— T2.8）。 */
int vtouch_pick_take(int *x, int *y);

int vtouch_poll_step(int timeout_ms)
{
    char line[VT_RING_LINE];
    static uint32_t last_drops;              /* 事件环丢行数的上次读数（v3：drops 可读 ⇒ 面板能告警） */
    int waited = 0, slice = 5;
    if (timeout_ms <= 0) timeout_ms = 1;
    for (;;) {
        if (vt_shm_ui_tick() != 0) return -1;                /* 核心死了：别"看着正常其实全死" */
        while (vt_shm_ring_pop(line, sizeof line))
            if (HK_ok && HK.event) HK.event(line);           /* 事件环 → 面板的事件日志 */
        {
            /* 环满时核心丢的是"这一条新的"（v3），面板这边按累计读数报一次，别让丢条静默。 */
            uint32_t d = vt_shm_ring_drops();
            if (d != last_drops) {
                char note[64];
                snprintf(note, sizeof note, "(事件环比面板读得快，已丢 %u 条)", (unsigned)d);
                if (HK_ok && HK.event) HK.event(note);
                last_drops = d;
            }
        }
        /* 取点（T2.8）：取到新结果 → 合成 pick_ev 行交面板事件回调（HK.event）。
         * 只在取点态里调 take（T2.4 递延①）：面板重启接旧核心时 pick_seq 可能非 0，
         * 不在取点态就不该把旧捕获吐出来。 */
        if (pick_armed) {
            int px, py;
            if (vtouch_pick_take(&px, &py)) {
                char note[48];
                pick_armed = 0;                          /* 一次请求只回报一次 */
                snprintf(note, sizeof note, "pick_ev %d %d", px, py);
                if (HK_ok && HK.event) HK.event(note);
            }
        }
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
        glue_wake();                                        /* 叫醒核心：别等 poll 超时才知道要停 */
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

/* ---------- 操作 / 取点 / 绑定只读（T2.4；面板 T2.5+ 逐字调用） ---------- */

/* 按名字找操作下标（只读区 A；比较口径同 glue_find）。 */
static int glue_op_find(const char *name)
{
    int i, n;
    if (!S || !name || !*name) return -1;
    n = S->op_count;
    if (n > MAX_OPS) n = MAX_OPS;
    for (i = 0; i < n; i++)
        if (strncmp(S->ops[i].name, name, OP_NAME_MAX) == 0) return i;
    return -1;
}

/* 投一条操作编辑（照 glue_post 三步：投邮箱 → 写唤醒管道 → 等 edit_applied 到位）。
 * 为什么必须等：邮箱是**单槽**的 —— 不等核心吃掉就投下一条，前一条会被覆盖；操作的新建 /
 * 删除 / 起跑丢一次就是真丢（区域批量加载踩过同款坑）。返回 0 = 核心已吃掉；-1 = 没共享内存 / 超时。 */
static int glue_post_op(uint32_t op, const char *name, const struct vt_op *payload)
{
    struct vt_shm_edit e;
    int spins = 0;
    if (!B) return -1;
    memset(&e, 0, sizeof e);
    e.op = op;
    if (name) snprintf(e.id, sizeof e.id, "%s", name);
    if (payload) e.payload = *payload;          /* 结构按值拷：区 B 一页装得下（见 vt_shm.h 的 _Static_assert） */
    e.seq = ++glue_seq;
    vt_shm_post_edit(&e);
    glue_wake();
    while (B->edit_applied != e.seq && spins++ < 10000) usleep(100);   /* 上限 ~1s，防死等 */
    if (B->edit_applied != e.seq) {
        fprintf(stderr, "vtouch-ui: 操作编辑 seq=%u op=%u 超时未生效\n", e.seq, (unsigned)op);
        return -1;
    }
    return 0;
}

/**
 * (vtouch-doc: vtouch_op_count)
 * @brief 操作表当前条数（面板列表用；只读区 A）。
 * @return  操作条数；没接共享内存时 0。
 */
int vtouch_op_count(void) { return S ? S->op_count : 0; }

/**
 * (vtouch-doc: vtouch_get_op)
 * @brief 取一条操作的元信息（名字 / 步数 / 门控 / 自动关；只读区 A）。
 * @param   i        操作下标（0..count-1）
 * @param   name     输出名字缓冲
 * @param   n        名字缓冲容量
 * @param   steps    输出步数（可为 NULL）
 * @param   gate     输出门控区域 id 缓冲（可为 NULL）
 * @param   gn       门控缓冲容量
 * @param   autoff   输出跑完自动关（可为 NULL）
 * @return  0 成功；-1 没接共享内存或下标越界。
 * @note    任一出参可为 NULL（跳过不写）；字符串一律 snprintf 截断、保证 NUL 结尾。
 */
int vtouch_get_op(int i, char *name, int n, int *steps, char *gate, int gn, int *autoff)
{
    const struct vt_op *o;
    if (!S || i < 0 || i >= S->op_count || i >= MAX_OPS) return -1;
    o = &S->ops[i];
    if (name && n > 0) snprintf(name, (size_t)n, "%s", o->name);
    if (gate && gn > 0) snprintf(gate, (size_t)gn, "%s", o->gate);
    if (steps) *steps = o->step_count;
    if (autoff) *autoff = o->auto_off;
    return 0;
}

/**
 * (vtouch-doc: vtouch_get_op_step)
 * @brief 取一条操作的某一步（类型 / 四个参数 / 时长；只读区 A）。
 * @param   i        操作下标
 * @param   s        步下标（0..步数-1）
 * @param   type     输出步骤类型 OP_STEP_*（可 NULL）
 * @param   a1       输出参数 1（可 NULL）
 * @param   a2       输出参数 2（可 NULL）
 * @param   a3       输出参数 3（可 NULL）
 * @param   a4       输出参数 4（可 NULL）
 * @param   ms       输出时长毫秒（可 NULL）
 * @return  0 成功；-1 没接共享内存或下标越界。
 * @note    点按：a1,a2 = 坐标、ms = 按住时长；滑动：a1,a2 → a3,a4 = 起终点、ms = 时长；等待：只用 ms。
 */
int vtouch_get_op_step(int i, int s, int *type, int *a1, int *a2, int *a3, int *a4, int *ms)
{
    const struct vt_step *st;
    if (!S || i < 0 || i >= S->op_count || i >= MAX_OPS) return -1;
    if (s < 0 || s >= S->ops[i].step_count || s >= MAX_STEPS) return -1;
    st = &S->ops[i].steps[s];
    if (type) *type = st->type;
    if (a1) *a1 = st->a1;
    if (a2) *a2 = st->a2;
    if (a3) *a3 = st->a3;
    if (a4) *a4 = st->a4;
    if (ms) *ms = st->ms;
    return 0;
}

/**
 * (vtouch-doc: vtouch_op_put)
 * @brief 新增或覆盖一条操作（整条投编辑邮箱 → 回读校验；面板侧入口）。
 * @param   name     操作名（核心再校验：1..15、[A-Za-z0-9_-]）
 * @param   gate     门控开关区域 id；NULL 或空串 = 无
 * @param   autoff   跑完自动关门控（非 0 视为 1）
 * @param   steps6   扁平步表：每 6 个 int 一组，顺序 type,a1,a2,a3,a4,ms
 * @param   nsteps   步数（1..32；越界当场拒，不投）
 * @return  0 核心已吃掉且回读通过（同名 + 步数一致）；-1 没接共享内存 / 超时 / 被核心拒（回读不通过）。
 * @note    邮箱是单槽：投完等 edit_applied 到位才返回（正常 ~1ms），否则下一条编辑会把它盖掉；核心的校验是单点（名字 / 步数 / 坐标 / 时长），被拒时回读失败、面板走现有错误提示路径。
 */
int vtouch_op_put(const char *name, const char *gate, int autoff, const int *steps6, int nsteps)
{
    struct vt_op op;
    int i, r;
    if (!B || !name || !*name) return -1;
    if (!steps6) { fprintf(stderr, "vtouch-ui: 操作载荷缺步表\n"); return -1; }
    if (nsteps < 1 || nsteps > MAX_STEPS) {          /* 越界不读 steps6：步表在面板侧，读越界就是 UB */
        fprintf(stderr, "vtouch-ui: 操作载荷步数非法（%d，应在 1..%d）\n", nsteps, MAX_STEPS);
        return -1;
    }
    memset(&op, 0, sizeof op);
    snprintf(op.name, sizeof op.name, "%s", name);
    snprintf(op.gate, sizeof op.gate, "%s", gate ? gate : "");
    op.auto_off = autoff ? 1 : 0;
    op.step_count = nsteps;
    for (i = 0; i < nsteps; i++) {                   /* 扁平步表：每 6 个 int 一组（type,a1..a4,ms） */
        op.steps[i].type = steps6[i * 6 + 0];
        op.steps[i].a1   = steps6[i * 6 + 1];
        op.steps[i].a2   = steps6[i * 6 + 2];
        op.steps[i].a3   = steps6[i * 6 + 3];
        op.steps[i].a4   = steps6[i * 6 + 4];
        op.steps[i].ms   = steps6[i * 6 + 5];
    }
    if (glue_post_op(VT_EDIT_OP_PUT, name, &op) != 0) return -1;
    /* 回读校验（spec §2.6）：核心不给逐条回执 —— 找到同名且步数一致才算真落地。 */
    r = glue_op_find(name);
    if (r < 0 || S->ops[r].step_count != nsteps) return -1;
    return 0;
}

/**
 * (vtouch-doc: vtouch_op_del)
 * @brief 删除一条操作（面板侧入口）。
 * @param   name     操作名
 * @return  0 表里已无同名条目（含本来就不存在）；-1 没接共享内存 / 超时。
 * @note    回读校验 = 找不到同名条目（spec §2.6）。
 */
int vtouch_op_del(const char *name)
{
    if (!B || !name || !*name) return -1;
    if (glue_post_op(VT_EDIT_OP_DEL, name, NULL) != 0) return -1;
    /* 回读校验：表里找不到同名才算删掉（本来就不存在 = 目标状态已成立，也算成）。 */
    return (glue_op_find(name) < 0) ? 0 : -1;
}

/**
 * (vtouch-doc: vtouch_op_clear)
 * @brief 清空操作表（面板侧入口）。
 * @note    回读校验 = op_count==0；未生效只打一行日志（void 返回，不阻塞面板）。
 */
void vtouch_op_clear(void)
{
    if (glue_post_op(VT_EDIT_OP_CLEAR, NULL, NULL) != 0) return;
    if (S && S->op_count != 0)                       /* 回读校验：没清干净就报一行，别静默 */
        fprintf(stderr, "vtouch-ui: 操作表清空未生效（op_count=%d）\n", S->op_count);
}

/**
 * (vtouch-doc: vtouch_op_run)
 * @brief 起跑一条操作（投编辑邮箱；核心忙 / 没空闲槽 / 帧内时按核心口径丢弃 + 日志）。
 * @param   name     操作名
 * @return  0 核心已吃掉本次请求；-1 没接共享内存 / 超时。
 * @note    **不做回读校验**（spec §2.6）：运行可能瞬间结束、状态已归位，回读判不了；要显示运行态请读 vtouch_op_status。
 */
int vtouch_op_run(const char *name)
{
    if (!B || !name || !*name) return -1;
    /* 不回读运行状态（spec §2.6）：操作可能瞬间跑完、状态已归位 —— 只认「邮箱被吃掉」。 */
    return glue_post_op(VT_EDIT_OP_RUN, name, NULL);
}

/**
 * (vtouch-doc: vtouch_op_stop)
 * @brief 中止运行中的操作（投编辑邮箱；没在跑时是空操作）。
 * @note    与 run 同口径：投递成功即返回；核心侧幂等、收尾也走它。
 */
void vtouch_op_stop(void)
{
    if (glue_post_op(VT_EDIT_OP_STOP, NULL, NULL) != 0)
        fprintf(stderr, "vtouch-ui: 停止请求未送达核心\n");
}

/**
 * (vtouch-doc: vtouch_op_status)
 * @brief 读执行器运行状态（只读区 A）。
 * @param   run_i    输出运行中的操作下标（-1 = 空闲；可 NULL）
 * @param   run_step 输出当前步（0 起；可 NULL）
 * @param   run_state 输出 0=空闲 1=运行（可 NULL）
 * @return  0 成功；-1 没接共享内存。
 * @note    运行中删表可能短暂显示错名（预裁决接受）：显示层、下轮运行自愈。
 */
int vtouch_op_status(int *run_i, int *run_step, int *run_state)
{
    if (!S) return -1;
    if (run_i) *run_i = S->op_run;
    if (run_step) *run_step = S->op_run_step;
    if (run_state) *run_state = S->op_run_state;
    return 0;
}

/**
 * (vtouch-doc: vtouch_pick_request)
 * @brief 请求取点：置区 B pick_mode=1，核心吞一次触摸后回填坐标并自清。
 * @note    核心侧两重防呆：20s 超时自清 + 面板死亡清理；本函数不叫醒核心（触摸按下本身会唤醒它）。
 *          同时置面板侧「取点态」（pick_armed）并把 take 基线推进到当前 pick_seq（T2.8；T2.4 递延①
 *          的等价防护：面板重启接旧核心时 pick_seq 可能非 0，不推基线的话点 [取点] 会在用户 tap 之前
 *          把**旧捕获**吐成 pick_ev —— 凭空回填旧坐标）。
 */
void vtouch_pick_request(void)
{
    pick_armed = 1;
    if (B) {
        pick_last_seq = B->pick_seq;              /* 先推基线、后置 mode：本次请求之后的捕获才算数 */
        B->pick_mode = 1;
    }
}   /* 不叫醒核心：触摸按下本身会唤醒它 */

/**
 * (vtouch-doc: vtouch_pick_cancel)
 * @brief 取消取点：清区 B pick_mode（面板内点击 = 取消）。
 * @note    同时清面板侧「取点态」（pick_armed）：取消后不再 take。
 */
void vtouch_pick_cancel(void) { pick_armed = 0; if (B) B->pick_mode = 0; }

/**
 * (vtouch-doc: vtouch_pick_take)
 * @brief 取走一次取点结果（对比 pick_seq 变化；1 = 有新坐标）。
 * @param   x        输出竖屏逻辑坐标 x（可 NULL）
 * @param   y        输出竖屏逻辑坐标 y（可 NULL）
 * @return  1 有新坐标（本次取走）；0 没有新结果。
 * @note    基线 pick_last_seq 在 vtouch_pick_request 里推进到当时的 pick_seq（T2.8）——
 *          只回报**本次取点态之后**的捕获；同一次捕获只回报一次。读侧以 ACQUIRE 读 pick_seq
 *          （配写侧屏障：读到新 seq 必能读到配对坐标）。面板在 vtouch_poll_step 里轮询它，
 *          读到就合成 pick_ev。
 */
int vtouch_pick_take(int *x, int *y)
{
    uint32_t s;
    if (!B) return 0;
    /* acquire：配写侧 __sync_synchronize 的读半边 —— 读到新 pick_seq 必能读到配对坐标（同事件环用法） */
    s = __atomic_load_n(&B->pick_seq, __ATOMIC_ACQUIRE);
    if (s == pick_last_seq) return 0;     /* 没新捕获：同一次结果不重复回报 */
    pick_last_seq = s;
    if (x) *x = (int)B->pick_x;
    if (y) *y = (int)B->pick_y;
    return 1;
}

/**
 * (vtouch-doc: vtouch_region_kind_get)
 * @brief 区域的开关型标记（0=普通 1=开关型；只读区 A）。
 * @param   i        区域下标
 * @return  kind 值；-1 没接共享内存或下标越界。
 */
int vtouch_region_kind_get(int i)
{
    if (!S || i < 0 || i >= S->region_count || i >= MAX_REGIONS) return -1;
    return S->regions[i].kind ? 1 : 0;
}

/**
 * (vtouch-doc: vtouch_region_toggle)
 * @brief 开关型区域的当前开/关状态（核心写、面板只读）。
 * @param   i        区域下标
 * @return  1 开 0 关；-1 没接共享内存或下标越界。
 * @note    与 mark 是两套来源：面板样式画 mark || (kind==toggle && toggle_on)。
 */
int vtouch_region_toggle(int i)
{
    if (!S || i < 0 || i >= S->region_count || i >= MAX_REGIONS) return -1;
    return S->regions[i].toggle_on ? 1 : 0;
}

/**
 * (vtouch-doc: vtouch_region_trig)
 * @brief 区域的触发绑定（绑定的操作名 + 触发时机；只读区 A）。
 * @param   i        区域下标
 * @param   op       输出操作名缓冲（可 NULL；未绑定 = 空串）
 * @param   n        缓冲容量
 * @param   ev       输出触发时机 0=无 1=按下 2=完整按压（可 NULL）
 * @return  0 成功；-1 没接共享内存或下标越界。
 * @note    绑定 / 开关型写入见 vtouch_region_bind / vtouch_region_kind（T3.3 已接线）；这里读的是核心区 A 里的现值（悬空引用照读）。
 */
int vtouch_region_trig(int i, char *op, int n, int *ev)
{
    if (!S || i < 0 || i >= S->region_count || i >= MAX_REGIONS) return -1;
    if (op && n > 0) snprintf(op, (size_t)n, "%s", S->regions[i].trig_op);
    if (ev) *ev = S->regions[i].trig_ev;
    return 0;
}

/* ---------- 触发侧（T3.3）：区域属性投递（BIND / KIND） ---------- */

/* 投一条区域属性编辑（BIND/KIND）：照 glue_post 三步（投邮箱 → 写唤醒管道 → 等 edit_applied 到位）。
 * 回读校验由调用方做 —— glue_verify 的签名收不下 BIND/KIND 的载荷字段（操作名 / 时机 / kind）。 */
static int glue_post_prop(uint32_t op, const char *id, const char *new_id, int t)
{
    struct vt_shm_edit e;
    int spins = 0;
    if (!B) return -1;
    memset(&e, 0, sizeof e);
    e.op = op;
    if (id) snprintf(e.id, sizeof e.id, "%s", id);
    if (new_id) snprintf(e.new_id, sizeof e.new_id, "%s", new_id);
    e.type = t;
    e.seq = ++glue_seq;
    vt_shm_post_edit(&e);
    glue_wake();
    /* 邮箱是单槽的：不等核心吃掉就投下一条，前一条会被覆盖（glue_post 同款理由）。 */
    while (B->edit_applied != e.seq && spins++ < 10000) usleep(100);   /* 上限 ~1s，防死等 */
    if (B->edit_applied != e.seq) {
        fprintf(stderr, "vtouch-ui: 区域属性编辑 seq=%u op=%u 超时未生效\n", e.seq, (unsigned)op);
        return -1;
    }
    return 0;
}

/**
 * (vtouch-doc: vtouch_region_bind)
 * @brief 把区域绑定到操作（投编辑邮箱 → 等 applied → 回读校验；面板侧入口）。
 * @param   id       区域名
 * @param   opname   操作名；NULL / 空串 / "-" = 解除绑定（照区 B 邮箱契约）
 * @param   ev       触发时机：1=按下 2=完整按压（解除时忽略 —— 核心会清 0）
 * @return  0 核心已吃掉且回读通过；-1 没接共享内存 / 超时 / 被核心拒（回读不通过）。
 * @note    回读口径：解除 = trig_op 空串且 trig_ev==0；绑定 = trig_op==opname 且 trig_ev==ev
 *          （悬空操作名照过 —— 核心允许悬空，触发时再解析）。邮箱单槽：投完等 edit_applied
 *          到位才返回（正常 ~1ms）；核心不给逐条回执，成没成以回读为准。
 */
int vtouch_region_bind(const char *id, const char *opname, int ev)
{
    int i, unbind;
    if (!B || !id || !*id) return -1;
    if (ev < 0 || ev > 2) return -1;               /* 面板侧预检（核心同一把尺子：0..2） */
    unbind = (!opname || !opname[0] || strcmp(opname, "-") == 0);
    if (glue_post_prop(VT_EDIT_BIND, id, unbind ? "-" : opname, ev) != 0) return -1;
    i = glue_find(id);
    if (i < 0) return -1;                          /* 区域不在了（被删）→ 回读不通过 */
    if (unbind) return (S->regions[i].trig_op[0] == 0 && S->regions[i].trig_ev == 0) ? 0 : -1;
    return (strcmp(S->regions[i].trig_op, opname) == 0 && S->regions[i].trig_ev == ev) ? 0 : -1;
}

/**
 * (vtouch-doc: vtouch_region_kind)
 * @brief 设置区域开关型（投编辑邮箱 → 等 applied → 回读校验；面板侧入口）。
 * @param   id       区域名
 * @param   kind     0=普通 1=开关型
 * @return  0 核心已吃掉且回读通过；-1 没接共享内存 / 超时 / 被核心拒（回读不通过）。
 * @note    回读口径 = 区域 kind 与请求一致；toggle_on 不动（核心口径：切回开关型沿用上次开关态）。
 */
int vtouch_region_kind(const char *id, int kind)
{
    int i;
    if (!B || !id || !*id) return -1;
    if (kind != 0 && kind != 1) return -1;         /* 面板侧预检（核心同一把尺子：0/1） */
    if (glue_post_prop(VT_EDIT_KIND, id, NULL, kind) != 0) return -1;
    i = glue_find(id);
    if (i < 0) return -1;
    return (S->regions[i].kind == (kind ? 1 : 0)) ? 0 : -1;
}
