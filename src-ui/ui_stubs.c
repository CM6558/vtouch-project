/* ui_stubs.c —— 面板「单跑模式」用的桩实现（先确定 UI 本身渲染/交互/线程正常，再接核心）。
 *
 * 为什么需要它：src-ui/vtouch_ui.cpp 依赖 11 个 vtouch_* 进程内接口（旧时代由同进程的核心提供）。
 * 先在**不接核心**的前提下把这 11 个实现成桩，就能单独验证面板：
 *   EGL 图层起没起、ImGui 帧有没有上屏、手指能不能点/拖面板、区域卡片的框选/移动/缩放/启停/改名、
 *   事件日志页、收起/关闭 UI —— 这些都与核心数据无关，桩里给一份内存区域表就够画。
 *
 * 切换：scripts/build_ui.sh 的 VTOUCH_UI_CORE=stub（默认）走本文件；=real 走 src 下全部 .c + ui_glue.c。
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* 与 src-ui/vtouch_ui.cpp 里 extern "C" 的那份定义逐字一致（布局必须相同） */
struct vtouch_hooks {
    void (*event)(const char *line);
    void (*region_changed)(void);
    int  (*consume)(int lx, int ly);
    int  (*ui)(int want);
};

/* 按值保存，不存指针：调用方传的是栈上临时量（见 vtouch_ui.cpp:1924 的 nativeInit）——
 * 旧核心的语义就是「拷进去」，存指针会悬空（实测会 blr 到野地址 → SIGBUS）。 */
static struct vtouch_hooks H;
static int H_ok;

/* ---- 内存区域表（桩的数据源）---- */
struct stu_region { char id[16]; int type, a1, a2, a3, a4, enabled;
                    char trig[16]; int trig_ev, kind; };   /* T3.3：触发绑定/开关型（内存表存得下、读得回） */
static struct stu_region R[32];
static int RN;

static void seed(void)
{
    if (RN) return;
    /* 两个演示区域：一个矩形、一个圆 —— 只为让面板首次起来就有东西可画、可拖、可点 */
    snprintf(R[RN].id, sizeof R[RN].id, "demo_rect");
    R[RN].type = 0; R[RN].a1 = 180; R[RN].a2 = 600; R[RN].a3 = 1260; R[RN].a4 = 1500; R[RN].enabled = 1; RN++;
    snprintf(R[RN].id, sizeof R[RN].id, "demo_circle");
    R[RN].type = 1; R[RN].a1 = 720; R[RN].a2 = 2100; R[RN].a3 = 300; R[RN].a4 = 0; R[RN].enabled = 1; RN++;
}

/* 接核心时代新增的入口：单跑模式下没人可推，空实现（接口面保持一致，面板代码两边都能编） */
void vtouch_ui_publish_rect(int visible, int rot, int scr_w, int scr_h, int x1, int y1, int x2, int y2)
{
    (void)visible; (void)rot; (void)scr_w; (void)scr_h; (void)x1; (void)y1; (void)x2; (void)y2;
}

/* ---- 11 个接口的桩 ---- */
void vtouch_set_hooks(const struct vtouch_hooks *h) { if (!h) return; H = *h; H_ok = 1; }

int vtouch_phys_slots(void) { return 10; }

int vtouch_phys_get(int i, int *down, int *lx, int *ly)
{
    if (i < 0 || i >= 10) return -1;
    *down = 0; *lx = 0; *ly = 0;      /* 单跑模式没有真实触点：面板不该画轨迹 */
    return 0;
}

int vtouch_init(int argc, char **argv) { (void)argc; (void)argv; return 0; }

int vtouch_poll_step(int timeout_ms)
{
    usleep((useconds_t)(timeout_ms > 0 ? timeout_ms : 1) * 1000);   /* 桩：睡够就返回，别空转烧 CPU */
    return 0;
}

void vtouch_cleanup(void) { }

void vtouch_region_clear(void) { RN = 0; }

int vtouch_region_count(void) { seed(); return RN; }

int vtouch_region_mark(int i) { (void)i; return 0; }   /* stub 模式：没有核心，恒 0 */

