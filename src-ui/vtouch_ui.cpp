/* vtouch_ui.cpp — 单进程 ImGui 面板/overlay（方案 B）。
 * 单全屏图层 → 单 EGL surface → 单 ImGui context → 单 NewFrame/帧。
 * 输入：渲染线程 10ms 快照 phys 表（slot latch），经官方事件 API 喂 ImGui；
 *   edge 事件（down/up/enter/exit）走 ev_cb。Java 只建层，不碰输入。
 */
#include <jni.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <pthread.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "imgui.h"
#include "backends/imgui_impl_opengl3.h"
#include "ui_chars.h"   /* 自动生成（scripts/gen_ui_chars.py）：面板文案里实际用到的字形集 */

/* 区域 id 上限（= 核心 src/vt_internal.h 的 REGION_ID_MAX；面板不 include 核心头，独立定义）——
 * vtouch_op_put 的每步区域引用表与编辑层本地 ref 副本（g_ope_refs）按它定宽。 */
#define REGION_ID_MAX 15

extern "C" {
struct vtouch_hooks {   /* 与 src/vtouchd.c 同一份定义：一处注册，见 vtouch_set_hooks */
    void (*event)(const char *line);
    void (*region_changed)(void);
    int  (*consume)(int lx, int ly);
    int  (*ui)(int want);
};
void vtouch_set_hooks(const struct vtouch_hooks *h);
int vtouch_phys_slots(void);
int vtouch_phys_get(int i, int *down, int *lx, int *ly);
int vtouch_init(int argc, char **argv);
int vtouch_poll_step(int timeout_ms);
void vtouch_cleanup(void);
void vtouch_region_clear(void);
int vtouch_region_count(void);
int vtouch_region_add(const char *id, int type, int a1, int a2, int a3, int a4, int enabled);
int vtouch_region_del(const char *id);                      /* 单条删（不整表重写） */
int vtouch_region_rename(const char *old_id, const char *new_id);   /* 原地改名（位置不变） */
int vtouch_region_mark(int i);
int vtouch_get_region(int i, char *id, int idn, int *type,
                      int *a1, int *a2, int *a3, int *a4, int *enabled);
/* 接核心时代新增：把面板矩形推给核心（核心据此吞触摸）。入参是**当前屏坐标**，
 * 逆变换回竖屏逻辑坐标由胶水层做（见 src-ui/ui_glue.c）。单跑模式（ui_stubs.c）里是空实现。 */
void vtouch_ui_publish_rect(int visible, int rot, int scr_w, int scr_h, int x1, int y1, int x2, int y2);
/* 操作 / 取点 / 绑定：T2.4 胶水新增的 15 个入口 + T3.3 的 2 个写入口，原型逐字（定义见
 * src-ui/ui_glue.c，单跑模式见 src-ui/ui_stubs.c）。T2.5 起面板调用「操作」那一组；取点 T2.8
 * 已接线；绑定读/写（trig/bind/kind）T3.3 已接线。 */
int  vtouch_op_count(void);
int  vtouch_get_op(int i, char *name, int n, int *steps, char *gate, int gn, int *autoff);
int  vtouch_get_op_step(int i, int s, int *type, int *a1, int *a2, int *a3, int *a4, int *ms, char *ref, int refn);
int  vtouch_op_put(const char *name, const char *gate, int autoff, const int *steps6,
                   const char (*refs)[REGION_ID_MAX + 1], int nsteps);   /* refs 可 NULL = 全空；每步空串 = 无 */
int  vtouch_op_del(const char *name);
void vtouch_op_clear(void);
int  vtouch_op_run(const char *name);
void vtouch_op_stop(void);
int  vtouch_op_status(int *run_i, int *run_step, int *run_state);
void vtouch_pick_request(void);
void vtouch_pick_cancel(void);
int  vtouch_pick_take(int *x, int *y);          /* 1 = 有新坐标 */
int  vtouch_region_kind_get(int i);
int  vtouch_region_toggle(int i);
int  vtouch_region_trig(int i, char *op, int n, int *ev);
int  vtouch_region_bind(const char *id, const char *opname, int ev);   /* 触发绑定（opname "-"/空 = 解除） */
int  vtouch_region_kind(const char *id, int kind);                     /* 开关型（0=普通 1=开关型） */
}

#define LOGT "VTouchUI"
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO, LOGT, __VA_ARGS__)
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOGT, __VA_ARGS__)
#define ALOGW(...) __android_log_print(ANDROID_LOG_WARN, LOGT, __VA_ARGS__)   /* 非致命但要说一声（如框选被上限拒掉） */

/* 时间锚点（定义在下面）：t_since_start 供启动各阶段日志与各处诊断用，先声明 */
static long now_ms(void);
static long t_since_start(void);

static int g_w = 1440, g_h = 3168;
static volatile int g_running = 1;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_render_th, g_poll_th;
static int g_poll_on = 0;

/* EGL（双槽：双图层原子翻转用。g_cur = 当前可见槽；老代码里的 g_win/g_surf 通过下面的宏
 * 自动指向"当前槽"，所以除了换绑/翻转那几处，其余代码一行不用改）。 */
static EGLDisplay g_dpy = EGL_NO_DISPLAY;
static EGLContext g_ctx = EGL_NO_CONTEXT;
static EGLConfig g_cfg = 0;
static ANativeWindow *g_win2[2] = {0, 0};
static ANativeWindow *g_win_new2[2] = {0, 0};   /* Java 送来的新 Surface（按槽） */
static EGLSurface g_surf2[2] = {EGL_NO_SURFACE, EGL_NO_SURFACE};
static int g_swap_win2[2] = {0, 0};
static int g_cur = 0;                  /* 当前可见槽（Java 原子翻转后调 nativeOnFlip 更新） */
static int g_pending = -1;             /* 正在准备的槽（-1 = 无） */
static int g_pending_frames = 0;       /* 待命槽已成功提交的帧数（到 2 才算就绪） */
static volatile int g_flip_req = 0;    /* 待命槽就绪 → 请 Java 做原子翻转 */
#define g_win      (g_win2[g_cur])
#define g_win_new  (g_win_new2[g_cur])
#define g_surf     (g_surf2[g_cur])
#define g_swap_win (g_swap_win2[g_cur])
/* 换绑后"首帧是否真的提交了"的信号：Java 在转屏时先把图层 alpha 归 0（挡住被拉伸的旧尺寸帧），
 * 拿到这个信号才恢复 alpha。用"eglSwapBuffers 成功"当判据 —— 与探针实测同一口径（误差 ~6ms）。 */
static volatile int g_swap_armed = 0, g_swap_done = 0;
/* 换绑后逐帧打点（只打 8 帧）：转屏时"面板卡片闪现到错位置"这类问题靠它定位 ——
 * 每帧把 surface 尺寸、方向、面板落位、屏幕尺寸一起打出来，哪一帧用了旧值一目了然。 */
static int g_diag_frames = 0;
/* 待生效的显示状态（Java 线程写、渲染线程在接手新窗口时取用；见 nativeOnDisplay 注释） */
static volatile int g_pend_disp = 0;
static volatile int g_pend_w = 0, g_pend_h = 0, g_pend_rot = 0;

/* 面板几何（唯一来源：下面 #define + panel_w()/in_panel() + build_panel() 三处同公式）
 * sidebar-fixed 骨架：固定侧栏 w-64(256) 不随内容滚，内容页自己滚。 */
static float g_pan_x = 780, g_pan_y = 200;
static int g_sheet = 1;                   /* 内容页开/合（合 = 只留侧栏） */
static int g_min = 0;                     /* 收起态：整窗只剩一条标题栏（会话内有效，重启展开） */
static int g_nav = 0;                     /* 0=区域列表 1=操作 2=事件日志 3=设置 4=说明 */
#define PAD_X 16
#define PAD_Y 12
#define TITLE_H 88
#define SIDE_W 256
#define SHEET_W 560
#define COL_GAP 12
#define WIN_W (PAD_X * 2 + SIDE_W + COL_GAP + SHEET_W)   /* 864 */
#define SIDE_ONLY_W (PAD_X * 2 + SIDE_W)                 /* 288 */
#define WIN_H 1180
#define MINI_W 224                                       /* 收起态悬浮小条宽（越小越不显眼） */
#define MINI_H 68                                        /* 收起态条高 = 44 按钮 + 上下 12 */
#define ZINC50  ImVec4(0.980f, 0.980f, 0.984f, 1.00f)   /* sidebar-fixed bg-zinc-50 */
#define ZINC100 ImVec4(0.957f, 0.957f, 0.961f, 1.00f)
#define ZINC200 ImVec4(0.894f, 0.894f, 0.906f, 1.00f)   /* border-zinc-200 */
#define ZINC900 ImVec4(0.094f, 0.094f, 0.106f, 1.00f)
#define WHITE   ImVec4(1.000f, 1.000f, 1.000f, 1.00f)
#define BLUE500 ImVec4(0.231f, 0.510f, 0.965f, 1.00f)   /* sidebar-fixed 主色 */
#define RED600  ImVec4(0.863f, 0.149f, 0.149f, 1.00f)
static float panel_w(void) { return (float)(g_min ? MINI_W : (g_sheet ? WIN_W : SIDE_ONLY_W)); }
static float panel_h(void) { return (float)(g_min ? MINI_H : WIN_H); }

/* 触摸快照 */
struct Dot { int on, x, y, tx[12], ty[12], tn; };
static Dot g_dots[64];
/* 触摸标记（每根手指的蓝点+轨迹、抬起扩散圈、面板触摸绿点）默认**关**：脚本注入时它会一直在
 * 屏幕上画圈，很吵。要临时打开：核心侧 env `VTOUCH_UI_MARK=1`，或面板侧边栏里的「触摸标记」按钮
 * （按钮的取值落盘到 /data/local/vtouch-runtime/ui.conf，重启保留）。 */
static int g_show_mark = 0;
static int g_uiconf_loaded = 0;
#define UI_CONF_PATH "/data/local/vtouch-runtime/ui.conf"
static void ui_conf_load_once(void)
{
    FILE *f;
    const char *e;
    if (g_uiconf_loaded) return;
    g_uiconf_loaded = 1;
    e = getenv("VTOUCH_UI_MARK");
    if (e && (e[0] == '1' || e[0] == 'o' || e[0] == 'O')) { g_show_mark = 1; return; }
    f = fopen(UI_CONF_PATH, "r");
    if (!f) return;
    {
        char b[32] = {0};
        size_t n = fread(b, 1, sizeof b - 1, f);
        fclose(f);
        if (n > 0 && (b[0] == '1' || b[0] == 'o' || b[0] == 'O')) g_show_mark = 1;
    }
}
static void ui_conf_save(void)
{
    FILE *f = fopen(UI_CONF_PATH, "w");
    if (!f) return;
    fprintf(f, "%d\n", g_show_mark);
    fclose(f);
}
static int g_prev_on[64];
static int g_mdown = 0, g_mslot = -1, g_mup_pend = 0;
static float g_mx = -1, g_my = -1;
static long g_mdown_t = 0;
static int g_drag = 0;                 /* 标题栏几何拖拽（快照侧直驱，不走 ImGui 拖拽机） */
static float g_drag_ox = 0, g_drag_oy = 0;
/* 取点（T2.8）：[取点] 点下置 1（渲染线程），回填/面板内点击取消清 0（poll 线程的 pick_ev 也会清，
 * 最坏一帧竞态，纯交互态）。g_pick_se/sf 记「发起取点的那一格」（回填目标；滑动四点同理）。 */
static int g_pick = 0;
static int g_pick_se = -1;
static int g_pick_sf = -1;
/* 取点捕获标记（T3.2）：取点成功后在捕获点画 ~2 秒十字 + 坐标文字（spec §4）。存**竖屏逻辑坐标**
 * （与回填/日志同一套；绘制前 p2c 换算，复用区域轮廓同款）；纯绘制 —— 不参与命中、不吞触摸、
 * 不写共享内存。poll 线程写、渲染线程读（与 g_flash_t 同款「单帧竞态纯装饰」口径）；
 * 多次取点以最新一次为准（重置过期）。 */
static int g_pickmk_x = 0, g_pickmk_y = 0;
static long g_pickmk_t = 0;              /* 捕获时刻（now_ms()）；0 = 无标记 */
#define PICK_MARK_MS 2000                /* 标记存活时长（~2 秒） */
/* g_need 无锁置位：只由渲染线程清零，其余线程只置 1（单字对齐存取原子；
 * 极小概率与清零竞态丢一次重画，按钮路径当前帧本就带新状态，WS 路径下次事件补画） */
static volatile int g_need = 1;
/* 「只有叠加层在请求重画」的独立标志（手指点/拖尾/闪烁/圆环/区域事件日志）。
 * 为什么分开：这些帧由用户手指驱动，60fps 连画整屏 1440×3168 会把下面的游戏挤掉帧
 * （用户反馈「点击时/激活区域时卡顿」），所以按 VT_OV_FPS_MS 节流；
 * 面板自身交互（按钮/拖动/强制帧/首帧）仍走 g_need 的即时路径。 */
static volatile int g_ov_need = 0;
#ifndef VT_OV_FPS_MS
#define VT_OV_FPS_MS 33      /* 叠加层重画最小间隔（0 = 不限频，仅测试用） */
#endif
static volatile int g_force_frames = 0;  /* 强制连画 N 帧（切换显隐/开关后兜底，免单帧被吞） */
static int g_drew_once = 0;   /* 提交过首帧没有：没提交过就无条件画 —— 否则 regions=0 + 无人触摸时
                               * 一帧都不提交，SF 那边 "nothing to draw"，面板要等点屏才出现 */
/* 「关闭 UI」：屏幕零占用 —— 面板和 overlay 都不画，图层由 Java 侧置 alpha=0/隐藏，
 * 但进程、EVIOCGRAB、WS、区域表、事件分发全部照跑（脚本照常收区域事件）。
 * 恢复通道：WS 命令 `ui show`（脚本 vt.uiShow()；onRegion 建连后也会自动发一次）。 */
static volatile int g_ui_off = 0;       /* volatile：WS 线程写、渲染线程读（缺了会「点了关闭 UI 不生效」） */
static volatile int g_ui_log_pending = -1;  /* ui_show_cb（WS/触摸线程）只置标志，日志由渲染线程打 */
static volatile int g_want_layer = 1;   /* Java 主循环轮询它决定图层可见性 */
static volatile int g_off_cleared = 0;  /* 关闭后已画过一帧透明，之后彻底不画 */
static int g_need_mouse_evt = 0, g_mouse_evt_down = 0;
static int g_swap_fail = 0;   /* eglSwapBuffers 连续失败次数：一直失败 = 帧根本没上屏，要显式退出 */
static int g_ov_show = 1;
static float g_scroll_acc = 0;           /* 手指拖面板内容滚动的累积量（渲染侧一次应用） */
static int g_scr_on = 0;                 /* 面板内非标题按下：潜在滚动 */
#define SCR_NONE 0
#define SCR_SIDE 1                       /* 侧栏 */
#define SCR_LIST 2                       /* 区域列表（区域页） */
#define SCR_SHEET 3                      /* 内容页 */
static int g_scr_target = SCR_NONE;      /* 按下时按实区锁定滚动容器 */
static volatile float g_dbg_scroll = 0;  /* 最近一次应用到的滚动位置（日志用） */
static volatile float g_list_scroll = 0; /* 区域列表子窗真实 ScrollY（渲染侧回读） */
static volatile float g_list_max = 0;    /* 区域列表子窗 ScrollMaxY（判断是否真的可滚） */
static volatile float g_card0_y = 0;     /* 首卡提交时的屏幕 Y（验证滚动是否真作用到内容） */
static volatile float g_list_top = 0;    /* 区域列表子窗顶（屏幕 Y） */
static volatile float g_list_left = 0;   /* 区域列表子窗左（屏幕 X） */
static volatile float g_list_h = 0;      /* 区域列表子窗高 */
static volatile float g_list_w = 0;      /* 区域列表子窗宽 */
static volatile float g_pan_r[4] = {0, 0, 0, 0};   /* 面板窗口真实屏幕矩形 */
/* 各容器实区（渲染侧每帧发布，快照侧按下时读；单帧竞态最坏错判一次命中，
 * 与既有遥测同风格，纯交互判定不做严格同步） */
static volatile float g_zone_title[4], g_zone_side[4], g_zone_sheet[4], g_zone_list[4];
static int in_zone(const volatile float *z, float x, float y)
{
    return x >= z[0] && x < z[2] && y >= z[1] && y < z[3];
}
static float g_scr_lx = 0, g_scr_ly = 0;
static int g_scr_moved = 0;
static int save_regions(void);   /* 定义见下：WS 线程只置位，实际写盘在渲染线程。返回 0=已落盘 */
static volatile int g_save_pending = 0;
static long g_save_retry_t = 0;   /* 落盘失败后的下次重试时刻（0=可立即尝试） */
/* 操作表（ops.conf，T2.7）落盘走**独立**的 pending/退避，不与区域表共用一个：
 * 两块文件、两条时间线 —— 一块写失败不该把另一块的改动一起卡住。编辑动作（新建/删除/编辑层
 * [完成]）只置位，实际写盘同样在渲染线程（唯一写者，节奏照 g_save_pending 那套）。 */
static volatile int g_ops_save_pending = 0;
static long g_ops_save_retry_t = 0;
/* 面板状态里引用了「已不存在的区域 id」要清理（面板自身改表：单条删/改名是面板自带能力，
 * WS 命令族只有 clear/list/add）：WS 线程只置请求，
 * 真正的清理放渲染线程做 —— g_hidden/g_sel_id 归它管，跨线程改同一份数组才是新问题。 */
static volatile int g_prune_req = 0;
static void ui_region_changed(void) { g_prune_req = 1; g_need = 1; g_force_frames = 2; g_save_pending = 1; }

/* 「关闭 UI」/恢复：want 1=显示 0=关闭 2=取反；返回 1=现在显示 / 0=现在关闭。
 * 只改标志 + 让 Java 侧轮询到（g_want_layer），不碰 region/核心锁 —— 可以被 WS 线程
 * 和面板按钮同时调用。关闭时还能收事件、还能注入，只是屏幕上什么都不占。 */
static int ui_show_cb(int want)
{
    int on;
    if (want == 2) on = g_ui_off ? 1 : 0;
    else on = want ? 1 : 0;
    g_ui_off = on ? 0 : 1;
    g_want_layer = on ? 1 : 0;
    g_off_cleared = 0;
    g_force_frames = 4;
    g_need = 1;
    /* 这条回调跑在 WS(触摸)线程上：同步 ALOGI 就是触摸线程上的阻塞 I/O（本项目铁律禁）。
     * 只置标志，日志交给渲染线程打。 */
    g_ui_log_pending = on ? 1 : 0;
    return on;
}

/* ---- 旋转：两套坐标系，只在这里换算 ----------------------------------------
 * 竖屏逻辑坐标（g_w×g_h）= daemon 的坐标系 = 区域表 = 事件坐标 = 脚本看到的坐标，永远不变；
 * 当前屏坐标（g_scr_w×g_scr_h）= ImGui/面板布局/命中判定所在的空间，随方向变。
 * 换算公式与 JS 侧 vt.p2c 同一套（r 语义同 AutoJs6 device.rotation）。 */
static volatile int g_rot = 0;        /* 0/1/2/3：Java 线程写、poll 线程的吞触摸谓词无锁读 ⇒ 必须 volatile */
static volatile int g_scr_w = 0, g_scr_h = 0;  /* 当前方向屏幕尺寸（= 图层 buffer 尺寸） */
static volatile long g_rot_settle_t = 0;       /* 转屏后「不吞触摸」的稳定窗口截止时刻 */
static int g_pan_moved = 0;           /* 用户拖过面板：换方向时不再自动回右上角 */
static void p2c_rot(int r, int x, int y, int *ox, int *oy)   /* 竖屏逻辑 → 指定方向的屏坐标 */
{
    switch (r) {
    case 1:  *ox = y;           *oy = g_w - 1 - x; break;
    case 3:  *ox = g_h - 1 - y; *oy = x;           break;
    case 2:  *ox = g_w - 1 - x; *oy = g_h - 1 - y; break;
    default: *ox = x;           *oy = y;           break;
    }
}
static void p2c(int x, int y, int *ox, int *oy)      /* 竖屏逻辑 → 当前屏 */
{
    p2c_rot(g_rot, x, y, ox, oy);
}
static void c2p_rot(int r, int x, int y, int *ox, int *oy)   /* 指定方向的屏坐标 → 竖屏逻辑（p2c_rot 的逆） */
{
    switch (r) {
    case 1:  *ox = g_w - 1 - y; *oy = x;           break;
    case 3:  *ox = y;           *oy = g_h - 1 - x; break;
    case 2:  *ox = g_w - 1 - x; *oy = g_h - 1 - y; break;
    default: *ox = x;           *oy = y;           break;
    }
}

/* ---- 区域跟随屏幕方向（用户口径）------------------------------------------------
 * 「区域在屏幕上看到的位置不随转屏改变」：右下角的区域，哪个方向都显示在右下角；
 * 尺寸按**屏上的像素**不缩放（圆圈半径、按钮大小不变）；越出屏幕不裁剪（屏外部分自然看不到）。
 *
 * 实现：不改核心、不改协议、不轮询（面板本来就由 Java 的 DisplayListener 事件驱动）。转屏时把每条
 * 区域的**屏坐标位置**保持不变，换算回竖屏坐标写回核心 —— 核心照旧用竖屏几何判定，于是「手指碰哪块」
 * 与「屏幕上看到哪块」同时跟着新方向走。
 * 节奏：**每帧一条**。编辑邮箱是单槽覆盖式，连投多条会互相覆盖（本项目实测丢 2/3），所以按帧切片，
 * 3 条区域 ≈ 3 帧（~50ms），32 条 ≈ 0.5s 渐进完成，渲染线程一秒都不卡。
 * 开关：默认**停用**（2026-09-19 用户口径「已设区域不因非人为操作改变」；见函数开头）。要回到旧的
 * 「区域跟着视口走、转屏后视觉位置不变」用 VTOUCH_REGION_ROT=on；不设=区域粘在玻璃上（坐标不动）。 */
static int g_rr_active = 0, g_rr_i = 0, g_rr_n = 0, g_rr_fail = 0, g_rr_done = 0;
static int g_rr_base_rot = 0, g_rr_base_w = 0, g_rr_base_h = 0;   /* 区域几何当前对应的「屏」 */
/* 批次状态机（评审 2026-09-18 修）：批首**锁存目标帧**并对整表**快照**，批内一律从快照重算。
 * 为什么必须这样：旧实现批内读活表、目标帧取当帧 ⇒ ①批中转屏（含转回）会把前半批映射到中间帧、
 * 后半批到最终帧，收尾只把基准记成最终帧 ⇒ 前半批永久错位**且随 #frame 落盘**（重启不自愈）；
 * ②批内写回失败/删除条目时按索引推进会漏算或错位。快照 + 按 id 定位 + 失败重跑，三条一起封住。 */
/* 快照容量：与核心侧 MAX_REGIONS（src/vt_internal.h:56）对齐 —— 面板不 include 核心头，这里独立定义。 */
#define RR_MAX_SNAP 32
static int g_rr_tgt_rot = 0, g_rr_tgt_w = 0, g_rr_tgt_h = 0;      /* 本批锁存的目标屏帧 */
static int g_rr_retry = 0;                                        /* 本批已重跑次数（上限 2） */
struct rr_snap { char id[16]; int type, a1, a2, a3, a4, en; };
static struct rr_snap g_rr_snap[RR_MAX_SNAP];                     /* 批首快照（id + 几何 + en） */
static int g_rr_off = -1;
static int g_rr_insane = 0;               /* 见过的「不自洽屏帧」次数（只用于限频日志） */
static int g_rr_env = -1;                 /* VTOUCH_REGION_BASE 是否已读 */

/* 上报的屏帧是否**自洽**：rot 0/2 时屏幕应为竖形、rot 1/3 时应为横形。
 * ⚠ 必须校验：Java 侧 getRotation() 与 getRealSize() 是两次独立反射（`VTouchUI.java:143-152`），
 * 转屏瞬间实测会出现「方向已变、尺寸还没变」的半更新三元组（该文件 `:22-23` 的注释亦记此事）。
 * 拿这种帧去换算：比例步退化成恒等（尺寸没变）+ 多转一次 ⇒ 区域被算到屏外；且基准被记成坏帧后，
 * 下一个自洽帧再换算一次 = **坏进坏出、不会自愈**（真机症状：转屏后位置全乱）。 */
static int frame_sane(int rot, int w, int h)
{
    if (w <= 0 || h <= 0) return 0;
    if (rot == 1 || rot == 3) return (w > h) ? 1 : 0;
    return (w <= h) ? 1 : 0;
}
/* 把一条区域几何从「base 屏帧」映射到「target 屏帧」下的竖屏几何。
 * **语义（用户 2026-09-18 钉死）：保持「视口坐标」不变** —— 即以当前方向的左上角为原点、用户在这块屏上
 * 看到的坐标原样保留，只把它换算到**竖屏坐标系**里存（表 / 脚本 / 核心判定用的就是这套）。
 *   例：竖屏显示 (100,100) → 手机逆时针转 90°(rot1) 后，竖屏坐标系里的值应为 (1339,100)；
 *       反过来 p2c_rot(1,1339,100) = (100,100) ✓ —— 于是表里的数字天然等于「手指在该点的原生坐标」，
 *       核心照旧拿原生坐标判定 ⇒ **显示的位置与检测的位置一致，且核心零改动**。
 * 不做比例缩放（那是「相对位置」语义，用户已否掉）；越出当前屏的视口坐标会被下面的最小钳制钉到边界。 */
static int region_rot_map(int br, int bw, int bh, int tr, int tw, int th,
                          int type, int a1, int a2, int a3, int a4,
                          int *o1, int *o2, int *o3, int *o4)
{
    int x1, y1, x2, y2, t;
    if (bw <= 0 || bh <= 0 || tw <= 0 || th <= 0) return -1;
    if (type == 1) {                     /* 圆：只搬圆心，半径按屏上像素不变 */
        p2c_rot(br, a1, a2, &x1, &y1);           /* ① 竖屏值 → 旧屏的视口坐标 */
        c2p_rot(tr, x1, y1, o1, o2);             /* ② 视口坐标原样带进新屏 → 竖屏值 */
        *o3 = (a3 > 0) ? a3 : 1; *o4 = 0;
    } else {                             /* 矩形：两角按视口坐标搬，屏上宽高自然不变 */
        p2c_rot(br, a1, a2, &x1, &y1);
        p2c_rot(br, a3, a4, &x2, &y2);
        if (x1 > x2) { t = x1; x1 = x2; x2 = t; }
        if (y1 > y2) { t = y1; y1 = y2; y2 = t; }
        c2p_rot(tr, x1, y1, o1, o2);
        c2p_rot(tr, x2, y2, o3, o4);
        /* c2p 是旋转+镜像：两个角的相对大小可能翻过来，归一一次（轴对齐矩形不变式） */
        if (*o1 > *o3) { t = *o1; *o1 = *o3; *o3 = t; }
        if (*o2 > *o4) { t = *o2; *o2 = *o4; *o4 = t; }
    }
    /* **不钳制到屏幕范围**（用户口径 2026-09-18：屏外允许、屏幕自己裁就行）。视口坐标原样换算成竖屏
     * 坐标，因此这里允许负数/超界 —— 核心侧已相应放宽量程（见 src/vt_region.c 的 region_add）。
     * 只保留两条核心判定需要的形状不变式：圆半径 ≥ 1；矩形起点 ≤ 终点（判定按 a1..a3 / a2..a4 包含）。 */
    if (type == 1) { if (*o3 < 1) *o3 = 1; }
    else { if (*o3 < *o1) *o3 = *o1; if (*o4 < *o2) *o4 = *o2; }
    return 0;
}
/* 每帧调一次（几个整数比较，极便宜）：把「区域几何所对应的屏」推进到当前屏。
 * ⚠️ **默认停用**（`g_rr_off`，见函数开头的用户口径）：只有这条路径会把区域坐标「写回」核心表与
 * regions.conf —— 触发者是转屏/启动这类非人为事件，所以默认不跑。下面对基准帧/批次的全部讲究，
 * 仅在 VTOUCH_REGION_ROT=on（恢复旧语义）时才生效。
 * ⚠️ 基准必须固定成**竖屏帧**（区域表 / regions.conf 的规范坐标系，也是旧文件的约定），
 * **不能**记成「面板启动时看到的方向」—— 否则启动方向不同，同一份表会被解释成不同的相对位置
 * （真机报过：同一批区域，横屏启动与竖屏启动显示在不同的相对位置）。
 * 表还空时不推进基准（区域可能还没载入完），等有区域了下一帧再转。 */
static void region_rot_step(void)
{
    char id[16];
    int type, a1, a2, a3, a4, en, n1, n2, n3, n4, n;
    /* 用户口径 2026-09-19：**已经设置的区域不因任何非人为操作改变**。跟随旋转会在转屏/面板启动/屏尺寸
     * 变化时按屏帧重算整表、并把新几何经 vtouch_region_add 写回核心表与 regions.conf —— 等于用内部维护
     * 动作改用户数据；且换算一旦遇上不自洽屏帧或 #frame 归属错判，区域会被算到屏外并落盘、重启不自愈。
     * 默认**停用**：区域表就是「用户设了多少就是多少」，显示按原有坐标、以当前屏左上角为原点画（下面的
     * p2c 绘制），不为「显示位置」反推数据。要恢复旧语义（区域跟随视口、转屏后视觉位置不变）用
     * VTOUCH_REGION_ROT=on —— 那条路会把坐标写回，属已知的非人为写入。 */
    if (g_rr_off < 0) {
        const char *v = getenv("VTOUCH_REGION_ROT");
        g_rr_off = (v && !strcmp(v, "on")) ? 0 : 1;
        ALOGI(g_rr_off ? "区域跟随旋转：停用（默认；VTOUCH_REGION_ROT=on 可恢复旧语义）"
                       : "区域跟随旋转：启用（VTOUCH_REGION_ROT=on，转屏会重算并写回区域坐标）");
    }
    /* 排障逃生门：VTOUCH_REGION_BASE=rot,w,h 直接指定「表里的数字属于哪个屏」，覆盖 regions.conf 的 #frame。
     * 用在「表是横屏加的、但文件里没记」这种历史数据上（改一次不用重编）。 */
    if (g_rr_env < 0) {
        g_rr_env = 1;
        const char *v = getenv("VTOUCH_REGION_BASE");
        int br, bw, bh;
        if (v) {
            if (sscanf(v, "%d,%d,%d", &br, &bw, &bh) == 3 && br >= 0 && br <= 3 && bw > 0 && bh > 0) {
                g_rr_base_rot = br; g_rr_base_w = bw; g_rr_base_h = bh;
                ALOGI("区域跟随旋转：基准帧被 VTOUCH_REGION_BASE 覆盖为 rot%d %dx%d", br, bw, bh);
            } else {
                ALOGW("VTOUCH_REGION_BASE 非法（%s）→ 忽略，用 #frame/竖屏", v);
            }
        }
    }
    if (g_rr_off || g_w <= 0 || g_h <= 0 || g_scr_w <= 0 || g_scr_h <= 0) return;
    /* ⚠ 只认自洽的屏帧（见 frame_sane）。不自洽就**什么都不做**：既不换算、也不推进基准，
     * 等 Java 侧 500ms 观察窗给到自洽值（通常 5~40ms 后；这段短暂错位被双图层翻转遮住）。 */
    if (!frame_sane(g_rot, g_scr_w, g_scr_h)) {
        g_rr_insane++;
        if (g_rr_insane == 1 || (g_rr_insane % 120) == 0)
            ALOGW("区域跟随旋转：屏帧不自洽（rot%d %dx%d，方向与屏幕形状矛盾），暂不换算（累计 %d 次）",
                  g_rot, g_scr_w, g_scr_h, g_rr_insane);
        return;
    }
    if (g_rr_base_w <= 0) { g_rr_base_rot = 0; g_rr_base_w = g_w; g_rr_base_h = g_h; }  /* 基准 = 竖屏帧 */
    if (!g_rr_active) {
        if (g_rr_base_rot == g_rot && g_rr_base_w == g_scr_w && g_rr_base_h == g_scr_h) return;
        int si;
        n = vtouch_region_count();
        if (n <= 0) return;                            /* 表还空 → 基准保持竖屏，等有区域再转 */
        if (n > RR_MAX_SNAP) n = RR_MAX_SNAP;          /* 核心表上限就是 RR_MAX_SNAP */
        /* 批首：① 锁存目标帧（批内不再看当帧）② 整表快照（id + 几何 + en）。
         * 之后一律从快照取源、按 id 在活表里定位 —— 批中转屏/删除/改名都不会让条目错位或二次换算。 */
        for (si = 0; si < n; si++) {
            if (vtouch_get_region(si, g_rr_snap[si].id, sizeof g_rr_snap[si].id, &g_rr_snap[si].type,
                                  &g_rr_snap[si].a1, &g_rr_snap[si].a2, &g_rr_snap[si].a3,
                                  &g_rr_snap[si].a4, &g_rr_snap[si].en) != 0) g_rr_snap[si].id[0] = 0;
        }
        g_rr_n = n; g_rr_i = 0; g_rr_fail = 0; g_rr_retry = 0; g_rr_active = 1;
        g_rr_tgt_rot = g_rot; g_rr_tgt_w = g_scr_w; g_rr_tgt_h = g_scr_h;
        ALOGI("区域跟随旋转：屏 %dx%d rot%d → %dx%d rot%d，重算 %d 条（每帧一条，目标帧已锁存）",
              g_rr_base_w, g_rr_base_h, g_rr_base_rot, g_rr_tgt_w, g_rr_tgt_h, g_rr_tgt_rot, g_rr_n);
    }
    if (g_rr_i >= g_rr_n) {                /* 收尾 */
        if (g_rr_fail > 0 && g_rr_retry < 2) {
            g_rr_retry++; g_rr_i = 0; g_rr_fail = 0;
            ALOGW("区域跟随旋转：本批有写回失败 → 用批首快照重跑（第 %d 次）", g_rr_retry);
            return;                        /* 基准不推进：表里还有条目停在旧屏 */
        }
        if (g_rr_fail > 0)
            ALOGW("区域跟随旋转：重跑后仍有 %d 条写回失败（这些条目停在旧屏，下次转屏会再算一次）", g_rr_fail);
        g_rr_active = 0;
        /* 基准 = **锁存的目标帧**（不是当帧）：若批中又转过，下一帧 base≠当前屏 ⇒ 自动开新批补算 */
        g_rr_base_rot = g_rr_tgt_rot; g_rr_base_w = g_rr_tgt_w; g_rr_base_h = g_rr_tgt_h;
        g_rr_done++;
        ui_region_changed();               /* 几何变了：置落盘 + 重画 */
        ALOGI("区域跟随旋转：第 %d 批完成（失败 %d 条）", g_rr_done, g_rr_fail);
        return;
    }
    if (g_rr_snap[g_rr_i].id[0]) {
        int j, ln = vtouch_region_count(), live = -1, lt, l1, l2, l3, l4, len;
        for (j = 0; j < ln; j++) {         /* 按 id 定位活表条目（索引会因删除左移，不能按索引认） */
            if (vtouch_get_region(j, id, sizeof id, &lt, &l1, &l2, &l3, &l4, &len) == 0 &&
                !strcmp(id, g_rr_snap[g_rr_i].id)) { live = j; break; }
        }
        if (live >= 0 &&
            region_rot_map(g_rr_base_rot, g_rr_base_w, g_rr_base_h, g_rr_tgt_rot, g_rr_tgt_w, g_rr_tgt_h,
                           g_rr_snap[g_rr_i].type, g_rr_snap[g_rr_i].a1, g_rr_snap[g_rr_i].a2,
                           g_rr_snap[g_rr_i].a3, g_rr_snap[g_rr_i].a4, &n1, &n2, &n3, &n4) == 0 &&
            (n1 != l1 || n2 != l2 || n3 != l3 || n4 != l4)) {   /* 与活值比 ⇒ 重跑幂等（已算好的跳过） */
            if (vtouch_region_add(g_rr_snap[g_rr_i].id, g_rr_snap[g_rr_i].type, n1, n2, n3, n4,
                                  g_rr_snap[g_rr_i].en) != 0) g_rr_fail++;
        }
    }
    g_rr_i++;
}
/* 显示方向/尺寸变化（Java 轮询到就调，随后会再送一个新 Surface）：
 * 面板按新屏重新落位（没被拖过就回右上角，同 nativeInit 公式）、强制连画几帧。 */
static void on_display(int w, int h, int rot)
{
    int changed = (w != g_scr_w || h != g_scr_h || rot != g_rot);
    g_scr_w = w; g_scr_h = h; g_rot = rot;
    /* 转屏瞬间坐标系正在换，而吞触摸判定是「按下时问一次、锁存整段手势」：一次错判会让
     * 手指整段被吞或整段漏吞。所以换向后的 500ms 内谓词一律返回 0（宁放不吞）。 */
    g_rot_settle_t = now_ms() + 500;
    if (w > 0 && h > 0) {
        if (!g_pan_moved) {
            g_pan_x = (float)(w - WIN_W - 40); if (g_pan_x < 0) g_pan_x = 0;
            g_pan_y = 200;
        } else {
            if (g_pan_x + panel_w() > (float)w) g_pan_x = (float)w - panel_w();
            if (g_pan_y + panel_h() > (float)h) g_pan_y = (float)h - panel_h();
            if (g_pan_x < 0) g_pan_x = 0;
            if (g_pan_y < 0) g_pan_y = 0;
        }
    }
    g_force_frames = 4;
    g_need = 1;
    if (changed) ALOGI("display %dx%d rot=%d (竖屏逻辑 %dx%d)", w, h, rot, g_w, g_h);
    /* 区域跟随当前方向的推进放在渲染循环里每帧自查（region_rot_step）：这样即使区域表比显示状态晚
     * 载入（regions.conf 在面板初始化早期才 import 完），也能在下一帧补上，不依赖「显示变化」事件。 */
}

/* 瞬态视觉 */
static char g_flash_id[16] = {0};
static long g_flash_t = 0;
struct Ring { int on, x, y; long t; };
static Ring g_rings[8];
static int g_ring_i = 0;
static char g_ex_id[16] = {0};
static long g_ex_t = 0;
static int g_ex_enter = 1;
static char g_evlog[8][96];
static int g_evlog_n = 0;
static double g_frame_ms = 16.0;

/* 可视化编辑状态：框选新增 / 选中拖改 / UI 侧显隐（core 表不动，只管画不画） */
static int g_cap_mode = 0;            /* 0 无 1 矩形框选 2 圆形框选 */
static int g_cap_slot = -1, g_cap_x0 = 0, g_cap_y0 = 0, g_cap_x1 = 0, g_cap_y1 = 0;
static char g_sel_id[16] = {0};
static int g_edit_slot = -1, g_edit_kind = 0;  /* 0 无 1 移动 2 缩放 */
static int g_edit_corner = 0;                 /* 缩放：矩形角 0..3（左上/右上/左下/右下） */
static int g_edit_dx = 0, g_edit_dy = 0, g_edit_e[4];
static long g_edit_last = 0;
static char g_hidden[32][16];
static int g_nhide = 0;

/* 区域改名：id 就是脚本监听的键。面板是 composer 图层，收不到系统输入法，
 * 所以内置一副触摸键盘（点一下一个字符）。默认 id 由框选自动给（r1/c1…）。 */
static int  g_name_i = -1;             /* 正在改名的区域下标，-1 = 关 */
static char g_name_old[16] = {0};      /* 原名 */
static char g_name_buf[17] = {0};      /* 编辑缓冲 */
static int  g_name_up = 0;             /* 大写档 */
static char g_name_msg[72] = {0};      /* 非法/重名提示 */

static int is_hidden(const char *id)
{
    for (int i = 0; i < g_nhide; i++) if (!strcmp(g_hidden[i], id)) return 1;
    return 0;
}
static int save_regions(void);   /* hide_toggle 先用，后定义 */
static void ev_note(const char *fmt, ...);   /* 同上：失败提示（定义在 ev 回调后） */
static void hide_toggle(const char *id)
{
    for (int i = 0; i < g_nhide; i++) {
        if (!strcmp(g_hidden[i], id)) {
            memmove(g_hidden[i], g_hidden[i + 1], (size_t)(g_nhide - i - 1) * 16);
            g_nhide--;
            save_regions();
            return;
        }
    }
    if (g_nhide < 32) { snprintf(g_hidden[g_nhide++], 16, "%s", id); save_regions(); }
    else { ALOGW("隐藏列表已满(32)：%s 未隐藏", id); ev_note("隐藏列表已满(32)，%s 未隐藏", id); }
}
/* regions.conf 格式版本：只有版本一致的才认，否则整份丢弃（防旧版本残留区域被反复加载） */
#define REGION_CONF_VER 2
/* 区域表必须放持久目录：/data/local/tmp 是 tmpfs（重启即失，区域是用户手划的，丢了很烦）。
 * /data 是 f2fs；日志/PID 继续留在 tmpfs（无需持久，还省 flash）。 */
#define REGION_CONF_DIR "/data/local/vtouch-runtime"
#define REGION_CONF_NEW REGION_CONF_DIR "/regions.conf"
#define REGION_CONF_OLD "/data/local/tmp/vtouch-runtime/regions.conf"   /* 升级前的老位置（只读一次做迁移） */
static void region_conf_dir(void)
{
    if (mkdir(REGION_CONF_DIR, 0775) < 0 && errno != EEXIST)
        ALOGE("regions.conf 目录创建失败 %s: %s", REGION_CONF_DIR, strerror(errno));
}
/* 落盘失败：把请求重新挂上、定好 1s 后重试（不刷盘），并返回 -1。
 * 所有调用点（按钮/框选/拖改/WS 钩子）都走它 —— 以前只有渲染循环那条路带重试，其余调用点
 * 把返回值一丢，写失败就等于这次改动永远不落盘（磁盘上还是旧表）。 */
static int save_failed(void)
{
    g_save_pending = 1;
    g_save_retry_t = now_ms() + 1000;
    return -1;
}
static int save_regions(void)
{
    char tmppath[128];
    int i, n;
    /* 转屏批次进行中：表是混合态（部分区域已换算、部分没有）→ 推迟到收尾后再写。
     * 不清 g_save_pending，渲染线程下一帧会再来一次（收尾时 ui_region_changed 也会再置位）。 */
    if (g_rr_active) { g_save_pending = 1; return 0; }
    region_conf_dir();
    snprintf(tmppath, sizeof tmppath, "%s.tmp", REGION_CONF_NEW);
    /* 先写 .tmp 再 rename：掉电/被杀不会留下半截文件（半截文件会被版本门整份丢弃 = 区域全丢） */
    FILE *f = fopen(tmppath, "w");
    if (!f) { ALOGE("regions.conf 写入失败 %s: %s", tmppath, strerror(errno)); return save_failed(); }
    fprintf(f, "#vtouch-regions v%d\n", REGION_CONF_VER);
    n = vtouch_region_count();
    /* #frame：**只记事实** —— 下面那些 region 行的数字一律是**竖屏逻辑坐标**（面板投编辑前
     * 就做过「当前屏 → 竖屏逻辑」的逆变换），所以「这些数字属于哪个屏」恒等于竖屏逻辑帧。
     * 历史上这里写「录制时的屏」（横屏加的表写 rot1 3168x1440，读数的人得猜 + 还要配合跟随旋转
     * 换算）；跟随旋转 2026-09-19 停用之后那句话既不成立、也永远不会再参与换算 ⇒ 现在只写
     * rot0 + 竖屏逻辑尺寸。老文件里的 `#frame 1 …` 仍能读进来（只当诊断），写完一次即自我纠正。 */
    if (g_w > 0 && g_h > 0) fprintf(f, "#frame 0 %d %d\n", g_w, g_h);
    for (i = 0; i < n; i++) {
        char id[16]; int t, a1, a2, a3, a4, en;
        char op[16]; int ev, kd;
        if (vtouch_get_region(i, id, sizeof id, &t, &a1, &a2, &a3, &a4, &en) != 0) continue;
        fprintf(f, "region %s %d %d %d %d %d %d\n", id, t, a1, a2, a3, a4, en);
        /* T3.3 增量（spec §2.8，**新行类型**；VER 不升 —— 旧读方静默忽略）：
         *   bind <区域id> <操作名|-> <down|press>   触发绑定（未绑定写 `-` 占位，同 ops.conf 的 gate 口径）
         *   kind <区域id> <0|1>                     开关型
         * 时机：仅「完整按压」（ev=2）写 press，其余一律 down（面板只会产出 1/2；未绑定 ev=0）。 */
        if (vtouch_region_trig(i, op, sizeof op, &ev) != 0) { op[0] = 0; ev = 0; }
        fprintf(f, "bind %s %s %s\n", id, op[0] ? op : "-", ev == 2 ? "press" : "down");
        kd = vtouch_region_kind_get(i);
        fprintf(f, "kind %s %d\n", id, kd == 1 ? 1 : 0);
    }
    for (i = 0; i < g_nhide; i++) fprintf(f, "hide %s\n", g_hidden[i]);
    if (fclose(f) != 0) { ALOGE("regions.conf 落盘失败: %s", strerror(errno)); return save_failed(); }
    if (rename(tmppath, REGION_CONF_NEW) != 0) {
        ALOGE("regions.conf rename 失败: %s", strerror(errno));
        return save_failed();
    }
    g_save_pending = 0;      /* 只有真落盘成功才清请求（唯一写者=渲染线程 / 启动期主线程） */
    g_save_retry_t = 0;
    return 0;
}
/* 核心表里有没有这个 id（面板只读区 A 的现役表；启动回灌「只补缺」靠它）。 */
static int region_exists(const char *id)
{
    int i, n = vtouch_region_count();
    for (i = 0; i < n; i++) {
        char cur[16]; int t, a1, a2, a3, a4, en;
        if (vtouch_get_region(i, cur, sizeof cur, &t, &a1, &a2, &a3, &a4, &en) == 0 && strcmp(cur, id) == 0)
            return 1;
    }
    return 0;
}
static void load_regions(void)
{
    char line[128];
    int ver = 0, migrated = 0, frame_seen = 0, nreg = 0, nskip = 0;
    FILE *f = fopen(REGION_CONF_NEW, "r");
    if (!f) {
        f = fopen(REGION_CONF_OLD, "r");      /* 首次升级：把 tmpfs 里的老表搬过来 */
        if (!f) return;
        migrated = 1;
        ALOGI("regions.conf 从旧路径迁移 → %s", REGION_CONF_NEW);
    }
    /* 版本门：无版本行 / 版本不符 = 旧版本残留（如已删掉的 demo 区域）→ 丢弃并立刻改写成空表 */
    if (!fgets(line, sizeof line, f) || sscanf(line, "#vtouch-regions v%d", &ver) != 1 || ver != REGION_CONF_VER) {
        fclose(f);
        ALOGI("regions.conf 旧格式/版本不符 → 丢弃清空");
        save_regions();
        return;
    }
    while (fgets(line, sizeof line, f)) {
        char id[16], op[16], tms[16]; int t, a1, a2, a3, a4, en, fr, fw, fh, ev, kd;
        /* #frame：这些数字是**在哪个屏上量的**（横屏加的表不能当竖屏口径读）。读到就把它当基准帧，
         * 启动后由 region_rot_step 按当前屏换算过去（每帧一条）。 */
        if (sscanf(line, "#frame %d %d %d", &fr, &fw, &fh) == 3) {
            if (fw > 0 && fh > 0 && fr >= 0 && fr <= 3) {
                g_rr_base_rot = fr; g_rr_base_w = fw; g_rr_base_h = fh;
                frame_seen = 1;
                ALOGI("regions.conf #frame rot%d %dx%d（表里数字所属的屏；跟随旋转默认停用 → 原样使用，不换算）",
                      fr, fw, fh);
            } else {
                ALOGW("regions.conf #frame 非法（rot%d %dx%d）→ 按竖屏规范帧解释", fr, fw, fh);
            }
        } else if (sscanf(line, "region %15s %d %d %d %d %d %d", id, &t, &a1, &a2, &a3, &a4, &en) == 7) {
            /* 判返回值：!= 0 有**两种**来源 —— ① 核心拒（非法 id：字符集/长度见 src/vt_region.c 的
             * vt_id_ok；或表满）；② glue_post 的 edit_applied 1s 超时（返回 -1，胶水层自己会打一条
             * 「编辑 seq=… 超时未生效」在前面）。文案两种都提，别把超时误报成「核心拒绝」。
             * 两种都跳过这一条、继续载入其余条目（一条坏记录不该带走整张表，更不许崩）。 */
            nreg++;
            /* **只补缺，不覆盖**（2026-09-19 批次 3）：核心表里已经有同 id 的区域 ⇒ 跳过。
             * 理由：面板是观察者 + 编辑器，核心表才是唯一真相；启动回灌若把脚本刚设的几何覆盖回去，
             * 就违反用户口径「已设置的区域不因任何非人为操作改变」。缺的照样补上（核心重启后表是空的
             * ⇒ 全量恢复，与改动前一致）。 */
            if (region_exists(id)) {
                nskip++;
                ALOGI("regions.conf %s 核心表里已有 → 跳过（不覆盖现役几何）", id);
                continue;
            }
            if (vtouch_region_add(id, t, a1, a2, a3, a4, en) != 0)
                ALOGW("regions.conf 跳过 %s（核心拒绝或编辑超时，见上一行 glue 日志, type%d %d,%d,%d,%d en%d）",
                      id, t, a1, a2, a3, a4, en);
        } else if (sscanf(line, "hide %15s", id) == 1) {
            if (g_nhide < 32 && !is_hidden(id)) snprintf(g_hidden[g_nhide++], 16, "%s", id);
        } else if (sscanf(line, "bind %15s %15s %15s", id, op, tms) == 3) {
            /* T3.3 增量（spec §2.8，新行类型；旧文件缺 = 默认「无绑定」）。时机词只认 down/press；
             * 词不认/区域没了/核心拒收/编辑超时 = 只警告跳过 —— 与 region 行「一条坏记录不带走整张表」同口径。
             * 绑定/开关型是**面板单源**字段（脚本不会设），核心表已有区域也照样回灌：不覆盖几何，只补这两项。 */
            ev = !strcmp(tms, "press") ? 2 : (!strcmp(tms, "down") ? 1 : -1);
            if (ev < 0) {
                ALOGW("regions.conf bind %s 时机词非法（%s，应 down/press）→ 跳过", id, tms);
            } else if (vtouch_region_bind(id, op, ev) != 0) {
                ALOGW("regions.conf bind %s → %s 未生效（区域不存在/被拒/编辑超时，见上一行 glue 日志）", id, op);
            }
        } else if (strncmp(line, "bind ", 5) == 0) {
            ALOGW("regions.conf bind 行不完整（缺操作名/时机）→ 跳过");
        } else if (sscanf(line, "kind %15s %d", id, &kd) == 2) {
            if (kd != 0 && kd != 1) {
                ALOGW("regions.conf kind %s 值非法（%d，应 0/1）→ 跳过", id, kd);
            } else if (vtouch_region_kind(id, kd) != 0) {
                ALOGW("regions.conf kind %s 未生效（区域不存在/被拒/编辑超时，见上一行 glue 日志）", id);
            }
        } else if (strncmp(line, "kind ", 5) == 0) {
            ALOGW("regions.conf kind 行不完整或值非数字 → 跳过");
        }
    }
    fclose(f);
    if (!frame_seen && nreg > 0)
        ALOGI("区域跟随旋转：regions.conf 无 #frame（旧文件，%d 条）→ 按**竖屏规范帧**解释；"
              "若这批区域其实是横屏时加的，它们会出现在「相对位置转 90°」的地方 —— 用面板拖动/重画一次即可写下 #frame",
              nreg);
    if (nreg > 0 && nskip)
        ALOGI("regions.conf 共 %d 条：补入 %d 条、跳过 %d 条（核心表里已有 ⇒ 不覆盖现役几何）",
              nreg, nreg - nskip, nskip);
    if (migrated) save_regions();     /* 迁移完立刻写回持久路径（老 tmpfs 文件留着无害） */
}

/* ---- 操作表落盘（ops.conf，T2.7；v2 T3.3）----------------------------------------------
 * 格式（docs/OPS_PLAN_V2.md §6.2，逐字）：`#vtouch-ops v2` 起头；一条操作 = op 行 + N 条 step 行 ——
 *   op <名> gate <门控区域id|-> autooff <0|1>
 *   step <type> <a1> <a2> <a3> <a4> <ms> <ref>
 * ref 空写 `-`（读回还原空串）；变量照写负数（如 -1）；读端兼容 v1（6 字段 step 行：ref 缺省空、
 * a3 照读——v1 滑动步的 a3 = x2，不能按缺省丢）。
 * 保存：.tmp + rename（同 save_regions，掉电不会留半截文件）；失败挂 g_ops_save_pending、
 * 1s 后退避重试（节奏同 save_failed）；成功一行「ops.conf 已存 N 条」。
 * 加载：版本门（v1 兼容读入 / v2 本格式；其余 = 整份跳过 + 改写当前表，同 load_regions 的丢弃清空口径）；
 * 只补缺（核心表已有同名 → 跳过，不覆盖现役定义）；一条坏记录只警告并继续，不带走全表。 */
#define OPS_CONF_VER  2
#define OPS_CONF_FILE REGION_CONF_DIR "/ops.conf"
#define OPS_MAX_STEPS 32        /* 同核心 MAX_STEPS / 编辑层 OPE_MAX_STEPS（面板不 include 核心头） */

/* 核心操作表里有没有这个名字（启动回灌「只补缺」靠它；与 region_exists 同款）。 */
static int op_exists(const char *name)
{
    int i, n = vtouch_op_count();
    for (i = 0; i < n; i++) {
        char cur[16];
        if (vtouch_get_op(i, cur, sizeof cur, NULL, NULL, 0, NULL) == 0 && strcmp(cur, name) == 0)
            return 1;
    }
    return 0;
}
/* 落盘失败：把请求重新挂上、定好 1s 后重试（不刷盘），并返回 -1。节奏/风格与 save_failed 同款。 */
static int ops_save_failed(void)
{
    g_ops_save_pending = 1;
    g_ops_save_retry_t = now_ms() + 1000;
    return -1;
}
static int save_ops(void)
{
    char tmppath[128];
    int i, n;
    region_conf_dir();     /* 与 regions.conf 同目录（/data/local/vtouch-runtime，持久分区） */
    snprintf(tmppath, sizeof tmppath, "%s.tmp", OPS_CONF_FILE);
    /* 先写 .tmp 再 rename：掉电/被杀不会留下半截文件（半截文件会被版本门整份丢弃 = 操作全丢） */
    FILE *f = fopen(tmppath, "w");
    if (!f) { ALOGE("ops.conf 写入失败 %s: %s", tmppath, strerror(errno)); return ops_save_failed(); }
    fprintf(f, "#vtouch-ops v%d\n", OPS_CONF_VER);
    n = vtouch_op_count();
    for (i = 0; i < n; i++) {
        char name[16], gate[16];
        int steps = 0, autoff = 0, s;
        if (vtouch_get_op(i, name, sizeof name, &steps, gate, sizeof gate, &autoff) != 0) continue;
        /* gate 空串 = 无门控 → 按 spec 写占位符 `-`（读回时还原空串）；悬空 id 原样写（核心允悬空） */
        fprintf(f, "op %s gate %s autooff %d\n", name, gate[0] ? gate : "-", autoff ? 1 : 0);
        for (s = 0; s < steps; s++) {
            int t, a1, a2, a3, a4, ms;
            char ref[REGION_ID_MAX + 1];
            if (vtouch_get_op_step(i, s, &t, &a1, &a2, &a3, &a4, &ms, ref, sizeof ref) != 0) continue;
            /* ref 空 = 无引用 → 写占位符 `-`（读回时还原空串，同 gate 口径）；变量（负数）照写 */
            fprintf(f, "step %d %d %d %d %d %d %s\n", t, a1, a2, a3, a4, ms, ref[0] ? ref : "-");
        }
    }
    if (fclose(f) != 0) { ALOGE("ops.conf 落盘失败: %s", strerror(errno)); return ops_save_failed(); }
    if (rename(tmppath, OPS_CONF_FILE) != 0) {
        ALOGE("ops.conf rename 失败: %s", strerror(errno));
        return ops_save_failed();
    }
    g_ops_save_pending = 0;      /* 只有真落盘成功才清请求（唯一写者=渲染线程 / 启动期主线程） */
    g_ops_save_retry_t = 0;
    ALOGI("ops.conf 已存 %d 条", n);
    return 0;
}
/* 结算一条从文件读到的记录（下一条 op 行 / EOF 时调用）。返回 0=补入 / 1=跳过（核心已有）/
 * -1=坏记录（已警告）。一条坏记录不影响后面的记录。 */
static int ops_load_put(const char *name, const char *gate, int autoff,
                        const int *flat, const char (*refs)[REGION_ID_MAX + 1],
                        int nsteps, int bad)
{
    if (bad || nsteps < 1) {
        ALOGW("ops.conf 跳过一条坏记录（%s%s）", name[0] ? name : "无名记录", bad ? "，字段非法" : "，无步骤行");
        return -1;
    }
    if (op_exists(name)) {
        ALOGI("ops.conf %s 核心表里已有 → 跳过（不覆盖现役定义）", name);
        return 1;
    }
    if (vtouch_op_put(name, gate, autoff, flat, refs, nsteps) != 0) {   /* refs = 每步 ref（v1 行读入时缺省空） */
        ALOGW("ops.conf 跳过 %s（核心拒收或编辑超时，见上一行 glue 日志, %d 步）", name, nsteps);
        return -1;
    }
    return 0;
}
static void load_ops(void)
{
    char line[192];
    int ver = 0, nrec = 0, nok = 0, nskip = 0, nbad = 0, orphan = 0, rc = 0;
    /* 待结算的一条（op 行 + 其后 step 行）：下一条 op 行/EOF 才结算 —— 半截记录不落表 */
    char nm[16] = {0};
    char gt[16] = {0};
    int  ao = 0, nst = 0, bad = 0;
    int  flat[OPS_MAX_STEPS * 6];
    char refs[OPS_MAX_STEPS][REGION_ID_MAX + 1];   /* v2 每步 ref（v1 行读入时缺省空） */
    FILE *f = fopen(OPS_CONF_FILE, "r");
    if (!f) return;          /* 没有文件 = 没有历史操作（首次运行），什么都不做、也不写盘 */
    /* 版本门：v1（兼容读入，spec §6.2）与 v2（本格式）都认；无版本行 / 其他版本 = 旧版本残留 →
     * 整份丢弃；照 regions.conf 口径改写当前表 */
    if (!fgets(line, sizeof line, f) || sscanf(line, "#vtouch-ops v%d", &ver) != 1 ||
        (ver != 1 && ver != OPS_CONF_VER)) {
        fclose(f);
        ALOGI("ops.conf 旧格式/版本不符 → 丢弃清空");
        save_ops();
        return;
    }
    while (fgets(line, sizeof line, f)) {
        char a[16], b[16];
        int ao2 = 0, t, a1, a2, a3, a4, ms;
        int pn = sscanf(line, "op %15s gate %15s autooff %d", a, b, &ao2);
        if (pn >= 1) {                    /* op 行（字段不全会只匹配 1/2 个）—— 先结算上一条 */
            if (nm[0]) {
                nrec++;
                rc = ops_load_put(nm, gt, ao, flat, refs, nst, bad);
                if (rc == 0) nok++; else if (rc == 1) nskip++; else nbad++;
                nm[0] = 0;
            }
            if (pn == 3) {
                snprintf(nm, sizeof nm, "%s", a);
                /* gate `-` = 无门控（保存侧占位符）→ 还原空串；悬空 id 原样带（核心允许悬空） */
                snprintf(gt, sizeof gt, "%s", (b[0] == '-' && b[1] == 0) ? "" : b);
                ao = ao2 ? 1 : 0;
                nst = 0; bad = 0;
            } else {
                ALOGW("ops.conf 跳过一条坏记录（op 行字段不全）");
                nrec++; nbad++;
                nm[0] = 0;
            }
        } else {
            char rf[REGION_ID_MAX + 1];
            int sn;
            rf[0] = 0;                     /* ref 缺省空（v1 6 字段行） */
            sn = sscanf(line, "step %d %d %d %d %d %d %15s", &t, &a1, &a2, &a3, &a4, &ms, rf);
            if (sn == 6 || sn == 7) {      /* 6 字段 = v1 行（a3 照读：v1 滑动步 a3=x2，不按缺省丢）；
                                            * 7 字段 = v2 行 */
                if (!nm[0]) {
                    if (!orphan) { ALOGW("ops.conf 有一行 step 不属于任何 op → 忽略"); orphan = 1; }
                    continue;
                }
                orphan = 0;
                if (rf[0] == '-' && rf[1] == 0) rf[0] = 0;   /* ref `-` = 空（写端占位符）→ 还原空串 */
                if (nst < OPS_MAX_STEPS) {
                    flat[nst * 6 + 0] = t; flat[nst * 6 + 1] = a1; flat[nst * 6 + 2] = a2;
                    flat[nst * 6 + 3] = a3; flat[nst * 6 + 4] = a4; flat[nst * 6 + 5] = ms;
                    snprintf(refs[nst], sizeof refs[nst], "%s", rf);
                    nst++;
                } else bad = 1;            /* 步数越上限：整条按坏记录处理（核心只收 1..32） */
            } else if (strncmp(line, "step ", 5) == 0) {   /* step 行解析失败（字段残缺）→ 整条作废 */
                if (nm[0]) bad = 1;
                else if (!orphan) { ALOGW("ops.conf 有一行 step 不属于任何 op → 忽略"); orphan = 1; }
            }
            /* 其余不识别的行（未来扩展/空行）静默忽略 —— 与 load_regions 同口径 */
        }
    }
    if (nm[0]) {                          /* 结算最后一条 */
        nrec++;
        rc = ops_load_put(nm, gt, ao, flat, refs, nst, bad);
        if (rc == 0) nok++; else if (rc == 1) nskip++; else nbad++;
    }
    fclose(f);
    if (nrec > 0 && (nskip || nbad))
        ALOGI("ops.conf 共 %d 条：补入 %d 条、跳过 %d 条（核心表里已有 %d、坏记录 %d）",
              nrec, nok, nskip + nbad, nskip, nbad);
}
static void gen_id(char *out, int circle)
{
    for (int k = 1; k < 100; k++) {
        char tmp[16];
        int n = vtouch_region_count(), used = 0;
        snprintf(tmp, sizeof tmp, circle ? "c%d" : "r%d", k);
        for (int i = 0; i < n; i++) {
            char id[16]; int t, a1, a2, a3, a4, en;
            if (vtouch_get_region(i, id, sizeof id, &t, &a1, &a2, &a3, &a4, &en) == 0 &&
                !strcmp(id, tmp)) { used = 1; break; }
        }
        if (!used) { snprintf(out, 16, "%s", tmp); return; }
    }
    snprintf(out, 16, circle ? "c99" : "r99");
}

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
/* 进程启动锚点（JNI_OnLoad 里记，即 System.load 那一刻）。
 * 启动耗时排查只认这个数：日志里 t=+Nms 各阶段一减就是每段花了多久。 */