int vtouch_region_add(const char *id, int type, int a1, int a2, int a3, int a4, int enabled)
{
    int i;
    seed();
    if (!id || !*id || RN >= (int)(sizeof R / sizeof R[0])) return -1;
    for (i = 0; i < RN; i++) if (!strcmp(R[i].id, id)) {
        R[i].type = type; R[i].a1 = a1; R[i].a2 = a2; R[i].a3 = a3; R[i].a4 = a4; R[i].enabled = enabled;
        if (H_ok && H.region_changed) H.region_changed();
        return 0;
    }
    memset(&R[RN], 0, sizeof R[RN]);   /* 新表位清零（同核心 region_add 追加分支）：trig/kind 不带上一任残留 */
    snprintf(R[RN].id, sizeof R[RN].id, "%s", id);
    R[RN].type = type; R[RN].a1 = a1; R[RN].a2 = a2; R[RN].a3 = a3; R[RN].a4 = a4; R[RN].enabled = enabled;
    RN++;
    if (H_ok && H.region_changed) H.region_changed();
    return 0;
}

int vtouch_region_del(const char *id)
{
    int i, j;
    for (i = 0; i < RN; i++) if (!strcmp(R[i].id, id)) {
        for (j = i; j + 1 < RN; j++) R[j] = R[j + 1];
        RN--;
        if (H_ok && H.region_changed) H.region_changed();
        return 0;
    }
    return -1;
}

int vtouch_region_rename(const char *old_id, const char *new_id)
{
    int i;
    if (!new_id || !*new_id || strlen(new_id) > 15) return -1;
    for (i = 0; i < RN; i++) if (!strcmp(R[i].id, old_id)) {
        snprintf(R[i].id, sizeof R[i].id, "%s", new_id);
        if (H_ok && H.region_changed) H.region_changed();
        return 0;
    }
    return -1;
}

int vtouch_get_region(int i, char *id, int idn, int *type,
                      int *a1, int *a2, int *a3, int *a4, int *enabled)
{
    seed();
    if (i < 0 || i >= RN) return -1;
    snprintf(id, idn, "%s", R[i].id);
    *type = R[i].type; *a1 = R[i].a1; *a2 = R[i].a2; *a3 = R[i].a3; *a4 = R[i].a4; *enabled = R[i].enabled;
    return 0;
}

/* ---- 操作 / 取点 / 绑定只读（T2.5 起面板调用；单跑模式给一份内存表）----
 * 与 ui_glue.c 逐个同签名；行为是「够画出来、够点」的桩（运行态停在「第 1 步」，点停止才归位）。 */
/* 区域 id 上限（= 核心 src/vt_internal.h 的 REGION_ID_MAX；桩文件不 include 核心头，独立定义）。 */
#define REGION_ID_MAX 15
/* 计算步表达式上限（= 核心 src/vt_internal.h 的 VT_EXPR_MAX；同上独立定义）。 */
#define VT_EXPR_MAX 63
struct stu_op { char name[16]; char gate[16]; int autoff, nsteps, steps[8][8];
                char refs[8][REGION_ID_MAX + 1]; char exprs[8][VT_EXPR_MAX + 1]; };
                /* v3：每步 8 int（type,a1..a4,ms,j1,j2）+ ref；v5：+ expr（都存得下、读得回，编辑层往返用） */
static struct stu_op O[32];
static int ON;
static int ORUN = -1, ORSTEP = 0, ORSTATE = 0;

static void seed_ops(void)
{
    if (ON) return;
    /* 一条演示操作（等待 100ms）：与演示区域同理 —— 面板首次起来就有东西可看、可跑、可删 */
    snprintf(O[ON].name, sizeof O[ON].name, "demo_wait");
    O[ON].nsteps = 1;
    O[ON].steps[0][0] = 3;    /* OP_STEP_WAIT（同核心枚举值） */
    O[ON].steps[0][5] = 100;  /* ms */
    ON++;
}

int vtouch_op_count(void) { seed_ops(); return ON; }

int vtouch_get_op(int i, char *name, int n, int *steps, char *gate, int gn, int *autoff)
{
    seed_ops();
    if (i < 0 || i >= ON) return -1;
    if (name && n > 0) snprintf(name, (size_t)n, "%s", O[i].name);
    if (gate && gn > 0) snprintf(gate, (size_t)gn, "%s", O[i].gate);
    if (steps) *steps = O[i].nsteps;
    if (autoff) *autoff = O[i].autoff;
    return 0;
}