static long g_t0_ms = 0;
static long t_since_start(void) { return g_t0_ms ? now_ms() - g_t0_ms : 0; }
static int in_panel(float x, float y)
{
    /* 普通浮动窗：g_pan 起点，宽高随内容页开合 / 收起态 */
    return x >= g_pan_x && x < g_pan_x + panel_w() && y >= g_pan_y && y < g_pan_y + panel_h();
}
static void push_ring(int x, int y)
{
    /* 原子自增：poll 线程（真手抬起）与渲染线程（面板命中）都会 push，非原子会撞同一格 */
    Ring *r = &g_rings[__atomic_fetch_add(&g_ring_i, 1, __ATOMIC_RELAXED) & 7];
    r->on = 1; r->x = x; r->y = y; r->t = now_ms();
}

/* 面板区吞触摸（注册给核心）：核心每根手指按下时问一次，参数是竖屏逻辑坐标。
 * 命中面板 → 这根手指整段不进系统（点按钮不会点到下层游戏）；面板自己靠 phys 快照照常响应。
 * UI 关闭/图层不可见时不吃，保持「穿透优先」。 */
static int ui_consume_cb(int lx, int ly)
{
    int sx, sy;
    if (g_ui_off || !g_want_layer) return 0;
    if (now_ms() < g_rot_settle_t) return 0;   /* 转屏稳定窗口内一律不吞（判定会被锁存整段手势） */
    p2c(lx, ly, &sx, &sy);
    return in_panel((float)sx, (float)sy) ? 1 : 0;
}

/* 取点结果落点（T2.8）：ui_ev_cb 收到 pick_ev 时转发过来。定义在 T2.6 区块 —— g_ope_ 与 g_ne_
 * 一族全局都定义在文件后段，这里只能先声明（与 op_edit_open 同款前向声明）。 */
static void pick_ev_apply(int px, int py);

/* ev 回调（poll 线程）：无锁写。单写者（poll）+单读者（render），定界数组，
 * 行内 snprintf 自带 NUL，最坏一帧内看到新旧混排的一行日志/闪错一次 id，
 * 纯装饰性；换来 poll 线程永不因渲染阻塞（此前 move 事件高频取锁饿死输入，整屏顿）。 */
static void ui_ev_cb(const char *line)
{
    char id[16], ev[32];
    int slot, lx, ly;
    if (!strncmp(line, "mark_ev ", 8)) {          /* 核心改了区域的"开关样式"标记 */
        g_need = 1; g_force_frames = 2; g_ov_need = 1;   /* 立刻重画（面板按需重绘，这一步是"事件驱动那一下"）*/
    }
    if (!strncmp(line, "toggle_ev ", 10)) {       /* 核心翻转了开关型区域的开关态（T3.3）：照 mark_ev —— 记日志（下面统一入环）+ 立刻重画 */
        g_need = 1; g_force_frames = 2; g_ov_need = 1;
    }
    if (g_evlog_n < 8) snprintf(g_evlog[g_evlog_n++], 96, "%s", line);
    else { memmove(g_evlog[0], g_evlog[1], 96 * 7); snprintf(g_evlog[7], 96, "%s", line); }
    if (sscanf(line, "region_ev %15s %31s %d %d %d", id, ev, &slot, &lx, &ly) == 5) {
        if (!strcmp(ev, "down")) {
            snprintf(g_flash_id, sizeof g_flash_id, "%s", id);
            g_flash_t = now_ms();
        } else if (!strcmp(ev, "up")) {
            push_ring(lx, ly);
        } else if (!strcmp(ev, "enter") || !strcmp(ev, "exit")) {
            snprintf(g_ex_id, sizeof g_ex_id, "%s", id);
            g_ex_enter = !strcmp(ev, "enter");
            g_ex_t = now_ms();
        }
    }
    {
        int px, py;
        if (sscanf(line, "pick_ev %d %d", &px, &py) == 2) pick_ev_apply(px, py);   /* 取点结果（T2.8） */
    }
    g_ov_need = 1;
}

/* 事件日志环（8 行）：追加一行原文（不带前缀；「事件日志」页逐行显示的就是它）。 */
static void ev_log_push(const char *line)
{
    if (g_evlog_n < 8) snprintf(g_evlog[g_evlog_n++], 96, "%s", line);
    else { memmove(g_evlog[0], g_evlog[1], 96 * 7); snprintf(g_evlog[7], 96, "%s", line); }
    g_need = 1;
}

/* 面板可见提示：塞一行「! ...」进「事件日志」环（用户排障看的就是这里）。
 * 只在失败路径调用（罕见），与 ui_ev_cb 同风格无锁 —— 最坏丢一行文本，不会更糟。 */
static void ev_note(const char *fmt, ...)
{
    char buf[96];
    va_list ap;
    va_start(ap, fmt);
    buf[0] = '!'; buf[1] = ' ';
    vsnprintf(buf + 2, sizeof buf - 2, fmt, ap);
    va_end(ap);
    ev_log_push(buf);
}

/* 选中区几何（UI 侧 id 查表） */
static int sel_geom(int *type, int *a1, int *a2, int *a3, int *a4)
{
    int n = vtouch_region_count();
    for (int i = 0; i < n; i++) {
        char id[16]; int t, b1, b2, b3, b4, en;
        if (vtouch_get_region(i, id, sizeof id, &t, &b1, &b2, &b3, &b4, &en) != 0) continue;
        if (!strcmp(id, g_sel_id)) {
            *type = t; *a1 = b1; *a2 = b2; *a3 = b3; *a4 = b4;
            return en;
        }
    }
    return -1;
}
/* 框选提交（归一化 + 最小尺寸门） */
static void cap_commit(void)
{
    int x0 = g_cap_x0, y0 = g_cap_y0, x1 = g_cap_x1, y1 = g_cap_y1;
    int xa = x0 < x1 ? x0 : x1, xb = x0 < x1 ? x1 : x0;
    int ya = y0 < y1 ? y0 : y1, yb = y0 < y1 ? y1 : y0;
    char id[16];
    if (g_cap_mode == 1) {
        if (xb - xa < 40 || yb - ya < 40) return;
        if (xa < 0) xa = 0; if (ya < 0) ya = 0;
        if (xb >= g_w) xb = g_w - 1; if (yb >= g_h) yb = g_h - 1;
        gen_id(id, 0);
        if (vtouch_region_add(id, 0, xa, ya, xb, yb, 1) == 0) {
            save_regions();
            ALOGI("cap add rect %s %d,%d,%d,%d", id, xa, ya, xb, yb);
        } else {
            /* 静默失败 = 「框了没反应」：日志 + 事件日志页都要说一句 */
            ALOGW("cap add rect 失败 %s（区域数可能已到上限）", id);
            ev_note("区域数已达上限(32)，新框未加入");
        }
    } else if (g_cap_mode == 2) {
        int dx = x1 - x0, dy = y1 - y0;
        int r = (int)sqrt((double)(dx * dx + dy * dy));
        if (r < 20 || x0 < 0 || x0 >= g_w || y0 < 0 || y0 >= g_h) return;
        gen_id(id, 1);
        if (vtouch_region_add(id, 1, x0, y0, r, 0, 1) == 0) {
            save_regions();
            ALOGI("cap add circle %s %d,%d r%d", id, x0, y0, r);
        } else {
            ALOGW("cap add circle 失败 %s（区域数可能已到上限）", id);
            ev_note("区域数已达上限(32)，新圈未加入");
        }
    }
}
/* 拖改应用（移动/缩放影子值；16ms 节流直播进表 ≈60Hz 跟手，up 时提交落盘） */
static void edit_apply_live(int x, int y)
{
    int t = 0, na1 = 0, na2 = 0, na3 = 0, na4 = 0;
    char id[16];
    int en;
    snprintf(id, sizeof id, "%s", g_sel_id);
    en = sel_geom(&t, &na1, &na2, &na3, &na4);
    if (en < 0) return;
    if (g_edit_kind == 1) {
        int w = g_edit_e[2] - g_edit_e[0], h = g_edit_e[3] - g_edit_e[1];
        if (t == 1) {
            na1 = x - g_edit_dx; na2 = y - g_edit_dy; na3 = g_edit_e[2]; na4 = 0;
            if (na1 < 0) na1 = 0; if (na1 >= g_w) na1 = g_w - 1;
            if (na2 < 0) na2 = 0; if (na2 >= g_h) na2 = g_h - 1;
        } else {
            na1 = x - g_edit_dx; na2 = y - g_edit_dy;
            na3 = na1 + w; na4 = na2 + h;
            if (na1 < 0) { na3 -= na1; na1 = 0; }
            if (na2 < 0) { na4 -= na2; na2 = 0; }
            if (na3 >= g_w) { na1 -= na3 - g_w + 1; na3 = g_w - 1; }
            if (na4 >= g_h) { na2 -= na4 - g_h + 1; na4 = g_h - 1; }
        }
    } else if (g_edit_kind == 2) {
        if (t == 1) {
            int dx = x - g_edit_e[0], dy = y - g_edit_e[1];
            int r = (int)sqrt((double)(dx * dx + dy * dy));
            if (r < 20) r = 20;
            na1 = g_edit_e[0]; na2 = g_edit_e[1]; na3 = r; na4 = 0;
        } else {
            int ox1 = g_edit_e[0], oy1 = g_edit_e[1], ox3 = g_edit_e[2], oy3 = g_edit_e[3];
            na1 = ox1; na2 = oy1; na3 = ox3; na4 = oy3;
            if (g_edit_corner == 0) { na1 = x; na2 = y; }
            else if (g_edit_corner == 1) { na3 = x; na2 = y; }
            else if (g_edit_corner == 2) { na1 = x; na4 = y; }
            else { na3 = x; na4 = y; }
            if (na3 - na1 < 40) { if (g_edit_corner & 1) na3 = na1 + 40; else na1 = na3 - 40; }
            if (na4 - na2 < 40) { if (g_edit_corner & 2) na4 = na2 + 40; else na2 = na4 - 40; }
            if (na1 < 0) na1 = 0; if (na2 < 0) na2 = 0;
            if (na3 >= g_w) na3 = g_w - 1; if (na4 >= g_h) na4 = g_h - 1;
        }
    }
    /* 16ms ≈ 一帧（原来 50ms 是 20Hz，真机上明显跟不上手指）。每次写的是**绝对值**，
     * 中间被核心合并掉几次无害（胶水对已存在区域不等核心回执）。 */
    if (now_ms() - g_edit_last >= 16) {
        if (vtouch_region_add(id, t, na1, na2, na3, na4, en) == 0) g_edit_last = now_ms();
    }
}
/* ---- 面板鼠标：按下分流 / 拖动 / 释放 ----
 * 唯一拖动区 = 标题栏实区（g_zone_title，渲染侧每帧发布）；其余按容器分流滚动：
 * 列表 > 内容页 > 侧栏，落在空隙里则既不拖也不滚。 */
static void panel_press(int slot, int x, int y)
{
    float fx = (float)x, fy = (float)y;
    g_mslot = slot; g_mx = fx; g_my = fy;
    g_mdown = 1; g_mup_pend = 0; g_mdown_t = now_ms();
    g_need_mouse_evt = 1; g_mouse_evt_down = 1;
    if (in_zone(g_zone_title, fx, fy)) {
        g_drag = 1;
        g_drag_ox = fx - g_pan_x; g_drag_oy = fy - g_pan_y;
        g_scr_on = 0; g_scr_target = SCR_NONE;
    } else {
        g_drag = 0;
        g_scr_target = in_zone(g_zone_list, fx, fy) ? SCR_LIST
                     : in_zone(g_zone_sheet, fx, fy) ? SCR_SHEET
                     : in_zone(g_zone_side, fx, fy) ? SCR_SIDE : SCR_NONE;
        g_scr_on = g_scr_target != SCR_NONE && g_name_i < 0;   /* 改名弹层里只点不滚 */
        g_scr_lx = fx; g_scr_ly = fy; g_scr_moved = 0;
    }
    ALOGI("panel down %d,%d drag=%d scr=%d", x, y, g_drag, g_scr_target);
}

static void panel_drag_move(int x, int y)
{
    if (!g_mdown) return;
    g_mx = (float)x; g_my = (float)y;
    if (g_drag) {
        g_pan_x = (float)x - g_drag_ox; g_pan_y = (float)y - g_drag_oy;
        g_pan_moved = 1;                       /* 用户挪过：换方向/换屏尺寸时不再自动回右上角 */
        if (g_pan_x < 0) g_pan_x = 0;
        if (g_pan_y < 0) g_pan_y = 0;
        if (g_pan_x > g_scr_w - 160) g_pan_x = (float)(g_scr_w - 160);
        if (g_pan_y > g_scr_h - 120) g_pan_y = (float)(g_scr_h - 120);
        g_need = 1;
    } else if (g_scr_on) {
        /* 拖拽滚动：死区 24px，超过后内容跟手（累积量由目标容器消费） */
        float sdx = (float)x - g_scr_lx, sdy = (float)y - g_scr_ly;
        if (!g_scr_moved && sdx * sdx + sdy * sdy > 24 * 24) g_scr_moved = 1;
        if (g_scr_moved) {
            g_scroll_acc += -sdy;
            g_scr_lx = (float)x; g_scr_ly = (float)y;
            g_need = 1;
        }
    }
}

static void panel_release(void)
{
    if (!g_mdown || g_mup_pend) return;
    g_mup_pend = 1;
    g_need_mouse_evt = 1; g_mouse_evt_down = 0;
    ALOGI("panel up drag=%d moved=%d pan=%d,%d scr=%d scroll=%.0f",
          g_drag, g_scr_moved, (int)g_pan_x, (int)g_pan_y, g_scr_target, g_dbg_scroll);
    g_drag = 0; g_scr_on = 0;
}

/* 快照：直读 phys 表 → 蓝点/轨迹 + 面板鼠标边沿（slot latch）+ 框选/拖改手势 */
static void snapshot_touches(void)
{
    int n = vtouch_phys_slots(), any = 0, i;
    static int logged_any = 0;
    /* UI「活着」= 可见且没被「关闭 UI」关掉。关闭后图层被 Java 藏掉、屏幕零占用，但手指照样
     * 会被这里看到 —— 面板控件/框选/拖改必须一律不响应，否则用户在原面板位置点一下就会把面板
     * 拖走、起框选、甚至改动区域表并落盘（而屏幕上什么都看不见，只能靠 regions.conf 发现）。 */
    int ui_live = (!g_ui_off && g_want_layer) ? 1 : 0;
    /* 把"面板现在占哪块屏幕、可不可见"推给核心：核心在注入前拿它决定这只手是给面板还是给 App。
     * 必须**每帧**推 —— 拖面板/换方向/关 UI 都会改变这块矩形。 */
    /* 稳定窗内一律**不吞**（宁放不吞）：转屏瞬间坐标系正在换，而吞触摸判定是"按下问一次、
     * 锁存整段手势"，一次错判会让手指整段被吞或整段漏吞（老面板在进程内的谓词里也是这个规矩，
     * 现在挪到"推给核心的矩形"上）。 */
    int eat_ok = ui_live && (now_ms() >= g_rot_settle_t);
    vtouch_ui_publish_rect(eat_ok, g_rot, g_scr_w, g_scr_h,
                           (int)g_pan_x, (int)g_pan_y,
                           (int)(g_pan_x + panel_w()), (int)(g_pan_y + panel_h()));
    if (n > 64) n = 64;
    for (i = 0; i < n; i++) {
        int d = 0, x = 0, y = 0;
        if (vtouch_phys_get(i, &d, &x, &y) != 0) d = 0;
        int sx, sy;                        /* 当前屏坐标：面板命中/鼠标喂给 ImGui 用这个；
                                            * x,y 仍是竖屏逻辑坐标（点/轨迹/框选/拖改都用它） */
        p2c(x, y, &sx, &sy);
        int down_edge = d && !g_prev_on[i];
        int up_edge = !d && g_prev_on[i];
        int in_p = d && ui_live && in_panel((float)sx, (float)sy);   /* 隐藏时不认面板命中 */
        if (d) {
            any = 1;
            if (!g_dots[i].on) g_dots[i].tn = 0;
            g_dots[i].on = 1; g_dots[i].x = x; g_dots[i].y = y;
            int k = g_dots[i].tn % 12;
            g_dots[i].tx[k] = x; g_dots[i].ty[k] = y;
            g_dots[i].tn++;
            /* 取点态（T2.8）：面板内新按下 = 取消（点面板里取消）。这一下不喂 ImGui ——
             * 它就是「取消」这个动作本身（吞掉，免得顺带点到数字键/按钮）。 */
            int pick_cancel = 0;
            if (down_edge && ui_live && in_p && g_pick) {
                pick_cancel = 1;
                g_pick = 0;
                vtouch_pick_cancel();
                g_need = 1; g_force_frames = 2;
                ALOGI("取点 取消（面板内点击）");
            }
            /* 手势优先于鼠标：框选 / 选中拖改（面板内一律走鼠标，保证按钮可用） */
            if (down_edge && ui_live && !in_p && !g_pick) {   /* 隐藏/取点态不开始框选/拖改（否则会改表并落盘；取点态里外侧按下 = 取点动作本身） */
                if (g_cap_mode && g_cap_slot < 0) {
                    g_cap_slot = i;
                    g_cap_x0 = g_cap_x1 = x; g_cap_y0 = g_cap_y1 = y;
                    ALOGI("cap start %d,%d", x, y);
                } else if (!g_cap_mode && g_sel_id[0] && g_edit_slot < 0) {
                    int t, a1, a2, a3, a4;
                    if (sel_geom(&t, &a1, &a2, &a3, &a4) >= 0) {
                        int corner = -1;
                        if (t == 1) {
                            int dx = x - a1, dy = y - a2;
                            int dc = (int)sqrt((double)(dx * dx + dy * dy));
                            if (dc >= a3 - 48 && dc <= a3 + 48) corner = 4; /* 圆环=缩放 */
                            else if (dx * dx + dy * dy <= a3 * a3) corner = 5; /* 圆内=移动 */
                        } else {
                            int cs[4][2] = {{a1, a2}, {a3, a2}, {a1, a4}, {a3, a4}};
                            for (int c = 0; c < 4; c++) {
                                int ddx = x - cs[c][0], ddy = y - cs[c][1];
                                if (ddx * ddx + ddy * ddy <= 48 * 48) { corner = c; break; }
                            }
                            if (corner < 0 && x >= a1 && x <= a3 && y >= a2 && y <= a4) corner = 5;
                        }
                        if (corner >= 0) {
                            g_edit_slot = i;
                            g_edit_e[0] = a1; g_edit_e[1] = a2;
                            g_edit_e[2] = a3; g_edit_e[3] = a4;
                            g_edit_last = 0;
                            if (corner == 5) {
                                g_edit_kind = 1;
                                g_edit_dx = x - a1; g_edit_dy = y - a2;
                                ALOGI("edit move %s", g_sel_id);
                            } else {
                                g_edit_kind = 2; g_edit_corner = corner == 4 ? 0 : corner;
                                ALOGI("edit resize %s", g_sel_id);
                            }
                        }
                    }
                }
            }
            if (ui_live && i == g_cap_slot) { g_cap_x1 = x; g_cap_y1 = y; }
            if (ui_live && i == g_edit_slot) edit_apply_live(x, y);
            if (ui_live && !pick_cancel && g_mslot < 0 && in_p && i != g_cap_slot && i != g_edit_slot) {
                panel_press(i, sx, sy);            /* 面板鼠标走当前屏坐标 */
            } else if (ui_live && i == g_mslot) {
                panel_drag_move(sx, sy);
            }
        } else {
            if (g_prev_on[i] && g_dots[i].on) push_ring(g_dots[i].x, g_dots[i].y);
            g_dots[i].on = 0;
            if (i == g_mslot) panel_release();
            if (i == g_mslot && !d) g_mslot = -1;
            if (up_edge && i == g_cap_slot) {
                if (ui_live) cap_commit();         /* 隐藏中抬起：放弃这次框选（不建区、不落盘） */
                g_cap_slot = -1;
            }
            if (up_edge && i == g_edit_slot && !ui_live) {
                g_edit_slot = -1; g_edit_kind = 0;   /* 隐藏中抬起：放弃这次拖改（不写表、不落盘） */
            } else if (up_edge && i == g_edit_slot) {
                int t, a1, a2, a3, a4, en;
                char id[16];
                snprintf(id, sizeof id, "%s", g_sel_id);
                en = sel_geom(&t, &a1, &a2, &a3, &a4);
                if (en >= 0) {
                    /* 最终值直写（edit_apply_live 只管节流直播） */
                    g_edit_last = 0;
                    edit_apply_live(x, y);
                    sel_geom(&t, &a1, &a2, &a3, &a4);
                    vtouch_region_add(id, t, a1, a2, a3, a4, en);
                    save_regions();
                    ALOGI("edit commit %s", id);
                }
                g_edit_slot = -1; g_edit_kind = 0;
            }
        }
        g_prev_on[i] = d;
    }
    if (any && !logged_any) ALOGI("touch down n=1+");
    if (!any && logged_any) ALOGI("touch up");
    logged_any = any;
    if (g_mdown && now_ms() - g_mdown_t > 2000) {
        g_mdown = 0; g_mup_pend = 0; g_mslot = -1; g_drag = 0; g_scr_on = 0;
    }
}

static void *poll_thread_fn(void *)
{
    while (g_running) {
        if (vtouch_poll_step(200) != 0) {
            /* poll 循环退出 = 触摸注入已经不可能工作（输入设备消失 / poll 出错）。
             * 静默留着会让面板「看着正常、其实全死」，所以收干净进程退出，让脚本看得见。 */
            ALOGE("poll loop 退出（输入设备消失/poll 出错）→ 面板退出，触摸回系统");
            vtouch_cleanup();
            _exit(0);
        }
    }
    return 0;
}

/* ---- EGL ---- */
static int egl_init_locked(ANativeWindow *w)
{
    static const EGLint cfg_attr[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 0, EGL_STENCIL_SIZE, 0, EGL_NONE
    };
    static const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLint n = 0;
    g_dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (g_dpy == EGL_NO_DISPLAY) return -1;
    if (!eglInitialize(g_dpy, 0, 0)) return -1;
    if (!eglChooseConfig(g_dpy, cfg_attr, &g_cfg, 1, &n) || n < 1) return -1;
    g_ctx = eglCreateContext(g_dpy, g_cfg, EGL_NO_CONTEXT, ctx_attr);
    if (g_ctx == EGL_NO_CONTEXT) return -1;
    ANativeWindow_setBuffersGeometry(w, 0, 0, WINDOW_FORMAT_RGBA_8888);
    return 0;
}

/* ---- 字体：SysSans-Hans TTF（TrueType 轮廓；CJK TTC 是 CFF 不可用）
 * 两档字号：正文 44（触摸可读）、元信息 30（卡片坐标/页头状态 = bento 的小字层级）
 * 字形表只取源码里真正出现的字（ui_chars.h）：ChineseFull 会把 2 万+ 汉字 × 两档
 * 字号全烘一遍（真机 ~860ms），图集 4096² 还装不下、后面的字被静默丢掉。 ---- */
static ImFont *g_font_meta = 0;
static const ImWchar *ui_glyph_ranges(void)
{
    static ImVector<ImWchar> rg;
    if (rg.empty()) {
        ImFontGlyphRangesBuilder b;
        b.AddRanges(ImGui::GetIO().Fonts->GetGlyphRangesDefault());   /* ASCII/Latin1 基础盘 */
        if (k_ui_chars[0]) b.AddText(k_ui_chars);
        else b.AddRanges(ImGui::GetIO().Fonts->GetGlyphRangesChineseFull());  /* 生成失败兜底 */
        b.BuildRanges(&rg);
    }
    return rg.Data;
}
static void font_probe(void)
{
    static const char *path = "/system/fonts/SysSans-Hans-Regular.ttf";
    ImGuiIO &io = ImGui::GetIO();
    long b0 = now_ms();
    const ImWchar *rg = ui_glyph_ranges();
    ImFont *f = io.Fonts->AddFontFromFileTTF(path, 44.0f, 0, rg);
    if (f) g_font_meta = io.Fonts->AddFontFromFileTTF(path, 30.0f, 0, rg);
    else io.Fonts->AddFontDefault();
    unsigned char *px; int pw, ph;
    io.Fonts->GetTexDataAsRGBA32(&px, &pw, &ph);   /* 建图集后才可判字形 */
    if (f && f->FindGlyphNoFallback(0x4E2D)) {
        ALOGI("font ok %s 44%s atlas=%dx%d bake=%.0fms t=+%.0fms",
              path, g_font_meta ? "+30" : "", pw, ph,
              (double)(now_ms() - b0), (double)t_since_start());
        return;
    }
    io.Fonts->Clear();
    io.Fonts->AddFontDefault();
    g_font_meta = 0;
    ALOGE("font fallback to default (no CJK)");
}

/* ---- overlay（全屏无形窗口）：描边/填充闪/进出脉冲/轨迹/点/圈 ---- */
static void build_overlay(int sw, int sh)
{
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2((float)sw, (float)sh));
    ImGui::Begin("##ov", 0,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground |
                 ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    if (g_scr_w > 0 && g_scr_h > 0) {
        int n = vtouch_region_count();
        long t = now_ms();
        for (int i = 0; i < n; i++) {
            char id[16]; int type, a1, a2, a3, a4, en;
            if (vtouch_get_region(i, id, sizeof id, &type, &a1, &a2, &a3, &a4, &en) != 0) continue;
            if (!en) continue;
            if (is_hidden(id)) continue;
            int is_sel = g_sel_id[0] && !strcmp(id, g_sel_id);
            int inside = 0;
            for (int di = 0; di < 64 && !inside; di++) {
                int fx, fy;
                if (!g_dots[di].on) continue;
                fx = g_dots[di].x; fy = g_dots[di].y;
                inside = (type == 1 ? ((fx - a1) * (fx - a1) + (fy - a2) * (fy - a2) <= a3 * a3)
                                    : (fx >= a1 && fx <= a3 && fy >= a2 && fy <= a4));
            }
            int is_flash = !strcmp(id, g_flash_id) && t - g_flash_t < 400;
            int is_ex = !strcmp(id, g_ex_id) && t - g_ex_t < 300;
            /* 区域存的是竖屏坐标（与脚本一致）；画到屏上要换算。rot 90/270 时矩形两角会互换
             * → 取 min/max 归一化；圆只换算圆心，半径不变。上面 inside 判定用竖屏坐标，别动。 */
            {
                int ux1, uy1, ux2, uy2;
                p2c(a1, a2, &ux1, &uy1);
                if (type == 1) { a1 = ux1; a2 = uy1; }
                else {
                    p2c(a3, a4, &ux2, &uy2);
                    a1 = ux1 < ux2 ? ux1 : ux2; a2 = uy1 < uy2 ? uy1 : uy2;
                    a3 = ux1 < ux2 ? ux2 : ux1; a4 = uy1 < uy2 ? uy2 : uy1;
                }
            }
            ImU32 col = is_flash ? IM_COL32(0, 255, 0, 255)
                        : is_sel ? IM_COL32(255, 200, 0, 255)
                        : is_ex ? (g_ex_enter ? IM_COL32(0, 220, 255, 255) : IM_COL32(180, 180, 180, 255))
                        : inside ? IM_COL32(0, 220, 0, 255) : IM_COL32(255, 40, 40, 255);
            float lw = (is_flash || is_ex || is_sel) ? 6.0f : 3.0f;
            ImVec2 p0 = (type == 1 ? ImVec2((float)(a1 - a3), (float)(a2 - a3))
                                   : ImVec2((float)a1, (float)a2));
            ImVec2 p1 = (type == 1 ? ImVec2((float)(a1 + a3), (float)(a2 + a3))
                                   : ImVec2((float)a3, (float)a4));
            /* 「开关样式」：脚本 vt.mark(id,1) 的标记，或**开关型区域当前开着**（T3.3：kind==toggle &&
             * toggle_on==1）—— 两条来源画同一套绿样式（spec §2.3/§4.2；mark 兼容保留）。
             * 标记/开关态都存在核心共享内存里（struct region），面板直接读同一份 ⇒ 没有额外通道、没有轮询。 */
            int mk = vtouch_region_mark(i) ||
                     (vtouch_region_kind_get(i) == 1 && vtouch_region_toggle(i) == 1);
            if (mk) {
                ImU32 mkfill = IM_COL32(0, 200, 0, 56);
                if (type == 1) dl->AddCircleFilled(ImVec2((float)a1, (float)a2), (float)a3, mkfill);
                else dl->AddRectFilled(p0, p1, mkfill);
                col = IM_COL32(0, 180, 0, 255);
                lw = 8.0f;
            }
            if (is_flash) {
                ImU32 fill = IM_COL32(0, 255, 0, (int)(90 * (400 - (t - g_flash_t)) / 400));
                if (type == 1) dl->AddCircleFilled(ImVec2((float)a1, (float)a2), (float)a3, fill);
                else dl->AddRectFilled(p0, p1, fill);
            }
            if (type == 1) dl->AddCircle(ImVec2((float)a1, (float)a2), (float)a3, col, 48, lw);
            else dl->AddRect(p0, p1, col, 0, 0, lw);
            if (is_sel) {
                /* 选中：四角手柄（拖角缩放，拖内移动） */
                ImVec2 cs[4] = {p0, ImVec2(p1.x, p0.y), ImVec2(p0.x, p1.y), p1};
                if (type == 1) {
                    for (int ci = 0; ci < 4; ci++) {
                        float ang = 3.14159f * (0.25f + 0.5f * ci);
                        ImVec2 hp = ImVec2((float)a1 + (float)a3 * cosf(ang),
                                           (float)a2 + (float)a3 * sinf(ang));
                        dl->AddCircleFilled(hp, 14.0f, IM_COL32(255, 200, 0, 255));
                    }
                } else {
                    for (int ci = 0; ci < 4; ci++)
                        dl->AddRectFilled(ImVec2(cs[ci].x - 14, cs[ci].y - 14),
                                          ImVec2(cs[ci].x + 14, cs[ci].y + 14),
                                          IM_COL32(255, 200, 0, 255));
                }
            }
            {
                char lbl[24];
                snprintf(lbl, sizeof lbl, mk ? "%s \xE2\x97\x8F\xE5\xBC\x80" : "%s", id);   /* "id ●开" */
                if (mk) {
                    /* 开关型区域：名字画到**正中间**（有绿底填充后，贴边的标签读不清） */
                    ImVec2 ts = ImGui::CalcTextSize(lbl);
                    ImVec2 c = ImVec2((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f);
                    dl->AddText(ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), col, lbl);
                } else {
                    dl->AddText(ImVec2((float)a1, (float)(type == 1 ? a2 - a3 - 34 : a2 - 34)), col, lbl);
                }
            }
        }
        /* 取点捕获标记（T3.2）：~2 秒十字 + 坐标文字（x,y 十进制 = 竖屏逻辑坐标）。
         * 纯绘制（spec §4）：不参与命中、不吞触摸、不写共享内存；过期即不画（渲染循环补擦除帧）。 */
        if (g_pickmk_t && t - g_pickmk_t < PICK_MARK_MS) {
            int mx, my;
            p2c(g_pickmk_x, g_pickmk_y, &mx, &my);
            ImU32 mc = IM_COL32(255, 140, 0, 255);          /* 橙：与区域红/绿、触点蓝区分 */
            const float arm = 48.0f;
            dl->AddLine(ImVec2((float)mx - arm, (float)my), ImVec2((float)mx + arm, (float)my), mc, 6.0f);
            dl->AddLine(ImVec2((float)mx, (float)my - arm), ImVec2((float)mx, (float)my + arm), mc, 6.0f);
            char mkb[32];
            snprintf(mkb, sizeof mkb, "%d,%d", g_pickmk_x, g_pickmk_y);
            dl->AddText(ImVec2((float)mx + arm + 10.0f, (float)my - 30.0f), mc, mkb);
        }
        /* 框选橡皮筋 + 提示（手势坐标是竖屏的，画之前换算） */
        if (g_cap_mode && g_cap_slot >= 0) {
            ImU32 cc = IM_COL32(0, 220, 255, 255);
            if (g_cap_mode == 1) {
                int ax, ay, bx, by;
                p2c(g_cap_x0, g_cap_y0, &ax, &ay);
                p2c(g_cap_x1, g_cap_y1, &bx, &by);
                int xa = ax < bx ? ax : bx, xb = ax < bx ? bx : ax;
                int ya = ay < by ? ay : by, yb = ay < by ? by : ay;
                dl->AddRect(ImVec2((float)xa, (float)ya), ImVec2((float)xb, (float)yb), cc, 0, 0, 4.0f);
            } else {
                int cx1, cy1, cx2, cy2;
                p2c(g_cap_x0, g_cap_y0, &cx1, &cy1);
                p2c(g_cap_x1, g_cap_y1, &cx2, &cy2);
                int dx = cx2 - cx1, dy = cy2 - cy1;
                float r = sqrtf((float)(dx * dx + dy * dy));
                dl->AddCircle(ImVec2((float)cx1, (float)cy1), r, cc, 64, 4.0f);
                dl->AddCircleFilled(ImVec2((float)cx1, (float)cy1), 10.0f, cc);
            }
        }
        if (g_cap_mode) {
            dl->AddText(ImVec2(60, 300), IM_COL32(0, 220, 255, 255),
                        g_cap_mode == 1 ? "\xE6\x8B\x96\xE6\x8B\xBD\xE6\xA1\x86\xE9\x80\x89\xE7\x9F\xA9\xE5\xBD\xA2"
                                        : "\xE6\x8B\x96\xE6\x8B\xBD\xE5\x9C\x86\xE5\xBF\x83\xE6\x8B\x96\xE5\x8D\x8A\xE5\xBE\x84");
        }
        /* 轨迹 + 蓝点（面板内手指不画蓝点，绿点另画）。点/轨迹存竖屏坐标，画前换算 */
        ui_conf_load_once();
        if (g_show_mark) {
        for (int di = 0; di < 64; di++) {
            if (!g_dots[di].on) continue;
            int dxs, dys;
            p2c(g_dots[di].x, g_dots[di].y, &dxs, &dys);
            int in_p = in_panel((float)dxs, (float)dys);
            int cnt = g_dots[di].tn < 12 ? g_dots[di].tn : 12;
            int base = g_dots[di].tn < 12 ? 0 : g_dots[di].tn - cnt;
            for (int k = 1; k < cnt; k++) {
                int i0 = (base + k - 1) % 12, i1 = (base + k) % 12;
                int a = 200 * k / cnt;
                int lx0, ly0, lx1, ly1;
                p2c(g_dots[di].tx[i0], g_dots[di].ty[i0], &lx0, &ly0);
                p2c(g_dots[di].tx[i1], g_dots[di].ty[i1], &lx1, &ly1);
                dl->AddLine(ImVec2((float)lx0, (float)ly0), ImVec2((float)lx1, (float)ly1),
                            IM_COL32(60, 140, 255, a), 5.0f);
            }
            if (!in_p) {
                dl->AddCircleFilled(ImVec2((float)dxs, (float)dys), 16.0f,
                                    IM_COL32(60, 140, 255, 255));
                char nb[8]; snprintf(nb, sizeof nb, "%d", di);
                dl->AddText(ImVec2((float)dxs + 20, (float)dys - 16),
                            IM_COL32(60, 140, 255, 255), nb);
            }
        }
        /* 抬起扩散圈 */
        for (int ri = 0; ri < 8; ri++) {
            if (!g_rings[ri].on) continue;
            long age = t - g_rings[ri].t;
            if (age > 400) { g_rings[ri].on = 0; continue; }
            float r = 10.0f + age * 0.25f;
            int rx, ry;
            p2c(g_rings[ri].x, g_rings[ri].y, &rx, &ry);
            dl->AddCircle(ImVec2((float)rx, (float)ry), r,
                          IM_COL32(0, 200, 0, (int)(220 * (400 - age) / 400)), 32, 4.0f);
        }
        /* 面板触摸绿点 */
        if (g_mdown) {
            dl->AddCircleFilled(ImVec2(g_mx, g_my), 18.0f, IM_COL32(0, 200, 0, 255));
        }
        }   /* end if (g_show_mark)：触摸标记全在这里面 */
    }
    ImGui::End();
}

/* ---- 样式：sidebar-fixed 骨架 + bento 卡片 ----
 * 两包只搬三样：圆角（bento 的 2xl 收敛到 xl=12 卡片/窗口、lg=8 按钮输入）、
 * 取色（zinc 灰阶 + blue-500 主色 + red-600 危险）、间距（卡片 16 / 段间 12-20）；
 * 不搬动画和字重。导航激活态用左侧强调线锚点（稳定），不用整块变色。 */
static void apply_bento_style(void)
{
    ImGuiStyle &s = ImGui::GetStyle();
    s.WindowRounding = 12; s.ChildRounding = 12; s.FrameRounding = 8;
    s.GrabRounding = 8; s.PopupRounding = 12; s.ScrollbarRounding = 8;
    s.WindowPadding = ImVec2(PAD_X, PAD_Y); s.FramePadding = ImVec2(16, 12);
    s.ItemSpacing = ImVec2(12, 10); s.ItemInnerSpacing = ImVec2(12, 8);
    s.ScrollbarSize = 14;
    s.WindowBorderSize = 1; s.ChildBorderSize = 1; s.FrameBorderSize = 0;
    ImVec4 *c = s.Colors;
    c[ImGuiCol_Text] = ZINC900;
    c[ImGuiCol_TextDisabled] = ImVec4(0.443f, 0.443f, 0.482f, 1.00f);  /* zinc-500 */
    c[ImGuiCol_WindowBg] = WHITE;
    c[ImGuiCol_ChildBg] = WHITE;
    c[ImGuiCol_PopupBg] = WHITE;
    c[ImGuiCol_Border] = ZINC200;
    c[ImGuiCol_Separator] = ZINC200;
    c[ImGuiCol_TitleBg] = WHITE;
    c[ImGuiCol_TitleBgActive] = WHITE;
    c[ImGuiCol_FrameBg] = ZINC100;
    c[ImGuiCol_Button] = ZINC900;                                  /* bento 主按钮 */
    c[ImGuiCol_ButtonHovered] = ImVec4(0.247f, 0.247f, 0.275f, 1.00f);
    c[ImGuiCol_ButtonActive] = ImVec4(0.160f, 0.160f, 0.180f, 1.00f);
    c[ImGuiCol_Header] = ZINC100;
    c[ImGuiCol_HeaderHovered] = ZINC200;
    c[ImGuiCol_HeaderActive] = ZINC200;
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = ImVec4(0.827f, 0.827f, 0.851f, 1.00f);   /* zinc-300 */
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.706f, 0.706f, 0.737f, 1.00f);
}
/* 按钮 helper：主（深底）/次（浅底）/强调（蓝）/危险（红），不搬 hover 动画 */
static bool btn_dark(const char *l, ImVec2 s)
{
    ImGui::PushStyleColor(ImGuiCol_Button, ZINC900);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.247f, 0.247f, 0.275f, 1));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
    bool r = ImGui::Button(l, s);
    ImGui::PopStyleColor(3);
    return r;
}
static bool btn_light(const char *l, ImVec2 s)
{
    ImGui::PushStyleColor(ImGuiCol_Button, ZINC100);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ZINC200);
    ImGui::PushStyleColor(ImGuiCol_Text, ZINC900);
    bool r = ImGui::Button(l, s);
    ImGui::PopStyleColor(3);
    return r;
}
static bool btn_blue(const char *l, ImVec2 s)
{
    ImGui::PushStyleColor(ImGuiCol_Button, BLUE500);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.149f, 0.388f, 0.922f, 1));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
    bool r = ImGui::Button(l, s);
    ImGui::PopStyleColor(3);
    return r;
}
static bool btn_red(const char *l, ImVec2 s)
{
    ImGui::PushStyleColor(ImGuiCol_Button, RED600);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.725f, 0.106f, 0.106f, 1));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
    bool r = ImGui::Button(l, s);
    ImGui::PopStyleColor(3);
    return r;
}
/* 图标按钮：字形不能用字体（CJK 字体范围里没有几何符号，画出来是方框），自绘。
 * restore=0 → 一条横线（最小化语义 = 整窗收成一条）；restore=1 → 向上箭头（展开）。 */
static bool btn_icon(const char *id, ImVec2 size, int restore, int ghost)
{
    char lbl[32]; snprintf(lbl, sizeof lbl, "##%s", id);
    ImGui::PushStyleColor(ImGuiCol_Button,
                          ghost ? ImVec4(0, 0, 0, 0) : ZINC100);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ZINC200);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.827f, 0.827f, 0.851f, 1.00f));
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    bool r = ImGui::Button(lbl, size);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImU32 fg = ghost ? IM_COL32(113, 113, 122, 255) : ImGui::GetColorU32(ImGuiCol_Text);
    float cx = p0.x + size.x * 0.5f, cy = p0.y + size.y * 0.5f;
    if (ghost) {                          /* 小条里图标收一档，别显眼 */
        if (restore) {
            ImVec2 g[3] = { ImVec2(cx - 11, cy + 4.5f), ImVec2(cx, cy - 4.5f), ImVec2(cx + 11, cy + 4.5f) };
            dl->AddPolyline(g, 3, fg, 0, 4.0f);
        } else {
            dl->AddRectFilled(ImVec2(cx - 12, cy - 2.5f), ImVec2(cx + 12, cy + 2.5f), fg, 2.5f);
        }
        ImGui::PopStyleColor(3);
        return r;
    }
    if (restore) {
        ImVec2 pts[3] = { ImVec2(cx - 15, cy + 6), ImVec2(cx, cy - 6), ImVec2(cx + 15, cy + 6) };
        dl->AddPolyline(pts, 3, fg, 0, 5.0f);
    } else {
        dl->AddRectFilled(ImVec2(cx - 16, cy - 3), ImVec2(cx + 16, cy + 3), fg, 3.0f);
    }
    ImGui::PopStyleColor(3);
    return r;
}
/* ---- 元信息小字（bento 小层级）：30px 字体 + Disabled 色，调用方先 snprintf ---- */
static void meta_push(void)
{
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
    if (g_font_meta) ImGui::PushFont(g_font_meta);
}
static void meta_pop(void)
{
    if (g_font_meta) ImGui::PopFont();
    ImGui::PopStyleColor();
}
static void text_meta_s(const char *s)   /* 单行小字 */
{
    meta_push(); ImGui::TextUnformatted(s); meta_pop();
}
static void text_meta_w(const char *s)   /* 可折行小字（窄栏用） */
{
    meta_push(); ImGui::TextWrapped("%s", s); meta_pop();
}

/* 侧栏导航项：白底 + zinc-200 边 + 左侧 8px 蓝色强调线（激活）；文字左对齐 + 轻微 hover 底色 */
static bool nav_btn(const char *l, int active, float w)
{
    float h = 76.0f;
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::PushID(l);
    bool r = ImGui::InvisibleButton("##nav", ImVec2(w, h));
    int hov = ImGui::IsItemHovered();
    ImGui::PopID();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImU32 bg = active ? IM_COL32(255, 255, 255, 255)
             : hov ? IM_COL32(244, 244, 245, 255) : IM_COL32(250, 250, 250, 0);
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), bg, 8.0f);
    if (active) {
        dl->AddRect(p, ImVec2(p.x + w, p.y + h), IM_COL32(228, 228, 231, 255), 8.0f, 0, 1.0f);
        dl->AddRectFilled(ImVec2(p.x + 6, p.y + 12), ImVec2(p.x + 14, p.y + h - 12),
                          IM_COL32(59, 130, 246, 255), 4.0f);
    }
    ImVec2 ts = ImGui::CalcTextSize(l);
    dl->AddText(ImVec2(p.x + 30, p.y + (h - ts.y) * 0.5f),
                active ? IM_COL32(24, 24, 27, 255) : IM_COL32(82, 82, 91, 255), l);
    return r;
}
/* 容器实区发布：渲染侧每帧写，快照侧按下时读 */
static void pub_zone(volatile float *z)
{
    ImVec2 p = ImGui::GetWindowPos(), s = ImGui::GetWindowSize();
    z[0] = p.x; z[1] = p.y; z[2] = p.x + s.x; z[3] = p.y + s.y;
}
/* 手指拖拽滚动：累积量只在按下时锁定的容器里应用一次（不匹配就丢弃，避免串滚） */
static void drag_scroll_for(int target)
{
    if (g_scroll_acc == 0 || g_scr_target != target) return;
    float y0 = ImGui::GetScrollY();
    float m = ImGui::GetScrollMaxY(), v = y0 + g_scroll_acc;
    if (v > m) v = m;
    if (v < 0) v = 0;
    ImGui::SetScrollY(v);
    g_dbg_scroll = v;
    g_scroll_acc = 0;
    ALOGI("scroll apply t=%d y0=%.0f max=%.0f -> %.0f", target, y0, m, v);
}
/* 页头：左标题 + 右状态（SetCursorPosX 右贴边） */
static void page_header(const char *title, const char *meta)
{
    ImGui::TextUnformatted(title);
    if (meta && meta[0]) {
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - ImGui::CalcTextSize(meta).x);
        ImGui::TextDisabled("%s", meta);
    }
}
static int any_region_enabled(void)
{
    int n = vtouch_region_count();
    for (int i = 0; i < n; i++) {
        char id[16]; int t, a1, a2, a3, a4, en;
        if (vtouch_get_region(i, id, sizeof id, &t, &a1, &a2, &a3, &a4, &en) == 0 && en) return 1;
    }
    return 0;
}
static void del_region(const char *id)
{
    /* 只删这一条：以前是「全清 + 回填」，那一帧 core 的表为空，正按住的手指可能漏一次 up */
    if (vtouch_region_del(id) != 0) {
        ALOGW("del_region 失败（core 里没这条？）: %s", id);
        ev_note("删除失败：%s", id);
        return;
    }
    if (!strcmp(g_sel_id, id)) g_sel_id[0] = 0;
    for (int j = 0; j < g_nhide; j++) {
        if (!strcmp(g_hidden[j], id)) {
            memmove(g_hidden[j], g_hidden[j + 1], (size_t)(g_nhide - j - 1) * 16);
            g_nhide--;
            break;
        }
    }
    save_regions();
    g_force_frames = 3;
    ALOGI("del %s n=%d", id, vtouch_region_count());
}

/* 名字合法性：0 ok / 1 空 / 2 超长 / 3 非法字符 / 4 重名（old_id 自己不算） */
static int id_name_ok(const char *old_id, const char *s)
{
    int i, n = (int)strlen(s), k = vtouch_region_count();
    if (n < 1) return 1;
    if (n > 15) return 2;
    for (i = 0; i < n; i++) {
        char ch = s[i];
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') || ch == '_' || ch == '-')) return 3;
    }
    for (i = 0; i < k; i++) {
        char id[16]; int t, b1, b2, b3, b4, en;
        if (vtouch_get_region(i, id, sizeof id, &t, &b1, &b2, &b3, &b4, &en) != 0) continue;
        if (!strcmp(id, s) && strcmp(id, old_id)) return 4;
    }
    return 0;
}
/* 原地改名：core 里位置不变 ⇒ 槽位命中状态不重置 ⇒ **按住手指改名不再丢 up** */
static void rename_region(const char *old_id, const char *new_id)
{
    if (vtouch_region_rename(old_id, new_id) != 0) {
        ALOGW("rename_region 失败（重名/超长/没这条）: %s -> %s", old_id, new_id);
        ev_note("改名失败：%s", new_id);
        return;
    }
    if (!strcmp(g_sel_id, old_id)) snprintf(g_sel_id, sizeof g_sel_id, "%s", new_id);
    for (int j = 0; j < g_nhide; j++)
        if (!strcmp(g_hidden[j], old_id)) snprintf(g_hidden[j], 16, "%s", new_id);
    save_regions();
    g_force_frames = 3;
    ALOGI("rename %s -> %s n=%d", old_id, new_id, vtouch_region_count());
}

static void name_open(int i, const char *id)
{
    g_name_i = i;
    snprintf(g_name_old, sizeof g_name_old, "%s", id);
    snprintf(g_name_buf, sizeof g_name_buf, "%s", id);
    g_name_up = 0;
    g_name_msg[0] = 0;
    g_need = 1; g_force_frames = 3;
    ALOGI("name open i=%d id=%s", i, id);
}

/* ---- 标题栏：唯一拖动区（整条无控件，按住即拖窗） ---- */
static void build_titlebar(float ww)
{
    const int wide = ww > 520;            /* 宽版：标题带状态小字 */
    const int tiny = ww < 260;            /* 收起态小条(224)：点 + 计数 + 幽灵图标；窄栏(288)不算，保留标题 */
    const float bw = tiny ? 44.0f : 76.0f;
    const float th = tiny ? 44.0f : (float)TITLE_H;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, tiny ? ImVec4(0, 0, 0, 0) : WHITE);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, tiny ? ImVec2(PAD_X, 0) : ImVec2(PAD_X, PAD_Y));
    ImGui::BeginChild("##title", ImVec2(0, th), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar);
    pub_zone(g_zone_title);
    {
        ImDrawList *dl = ImGui::GetWindowDrawList();
        ImVec2 p = ImGui::GetCursorScreenPos();
        int en = any_region_enabled(), n = vtouch_region_count();
        dl->AddCircleFilled(ImVec2(p.x + 12, p.y + 22), tiny ? 9.0f : 11.0f,
                            en ? IM_COL32(34, 197, 94, 255) : IM_COL32(161, 161, 170, 255));
        ImGui::Dummy(ImVec2(tiny ? 26.0f : 30.0f, 44));
        ImGui::SameLine();
        if (wide) {
            ImGui::TextUnformatted("vtouch 面板");
        } else if (!tiny) {               /* 窄栏：标题降一档字号，给按钮让位 */
            meta_push(); ImGui::TextUnformatted("vtouch"); meta_pop();
        }
        /* 右侧：状态小字（放得下才画）+ 收起/还原按钮（永远贴右，不随宽窄消失） */
        char tb[64] = {0};
        if (g_min) snprintf(tb, sizeof tb, "%d/32", n);
        else if (wide) snprintf(tb, sizeof tb, "%s · %d/32 · %.1fms",
                                en ? "监听中" : "待命", n, g_frame_ms);
        meta_push();
        float tw = tb[0] ? ImGui::CalcTextSize(tb).x : 0;
        meta_pop();
        ImGui::SameLine();
        float bx = ImGui::GetContentRegionMax().x - bw;
        if (tb[0] && bx - 16 - tw > ImGui::GetCursorPosX() + 12) {
            meta_push();
            ImGui::SetCursorPosX(bx - 16 - tw);
            ImGui::TextUnformatted(tb);
            meta_pop();
            ImGui::SameLine();
        }
        ImGui::SetCursorPosX(bx);
        if (btn_icon(g_min ? "restore" : "collapse", ImVec2(bw, 44), g_min, tiny)) {
            g_min = !g_min;
            if (!g_min) {                  /* 展开：按新尺寸拉回屏内，别只露一角 */
                if (g_pan_x + panel_w() > (float)g_scr_w) g_pan_x = (float)g_scr_w - panel_w();
                if (g_pan_y + panel_h() > (float)g_scr_h) g_pan_y = (float)g_scr_h - panel_h();
                if (g_pan_x < 0) g_pan_x = 0;
                if (g_pan_y < 0) g_pan_y = 0;
            }
            g_need = 1; g_force_frames = 3;
            ALOGI("panel min=%d w=%.0f h=%.0f at %.0f,%.0f",
                  g_min, panel_w(), panel_h(), g_pan_x, g_pan_y);
        }
        /* 按钮不在拖动区：标题实区右边界收到按钮左侧（按按钮不会顺手拖窗） */
        g_zone_title[2] = ImGui::GetItemRectMin().x - 12;
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

/* ---- 侧栏：固定 w-64，导航分「页面 / 框选工具」两组 + 底部动作 ---- */
static void build_sidebar(void)
{
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ZINC50);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 12));
    ImGui::BeginChild("##side", ImVec2((float)SIDE_W, 0), ImGuiChildFlags_None);
    pub_zone(g_zone_side);
    drag_scroll_for(SCR_SIDE);
    {
        float bw = ImGui::GetContentRegionAvail().x;
        ImGui::TextDisabled("页面");
        if (nav_btn("区域列表", g_nav == 0 && g_sheet, bw)) { g_nav = 0; g_sheet = 1; g_need = 1; }
        if (nav_btn("操作",     g_nav == 1 && g_sheet, bw)) { g_nav = 1; g_sheet = 1; g_need = 1; }
        if (nav_btn("事件日志", g_nav == 2 && g_sheet, bw)) { g_nav = 2; g_sheet = 1; g_need = 1; }
        if (nav_btn("设置",     g_nav == 3 && g_sheet, bw)) { g_nav = 3; g_sheet = 1; g_need = 1; }
        if (nav_btn("说明",     g_nav == 4 && g_sheet, bw)) { g_nav = 4; g_sheet = 1; g_need = 1; }
        ImGui::Dummy(ImVec2(0, 8));
        ImGui::TextDisabled("框选工具");
        if (nav_btn(g_cap_mode == 1 ? "矩形 · 进行中" : "矩形框选", g_cap_mode == 1, bw)) {
            g_cap_mode = (g_cap_mode == 1) ? 0 : 1;
            g_cap_slot = -1; g_nav = 0; g_sheet = 1; g_need = 1;
        }
        if (nav_btn(g_cap_mode == 2 ? "圆形 · 进行中" : "圆形框选", g_cap_mode == 2, bw)) {
            g_cap_mode = (g_cap_mode == 2) ? 0 : 2;
            g_cap_slot = -1; g_nav = 0; g_sheet = 1; g_need = 1;
        }
        ImGui::Dummy(ImVec2(0, 4));
        text_meta_w(g_cap_mode == 1 ? "到屏上拖矩形，松手生成"
                  : g_cap_mode == 2 ? "按下点=圆心，拖动定半径"
                  : "点一项，再按住屏上拖框");
        ImGui::Dummy(ImVec2(0, 10));
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0, 6));
        if (g_ov_show) {
            if (btn_dark("显隐：显", ImVec2(bw, 76))) { g_ov_show = 0; g_force_frames = 3; }
        } else {
            if (btn_light("显隐：隐", ImVec2(bw, 76))) { g_ov_show = 1; g_force_frames = 3; }
        }
        if (g_show_mark) {
            if (btn_dark("触摸标记：显", ImVec2(bw, 76))) { g_show_mark = 0; g_need = 1; ui_conf_save(); }
        } else {
            if (btn_light("触摸标记：隐", ImVec2(bw, 76))) { g_show_mark = 1; g_need = 1; ui_conf_save(); }
        }
        if (btn_light("关闭 UI", ImVec2(bw, 76))) ui_show_cb(0);
        if (btn_red("退出", ImVec2(bw, 76))) { vtouch_cleanup(); _exit(0); }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

/* ---- 区域卡片（bento：白卡 + zinc-100 边 + xl 圆角 + 等宽四键） ---- */

/* T3.3 触发侧三行的点击处理（触发 / 时机 / 开关型）：写路径都是共享内存编辑邮箱
 * （ui_glue.c 的 vtouch_region_bind / vtouch_region_kind —— 投完等 applied + 回读校验），
 * 成功才 save_regions()（增量字段随之落盘，spec §2.8）；失败走 ev_note 提示、卡片按真值重画。 */

/* 触发行：点击循环 无 → 各操作（按操作表顺序）→ 无；悬空值下一次点击归「无」（同门控行惯例）。
 * 循环到操作时沿用当前时机（无时机 = 默认「按下」）；一个操作都没有时只提示、不动表。 */
static void rc_trig_cycle(const char *id, const char *cur_op, int cur_ev)
{
    char ops[32][16], nxt[16];
    int n = 0, k, ev, cnt = vtouch_op_count();
    for (k = 0; k < cnt && n < 32; k++) {
        char nm[16];
        if (vtouch_get_op(k, nm, sizeof nm, NULL, NULL, 0, NULL) == 0) {
            snprintf(ops[n], sizeof ops[n], "%s", nm);
            n++;
        }
    }
    ev = (cur_ev == 2) ? 2 : 1;                    /* 绑上就有意义：沿用当前时机；无时机默认「按下」 */
    if (!cur_op[0]) {
        if (n == 0) {                              /* 一个操作都没有：提示，不动表 */
            ALOGW("bind %s：没有操作可绑定", id);
            ev_note("没有操作可绑定：先到「操作」页建一条");
            return;
        }
        snprintf(nxt, sizeof nxt, "%s", ops[0]);
    } else {
        for (k = 0; k < n; k++) if (!strcmp(ops[k], cur_op)) break;
        if (k + 1 < n) snprintf(nxt, sizeof nxt, "%s", ops[k + 1]);
        else nxt[0] = 0;                           /* 到末尾（含悬空值）→ 回「无」 */
    }
    if (vtouch_region_bind(id, nxt[0] ? nxt : "-", ev) != 0) {
        ALOGW("bind %s → %s 未生效", id, nxt[0] ? nxt : "无");
        ev_note("绑定未生效：%s", id);
        g_force_frames = 2;
        return;
    }
    ALOGI("bind %s → %s ev%d", id, nxt[0] ? nxt : "无", ev);
    save_regions();
    g_force_frames = 3;
}

/* 时机行：点击在「按下(1) / 完整按压(2)」间切换（时机是绑定的属性）。未绑定点了不落任何东西 ——
 * M1 口径：核心不许出现「无绑定却有时机」（解除即清 trig_ev=0），所以无绑定就没有可切的时机。 */
static void rc_ev_cycle(const char *id, const char *cur_op, int cur_ev)
{
    int ev = (cur_ev == 2) ? 1 : 2;
    if (!cur_op[0]) return;
    if (vtouch_region_bind(id, cur_op, ev) != 0) {
        ALOGW("bind %s → %s ev%d 未生效", id, cur_op, ev);
        ev_note("时机未生效：%s", id);
        g_force_frames = 2;
        return;
    }
    ALOGI("bind %s → %s ev%d", id, cur_op, ev);
    save_regions();
    g_force_frames = 3;
}

/* 开关型行：点击切换 kind（0 普通 ↔ 1 开关型）。toggle_on 的翻转只由屏上完整按压产生
 * （核心区域线程，spec §4.2），面板只显示、不翻转 —— 行里的「当前开/关」是只读回显。 */
static void rc_kind_cycle(const char *id, int cur_kind)
{
    int kd = cur_kind ? 0 : 1;
    if (vtouch_region_kind(id, kd) != 0) {
        ALOGW("kind %s → %d 未生效", id, kd);
        ev_note("开关型切换未生效：%s", id);
        g_force_frames = 2;
        return;
    }
    ALOGI("kind %s → %d", id, kd);
    save_regions();
    g_force_frames = 3;
}