int vtouch_get_op_step(int i, int s, int *type, int *a1, int *a2, int *a3, int *a4, int *ms, char *ref, int refn,
                       int *j1, int *j2, char *expr, int exprn)
{
    seed_ops();
    if (i < 0 || i >= ON || s < 0 || s >= O[i].nsteps) return -1;
    if (type) *type = O[i].steps[s][0];
    if (a1) *a1 = O[i].steps[s][1];
    if (a2) *a2 = O[i].steps[s][2];
    if (a3) *a3 = O[i].steps[s][3];
    if (a4) *a4 = O[i].steps[s][4];
    if (ms) *ms = O[i].steps[s][5];
    if (j1) *j1 = O[i].steps[s][6];
    if (j2) *j2 = O[i].steps[s][7];
    if (ref && refn > 0) snprintf(ref, (size_t)refn, "%s", O[i].refs[s]);       /* T3.1：回读存的 ref（空 = 无） */
    if (expr && exprn > 0) snprintf(expr, (size_t)exprn, "%s", O[i].exprs[s]);  /* v5：回读存的 expr（空 = 无） */
    return 0;
}

int vtouch_op_put(const char *name, const char *gate, int autoff, const int *steps8,
                  const char (*refs)[REGION_ID_MAX + 1], const char (*exprs)[VT_EXPR_MAX + 1], int nsteps, int *out_err)
{
    int i, k;
    size_t rn;
    seed_ops();
    if (out_err) *out_err = 0;
    if (!name || !*name || !steps8 || nsteps < 1 || nsteps > 8) {   /* 桩上限与 steps[8] 对齐 */
        if (out_err) *out_err = 1;
        return -1;
    }
    for (i = 0; i < ON; i++) if (!strcmp(O[i].name, name)) break;
    if (i >= ON) {
        if (ON >= (int)(sizeof O / sizeof O[0])) { if (out_err) *out_err = 1; return -1; }
        i = ON++;
    }
    snprintf(O[i].name, sizeof O[i].name, "%s", name);
    snprintf(O[i].gate, sizeof O[i].gate, "%s", gate ? gate : "");
    O[i].autoff = autoff ? 1 : 0;
    O[i].nsteps = nsteps;
    memcpy(O[i].steps, steps8, (size_t)nsteps * 8 * sizeof(int));
    memset(O[i].refs, 0, sizeof O[i].refs);            /* 每步 ref 先清（不带上一任残留） */
    if (refs) {                                        /* ref 通道镜像：NULL = 全空；每步空串 = 无 */
        for (k = 0; k < nsteps; k++) {
            rn = strnlen(refs[k], REGION_ID_MAX + 1);
            if (rn <= REGION_ID_MAX)                   /* 未终止（防御照款）→ 留空 */
                snprintf(O[i].refs[k], sizeof O[i].refs[k], "%s", refs[k]);
        }
    }
    memset(O[i].exprs, 0, sizeof O[i].exprs);          /* 每步 expr 先清（同上） */
    if (exprs) {                                       /* expr 通道镜像（v5）：NULL = 全空；每步空串 = 无 */
        for (k = 0; k < nsteps; k++) {
            rn = strnlen(exprs[k], VT_EXPR_MAX + 1);
            if (rn <= VT_EXPR_MAX)                     /* 未终止（防御照款）→ 留空 */
                snprintf(O[i].exprs[k], sizeof O[i].exprs[k], "%s", exprs[k]);
        }
    }
    return 0;
}

int vtouch_op_del(const char *name)
{
    int i, j;
    seed_ops();
    if (!name || !*name) return -1;
    for (i = 0; i < ON; i++) if (!strcmp(O[i].name, name)) {
        for (j = i; j + 1 < ON; j++) O[j] = O[j + 1];
        ON--;
        if (ORUN == i) { ORUN = -1; ORSTEP = 0; ORSTATE = 0; }
        return 0;
    }
    return -1;
}

void vtouch_op_clear(void)
{
    ON = 0;
    ORUN = -1; ORSTEP = 0; ORSTATE = 0;
}