static void region_card(int i, const char *id, int type, int a1, int a2, int a3, int a4, int en)
{
    ImGui::PushID(i);
    int is_sel = g_sel_id[0] && !strcmp(id, g_sel_id);
    int hid = is_hidden(id);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, is_sel ? ImVec4(0.937f, 0.965f, 1.00f, 1.00f) : WHITE);
    ImGui::PushStyleColor(ImGuiCol_Border, is_sel ? BLUE500 : ZINC200);
    ImGui::BeginChild("card", ImVec2(0, 0), ImGuiChildFlags_Border | ImGuiChildFlags_AutoResizeY,
                      ImGuiWindowFlags_NoScrollbar);
    {
        ImDrawList *dl = ImGui::GetWindowDrawList();
        ImVec2 p = ImGui::GetCursorScreenPos();
        dl->AddCircleFilled(ImVec2(p.x + 11, p.y + 22), 11,
                            en ? IM_COL32(34, 197, 94, 255) : IM_COL32(161, 161, 170, 255));
        ImGui::Dummy(ImVec2(28, 44));
        ImGui::SameLine();
        /* id 就是脚本监听的键：点它改名（框选自动给 r1/c1…） */
        {
            char chip[40];
            snprintf(chip, sizeof chip, "%s", id);
            float cw2 = ImGui::CalcTextSize(chip).x + 52;
            if (cw2 < 130) cw2 = 130;
            if (btn_light(chip, ImVec2(cw2, 56))) name_open(i, id);
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            char head[112];
            snprintf(head, sizeof head, "%s%s%s   点名字改名", type == 1 ? "圆形" : "矩形",
                     en ? "" : " · 已停用", hid ? " · 已隐藏" : "");
            text_meta_s(head);
        }
        {
            char m[72];
            if (type == 1) snprintf(m, sizeof m, "圆心 %d,%d   半径 %d", a1, a2, a3);
            else snprintf(m, sizeof m, "%d,%d - %d,%d   %dx%d",
                          a1, a2, a3, a4, a3 - a1, a4 - a2);
            text_meta_s(m);
        }
        /* T3.3 触发侧三行（全宽，键高 76 保手指可点）：触发 / 时机 / 开关型 —— 点击处理见 rc_*_cycle。
         * 值全部实时读核心（只读区 A）：悬空绑定照显（核心允许悬空）；开关型行带当前开关态只读回显。 */
        {
            char op[16] = {0}; int ev = 0, kd = 0, tg = 0;
            char lbl[72];
            float rw = ImGui::GetContentRegionAvail().x;
            vtouch_region_trig(i, op, sizeof op, &ev);   /* 读失败保默认（无绑定/按下）—— 画得出来 */
            kd = vtouch_region_kind_get(i);
            tg = vtouch_region_toggle(i);
            snprintf(lbl, sizeof lbl, "触发：%s", op[0] ? op : "无");
            if (btn_light(lbl, ImVec2(rw, 76))) rc_trig_cycle(id, op, ev);
            snprintf(lbl, sizeof lbl, "时机：%s", ev == 2 ? "完整按压" : "按下");
            if (btn_light(lbl, ImVec2(rw, 76))) rc_ev_cycle(id, op, ev);
            if (kd == 1) snprintf(lbl, sizeof lbl, "开关型：开 · 当前%s", tg == 1 ? "开" : "关");
            else snprintf(lbl, sizeof lbl, "开关型：关");
            if (btn_light(lbl, ImVec2(rw, 76))) rc_kind_cycle(id, kd);
        }
        /* 等宽四键（gap 12，贴合 bento 一致间隙；键高 76 保手指可点） */
        float bw = (ImGui::GetContentRegionAvail().x - 3 * 12) * 0.25f;
        if (en) {
            if (btn_light("停用", ImVec2(bw, 76))) {
                ALOGI("click row %d %s -> 0", i, id);
                vtouch_region_add(id, type, a1, a2, a3, a4, 0);
                save_regions(); g_force_frames = 3;
            }
        } else {
            if (btn_dark("启用", ImVec2(bw, 76))) {
                ALOGI("click row %d %s -> 1", i, id);
                vtouch_region_add(id, type, a1, a2, a3, a4, 1);
                save_regions(); g_force_frames = 3;
            }
        }
        ImGui::SameLine();
        if (is_sel) {
            if (btn_blue("取消", ImVec2(bw, 76))) { g_sel_id[0] = 0; g_need = 1; ALOGI("select -"); }
        } else {
            if (btn_light("选中", ImVec2(bw, 76))) {
                snprintf(g_sel_id, sizeof g_sel_id, "%s", id);
                g_need = 1; ALOGI("select %s", g_sel_id);
            }
        }
        ImGui::SameLine();
        if (btn_light(hid ? "显示" : "隐藏", ImVec2(bw, 76))) { hide_toggle(id); g_need = 1; }
        ImGui::SameLine();
        if (btn_red("删除", ImVec2(bw, 76))) del_region(id);
        if (is_sel) text_meta_w("选中：框内拖动移动，四角黄块缩放");
    }
    ImGui::EndChild();
    ImGui::PopStyleColor(2);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(12, 12));
    ImGui::Dummy(ImVec2(0, 0));   /* bento 卡片间隙 gap-4 */
    ImGui::PopStyleVar();
    ImGui::PopID();
}

static void page_regions(void)
{
    int n = vtouch_region_count();
    char meta[48];
    snprintf(meta, sizeof meta, "%d/32 区域", n);
    page_header("区域列表", meta);
    if (g_cap_mode)
        text_meta_w(g_cap_mode == 1 ? "到屏上拖矩形，松手生成" : "按下点=圆心，拖动定半径");
    else if (n == 0)
        text_meta_w("侧栏点「矩形框选 / 圆形框选」，再按住屏上拖框");
    else
        text_meta_w("上下拖动列表滚动 · 卡片四键管理");
    ImGui::Dummy(ImVec2(0, 6));
    /* 列表自成一格可滚容器（区域多时正常滚动；条始终可见以示可滚） */
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ZINC50);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 12));
    ImGui::BeginChild("##regions", ImVec2(0, 0), ImGuiChildFlags_None,
                      ImGuiWindowFlags_AlwaysVerticalScrollbar);
    pub_zone(g_zone_list);
    drag_scroll_for(SCR_LIST);
    {
        if (n == 0) ImGui::TextDisabled("还没有区域");
        for (int i = 0; i < n; i++) {
            if (i == 0) g_card0_y = ImGui::GetCursorScreenPos().y;
            char id[16]; int type, a1, a2, a3, a4, en;
            if (vtouch_get_region(i, id, sizeof id, &type, &a1, &a2, &a3, &a4, &en) != 0) continue;
            region_card(i, id, type, a1, a2, a3, a4, en);
        }
    }
    g_list_scroll = ImGui::GetScrollY();
    g_list_max = ImGui::GetScrollMaxY();
    g_list_top = ImGui::GetWindowPos().y;
    g_list_left = ImGui::GetWindowPos().x;
    g_list_h = ImGui::GetWindowSize().y;
    g_list_w = ImGui::GetWindowSize().x;
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}
/* ---- 操作页（T2.5）：卡片列表 + 运行/停止/新建/删除；数据全部实时读核心（只读区 A）----
 * 运行状态由渲染线程每拍看一眼（ops_run_watch）：一变就请求重画 —— 不然面板静止不重绘，
 * 「运行中 · 第 k/n 步」会永远停在按下那一刻的读数上。 */

/* 步骤类型 / 条件行为 / 变量编码：与核心常量（src/vt_internal.h）同值；面板不 include 核心头，独立定义。 */
#define OP_STEP_TAP   1
#define OP_STEP_SWIPE 2
#define OP_STEP_WAIT  3
#define OP_STEP_DOWN        4          /* 按下（按下并保持） */
#define OP_STEP_UP          5          /* 弹起（松开当前按住的手指） */
#define OP_STEP_COND_REGION 6          /* 区域判断（a1,a2 的点 ∈ ref 区域） */
#define OP_STEP_COND_TOGGLE 7          /* 开关判断（ref 区域须开关型且开着） */
#define OP_COND_ABORT       0          /* 不成立行为：中止（默认） */
#define OP_COND_SKIP        1          /* 不成立行为：跳过下一步 */
#define OP_VAR_TDX (-1)                /* 变量编码：-1..-5 = 触发按下x / 触发按下y / 触发弹起x / 触发弹起y / 触发时长 */
#define OP_VAR_TDY (-2)
#define OP_VAR_TUX (-3)
#define OP_VAR_TUY (-4)
#define OP_VAR_TMS (-5)

/* 操作运行状态看门（渲染线程每拍调）：op_status 的（运行下标, 当前步, 状态）一变就请求重画。 */
static void ops_run_watch(void)
{
    static int last = -1;
    int ri = -1, step = 0, state = 0, sig;
    if (vtouch_op_status(&ri, &step, &state) != 0) return;   /* 没接共享内存（real 模式下没接上时）：无事可做 */
    sig = state ? (ri + 1) * 1024 + step : -1;               /* 加 1 保非负；步数远小于 1024 */
    if (sig == last) return;
    last = sig;
    g_need = 1;
}

/* 默认名：opN，N = 第一个没被占用的序号（与 gen_id 同款逻辑：从 1 起扫，99 兜底）。 */
static void gen_op_name(char *out)
{
    int k, n = vtouch_op_count();
    for (k = 1; k < 100; k++) {
        char tmp[16];
        int used = 0;
        snprintf(tmp, sizeof tmp, "op%d", k);
        for (int i = 0; i < n; i++) {
            char nm[16];
            if (vtouch_get_op(i, nm, sizeof nm, NULL, NULL, 0, NULL) == 0 && !strcmp(nm, tmp)) {
                used = 1;
                break;
            }
        }
        if (!used) { snprintf(out, 16, "%s", tmp); return; }
    }
    snprintf(out, 16, "op99");
}

/* ＋新建：默认 1 步「等待 100ms」—— 安全中性、核心必过（点按/滑动要坐标，面板此刻没有可用的
 * 默认值；等待既不碰坐标也不占虚拟槽），参数交给编辑层（T2.6）再改。 */
static void op_new(void)
{
    const int wait100[6] = { OP_STEP_WAIT, 0, 0, 0, 0, 100 };
    char nm[16];
    gen_op_name(nm);
    if (vtouch_op_put(nm, "", 0, wait100, NULL, 1) == 0) {   /* refs=NULL：无区域引用（快速路径） */
        ALOGI("op new %s", nm);
        g_ops_save_pending = 1;      /* 表变了 → 渲染线程那一拍写 ops.conf（T2.7） */
        g_force_frames = 3;
    } else {
        ALOGW("op new 失败 %s（核心拒或编辑超时）", nm);
        ev_note("新建操作失败：%s", nm);
    }
}

/* 删除（与区域卡片同款：立即删，失败进事件日志）。 */
static void op_del_by_name(const char *name)
{
    if (vtouch_op_del(name) != 0) {
        ALOGW("op del 失败 %s", name);
        ev_note("删除操作失败：%s", name);
        return;
    }
    ALOGI("op del %s n=%d", name, vtouch_op_count());
    g_ops_save_pending = 1;          /* 表变了 → 渲染线程那一拍写 ops.conf（T2.7） */
    g_force_frames = 3;
}

/* 打开操作编辑覆盖层（T2.6：快照 + 校验；定义在 page_ops 之后，op_card 的 [编辑] 先用）。 */
static void op_edit_open(int i, const char *name);

/* 一张操作卡片：名字 + 元信息行（步数/门控/自动关）+ 运行中行 + 等宽三键。
 * run_i/run_step/run_state 是页头本帧读好传下来的 op_status 读数。 */
static void op_card(int i, const char *name, int steps, const char *gate, int autoff,
                    int run_i, int run_step, int run_state)
{
    int is_run = run_state && run_i == i;      /* 同时至多一条在跑（核心对忙的起跑直接丢弃） */
    ImGui::PushID(1000 + i);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, is_run ? ImVec4(0.937f, 0.965f, 1.00f, 1.00f) : WHITE);
    ImGui::PushStyleColor(ImGuiCol_Border, is_run ? BLUE500 : ZINC200);
    ImGui::BeginChild("card", ImVec2(0, 0), ImGuiChildFlags_Border | ImGuiChildFlags_AutoResizeY,
                      ImGuiWindowFlags_NoScrollbar);
    {
        ImDrawList *dl = ImGui::GetWindowDrawList();
        ImVec2 p = ImGui::GetCursorScreenPos();
        dl->AddCircleFilled(ImVec2(p.x + 11, p.y + 22), 11,
                            is_run ? IM_COL32(34, 197, 94, 255) : IM_COL32(161, 161, 170, 255));
        ImGui::Dummy(ImVec2(28, 44));
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(name);
        {
            char m[96];
            snprintf(m, sizeof m, "%d 步 · 门控 %s · 跑完自动关 %s",
                     steps, gate[0] ? gate : "无", autoff ? "开" : "关");
            text_meta_s(m);
        }
        if (is_run) {                          /* 运行态实时读核心：第 k/n 步 */
            char m[48];
            int k = run_step + 1;              /* 核心的 op_run_step 是 0 起，显示按人话 1 起 */
            if (k < 1) k = 1;
            if (k > steps) k = steps;          /* 显示层夹住：运行中删表等竞态也不画出「第 5/3 步」 */
            snprintf(m, sizeof m, "运行中 · 第 %d/%d 步", k, steps);
            text_meta_s(m);
        }
        /* 等宽三键（gap 12，键高 76 保手指可点）：运行/停止 · 编辑 · 删除 */
        float bw = (ImGui::GetContentRegionAvail().x - 2 * 12) / 3.0f;
        if (is_run) {
            if (btn_dark("停止", ImVec2(bw, 76))) {
                ALOGI("op stop %s", name);
                vtouch_op_stop();
                g_force_frames = 3;
            }
        } else {
            if (btn_blue("运行", ImVec2(bw, 76))) {
                if (vtouch_op_run(name) != 0) {   /* 非 0 = 没送达（超时/没接共享内存）→ 可见反馈 */
                    ALOGW("op run 请求未送达 %s", name);
                    ev_note("运行请求未送达：%s", name);
                } else ALOGI("op run %s", name);
                g_force_frames = 3;
            }
        }
        ImGui::SameLine();
        if (btn_light("编辑", ImVec2(bw, 76))) op_edit_open(i, name);   /* 编辑覆盖层（T2.6） */
        ImGui::SameLine();
        if (btn_red("删除", ImVec2(bw, 76))) op_del_by_name(name);
    }
    ImGui::EndChild();
    ImGui::PopStyleColor(2);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(12, 12));
    ImGui::Dummy(ImVec2(0, 0));   /* bento 卡片间隙 gap-4 */
    ImGui::PopStyleVar();
    ImGui::PopID();
}

static void page_ops(void)
{
    int n = vtouch_op_count();
    int run_i = -1, run_step = 0, run_state = 0;
    char meta[40];
    vtouch_op_status(&run_i, &run_step, &run_state);   /* 本帧读数：页头与卡片共用同一份 */
    snprintf(meta, sizeof meta, "%d 条", n);
    page_header("操作", meta);
    if (run_state) {
        char rn[16] = {0}, m[64];
        int rsteps = 0, k = run_step + 1;
        if (run_i >= 0 && vtouch_get_op(run_i, rn, sizeof rn, &rsteps, NULL, 0, NULL) == 0 && rn[0]) {
            if (k < 1) k = 1;
            if (rsteps < 1) rsteps = 1;            /* 显示层兜底：不为它画「第 1/0 步」（核心校验保证 ≥1） */
            if (k > rsteps) k = rsteps;            /* 上夹界：与卡片「第 k/n 步」同口径 */
            snprintf(m, sizeof m, "运行中：%s · 第 %d/%d 步", rn, k, rsteps);
        } else {
            snprintf(m, sizeof m, "运行中");     /* 运行中删表等竞态：名字读不到就只报状态 */
        }
        text_meta_s(m);
    } else {
        text_meta_s("空闲");
    }
    ImGui::Dummy(ImVec2(0, 4));
    if (btn_blue("＋新建", ImVec2(260, 84))) op_new();
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    text_meta_s("默认 1 步「等待 100ms」");
    ImGui::Dummy(ImVec2(0, 6));
    if (n == 0) {
        text_meta_w("还没有操作：点「＋新建」加一条");
        return;
    }
    /* 列表自成一格可滚容器（与区域列表同款；拖动滚动目标同走 SCR_LIST —— 谁在显谁发布实区） */
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ZINC50);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 12));
    ImGui::BeginChild("##ops", ImVec2(0, 0), ImGuiChildFlags_None,
                      ImGuiWindowFlags_AlwaysVerticalScrollbar);
    pub_zone(g_zone_list);
    drag_scroll_for(SCR_LIST);
    {
        for (int i = 0; i < n; i++) {
            char name[16], gate[16];
            int steps = 0, autoff = 0;
            if (vtouch_get_op(i, name, sizeof name, &steps, gate, sizeof gate, &autoff) != 0) continue;
            op_card(i, name, steps, gate, autoff, run_i, run_step, run_state);
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

/* ==== T2.6：操作编辑覆盖层 + 数字弹层 ================================================
 * 编辑对象是**打开那一刻的快照**（面板本地副本）：改名/增删排序步骤/换参数都只动副本，
 * [完成] 才由 vtouch_op_put 整条落表 —— 编辑半途核心表怎么动都不会把这轮编辑甩掉。
 * 范围口径照核心单点校验（src/vt_ops.c 的 op_valid）：名字 1..15 [A-Za-z0-9_-]（裸 `-` 除外）；步数 1..32；
 * 坐标 0..竖屏逻辑宽高-1；时长 点按 0..60000 / 滑动 1..60000 / 等待 0..600000。
 * 面板预检是**硬门**：非法值拒收 + 就地提示（不只提示）；最终仍以核心为准（put 回读失败
 * 就地提示、覆盖层不关）。 */

#define OPE_MAX_STEPS 32                /* = 核心 MAX_STEPS（面板不 include 核心头，独立定义） */

static int  g_ope_i = -1;               /* 正在编辑的操作下标（-1 = 覆盖层关；build_panel 的判据） */
static char g_ope_orig[16] = {0};       /* 打开时的原名（改过名后 [完成] 照它删旧条目） */
static char g_ope_name[16] = {0};       /* 编辑中的名字（名字子层改，[完成] 才落表） */
static char g_ope_gate[16] = {0};       /* 门控区域 id（空 = 无） */
static int  g_ope_autoff = 0;           /* 跑完自动关 */
static int  g_ope_nsteps = 0;           /* 步骤数（本地副本 1..32） */
static int  g_ope_steps[OPE_MAX_STEPS][6];   /* 本地副本：每步 type,a1,a2,a3,a4,ms */
static char g_ope_refs[OPE_MAX_STEPS][REGION_ID_MAX + 1];   /* 本地副本：每步区域引用（条件步 ref；空 = 无） */
static char g_ope_msg[128] = {0};       /* [完成] 拒收/失败的就地提示（红字；放得下 v2 长文案） */
static int  g_ope_kb = 0;               /* 名字子层（字符键盘）开 */
static char g_ope_kbmsg[72] = {0};      /* 名字子层里的拒收提示 */
static int  g_ope_up = 0;               /* 名字子层大小写档 */
static int  g_ope_se = -1;              /* 数字弹层：正在编第几步（-1 = 关） */
static int  g_ope_sf = 0;               /* 数字弹层：第几个字段（0 起） */
static int  g_ope_vl = 0;               /* 变量选择弹层开（数字弹层的 [变量]；1 = 开） */
static int  g_ope_rl = -1;              /* 区域选择弹层：正在选第几步的 ref（-1 = 关） */
static int  g_ne_lit = 1;               /* 数字弹层模式：1 = 编字面值（显示输入缓冲）；0 = 变量已选中（显示中文名） */
static char g_ope_saved_as[16] = {0};   /* 本会话最近一次 put 成功的名字（[完成] 重试豁免自己刚写进表的名字）；开层/关层清空 */
static char g_ope_del_owed[16] = {0};   /* 尚欠删除的旧名（del 超时/未送达留下的账，再点 [完成] 先补删）；开层/关层清空 */
static char g_ne_buf[8] = {0};          /* 数字弹层输入缓冲（最多 6 位数字；坐标/毫秒共用） */
static char g_ne_msg[72] = {0};         /* 数字弹层就地提示（范围/位数的硬门反馈） */

/* 自绘字符键盘弹层（区域改名 / 操作改名共用：面板收不到系统输入法，字符全靠点）。
 * 画在面板窗内并盖住侧栏与内容页（那两块本帧干脆不画，免得底下按钮还能吃点击）：
 * 标题 + 当前值框（带原名对照）+ 6x6 字符键 + 特殊键（_ - 大小写 退格）+ 取消/确定。
 * buf 上限 15 字符、up 是大写档；本函数只画/只改，语义留给调用方：
 * 返回 0 = 无动作 / 1 = 取消 / 2 = 确定。 */
static int draw_char_kb(const char *title, const char *oldname, char *buf, int bufc,
                        int *up, char *msg, int msgc)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 wp = ImGui::GetWindowPos();
    float ww = ImGui::GetWindowWidth(), wh = ImGui::GetWindowHeight();
    ImVec2 a(wp.x + 12, wp.y + (float)TITLE_H + 10), b(wp.x + ww - 12, wp.y + wh - 12);
    int act = 0;
    int cap = bufc - 1;
    if (cap > 15) cap = 15;
    dl->AddRectFilled(a, b, IM_COL32(255, 255, 255, 253), 14);
    dl->AddRect(a, b, IM_COL32(228, 228, 231, 255), 14, 0, 1.5f);
    float x0 = a.x + 26, y0 = a.y + 24, cw = (b.x - x0) - 26;
    ImGui::SetCursorScreenPos(ImVec2(x0, y0));
    text_meta_s(title);
    /* 当前输入 + 原名对照 */
    ImGui::PushStyleColor(ImGuiCol_Border, BLUE500);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.976f, 0.980f, 0.984f, 1.00f));
    ImGui::BeginChild("##nameval", ImVec2(cw, 96), ImGuiChildFlags_Border, ImGuiWindowFlags_NoScrollbar);
    {
        char show[48];
        snprintf(show, sizeof show, "%s", buf[0] ? buf : "(空)");
        ImVec2 tp = ImGui::GetCursorScreenPos();
        ImDrawList *d2 = ImGui::GetWindowDrawList();
        d2->AddText(ImVec2(tp.x + 18, tp.y + 28), IM_COL32(24, 24, 27, 255), show);
        if (oldname && oldname[0] && strcmp(oldname, buf)) {
            char old[48];
            snprintf(old, sizeof old, "原名 %s", oldname);
            d2->AddText(ImVec2(tp.x + 22 + ImGui::CalcTextSize(show).x + 26, tp.y + 34),
                        IM_COL32(161, 161, 170, 255), old);
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor(2);
    if (msg[0]) {
        ImGui::SetCursorScreenPos(ImVec2(x0, y0 + 146));
        ImGui::TextColored(ImVec4(0.863f, 0.149f, 0.149f, 1.00f), "%s", msg);
    }
    /* 字符键：6 列 x 6 行 = a-z + 0-9 */
    static const char *krow[6] = { "abcdef", "ghijkl", "mnopqr", "stuvwx", "yz0123", "456789" };
    float gap = 10.0f, kw = (cw - 5 * gap) / 6.0f, kh = 76.0f;
    float ky = y0 + 196;
    for (int r = 0; r < 6; r++) {
        for (int c = 0; c < 6; c++) {
            char lab[2] = { krow[r][c], 0 };
            if (lab[0] >= 'a' && lab[0] <= 'z' && *up) lab[0] = (char)(lab[0] - 'a' + 'A');
            ImGui::PushID(100 + r * 6 + c);
            ImGui::SetCursorScreenPos(ImVec2(x0 + c * (kw + gap), ky + r * (kh + gap)));
            if (btn_light(lab, ImVec2(kw, kh))) {
                int n = (int)strlen(buf);
                if (n < cap) { buf[n] = lab[0]; buf[n + 1] = 0; }
                else snprintf(msg, (size_t)msgc, "最多 15 个字符");
                g_need = 1; g_force_frames = 2;
            }
            ImGui::PopID();
        }
    }
    /* 特殊键：_ - 大小写 退格 */
    float fy = ky + 6 * (kh + gap);
    {
        float sw2 = (cw - 3 * gap) / 4.0f;
        const char *sp[4] = { "_", "-", *up ? "小写" : "大写", "退格" };
        for (int c = 0; c < 4; c++) {
            ImGui::PushID(200 + c);
            ImGui::SetCursorScreenPos(ImVec2(x0 + c * (sw2 + gap), fy));
            if (btn_light(sp[c], ImVec2(sw2, 76))) {
                if (c == 0 || c == 1) {
                    int n = (int)strlen(buf);
                    if (n < cap) { buf[n] = sp[c][0]; buf[n + 1] = 0; }
                } else if (c == 2) {
                    *up = !*up;
                } else {
                    int n = (int)strlen(buf);
                    if (n > 0) buf[n - 1] = 0;
                }
                msg[0] = 0;
                g_need = 1; g_force_frames = 2;
            }
            ImGui::PopID();
        }
    }
    /* 取消 / 确定 */
    float by = fy + 76 + gap + 12, bw2 = (cw - gap) * 0.5f;
    ImGui::PushID(300);
    ImGui::SetCursorScreenPos(ImVec2(x0, by));
    if (btn_light("取消", ImVec2(bw2, 92))) act = 1;
    ImGui::SetCursorScreenPos(ImVec2(x0 + bw2 + gap, by));
    if (btn_blue("确定", ImVec2(bw2, 92))) act = 2;
    ImGui::PopID();
    return act;
}

/* 操作名合法性（照核心 vt_id_ok：1..15、[A-Za-z0-9_-]，裸 `-` 除外；与区域改名同一把尺子，撞的是别的操作名）：
 * 0 ok / 1 空 / 2 超长 / 3 非法字符 / 4 重名（old_name 自己不算；本会话刚 put 成功的名字 saved_as
 * 也不算 —— 删旧失败的 [完成] 重试必须能过这一关，否则表里刚写进去的新名会把自己判成「被别人用了」）。 */
static int ope_name_ok(const char *old_name, const char *s)
{
    int i, n = (int)strlen(s), k = vtouch_op_count();
    if (n < 1) return 1;
    if (n > 15) return 2;
    for (i = 0; i < n; i++) {
        char ch = s[i];
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') || ch == '_' || ch == '-')) return 3;
    }
    for (i = 0; i < k; i++) {
        char nm[16];
        if (vtouch_get_op(i, nm, sizeof nm, NULL, NULL, 0, NULL) != 0) continue;
        if (!strcmp(nm, s) && strcmp(nm, old_name) && strcmp(nm, g_ope_saved_as)) return 4;
    }
    return 0;
}
static const char *ope_name_why(int rc)
{
    return rc == 1 ? "名字不能为空" : rc == 2 ? "最多 15 个字符" :
           rc == 3 ? "只能用 a-z A-Z 0-9 _ -" : "这个名字已经被别的操作用了";
}

/* 字段模型（v2 七类型）：字段序号 → steps6 下标（0=type、1..4=坐标参数、5=ms）；-1 = 该类型没这个字段。
 * 坐标字段（下标 1..4）在数字弹层里会多画一个 [取点]（T2.8 已接线：核心吞一次触摸回填）；
 * 可变量的字段（spec §2.1）多画一个 [变量]（ope_var_ok 判）。条件步的 a3/ref 不在字段表里 ——
 * 它们是步骤行上的行内控件（不成立行为两键 + 区域下拉）。 */
static const int ope_fidx[7][5] = {
    { 1, 2, 5, -1, -1 },       /* 点按：x, y, 按住 ms */
    { 1, 2, 3, 4, 5 },         /* 滑动：起点 x, 起点 y, 终点 x, 终点 y, 时长 ms */
    { 5, -1, -1, -1, -1 },     /* 等待：ms */
    { 1, 2, -1, -1, -1 },      /* 按下：x, y */
    { -1, -1, -1, -1, -1 },    /* 弹起：无字段 */
    { 1, 2, -1, -1, -1 },      /* 区域判断：判定点 x, 判定点 y */
    { -1, -1, -1, -1, -1 },    /* 开关判断：无数字字段 */
};
static const char *const ope_flabel[7][5] = {
    { "坐标 x", "坐标 y", "按住时长 ms", "", "" },
    { "起点 x", "起点 y", "终点 x", "终点 y", "滑动时长 ms" },
    { "等待时长 ms", "", "", "", "" },
    { "坐标 x", "坐标 y", "", "", "" },
    { "", "", "", "", "" },
    { "判定点 x", "判定点 y", "", "", "" },
    { "", "", "", "", "" },
};
static int ope_nfields(int type)
{
    return type == OP_STEP_TAP ? 3 : type == OP_STEP_SWIPE ? 5 : type == OP_STEP_WAIT ? 1 :
           type == OP_STEP_DOWN ? 2 : type == OP_STEP_COND_REGION ? 2 : 0;
}
static const char *ope_tname(int type)
{
    switch (type) {
    case OP_STEP_TAP:         return "点按";
    case OP_STEP_SWIPE:       return "滑动";
    case OP_STEP_WAIT:        return "等待";
    case OP_STEP_DOWN:        return "按下";
    case OP_STEP_UP:          return "弹起";
    case OP_STEP_COND_REGION: return "区域判断";
    case OP_STEP_COND_TOGGLE: return "开关判断";
    default:                  return "?";
    }
}

/* 变量中文名（spec §1.1，逐字）：下标 0..4 ↔ 值 -1..-5（OP_VAR_TDX..OP_VAR_TMS）。 */
static const char *const ope_vname_tab[5] = { "触发按下x", "触发按下y", "触发弹起x", "触发弹起y", "触发时长" };
static const char *ope_vname(int v)      /* 值是变量引用 → 中文名；不是 → NULL */
{
    if (v >= OP_VAR_TMS && v <= OP_VAR_TDX) return ope_vname_tab[OP_VAR_TDX - v];
    return NULL;
}
/* 数值字段显示文本：变量 → 中文名；字面值 → 数字。 */
static void ope_num_text(int v, char *out, int outcap)
{
    const char *n = ope_vname(v);
    if (n) snprintf(out, (size_t)outcap, "%s", n);
    else snprintf(out, (size_t)outcap, "%d", v);
}
/* 该字段允不允许变量引用（spec §2.1）：点按/滑动/等待的时长；点按/滑动/按下/区域判断的坐标。
 * v2 里凡存在的数值字段都可变量 —— 仍按类型/下标写死，防以后加字段时悄悄放行。 */
static int ope_var_ok(int type, int idx)
{
    if (idx == 5) return type == OP_STEP_TAP || type == OP_STEP_SWIPE || type == OP_STEP_WAIT;
    if (idx >= 1 && idx <= 4)
        return type == OP_STEP_TAP || type == OP_STEP_SWIPE || type == OP_STEP_DOWN || type == OP_STEP_COND_REGION;
    return 0;
}

/* 参数字段的范围硬门（照核心 op_valid 的尺子）：1 = 合法；不然 why 写人话（字段名 + 范围）。
 * v2：数值字段可为变量引用（-1..-5），放行；字面值照旧按类型/坐标轴分档。 */
static int ne_check(const char *label, int type, int fi, int v, char *why, int whycap)
{
    int idx;
    if (type < OP_STEP_TAP || type > OP_STEP_COND_TOGGLE || fi < 0 || fi >= ope_nfields(type)) {
        snprintf(why, (size_t)whycap, "步骤类型非法");
        return 0;
    }
    idx = ope_fidx[type - 1][fi];
    if (ope_vname(v)) return 1;                       /* -1..-5：变量引用（v2 数值字段全可变量） */
    if (idx >= 1 && idx <= 4) {                       /* 坐标字段：x 看逻辑宽、y 看逻辑高 */
        int lim = (idx == 1 || idx == 3) ? g_w : g_h;
        if (v < 0 || v >= lim) { snprintf(why, (size_t)whycap, "%s 必须在 0..%d", label, lim - 1); return 0; }
        return 1;
    }
    if (type == OP_STEP_TAP) {
        if (v < 0 || v > 60000) { snprintf(why, (size_t)whycap, "%s 必须在 0..60000", label); return 0; }
    } else if (type == OP_STEP_SWIPE) {
        if (v < 1 || v > 60000) { snprintf(why, (size_t)whycap, "%s 必须在 1..60000", label); return 0; }
    } else {
        if (v < 0 || v > 600000) { snprintf(why, (size_t)whycap, "%s 必须在 0..600000", label); return 0; }
    }
    return 1;
}

/* 整步校验（[完成] 预检用；单点仍是核心 op_valid，这里只是不让明显非法的载荷出门）。
 * 条件步：不成立行为 a3 ∈ {0,1}、ref 必须已选（缺区域 = 明显非法，面板先拦）。 */
static int ope_step_check(int si, const int *s6, char *why, int whycap)
{
    int t = s6[0], nf, fi;
    if (t < OP_STEP_TAP || t > OP_STEP_COND_TOGGLE) {
        snprintf(why, (size_t)whycap, "第 %d 步类型非法", si + 1);
        return 0;
    }
    nf = ope_nfields(t);
    for (fi = 0; fi < nf; fi++) {
        char f[72];
        if (!ne_check(ope_flabel[t - 1][fi], t, fi, s6[ope_fidx[t - 1][fi]], f, (int)sizeof f)) {
            snprintf(why, (size_t)whycap, "第 %d 步：%s", si + 1, f);
            return 0;
        }
    }
    if (t == OP_STEP_COND_REGION || t == OP_STEP_COND_TOGGLE) {
        if (s6[3] != OP_COND_ABORT && s6[3] != OP_COND_SKIP) {
            snprintf(why, (size_t)whycap, "第 %d 步：不成立行为非法", si + 1);
            return 0;
        }
        if (!g_ope_refs[si][0]) {
            snprintf(why, (size_t)whycap, "第 %d 步：请选择区域", si + 1);
            return 0;
        }
    }
    return 1;
}

/* 步骤行参数文本（T3.1 摘要）：坐标/时长格显示变量中文名或原值；条件步带区域名与不成立行为。 */
static void ope_step_text(int si, char *out, int outcap)
{
    const int *s6 = g_ope_steps[si];
    const char *ref = g_ope_refs[si];
    char x1[24], y1[24], x2[24], y2[24], ms[24];
    switch (s6[0]) {
    case OP_STEP_TAP:
        ope_num_text(s6[1], x1, (int)sizeof x1); ope_num_text(s6[2], y1, (int)sizeof y1);
        ope_num_text(s6[5], ms, (int)sizeof ms);
        snprintf(out, (size_t)outcap, "%s,%s · 按住 %sms", x1, y1, ms);
        break;
    case OP_STEP_SWIPE:
        ope_num_text(s6[1], x1, (int)sizeof x1); ope_num_text(s6[2], y1, (int)sizeof y1);
        ope_num_text(s6[3], x2, (int)sizeof x2); ope_num_text(s6[4], y2, (int)sizeof y2);
        ope_num_text(s6[5], ms, (int)sizeof ms);
        snprintf(out, (size_t)outcap, "%s,%s → %s,%s · %sms", x1, y1, x2, y2, ms);
        break;
    case OP_STEP_WAIT:
        ope_num_text(s6[5], ms, (int)sizeof ms);
        snprintf(out, (size_t)outcap, "%sms", ms);
        break;
    case OP_STEP_DOWN:
        ope_num_text(s6[1], x1, (int)sizeof x1); ope_num_text(s6[2], y1, (int)sizeof y1);
        snprintf(out, (size_t)outcap, "%s,%s", x1, y1);
        break;
    case OP_STEP_UP:
        snprintf(out, (size_t)outcap, "松开");
        break;
    case OP_STEP_COND_REGION:
        ope_num_text(s6[1], x1, (int)sizeof x1); ope_num_text(s6[2], y1, (int)sizeof y1);
        snprintf(out, (size_t)outcap, "%s,%s · %s · %s", x1, y1,
                 ref[0] ? ref : "未选区域", s6[3] == OP_COND_SKIP ? "跳过下一步" : "中止");
        break;
    case OP_STEP_COND_TOGGLE:
        snprintf(out, (size_t)outcap, "%s · %s",
                 ref[0] ? ref : "未选区域", s6[3] == OP_COND_SKIP ? "跳过下一步" : "中止");
        break;
    default:
        snprintf(out, (size_t)outcap, "类型非法（%d）", s6[0]);
        break;
    }
}

/* 数字弹层：把当前字段的现值装进输入缓冲（变量引用 → 缓冲留空、值框显示中文名）。类型/字段下标
 * 非法 → 不装值、直接关层（防 ope_fidx 越界读；口径同 draw_num_edit 的同一道判据）。 */
static void ope_num_load(void)
{
    int type, nf, idx, v;
    g_ne_buf[0] = 0;
    g_ne_lit = 1;
    if (g_ope_se < 0 || g_ope_se >= g_ope_nsteps) { g_ope_se = -1; return; }
    type = g_ope_steps[g_ope_se][0];
    nf = ope_nfields(type);
    if (nf == 0 || g_ope_sf < 0 || g_ope_sf >= nf) { g_ope_se = -1; return; }
    idx = ope_fidx[type - 1][g_ope_sf];
    if (idx >= 0) {
        v = g_ope_steps[g_ope_se][idx];
        if (ope_vname(v)) g_ne_lit = 0;              /* 变量引用：值框显示中文名（缓冲留空） */
        else snprintf(g_ne_buf, sizeof g_ne_buf, "%d", v);
    }
}

/* 取点结果落点（T2.8；ui_ev_cb 从 poll 线程转发，见其上方的前向声明）：
 * 回填到发起取点的那一格坐标字段（x 格收 x、y 格收 y；滑动四点同理）+ 退取点态 + 重画。
 * 值同时写进 g_ne_buf（值框同显；副本要 [确定] 才落 —— 不写 buf 的话下一次 [确定] 会拿旧值回写）。
 * T3.1：取到的是字面值 —— 顺带切回字面模式（g_ne_lit=1），免得值框还显示中文名。 */
static void pick_ev_apply(int px, int py)
{
    /* 捕获标记（T3.2）：每次取点成功记下捕获点 + 时刻（后一次重置过期）——绘制在渲染线程
     * （build_overlay），这里只存；纯绘制，与回填成败无关（回填逻辑照旧见下）。 */
    g_pickmk_x = px; g_pickmk_y = py; g_pickmk_t = now_ms();
    if (g_pick) {
        int se = g_pick_se, sf = g_pick_sf, t, idx;
        if (se >= 0 && se < g_ope_nsteps) {
            t = g_ope_steps[se][0];
            if (sf >= 0 && sf < ope_nfields(t)) {
                idx = ope_fidx[t - 1][sf];
                if (idx >= 1 && idx <= 4) {                 /* 坐标格：1/3 = x 格，2/4 = y 格 */
                    int v = (idx == 1 || idx == 3) ? px : py;
                    g_ope_steps[se][idx] = v;
                    g_ne_lit = 1;                           /* 取点结果 = 字面值 */
                    g_ope_vl = 0;                           /* 防御：变量列表开着时也让结果落地 */
                    snprintf(g_ne_buf, sizeof g_ne_buf, "%d", v);
                    g_ne_msg[0] = 0;
                    ALOGI("取点 回填 第 %d 步 参数 %d = %d", se + 1, sf + 1, v);
                }
            }
        }
    }
    g_pick = 0;                          /* 退取点态（不管回填成没成：这一轮取点结束了） */
    g_need = 1; g_force_frames = 2;      /* 重画 */
}

/* 加一步：默认值必须核心必过 —— 点按 = 逻辑屏中心按住 50ms；滑动 = 中心 → 中心下方 200px、300ms；
 * 等待 = 100ms；按下 / 区域判断 = 中心点（区域判断还须选区域，默认空、[完成] 预检拦）；弹起 / 开关判断 = 无字段。
 * 坐标默认取屏中心是唯一「任何逻辑尺寸都必合法」的取法（精确落点交给 [参数]/[取点]）。 */
static void ope_add_step(int type)
{
    int cx = g_w / 2, cy = g_h / 2;
    int *s6;
    if (g_ope_nsteps >= OPE_MAX_STEPS) {
        snprintf(g_ope_msg, sizeof g_ope_msg, "最多 %d 步", OPE_MAX_STEPS);
        g_force_frames = 2;
        return;
    }
    s6 = g_ope_steps[g_ope_nsteps];
    memset(s6, 0, sizeof g_ope_steps[0]);
    s6[0] = type;
    g_ope_refs[g_ope_nsteps][0] = 0;                 /* 新步 ref 清空（条件步未选区域） */
    if (type == OP_STEP_TAP) {
        s6[1] = cx; s6[2] = cy; s6[5] = 50;
    } else if (type == OP_STEP_SWIPE) {
        s6[1] = cx; s6[2] = cy; s6[3] = cx; s6[4] = cy + 200; if (s6[4] >= g_h) s6[4] = g_h - 1; s6[5] = 300;
    } else if (type == OP_STEP_DOWN || type == OP_STEP_COND_REGION) {
        s6[1] = cx; s6[2] = cy;
    } else if (type == OP_STEP_WAIT) {
        s6[5] = 100;
    }
    /* 弹起 / 开关判断：无字段（s6 已清零；条件步 a3=0 = 不成立中止，默认） */
    g_ope_nsteps++;
    g_ope_msg[0] = 0;
    g_need = 1; g_force_frames = 2;
    ALOGI("op edit 加一步 %s（现 %d 步）", ope_tname(type), g_ope_nsteps);
}

/* 步骤上移/下移（±1）：行序 = 执行序（核心按表顺序跑）；到头不动。 */
static void ope_move(int i, int d)
{
    int j = i + d, t6[6];
    char tr[REGION_ID_MAX + 1];
    if (j < 0 || j >= g_ope_nsteps) return;
    memcpy(t6, g_ope_steps[i], sizeof t6);
    memcpy(g_ope_steps[i], g_ope_steps[j], sizeof t6);
    memcpy(g_ope_steps[j], t6, sizeof t6);
    memcpy(tr, g_ope_refs[i], sizeof tr);            /* ref 跟着步一起动 */
    memcpy(g_ope_refs[i], g_ope_refs[j], sizeof tr);
    memcpy(g_ope_refs[j], tr, sizeof tr);
    g_need = 1; g_force_frames = 2;
    ALOGI("op edit 步骤 %d %s", i + 1, d < 0 ? "上移" : "下移");
}

/* 删一步：至少留 1 步（核心校验 1..32，本地先拦，免得 [完成] 才报）。 */
static void ope_del_step(int i)
{
    int j;
    if (g_ope_nsteps <= 1) {
        snprintf(g_ope_msg, sizeof g_ope_msg, "至少保留 1 步");
        g_force_frames = 2;
        return;
    }
    for (j = i; j + 1 < g_ope_nsteps; j++) {
        memcpy(g_ope_steps[j], g_ope_steps[j + 1], sizeof g_ope_steps[0]);
        memcpy(g_ope_refs[j], g_ope_refs[j + 1], sizeof g_ope_refs[0]);   /* ref 跟着步一起动 */
    }
    g_ope_nsteps--;
    g_ope_msg[0] = 0;
    g_need = 1; g_force_frames = 2;
    ALOGI("op edit 删一步（现 %d 步）", g_ope_nsteps);
}

/* 门控行：点击循环 无 → 各 kind==toggle 区域（按区域表顺序）→ 无。读核心现表（区域随时可改）、
 * 不缓存；悬空值或被取消开关型的旧值，下一次点击即归「无」。 */
static void ope_gate_cycle(void)
{
    char ids[32][16];
    int n = 0, i, cnt = vtouch_region_count();
    for (i = 0; i < cnt && n < 32; i++) {
        char id[16]; int t, a1, a2, a3, a4, en;
        if (vtouch_region_kind_get(i) != 1) continue;
        if (vtouch_get_region(i, id, sizeof id, &t, &a1, &a2, &a3, &a4, &en) != 0) continue;
        snprintf(ids[n], sizeof ids[n], "%s", id);
        n++;
    }
    g_need = 1; g_force_frames = 2;
    if (n == 0) {
        g_ope_gate[0] = 0;                       /* 顺手清门控值：没开关型区域了，别让保存写出悬空 id（核心容忍但清理更干净） */
        snprintf(g_ope_msg, sizeof g_ope_msg, "没有开关型区域：门控只认开关型（在区域页设）");
        return;
    }
    g_ope_msg[0] = 0;
    if (!g_ope_gate[0]) { snprintf(g_ope_gate, sizeof g_ope_gate, "%s", ids[0]); return; }
    for (i = 0; i < n; i++) if (!strcmp(ids[i], g_ope_gate)) break;
    if (i + 1 < n) snprintf(g_ope_gate, sizeof g_ope_gate, "%s", ids[i + 1]);
    else g_ope_gate[0] = 0;                          /* 到末尾（含悬空值）→ 回「无」 */
}

/* 关闭编辑覆盖层（含全部子层状态）：[取消] 与保存全成都走它。 */
static void op_edit_close(void)
{
    g_ope_i = -1;
    g_ope_kb = 0; g_ope_kbmsg[0] = 0;
    g_ope_se = -1; g_ne_msg[0] = 0;
    g_ope_vl = 0; g_ope_rl = -1;                    /* 子层状态一并关（变量 / 区域选择弹层） */
    g_ope_msg[0] = 0;
    g_ope_saved_as[0] = 0; g_ope_del_owed[0] = 0;   /* 会话态只活在开层期间（[取消] 也丢账：旧条目可去列表里删） */
    g_need = 1; g_force_frames = 3;
}

/* 打开编辑覆盖层：按下标读一条快照（名字核不上/读步失败就拒 —— 表在点按与打开之间动过）。
 * 定义在 page_ops 之后（op_card 的 [编辑] 按钮用它，先有原型声明）。 */
static void op_edit_open(int i, const char *name)
{
    char nm[16], gate[16];
    int steps = 0, autoff = 0, s;
    if (!name || !name[0]) return;
    if (vtouch_get_op(i, nm, sizeof nm, &steps, gate, sizeof gate, &autoff) != 0 || strcmp(nm, name)) {
        ALOGW("op edit open 失败（表动过？）: %s", name);
        ev_note("打开编辑失败：%s", name);
        return;
    }
    if (steps < 1 || steps > OPE_MAX_STEPS) {
        ALOGW("op edit open 步数异常 %d（%s）", steps, name);
        ev_note("打开编辑失败：%s", name);
        return;
    }
    memset(g_ope_refs, 0, sizeof g_ope_refs);        /* 本地 ref 副本先清（读入逐步覆盖） */
    for (s = 0; s < steps; s++) {
        if (vtouch_get_op_step(i, s, &g_ope_steps[s][0], &g_ope_steps[s][1], &g_ope_steps[s][2],
                               &g_ope_steps[s][3], &g_ope_steps[s][4], &g_ope_steps[s][5],
                               g_ope_refs[s], (int)sizeof g_ope_refs[s]) != 0) {
            ALOGW("op edit 读第 %d 步失败 %s", s + 1, name);
            ev_note("打开编辑失败：%s", name);
            return;
        }
    }
    g_ope_i = i;
    snprintf(g_ope_orig, sizeof g_ope_orig, "%s", nm);
    snprintf(g_ope_name, sizeof g_ope_name, "%s", nm);
    snprintf(g_ope_gate, sizeof g_ope_gate, "%s", gate);
    g_ope_autoff = autoff ? 1 : 0;
    g_ope_nsteps = steps;
    g_ope_msg[0] = 0; g_ope_kbmsg[0] = 0; g_ope_kb = 0; g_ope_up = 0;
    g_ope_se = -1; g_ope_sf = 0; g_ne_msg[0] = 0;
    g_ope_vl = 0; g_ope_rl = -1;                    /* 子层状态一并清（变量 / 区域选择弹层） */
    g_ope_saved_as[0] = 0; g_ope_del_owed[0] = 0;   /* 会话态开层清零（只服务本次编辑） */
    g_need = 1; g_force_frames = 3;
    ALOGI("op edit open i=%d %s（%d 步）", i, name, steps);
}

/* [完成] 保存流：先补上轮欠账（旧名没删掉）→ 面板预检（硬门）→ vtouch_op_put 整条落表 →
 * 改过名再删旧名 → **全成**才关层。任何一步失败：就地提示、覆盖层不关（改完可再点 [完成]）。
 * 删旧失败记成 g_ope_del_owed，重试点先补删 —— 「再点 [完成] 重试」这条路曾因名字查重把自己
 * 刚 put 的新名判成「被别人用了」而走不通，saved_as 豁免 + 补删台账一起把它接通。 */
static void op_edit_save(void)
{
    int flat[OPE_MAX_STEPS * 6];
    int s, k, rc;
    int old_settled = 0;             /* 头顶补删删掉的正是 g_ope_orig：底部「old≠del_owed」门槛的落点 */
    char why[96];

    /* 上轮留下的欠账：先补删旧条目（del 超时/未送达会欠着；补成再走正常保存流）。 */
    if (g_ope_del_owed[0]) {
        if (vtouch_op_del(g_ope_del_owed) != 0) {
            snprintf(g_ope_msg, sizeof g_ope_msg, "旧条目『%s』仍未删除成功，再点 [完成] 重试", g_ope_del_owed);
            ALOGW("op edit 旧条目仍未删除 %s", g_ope_del_owed);
            g_force_frames = 2;
            return;
        }
        old_settled = !strcmp(g_ope_del_owed, g_ope_orig);   /* 记在清零前：这个旧名已确认不在表里 */
        g_ope_del_owed[0] = 0;
        g_ops_save_pending = 1;      /* 表里少了一条（欠账补删掉）→ 落盘（T2.7） */
    }
    rc = ope_name_ok(g_ope_orig, g_ope_name);
    if (rc != 0) {
        snprintf(g_ope_msg, sizeof g_ope_msg, "%s", ope_name_why(rc));
        ALOGI("op edit 拒收：名字 rc=%d", rc);
        g_force_frames = 2;
        return;
    }
    if (g_ope_nsteps < 1 || g_ope_nsteps > OPE_MAX_STEPS) {
        snprintf(g_ope_msg, sizeof g_ope_msg, "步数必须在 1..%d", OPE_MAX_STEPS);
        g_force_frames = 2;
        return;
    }
    for (s = 0; s < g_ope_nsteps; s++) {
        if (!ope_step_check(s, g_ope_steps[s], why, (int)sizeof why)) {
            snprintf(g_ope_msg, sizeof g_ope_msg, "%s", why);
            ALOGI("op edit 拒收：%s", why);
            g_force_frames = 2;
            return;
        }
    }
    for (s = 0; s < g_ope_nsteps; s++)
        for (k = 0; k < 6; k++) flat[s * 6 + k] = g_ope_steps[s][k];
    /* ref 通道（T3.1）：g_ope_refs 直接递（每步一格，空串 = 无）；条件步的 ref 由此进核心 */
    if (vtouch_op_put(g_ope_name, g_ope_gate, g_ope_autoff, flat, g_ope_refs, g_ope_nsteps) != 0) {
        snprintf(g_ope_msg, sizeof g_ope_msg, "保存被核心拒（名字/步类型/坐标/时长/区域引用照核心校验，含表满）或编辑未送达");
        ALOGW("op edit put 失败 %s", g_ope_name);
        g_force_frames = 2;
        return;
    }
    snprintf(g_ope_saved_as, sizeof g_ope_saved_as, "%s", g_ope_name);   /* 登记成功名：重试时它在表里，不再判成重名 */
    g_ops_save_pending = 1;                      /* 整条已落表 → 渲染线程那一拍写 ops.conf（T2.7） */
    /* 需要删旧名 ⟺ 改过名 且 旧名不是刚补删掉的那个（old≠del_owed：del_owed 补成后已按规格清零，
     * settled 承接同一条门槛 —— 回读已确认表里没有它，再删一次纯属空转）。 */
    if (strcmp(g_ope_orig, g_ope_name) && !old_settled) {
        if (vtouch_op_del(g_ope_orig) != 0) {        /* 改了名但旧名没删掉：表里会两条 → 记账，下次 [完成] 先补删 */
            snprintf(g_ope_del_owed, sizeof g_ope_del_owed, "%s", g_ope_orig);
            snprintf(g_ope_msg, sizeof g_ope_msg, "已保存；旧条目『%s』删除失败——再点 [完成] 重试", g_ope_orig);
            ALOGW("op edit 旧条目删除失败 %s", g_ope_orig);
            g_force_frames = 2;
            return;
        }
    }
    ALOGI("op edit save %s（%d 步%s）", g_ope_name, g_ope_nsteps,
          strcmp(g_ope_orig, g_ope_name) ? "，改名" : "");
    {
        char line[96];
        snprintf(line, sizeof line, "操作已保存：%s · %d 步", g_ope_name, g_ope_nsteps);
        ev_log_push(line);                           /* 事件日志一行（成功路径，不带 '!' 前缀） */
    }
    op_edit_close();
}

/* 步骤行：`i. 点按` + 参数小字一行（摘要），下面按键 [参数（无字段的类型不画）][↑][↓][删]（行高 72 保手指可点）。
 * 条件步（区域判断 / 开关判断）再加两行：区域下拉 + 不成立行为两键（中止 / 跳过下一步）。 */
static void ope_step_row(int i)
{
    const int *s6 = g_ope_steps[i];
    int t = s6[0], nf = ope_nfields(t);
    char tb[32], p[96];
    ope_step_text(i, p, sizeof p);
    ImGui::PushID(2000 + i);
    snprintf(tb, sizeof tb, "%d. %s", i + 1, ope_tname(t));
    ImGui::TextUnformatted(tb);
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    text_meta_s(p);
    {
        int nbtn = (nf > 0 ? 1 : 0) + 3;             /* [参数] + ↑ ↓ 删 */
        float bw = (ImGui::GetContentRegionAvail().x - (nbtn - 1) * 12) / nbtn;   /* 缝按真实 SameLine 间距 12 扣 */
        if (nf > 0) {
            if (btn_blue("参数", ImVec2(bw, 72))) {  /* 进数字弹层：从第 1 个字段起逐个编 */
                g_ope_se = i; g_ope_sf = 0;
                ope_num_load();
                g_ne_msg[0] = 0;
                g_need = 1; g_force_frames = 2;
            }
            ImGui::SameLine();
        }
        if (btn_light("↑", ImVec2(bw, 72))) ope_move(i, -1);
        ImGui::SameLine();
        if (btn_light("↓", ImVec2(bw, 72))) ope_move(i, +1);
        ImGui::SameLine();
        if (btn_red("删", ImVec2(bw, 72))) ope_del_step(i);
    }
    if (t == OP_STEP_COND_REGION || t == OP_STEP_COND_TOGGLE) {
        /* 条件参数：区域下拉（开关判断的列表只列开关型，见 draw_ope_rlist）+ 不成立行为两键（默认中止） */
        char g[48];
        float aw = ImGui::GetContentRegionAvail().x;
        snprintf(g, sizeof g, "区域：%s", g_ope_refs[i][0] ? g_ope_refs[i] : "未选");
        if (btn_light(g, ImVec2(aw, 72))) {
            g_ope_rl = i;
            g_need = 1; g_force_frames = 2;
            ALOGI("op edit 区域列表开 第 %d 步 %s", i + 1, ope_tname(t));
        }
        {
            float bw2 = (aw - 12) * 0.5f;
            bool hit;
            hit = (s6[3] == OP_COND_ABORT) ? btn_blue("中止", ImVec2(bw2, 72)) : btn_light("中止", ImVec2(bw2, 72));
            if (hit && s6[3] != OP_COND_ABORT) {
                g_ope_steps[i][3] = OP_COND_ABORT;
                g_need = 1; g_force_frames = 2;
                ALOGI("op edit 条件行为 第 %d 步 → 中止", i + 1);
            }
            ImGui::SameLine();
            hit = (s6[3] == OP_COND_SKIP) ? btn_blue("跳过下一步", ImVec2(bw2, 72)) : btn_light("跳过下一步", ImVec2(bw2, 72));
            if (hit && s6[3] != OP_COND_SKIP) {
                g_ope_steps[i][3] = OP_COND_SKIP;
                g_need = 1; g_force_frames = 2;
                ALOGI("op edit 条件行为 第 %d 步 → 跳过下一步", i + 1);
            }
        }
    }
    ImGui::Dummy(ImVec2(0, 4));                      /* 行间缝 */
    ImGui::PopID();
}

/* 名字子层（操作改名）：整面盖住编辑覆盖层，字符键盘复用 draw_char_kb。
 * [确定] 走面板硬门（合法 + 不撞别的操作名）：拒收就地提示、子层不关。 */
static void draw_ope_name_kb(void)
{
    int act = draw_char_kb("给操作起个名字：脚本/触发绑定按名字认它（与别的操作重名会被拒）",
                           g_ope_orig, g_ope_name, (int)sizeof g_ope_name, &g_ope_up,
                           g_ope_kbmsg, (int)sizeof g_ope_kbmsg);
    if (act == 1) {
        g_ope_kb = 0; g_ope_kbmsg[0] = 0;
        g_need = 1; g_force_frames = 3;
        ALOGI("op edit 改名取消");
    } else if (act == 2) {
        int rc = ope_name_ok(g_ope_orig, g_ope_name);
        if (rc == 0) {
            g_ope_kb = 0; g_ope_kbmsg[0] = 0;
            g_need = 1; g_force_frames = 3;
            ALOGI("op edit 改名 -> %s", g_ope_name);
        } else {
            snprintf(g_ope_kbmsg, sizeof g_ope_kbmsg, "%s", ope_name_why(rc));
            g_need = 1; g_force_frames = 2;
            ALOGI("op edit 改名拒 rc=%d", rc);
        }
    }
}

/* 数字弹层：一次编某一步的一个字段（字段标签 + 当前值），坐标与毫秒都走它。
 * [确定] 先过硬门（范围校验）：非法拒收 + 就地提示、不落值；合法写进本地副本并自动进下一个字段
 * （最后一个字段编完回编辑层）。[取消] 回编辑层（本轮已确认过的字段保留）。
 * T3.1 变量：可变量字段多一个 [变量]（弹 5 项中文名 + 「数值」回退，见 draw_ope_vlist）—— 选中变量
 * 后值框显示中文名（g_ne_lit=0），[确定] 直接确认该变量引用；按数字键/退格 = 切回字面输入。 */
static void draw_num_edit(void)
{
    const int *s6;
    int type, nf, idx, is_coord, var_ok, nbtn;
    const char *label;
    ImDrawList *dl;
    ImVec2 wp, a, b;
    float ww, wh, x0, y0, cw, vby, vbh, boxw, btnw;
    if (g_ope_se < 0 || g_ope_se >= g_ope_nsteps) { g_ope_se = -1; return; }
    s6 = g_ope_steps[g_ope_se];
    type = s6[0];
    nf = ope_nfields(type);
    if (nf == 0 || g_ope_sf < 0 || g_ope_sf >= nf) { g_ope_se = -1; return; }
    idx = ope_fidx[type - 1][g_ope_sf];
    if (idx < 0) { g_ope_se = -1; return; }          /* 防御：字段表里没有这一格 */
    label = ope_flabel[type - 1][g_ope_sf];
    is_coord = (idx >= 1 && idx <= 4);
    var_ok = ope_var_ok(type, idx);

    dl = ImGui::GetWindowDrawList();
    wp = ImGui::GetWindowPos();
    ww = ImGui::GetWindowWidth();
    wh = ImGui::GetWindowHeight();
    a = ImVec2(wp.x + 12, wp.y + (float)TITLE_H + 10);
    b = ImVec2(wp.x + ww - 12, wp.y + wh - 12);
    dl->AddRectFilled(a, b, IM_COL32(255, 255, 255, 253), 14);
    dl->AddRect(a, b, IM_COL32(228, 228, 231, 255), 14, 0, 1.5f);
    x0 = a.x + 26; y0 = a.y + 24; cw = (b.x - x0) - 26;
    ImGui::SetCursorScreenPos(ImVec2(x0, y0));
    {
        char t[64];
        snprintf(t, sizeof t, "第 %d 步 · %s · 参数 %d/%d", g_ope_se + 1, ope_tname(type), g_ope_sf + 1, nf);
        text_meta_s(t);
    }
    /* 当前值框（坐标字段旁 [取点]；可变量字段旁 [变量]）：变量引用显示中文名，字面值显示输入缓冲 */
    vby = y0 + 46; vbh = 108;
    btnw = 210; nbtn = (is_coord ? 1 : 0) + (var_ok ? 1 : 0);
    boxw = cw - nbtn * (btnw + 12);
    ImGui::PushStyleColor(ImGuiCol_Border, BLUE500);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.976f, 0.980f, 0.984f, 1.00f));
    ImGui::SetCursorScreenPos(ImVec2(x0, vby));
    ImGui::BeginChild("##numval", ImVec2(boxw, vbh), ImGuiChildFlags_Border, ImGuiWindowFlags_NoScrollbar);
    {
        char show[24];
        const char *vn = !g_ne_lit ? ope_vname(s6[idx]) : NULL;
        if (vn) snprintf(show, sizeof show, "%s", vn);          /* 变量引用：显示中文名 */
        else snprintf(show, sizeof show, "%s", g_ne_buf[0] ? g_ne_buf : "(空)");
        ImVec2 tp = ImGui::GetCursorScreenPos();
        ImDrawList *d2 = ImGui::GetWindowDrawList();
        if (g_font_meta) d2->AddText(g_font_meta, g_font_meta->FontSize, ImVec2(tp.x + 18, tp.y + 14),
                                     IM_COL32(113, 113, 122, 255), label);
        else d2->AddText(ImVec2(tp.x + 18, tp.y + 14), IM_COL32(113, 113, 122, 255), label);
        d2->AddText(ImVec2(tp.x + 18, tp.y + 46), IM_COL32(24, 24, 27, 255), show);
    }
    ImGui::EndChild();
    ImGui::PopStyleColor(2);
    {
        float bx = x0 + boxw + 12;
        if (is_coord) {
            ImGui::SetCursorScreenPos(ImVec2(bx, vby));
            if (btn_light("取点", ImVec2(btnw, vbh))) {
                /* T2.8 接线：先进取点态（先 cancel 再 request —— 清掉可能残留的旧请求，核心的
                 * 0→1 转变与 20s 计时从这一次点按起算），核心吞一次触摸回填；点面板里 = 取消。 */
                g_pick_se = g_ope_se; g_pick_sf = g_ope_sf;
                vtouch_pick_cancel();
                vtouch_pick_request();
                g_pick = 1;
                g_ne_msg[0] = 0;
                g_need = 1; g_force_frames = 2;
                ALOGI("取点 请求 第 %d 步 参数 %d/%d", g_ope_se + 1, g_ope_sf + 1, nf);
            }
            bx += btnw + 12;
        }
        if (var_ok) {
            ImGui::SetCursorScreenPos(ImVec2(bx, vby));
            if (btn_light("变量", ImVec2(btnw, vbh))) {          /* 弹变量列表（值 ↔ 变量切换） */
                g_ope_vl = 1;
                g_ne_msg[0] = 0;
                g_need = 1; g_force_frames = 2;
                ALOGI("op edit 变量列表开 第 %d 步 参数 %d/%d", g_ope_se + 1, g_ope_sf + 1, nf);
            }
        }
    }
    if (g_pick) {
        /* 取点态提示条（T2.8）：与错误提示共用固定槽位（键盘位置不动，防误点）。 */
        ImVec2 p1 = ImVec2(x0, vby + vbh + 12), p2 = ImVec2(x0 + cw, p1.y + 46);
        dl->AddRectFilled(p1, p2, IM_COL32(219, 234, 254, 255), 8);
        dl->AddRect(p1, p2, IM_COL32(59, 130, 246, 255), 8, 0, 1.5f);
        dl->AddText(ImVec2(p1.x + 14, p1.y + 3), IM_COL32(29, 78, 216, 255), "点屏幕上目标位置（点面板里取消）");
    } else if (g_ne_msg[0]) {
        ImGui::SetCursorScreenPos(ImVec2(x0, vby + vbh + 12));
        ImGui::TextColored(ImVec4(0.863f, 0.149f, 0.149f, 1.00f), "%s", g_ne_msg);
    }
    /* 数字键：3 列 x 4 行（1-9 / ⌫ 0），再一行 [取消][确定] */
    {
        static const char *nrow[3] = { "123", "456", "789" };
        float gap = 10.0f, kw = (cw - 2 * gap) / 3.0f, kh = 96.0f;
        /* 提示槽固定占位：出现/消失提示时键盘不动（防误点） */
        float ky = vby + vbh + 12 + 52.0f + 8.0f;
        for (int r = 0; r < 3; r++) {
            for (int c = 0; c < 3; c++) {
                char lab[2] = { nrow[r][c], 0 };
                ImGui::PushID(400 + r * 3 + c);
                ImGui::SetCursorScreenPos(ImVec2(x0 + c * (kw + gap), ky + r * (kh + gap)));
                if (btn_light(lab, ImVec2(kw, kh))) {
                    int n;
                    if (!g_ne_lit) { g_ne_lit = 1; g_ne_buf[0] = 0; }   /* 按数字 = 弃变量、改字面输入 */
                    n = (int)strlen(g_ne_buf);
                    if (n < 6) { g_ne_buf[n] = lab[0]; g_ne_buf[n + 1] = 0; }
                    else snprintf(g_ne_msg, sizeof g_ne_msg, "最多 6 位数字");
                    g_need = 1; g_force_frames = 2;
                }
                ImGui::PopID();
            }
        }
        /* 第 4 行：退格 + 0（右格留空） */
        ImGui::PushID(430);
        ImGui::SetCursorScreenPos(ImVec2(x0, ky + 3 * (kh + gap)));
        if (btn_light("⌫", ImVec2(kw, kh))) {
            int n;
            if (!g_ne_lit) { g_ne_lit = 1; g_ne_buf[0] = 0; }       /* 同上：切回字面输入 */
            n = (int)strlen(g_ne_buf);
            if (n > 0) g_ne_buf[n - 1] = 0;
            g_ne_msg[0] = 0;
            g_need = 1; g_force_frames = 2;
        }
        ImGui::PopID();
        ImGui::PushID(431);
        ImGui::SetCursorScreenPos(ImVec2(x0 + kw + gap, ky + 3 * (kh + gap)));
        if (btn_light("0", ImVec2(kw, kh))) {
            int n;
            if (!g_ne_lit) { g_ne_lit = 1; g_ne_buf[0] = 0; }       /* 同上：切回字面输入 */
            n = (int)strlen(g_ne_buf);
            if (n < 6) { g_ne_buf[n] = '0'; g_ne_buf[n + 1] = 0; }
            else snprintf(g_ne_msg, sizeof g_ne_msg, "最多 6 位数字");
            g_need = 1; g_force_frames = 2;
        }
        ImGui::PopID();
        /* 取消 / 确定 */
        float by = ky + 4 * (kh + gap) + 4, bw2 = (cw - gap) * 0.5f;
        ImGui::PushID(450);
        ImGui::SetCursorScreenPos(ImVec2(x0, by));
        if (btn_light("取消", ImVec2(bw2, 92))) {
            ALOGI("op edit 参数取消 第 %d 步", g_ope_se + 1);
            g_ope_se = -1; g_ne_msg[0] = 0;
            g_need = 1; g_force_frames = 3;
        }
        ImGui::SetCursorScreenPos(ImVec2(x0 + bw2 + gap, by));
        if (btn_blue("确定", ImVec2(bw2, 92))) {
            if (!g_ne_lit) {
                /* 变量引用：值已存（-1..-5），无需数字解析 —— 直接确认、进下一个字段 */
                const char *vn = ope_vname(s6[idx]);
                ALOGI("op edit 参数 %s=%s（变量）", label, vn ? vn : "?");
                g_ne_msg[0] = 0;
                if (g_ope_sf + 1 < nf) { g_ope_sf++; ope_num_load(); }
                else g_ope_se = -1;
                g_need = 1; g_force_frames = 3;
            } else {
                int v = 0;
                char why[72];
                const char *p;
                for (p = g_ne_buf; *p; p++) v = v * 10 + (*p - '0');
                if (!ne_check(label, type, g_ope_sf, v, why, (int)sizeof why)) {
                    snprintf(g_ne_msg, sizeof g_ne_msg, "%s", why);
                    ALOGI("op edit 参数拒收 %s=%d", label, v);
                    g_force_frames = 2;
                } else {
                    g_ope_steps[g_ope_se][idx] = v;
                    g_ne_msg[0] = 0;
                    if (g_ope_sf + 1 < nf) {
                        ALOGI("op edit 参数 %s=%d → 下一字段", label, v);
                        g_ope_sf++;
                        ope_num_load();
                    } else {
                        ALOGI("op edit 参数 %s=%d（本步完成）", label, v);
                        g_ope_se = -1;
                    }
                    g_need = 1; g_force_frames = 3;
                }
            }
        }
        ImGui::PopID();
    }
}