int vtouch_op_run(const char *name)
{
    int i;
    seed_ops();
    if (!name || !*name) return -1;
    for (i = 0; i < ON; i++) if (!strcmp(O[i].name, name)) {
        ORUN = i; ORSTEP = 0; ORSTATE = 1;   /* 桩：停在「第 1 步」，点停止才归位（不做步进模拟） */
        return 0;
    }
    return -1;
}

void vtouch_op_stop(void)
{
    ORUN = -1; ORSTEP = 0; ORSTATE = 0;
}

int vtouch_op_status(int *run_i, int *run_step, int *run_state)
{
    if (run_i) *run_i = ORUN;
    if (run_step) *run_step = ORSTEP;
    if (run_state) *run_state = ORSTATE;
    return 0;
}

/* 表达式校验（v5；v10 起带名字表）：单跑模式没有核心解析器 —— 恒放行（桩行为：面板画得出来、点得动就行）。 */
int vtouch_expr_check(const char *s, const char (*names)[16], int nnames, char *why, int whycap)
{
    (void)s; (void)names; (void)nnames;
    if (why && whycap > 0) why[0] = 0;
    return 0;
}

/* 取点：单跑模式没有核心，空实现（面板画得出来、点得动就行）。 */
void vtouch_pick_request(void) { }
void vtouch_pick_cancel(void) { }
int  vtouch_pick_take(int *x, int *y) { (void)x; (void)y; return 0; }

/* 绑定 / 开关型（T3.3）：内存表里存得下、读得回 —— 面板三行在单跑模式也点得动
 * （没有核心可投递；语义对齐契约：解除清时机、kind 只收 0/1）。 */
int vtouch_region_bind(const char *id, const char *opname, int ev)
{
    int i, unbind;
    seed();
    if (!id || !*id || ev < 0 || ev > 2) return -1;
    unbind = (!opname || !opname[0] || strcmp(opname, "-") == 0);
    for (i = 0; i < RN; i++) if (!strcmp(R[i].id, id)) {
        R[i].trig[0] = 0;
        if (!unbind) snprintf(R[i].trig, sizeof R[i].trig, "%s", opname);
        R[i].trig_ev = unbind ? 0 : ev;   /* 同核心口径：解除同时清时机 */
        return 0;
    }
    return -1;
}

int vtouch_region_kind(const char *id, int kind)
{
    int i;
    seed();
    if (!id || !*id || (kind != 0 && kind != 1)) return -1;
    for (i = 0; i < RN; i++) if (!strcmp(R[i].id, id)) { R[i].kind = kind; return 0; }
    return -1;
}

int vtouch_region_kind_get(int i) { seed(); return (i >= 0 && i < RN) ? R[i].kind : -1; }
int vtouch_region_toggle(int i) { seed(); return (i >= 0 && i < RN) ? 0 : -1; }   /* 单跑模式没有核心翻转：恒关 */
int vtouch_region_trig(int i, char *op, int n, int *ev)
{
    seed();
    if (i < 0 || i >= RN) return -1;
    if (op && n > 0) snprintf(op, (size_t)n, "%s", R[i].trig);
    if (ev) *ev = R[i].trig_ev;
    return 0;
}

/* 视觉面板抓帧（T3.2）：单跑模式没有 Java/抓帧 —— 请求置空、无帧、无错误
 * （采集覆盖层会走「抓帧超时」路径，面板照常可交互；真抓帧只在接核心（real）构建里走）。 */
void vtouch_vis_panel_capture_req(void) { }
void vtouch_vis_cap_interval_set(int ms) { (void)ms; }

int vtouch_vis_panel_frame_take(int *w, int *h, int *rot, const unsigned char **buf)
{
    (void)w; (void)h; (void)rot; (void)buf;
    return 0;
}

int vtouch_vis_panel_err_take(int *err) { (void)err; return 0; }

/* 试查（Task 7.1）：单跑模式没有核心 —— 投递永远失败（面板显示「没接核心」）、无结果可取。 */
unsigned vtouch_vis_test_post(int kind, const char *ref, const char *region, int a1, int a2)
{
    (void)kind; (void)ref; (void)region; (void)a1; (void)a2;
    return 0;
}

int vtouch_vis_test_take(unsigned *seq, int *x, int *y, int *err)
{
    (void)seq; (void)x; (void)y; (void)err;
    return 0;
}