/* 变量选择弹层（数字弹层的 [变量]）：5 项中文名（spec §1.1 逐字）+「数值」回退 + [取消]。
 * 选中变量 → 存 -1..-5、关层（数字层值框显示中文名）；「数值」→ 回数字键盘输入
 * （从变量切回才清缓冲；本来就是字面则保留已输入的数字）。 */
static void draw_ope_vlist(void)
{
    ImDrawList *dl;
    ImVec2 wp, a, b;
    float ww, wh, x0, y0, cw, by;
    int type, nf, idx, v, k;
    if (g_ope_se < 0 || g_ope_se >= g_ope_nsteps) { g_ope_vl = 0; return; }
    type = g_ope_steps[g_ope_se][0];
    nf = ope_nfields(type);
    if (nf == 0 || g_ope_sf < 0 || g_ope_sf >= nf) { g_ope_vl = 0; g_ope_se = -1; return; }
    idx = ope_fidx[type - 1][g_ope_sf];
    if (idx < 0 || !ope_var_ok(type, idx)) { g_ope_vl = 0; return; }
    v = g_ope_steps[g_ope_se][idx];

    dl = ImGui::GetWindowDrawList();
    wp = ImGui::GetWindowPos();
    ww = ImGui::GetWindowWidth();
    wh = ImGui::GetWindowHeight();
    a = ImVec2(wp.x + 12, wp.y + (float)TITLE_H + 10);
    b = ImVec2(wp.x + ww - 12, wp.y + wh - 12);
    dl->AddRectFilled(a, b, IM_COL32(255, 255, 255, 253), 14);
    dl->AddRect(a, b, IM_COL32(228, 228, 231, 255), 14, 0, 1.5f);
    x0 = a.x + 26; y0 = a.y + 24; cw = (b.x - x0) - 26;
    ImGui::SetCursorScreenPos(ImVec2(x0, y0));
    {
        char t[96];
        snprintf(t, sizeof t, "第 %d 步 · %s · %s", g_ope_se + 1, ope_tname(type), ope_flabel[type - 1][g_ope_sf]);
        text_meta_s(t);
    }
    ImGui::SetCursorScreenPos(ImVec2(x0, y0 + 40));
    text_meta_s("选一个变量（触发数据）；「数值」= 回退数字键盘输入");
    by = y0 + 100;
    for (k = 0; k < 5; k++) {
        ImGui::PushID(600 + k);
        ImGui::SetCursorScreenPos(ImVec2(x0, by + k * (84 + 12)));
        if ((v == -(k + 1)) ? btn_blue(ope_vname_tab[k], ImVec2(cw, 84))
                            : btn_light(ope_vname_tab[k], ImVec2(cw, 84))) {
            g_ope_steps[g_ope_se][idx] = -(k + 1);   /* 存 -1..-5；显示交给中文名表 */
            g_ne_lit = 0;
            g_ne_buf[0] = 0;
            g_ope_vl = 0;
            g_need = 1; g_force_frames = 3;
            ALOGI("op edit 参数变量 第 %d 步 %s = %s", g_ope_se + 1,
                  ope_flabel[type - 1][g_ope_sf], ope_vname_tab[k]);
        }
        ImGui::PopID();
    }
    by += 5 * (84 + 12);
    ImGui::PushID(610);
    ImGui::SetCursorScreenPos(ImVec2(x0, by));
    if ((g_ne_lit ? btn_blue("数值", ImVec2(cw, 84)) : btn_light("数值", ImVec2(cw, 84)))) {
        if (!g_ne_lit) g_ne_buf[0] = 0;              /* 变量 → 字面：清缓冲重新输入；本来就是字面则保留输入 */
        g_ne_lit = 1;                                /* 回退字面输入：值等 [确定] 落 */
        g_ope_vl = 0;
        g_need = 1; g_force_frames = 3;
        ALOGI("op edit 参数变量 第 %d 步 %s → 数值输入", g_ope_se + 1, ope_flabel[type - 1][g_ope_sf]);
    }
    ImGui::PopID();
    ImGui::PushID(611);
    ImGui::SetCursorScreenPos(ImVec2(x0, by + 84 + 12));
    if (btn_light("取消", ImVec2(cw, 84))) {
        g_ope_vl = 0;
        g_need = 1; g_force_frames = 2;
        ALOGI("op edit 变量列表取消 第 %d 步", g_ope_se + 1);
    }
    ImGui::PopID();
}

/* 区域选择弹层（条件步的 [区域] 按钮）：列表读区域表实时、可滚动；开关判断只列开关型
 * （spec §7：运行时核心仍校验，双保险）。点一条 → 写进该步 ref、关层。 */
static void draw_ope_rlist(void)
{
    ImDrawList *dl;
    ImVec2 wp, a, b;
    float ww, wh, x0, y0, cw, list_top, list_bot, list_h;
    int i, n, rli = g_ope_rl, only_tg, avail = 0;
    if (rli < 0 || rli >= g_ope_nsteps) { g_ope_rl = -1; return; }
    only_tg = (g_ope_steps[rli][0] == OP_STEP_COND_TOGGLE);

    dl = ImGui::GetWindowDrawList();
    wp = ImGui::GetWindowPos();
    ww = ImGui::GetWindowWidth();
    wh = ImGui::GetWindowHeight();
    a = ImVec2(wp.x + 12, wp.y + (float)TITLE_H + 10);
    b = ImVec2(wp.x + ww - 12, wp.y + wh - 12);
    dl->AddRectFilled(a, b, IM_COL32(255, 255, 255, 253), 14);
    dl->AddRect(a, b, IM_COL32(228, 228, 231, 255), 14, 0, 1.5f);
    x0 = a.x + 26; y0 = a.y + 24; cw = (b.x - x0) - 26;
    ImGui::SetCursorScreenPos(ImVec2(x0, y0));
    {
        char t[80];
        snprintf(t, sizeof t, "第 %d 步 · %s · 选择区域", rli + 1, ope_tname(g_ope_steps[rli][0]));
        text_meta_s(t);
    }
    ImGui::SetCursorScreenPos(ImVec2(x0, y0 + 40));
    text_meta_s(only_tg ? "只列开关型区域（在区域页设）" : "点一个区域作为判定目标（悬空引用运行时报「区域不存在」）");
    list_top = y0 + 76;
    list_bot = b.y - 24 - 92 - 12;                   /* 底部给 [取消] 留位 */
    list_h = list_bot - list_top;
    if (list_h < 120) list_h = 120;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ZINC50);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 12));
    ImGui::SetCursorScreenPos(ImVec2(x0, list_top));
    ImGui::BeginChild("##opregs", ImVec2(cw, list_h), ImGuiChildFlags_None,
                      ImGuiWindowFlags_AlwaysVerticalScrollbar);
    pub_zone(g_zone_list);                           /* 列表实区：拖它滚动（同区域/操作列表的口径） */
    drag_scroll_for(SCR_LIST);
    n = vtouch_region_count();
    for (i = 0; i < n; i++) {
        char id[16]; int t2, a1, a2, a3, a4, en;
        if (only_tg && vtouch_region_kind_get(i) != 1) continue;
        if (vtouch_get_region(i, id, sizeof id, &t2, &a1, &a2, &a3, &a4, &en) != 0) continue;
        avail++;
        ImGui::PushID(3000 + i);
        if ((strcmp(g_ope_refs[rli], id) == 0) ? btn_blue(id, ImVec2(ImGui::GetContentRegionAvail().x, 76))
                                               : btn_light(id, ImVec2(ImGui::GetContentRegionAvail().x, 76))) {
            snprintf(g_ope_refs[rli], sizeof g_ope_refs[rli], "%s", id);
            g_ope_rl = -1;
            g_need = 1; g_force_frames = 3;
            ALOGI("op edit 区域选中 第 %d 步 = %s", rli + 1, id);
        }
        ImGui::PopID();
    }
    if (avail == 0)
        text_meta_w(only_tg ? "没有开关型区域：先到区域页把某个区域设成开关型" : "还没有区域：先到区域页框一个");
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    ImGui::PushID(3100);
    ImGui::SetCursorScreenPos(ImVec2(x0, b.y - 24 - 92));
    if (btn_light("取消", ImVec2(cw, 92))) {
        g_ope_rl = -1;
        g_need = 1; g_force_frames = 2;
        ALOGI("op edit 区域列表取消 第 %d 步", rli + 1);
    }
    ImGui::PopID();
}

/* 编辑覆盖层主屏（整面盖住，同改名弹层）：名字行 / 步骤列表（可滚）/ 加步（两行七类型）/
 * 门控循环 / 跑完自动关 / [取消][完成]。子层（名字键盘、数字弹层、变量选择、区域选择）开着时
 * 本屏不画（被整面盖住）。 */
static void draw_op_edit(void)
{
    ImDrawList *dl;
    ImVec2 wp, a, b;
    float ww, wh, x0, y0, cw, msg_h, ly, by_bottom, done_y, autooff_y, gate_y, add_y, add_y2, list_top, list_bot, list_h;

    if (g_ope_kb) { draw_ope_name_kb(); return; }
    if (g_ope_vl) { draw_ope_vlist(); return; }      /* 变量选择弹层（数字弹层之上） */
    if (g_ope_rl >= 0) { draw_ope_rlist(); return; } /* 区域选择弹层（条件步） */
    if (g_ope_se >= 0) { draw_num_edit(); return; }

    dl = ImGui::GetWindowDrawList();
    wp = ImGui::GetWindowPos();
    ww = ImGui::GetWindowWidth();
    wh = ImGui::GetWindowHeight();
    a = ImVec2(wp.x + 12, wp.y + (float)TITLE_H + 10);
    b = ImVec2(wp.x + ww - 12, wp.y + wh - 12);
    dl->AddRectFilled(a, b, IM_COL32(255, 255, 255, 253), 14);
    dl->AddRect(a, b, IM_COL32(228, 228, 231, 255), 14, 0, 1.5f);
    x0 = a.x + 26; y0 = a.y + 24; cw = (b.x - x0) - 26;
    /* 名字行（点它开字符键盘子层） */
    ImGui::SetCursorScreenPos(ImVec2(x0, y0));
    text_meta_s("名字（脚本按名字认它；与别的操作重名会被拒）");
    ImGui::SetCursorScreenPos(ImVec2(x0, y0 + 36));
    if (btn_light(g_ope_name, ImVec2(cw, 84))) {
        g_ope_kb = 1; g_ope_kbmsg[0] = 0; g_ope_up = 0;
        g_need = 1; g_force_frames = 2;
        ALOGI("op edit 改名子层开 %s", g_ope_name);
    }
    /* 就地提示（[完成] 拒收 / 步数门 / 门控提示）；提示槽固定占位：出现提示时下面整块不动，防误点 */
    msg_h = 46.0f;
    if (g_ope_msg[0]) {
        ImGui::SetCursorScreenPos(ImVec2(x0, y0 + 36 + 84 + 12));
        ImGui::TextColored(ImVec4(0.863f, 0.149f, 0.149f, 1.00f), "%s", g_ope_msg);
    }
    /* 步骤区标题 + 底部按钮锚点（从底往上排，列表拿中间剩下的高度） */
    ly = y0 + 36 + 84 + 12 + msg_h;
    by_bottom = b.y - 24;
    done_y = by_bottom - 92;
    autooff_y = done_y - 12 - 76;
    gate_y = autooff_y - 12 - 76;
    add_y2 = gate_y - 12 - 76;               /* 加步第二行：弹起 / 区域判断 / 开关判断 */
    add_y = add_y2 - 12 - 76;                /* 加步第一行：点按 / 滑动 / 等待 / 按下 */
    list_top = ly + 36;
    list_bot = add_y - 12;
    list_h = list_bot - list_top;
    if (list_h < 140) list_h = 140;                  /* 极窄窗兜底（正常 864x1180、两行加步下 ~400） */
    ImGui::SetCursorScreenPos(ImVec2(x0, ly));
    {
        char t[80];
        snprintf(t, sizeof t, "步骤 · %d 步（最多 %d；列表可上下拖动滚）", g_ope_nsteps, OPE_MAX_STEPS);
        text_meta_s(t);
    }
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ZINC50);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 12));
    ImGui::SetCursorScreenPos(ImVec2(x0, list_top));
    ImGui::BeginChild("##opsteps", ImVec2(cw, list_h), ImGuiChildFlags_None,
                      ImGuiWindowFlags_AlwaysVerticalScrollbar);
    pub_zone(g_zone_list);                           /* 列表实区：拖它滚动（同区域/操作列表的口径） */
    drag_scroll_for(SCR_LIST);
    for (int i = 0; i < g_ope_nsteps; i++) ope_step_row(i);
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    /* 加步（v2 七类型，两行）/ 门控 / 自动关 / 收尾 */
    {
        float bw4 = (cw - 3 * 12) / 4.0f;
        ImGui::SetCursorScreenPos(ImVec2(x0, add_y));
        if (btn_light("＋点按", ImVec2(bw4, 76))) ope_add_step(OP_STEP_TAP);
        ImGui::SetCursorScreenPos(ImVec2(x0 + bw4 + 12, add_y));
        if (btn_light("＋滑动", ImVec2(bw4, 76))) ope_add_step(OP_STEP_SWIPE);
        ImGui::SetCursorScreenPos(ImVec2(x0 + 2 * (bw4 + 12), add_y));
        if (btn_light("＋等待", ImVec2(bw4, 76))) ope_add_step(OP_STEP_WAIT);
        ImGui::SetCursorScreenPos(ImVec2(x0 + 3 * (bw4 + 12), add_y));
        if (btn_light("＋按下", ImVec2(bw4, 76))) ope_add_step(OP_STEP_DOWN);
        {
            float bw3 = (cw - 2 * 12) / 3.0f;
            ImGui::SetCursorScreenPos(ImVec2(x0, add_y2));
            if (btn_light("＋弹起", ImVec2(bw3, 76))) ope_add_step(OP_STEP_UP);
            ImGui::SetCursorScreenPos(ImVec2(x0 + bw3 + 12, add_y2));
            if (btn_light("＋区域判断", ImVec2(bw3, 76))) ope_add_step(OP_STEP_COND_REGION);
            ImGui::SetCursorScreenPos(ImVec2(x0 + 2 * (bw3 + 12), add_y2));
            if (btn_light("＋开关判断", ImVec2(bw3, 76))) ope_add_step(OP_STEP_COND_TOGGLE);
        }
    }
    {
        char g[72];
        snprintf(g, sizeof g, "门控开关：%s", g_ope_gate[0] ? g_ope_gate : "无");
        ImGui::SetCursorScreenPos(ImVec2(x0, gate_y));
        if (btn_light(g, ImVec2(cw, 76))) ope_gate_cycle();
        ImGui::SetCursorScreenPos(ImVec2(x0, autooff_y));
        if (btn_light(g_ope_autoff ? "跑完自动关：开" : "跑完自动关：关", ImVec2(cw, 76))) {
            g_ope_autoff = !g_ope_autoff;
            g_need = 1; g_force_frames = 2;
            ALOGI("op edit 自动关 %s", g_ope_autoff ? "开" : "关");
        }
    }
    {
        float bw2 = (cw - 12) * 0.5f;
        ImGui::SetCursorScreenPos(ImVec2(x0, done_y));
        if (btn_light("取消", ImVec2(bw2, 92))) {    /* 取消 = 丢本轮编辑回操作页（改名弹层同款语义） */
            ALOGI("op edit 取消（丢编辑）");
            op_edit_close();
        }
        ImGui::SetCursorScreenPos(ImVec2(x0 + bw2 + 12, done_y));
        if (btn_blue("完成", ImVec2(bw2, 92))) op_edit_save();
    }
}

static void page_log(void)
{
    char meta[32];
    snprintf(meta, sizeof meta, "%d 条", g_evlog_n);
    page_header("事件日志", meta);
    if (g_evlog_n == 0) ImGui::TextDisabled("暂无事件，触摸屏幕试试");
    for (int i = 0; i < g_evlog_n; i++) ImGui::TextWrapped("%s", g_evlog[i]);
}
static void page_settings(void)
{
    page_header("设置", "");
    ImGui::Dummy(ImVec2(0, 4));
    ImGui::TextDisabled("叠加层显隐");
    if (g_ov_show) {
        if (btn_dark("显隐：显", ImVec2(260, 84))) { g_ov_show = 0; g_force_frames = 3; }
    } else {
        if (btn_light("显隐：隐", ImVec2(260, 84))) { g_ov_show = 1; g_force_frames = 3; }
    }
    ImGui::TextDisabled("只影响绘制，监听不停");
    ImGui::Dummy(ImVec2(0, 8));
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, 8));
    ImGui::TextDisabled("操作");
    ImGui::TextWrapped("拖标题栏移窗，其它地方不拖窗");
    ImGui::TextWrapped("标题栏右侧箭头按钮：整窗收成一条，再点一次展开");
    ImGui::TextWrapped("区域列表内上下拖动滚动");
    ImGui::TextWrapped("矩形/圆形框选：到屏上拖，松手生成");
    ImGui::TextWrapped("选中后：拖框内移动，拖四角黄块缩放");
    ImGui::TextWrapped("点区域卡片上的名字可改 id（脚本按 id 监听）");
    ImGui::TextWrapped("改完自动存 regions.conf，重启还在");
}

/* 说明页（T3.2）：12 条术语（spec §5 逐字 = 面板文案唯一来源）。数组内容从
 * docs/OPS_PLAN_V2.md §5 机器提取（去 ** 加粗标记；折行按 CommonMark 软换行拼回单空格），
 * 提取结果逐字节复核 —— 改文案先改 spec、再按同一规则重提，别手改这里。 */
static const char *const g_help_lines[] = {
    "1. 触发：给区域绑一条操作；手指碰到这个区域就会跑那条操作。",
    "2. 时机·按下：手指碰到区域的那一刻就跑。",
    "3. 时机·完整按压：手指碰到、抬起后跑（按一下、抬起来，才算数）。",
    "4. 开关型：把区域当开关——每完整按压一次，「开/关」翻转一次（绿色 = 开）。",
    "5. 门控：操作的门禁——绑一个开关型区域，它开着，操作才允许跑。",
    "6. 跑完自动关：操作正常跑完后，自动把门控开关翻回「关」。",
    "7. 取点：点 [取点]，然后去屏幕上点一下——那个位置填进正在编辑的坐标格，并在屏上标一下。",
    "8. 变量（触发数据）：这次触发的那根手指——触发按下x / 触发按下y（按下位置）、触发弹起x / 触发弹起y（抬起位置）、触发时长（按下到抬起的毫秒数）。「完整按压」触发时全都有； 「按下」触发只有按下位置；面板手动运行没有。",
    "9. 按下 / 弹起：两条分开的步骤——按下 = 按住不放；弹起 = 松开。中间可以夹「等待」「判断」。",
    "10. 区域判断：检查一个点在不在某个区域内；不满足时按你选的来：中止、或跳过下一步。",
    "11. 开关判断：检查某个开关型区域现在是不是「开」。",
    "12. 中止原因速查：变量无值 / 槽占用 / 未按下 / 区域不存在 / 非开关型 / 条件不成立。",
};
static void page_help(void)
{
    page_header("说明", "");
    ImGui::Dummy(ImVec2(0, 6));
    /* 正文自成一格可滚容器（与区域列表 / 操作页同款：AlwaysVerticalScrollbar + SCR_LIST） */
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ZINC50);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 12));
    ImGui::BeginChild("##help", ImVec2(0, 0), ImGuiChildFlags_None,
                      ImGuiWindowFlags_AlwaysVerticalScrollbar);
    pub_zone(g_zone_list);
    drag_scroll_for(SCR_LIST);
    for (int i = 0; i < (int)(sizeof g_help_lines / sizeof g_help_lines[0]); i++) {
        ImGui::TextWrapped("%s", g_help_lines[i]);
        ImGui::Dummy(ImVec2(0, 10));
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

/* 改名弹层（区域）：键盘本体抽到 draw_char_kb（与操作改名共用）；这里只留区域改名的语义：
 * 确定走 id_name_ok（合法 + 不撞别的区域），成功才 rename_region。 */
static void draw_name_edit(void)
{
    int act = draw_char_kb("给区域起个名字：脚本就按这个名字认它（默认 r1/c1…，可改）",
                           g_name_old, g_name_buf, (int)sizeof g_name_buf, &g_name_up,
                           g_name_msg, (int)sizeof g_name_msg);
    if (act == 1) {
        g_name_i = -1; g_name_msg[0] = 0; g_need = 1; g_force_frames = 3;
        ALOGI("name cancel");
    } else if (act == 2) {
        int rc = id_name_ok(g_name_old, g_name_buf);
        if (rc == 0) {
            if (strcmp(g_name_old, g_name_buf)) rename_region(g_name_old, g_name_buf);
            g_name_i = -1; g_name_msg[0] = 0; g_need = 1; g_force_frames = 3;
        } else {
            snprintf(g_name_msg, sizeof g_name_msg, "%s",
                     rc == 1 ? "名字不能为空" : rc == 2 ? "最多 15 个字符" :
                     rc == 3 ? "只能用 a-z A-Z 0-9 _ -" : "这个名字已经被别的区域用了");
            g_need = 1; g_force_frames = 2;
            ALOGI("name reject rc=%d", rc);
        }
    }
}

/* ---- 面板：标题栏（唯一拖动区）/ 固定侧栏 / 可滚内容页；收起态只剩标题栏 ---- */
static void build_panel(void)
{
    float ww = panel_w();
    if (g_pan_x < 0) g_pan_x = 0;
    if (g_pan_y < 0) g_pan_y = 0;
    if (g_pan_x > g_scr_w - 200) g_pan_x = (float)(g_scr_w - 200);
    if (g_pan_y > g_scr_h - 200) g_pan_y = (float)(g_scr_h - 200);
    /* 每帧无条件推送：先前用 Appearing 只在首帧生效，且会把快照侧在 Begin 之后的推送
     * 覆盖掉（同一帧内最后一次 SetNextWindowPos 的 cond 才进下一次 Begin），
     * 结果 g_pan 已经改了、窗口却永远停在首帧位置。Always 才能跟手。 */
    ImGui::SetNextWindowPos(ImVec2(g_pan_x, g_pan_y), ImGuiCond_Always);
    /* push/pop 用同一快照：本函数一次调用内可能被标题栏 toggle 改写 g_min，
     * 直接读 g_min 会出现“push 0 次 / pop 2 次”而触发 ImGui 断言崩溃（已实测）。 */
    const int min_bg = g_min;
    if (min_bg) {                         /* 收起态：半透明白 + 浅描边，尽量不抢眼 */
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(1.00f, 1.00f, 1.00f, 0.86f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.72f, 0.72f, 0.75f, 0.45f));
    }
    ImGui::SetNextWindowSize(ImVec2(ww, panel_h()), ImGuiCond_Always);
    ImGui::Begin("##vtpanel", 0,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar |
                 ImGuiWindowFlags_NoScrollWithMouse);
    {
        ImVec2 pp = ImGui::GetWindowPos(), ps = ImGui::GetWindowSize();
        g_pan_r[0] = pp.x; g_pan_r[1] = pp.y; g_pan_r[2] = pp.x + ps.x; g_pan_r[3] = pp.y + ps.y;
    }
    /* 列表实区每帧先清空：只在列表页发布（区域列表 / 操作 / 编辑层步骤表 / 说明），其它页不命中 → 不会误滚 */
    g_zone_list[0] = g_zone_list[1] = g_zone_list[2] = g_zone_list[3] = 0;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(PAD_X, PAD_Y));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(12, 10));
    build_titlebar(ww);
    if (g_min) {   /* 收起态：只有标题条；清掉侧栏/内容页实区，免上一帧残留命中 */
        g_zone_side[0] = g_zone_side[1] = g_zone_side[2] = g_zone_side[3] = 0;
        g_zone_sheet[0] = g_zone_sheet[1] = g_zone_sheet[2] = g_zone_sheet[3] = 0;
    } else if (g_name_i >= 0 || g_ope_i >= 0) {
        /* 覆盖层（区域改名 / 操作编辑）整面盖住：同样清底下实区 —— 不然按下可能命中上一帧残留 → 误滚 */
        g_zone_side[0] = g_zone_side[1] = g_zone_side[2] = g_zone_side[3] = 0;
        g_zone_sheet[0] = g_zone_sheet[1] = g_zone_sheet[2] = g_zone_sheet[3] = 0;
    }
    if (!g_min) ImGui::Dummy(ImVec2(0, COL_GAP - 10));
    if (!g_min && g_name_i >= 0) draw_name_edit();          /* 改名弹层：本帧不画侧栏/内容页 */
    else if (!g_min && g_ope_i >= 0) draw_op_edit();        /* 操作编辑覆盖层（T2.6）：同上 */
    if (!g_min && g_name_i < 0 && g_ope_i < 0) build_sidebar();
    if (!g_min && g_sheet && g_name_i < 0 && g_ope_i < 0) {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_ChildBg, WHITE);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 12));
        ImGui::BeginChild("##sheet", ImVec2(0, 0), ImGuiChildFlags_None);
        pub_zone(g_zone_sheet);
        drag_scroll_for(SCR_SHEET);
        if (g_nav == 0) page_regions();
        else if (g_nav == 1) page_ops();
        else if (g_nav == 2) page_log();
        else if (g_nav == 4) page_help();
        else page_settings();
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
    }
    if (g_scr_target == SCR_NONE) g_scroll_acc = 0;   /* 非滚动容器按下：丢弃累积量 */
    ImGui::PopStyleVar(2);
    ImGui::End();
    if (min_bg) ImGui::PopStyleColor(2);
}

/* 单帧：事件 API 喂输入 → 单 NewFrame → 双内容 → 提交 */
static void draw_frame(int sw, int sh)
{
    ImGuiIO &io = ImGui::GetIO();
    eglMakeCurrent(g_dpy, g_surf, g_surf, g_ctx);
    io.DisplaySize = ImVec2((float)sw, (float)sh);
    io.DisplayFramebufferScale = ImVec2(1, 1);
    int fd = -1;
    for (int fi = 0; fi < 64; fi++) if (g_dots[fi].on) { fd = fi; break; }
    if (g_mdown || g_mup_pend || fd >= 0) {
        float mx = g_mx, my = g_my;
        if (!g_mdown && fd >= 0) {
            /* 未按下时的 hover 位置也要用当前屏坐标（g_dots 是竖屏逻辑坐标，横屏下会错位） */
            int sx, sy;
            p2c(g_dots[fd].x, g_dots[fd].y, &sx, &sy);
            mx = (float)sx; my = (float)sy;
        }
        io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
        io.AddMousePosEvent(mx, my);
    }
    if (g_need_mouse_evt) {
        io.AddMouseButtonEvent(0, g_mouse_evt_down ? true : false);
        g_need_mouse_evt = 0;
    }
    ImGui_ImplOpenGL3_NewFrame();
    ImGui::NewFrame();
    if (g_ov_show) build_overlay(sw, sh);
    build_panel();
    ImGui::Render();
    if (g_mdown && g_mup_pend) { g_mdown = 0; g_mup_pend = 0; g_mslot = -1; }
    glViewport(0, 0, sw, sh);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    {
        /* 空指针保护：ImGui 上下文/帧数据若丢了，把 NULL 喂给后端会跳到 0 地址
         *（真机 tombstone 里见过 pc=0x0、返回地址在 render_thread_fn）。宁可跳一帧。 */
        ImDrawData *dd = ImGui::GetDrawData();
        if (!dd || !ImGui::GetCurrentContext()) ALOGE("GetDrawData/Context 为空 → 跳过本帧渲染");
        else ImGui_ImplOpenGL3_RenderDrawData(dd);
    }
    /* 提交失败要记账：连续失败说明这帧没上屏（面板看着在跑、其实全白） */
    if (!eglSwapBuffers(g_dpy, g_surf)) {
        g_swap_fail++;
        if (g_swap_fail == 1 || g_swap_fail % 50 == 0)
            ALOGE("eglSwapBuffers 失败 x%d (0x%x)", g_swap_fail, eglGetError());
    } else {
        g_swap_fail = 0;
        if (g_swap_armed > 0 && --g_swap_armed == 0) {
            g_swap_done = 1;
            ALOGI("换绑后连续两帧已提交上屏（Java 可恢复图层）t=+%.0fms", (double)t_since_start());
        }
    }
}

/* 面板侧是否还有这个 id（区域表被改过之后用来清理悬空引用 —— 改表的入口是面板自身：
 * 删/改名走共享内存编辑邮箱；WS 命令族只有 clear/list/add）。
 * 调用点：渲染线程（经 g_prune_req）—— 此时不持有 region 锁（region_changed 在锁外调），
 * 所以这里再进 vtouch_get_region 取锁是安全的。 */
static int region_id_exists(const char *id)
{
    int n = vtouch_region_count(), i;
    char jd[16]; int t, a1, a2, a3, a4, en;
    if (!id || !*id) return 0;
    for (i = 0; i < n; i++) {
        if (vtouch_get_region(i, jd, sizeof jd, &t, &a1, &a2, &a3, &a4, &en) != 0) continue;
        if (!strcmp(jd, id)) return 1;
    }
    return 0;
}
static void prune_panel_refs(void)
{
    int i, k = 0, changed = 0;
    for (i = 0; i < g_nhide; i++) {
        if (region_id_exists(g_hidden[i])) {
            if (k != i) snprintf(g_hidden[k], 16, "%s", g_hidden[i]);
            k++;
        } else changed = 1;
    }
    g_nhide = k;
    if (g_sel_id[0] && !region_id_exists(g_sel_id)) { g_sel_id[0] = 0; changed = 1; }
    if (g_flash_id[0] && !region_id_exists(g_flash_id)) g_flash_id[0] = 0;
    if (g_ex_id[0] && !region_id_exists(g_ex_id)) g_ex_id[0] = 0;
    if (changed) g_save_pending = 1;      /* hide 表变了 → 让渲染线程重写盘 */
}

static void *render_thread_fn(void *)
{
    int gl_ready = 0;
    long egl_warn_t = 0;
    int egl_fail_n = 0, egl_ws_fail_n = 0;   /* 连续失败计数：有上界才不"看着在跑、其实全死" */
    while (g_running) {
        int sw = 0, sh = 0, go = 0, i;
        long t0 = now_ms();
        /* 区域表被改过（含面板自身改表：删/改名 —— WS 命令族只有 clear/list/add）→
         * 清掉引用了「已不存在 id」的面板状态。
         * 不做的话：gen_id 会自动复用被删掉的 r1/c1，而 g_hidden 里还留着旧 id ⇒ 新框出来的
         * 区域「生下来就是已隐藏」，而且 hide 行会被写进 regions.conf 一直带下去。 */
        if (g_prune_req) { g_prune_req = 0; prune_panel_refs(); }
        /* 落盘必须在这里（不在 draw_frame 里）：UI 关闭时下面会 continue，draw_frame 根本不跑，
         * 隐藏期间面板自身改的区域（WS 命令族只有 clear/list/add）就永远不落盘了。渲染线程是唯一写者；
         * 失败的重挂/退避在 save_regions() 内部统一处理（所有调用点一致）。 */
        if (g_save_pending && (g_save_retry_t == 0 || now_ms() >= g_save_retry_t)) {
            save_regions();
        }
        /* 操作表（ops.conf，T2.7）同款、**独立** pending/退避：两块文件各写各的，一块失败不拖另一块。 */
        if (g_ops_save_pending && (g_ops_save_retry_t == 0 || now_ms() >= g_ops_save_retry_t)) {
            save_ops();
        }
        /* 「关闭 UI」/恢复的日志：ui_show_cb 跑在 WS(触摸)线程上、不许在那里同步写日志，
         * 只置标志，由这里打（隐藏期间本循环仍在 30ms 跑，所以不会丢）。 */
        if (g_ui_log_pending >= 0) {
            int onp = g_ui_log_pending;
            g_ui_log_pending = -1;
            g_need = 1;
            ALOGI("ui %s (off=%d) t=+%.0fms", onp ? "show" : "hide", onp ? 0 : 1, (double)t_since_start());
        }
        pthread_mutex_lock(&g_mu);
        /* 待命槽（双图层）：为它单建 EGLSurface —— **不动可见槽**，所以屏幕一直有图。
         * 建好后由下面的"待命槽出帧"计数两帧，再请 Java 原子翻转。 */
        if (g_pending >= 0 && g_swap_win2[g_pending]) {
            int p = g_pending;
            if (g_pend_disp) { g_pend_disp = 0; on_display(g_pend_w, g_pend_h, g_pend_rot); }
            if (g_surf2[p] != EGL_NO_SURFACE) {
                eglMakeCurrent(g_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
                eglDestroySurface(g_dpy, g_surf2[p]); g_surf2[p] = EGL_NO_SURFACE;
            }
            if (g_win2[p]) ANativeWindow_release(g_win2[p]);
            g_win2[p] = g_win_new2[p]; g_win_new2[p] = 0;
            g_swap_win2[p] = 0;
            ANativeWindow_setBuffersGeometry(g_win2[p], 0, 0, WINDOW_FORMAT_RGBA_8888);
            g_surf2[p] = eglCreateWindowSurface(g_dpy, g_cfg, g_win2[p], 0);
            if (g_surf2[p] == EGL_NO_SURFACE) {
                ALOGE("待命槽 %d 建 EGLSurface 失败 (0x%x)", p, eglGetError());
                g_pending = -1;
            } else {
                g_pending_frames = 0;
                ALOGI("待命槽 %d 就绪（可见槽 %d 继续出图，屏幕不空）", p, g_cur);
            }
        }
        if (g_swap_win) {   /* 换 Surface（首次 / 换方向 / 换尺寸）：旧 EGL surface 必须销毁，
                             * 否则还挂在旧窗口上（尺寸还是旧的）。context/ImGui 都保留。 */
            if (g_pend_disp) {   /* 落位/朝向与新窗口**同时**生效（避免"新落位画进旧缓冲"那一帧） */
                g_pend_disp = 0;
                on_display(g_pend_w, g_pend_h, g_pend_rot);
            }
            if (g_surf != EGL_NO_SURFACE) {
                eglMakeCurrent(g_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
                eglDestroySurface(g_dpy, g_surf); g_surf = EGL_NO_SURFACE;
            }
            if (g_win) ANativeWindow_release(g_win);
            g_win = g_win_new; g_win_new = 0;
            g_swap_win = 0;
            /* 要等**两帧**成功提交再让 Java 恢复 alpha：只等一帧的话，新 surface 的首个缓冲
             * 可能还没被合成器取走，恢复后先显示一帧"未就绪"内容 —— 真机表现就是"窗口位置闪现"
             * （日志证据：恢复到首帧提交只隔 5ms）。多花一帧 ≈8ms，换"恢复时屏幕上那帧一定画好了"。 */
            g_swap_armed = 2;
            g_diag_frames = 8;     /* 接下来 8 帧逐帧打点（转屏定位用） */
            g_need = 1;
            g_force_frames = 4;
            ALOGI("surface swapped t=+%.0fms", (double)t_since_start());
        }
        snapshot_touches();
        region_rot_step();        /* 区域跟随旋转：每帧推进一条（编辑邮箱单槽，必须一条一拍） */
        ops_run_watch();          /* 操作运行状态变了 → 请求重画（「运行中 · 第 k/n 步」实时读核心） */
        int need_draw = g_need;   /* 显式请求的重画：不被下面的静止门吞掉 */
        go = g_need || g_ov_need;
        if (g_force_frames > 0) go = 1;
        /* 移动门限：dots 静止就跳过（位移才画），按钮/事件照常即时。
         * 之前 33ms 墙钟门限与 60Hz vsync 打架出 judder，已撤。 */
        /* overlay 藏起时：手指/闪烁/圈都不画（省整面合成），只留面板交互和事件日志 */
        if (g_ov_show) {
            for (i = 0; i < 64; i++) if (g_dots[i].on) { go = 1; break; }
        }
        if (g_mdown || g_mup_pend || g_need_mouse_evt) go = 1;
        int flash_fresh = 0, ring_fresh = 0;
        if (g_ov_show) {
            flash_fresh = (now_ms() - g_flash_t < 400 || now_ms() - g_ex_t < 350);
            if (flash_fresh) go = 1;
            for (i = 0; i < 8; i++)
                if (g_rings[i].on && now_ms() - g_rings[i].t < 400) { ring_fresh = 1; go = 1; break; }
        }
        g_need = 0;
        g_ov_need = 0;
        {
            /* 叠加层「由有到无」必须补一帧：圆环/手指点是逐帧重画才消失的，
             * 过期后若没有下一帧，屏上就留一圈很淡的残影（用户实测到过）。
             * 这一帧算 urgent，不受 VT_OV_FPS_MS 限频。 */
            int ov_active;
            if (g_ov_show) {
                ov_active = flash_fresh;
                for (i = 0; i < 8 && !ov_active; i++)
                    if (g_rings[i].on && now_ms() - g_rings[i].t < 400) ov_active = 1;
                for (i = 0; i < 64 && !ov_active; i++) if (g_dots[i].on) ov_active = 1;
                /* 取点标记（T3.2）同属「由有到无」：过期后补一帧擦掉（否则十字留在屏上） */
                if (!ov_active && g_pickmk_t && now_ms() - g_pickmk_t < PICK_MARK_MS) ov_active = 1;
            } else ov_active = 0;
            static int ov_was = 0;
            if (ov_was && !ov_active) { go = 1; g_force_frames = 1; }
            ov_was = ov_active;
        }
        /* 首帧保证：没提交过就一定要画（regions=0 且没人碰屏时，静止门会把唯一那次重画吞掉，
         * 结果是 buffer 一帧都没 queue、SF 报 nothing to draw，要等用户点一下屏才出现） */
        if (!g_drew_once) go = 1;
        /* 静止跳过：纯 dots 且位置签名不变就不画（按住不动零开销）；
         * 有位移每 tick 都画（mailbox 平滑），按钮/事件/闪烁/显式 g_need 照常即时。 */
        int urgent = go && (g_mdown || g_mup_pend || g_need_mouse_evt || flash_fresh || ring_fresh
                            || g_force_frames > 0 || !g_drew_once);
        static int last_sig = 0;
        int sig = 0;
        for (i = 0; i < 64; i++) if (g_dots[i].on) sig += g_dots[i].x * 3 + g_dots[i].y * 5 + i + 1;
        if (go && !urgent && !need_draw && sig == last_sig) go = 0;
        else last_sig = sig;
        /* 叠加层单独驱动的重画限频（VT_OV_FPS_MS，默认 33ms ≈ 30fps）：
         * 整屏一帧实测 ~2.4ms CPU + 一次全屏合成，手指在屏上/闪烁期间连画 60fps，
         * 在 120Hz 屏上会顶掉合成余量、游戏掉帧。限频不阻塞循环（下面走 10ms 轮询），
         * 所以按钮/拖动的响应延迟仍是 ≤10ms，不受影响。 */
        if (go && VT_OV_FPS_MS > 0 && g_drew_once && !need_draw
            && !g_mdown && !g_mup_pend && !g_need_mouse_evt && g_force_frames <= 0) {
            static long ov_t = 0;
            if (now_ms() - ov_t < VT_OV_FPS_MS) {
                go = 0;
                g_ov_need = 1;   /* 只是推迟，不丢请求：丢了就再也没人请求 → 该擦的擦不掉 */
            } else ov_t = now_ms();
        }
        if (g_win) { sw = ANativeWindow_getWidth(g_win); sh = ANativeWindow_getHeight(g_win); }
        if (!gl_ready && g_win) {
            if (egl_init_locked(g_win) == 0) {
                IMGUI_CHECKVERSION();
                ImGui::CreateContext();
                font_probe();
                ImGui::StyleColorsLight();
                apply_bento_style();
                ImGui_ImplOpenGL3_Init("#version 100");
                gl_ready = 1;
                egl_fail_n = 0;
                g_force_frames = 4;   /* 首帧连画几帧：兜住 EGL surface/SF 首次合成的竞态 */
                ALOGI("gl ready t=+%.0fms", (double)t_since_start());
            } else {
                if (now_ms() - egl_warn_t > 3000) {   /* 失败不刷屏：最多每 3s 一条，并让出 CPU */
                    egl_warn_t = now_ms();
                    ALOGE("egl init failed (0x%x) —— 后续重试，日志限频", eglGetError());
                }
                /* 「不能假装在跑」≠「杀掉后端」：EGL 起不来只意味着面板画不出来，而触摸采集/注入/
                 * 区域事件/WS 全都不依赖 EGL —— 这时退出等于把好用的后端一起干掉，还会放掉
                 * EVIOCGRAB（物理触摸回系统、脚本的注入也就没了）。所以这里只把话说清楚：
                 * 第 50 次升级成明确提示（含怎么停），之后每 30s 再报一行，然后继续重试。 */
                if (++egl_fail_n == 50)
                    ALOGE("EGL 连续失败 %d 次：面板 UI 画不出来（屏幕关闭 / 合成器异常？）—— "
                          "触摸、事件、注入都不受影响，继续重试；要停就 kill -9 $(pidof vtouch-ui)", egl_fail_n);
                else if (egl_fail_n > 50 && egl_fail_n % 150 == 0)
                    ALOGE("EGL 仍失败（第 %d 次）", egl_fail_n);
                pthread_mutex_unlock(&g_mu);
                usleep(200 * 1000);
                continue;
            }
        }
        if (g_surf != EGL_NO_SURFACE) {
            /* 绘制尺寸的**唯一可信来源**是 eglQuerySurface 的真实缓冲区尺寸。
             * 踩过的坑（探针实测）：ANativeWindow_getWidth/Height 返回的是图层 **default** 几何，
             * 旋转换绑之后会陈旧 → 拿它算坐标会把场景画成错位椭圆（横屏红"圆"变 439x791）。 */
            EGLint ew = 0, eh = 0;
            if (eglQuerySurface(g_dpy, g_surf, EGL_WIDTH, &ew) && eglQuerySurface(g_dpy, g_surf, EGL_HEIGHT, &eh)
                && ew > 0 && eh > 0) {
                sw = (int)ew; sh = (int)eh;
            }
        }
        if (gl_ready && g_surf == EGL_NO_SURFACE && g_win) {
            ANativeWindow_setBuffersGeometry(g_win, 0, 0, WINDOW_FORMAT_RGBA_8888);
            g_surf = eglCreateWindowSurface(g_dpy, g_cfg, g_win, 0);
            if (g_surf == EGL_NO_SURFACE) {
                if (now_ms() - egl_warn_t > 3000) {
                    egl_warn_t = now_ms();
                    ALOGE("eglCreateWindowSurface 失败 (0x%x)", eglGetError());
                }
                if (++egl_ws_fail_n == 50)   /* 同 init：只升级日志，不杀后端（见上面那段说明） */
                    ALOGE("eglCreateWindowSurface 连续失败 %d 次：面板 UI 画不出来（触摸/事件/注入不受影响，继续重试）", egl_ws_fail_n);
                else if (egl_ws_fail_n > 50 && egl_ws_fail_n % 150 == 0)
                    ALOGE("eglCreateWindowSurface 仍失败（第 %d 次）", egl_ws_fail_n);
                pthread_mutex_unlock(&g_mu);
                usleep(200 * 1000);
                continue;
            }
            egl_ws_fail_n = 0;
        }
        if (g_ui_off) {
            /* 关闭 UI：先把在途的交互状态一次收干净，再画一帧全透明清残影，之后彻底不画
             * （Java 侧把图层 alpha 置 0 不参与合成）。这些量都归渲染线程，所以在这里清 ——
             * 放到 ui_show_cb（WS 线程）里清才是新的竞态。主闸门是 snapshot_touches 的 ui_live。 */
            if (!g_off_cleared) {
                g_mdown = 0; g_mup_pend = 0; g_need_mouse_evt = 0; g_mslot = -1;
                g_drag = 0; g_scr_on = 0; g_scr_moved = 0; g_scroll_acc = 0;
                g_cap_slot = -1; g_edit_slot = -1; g_edit_kind = 0;
                if (gl_ready && g_surf != EGL_NO_SURFACE && sw > 0) {
                    glViewport(0, 0, sw, sh);
                    glClearColor(0, 0, 0, 0);
                    glClear(GL_COLOR_BUFFER_BIT);
                    if (!eglSwapBuffers(g_dpy, g_surf)) ALOGE("ui off 清帧提交失败 (0x%x)", eglGetError());
                    ALOGI("ui off cleared t=+%.0fms", (double)t_since_start());
                }
                g_off_cleared = 1;
            }
            pthread_mutex_unlock(&g_mu);
            usleep(30 * 1000);
            continue;
        }
        if (gl_ready && go && g_surf != EGL_NO_SURFACE && sw > 0) {
            long b0 = now_ms();
            draw_frame(sw, sh);
            if (!g_drew_once) {   /* 启动耗时锚点：第一帧已提交给 SF */
                g_drew_once = 1;
                ALOGI("first frame t=+%.0fms draw=%.0fms", (double)t_since_start(),
                      (double)(now_ms() - b0));
            }
            if (g_diag_frames > 0) {
                g_diag_frames--;
                ALOGI("diag 换绑后第%d帧: surf=%dx%d rot=%d scr=%dx%d pan=%.0f,%.0f panel=%.0fx%.0f logical=%dx%d",
                      8 - g_diag_frames, sw, sh, g_rot, g_scr_w, g_scr_h,
                      (double)g_pan_x, (double)g_pan_y, (double)panel_w(), (double)panel_h(), g_w, g_h);
            }
            /* 待命槽也画：画满两帧才请 Java 翻转（只画一帧就翻，合成器可能还没取走首个缓冲）。 */
            if (g_pending >= 0 && g_surf2[g_pending] != EGL_NO_SURFACE) {
                EGLint pw = 0, ph = 0;
                int save = g_cur;
                eglQuerySurface(g_dpy, g_surf2[g_pending], EGL_WIDTH, &pw);
                eglQuerySurface(g_dpy, g_surf2[g_pending], EGL_HEIGHT, &ph);
                if (pw > 0 && ph > 0) {
                    g_cur = g_pending;                 /* 让 draw_frame 里的宏指向待命槽 */
                    draw_frame((int)pw, (int)ph);
                    g_cur = save;
                    if (++g_pending_frames >= 2) {
                        g_flip_req = 1;
                        ALOGI("待命槽 %d 已画 %d 帧 → 请 Java 原子翻转", g_pending, g_pending_frames);
                    }
                }
            }
            long dt = now_ms() - b0;
            g_frame_ms = g_frame_ms * 0.8 + (double)dt * 0.2;
            if (g_swap_fail >= 100) {   /* ~1.6s 一帧都没上屏：不假装在跑，收干净退出让脚本看得见 */
                ALOGE("连续 %d 帧提交失败 → 面板退出，触摸回系统", g_swap_fail);
                vtouch_cleanup();
                _exit(0);
            }
            if (g_force_frames > 0) g_force_frames--;
            int more = g_mdown || g_mup_pend || g_need_mouse_evt;
            if (g_ov_show) {
                if (!more) for (int k = 0; k < 64; k++) if (g_dots[k].on) { more = 1; break; }
                if (!more && (now_ms() - g_flash_t < 400 || now_ms() - g_ex_t < 350)) more = 1;
                if (!more) for (int k = 0; k < 8; k++)
                    if (g_rings[k].on && now_ms() - g_rings[k].t < 400) { more = 1; break; }
            }
            if (more) g_ov_need = 1;
            long ft = now_ms() - t0;
            if (ft < 16) usleep((useconds_t)((16 - ft) * 1000));
        } else {
            pthread_mutex_unlock(&g_mu);
            usleep(10 * 1000);
            continue;
        }
        pthread_mutex_unlock(&g_mu);
    }
    return 0;
}

#ifdef VT_TEST_EV
/* 仅测试构建（-DVT_TEST_EV）：每秒 10 次合成区域事件，模拟"手指在区域里动"的闪烁/日志负载。
 * 生产构建里这段代码不存在。用法：VTOUCH_TEST_EV=1 启动面板。 */
static void *test_ev_thread(void *)
{
    const char *v = getenv("VTOUCH_TEST_EV");
    int want, i = 0;
    if (!v) return NULL;
    want = atoi(v);                     /* 0 = 不停发；N = 发 N 条后停（用于量「圆环过期后那一帧擦除」） */
    for (;;) {
        char line[96];
        i++;
        snprintf(line, sizeof line, "region_ev r1 %s 0 %d %d", (i & 1) ? "down" : "up", 700 + (i % 40), 880);
        ui_ev_cb(line);
        if (want > 0 && i >= want) break;
        usleep(100 * 1000);
    }
    return NULL;
}
#endif

/* ---- JNI ---- */
extern "C" {

/* System.load 那一刻 = 进程启动锚点：日志里所有 t=+Nms 都相对它 */
JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *, void *)
{
    g_t0_ms = now_ms();
    return JNI_VERSION_1_6;
}

JNIEXPORT jint JNICALL Java_VTouchUI_nativeInit(JNIEnv *env, jclass, jint w, jint h)
{
    g_w = w; g_h = h;
    /* 初始按竖屏假设落位；Java 拿到真实 display 尺寸后会立刻调 nativeOnDisplay 校正 */
    g_scr_w = w; g_scr_h = h;
    g_pan_x = (float)(w - WIN_W - 40); if (g_pan_x < 0) g_pan_x = 0;
    g_pan_y = 200;
    /* 帧 0 之前的兜底拖动区（渲染后每帧由实区覆盖） */
    g_zone_title[0] = g_pan_x + PAD_X; g_zone_title[1] = g_pan_y + PAD_Y;
    g_zone_title[2] = g_pan_x + WIN_W - PAD_X; g_zone_title[3] = g_pan_y + PAD_Y + TITLE_H;
    char ws[16], hs[16];
    snprintf(ws, sizeof ws, "%d", w); snprintf(hs, sizeof hs, "%d", h);
    {   /* 4 个 UI 回调一次注册（字段顺序见 struct vtouch_hooks） */
        struct vtouch_hooks hooks = { ui_ev_cb, ui_region_changed, ui_consume_cb, ui_show_cb };
        vtouch_set_hooks(&hooks);
    }
    char *argv[] = {(char *)"vtouch-ui", (char *)"-w", ws, (char *)"-h", hs,
                    (char *)"-p", (char *)"27183", 0};
#ifdef VT_TEST_EV
    { pthread_t th; if (pthread_create(&th, NULL, test_ev_thread, NULL) == 0) pthread_detach(th); }
#endif
    if (vtouch_init(7, argv) != 0) return -2;
    load_regions();   /* 上次落盘的表（regions.conf），没有则空表 */
    load_ops();       /* 上次落盘的操作表（ops.conf）：只补缺（核心已有同名不动）；坏记录单条跳过 */
    ALOGI("panel init %dx%d regions=%d t=+%.0fms", w, h, vtouch_region_count(),
          (double)t_since_start());
    g_poll_on = 1;
    if (pthread_create(&g_render_th, 0, render_thread_fn, 0) != 0) {
        ALOGE("render 线程创建失败: %s", strerror(errno));
        return -3;                       /* 别返回 0：Java 会以为面板起来了 */
    }
    if (g_poll_on && pthread_create(&g_poll_th, 0, poll_thread_fn, 0) != 0) {
        ALOGE("poll 线程创建失败: %s", strerror(errno));
        g_running = 0; pthread_join(g_render_th, 0);
        return -3;
    }
    return 0;
}

/* Java 送来某个槽的新 Surface（0/1 = 槽号）。非当前槽 = 双图层模式下的"待命槽"：
 * 渲染线程会为它单建一个 EGLSurface 并先画两帧（此期间可见槽照常出图，屏幕不空）。 */
JNIEXPORT void JNICALL Java_VTouchUI_nativeOnSurface(JNIEnv *env, jclass, jint id, jobject surf)
{
    ANativeWindow *w;
    if (id < 0 || id > 1) return;
    w = ANativeWindow_fromSurface(env, surf);
    if (!w) { ALOGE("fromSurface 失败"); return; }
    pthread_mutex_lock(&g_mu);
    if (g_win_new2[id]) ANativeWindow_release(g_win_new2[id]);
    g_win_new2[id] = w;
    g_swap_win2[id] = 1;
    if (id != g_cur) { g_pending = id; g_pending_frames = 0; g_flip_req = 0; }
    g_need = 1;
    pthread_mutex_unlock(&g_mu);
    ALOGI("surface ready slot=%d t=+%.0fms", id, (double)t_since_start());
}

/* Java 做完原子翻转后调它，告诉 native"现在可见的是哪个槽"。 */
JNIEXPORT void JNICALL Java_VTouchUI_nativeOnFlip(JNIEnv *, jclass, jint slot)
{
    pthread_mutex_lock(&g_mu);
    if (slot >= 0 && slot <= 1) {
        g_cur = slot;
        g_pending = -1;
        g_pending_frames = 0;
        g_flip_req = 0;
        g_need = 1;
    }
    pthread_mutex_unlock(&g_mu);
    ALOGI("flip done → 可见槽=%d", g_cur);
}

/* Java 检测到方向/尺寸变化就调它（随后会再送一个新 Surface）。
 *
 * **立刻生效**（不延迟到换绑）：Java 的 DisplayListener 回调早于真实状态更新，所以回调里会先用
 * 预测值调一次这里（尺寸交换、方向 +1），让"屏幕转过去的那一刻"面板已经在新落位 —— 用户要的
 * "位置切换自然"就是这么来的；真值到了再调一次做校正（猜错也在遮挡里，看不见）。
 * 但**不当场改落位**：落位/朝向改在"某个槽接手新窗口的那一刻"生效（当前槽与待命槽两条路径都
 * 会应用它）。否则可见槽会用新落位去画旧尺寸缓冲 —— 真机实测过那一帧：形状正常、位置不对，
 * 也就是用户看到的"窗口位置闪现"。 */
JNIEXPORT void JNICALL Java_VTouchUI_nativeOnDisplay(JNIEnv *, jclass, jint w, jint h, jint rot)
{
    g_pend_w = w; g_pend_h = h; g_pend_rot = rot;
    __sync_synchronize();
    g_pend_disp = 1;
}

JNIEXPORT void JNICALL Java_VTouchUI_nativeDestroy(JNIEnv *, jclass)
{
    g_running = 0;
    pthread_join(g_render_th, 0);
    if (g_poll_on) pthread_join(g_poll_th, 0);
    vtouch_cleanup();
}

/* Java 主循环轮询它：图层要不要显示（「关闭 UI」时 native 置 0，Java 把图层 alpha 归 0）。 */
JNIEXPORT jint JNICALL Java_VTouchUI_nativeWantLayerVisible(JNIEnv *, jclass)
{
    return g_want_layer;
}

/* Java 主循环轮询它：转屏遮挡期间"新 surface 首帧提交了没有"。读到即清零（一次性事件）。 */
JNIEXPORT jint JNICALL Java_VTouchUI_nativeTakeSwapDone(JNIEnv *, jclass)
{
    /* 两种模式共用一个"就绪"信号：单图层换绑（g_swap_done）/ 双图层待命槽（g_flip_req）。 */
    int v = g_swap_done || g_flip_req;
    g_swap_done = 0;
    g_flip_req = 0;
    return v;
}

} /* extern "C" */
