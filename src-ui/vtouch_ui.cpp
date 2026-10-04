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
#include <dirent.h>
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
#include "vt_vision.h"  /* T3.2：灰度公式单一来源（vt_vis_gray_px；.tmpl 写端与引擎逐字同款）—— build_ui.sh 加 -Isrc */

/* 区域 id 上限（= 核心 src/vt_internal.h 的 REGION_ID_MAX；面板不 include 核心头，独立定义）——
 * vtouch_op_put 的每步区域引用表与编辑层本地 ref 副本（g_ope_refs）按它定宽。 */
#define REGION_ID_MAX 15
/* 计算步表达式上限（= 核心 src/vt_internal.h 的 VT_EXPR_MAX；同上独立定义）——
 * vtouch_op_put 的每步表达式表按它定宽（3.1 的编辑层本地副本 g_ope_exprs 同款）。 */
#define VT_EXPR_MAX 63

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
 * 已接线；绑定读/写（trig/bind/kind）T3.3 已接线；v5 表达式校验（expr_check）T3.1 接线。 */
int  vtouch_op_count(void);
int  vtouch_get_op(int i, char *name, int n, int *steps, char *gate, int gn, int *autoff);
int  vtouch_get_op_step(int i, int s, int *type, int *a1, int *a2, int *a3, int *a4, int *ms, char *ref, int refn,
                        int *j1, int *j2, char *expr, int exprn);   /* j1/j2 = 条件步跳转目标（成立/不成立侧；可 NULL）；expr = 计算步表达式（v5；可 NULL） */
int  vtouch_op_put(const char *name, const char *gate, int autoff, const int *steps8,
                   const char (*refs)[REGION_ID_MAX + 1], const char (*exprs)[VT_EXPR_MAX + 1], int nsteps, int *out_err);   /* steps8 = flat 8/步（t,a1..a4,ms,j1,j2）；refs/exprs 可 NULL = 全空；每步空串 = 无；out_err 可 NULL */
int  vtouch_op_del(const char *name);
void vtouch_op_clear(void);
int  vtouch_op_run(const char *name);
void vtouch_op_stop(void);
int  vtouch_op_status(int *run_i, int *run_step, int *run_state);
int  vtouch_expr_check(const char *s, char *why, int whycap);   /* v5：计算步表达式校验（转发核心 vt_expr_check；0 = 合法、非 0 = why 填短中文原因） */
void vtouch_pick_request(void);
void vtouch_pick_cancel(void);
int  vtouch_pick_take(int *x, int *y);          /* 1 = 有新坐标 */
int  vtouch_region_kind_get(int i);
int  vtouch_region_toggle(int i);
int  vtouch_region_trig(int i, char *op, int n, int *ev);
int  vtouch_region_bind(const char *id, const char *opname, int ev);   /* 触发绑定（opname "-"/空 = 解除） */
int  vtouch_region_kind(const char *id, int kind);                     /* 开关型（0=普通 1=开关型） */
/* 视觉模板/点集（T3.2）：面板侧抓帧旁路（请求 → 面板侧缓冲取帧/取错；不进共享内存、不动帧区协议）。
 * 定义见 src-ui/ui_glue.c（单跑模式见 src-ui/ui_stubs.c）。 */
void vtouch_vis_panel_capture_req(void);
int  vtouch_vis_panel_frame_take(int *w, int *h, int *rot, const unsigned char **buf);
int  vtouch_vis_panel_err_take(int *err);
/* 试查（Task 7.1）：「试一下」→ 核心执行一次查找（投递/取结果；定义见 src-ui/ui_glue.c，
 * 单跑模式见 src-ui/ui_stubs.c）。 */
unsigned vtouch_vis_test_post(int kind, const char *ref, const char *region, int a1, int a2);
int      vtouch_vis_test_take(unsigned *seq, int *x, int *y, int *err);
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

/* 面板几何（唯一来源：下面 #define + panel_w()/panel_h()（定义在 g_scr 声明之后）+ in_panel() + build_panel() 同公式）
 * sidebar-fixed 骨架：固定侧栏 w-64(256) 不随内容滚，内容页自己滚。 */
static float g_pan_x = 780, g_pan_y = 200;
static int g_sheet = 1;                   /* 内容页开/合（合 = 只留侧栏） */
static int g_min = 0;                     /* 收起态：整窗只剩一条标题栏（会话内有效，重启展开） */
static int g_nav = 0;                     /* 0=区域列表 1=操作 2=事件日志 3=设置 4=说明 5=方案 */
#define PAD_X 16
#define PAD_Y 12
#define TITLE_H 88
#define SIDE_W 256
#define SHEET_W 560
#define COL_GAP 12
#define WIN_W (PAD_X * 2 + SIDE_W + COL_GAP + SHEET_W)   /* 864 */
#define SIDE_ONLY_W (PAD_X * 2 + SIDE_W)                 /* 288 */
#define WIN_H 1180
#define WIN_Y_REF 200                                    /* 默认顶距（高度上限按它折算） */
#define WIN_BOTTOM_PAD 32                                /* 窗口底与屏底的留白 */
#define WIN_MIN_H 420                                    /* 矮屏保底高度（低于此不再压） */
#define MINI_W 224                                       /* 收起态悬浮小条宽（越小越不显眼） */
#define MINI_H 68                                        /* 收起态条高 = 44 按钮 + 上下 12 */
#define ZINC50  ImVec4(0.980f, 0.980f, 0.984f, 1.00f)   /* sidebar-fixed bg-zinc-50 */
#define ZINC100 ImVec4(0.957f, 0.957f, 0.961f, 1.00f)
#define ZINC200 ImVec4(0.894f, 0.894f, 0.906f, 1.00f)   /* border-zinc-200 */
#define ZINC900 ImVec4(0.094f, 0.094f, 0.106f, 1.00f)
#define WHITE   ImVec4(1.000f, 1.000f, 1.000f, 1.00f)
#define BLUE500 ImVec4(0.231f, 0.510f, 0.965f, 1.00f)   /* sidebar-fixed 主色 */
#define RED600  ImVec4(0.863f, 0.149f, 0.149f, 1.00f)
/* 预览小地图标记色（T2.5）：点按 = 蓝（同 BLUE500 的 U32 形）/ 按下 = 橙（同取点十字标记）/ 判定点 = 紫 */
#define PV_BLUE   IM_COL32(59, 130, 246, 255)
#define PV_ORANGE IM_COL32(255, 140, 0, 255)
#define PV_PURPLE IM_COL32(147, 51, 234, 255)

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
/* 试查（Task 7.1「试一下」）：找图/找色步骤参数层 + 模板页发起 → 核心执行一次查找 → 结果行 +
 * 屏幕标记。整条链都在渲染线程（点击、轮询、绘制同线程；等待期间每拍轮询 test_res_seq，≤10ms）。 */
static int g_vis_test_on = 0;            /* 请求在途（等待结果） */
static unsigned g_vis_test_seq = 0;      /* 本次请求序号（与核心结果序号比对认领） */
static long g_vis_test_t0 = 0;           /* 发起时刻（now_ms()；1.5s 超时判据） */
static int g_vis_test_err = 0;           /* 结果码：0 命中 / -1 未命中 / 1..4 错误码（核心）/ 面板内部态见下 */
static char g_vis_test_msg[96] = {0};    /* 结果行文本（固定槽显示） */
#define VIS_TEST_TMO_MS 1500             /* 等待超时（1.5s 无响应 →「超时（无响应）」） */
#define VIS_TEST_WAIT   (-2)             /* 面板内部显示态：等待结果 */
#define VIS_TEST_TMO    (-3)             /* 面板内部显示态：超时（无响应） */
#define VIS_TEST_NOCORE (-4)             /* 面板内部显示态：没接核心 */
/* 试查命中标记（Task 7.1）：命中后在命中点画 ~2 秒方框 + 坐标文字（找图 = 模板 w×h 按方向映射、
 * 找色 = 固定 80×80 居中）；纯绘制 —— 不参与命中、不吞触摸、不写共享内存（同取点标记口径）。 */
static int g_vis_testmk_x = 0, g_vis_testmk_y = 0;
static long g_vis_testmk_t = 0;          /* 命中时刻（now_ms()）；0 = 无标记 */
static int g_vis_testmk_kind = 0;        /* 0 = 找图 / 1|2 = 找色（方框口径不同） */
static int g_vis_testmk_tw = 80, g_vis_testmk_th = 80, g_vis_testmk_trot = 0;   /* 模板 w/h + 抓取方向（找色 = 80/80） */
#define VIS_TEST_MARK_MS 2000            /* 标记存活时长（~2 秒，同取点标记） */
/* 编辑层（T2.6/T2.4）里「渲染与吞触摸都要读」的三个标量定义在这里（g_ope_ 一族其余在 T2.6 区块）：
 * 文件前段的快照/吞触摸判据（ui_rect_now / snapshot_touches）要用它们 —— C++ 变量不能像函数那样
 * 先声明后定义（后置带初值的定义会判重定义），所以把定义搬前。
 * 视觉（T3.2）一族同理由：ui_rect_now（采集覆盖层整屏吞）与编辑层开关（op_edit_close/open）都要读。 */
static int g_ope_i = -1;                 /* 正在编辑的操作下标（-1 = 编辑层关；三态矩形的判据） */
static int g_ope_coll = 0;               /* 编辑层收起态（取点自动收起 / 手动收起共用；1 = 收成底部条） */
#define VIS_PTS_MAX 16                   /* 点集参考点上限（= 核心 VT_VIS_PTS_MAX；面板不 include 核心头） */
static int g_vis_cap = 0;                /* 采集覆盖层（T3.2）：0=关 1=模板框选 2=点集编辑 3=吸色（找色步 [取点]） */
static int g_vis_cap_wait = 0;           /* 等帧中（请求已发） */
static int g_vis_cap_err = 0;            /* 抓帧失败码（显示用；-1 = 3s 超时哨兵） */
static long g_vis_cap_t0 = 0;            /* 请求时刻（超时判据；now_ms()） */
static const unsigned char *g_vis_img = 0;   /* 面板侧帧缓冲（ui_glue.c 内；只读，紧排 w*4） */
static int g_vis_img_w = 0, g_vis_img_h = 0, g_vis_img_rot = 0;
static int g_vis_drag = 0;               /* 采集层拖动中（模板框选 / 点选轻点判据共用） */
static int g_vis_gest = 0;               /* 采集层手势（Task 7.2）：0 无/待定 1 新框 2 移动 3 角缩放 4 平移 */
static int g_vis_gest_corner = 0;        /* 角缩放：0..3 = 左上/右上/左下/右下 */
static int g_vis_gest_box[4] = {0, 0, 0, 0};   /* 手势起点框（帧坐标；移动/角缩放用） */
static float g_vis_zoom = 1.0f;          /* 采集层缩放（Task 7.2）：1x..8x（1 = 适应）；步进 0.25 */
static float g_vis_pan_x = 0, g_vis_pan_y = 0;   /* 采集层平移（屏像素；缩放 >1x 时有效；锚点 = 画面中心） */
static float g_vis_pmx = 0, g_vis_pmy = 0;       /* 平移增量基准（屏坐标） */
static int g_vis_magf = -1;              /* 已应用放大滤波（-1 未设 0 线性 1 最近邻；纹理重建后复位） */
static float g_vis_dx0 = 0, g_vis_dy0 = 0;   /* 拖动起点（屏坐标） */
static int g_vis_sel[4] = {0, 0, 0, 0};  /* 已定框选（帧坐标 x0,y0,x1,y1 含端点） */
static int g_vis_sel_on = 0;             /* 框选有效 */
static int g_vis_base_x = -1, g_vis_base_y = -1;   /* 点集基准点（帧坐标；-1 = 未选） */
static uint32_t g_vis_base_rgb = 0;      /* 点集基准色 */
static int g_vis_base_tol = 8;           /* 基准容差（默认 8；写盘时每点 tol = 它，v1 不逐点编辑） */
static int g_vis_pts_n = 0;              /* 参考点数（≤ VIS_PTS_MAX） */
static int g_vis_pts_x[VIS_PTS_MAX], g_vis_pts_y[VIS_PTS_MAX];
static uint32_t g_vis_pts_rgb[VIS_PTS_MAX];
static char g_vis_cap_msg[72] = {0};     /* 采集层就地提示（上限等） */
static int g_vis_kb = 0;                 /* 采集层命名键盘子层：0=关 1=模板 2=点集 */
static char g_vis_kb_buf[16] = {0}, g_vis_kb_msg[72] = {0};
static int g_vis_kb_up = 0;
static int g_vis_pick_se = -1;           /* 吸色回填目标步（-1 = 非吸色会话） */
static int g_vis_ed = 0;                 /* 视觉步参数层（编辑层子层）：0=关 1=找图 2=找色（g_ope_se = 步号） */
static int g_vis_num = 0;                /* 数字键盘子层（从视觉参数层进；复用 draw_num_edit） */
static int g_vis_tl = 0;                 /* 模板列表子层 */
static int g_vis_pl = 0;                 /* 点集列表子层 */
static int g_vis_hex = 0;                /* 颜色十六进制键盘子层 */
static char g_vis_hexbuf[8] = {0}, g_vis_hexmsg[72] = {0};
static int g_vis_hexup = 0;
static char g_vis_edmsg[96] = {0};       /* 视觉参数层就地提示 */
static unsigned int g_vis_tex = 0;       /* 采集图像 GL 纹理（0 = 未建；渲染线程独占） */
static int g_vis_tex_w = 0, g_vis_tex_h = 0;
static int g_vis_tex_dirty = 0;          /* 新帧到达 → 下一帧上传 */
static long g_pick_t0 = 0;               /* 取点发起时刻（now_ms()；0 = 无）—— 面板自带 20s 兜底计时 */
/* 取点弹回抑制（T2.4 修复轮 1，评审 Important 1）：编辑层「收起→展开」跃迁的那一帧 / 条上取消那一下，
 * 还按着的手指一律抑制到抬起（1 = 该 slot 不喂 ImGui）—— 否则矩形回整屏后会被快照锁存成面板鼠标
 * （1315 不要求 down_edge），抬手时在弹回层里发一次真点击（取点取消 / 回填 / 20s 超时三路通吃）。 */
static int g_pick_finger[64];
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
#define SCR_SHEET 3                       /* 内容页 */
#define SCR_KB 4                         /* 名称/数字键盘（内容超高时手动拖滚） */
static int g_scr_target = SCR_NONE;      /* 按下时按实区锁定滚动容器 */
static volatile float g_dbg_scroll = 0;  /* 最近一次应用到的滚动位置（日志用） */
static float g_kb_sc = 0;                /* 键盘（名称/数字）手动滚动偏移：内容超出可用高时生效 */
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
static volatile float g_zone_title[4], g_zone_side[4], g_zone_sheet[4], g_zone_list[4], g_zone_kb[4];
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
 * 换算公式与坐标契约同一套（r 语义同 Android getRotation）。 */
static volatile int g_rot = 0;        /* 0/1/2/3：Java 线程写、poll 线程的吞触摸谓词无锁读 ⇒ 必须 volatile */
static volatile int g_scr_w = 0, g_scr_h = 0;  /* 当前方向屏幕尺寸（= 图层 buffer 尺寸） */
static volatile long g_rot_settle_t = 0;       /* 转屏后「不吞触摸」的稳定窗口截止时刻 */
static int g_pan_moved = 0;           /* 用户拖过面板：换方向时不再自动回右上角 */

/* 面板几何（宽度公式见上面 #define；高度 = 设计高 WIN_H，矮屏自动压缩）。
 * 高度必须与「实际窗口尺寸 / 命中判定 / 吞触摸矩形」同源 —— 三处都调这里：
 * 屏幕比 WIN_H 矮（横屏 / 小屏）时压进「屏高 − 默认顶距 − 底部留白」，保底 WIN_MIN_H；
 * 内容页全部按运行时窗口高自适应（wh = GetWindowHeight、列表填满剩余空间 + 内部滚动），
 * 压矮后自动缩短，底边不再出屏。收起态（MINI_H）不变。 */
static float panel_w(void)
{
    float w = (float)(g_min ? MINI_W : (g_sheet ? WIN_W : SIDE_ONLY_W));
    if (!g_min && g_scr_w > 0) {   /* 窄屏（小分辨率）：面板宽压进屏内（sheet 子窗填满剩余宽，自动跟随） */
        float cap = (float)g_scr_w - 24.0f;
        if (w > cap) w = cap;
    }
    return w;
}
static float panel_h(void)
{
    if (g_min) return (float)MINI_H;
    float h = (float)WIN_H;
    if (g_scr_h > 0) {
        float avail = (float)g_scr_h - WIN_Y_REF - WIN_BOTTOM_PAD;
        if (avail < WIN_MIN_H) avail = (float)WIN_MIN_H;
        if (h > avail) h = avail;
    }
    return h;
}
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
 * 开关：**默认启用**（2026-10-03 用户口径「区域应当始终和当前方向的左上角保持同一 xy」——跟随视口；
 * 覆盖 2026-09-19 的「已设区域不因非人为操作改变」：核心零改动时，xy 不变式只能靠重算写回保证，
 * 且必须写回才能让「显示的位置 = 检测的位置」同时成立）。VTOUCH_REGION_ROT=off 回到「粘在玻璃上」档。
 * 写回与重启：基准帧随重算批推进、落盘 #frame 如实记录 —— 重启后按同一帧解释，不偏移。 */
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
 * 默认**启用**（`g_rr_off`，2026-10-03 用户口径「区域应当始终和当前方向的左上角保持同一 xy」）：
 * 这条路径会把区域坐标「写回」核心表与 regions.conf（触发者是转屏/启动这类非人为事件）——
 * 写回是必须的：核心零改动的前提下，只有表里的几何跟着方向走，才能同时保证
 * 「以当前方向左上角为原点的 xy 不变」与「显示的位置 = 检测的位置」。VTOUCH_REGION_ROT=off 回旧档。
 * ⚠️ 基准必须固定成**竖屏帧**（区域表 / regions.conf 的规范坐标系，也是旧文件的约定），
 * **不能**记成「面板启动时看到的方向」—— 否则启动方向不同，同一份表会被解释成不同的相对位置
 * （真机报过：同一批区域，横屏启动与竖屏启动显示在不同的相对位置）。
 * 空表时基准跟住当前屏（见下方分支）：第一条画进来的区域锚在当前帧，基准停旧帧会把它错算一次。 */
static void region_rot_step(void)
{
    char id[16];
    int n1, n2, n3, n4, n;      /* 2026-09-18 快照重构后 type/a1..a4/en 不再直用（-Wall 清理） */
    /* 2026-10-03 用户口径：「区域应当始终和当前方向的左上角保持同一 xy」= 跟随视口 ⇒ **默认启用**。
     * 该口径覆盖 2026-09-19 的「已设区域不因非人为操作改变」——两者不可兼得（核心零改动时，只有写回
     * 才能让「视口 xy 不变」与「显示=命中」同时成立）；当时叫停的另一半原因（不自洽屏帧 / #frame 归属
     * 错判 → 算到屏外且重启不自愈）已由 frame_sane 门、批首快照状态机、失败重跑、#frame 如实落盘封住。 */
    if (g_rr_off < 0) {
        const char *v = getenv("VTOUCH_REGION_ROT");
        g_rr_off = (v && !strcmp(v, "off")) ? 1 : 0;
        ALOGI(g_rr_off ? "区域跟随旋转：停用（VTOUCH_REGION_ROT=off，区域粘在玻璃上、坐标不动）"
                       : "区域跟随旋转：启用（默认；区域始终与当前方向左上角保持同一 xy，转屏重算写回）");
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
     * 等 Java 侧 500ms 观察窗给到自洽值（通常 5~10ms 后；这段短暂错位被双图层翻转遮住）。 */
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
        if (n <= 0) {
            /* 空表：基准跟住当前屏 —— 之后第一条画进来的区域锚在当前帧（框选/拖动都按当屏换算存值），
             * 基准若停在旧帧，下一帧会把它当旧帧错算一次（场景：清空 / 新方案后横屏画第一笔）。
             * 敢在这里推进的理由：载入路径（load_regions）总是**先把基准定死**（#frame 或竖屏规范帧）
             * 再投 add，且 add 等生效才返回（ui_glue.c 的 glue_post 等 edit_applied）——
             * 与渲染循环同线程的这里观察不到「载入中」的空表。 */
            g_rr_base_rot = g_rot; g_rr_base_w = g_scr_w; g_rr_base_h = g_scr_h;
            return;
        }
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
            g_pan_x = (float)(w - panel_w() - 40); if (g_pan_x < 0) g_pan_x = 0;
            g_pan_y = 200;
            {   /* 矮屏（横屏 / 小屏）：默认顶距放不下整窗时上移，尽量多留高度（保底 16） */
                float ny = (float)h - panel_h() - WIN_BOTTOM_PAD;
                if (ny < 16) ny = 16;
                if (g_pan_y > ny) g_pan_y = ny;
            }
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
/* 方案镜像钩子（v4 T1.1「方案文件层」段，定义见下）：两个 save 的落盘成功出口各调一次 */
static void scheme_mirror(void);
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
    /* #frame：记「下面那些 region 行的数字**属于哪个屏帧**」—— 载入端拿它当跟随旋转的基准帧
     * （见 load_regions 的 #frame 分支与 region_rot_step）。基准随重算批推进（批内写盘被推迟、
     * 收尾后才落），这里如实记录同一值：
     *   · 基准是竖屏（缺省 / 从未转过）→ 输出与旧行为一致（rot0 + 竖屏逻辑尺寸）；
     *   · 基准被推进过（如横屏用过 → rot1 3168x1440）→ 记 rot1：重启后按同一帧解释，
     *     **不会被当竖屏多算一次**（2026-10-03 修的「横屏用过之后重启偏移」缝）。
     * 老文件里的 `#frame 1 …` 照读照留 —— 它是那批数字的解释依据（拖动/重画会按当前帧重新锚定）。 */
    if (g_w > 0 && g_h > 0) {
        int br = 0, bw = g_w, bh = g_h;
        if (g_rr_base_w > 0) { br = g_rr_base_rot; bw = g_rr_base_w; bh = g_rr_base_h; }
        fprintf(f, "#frame %d %d %d\n", br, bw, bh);
    }
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
    scheme_mirror();         /* v4：镜像 live → schemes/<current>（失败告警不阻塞，见「方案文件层」） */
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
    /* v4 T1.2 可重入（切换期会再跑一遍）：hide 表是 regions.conf 的**派生态**（文件 = 唯一来源）——
     * 每次载入先复位、再按文件重建。启动时本就为 0（行为不变）；不复位的话二次载入是追加式，
     * 新文件里没有的旧 hide 条目会残留（旧方案隐藏态泄漏进新方案）。 */
    g_nhide = 0;
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
                ALOGI("regions.conf #frame rot%d %dx%d（表里数字所属的屏帧 —— 跟随旋转的基准帧，转屏按它换算）",
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
    /* 无 #frame（旧文件）：**显式钉死**竖屏规范帧 —— 载入是基准帧的权威来源（region_rot_step 的空表
     * 分支会把基准推向当前屏；这里不钉的话，横屏启动时这批数字会被当成当前帧锚而错位）。 */
    if (!frame_seen && g_w > 0 && g_h > 0) { g_rr_base_rot = 0; g_rr_base_w = g_w; g_rr_base_h = g_h; }
    if (!frame_seen && nreg > 0)
        ALOGI("区域跟随旋转：regions.conf 无 #frame（旧文件，%d 条）→ 按**竖屏规范帧**解释；"
              "若这批区域其实是横屏时加的，可用 VTOUCH_REGION_BASE=rot,w,h 指定基准帧（或删除重画）",
              nreg);
    if (nreg > 0 && nskip)
        ALOGI("regions.conf 共 %d 条：补入 %d 条、跳过 %d 条（核心表里已有 ⇒ 不覆盖现役几何）",
              nreg, nreg - nskip, nskip);
    if (migrated) save_regions();     /* 迁移完立刻写回持久路径（老 tmpfs 文件留着无害） */
}

/* 步骤类型 / 条件档位 / 变量编码：与核心常量（src/vt_internal.h）同值；面板不 include 核心头，独立定义。
 * 位置注定在这里（ops.conf 段之前）：load_ops 的**类型感知翻译**（旧行条件步 a4 缺省 → 继续）也用它。 */
#define OP_STEP_TAP   1
#define OP_STEP_SWIPE 2
#define OP_STEP_WAIT  3
#define OP_STEP_DOWN        4          /* 按下（按下并保持） */
#define OP_STEP_UP          5          /* 弹起（松开当前按住的手指） */
#define OP_STEP_COND_REGION 6          /* 区域判断（a1,a2 的点 ∈ ref 区域） */
#define OP_STEP_COND_TOGGLE 7          /* 开关判断（ref 区域须开关型且开着） */
#define OP_STEP_JUMP        8          /* 跳转（a1 = 目标步骤：0 = 结束、1..步数 = 目标） */
#define OP_STEP_CALC        9          /* 计算（v5；a1 = 结果槽 1..4、expr = 表达式） */
#define OP_STEP_FINDIMAGE  10          /* 找图（T3.2 v8；ref=模板名、expr=区域名（空=全屏）、a1=阈值 0..255、
                                        * a2=0、a3/a4=不成立/成立档、j1/j2=该侧目标） */
#define OP_STEP_FINDCOLOR  11          /* 找色（T3.2 v8；a1=模式 0 单点/1 多点；ref=点集名（多点必填/单点必空）；
                                        * expr=区域名；单点 a2=(颜色<<8)|容差、多点 a2=0；a3/a4/j1/j2 同上） */
#define OP_COND_ABORT       0          /* 档位：中止（条件步成立/不成立侧共用；不成立侧默认） */
#define OP_COND_SKIP        1          /* 档位：跳过下一步 */
#define OP_COND_CONT        2          /* 档位：继续下一步（成立侧默认） */
#define OP_COND_JUMP        3          /* 档位：跳到…（目标 = 该侧 j1/j2：0 = 结束；跑前守卫兜底） */
#define OP_VAR_TDX (-1)                /* 变量编码：-1..-5 = 触发按下x / 触发按下y / 触发弹起x / 触发弹起y / 触发时长 */
#define OP_VAR_TDY (-2)
#define OP_VAR_TUX (-3)
#define OP_VAR_TUY (-4)
#define OP_VAR_TMS (-5)
#define OP_VAR_R1  (-6)                /* 结果槽编码（v5）：-6..-9 = r1..r4（同核心 OP_VAR_R1..R4；变量域 = -9..-1） */
#define OP_VAR_R2  (-7)
#define OP_VAR_R3  (-8)
#define OP_VAR_R4  (-9)

/* ---- 操作表落盘（ops.conf，T2.7；v2 T3.3；v3 T2.1；v4 T3.1 v5）------------------------------------
 * 格式（docs/OPS_PLAN_V5.md §3，逐字）：`#vtouch-ops v4` 起头；一条操作 = op 行 + N 条 step 行 ——
 *   op <名> gate <门控区域id|-> autooff <0|1>
 *   step <type> <a1> <a2> <a3> <a4> <ms> <ref> <j1> <j2> <expr>
 * ref / expr 空写 `-`（读回还原空串）；变量照写负数（如 -1）。读端兼容 v1（6 字段行）/ v2（7 字段行）/
 * v3（9 字段行）：缺省 j1=j2=0、expr 空；**类型感知翻译**：旧行的条件步（t=6/7）成立档 a4 缺省 →
 * OP_COND_CONT（继续下一步，老文件里 a4=0 只是「没有该字段」）；非条件步照读——尤其滑动步 a3/a4 =
 * 终点坐标，绝不能动。v4 的 expr 可含空白（照写）⇒ 读端取 j2 之后的行尾整段（`-` = 空）。
 * 保存：.tmp + rename（同 save_regions，掉电不会留半截文件）；失败挂 g_ops_save_pending、
 * 1s 后退避重试（节奏同 save_failed）；成功一行「ops.conf 已存 N 条」。
 * 加载：版本门（v1/v2/v3 兼容读入 / v4 本格式；其余 = 整份跳过 + 改写当前表，同 load_regions 的丢弃清空口径）；
 * 只补缺（核心表已有同名 → 跳过，不覆盖现役定义）；一条坏记录只警告并继续，不带走全表。 */
#define OPS_CONF_VER  4
#define OPS_CONF_FILE REGION_CONF_DIR "/ops.conf"
#define OPS_MAX_STEPS 32        /* 同核心 MAX_STEPS / 编辑层 OPE_MAX_STEPS（面板不 include 核心头） */
#define OPS_EXPR_MAX  63        /* 计算步表达式上限（同核心 VT_EXPR_MAX；编辑层 g_ope_exprs 与 conf v4 第 10 字段按它定宽） */
static_assert(OPS_EXPR_MAX == VT_EXPR_MAX, "同核心 VT_EXPR_MAX");

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
            int t, a1, a2, a3, a4, ms, j1, j2;
            char ref[REGION_ID_MAX + 1], expr[OPS_EXPR_MAX + 1];
            if (vtouch_get_op_step(i, s, &t, &a1, &a2, &a3, &a4, &ms, ref, sizeof ref, &j1, &j2, expr, sizeof expr) != 0) continue;
            /* ref / expr 空 = 无引用 → 写占位符 `-`（读回时还原空串，同 gate 口径）；变量（负数）照写；
             * j1/j2 = 条件步跳转目标（v3 第 8/9 字段，非条件步恒 0）；expr = 计算步表达式（v4 第 10 字段，照写） */
            fprintf(f, "step %d %d %d %d %d %d %s %d %d %s\n", t, a1, a2, a3, a4, ms, ref[0] ? ref : "-", j1, j2,
                    expr[0] ? expr : "-");
        }
    }
    if (fclose(f) != 0) { ALOGE("ops.conf 落盘失败: %s", strerror(errno)); return ops_save_failed(); }
    if (rename(tmppath, OPS_CONF_FILE) != 0) {
        ALOGE("ops.conf rename 失败: %s", strerror(errno));
        return ops_save_failed();
    }
    g_ops_save_pending = 0;      /* 只有真落盘成功才清请求（唯一写者=渲染线程 / 启动期主线程） */
    g_ops_save_retry_t = 0;
    scheme_mirror();             /* v4：镜像 live → schemes/<current>（失败告警不阻塞，见「方案文件层」） */
    ALOGI("ops.conf 已存 %d 条", n);
    return 0;
}
/* 结算一条从文件读到的记录（下一条 op 行 / EOF 时调用）。返回 0=补入 / 1=跳过（核心已有）/
 * -1=坏记录（已警告）。一条坏记录不影响后面的记录。 */
static int ops_load_put(const char *name, const char *gate, int autoff,
                        const int *flat, const char (*refs)[REGION_ID_MAX + 1],
                        const char (*exprs)[OPS_EXPR_MAX + 1], int nsteps, int bad)
{
    if (bad || nsteps < 1) {
        ALOGW("ops.conf 跳过一条坏记录（%s%s）", name[0] ? name : "无名记录", bad ? "，字段非法" : "，无步骤行");
        return -1;
    }
    if (op_exists(name)) {
        ALOGI("ops.conf %s 核心表里已有 → 跳过（不覆盖现役定义）", name);
        return 1;
    }
    /* refs / exprs = 每步 ref / 表达式（旧版行读入时缺省空）；out_err=NULL：沿用「见上一行 glue 日志」口径 */
    if (vtouch_op_put(name, gate, autoff, flat, refs, exprs, nsteps, NULL) != 0) {
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
    int  flat[OPS_MAX_STEPS * 8];
    char refs[OPS_MAX_STEPS][REGION_ID_MAX + 1];   /* v3 每步 ref（旧版行读入时缺省空） */
    char exprs[OPS_MAX_STEPS][OPS_EXPR_MAX + 1];   /* v4 每步表达式（v1/v2/v3 行缺省空；`-` = 空） */
    FILE *f = fopen(OPS_CONF_FILE, "r");
    if (!f) return;          /* 没有文件 = 没有历史操作（首次运行），什么都不做、也不写盘 */
    /* 版本门：v1 / v2 / v3（兼容读入，spec §7）与 v4（本格式）都认；无版本行 / 其他版本 = 旧版本残留 →
     * 整份丢弃；照 regions.conf 口径改写当前表 */
    if (!fgets(line, sizeof line, f) || sscanf(line, "#vtouch-ops v%d", &ver) != 1 ||
        (ver != 1 && ver != 2 && ver != 3 && ver != OPS_CONF_VER)) {
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
                rc = ops_load_put(nm, gt, ao, flat, refs, exprs, nst, bad);
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
            int sn, j1 = 0, j2 = 0, pos = 0;
            rf[0] = 0;                     /* ref 缺省空（v1 6 字段行） */
            /* v4 行 10 字段：step <t> <a1> <a2> <a3> <a4> <ms> <ref> <j1> <j2> <expr>；v3 = 9、v2 = 7、v1 = 6 ——
             * 短行 sscanf 提前收工（sn = 实际匹配数），缺的 ref/j1/j2/expr 照上面的缺省留空 / 0。
             * %n 记 j2 之后的位置：其后还有内容 = v4 的 expr 段（v1/v2/v3 行没有；expr 可含空白 → 取行尾整段） */
            sn = sscanf(line, "step %d %d %d %d %d %d %15s %d %d %n", &t, &a1, &a2, &a3, &a4, &ms, rf, &j1, &j2, &pos);
            if (sn == 6 || sn == 7 || sn == 9) {   /* 6 = v1 行（a3 照读：v1 滑动步 a3=x2，不按缺省丢）；
                                                    * 7 = v2 行；9 = v3 行（含 j1/j2）/ v4 行（expr 另看行尾） */
                if (!nm[0]) {
                    if (!orphan) { ALOGW("ops.conf 有一行 step 不属于任何 op → 忽略"); orphan = 1; }
                    continue;
                }
                orphan = 0;
                if (rf[0] == '-' && rf[1] == 0) rf[0] = 0;   /* ref `-` = 空（写端占位符）→ 还原空串 */
                /* 类型感知翻译（spec §7）：旧行（sn<9）的条件步成立档 → OP_COND_CONT（继续下一步）——
                 * 老文件里 a4=0 只是「没有该字段」；其余类型照读不动（尤其滑动 a3/a4 = 终点坐标） */
                if (sn < 9 && (t == OP_STEP_COND_REGION || t == OP_STEP_COND_TOGGLE)) a4 = OP_COND_CONT;
                if (nst < OPS_MAX_STEPS) {
                    flat[nst * 8 + 0] = t; flat[nst * 8 + 1] = a1; flat[nst * 8 + 2] = a2;
                    flat[nst * 8 + 3] = a3; flat[nst * 8 + 4] = a4; flat[nst * 8 + 5] = ms;
                    flat[nst * 8 + 6] = j1; flat[nst * 8 + 7] = j2;
                    snprintf(refs[nst], sizeof refs[nst], "%s", rf);
                    exprs[nst][0] = 0;     /* v1/v2/v3 行缺 expr → 空（v4 行下面按行尾覆盖） */
                    if (sn == 9) {         /* v4：j2 之后的行尾整段 = expr（去首尾空白；`-` = 空） */
                        char *e = line + pos;
                        int en;
                        while (*e == ' ' || *e == '\t') e++;
                        en = (int)strlen(e);
                        while (en > 0 && (e[en - 1] == '\n' || e[en - 1] == '\r' ||
                                          e[en - 1] == ' ' || e[en - 1] == '\t')) en--;
                        if (en > 0 && !(en == 1 && e[0] == '-')) {
                            if (en > OPS_EXPR_MAX) bad = 1;    /* 超上限：整条按坏记录处理（核心只收 ≤63） */
                            else { memcpy(exprs[nst], e, (size_t)en); exprs[nst][en] = 0; }
                        }
                    }
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
        rc = ops_load_put(nm, gt, ao, flat, refs, exprs, nst, bad);
        if (rc == 0) nok++; else if (rc == 1) nskip++; else nbad++;
    }
    fclose(f);
    if (nrec > 0 && (nskip || nbad))
        ALOGI("ops.conf 共 %d 条：补入 %d 条、跳过 %d 条（核心表里已有 %d、坏记录 %d）",
              nrec, nok, nskip + nbad, nskip, nbad);
}

/* ---- 方案文件层（v4 T1.1：schemes/<名>/ + current / 迁移 / 镜像）------------------------------------
 * 布局（docs/OPS_PLAN_V4.md §1）：REGION_CONF_DIR 下 regions.conf / ops.conf（live 工作副本，格式零改动）
 * + current（一行方案名）+ schemes/<名>/{regions.conf,ops.conf}。不变量 live == schemes/<current>：
 * 启动兜底迁移 + 强制同步（nativeInit）、save_regions / save_ops 落盘后镜像，共同保证。核心不感知方案。
 * 迁移（spec §2）：live 任一存在 → 建「默认」收编（缺失份写空合法文件）；全新 → 建空「默认」；
 * 冲突 → 自增「默认2 / 默认3…」；current 写定；日志 `方案 兜底迁移 → <名>（区域 N / 操作 M）`。
 * 名字：用户输入走 scheme_name_ok（同 vt_id_ok 尺子）；内部兜底名「默认[N]」非 ASCII，不走字符集门
 * （读取/枚举用 scheme_name_known 承认这两类）。 */
#define SCHEME_DIR REGION_CONF_DIR "/schemes"
#define SCHEME_CUR REGION_CONF_DIR "/current"
#define SCHEME_LIST_MAX 64      /* scheme_list 一次最多枚举的方案数（T2.1 列表页缓冲按它定宽） */

/* 方案名合法性（用户输入口；与区域 id / 操作名同一把尺子 vt_id_ok）：[A-Za-z0-9_-]、1..15、裸 `-` 除外。
 * 0 ok / 1 空 / 2 超长 / 3 非法字符（含裸 `-`；`.` 等一律非法，天然排除 `.`/`..` 目录名）。
 * 无「重名」位 —— 撞目录由 scheme_exists / scheme_list 单独判（T2.1 新建/改名用）。 */
static int scheme_name_ok(const char *n)
{
    int i, len = (int)strlen(n);
    if (len < 1) return 1;
    if (len > 15) return 2;
    if (len == 1 && n[0] == '-') return 3;    /* 裸 `-` = 解除/无门控哨兵（vt_id_ok 同款拒收） */
    for (i = 0; i < len; i++) {
        char ch = n[i];
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') || ch == '_' || ch == '-')) return 3;
    }
    return 0;
}
/* current 读取与目录枚举的「承认」规则：长度/路径安全先过，再认合法用户名或内部兜底名「默认[N]」。 */
static int scheme_name_known(const char *n)
{
    int len = (int)strlen(n);
    if (len < 1 || len > 15 || strchr(n, '/')) return 0;
    if (!strcmp(n, ".") || !strcmp(n, "..")) return 0;    /* 目录名恰好是这两者也不认（防越界） */
    return scheme_name_ok(n) == 0 || strncmp(n, "默认", 6) == 0;
}
/* 方案目录是否存在（stat 判目录；迁移冲突自增 / T2.1 撞名检查用）。 */
static int scheme_exists(const char *name)
{
    char path[160];
    struct stat st;
    snprintf(path, sizeof path, "%s/%s", SCHEME_DIR, name);
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}
/* qsort 比较器：方案名按字节序（strcmp）排（names 每格 16 字节）。 */
static int scheme_name_cmp(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}
/* 枚举 schemes/ 下的方案（readdir；只收承认名 scheme_name_known 的目录），字母序装进 names（每格 16 字节），
 * 返回条数（≤ max）。目录不存在/读不了 = 0 条（面板退化为空列表，不报错）。 */
static int scheme_list(char (*names)[16], int max)
{
    DIR *d = opendir(SCHEME_DIR);
    struct dirent *e;
    int n = 0;
    if (!d) return 0;
    while (n < max && (e = readdir(d)) != NULL) {
        if (!scheme_name_known(e->d_name)) continue;    /* 滤 `.`/`..`、超长、非法名、外来目录 */
        if (!scheme_exists(e->d_name)) continue;        /* 只收目录（dirent 的类型字段不可靠，用 stat） */
        snprintf(names[n], 16, "%s", e->d_name);
        n++;
    }
    closedir(d);
    qsort(names, (size_t)n, 16, scheme_name_cmp);
    return n;
}
/* 读 current（一行名；容忍首尾空白 / 尾随换行）：0 且 out = 名（16 字节）；1 = 缺失/空/不可用（调用方走迁移）。
 * 承认规则同 scheme_name_known（内部兜底名「默认」非 ASCII，不套字符集门）。 */
static int scheme_cur_get(char *out)
{
    char line[64], *p, *q;
    FILE *f = fopen(SCHEME_CUR, "rb");
    out[0] = 0;
    if (!f) return 1;
    if (!fgets(line, sizeof line, f)) { fclose(f); return 1; }
    fclose(f);
    for (p = line; *p && *p != '\r' && *p != '\n'; p++) { }    /* 只认第一行 */
    *p = 0;
    p = line;
    while (*p == ' ' || *p == '\t') p++;                       /* 去首空白 */
    q = p + strlen(p);
    while (q > p && (q[-1] == ' ' || q[-1] == '\t')) *--q = 0; /* 去尾空白 */
    if (!scheme_name_known(p)) return 1;
    snprintf(out, 16, "%s", p);
    return 0;
}
/* 写 current（.tmp + rename，同 save_regions 口径：掉电不留半截）；名照写、无换行。0 / -1。
 * 只接受「承认」的名字（空/超长/带 `/` 直接拒 —— 防御性；正常调用方已先过 scheme_name_ok）。 */
static int scheme_cur_set(const char *name)
{
    char tmppath[128];
    FILE *f;
    if (!scheme_name_known(name)) return -1;
    region_conf_dir();
    snprintf(tmppath, sizeof tmppath, "%s.tmp", SCHEME_CUR);
    f = fopen(tmppath, "wb");
    if (!f) { ALOGE("current 写入失败 %s: %s", tmppath, strerror(errno)); return -1; }
    fputs(name, f);
    if (fclose(f) != 0 || rename(tmppath, SCHEME_CUR) != 0) {
        ALOGE("current 落盘失败 %s: %s", SCHEME_CUR, strerror(errno));
        remove(tmppath);
        return -1;
    }
    return 0;
}
/* 写「空表合法文件」= 本表当前版本行一行（spec §2；.tmp + rename 同口径）。0 / -1。 */
static int scheme_write_empty(const char *path, int kind)
{
    char tmppath[160];
    FILE *f;
    snprintf(tmppath, sizeof tmppath, "%s.tmp", path);
    f = fopen(tmppath, "wb");
    if (!f) return -1;
    if (kind == 0) fprintf(f, "#vtouch-regions v%d\n", REGION_CONF_VER);
    else           fprintf(f, "#vtouch-ops v%d\n", OPS_CONF_VER);
    if (fclose(f) != 0 || rename(tmppath, path) != 0) { remove(tmppath); return -1; }
    return 0;
}
/* 方案侧文件缺失 → 补空合法文件（「同步」里先补齐再复制；已存在则不动）。0 / -1。 */
static int scheme_ensure_empty(const char *path, int kind)
{
    FILE *f = fopen(path, "rb");
    if (f) { fclose(f); return 0; }
    return scheme_write_empty(path, kind);
}
/* 逐字节复制 src → dst（.tmp + rename；源缺失 → dst 处写空合法文件 —— 迁移「缺失那份」与镜像兜底同一口径）。
 * 0 / -1（失败保留 errno，调用方打原因）。 */
static int scheme_copy_file(const char *src, const char *dst, int kind)
{
    char tmppath[160];
    unsigned char buf[4096];
    size_t n;
    int err = 0;
    FILE *in = fopen(src, "rb");
    FILE *out;
    if (!in) {
        if (errno != ENOENT) return -1;
        return scheme_write_empty(dst, kind);     /* 源缺失 → 空表补齐 */
    }
    snprintf(tmppath, sizeof tmppath, "%s.tmp", dst);
    out = fopen(tmppath, "wb");
    if (!out) { err = errno; fclose(in); errno = err; return -1; }
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) { err = EIO; break; }
    }
    if (!err && ferror(in)) err = EIO;
    fclose(in);
    if (fclose(out) != 0 && !err) err = errno;
    if (!err && rename(tmppath, dst) != 0) err = errno;
    if (err) { remove(tmppath); errno = err; return -1; }
    return 0;
}
/* 计数文件里按 `prefix` 开头的行数（迁移日志「区域 N / 操作 M」口径：从文件解析，空/缺 = 0）。 */
static int scheme_count_lines(const char *path, const char *prefix)
{
    char line[256];
    int n = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    while (fgets(line, sizeof line, f))
        if (strncmp(line, prefix, strlen(prefix)) == 0) n++;
    fclose(f);
    return n;
}
/* 启动接管（spec §2；nativeInit 的 load_regions/load_ops 之前调）：current 指向的方案目录/两文件不全
 * → 兜底迁移（建新方案收编 live / 建空「默认」，冲突自增），current 写定。out = 当前方案名（16 字节；
 * 空 = 迁移失败 —— 不阻塞启动，调用方照旧按 live 文件继续）。日志逐字：
 * `方案 兜底迁移 → <名>（区域 N / 操作 M）`（N/M 从落定后的方案文件解析计数）。 */
static void scheme_migrate(char *out)
{
    char cur[16], name[16], path[160];
    char names[SCHEME_LIST_MAX][16];
    int nn, k, i, used;
    out[0] = 0;
    region_conf_dir();
    if (mkdir(SCHEME_DIR, 0775) < 0 && errno != EEXIST) {
        ALOGE("方案目录创建失败 %s: %s", SCHEME_DIR, strerror(errno));
        return;
    }
    if (scheme_cur_get(cur) == 0) {
        struct stat st;
        int have_r, have_o;
        snprintf(path, sizeof path, "%s/%s/regions.conf", SCHEME_DIR, cur);
        have_r = stat(path, &st) == 0;
        snprintf(path, sizeof path, "%s/%s/ops.conf", SCHEME_DIR, cur);
        have_o = stat(path, &st) == 0;
        if (have_r && have_o) { snprintf(out, 16, "%s", cur); return; }    /* 已是有效方案 */
        ALOGW("方案 current=%s 的方案目录/文件缺失 → 兜底迁移", cur);
    }
    /* 冲突自增：默认 → 默认2 → 默认3…（先枚举承认名；枚举满可能被截断，逐个 stat 兜底） */
    nn = scheme_list(names, SCHEME_LIST_MAX);
    for (k = 1, used = 1; k <= 100000 && used; k++) {
        if (k == 1) snprintf(name, sizeof name, "默认");
        else        snprintf(name, sizeof name, "默认%d", k);
        used = 0;
        for (i = 0; i < nn; i++) if (!strcmp(names[i], name)) { used = 1; break; }
        if (!used && nn == SCHEME_LIST_MAX && scheme_exists(name)) used = 1;
    }
    if (used) { ALOGE("方案兜底迁移：默认名冲突自增超限"); return; }
    snprintf(path, sizeof path, "%s/%s", SCHEME_DIR, name);
    if (mkdir(path, 0775) < 0) { ALOGE("方案目录创建失败 %s: %s", path, strerror(errno)); return; }
    /* 收编 live 两文件（缺失份自动写空合法文件；全新则两份都是空表 —— spec §2 同一实现） */
    snprintf(path, sizeof path, "%s/%s/regions.conf", SCHEME_DIR, name);
    if (scheme_copy_file(REGION_CONF_NEW, path, 0) != 0) {
        ALOGE("方案兜底迁移：收编 regions.conf 失败 %s: %s", path, strerror(errno));
        return;
    }
    snprintf(path, sizeof path, "%s/%s/ops.conf", SCHEME_DIR, name);
    if (scheme_copy_file(OPS_CONF_FILE, path, 1) != 0) {
        ALOGE("方案兜底迁移：收编 ops.conf 失败 %s: %s", path, strerror(errno));
        return;
    }
    if (scheme_cur_set(name) != 0) return;     /* current 写定（失败已在里面报） */
    {
        int nr, no;
        snprintf(path, sizeof path, "%s/%s/regions.conf", SCHEME_DIR, name);
        nr = scheme_count_lines(path, "region ");
        snprintf(path, sizeof path, "%s/%s/ops.conf", SCHEME_DIR, name);
        no = scheme_count_lines(path, "op ");
        ALOGI("方案 兜底迁移 → %s（区域 %d / 操作 %d）", name, nr, no);
    }
    snprintf(out, 16, "%s", name);
}
/* 启动强制同步（spec §2-3）：schemes/<name>/ 两文件 → live 两路径。方案侧缺失的份先补空合法文件
 * （「缺失文件按写空合法文件补齐后再复制」），再逐字节复制。0 = 两文件都同步完成；-1 = 有失败
 * （调用方告警并按现有 live 文件继续 —— 不阻塞启动、不影响注入）。 */
static int scheme_sync_live(const char *name)
{
    char sp[160];
    int rc = 0;
    snprintf(sp, sizeof sp, "%s/%s/regions.conf", SCHEME_DIR, name);
    if (scheme_ensure_empty(sp, 0) != 0 || scheme_copy_file(sp, REGION_CONF_NEW, 0) != 0) rc = -1;
    snprintf(sp, sizeof sp, "%s/%s/ops.conf", SCHEME_DIR, name);
    if (scheme_ensure_empty(sp, 1) != 0 || scheme_copy_file(sp, OPS_CONF_FILE, 1) != 0) rc = -1;
    return rc;
}
/* 存盘镜像（spec §3）：live 两文件 → schemes/<current>/ 同名复制。save_regions / save_ops 落盘成功后
 * 各调一次（调用点 = 两个 save 的成功出口）。失败打 `方案 镜像失败 <名>: <原因>`（ALOGE）不阻塞、
 * 不回滚 —— 下次存盘自然补齐。只写 schemes/、不碰 live ⇒ 不会递归触发保存。
 * 无 current（迁移失败/尚未接管）→ 直接返回（迁移侧已报错；下次启动兜底再试）。 */
static void scheme_mirror(void)
{
    char cur[16], sp[160];
    if (scheme_cur_get(cur) != 0) return;
    snprintf(sp, sizeof sp, "%s/%s/regions.conf", SCHEME_DIR, cur);
    if (scheme_copy_file(REGION_CONF_NEW, sp, 0) != 0) ALOGE("方案 镜像失败 %s: %s", cur, strerror(errno));
    snprintf(sp, sizeof sp, "%s/%s/ops.conf", SCHEME_DIR, cur);
    if (scheme_copy_file(OPS_CONF_FILE, sp, 1) != 0) ALOGE("方案 镜像失败 %s: %s", cur, strerror(errno));
}

/* ---- 方案切换执行器（v4 T1.2：预检 / 静默边界 / flush+写 live / clear+重放 / current+日志+刷新）------------
 * 事务顺序照 spec §4（编号同）：①预检（dry-run，零状态修改）②静默边界 ③flush 旧 ④写 live ⑤核心替换
 * （先区域后操作）⑥写 current ⑦日志 + UI 刷新。失败面：预检拒 → 原状；flush 失败仅告警；写 live 失败
 * → 核心未触（9 时 regions 已回滚，保持 live==schemes/<current>）；核心单条拒收 = 既有「坏记录单条
 * 跳过 + 警告」口径（不算切换失败）；current 写失败 → 能回滚就整体回滚（live+核心 ← <旧>）并返回 10，
 * 无 current 可回滚时保持旧行为（下次启动兜底迁移）。
 * scheme_switch 返回码（0 = 成功；非零 = 失败原因码，供调用方就地提示）：
 *   1 = 名字非法（空 / 超长 / `/` / `.` / `..` / 裸 `-`；门 = scheme_name_known 承认域 ——
 *       与方案列表 / current 同一把尺子，内部兜底名「默认[N]」放行）
 *   2 = 方案目录或文件缺失（schemes/<名>/ 目录或两文件之一读不到）
 *   3 = regions.conf 版本门不过（缺版本行 / 非当前版本）
 *   4 = regions.conf 坏行
 *   5 = ops.conf 版本门不过（缺版本行 / 非 v1/v2/v3/v4）
 *   6 = ops.conf 坏行（含：记录无步骤行 / 孤儿 step 行）
 *   7 = ops.conf 单条步数越限（>32）
 *   8 = 写 live regions.conf 失败（核心未触、原状）
 *   9 = 写 live ops.conf 失败（regions 已回滚、原状；核心未触）
 *  10 = 写 current 失败（已整体回滚：live 与核心回到 <旧>、原状） */

/* 行首识别词（预检用；词后跟空格/制表符才算该类别 —— 与 load 各分支的 sscanf 字面量同集合）。 */
static int scheme_line_kw(const char *line, const char *kw)
{
    size_t n = strlen(kw);
    return strncmp(line, kw, n) == 0 && (line[n] == ' ' || line[n] == '\t');
}
/* 预检 regions.conf 单行（只读）：坏行 → 1，其余 → 0。判定 = 识别词 + 该类别按 load 同款 sscanf 解析
 * 失败；#frame 另查值域（rot 0..3 / w,h > 0）。未识别行 / 空行照 load「未来扩展静默忽略」口径放行。
 * 比 load 更严的只有三处：region / #frame / hide 解析失败在 load 是**静默忽略**（数据行悄悄丢），
 * 预检按坏行拒切；其余与 load 的警告 / 坏记录口径一一对应。 */
static int scheme_bad_region_line(const char *line)
{
    char id[16], op[16], tms[16];
    int t, a1, a2, a3, a4, en, fr, fw, fh, kd;
    if (scheme_line_kw(line, "#frame")) {
        if (sscanf(line, "#frame %d %d %d", &fr, &fw, &fh) != 3) return 1;
        return !(fr >= 0 && fr <= 3 && fw > 0 && fh > 0);
    }
    if (scheme_line_kw(line, "region"))
        return sscanf(line, "region %15s %d %d %d %d %d %d", id, &t, &a1, &a2, &a3, &a4, &en) != 7;
    if (scheme_line_kw(line, "hide"))
        return sscanf(line, "hide %15s", id) != 1;
    if (scheme_line_kw(line, "bind")) {
        if (sscanf(line, "bind %15s %15s %15s", id, op, tms) != 3) return 1;
        return strcmp(tms, "down") != 0 && strcmp(tms, "press") != 0;
    }
    if (scheme_line_kw(line, "kind")) {
        if (sscanf(line, "kind %15s %d", id, &kd) != 2) return 1;
        return kd != 0 && kd != 1;
    }
    return 0;      /* 未识别 / 空行：放行（未来扩展增量行照 load 静默忽略口径） */
}
/* 预检（dry-run，spec §4-1）：只读解析 schemes/<name>/ 两文件 —— 版本门 / 坏行 / 步数上限，照 load
 * 口径但**不推核心**、零状态修改。返回 0 = 可切换；非零 = 上表 2..7 的失败码。 */
static int scheme_precheck(const char *name)
{
    char sp[160], line[256];
    FILE *f;
    int ver;
    snprintf(sp, sizeof sp, "%s/%s/regions.conf", SCHEME_DIR, name);
    f = fopen(sp, "r");
    if (!f) return 2;
    if (!fgets(line, sizeof line, f) || sscanf(line, "#vtouch-regions v%d", &ver) != 1 || ver != REGION_CONF_VER) {
        fclose(f);
        return 3;
    }
    while (fgets(line, sizeof line, f)) {
        if (scheme_bad_region_line(line)) { fclose(f); return 4; }
    }
    fclose(f);
    snprintf(sp, sizeof sp, "%s/%s/ops.conf", SCHEME_DIR, name);
    f = fopen(sp, "r");
    if (!f) return 2;
    if (!fgets(line, sizeof line, f) || sscanf(line, "#vtouch-ops v%d", &ver) != 1 ||
        (ver != 1 && ver != 2 && ver != 3 && ver != OPS_CONF_VER)) {
        fclose(f);
        return 5;
    }
    {
        /* ops 段逐行扫（只读）：与 load_ops 同结构 —— 记录边界 = op 行；记录内 step 行 6/7/9 字段
         * （v4 第 10 字段 expr 不参与预检：坏 expr 走 load 的「坏记录单条跳过」口径，不算切换失败）。
         * 坏行 → 6；单条 step 数超 OPS_MAX_STEPS → 7；记录无步骤行 → 6。 */
        int have = 0, nst = 0, badline = 0, ovf = 0, rc = 0;
        while (!rc && fgets(line, sizeof line, f)) {
            char a[16], b[16];
            int ao2 = 0, t, a1, a2, a3, a4, ms, j1, j2, sn;
            char rf[REGION_ID_MAX + 1];
            int pn = sscanf(line, "op %15s gate %15s autooff %d", a, b, &ao2);
            if (pn >= 1) {
                if (have) {                        /* 结算上一条（坏行优先于越限报告） */
                    if (badline || nst < 1) rc = 6;
                    else if (ovf) rc = 7;
                    have = 0;
                }
                if (!rc) {
                    if (pn == 3) { have = 1; nst = 0; badline = 0; ovf = 0; }
                    else rc = 6;                   /* op 行字段不全（load 警告口径） */
                }
            } else {
                sn = sscanf(line, "step %d %d %d %d %d %d %15s %d %d", &t, &a1, &a2, &a3, &a4, &ms, rf, &j1, &j2);
                if (sn == 6 || sn == 7 || sn == 9) {
                    if (!have) rc = 6;             /* 孤儿 step 行（load 警告口径） */
                    else if (nst < OPS_MAX_STEPS) nst++;
                    else ovf = 1;                  /* 步数越限（load 整条作废口径） */
                } else if (strncmp(line, "step ", 5) == 0) {
                    rc = 6;                        /* step 行字段残缺（load 整条作废口径） */
                }
                /* 其余行放行（未来扩展 / 空行） */
            }
        }
        fclose(f);
        if (!rc && have) {
            if (badline || nst < 1) rc = 6;
            else if (ovf) rc = 7;
        }
        return rc;
    }
}
/* 关闭操作编辑覆盖层（定义在操作编辑区块 T2.6；切换的静默边界要关掉它 —— 含全部子层与取点态）。 */
static void op_edit_close(void);
/* 视觉（T3.2）：视觉步参数层（定义在文件后段视觉区块；draw_op_edit 的分发要用）；
 * 名字校验（ope_step_check 的区域/模板/点集名校验要用）。 */
static void draw_vis_edit(void);
static int vis_name_ok(const char *n);
/* 回滚复制（评审 I2 / Minor 3）：scheme_copy_file 的「源缺失 → 写空表」语义在回滚方向是危险的
 * （源没了还写空表 = 把 live 清掉）—— 源缺失直接 -1（调用方告警、保持现状）；其余照常复制。0 / -1。 */
static int scheme_restore_file(const char *src, const char *dst, int kind)
{
    FILE *f = fopen(src, "rb");
    if (!f) return -1;
    fclose(f);
    return scheme_copy_file(src, dst, kind);
}
/* 切换执行（spec §4；返回码见段首注释）。名字门 = scheme_name_known 承认域（与 scheme_list /
 * scheme_cur_get 同一把尺子 —— 内部兜底名「默认[N]」可切）。name == current → 直接成功（no-op、
 * 零状态修改）。非 static：跨区块入口 —— T2.1「方案」页接线调用（本任务只落执行器；T1.1 文件层函数全 static）。 */
int scheme_switch(const char *name)
{
    char cur[16], sp[160], dp[160];
    int rc, rst = 0;

    if (!name || !scheme_name_known(name)) return 1;
    if (scheme_cur_get(cur) == 0 && strcmp(cur, name) == 0) return 0;    /* 已是当前 → no-op */
    rc = scheme_precheck(name);                                          /* ① 预检：坏 → 拒切、原状 */
    if (rc != 0) return rc;

    /* ② 静默边界（spec §4-2）：操作在跑 → 停（VT_EDIT_OP_STOP 的胶水入口 = vtouch_op_stop）；
     * 取点态 / 编辑覆盖层（操作编辑层含子层；区域改名弹层）开着 → 关（复用现有 close 路径）。 */
    if (vtouch_op_status(NULL, NULL, &rst) == 0 && rst) vtouch_op_stop();
    if (g_ope_i >= 0) {
        op_edit_close();                 /* 既有 close 路径：内含取点撤单 + 全部子层 / 收起态复位 */
    } else if (g_pick) {                 /* 防御：取点态理论上总伴随编辑层开着 */
        g_pick = 0; g_pick_t0 = 0;
        vtouch_pick_cancel();
    }
    if (g_name_i >= 0) {                 /* 区域改名弹层：无 close 函数，照 draw_name_edit 取消分支复位 */
        g_name_i = -1; g_name_msg[0] = 0;
    }

    /* ③ flush 旧（兜底镜像，spec §4-3；失败仅告警、不中断） */
    if (cur[0]) {
        snprintf(dp, sizeof dp, "%s/%s/regions.conf", SCHEME_DIR, cur);
        if (scheme_copy_file(REGION_CONF_NEW, dp, 0) != 0)
            ALOGW("方案 切换：flush %s/regions.conf 失败: %s", cur, strerror(errno));
        snprintf(dp, sizeof dp, "%s/%s/ops.conf", SCHEME_DIR, cur);
        if (scheme_copy_file(OPS_CONF_FILE, dp, 1) != 0)
            ALOGW("方案 切换：flush %s/ops.conf 失败: %s", cur, strerror(errno));
    }
    /* ④ 写 live（schemes/<new>/ 两文件 → live；复用 1.1 复制 helper）。写失败 → 核心未触：8 时两文件
     * 都还是原状（复制自身 .tmp + rename，失败不留半改）；9 时 regions 已换 → 用 ③ 刚写好的
     * schemes/<cur>/regions.conf 回滚（失败仅告警），保持 live==schemes/<current>，防「重试切换的
     * ③ flush 把新内容写回旧方案目录」（评审 I2）。 */
    snprintf(sp, sizeof sp, "%s/%s/regions.conf", SCHEME_DIR, name);
    if (scheme_copy_file(sp, REGION_CONF_NEW, 0) != 0) return 8;
    snprintf(sp, sizeof sp, "%s/%s/ops.conf", SCHEME_DIR, name);
    if (scheme_copy_file(sp, OPS_CONF_FILE, 1) != 0) {
        if (cur[0]) {
            snprintf(dp, sizeof dp, "%s/%s/regions.conf", SCHEME_DIR, cur);
            if (scheme_restore_file(dp, REGION_CONF_NEW, 0) != 0)
                ALOGW("方案 切换：回滚 live regions 失败: %s", strerror(errno));
        }
        return 9;
    }

    /* ⑤ 核心替换（spec §4-5）：**先区域后操作**；清空后重放 ⇒ load 的「只补缺 / 重复跳过」均不触发
     * （全量补入）；单条被核心拒 = 既有「坏记录单条跳过 + 警告」口径（不算切换失败）。load 可重入
     * （见 load_regions 段首：hide 表按文件重建；load_ops 无跨调用状态）。 */
    vtouch_region_clear();
    load_regions();
    vtouch_op_clear();
    load_ops();

    /* ⑥ 写 current（失败 → 整体回滚 <cur>；评审 Minor 3 复合边）。单回滚 live 不够：下一拍 save 的
     * 写盘源是**核心表**（save_regions 从核心序列化），且切换后表变 ⇒ glue watch_table 会重置 save 位
     * （单纯抑制不可靠）⇒ 连核心一起回滚（live+core ← <旧>），此后任何 save/mirror 只会把 <旧> 内容
     * 写回 <旧> 目录。无 current 无从回滚 → 保持旧行为（mirror 无 current 直接返回、无损坏面）。 */
    if (scheme_cur_set(name) != 0) {
        if (!cur[0]) {
            ALOGW("方案 切换 %s：current 未写定（下次启动兜底迁移）", name);
        } else {
            ALOGW("方案 切换 %s：current 未写定 → 回滚 %s", name, cur);
            snprintf(dp, sizeof dp, "%s/%s/regions.conf", SCHEME_DIR, cur);
            if (scheme_restore_file(dp, REGION_CONF_NEW, 0) != 0)
                ALOGW("方案 切换：回滚 live regions 失败: %s", strerror(errno));
            snprintf(dp, sizeof dp, "%s/%s/ops.conf", SCHEME_DIR, cur);
            if (scheme_restore_file(dp, OPS_CONF_FILE, 1) != 0)
                ALOGW("方案 切换：回滚 live ops 失败: %s", strerror(errno));
            vtouch_region_clear();
            load_regions();
            vtouch_op_clear();
            load_ops();
            return 10;
        }
    }

    /* ⑦ 日志（spec §8 逐字；N/M 从方案文件解析，口径同迁移日志）+ UI 刷新（区域侧走既有「表变了」
     * 刷新口径；区域 / 操作 / 方案页列表逐帧读核心，重画即重读） */
    {
        int nr, no;
        snprintf(sp, sizeof sp, "%s/%s/regions.conf", SCHEME_DIR, name);
        nr = scheme_count_lines(sp, "region ");
        snprintf(sp, sizeof sp, "%s/%s/ops.conf", SCHEME_DIR, name);
        no = scheme_count_lines(sp, "op ");
        ALOGI("方案 切换 %s → %s（区域 %d / 操作 %d）", cur[0] ? cur : "-", name, nr, no);
    }
    ui_region_changed();
    g_need = 1; g_force_frames = 3;
    return 0;
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

/* 编辑层收起条（取点 / 手动共用）几何：底部居中一条（当前屏坐标）。
 * 「那一小块」= 其余整屏可点：取点收起时 = 取点落点；手动收起时 = 看游戏（编辑状态全保留）。 */
#define OPE_BAR_H 92
static void ope_bar_rect(float *x1, float *y1, float *x2, float *y2)
{
    float w = (float)g_scr_w - 48;
    if (w > 960) w = 960;
    *x1 = ((float)g_scr_w - w) * 0.5f; *x2 = *x1 + w;
    *y2 = (float)g_scr_h - 24; *y1 = *y2 - OPE_BAR_H;
}
/* 「面板占屏」状态矩形（当前屏坐标）：吞触摸与喂 ImGui 共用同一个判据（三态，T2.4）——
 *   编辑层展开 → 整屏（编辑时不让误触漏进游戏）；
 *   编辑层收起（取点 / 手动）→ 底部收起条那一小块（其余整屏可点；取点收起时 = 取点落点）；
 *   编辑层关闭 → 面板窗口（现状）。 */
static void ui_rect_now(float *x1, float *y1, float *x2, float *y2)
{
    if (g_vis_cap) {                 /* 采集覆盖层（T3.2）：整屏吞（框选/点选/吸色都在整屏层上做） */
        *x1 = 0; *y1 = 0; *x2 = (float)g_scr_w - 1; *y2 = (float)g_scr_h - 1;
        return;
    }
    if (g_ope_i >= 0) {
        if (g_ope_coll) ope_bar_rect(x1, y1, x2, y2);
        else { *x1 = 0; *y1 = 0; *x2 = (float)g_scr_w - 1; *y2 = (float)g_scr_h - 1; }
        return;
    }
    *x1 = g_pan_x; *y1 = g_pan_y;
    *x2 = g_pan_x + panel_w(); *y2 = g_pan_y + panel_h();
}
static int ui_hit_rect(float x, float y)
{
    float x1, y1, x2, y2;
    ui_rect_now(&x1, &y1, &x2, &y2);
    return x >= x1 && x < x2 && y >= y1 && y < y2;
}

/* 面板区吞触摸（注册给核心）：核心每根手指按下时问一次，参数是竖屏逻辑坐标。
 * 命中面板 → 这根手指整段不进系统（点按钮不会点到下层游戏）；面板自己靠 phys 快照照常响应。
 * UI 关闭/图层不可见时不吃，保持「穿透优先」。判据与推给核心的矩形同源（ui_hit_rect）。 */
static int ui_consume_cb(int lx, int ly)
{
    int sx, sy;
    if (g_ui_off || !g_want_layer) return 0;
    if (now_ms() < g_rot_settle_t) return 0;   /* 转屏稳定窗口内一律不吞（判定会被锁存整段手势） */
    p2c(lx, ly, &sx, &sy);
    return ui_hit_rect((float)sx, (float)sy) ? 1 : 0;
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
        g_scr_target = in_zone(g_zone_kb, fx, fy) ? SCR_KB
                     : in_zone(g_zone_list, fx, fy) ? SCR_LIST
                     : in_zone(g_zone_sheet, fx, fy) ? SCR_SHEET
                     : in_zone(g_zone_side, fx, fy) ? SCR_SIDE : SCR_NONE;
        /* 键盘（名称/数字）里允许拖滚（内容超高时的兜底）；其它覆盖层照旧「只点不滚」 */
        g_scr_on = (g_scr_target == SCR_KB) ||
                   (g_scr_target != SCR_NONE && g_name_i < 0);
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
     * 必须**每帧**推 —— 拖面板/换方向/关 UI 都会改变这块矩形。T2.4 起矩形是**三态**的
     * （编辑层整屏 / 编辑层收起条 / 面板窗口，见 ui_rect_now）：编辑层打开时整屏吞，
     * 收起时只吞底部条（其余整屏 = 取点落点 / 看游戏）。 */
    /* 稳定窗内一律**不吞**（宁放不吞）：转屏瞬间坐标系正在换，而吞触摸判定是"按下问一次、
     * 锁存整段手势"，一次错判会让手指整段被吞或整段漏吞（老面板在进程内的谓词里也是这个规矩，
     * 现在挪到"推给核心的矩形"上）。 */
    int eat_ok = ui_live && (now_ms() >= g_rot_settle_t);
    {
        float rx1, ry1, rx2, ry2;
        ui_rect_now(&rx1, &ry1, &rx2, &ry2);
        vtouch_ui_publish_rect(eat_ok, g_rot, g_scr_w, g_scr_h,
                               (int)rx1, (int)ry1, (int)rx2, (int)ry2);
    }
    if (n > 64) n = 64;
    /* 取点弹回抑制（T2.4 修复 1）：编辑层「收起 → 展开」跃迁的那一帧，凡还按着的手指一律抑制到
     * 抬起 —— 矩形回整屏后，快照锁存（1315，不要求 down_edge）会把这些手指锁成面板鼠标，抬手时
     * 在弹回层里发一次真点击（取点取消 / 回填 / 20s 超时三路通吃）。用**当前帧的按下集合**（d，
     * 见下面每槽的置位）而不是上一帧快照 g_prev_on：手指首见按下与跃迁同帧的窄竞态（poll 线程先
     * 吃到 pick_ev、渲染线程同帧才首见该手指）也盖住。 */
    static int prev_coll = 0;
    int coll_fell = prev_coll && !g_ope_coll;   /* 本帧检测到「收起 → 展开」跃迁 */
    prev_coll = g_ope_coll;
    for (i = 0; i < n; i++) {
        int d = 0, x = 0, y = 0;
        if (vtouch_phys_get(i, &d, &x, &y) != 0) d = 0;
        int sx, sy;                        /* 当前屏坐标：面板命中/鼠标喂给 ImGui 用这个；
                                            * x,y 仍是竖屏逻辑坐标（点/轨迹/框选/拖改都用它） */
        p2c(x, y, &sx, &sy);
        int down_edge = d && !g_prev_on[i];
        int up_edge = !d && g_prev_on[i];
        int in_p = d && ui_live && ui_hit_rect((float)sx, (float)sy);   /* 隐藏时不认面板命中；三态矩形（T2.4） */
        if (d) {
            any = 1;
            if (coll_fell) g_pick_finger[i] = 1;   /* 弹回抑制（T2.4 修复 1）：跃迁帧还按着的手指不许被锁存 */
            if (!g_dots[i].on) g_dots[i].tn = 0;
            g_dots[i].on = 1; g_dots[i].x = x; g_dots[i].y = y;
            int k = g_dots[i].tn % 12;
            g_dots[i].tx[k] = x; g_dots[i].ty[k] = y;
            g_dots[i].tn++;
            /* 取点态（T2.8/T2.4）：收起条上点按 = 取消（「点这里取消」）。这一下不喂 ImGui ——
             * 它就是「取消」这个动作本身（吞掉，免得顺带点到别的）。取消后自动弹回编辑层
             * （参数层保持）；这里 in_p 在取点态 = 底部条（ui_rect_now 三态），不会误吃取点落点。
             * 手指还按着：g_pick_finger 抑制到抬起 —— 弹回后矩形回整屏也不许锁存成点击（T2.4 修复 1）。 */
            int pick_cancel = 0;
            if (down_edge && ui_live && in_p && g_pick) {
                pick_cancel = 1;
                g_pick_finger[i] = 1;                    /* 抑制到抬起（T2.4 修复 1）：这一下是「取消」不是点击 */
                g_pick = 0; g_pick_t0 = 0;
                g_ope_coll = 0;                          /* 自动弹回编辑层 */
                vtouch_pick_cancel();
                g_need = 1; g_force_frames = 2;
                ALOGI("取点 取消（收起条点按）→ 弹回编辑层");
            }
            /* 手势优先于鼠标：框选 / 选中拖改（面板内一律走鼠标，保证按钮可用）。
             * 编辑层开着（含收起态）屏上手势一律不跑：展开时整屏给编辑，收起时整屏给游戏/取点，
             * 都不是框选/拖改的语境（不然收起态里点游戏会顺手改区域表并落盘）。 */
            if (down_edge && ui_live && !in_p && !g_pick && g_ope_i < 0) {   /* 隐藏/取点态不开始框选/拖改（否则会改表并落盘；取点态里外侧按下 = 取点动作本身） */
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
            if (ui_live && !pick_cancel && !g_pick_finger[i] && g_mslot < 0 && in_p && i != g_cap_slot && i != g_edit_slot) {
                panel_press(i, sx, sy);            /* 面板鼠标走当前屏坐标 */
            } else if (ui_live && i == g_mslot) {
                panel_drag_move(sx, sy);
            }
        } else {
            g_pick_finger[i] = 0;                    /* 抑制到抬起：抬起即清位（T2.4 修复 1） */
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
        /* 试查命中标记（Task 7.1）：~2 秒方框 + 坐标文字（x,y = 竖屏逻辑坐标）。
         * 找图 = 模板 w×h 按（模板抓取方向 → 当前方向）的旋转映射（奇步宽高互换，同核心模板旋转口径）；
         * 找色 = 固定 80×80 居中。纯绘制（同取点标记口径）：不参与命中、不吞触摸、不写共享内存。 */
        if (g_vis_testmk_t && t - g_vis_testmk_t < VIS_TEST_MARK_MS) {
            int tmx, tmy, tbx, tby, tbw, tbh;
            ImU32 tc = IM_COL32(0, 200, 0, 255);            /* 绿：与取点橙 / 触点蓝区分 */
            p2c(g_vis_testmk_x, g_vis_testmk_y, &tmx, &tmy);
            if (g_vis_testmk_kind == 0) {
                int steps = (g_vis_testmk_trot - g_rot) & 3;
                tbw = (steps & 1) ? g_vis_testmk_th : g_vis_testmk_tw;
                tbh = (steps & 1) ? g_vis_testmk_tw : g_vis_testmk_th;
                tbx = tmx; tby = tmy;                       /* 命中点 = 匹配框左上（帧锚点；p2c 后与帧坐标同点） */
            } else {
                tbw = 80; tbh = 80;
                tbx = tmx - 40; tby = tmy - 40;             /* 找色：命中像素居中 */
            }
            dl->AddRect(ImVec2((float)tbx, (float)tby),
                        ImVec2((float)(tbx + tbw - 1), (float)(tby + tbh - 1)), tc, 0, 0, 6.0f);
            {
                char tkb[32];
                snprintf(tkb, sizeof tkb, "%d,%d", g_vis_testmk_x, g_vis_testmk_y);
                dl->AddText(ImVec2((float)tbx, (float)(tby - 34)), tc, tkb);
            }
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
        if (nav_btn("方案",     g_nav == 5 && g_sheet, bw)) { g_nav = 5; g_sheet = 1; g_need = 1; }
        if (nav_btn("模板",     g_nav == 6 && g_sheet, bw)) { g_nav = 6; g_sheet = 1; g_vis_test_on = 0; g_vis_test_seq = 0; g_vis_test_msg[0] = 0; g_need = 1; }
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
    const int wait100[8] = { OP_STEP_WAIT, 0, 0, 0, 0, 100, 0, 0 };
    char nm[16];
    gen_op_name(nm);
    if (vtouch_op_put(nm, "", 0, wait100, NULL, NULL, 1, NULL) == 0) {   /* refs/exprs=NULL：等待步无区域引用/表达式（全空口径）；out_err=NULL */
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
 * 坐标 0..竖屏逻辑宽高-1；时长 点按 0..60000 / 滑动 1..60000 / 等待 0..600000；
 * v3 增量：条件步两档位 0..3（档位 = 跳转 → 该侧目标 0..当前步数）；跳转步目标 0..当前步数（0 = 结束）。
 * 面板预检是**硬门**：非法值拒收 + 就地提示（不只提示）；最终仍以核心为准（put 回读失败
 * 就地提示、覆盖层不关）。 */

#define OPE_MAX_STEPS 32                /* = 核心 MAX_STEPS（面板不 include 核心头，独立定义） */

/* g_ope_i / g_ope_coll / g_pick_t0 的定义已上移到取点区块（文件前段的吞触摸/快照判据要读它们）。 */
static char g_ope_orig[16] = {0};       /* 打开时的原名（改过名后 [完成] 照它删旧条目） */
static char g_ope_name[16] = {0};       /* 编辑中的名字（名字子层改，[完成] 才落表） */
static char g_ope_gate[16] = {0};       /* 门控区域 id（空 = 无） */
static int  g_ope_autoff = 0;           /* 跑完自动关 */
static int  g_ope_nsteps = 0;           /* 步骤数（本地副本 1..32） */
static int  g_ope_steps[OPE_MAX_STEPS][8];   /* 本地副本：每步 type,a1,a2,a3,a4,ms,j1,j2（v3 模型 [8]，字段序同核心 vt_step；ref 另存 g_ope_refs） */
static char g_ope_refs[OPE_MAX_STEPS][REGION_ID_MAX + 1];   /* 本地副本：每步区域引用（条件步 ref；空 = 无） */
static char g_ope_exprs[OPE_MAX_STEPS][OPS_EXPR_MAX + 1];   /* 本地副本：每步表达式（计算步 expr；空 = 无；v5） */
static char g_ope_msg[128] = {0};       /* [完成] 拒收/失败的就地提示（红字；放得下 v2 长文案） */
static int  g_ope_kb = 0;               /* 名字子层（字符键盘）开 */
static char g_ope_kbmsg[72] = {0};      /* 名字子层里的拒收提示 */
static int  g_ope_up = 0;               /* 名字子层大小写档 */
static int  g_ope_se = -1;              /* 参数弹层：正在编第几步（-1 = 关） */
static int  g_ope_sf = 0;               /* 参数弹层：激活格（字段序号，0 起） */
static int  g_ne_tgt = 0;               /* 参数弹层模式：0 = 全字段一屏；1 = 成立目标（j1）；2 = 不成立目标（j2） */
static int  g_ope_vl = 0;               /* 变量选择弹层开（参数弹层的 [变量]；1 = 开） */
static int  g_ope_rl = -1;              /* 区域选择弹层：正在选第几步的 ref（-1 = 关） */
static int  g_ope_pv = 0;               /* 预览页（T2.5）：全屏只读层开（编辑层头 [预览] 进、[关闭] 返回） */
/* 表达式子层（v5 计算步）：T3.1 建状态 + 入口；完整 UI（槽 chips / 字符键盘 6×3 / 插入 chips 2×8 /
 * 公式快捷行 / 变量图例 / 矮屏自适应 + 拖滚兜底）在 draw_ope_expr。 */
static int  g_ope_ex = 0;               /* 1 = 开（子层分发在 draw_op_edit 顶部 g_ope_se 之前） */
static char g_ope_expr_buf[OPS_EXPR_MAX + 1];   /* 子层编辑缓冲：进入时从 g_ope_exprs[g_ope_se] 快照；[确定] 校验过写回 */
static int  g_ope_expr_slot = 1;        /* 子层槽选择 r1..r4（写回该步 a1；进入时从 g_ope_steps[g_ope_se][1] 快照） */
static char g_ope_ex_msg[72] = {0};     /* 子层拒收提示（红字；[确定] 不过时层不关） */
static char g_ope_saved_as[16] = {0};   /* 本会话最近一次 put 成功的名字（[完成] 重试豁免自己刚写进表的名字）；开层/关层清空 */
static char g_ope_del_owed[16] = {0};   /* 尚欠删除的旧名（del 超时/未送达留下的账，再点 [完成] 先补删）；开层/关层清空 */
/* 参数弹层 v3 本地缓冲（全字段一屏 / 原子落）：每格 = {值, 文本}（结构说明见 ope_num_load）。
 * 层内编辑只改缓冲；[完成] 全字段校验全过才一次写回 g_ope_steps，[取消] 全丢（含取点/变量改动）。 */
static int  g_ne_vals[8];               /* 每格值：字面值 ≥ 0 / 变量引用 -9..-1（触发变量 -1..-5 / 结果槽 -6..-9；目标模式单格 = j1/j2） */
static char g_ne_text[8][8];            /* 每格字面输入文本（≤6 位数字；变量态 / 目标格「结束」留空） */
static char g_ne_msg[72] = {0};         /* 参数弹层就地提示（范围/位数的硬门反馈 + [完成] 拒收） */
/* 目标模式槽位（ne_open_target 的 slot 参数）：1 = 成立目标（j1）/ 2 = 不成立目标（j2）。 */
#define NE_TGT_J1 1
#define NE_TGT_J2 2

/* 自绘字符键盘弹层（区域改名 / 操作改名共用：面板收不到系统输入法，字符全靠点）。
 * 画在当前窗口内并盖住底下内容（调用方那一帧干脆不画底下，免得底下按钮还能吃点击）：
 * 标题 + 当前值框（带原名对照）+ 6x6 字符键 + 特殊键（_ - 大小写 退格）+ 取消/确定。
 * top = sheet 顶边距（相对窗口顶；面板窗里有标题栏用 TITLE_H+10，编辑层整屏窗用 12）。
 * buf 上限 15 字符、up 是大写档；本函数只画/只改，语义留给调用方：
 * 返回 0 = 无动作 / 1 = 取消 / 2 = 确定。 */
static int draw_char_kb(const char *title, const char *oldname, char *buf, int bufc,
                        int *up, char *msg, int msgc, float top)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 wp = ImGui::GetWindowPos();
    float ww = ImGui::GetWindowWidth(), wh = ImGui::GetWindowHeight();
    ImVec2 a(wp.x + 12, wp.y + top), b(wp.x + ww - 12, wp.y + wh - 12);
    int act = 0;
    /* —— 矮屏/横屏兜底（1）：超高内容手动拖滚的状态（隔帧重开 = 回顶）—— */
    {
        static int lf = -1;
        if (ImGui::GetFrameCount() - lf > 1) g_kb_sc = 0;
        lf = ImGui::GetFrameCount();
        if (g_scroll_acc != 0 && g_scr_target == SCR_KB) { g_kb_sc += g_scroll_acc; g_scroll_acc = 0; }
    }
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
    /* 字符键：6 列 x 6 行 = a-z + 0-9。高度自适应（矮屏/横屏）：键高按可用高算、两档紧凑；
     * 仍放不下 → 键区手动拖滚（g_zone_kb + SCR_KB：内容随 g_kb_sc 整体位移、裁剪在卡片内）。 */
    static const char *krow[6] = { "abcdef", "ghijkl", "mnopqr", "stuvwx", "yz0123", "456789" };
    float gap = 10.0f, kw = (cw - 5 * gap) / 6.0f;
    float sph = 76.0f, bth = 92.0f, kh = 76.0f;
    {
        float avail_kb = b.y - (y0 + 196.0f);                        /* 键区可用高 */
        float fixed_bot = sph + gap + 12.0f + bth + 6.0f * gap;     /* 特殊行+按钮行+缝 */
        float khn = (avail_kb - fixed_bot) / 6.0f;
        if (khn < 56.0f) {
            sph = 62.0f; bth = 70.0f;
            fixed_bot = sph + gap + 12.0f + bth + 6.0f * gap;
            khn = (avail_kb - fixed_bot) / 6.0f;
        }
        kh = khn < 76.0f ? khn : 76.0f;
        if (kh < 40.0f) kh = 40.0f;                                  /* 再矮由手动滚动兜底 */
    }
    {
        float kb_ch = 6.0f * (kh + gap) + sph + gap + 12.0f + bth;   /* 键区内容总高 */
        float kb_max = kb_ch - (b.y - (y0 + 196.0f));
        if (kb_max < 0) kb_max = 0;
        if (g_kb_sc < 0) g_kb_sc = 0;                                /* 下限 clamp：与上限对称（拖滚不为负） */
        if (g_kb_sc > kb_max) g_kb_sc = kb_max;
        pub_zone(g_zone_kb);                                         /* 拖键区滚（超高时） */
        ImGui::PushClipRect(ImVec2(a.x + 4, y0 + 192.0f), ImVec2(b.x - 4, b.y - 4), true);
    }
    float ky = y0 + 196 - g_kb_sc;
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
            if (btn_light(sp[c], ImVec2(sw2, sph))) {
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
    float by = fy + sph + gap + 12, bw2 = (cw - gap) * 0.5f;
    ImGui::PushID(300);
    ImGui::SetCursorScreenPos(ImVec2(x0, by));
    if (btn_light("取消", ImVec2(bw2, bth))) act = 1;
    ImGui::SetCursorScreenPos(ImVec2(x0 + bw2 + gap, by));
    if (btn_blue("确定", ImVec2(bw2, bth))) act = 2;
    ImGui::PopID();
    ImGui::PopClipRect();                        /* 键区裁剪到此（含取消/确定） */
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

/* 字段模型（v5 九类型）：字段序号 → g_ope_steps 下标（0=type、1..4=坐标参数、5=ms）；-1 = 该类型没这个字段。
 * 坐标字段（下标 1..4）在数字弹层里会多画一个 [取点]（T2.8 已接线：核心吞一次触摸回填）；
 * 可变量的字段（spec §2.1）多画一个 [变量]（ope_var_ok 判）。条件步的 a3/a4/j1/j2/ref 不在字段表里 ——
 * 它们是步骤行上的行内控件（档位 + 目标格 + 区域下拉）；跳转步的「目标」= a1，走字段表（1 格）；
 * 计算步无数字字段（表达式走表达式子层，不进数字弹层）。 */
static const int ope_fidx[11][5] = {
    { 1, 2, 5, -1, -1 },       /* 点按：x, y, 按住 ms */
    { 1, 2, 3, 4, 5 },         /* 滑动：起点 x, 起点 y, 终点 x, 终点 y, 时长 ms */
    { 5, -1, -1, -1, -1 },     /* 等待：ms */
    { 1, 2, -1, -1, -1 },      /* 按下：x, y */
    { -1, -1, -1, -1, -1 },    /* 弹起：无字段 */
    { 1, 2, -1, -1, -1 },      /* 区域判断：判定点 x, 判定点 y */
    { -1, -1, -1, -1, -1 },    /* 开关判断：无数字字段 */
    { 1, -1, -1, -1, -1 },     /* 跳转：目标步骤（0 = 结束） */
    { -1, -1, -1, -1, -1 },    /* 计算：无数字字段（表达式子层编辑） */
    { 1, 5, -1, -1, -1 },      /* 找图（T3.2；T7.4 加持续超时）：阈值 = a1、超时 ms = ms（数字键盘子层编辑） */
    { 2, 5, -1, -1, -1 },      /* 找色（T3.2；T7.4 加持续超时）：容差 = a2 低 8 位（ne_field_get/set 拆包）、超时 ms = ms */
};
static const char *const ope_flabel[11][5] = {
    { "坐标 x", "坐标 y", "按住时长 ms", "", "" },
    { "起点 x", "起点 y", "终点 x", "终点 y", "滑动时长 ms" },
    { "等待时长 ms", "", "", "", "" },
    { "坐标 x", "坐标 y", "", "", "" },
    { "", "", "", "", "" },
    { "判定点 x", "判定点 y", "", "", "" },
    { "", "", "", "", "" },
    { "目标", "", "", "", "" },
    { "", "", "", "", "" },    /* 计算：无数字字段 */
    { "阈值", "超时 ms", "", "", "" }, /* 找图：阈值 0..255；超时 0..60000（0 = 单次） */
    { "容差", "超时 ms", "", "", "" }, /* 找色（单点）：容差 0..255；超时 0..60000（0 = 单次） */
};
static int ope_nfields(int type)
{
    return type == OP_STEP_TAP ? 3 : type == OP_STEP_SWIPE ? 5 : type == OP_STEP_WAIT ? 1 :
           type == OP_STEP_DOWN ? 2 : type == OP_STEP_COND_REGION ? 2 : type == OP_STEP_JUMP ? 1 :
           type == OP_STEP_FINDIMAGE ? 2 : type == OP_STEP_FINDCOLOR ? 2 : 0;
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
    case OP_STEP_JUMP:        return "跳转";
    case OP_STEP_CALC:        return "计算";
    case OP_STEP_FINDIMAGE:   return "找图";
    case OP_STEP_FINDCOLOR:   return "找色";
    default:                  return "?";
    }
}

/* 变量 / 结果槽中文名（spec §1.1 逐字 + v5 §6）：下标 0..8 ↔ 值 -1..-9（OP_VAR_TDX..OP_VAR_TMS、OP_VAR_R1..OP_VAR_R4）。 */
static const char *const ope_vname_tab[9] = { "触发按下x", "触发按下y", "触发弹起x", "触发弹起y", "触发时长",
                                              "结果1", "结果2", "结果3", "结果4" };
static const char *ope_vname(int v)      /* 值是变量 / 结果槽引用 → 中文名；不是 → NULL */
{
    if (v >= OP_VAR_R4 && v <= OP_VAR_TDX) return ope_vname_tab[OP_VAR_TDX - v];
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
 * v3：数值字段可为变量引用（-9..-1：触发变量 -1..-5 / 结果槽 -6..-9），放行（跳转目标除外——它是控制流编号，不可变量）；字面值照旧按
 * 类型/坐标轴分档；跳转步目标在字段层先按 0..OPE_MAX_STEPS 收（0 = 结束），保存预检再按当前步数收紧。 */
static int ne_check(const char *label, int type, int fi, int v, char *why, int whycap)
{
    int idx;
    if (type < OP_STEP_TAP || type > OP_STEP_FINDCOLOR || fi < 0 || fi >= ope_nfields(type)) {
        snprintf(why, (size_t)whycap, "步骤类型非法");
        return 0;
    }
    idx = ope_fidx[type - 1][fi];
    if (type == OP_STEP_JUMP) {                       /* 跳转目标：0..32（0 = 结束） */
        if (v < 0 || v > OPE_MAX_STEPS) {
            snprintf(why, (size_t)whycap, "%s 必须在 0..%d（0 = 结束）", label, OPE_MAX_STEPS);
            return 0;
        }
        return 1;
    }
    if (type == OP_STEP_FINDIMAGE || type == OP_STEP_FINDCOLOR) {   /* 视觉步字段（T3.2；T7.4 加持续超时）：
                                                                     * fi 0 = 阈值 / 容差 0..255；fi 1 = 超时 ms 0..60000（0 = 单次） */
        if (fi == 1) {
            if (v < 0 || v > 60000) {
                snprintf(why, (size_t)whycap, "%s 必须在 0..60000（0 = 单次）", label);
                return 0;
            }
            return 1;
        }
        if (type == OP_STEP_FINDCOLOR) v = (int)((uint32_t)v & 0xFFu);   /* 找色：a2 = (颜色<<8)|容差 打包，先拆低 8 位（修复轮） */
        if (v < 0 || v > 255) {
            snprintf(why, (size_t)whycap, "%s 必须在 0..255", label);
            return 0;
        }
        return 1;
    }
    if (ope_vname(v)) return 1;                       /* -9..-1：变量 / 结果槽引用（v2 数值字段全可变量；v5 扩结果槽） */
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
 * 条件步：两档位 a3/a4 ∈ 0..3；档位 = 跳转 → 该侧目标（不成立侧 = j2、成立侧 = j1）∈ 0..当前步数；ref 必须已选。
 * 跳转步：目标 a1 ∈ 0..当前步数（0 = 结束）。其余档位的目标忽略（照核心 op_valid 口径）。
 * 计算步（v5）：a1 ∈ 1..4；expr 非空且过同源 vtouch_expr_check（spec §4）；其余字段必须 0 / ref 空。 */
static int ope_step_check(int si, const int *s6, char *why, int whycap)
{
    int t = s6[0], nf, fi;
    if (t < OP_STEP_TAP || t > OP_STEP_FINDCOLOR) {
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
        if (s6[3] < OP_COND_ABORT || s6[3] > OP_COND_JUMP) {
            snprintf(why, (size_t)whycap, "第 %d 步：不成立行为非法", si + 1);
            return 0;
        }
        if (s6[4] < OP_COND_ABORT || s6[4] > OP_COND_JUMP) {
            snprintf(why, (size_t)whycap, "第 %d 步：成立行为非法", si + 1);
            return 0;
        }
        if (s6[4] == OP_COND_JUMP && (s6[6] < 0 || s6[6] > g_ope_nsteps)) {   /* 成立侧目标 = j1 */
            snprintf(why, (size_t)whycap, "第 %d 步成立侧跳转目标超出步数", si + 1);
            return 0;
        }
        if (s6[3] == OP_COND_JUMP && (s6[7] < 0 || s6[7] > g_ope_nsteps)) {   /* 不成立侧目标 = j2 */
            snprintf(why, (size_t)whycap, "第 %d 步不成立侧跳转目标超出步数", si + 1);
            return 0;
        }
        if (!g_ope_refs[si][0]) {
            snprintf(why, (size_t)whycap, "第 %d 步：请选择区域", si + 1);
            return 0;
        }
    } else if (t == OP_STEP_JUMP) {
        if (s6[1] < 0 || s6[1] > g_ope_nsteps) {         /* 目标 0 = 结束；1..当前步数 = 目标步骤 */
            snprintf(why, (size_t)whycap, "第 %d 步跳转目标超出步数", si + 1);
            return 0;
        }
    } else if (t == OP_STEP_CALC) {
        /* 计算步（v5）：槽 1..4；expr 非空且过同源 vtouch_expr_check（spec §4）；
         * 其余字段必须 0 / ref 空（防御镜像核心 op_valid 的计算步口径）。 */
        char w2[72];
        if (s6[1] < 1 || s6[1] > 4) {
            snprintf(why, (size_t)whycap, "第 %d 步：结果槽必须在 1..4", si + 1);
            return 0;
        }
        if (s6[2] || s6[3] || s6[4] || s6[5] || s6[6] || s6[7] || g_ope_refs[si][0]) {
            snprintf(why, (size_t)whycap, "第 %d 步：计算步其它字段必须为空", si + 1);
            return 0;
        }
        if (!g_ope_exprs[si][0]) {
            snprintf(why, (size_t)whycap, "第 %d 步：表达式为空（点 [参数] 编辑）", si + 1);
            return 0;
        }
        w2[0] = 0;
        if (vtouch_expr_check(g_ope_exprs[si], w2, (int)sizeof w2) != 0) {
            snprintf(why, (size_t)whycap, "第 %d 步：表达式错（%s）", si + 1, w2[0] ? w2 : "非法");
            return 0;
        }
    } else if (t == OP_STEP_FINDIMAGE || t == OP_STEP_FINDCOLOR) {
        /* 视觉步（T3.2 v8；T7.4 扩 ms）：镜像核心 op_valid —— 找图 ref=模板名（必填）、expr=区域名（空或合法）、
         * a1=阈值 0..255、a2=0、ms=0..60000（0 = 单次、>0 = 持续查找超时）；找色 a1=模式、单点 ref 空 +
         * a2=(颜色<<8)|容差、多点 ref=点集名（必填）+ a2=0；两类型 a3/a4 档位、j1/j2 目标域（同条件步）。 */
        if (s6[5] < 0 || s6[5] > 60000) {
            snprintf(why, (size_t)whycap, "第 %d 步：持续超时必须在 0..60000（0 = 单次）", si + 1);
            return 0;
        }
        if (t == OP_STEP_FINDIMAGE) {
            if (s6[1] < 0 || s6[1] > 255) {
                snprintf(why, (size_t)whycap, "第 %d 步：阈值必须在 0..255", si + 1);
                return 0;
            }
            if (s6[2] != 0) {
                snprintf(why, (size_t)whycap, "第 %d 步：找图其它字段必须为空", si + 1);
                return 0;
            }
            if (!g_ope_refs[si][0]) {
                snprintf(why, (size_t)whycap, "第 %d 步：请选择模板（点 [参数]）", si + 1);
                return 0;
            }
            if (vis_name_ok(g_ope_refs[si]) != 0) {
                snprintf(why, (size_t)whycap, "第 %d 步：模板名非法（[A-Za-z0-9_-]、1..15）", si + 1);
                return 0;
            }
        } else {
            if (s6[1] != 0 && s6[1] != 1) {
                snprintf(why, (size_t)whycap, "第 %d 步：模式非法（0/1）", si + 1);
                return 0;
            }
            if (s6[1] == 1) {                 /* 多点：点集名必填 + a2 必须 0（基准色/容差/点表在 .pts） */
                if (!g_ope_refs[si][0]) {
                    snprintf(why, (size_t)whycap, "第 %d 步：请选择点集（多点必填）", si + 1);
                    return 0;
                }
                if (vis_name_ok(g_ope_refs[si]) != 0) {
                    snprintf(why, (size_t)whycap, "第 %d 步：点集名非法（[A-Za-z0-9_-]、1..15）", si + 1);
                    return 0;
                }
                if (s6[2] != 0) {
                    snprintf(why, (size_t)whycap, "第 %d 步：多点模式字段必须为空", si + 1);
                    return 0;
                }
            } else if (g_ope_refs[si][0]) {   /* 单点：点集名必须空 */
                snprintf(why, (size_t)whycap, "第 %d 步：单点模式不能带点集", si + 1);
                return 0;
            }
        }
        if (s6[3] < OP_COND_ABORT || s6[3] > OP_COND_JUMP) {
            snprintf(why, (size_t)whycap, "第 %d 步：不成立行为非法", si + 1);
            return 0;
        }
        if (s6[4] < OP_COND_ABORT || s6[4] > OP_COND_JUMP) {
            snprintf(why, (size_t)whycap, "第 %d 步：成立行为非法", si + 1);
            return 0;
        }
        if (s6[4] == OP_COND_JUMP && (s6[6] < 0 || s6[6] > g_ope_nsteps)) {
            snprintf(why, (size_t)whycap, "第 %d 步成立侧跳转目标超出步数", si + 1);
            return 0;
        }
        if (s6[3] == OP_COND_JUMP && (s6[7] < 0 || s6[7] > g_ope_nsteps)) {
            snprintf(why, (size_t)whycap, "第 %d 步不成立侧跳转目标超出步数", si + 1);
            return 0;
        }
        if (g_ope_exprs[si][0] && vis_name_ok(g_ope_exprs[si]) != 0) {
            snprintf(why, (size_t)whycap, "第 %d 步：区域名非法（[A-Za-z0-9_-]、1..15）", si + 1);
            return 0;
        }
    }
    return 1;
}

/* 档位 + 目标 → 人话（摘要用；目标 0 = 结束）。词表照 spec §8 / 计划 Global Constraints。 */
static void ope_tier_text(int tier, int target, char *out, int outcap)
{
    switch (tier) {
    case OP_COND_CONT: snprintf(out, (size_t)outcap, "继续下一步"); break;
    case OP_COND_SKIP: snprintf(out, (size_t)outcap, "跳过下一步"); break;
    case OP_COND_JUMP: snprintf(out, (size_t)outcap, target == 0 ? "跳到结束" : "跳到第 %d 步", target); break;
    default:           snprintf(out, (size_t)outcap, "中止"); break;   /* OP_COND_ABORT（非法值兜底同款） */
    }
}

/* 条件档位的循环序（spec §1.1 固定序：继续下一步 → 跳过下一步 → 跳到… → 中止 → 回继续）。 */
static int ope_tier_next(int tier)
{
    switch (tier) {
    case OP_COND_CONT: return OP_COND_SKIP;
    case OP_COND_SKIP: return OP_COND_JUMP;
    case OP_COND_JUMP: return OP_COND_ABORT;
    default:           return OP_COND_CONT;   /* 中止 / 非法值兜底：回「继续下一步」 */
    }
}
/* 循环钮文字（档位名；「跳到…」只是档位名，具体目标在旁边的目标格）。 */
static const char *ope_tier_name(int tier)
{
    switch (tier) {
    case OP_COND_CONT: return "继续下一步";
    case OP_COND_SKIP: return "跳过下一步";
    case OP_COND_JUMP: return "跳到…";
    default:           return "中止";         /* OP_COND_ABORT（非法值兜底同款） */
    }
}

/* 步骤行参数文本（T3.1 摘要；v3 起两档全显）：坐标/时长格显示变量中文名或原值；条件步 = 区域名 +
 * 成立/不成立两档（跳转档带目标）；跳转步 = 目标（「跳到 第 N 步」/「跳到 结束」）；
 * 计算步（v5）= `计算 → r1 = <表达式>`（spec §6）。 */
static void ope_step_text(int si, char *out, int outcap)
{
    const int *s6 = g_ope_steps[si];
    const char *ref = g_ope_refs[si];
    char x1[24], y1[24], x2[24], y2[24], ms[24], t1[24], t2[24];
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
        ope_tier_text(s6[4], s6[6], t1, (int)sizeof t1);       /* 成立侧：档 a4、目标 j1 */
        ope_tier_text(s6[3], s6[7], t2, (int)sizeof t2);       /* 不成立侧：档 a3、目标 j2 */
        snprintf(out, (size_t)outcap, "%s,%s · %s · 成立 → %s / 不成立 → %s", x1, y1,
                 ref[0] ? ref : "未选区域", t1, t2);
        break;
    case OP_STEP_COND_TOGGLE:
        ope_tier_text(s6[4], s6[6], t1, (int)sizeof t1);
        ope_tier_text(s6[3], s6[7], t2, (int)sizeof t2);
        snprintf(out, (size_t)outcap, "%s · 成立 → %s / 不成立 → %s",
                 ref[0] ? ref : "未选区域", t1, t2);
        break;
    case OP_STEP_JUMP:
        if (s6[1] == 0) snprintf(out, (size_t)outcap, "跳到 结束");
        else snprintf(out, (size_t)outcap, "跳到 第 %d 步", s6[1]);
        break;
    case OP_STEP_CALC:
        /* 摘要 = `计算 → r1 = <表达式>`（spec §6；空表达式防御显示「(空)」，完整表达式进子层看） */
        snprintf(out, (size_t)outcap, "计算 → r%d = %s", s6[1],
                 g_ope_exprs[si][0] ? g_ope_exprs[si] : "(空)");
        break;
    case OP_STEP_FINDIMAGE:
        /* 摘要（T3.2）：模板 / 区域 / 阈值 + 成立/不成立两档（跳转档带目标；同条件步口径） */
        ope_tier_text(s6[4], s6[6], t1, (int)sizeof t1);
        ope_tier_text(s6[3], s6[7], t2, (int)sizeof t2);
        snprintf(out, (size_t)outcap, "模板 %s · %s · 阈值 %d · 成立 → %s / 不成立 → %s",
                 ref[0] ? ref : "未选", g_ope_exprs[si][0] ? g_ope_exprs[si] : "全屏", s6[1], t1, t2);
        break;
    case OP_STEP_FINDCOLOR:
        ope_tier_text(s6[4], s6[6], t1, (int)sizeof t1);
        ope_tier_text(s6[3], s6[7], t2, (int)sizeof t2);
        if (s6[1] == 1)
            snprintf(out, (size_t)outcap, "多点 · 点集 %s · %s · 成立 → %s / 不成立 → %s",
                     ref[0] ? ref : "未选", g_ope_exprs[si][0] ? g_ope_exprs[si] : "全屏", t1, t2);
        else
            snprintf(out, (size_t)outcap, "单点 #%06X 容差 %d · %s · 成立 → %s / 不成立 → %s",
                     (unsigned)(((uint32_t)s6[2] >> 8) & 0xFFFFFFu), (int)((uint32_t)s6[2] & 0xFFu),
                     g_ope_exprs[si][0] ? g_ope_exprs[si] : "全屏", t1, t2);
        break;
    default:
        snprintf(out, (size_t)outcap, "类型非法（%d）", s6[0]);
        break;
    }
}

/* 参数弹层缓冲小工具（v3 全字段一屏 / 目标模式共用） */
static int ne_parse(const char *s)       /* 字面文本 → 值（空串 = 0，承 v2 口径；文本只含数字） */
{
    int v = 0;
    for (; *s; s++) v = v * 10 + (*s - '0');
    return v;
}
/* 该格是不是「目标格」：跳转步的「目标」（fi=0）/ 目标模式单格（j1、j2）—— 0..32、0 显示「结束」。 */
static int ne_is_target(int type, int fi)
{
    if (g_ne_tgt) return 1;
    return type == OP_STEP_JUMP && fi == 0;
}
/* 格的标签：目标模式 = 成立目标 / 不成立目标；其余照字段表。 */
static const char *ne_label(int type, int fi)
{
    if (g_ne_tgt) return g_ne_tgt == NE_TGT_J1 ? "成立目标" : "不成立目标";
    return ope_flabel[type - 1][fi];
}
/* 格的显示文本：变量 → 中文名；目标格值 0 → 「结束」；字面 → 输入文本（空串 → 「(空)」）。 */
static void ne_cell_text(int type, int fi, char *out, int outcap)
{
    int v = g_ne_vals[fi];
    const char *vn = ope_vname(v);
    if (vn) { snprintf(out, (size_t)outcap, "%s", vn); return; }
    if (ne_is_target(type, fi) && v == 0) { snprintf(out, (size_t)outcap, "结束"); return; }
    if (g_ne_text[fi][0]) { snprintf(out, (size_t)outcap, "%s", g_ne_text[fi]); return; }
    snprintf(out, (size_t)outcap, "(空)");
}
/* 目标格范围硬门（[完成] 用）：0..32（0 = 结束）；保存预检再按当前步数收紧（op_edit_save）。 */
static int ne_check_target(const char *label, int v, char *why, int whycap)
{
    if (v < 0 || v > OPE_MAX_STEPS) {
        snprintf(why, (size_t)whycap, "%s 必须在 0..%d（0 = 结束）", label, OPE_MAX_STEPS);
        return 0;
    }
    return 1;
}

/* 视觉步（T3.2）字段语义值读写：默认 = 字段直存 g_ope_steps[idx]；找色的容差字段特殊 ——
 * a2 是打包值 (颜色<<8)|容差，字段值 = 低 8 位（写回只改低 8 位，颜色段保留）。 */
static int ne_field_get(int se, int type, int fi)
{
    int idx = ope_fidx[type - 1][fi];
    if (type == OP_STEP_FINDCOLOR && idx == 2) return (int)((uint32_t)g_ope_steps[se][2] & 0xFFu);
    return g_ope_steps[se][idx];
}
static void ne_field_set(int se, int type, int fi, int v)
{
    int idx = ope_fidx[type - 1][fi];
    if (type == OP_STEP_FINDCOLOR && idx == 2) {
        g_ope_steps[se][2] = (int)(((uint32_t)g_ope_steps[se][2] & ~0xFFu) | ((uint32_t)v & 0xFFu));
        return;
    }
    g_ope_steps[se][idx] = v;
}

/* 参数弹层 v3：进层快照 —— 把该步全部数值字段（目标模式 = j1/j2 单格）装进本地缓冲。
 * 每格 = {值, 文本}：g_ne_vals[fi] 存语义值（字面值 ≥ 0 / 变量引用 -9..-1）；g_ne_text[fi] 存字面
 * 输入文本（变量态留空 = 显示中文名；目标格值 0 留空 = 显示「结束」）。层内编辑只改缓冲；
 * [完成] 全字段校验全过才一次写回 g_ope_steps，[取消] 全丢。类型/字段非法 → 直接关层。 */
static void ope_num_load(void)
{
    int type, nf, fi, v;
    memset(g_ne_vals, 0, sizeof g_ne_vals);
    memset(g_ne_text, 0, sizeof g_ne_text);
    if (g_ope_se < 0 || g_ope_se >= g_ope_nsteps) { g_ope_se = -1; g_ne_tgt = 0; return; }
    type = g_ope_steps[g_ope_se][0];
    if (g_ne_tgt) {                                  /* 目标模式：单格 j1/j2；类型非法就关层 */
        if (type != OP_STEP_COND_REGION && type != OP_STEP_COND_TOGGLE &&
            type != OP_STEP_FINDIMAGE && type != OP_STEP_FINDCOLOR) { g_ope_se = -1; g_ne_tgt = 0; return; }
        v = g_ope_steps[g_ope_se][5 + g_ne_tgt];
        g_ne_vals[0] = v;
        if (v > 0) snprintf(g_ne_text[0], sizeof g_ne_text[0], "%d", v);
        g_ope_sf = 0;
        return;
    }
    nf = ope_nfields(type);
    if (nf == 0 || g_ope_sf < 0 || g_ope_sf >= nf) { g_ope_se = -1; return; }
    for (fi = 0; fi < nf; fi++) {
        v = ne_field_get(g_ope_se, type, fi);        /* 视觉步容差走拆包读（ne_field_get） */
        g_ne_vals[fi] = v;
        if (v < 0) continue;                         /* 变量引用：文本留空（显示中文名） */
        if (ne_is_target(type, fi) && v == 0) continue;   /* 目标格 0 = 结束：文本留空（显示「结束」） */
        snprintf(g_ne_text[fi], sizeof g_ne_text[fi], "%d", v);
    }
}

/* 条件目标格键盘模式入口（T2.3 行控件调用；调用方式 = ne_open_target(第几步, NE_TGT_J1|NE_TGT_J2)）。
 * 打开参数弹层目标模式：单格「成立目标 / 不成立目标」，键盘编 0..32（0 显示「结束」），无变量/取点；
 * 层内只改缓冲，[完成] 校验过写回 g_ope_steps[se][6|7]（j1/j2），[取消] 全丢。 */
static void ne_open_target(int se, int slot)
{
    if (se < 0 || se >= g_ope_nsteps || (slot != NE_TGT_J1 && slot != NE_TGT_J2)) return;
    if (g_ope_steps[se][0] != OP_STEP_COND_REGION && g_ope_steps[se][0] != OP_STEP_COND_TOGGLE &&
        g_ope_steps[se][0] != OP_STEP_FINDIMAGE && g_ope_steps[se][0] != OP_STEP_FINDCOLOR) return;
    g_ope_se = se; g_ope_sf = 0; g_ne_tgt = slot;
    ope_num_load();
    g_ne_msg[0] = 0;
    g_need = 1; g_force_frames = 2;
    ALOGI("op edit 目标格开 第 %d 步 %s（现值 %d）", se + 1,
          slot == NE_TGT_J1 ? "成立目标" : "不成立目标", g_ope_steps[se][5 + slot]);
}

/* 取点结果落点（T2.8；ui_ev_cb 从 poll 线程转发，见其上方的前向声明）：
 * v3：结果把激活格所在坐标对两格同填（(1,2) 或 (3,4)；字面值、中断变量态）——只改本地缓冲，
 * [完成] 才随全字段一次写回（参数层没开着 / 开在别的步 → 无处可落，丢弃）+ 退取点态 + 重画。
 * 捕获标记（T3.2）照旧：记下捕获点 + 时刻，绘制在渲染线程（build_overlay），与回填成败无关。 */
static void pick_ev_apply(int px, int py)
{
    g_pickmk_x = px; g_pickmk_y = py; g_pickmk_t = now_ms();
    if (g_pick) {
        int se = g_pick_se, sf = g_pick_sf, t, nf, idx, fi, p;
        if (se >= 0 && se < g_ope_nsteps && se == g_ope_se && !g_ne_tgt) {   /* 参数层还开着同一步才有缓冲可落 */
            t = g_ope_steps[se][0];
            nf = ope_nfields(t);
            if (sf >= 0 && sf < nf) {
                idx = ope_fidx[t - 1][sf];
                if (idx >= 1 && idx <= 4 && t != OP_STEP_JUMP) {   /* 坐标格（跳转的 a1 = 目标编号，不是坐标） */
                    p = (idx == 1 || idx == 2) ? 1 : 3;            /* 所在坐标对起点：1=(x,y)、3=(x2,y2) */
                    for (fi = 0; fi < nf; fi++) {
                        int c = ope_fidx[t - 1][fi];
                        if (c == p || c == p + 1) {                /* 两格同填：字面值、中断变量态 */
                            g_ne_vals[fi] = (c == p) ? px : py;
                            snprintf(g_ne_text[fi], sizeof g_ne_text[fi], "%d", g_ne_vals[fi]);
                        }
                    }
                    g_ope_vl = 0;                              /* 防御：变量列表开着时也退回参数层（结果已进缓冲） */
                    g_ne_msg[0] = 0;
                    ALOGI("取点 回填 第 %d 步 参数 x,y = %d,%d", se + 1, px, py);
                }
            }
        }
    }
    g_pick = 0;                          /* 退取点态（不管回填成没成：这一轮取点结束了） */
    if (g_pick_t0) {                     /* 只在「这一轮取点确实在飞」时收尾：计时复位 + 弹回编辑层
                                          * （参数层保持 —— g_ope_se 不动）。带外 pick_ev（非取点态）
                                          * 不许展开手动收起的编辑层。 */
        g_pick_t0 = 0;
        g_ope_coll = 0;
    }
    g_need = 1; g_force_frames = 2;      /* 重画 */
}

/* 加一步：默认值必须核心必过 —— 点按 = 逻辑屏中心按住 50ms；滑动 = 中心 → 中心下方 200px、300ms；
 * 等待 = 100ms；按下 / 区域判断 = 中心点（区域判断还须选区域，默认空、[完成] 预检拦）；弹起 / 开关判断 = 无字段；
 * 跳转 = 目标 1（spec §2.1 缺省）；条件步默认 成立继续 / 不成立中止（spec §1.1）；
 * 计算（v5）= 槽 r1、表达式空（加完调用方立即开表达式子层，spec §6）；
 * 找图（T3.2）= 阈值 8、模板待选；找色 = 单点、颜色 #000000 容差 8、点集待选（加完调用方立即开参数层）。
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
    g_ope_exprs[g_ope_nsteps][0] = 0;               /* 新步 expr 清空（v5；计算步待编辑） */
    if (type == OP_STEP_TAP) {
        s6[1] = cx; s6[2] = cy; s6[5] = 50;
    } else if (type == OP_STEP_SWIPE) {
        s6[1] = cx; s6[2] = cy; s6[3] = cx; s6[4] = cy + 200; if (s6[4] >= g_h) s6[4] = g_h - 1; s6[5] = 300;
    } else if (type == OP_STEP_DOWN || type == OP_STEP_COND_REGION) {
        s6[1] = cx; s6[2] = cy;
    } else if (type == OP_STEP_WAIT) {
        s6[5] = 100;
    } else if (type == OP_STEP_JUMP) {
        s6[1] = 1;                                   /* 跳转目标默认 1（spec §2.1 缺省） */
    } else if (type == OP_STEP_CALC) {
        s6[1] = 1;                                   /* 计算：槽默认 r1（spec §6：a1=1、其余 0、expr 空） */
    } else if (type == OP_STEP_FINDIMAGE) {
        s6[1] = 8;                                   /* 找图：阈值默认 8（spec VISION §11-#5） */
    } else if (type == OP_STEP_FINDCOLOR) {
        s6[1] = 0;                                   /* 找色：模式默认单点 */
        s6[2] = 8;                                   /* 颜色 #000000、容差 8（打包 (0<<8)|8） */
    }
    /* 弹起 / 开关判断：无字段（s6 已清零）。条件步与视觉步默认：不成立中止（a3=0）/ 成立继续（a4=CONT）。 */
    if (type == OP_STEP_COND_REGION || type == OP_STEP_COND_TOGGLE ||
        type == OP_STEP_FINDIMAGE || type == OP_STEP_FINDCOLOR) s6[4] = OP_COND_CONT;
    g_ope_nsteps++;
    g_ope_msg[0] = 0;
    g_need = 1; g_force_frames = 2;
    ALOGI("op edit 加一步 %s（现 %d 步）", ope_tname(type), g_ope_nsteps);
}

/* 步骤上移/下移（±1）：行序 = 执行序（核心按表顺序跑）；到头不动。 */
static void ope_move(int i, int d)
{
    int j = i + d, t6[8];
    char tr[REGION_ID_MAX + 1], te[OPS_EXPR_MAX + 1];
    if (j < 0 || j >= g_ope_nsteps) return;
    memcpy(t6, g_ope_steps[i], sizeof t6);
    memcpy(g_ope_steps[i], g_ope_steps[j], sizeof t6);
    memcpy(g_ope_steps[j], t6, sizeof t6);
    memcpy(tr, g_ope_refs[i], sizeof tr);            /* ref 跟着步一起动 */
    memcpy(g_ope_refs[i], g_ope_refs[j], sizeof tr);
    memcpy(g_ope_refs[j], tr, sizeof tr);
    memcpy(te, g_ope_exprs[i], sizeof te);           /* expr 跟着步一起动（v5） */
    memcpy(g_ope_exprs[i], g_ope_exprs[j], sizeof te);
    memcpy(g_ope_exprs[j], te, sizeof te);
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
        memcpy(g_ope_exprs[j], g_ope_exprs[j + 1], sizeof g_ope_exprs[0]);   /* expr 跟着步一起动（v5） */
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

/* 关闭编辑覆盖层（含全部子层状态）：[取消] 与保存全成都走它。
 * T2.4 边界：关层时若在取点态 → 撤单（核心 pick_mode 清掉）+ 恢复矩形（g_ope_i=-1 后
 * 三态矩形下一帧回到面板窗口）；收起态一并复位（下次开层是展开态）。 */
static void op_edit_close(void)
{
    g_ope_i = -1;
    if (g_pick) {                                    /* 防御：正常流程取点中只能点收起条，走不到关层 */
        g_pick = 0; g_pick_t0 = 0;
        vtouch_pick_cancel();
        ALOGI("取点 取消（关编辑层）");
    }
    g_ope_coll = 0;                                  /* 收起态复位（会话态只活在开层期间） */
    g_ope_pv = 0;                                    /* 预览页（T2.5）一并关（防御：正常只能经 [关闭] 退出） */
    g_ope_kb = 0; g_ope_kbmsg[0] = 0;
    g_ope_se = -1; g_ne_tgt = 0; g_ne_msg[0] = 0;
    g_ope_vl = 0; g_ope_rl = -1;                    /* 子层状态一并关（变量 / 区域选择弹层） */
    g_ope_ex = 0; g_ope_ex_msg[0] = 0;              /* 表达式子层（v5 计算步）一并关 */
    g_vis_ed = 0; g_vis_num = 0; g_vis_tl = 0; g_vis_pl = 0; g_vis_hex = 0; g_vis_edmsg[0] = 0;   /* 视觉步子层（T3.2） */
    if (g_vis_cap) {                                /* 采集覆盖层（T3.2）一并关（防御：正常只能经 [取消] 退出） */
        g_vis_cap = 0; g_vis_cap_wait = 0; g_vis_cap_err = 0;
        g_vis_kb = 0; g_vis_pick_se = -1;
    }
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
    memset(g_ope_exprs, 0, sizeof g_ope_exprs);      /* 本地 expr 副本先清（v5；读入逐步覆盖） */
    for (s = 0; s < steps; s++) {
        if (vtouch_get_op_step(i, s, &g_ope_steps[s][0], &g_ope_steps[s][1], &g_ope_steps[s][2],
                               &g_ope_steps[s][3], &g_ope_steps[s][4], &g_ope_steps[s][5],
                               g_ope_refs[s], (int)sizeof g_ope_refs[s],
                               &g_ope_steps[s][6], &g_ope_steps[s][7],
                               g_ope_exprs[s], (int)sizeof g_ope_exprs[s]) != 0) {   /* v3：j1/j2（成立/不成立侧跳转目标）；v5：expr（计算步表达式） */
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
    g_ope_se = -1; g_ope_sf = 0; g_ne_tgt = 0; g_ne_msg[0] = 0;
    g_ope_vl = 0; g_ope_rl = -1;                    /* 子层状态一并清（变量 / 区域选择弹层） */
    g_ope_ex = 0; g_ope_ex_msg[0] = 0;              /* 表达式子层（v5 计算步）一并清 */
    g_vis_ed = 0; g_vis_num = 0; g_vis_tl = 0; g_vis_pl = 0; g_vis_hex = 0; g_vis_edmsg[0] = 0;   /* 视觉步子层一并清 */
    g_vis_cap = 0; g_vis_cap_wait = 0; g_vis_cap_err = 0; g_vis_kb = 0; g_vis_pick_se = -1;      /* 采集覆盖层（防御） */
    g_ope_coll = 0; g_pick_t0 = 0;                  /* 收起态 / 取点计时清零（防御：正常流程关层已清） */
    g_ope_pv = 0;                                   /* 预览页状态清零（防御；会话态只活在开层期间） */
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
    int flat[OPE_MAX_STEPS * 8];
    int s, k, rc, perr = 0;
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
        for (k = 0; k < 8; k++) flat[s * 8 + k] = g_ope_steps[s][k];   /* v3：flat 8/步（含 j1/j2） */
    /* ref / expr 通道：g_ope_refs / g_ope_exprs 直接递（每步一格，空串 = 无）—— 条件步的 ref、
     * 计算步的表达式（v5 T3.1 接真实数据）由此进核心 */
    if (vtouch_op_put(g_ope_name, g_ope_gate, g_ope_autoff, flat, g_ope_refs, g_ope_exprs, g_ope_nsteps, &perr) != 0) {
        if (perr == 3)   /* out_err 3 = 核心拒收（投递成功但回读不通过） */
            snprintf(g_ope_msg, sizeof g_ope_msg, "保存被核心拒（名字/步类型/坐标/时长/区域引用照核心校验，含表满）");
        else             /* 1 = 未投递（参数非法）/ 2 = 编辑未送达（超时） */
            snprintf(g_ope_msg, sizeof g_ope_msg, "保存未送达（编辑超时或面板未接共享内存）");
        ALOGW("op edit put 失败 %s（原因码 %d）", g_ope_name, perr);
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

/* 条件步一侧（成立 / 不成立）的行内控件：循环钮 + 档位 = 跳到… 时该侧出现的目标格。
 * 循环钮文字随档位（ope_tier_name），点击按固定序推进（ope_tier_next：继续下一步 → 跳过下一步 →
 * 跳到… → 中止）；目标格显示「第 N 步」/「结束」，点它开目标模式数字键盘编 0..32
 * （ne_open_target，槽 = j1 / j2）。slot 传 NE_TGT_J1（成立侧：档 = a4、目标 = j1）或
 * NE_TGT_J2（不成立侧：档 = a3、目标 = j2）。 */
static void ope_cond_side(int i, int slot)
{
    const int *s6 = g_ope_steps[i];
    int *tier = &g_ope_steps[i][slot == NE_TGT_J1 ? 4 : 3];
    int tgt = s6[slot == NE_TGT_J1 ? 6 : 7];
    const char *side = slot == NE_TGT_J1 ? "成立" : "不成立";
    char lab[48];
    float aw = ImGui::GetContentRegionAvail().x;
    ImGui::PushID(slot);                             /* 两侧同标签（如都是「第 5 步」）也要 ID 唯一 */
    snprintf(lab, sizeof lab, "%s：%s", side, ope_tier_name(*tier));
    if (*tier == OP_COND_JUMP) {                     /* 跳到…：循环钮 + 目标格并排 */
        char cell[24], t2[24];
        float tw = 240.0f, bw = aw - 12 - tw;
        if (bw < 200.0f) bw = 200.0f;                /* 极窄窗兜底 */
        if (btn_light(lab, ImVec2(bw, 72))) {
            *tier = ope_tier_next(*tier);
            ope_tier_text(*tier, tgt, t2, (int)sizeof t2);
            g_need = 1; g_force_frames = 2;
            ALOGI("op edit 条件行为 第 %d 步 %s → %s", i + 1, side, t2);
        }
        ImGui::SameLine();
        if (tgt == 0) snprintf(cell, sizeof cell, "结束");
        else snprintf(cell, sizeof cell, "第 %d 步", tgt);
        if (btn_blue(cell, ImVec2(tw, 72))) ne_open_target(i, slot);   /* 点目标格：编 0..32（0 = 结束） */
    } else if (btn_light(lab, ImVec2(aw, 72))) {
        char t2[24];
        *tier = ope_tier_next(*tier);
        ope_tier_text(*tier, tgt, t2, (int)sizeof t2);
        g_need = 1; g_force_frames = 2;
        ALOGI("op edit 条件行为 第 %d 步 %s → %s", i + 1, side, t2);
    }
    ImGui::PopID();
}

/* 表达式子层入口（v5 计算步）：
 * 计算步的 [参数] 与「＋计算」都走它 —— g_ope_se = 该步、g_ope_ex = 1（子层分发在 draw_op_edit 顶部）、
 * 表达式与槽选择从该步快照进子层缓冲（[确定] 才写回）。 */
static void ope_expr_open(int se)
{
    if (se < 0 || se >= g_ope_nsteps) return;
    if (g_ope_steps[se][0] != OP_STEP_CALC) return;   /* 防御：只有计算步有表达式子层 */
    g_ope_se = se;
    g_ope_ex = 1;
    snprintf(g_ope_expr_buf, sizeof g_ope_expr_buf, "%s", g_ope_exprs[se]);
    g_ope_expr_slot = g_ope_steps[se][1];
    if (g_ope_expr_slot < 1 || g_ope_expr_slot > 4) g_ope_expr_slot = 1;
    g_ope_ex_msg[0] = 0;
    g_need = 1; g_force_frames = 2;
    ALOGI("op edit 表达式开 第 %d 步（槽 r%d）", se + 1, g_ope_expr_slot);
}

/* 视觉步参数层入口（T3.2）：找图/找色的 [参数] 与「＋找图/＋找色」都走它 ——
 * g_ope_se = 该步、g_vis_ed = 1（子层分发在 draw_op_edit 顶部、draw_num_edit 之前）。 */
static void ope_vis_open(int se)
{
    int t;
    if (se < 0 || se >= g_ope_nsteps) return;
    t = g_ope_steps[se][0];
    if (t != OP_STEP_FINDIMAGE && t != OP_STEP_FINDCOLOR) return;   /* 防御：只有视觉步有参数层 */
    g_ope_se = se;
    g_vis_ed = 1;
    g_vis_num = 0; g_vis_tl = 0; g_vis_pl = 0; g_vis_hex = 0;
    g_vis_edmsg[0] = 0;
    g_vis_test_on = 0; g_vis_test_seq = 0; g_vis_test_msg[0] = 0;   /* 试查固定槽清空（M3：换步不串台） */
    g_need = 1; g_force_frames = 2;
    ALOGI("op edit 视觉参数开 第 %d 步 %s", se + 1, ope_tname(t));
}

/* 步骤行：`i. 点按` + 参数小字一行（摘要），下面按键 [参数（无字段的类型不画；计算步 = 表达式子层入口、
 * 视觉步 = 视觉参数层入口）] [↑][↓][删]（行高 72 保手指可点）。条件步（区域判断 / 开关判断）再加三行：
 * 区域下拉 + 成立 / 不成立各一枚四档循环钮（继续下一步 → 跳过下一步 → 跳到… → 中止；档位 = 跳到… 时
 * 该侧出现目标格）。视觉步（找图/找色，T3.2）复用同一对四档循环钮（无区域下拉行 —— 区域在参数层选）。 */
static void ope_step_row(int i)
{
    const int *s6 = g_ope_steps[i];
    int t = s6[0], nf = ope_nfields(t);
    int has_par = (nf > 0 || t == OP_STEP_CALC);   /* 计算步无数字字段，但 [参数] = 表达式子层入口（v5） */
    char tb[32], p[176];         /* 摘要缓冲：v3 条件串最坏 ~113B（变量名 + 区域名 + 两侧档位词）→ 96 不够 */
    ope_step_text(i, p, sizeof p);
    ImGui::PushID(2000 + i);
    snprintf(tb, sizeof tb, "%d. %s", i + 1, ope_tname(t));
    ImGui::TextUnformatted(tb);
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    text_meta_s(p);
    {
        int nbtn = (has_par ? 1 : 0) + 3;            /* [参数] + ↑ ↓ 删 */
        float bw = (ImGui::GetContentRegionAvail().x - (nbtn - 1) * 12) / nbtn;   /* 缝按真实 SameLine 间距 12 扣 */
        if (has_par) {
            if (btn_blue("参数", ImVec2(bw, 72))) {
                if (t == OP_STEP_CALC) {
                    ope_expr_open(i);                /* 计算步：开表达式子层（不开数字键盘） */
                } else if (t == OP_STEP_FINDIMAGE || t == OP_STEP_FINDCOLOR) {
                    ope_vis_open(i);                 /* 视觉步：开参数层（模板/区域/阈值 或 模式/颜色/容差/点集） */
                } else {                             /* 其余：进参数弹层，全字段一屏（v3；点格切换激活） */
                    g_ope_se = i; g_ope_sf = 0; g_ne_tgt = 0;
                    ope_num_load();
                    g_ne_msg[0] = 0;
                    g_need = 1; g_force_frames = 2;
                    ALOGI("op edit 参数开 第 %d 步 %s（%d 格）", i + 1, ope_tname(t), nf);
                }
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
        /* 条件参数：区域下拉（开关判断的列表只列开关型，见 draw_ope_rlist）+ 两枚四档循环钮
         * （成立 / 不成立各一，行内循环编辑；档位 = 跳到… 时该侧出现目标格，见 ope_cond_side） */
        char g[48];
        float aw = ImGui::GetContentRegionAvail().x;
        snprintf(g, sizeof g, "区域：%s", g_ope_refs[i][0] ? g_ope_refs[i] : "未选");
        if (btn_light(g, ImVec2(aw, 72))) {
            g_ope_rl = i;
            g_need = 1; g_force_frames = 2;
            ALOGI("op edit 区域列表开 第 %d 步 %s", i + 1, ope_tname(t));
        }
        ope_cond_side(i, NE_TGT_J1);                 /* 成立侧：档 = a4、目标 = j1 */
        ope_cond_side(i, NE_TGT_J2);                 /* 不成立侧：档 = a3、目标 = j2 */
    } else if (t == OP_STEP_FINDIMAGE || t == OP_STEP_FINDCOLOR) {
        /* 视觉步（T3.2）：成立 / 不成立四档照搬条件步控件（参数化同一实现，防两处漂移 ——
         * 字段映射相同：a4/j1 = 成立侧、a3/j2 = 不成立侧）；模板/区域/阈值等在 [参数] 子层编辑。 */
        ope_cond_side(i, NE_TGT_J1);
        ope_cond_side(i, NE_TGT_J2);
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
                           g_ope_kbmsg, (int)sizeof g_ope_kbmsg, 12.0f);   /* 编辑层整屏窗：顶边距 12 */
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

/* 参数弹层 v3（全字段一屏 / 本地缓冲原子落 / 取点填对）：
 * - 打开 [参数]：该步全部数值字段一次列出（格 = 标签 + 值文本）；点格 = 激活（高亮）；
 *   键盘编激活格；[变量] 仅激活格可变量时显示；[取点] 仅激活格是坐标格时显示。
 * - 本地缓冲原子落：进层快照全部字段（ope_num_load）→ 层内只改缓冲（取点/变量也只改缓冲）→
 *   [完成] 全字段校验全过才一次写回 g_ope_steps（任一不过 → 提示该格 + 激活它 + 停留）；
 *   [取消] 全丢（含取点 / 变量改动）。⚠ 对 v2「逐字段 [确定] 即时落」的有意语义变化（spec §3.1）。
 * - 目标模式（ne_open_target）：单格「成立目标 / 不成立目标」，0..32、0 显示「结束」；无变量 / 取点。 */
static void draw_num_edit(void)
{
    int type, nf, idx, is_coord, var_ok, nbtn, ncol, nrow, fi, col, row, n, bad;
    const char *label;
    ImDrawList *dl;
    ImVec2 wp, a, b, avail;
    float ww, wh, x0, y0, cw, ry, cellw, cellh, cellgap, cy0, vy, vbh, boxw, btnw, bx, by, ky, kw, kh;
    if (g_ope_se < 0 || g_ope_se >= g_ope_nsteps) { g_ope_se = -1; g_ne_tgt = 0; g_vis_num = 0; return; }
    type = g_ope_steps[g_ope_se][0];
    if (g_ne_tgt && type != OP_STEP_COND_REGION && type != OP_STEP_COND_TOGGLE &&
        type != OP_STEP_FINDIMAGE && type != OP_STEP_FINDCOLOR) { g_ope_se = -1; g_ne_tgt = 0; g_vis_num = 0; return; }
    nf = g_ne_tgt ? 1 : ope_nfields(type);
    if (nf == 0) { g_ope_se = -1; g_ne_tgt = 0; g_vis_num = 0; return; }
    if (g_ope_sf < 0 || g_ope_sf >= nf) g_ope_sf = 0;

    dl = ImGui::GetWindowDrawList();
    wp = ImGui::GetWindowPos();
    ww = ImGui::GetWindowWidth();
    wh = ImGui::GetWindowHeight();
    a = ImVec2(wp.x + 12, wp.y + 12);      /* T2.4 整屏窗：顶边距 12（不再避面板标题栏） */
    b = ImVec2(wp.x + ww - 12, wp.y + wh - 12);
    dl->AddRectFilled(a, b, IM_COL32(255, 255, 255, 253), 14);
    dl->AddRect(a, b, IM_COL32(228, 228, 231, 255), 14, 0, 1.5f);
    x0 = a.x + 26; y0 = a.y + 24;
    ImGui::SetCursorScreenPos(ImVec2(x0, y0));
    avail = ImGui::GetContentRegionAvail();          /* 自适应布局：T2.4 全屏化后自然变大（别写死面板窗口尺寸） */
    cw = avail.x - 26;
    if (cw < 420) cw = 420;                          /* 防御下限（现面板宽 864 → ~784） */
    ry = y0 + avail.y;                               /* 内容区底：键盘 / 按钮从这里往上锚（自适应） */
    {   /* 标题 */
        char t[64];
        if (g_ne_tgt)
            snprintf(t, sizeof t, "第 %d 步 · %s · %s（0 = 结束）", g_ope_se + 1, ope_tname(type), ne_label(type, 0));
        else
            snprintf(t, sizeof t, "第 %d 步 · %s · 参数（点格激活）", g_ope_se + 1, ope_tname(type));
        ImGui::SetCursorScreenPos(ImVec2(x0, y0));
        text_meta_s(t);
    }
    /* 全字段一屏：格 = 标签 + 值文本；点格 = 激活（高亮） */
    cellgap = 12.0f;
    ncol = (nf >= 2) ? 2 : 1;
    cellw = (cw - (float)(ncol - 1) * cellgap) / (float)ncol;
    nrow = (nf + ncol - 1) / ncol;
    /* —— 矮屏自适应（横屏/小屏）：格子/值框/按钮/键高按可用高收缩（两级；正常全尺寸）—— */
    float span = ry - y0;
    float cellh_a = 88.0f, vbh_a = 96.0f, bth_a = 92.0f, kh_a;
    {
        float fixed0 = 44.0f + 12.0f + 58.0f + 24.0f + 3.0f * 10.0f;   /* 标题/缝/提示槽/底缝/键缝 */
        float fx1 = fixed0 + (float)nrow * 88.0f + (float)(nrow - 1) * 12.0f + 96.0f + 92.0f;
        float fx2 = fixed0 + (float)nrow * 72.0f + (float)(nrow - 1) * 12.0f + 72.0f + 70.0f;
        kh_a = (span - fx1) / 4.0f;
        if (kh_a < 44.0f) { cellh_a = 72.0f; vbh_a = 72.0f; bth_a = 70.0f; kh_a = (span - fx2) / 4.0f; }
        if (kh_a > 88.0f) kh_a = 88.0f;
        if (kh_a < 44.0f) kh_a = 44.0f;
    }
    cellh = cellh_a;
    cy0 = y0 + 44;
    for (fi = 0; fi < nf; fi++) {
        char vt[24];
        int hit, act;
        col = fi % ncol; row = fi / ncol;
        ImVec2 p0(x0 + (float)col * (cellw + cellgap), cy0 + (float)row * (cellh + cellgap));
        ImGui::PushID(700 + fi);
        ImGui::SetCursorScreenPos(p0);
        hit = ImGui::InvisibleButton("##cell", ImVec2(cellw, cellh));
        ImGui::PopID();
        act = (fi == g_ope_sf);
        ne_cell_text(type, fi, vt, (int)sizeof vt);
        dl->AddRectFilled(p0, ImVec2(p0.x + cellw, p0.y + cellh),
                          act ? IM_COL32(219, 234, 254, 255) : IM_COL32(244, 244, 245, 255), 10.0f);
        dl->AddRect(p0, ImVec2(p0.x + cellw, p0.y + cellh),
                    act ? IM_COL32(59, 130, 246, 255) : IM_COL32(228, 228, 231, 255), 10.0f, 0, act ? 3.0f : 1.5f);
        if (g_font_meta) dl->AddText(g_font_meta, g_font_meta->FontSize, ImVec2(p0.x + 18, p0.y + 8),
                                     IM_COL32(113, 113, 122, 255), ne_label(type, fi));
        else dl->AddText(ImVec2(p0.x + 18, p0.y + 8), IM_COL32(113, 113, 122, 255), ne_label(type, fi));
        dl->AddText(ImVec2(p0.x + 18, p0.y + 40), IM_COL32(24, 24, 27, 255), vt);
        if (hit && fi != g_ope_sf) {                 /* 点格 = 激活该格 */
            g_ope_sf = fi;
            g_ne_msg[0] = 0;
            g_need = 1; g_force_frames = 2;
            ALOGI("op edit 参数激活 第 %d 步 格 %d/%d", g_ope_se + 1, fi + 1, nf);
        }
    }
    /* 激活格：值框 + [取点] / [变量]（仅该格允许时显示） */
    idx = g_ne_tgt ? (5 + g_ne_tgt) : ope_fidx[type - 1][g_ope_sf];
    if (idx < 0) { g_ope_se = -1; g_ne_tgt = 0; g_vis_num = 0; return; }     /* 防御：字段表里没有这一格 */
    label = ne_label(type, g_ope_sf);
    is_coord = !g_ne_tgt && idx >= 1 && idx <= 4 && type != OP_STEP_JUMP &&
               type != OP_STEP_FINDIMAGE && type != OP_STEP_FINDCOLOR;   /* 跳转 a1=目标编号、视觉 a1/a2=阈值/容差：不给 [取点] */
    var_ok = !g_ne_tgt && ope_var_ok(type, idx);
    vy = cy0 + (float)nrow * cellh + (float)(nrow - 1) * cellgap + 12;
    vbh = vbh_a;
    btnw = 210;
    nbtn = (is_coord ? 1 : 0) + (var_ok ? 1 : 0);
    boxw = cw - (float)nbtn * (btnw + 12);
    ImGui::PushStyleColor(ImGuiCol_Border, BLUE500);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.976f, 0.980f, 0.984f, 1.00f));
    ImGui::SetCursorScreenPos(ImVec2(x0, vy));
    ImGui::BeginChild("##numval", ImVec2(boxw, vbh), ImGuiChildFlags_Border, ImGuiWindowFlags_NoScrollbar);
    {
        char show[24];
        ImVec2 tp;
        ImDrawList *d2;
        ne_cell_text(type, g_ope_sf, show, (int)sizeof show);
        tp = ImGui::GetCursorScreenPos();
        d2 = ImGui::GetWindowDrawList();
        if (g_font_meta) d2->AddText(g_font_meta, g_font_meta->FontSize, ImVec2(tp.x + 18, tp.y + 8),
                                     IM_COL32(113, 113, 122, 255), label);
        else d2->AddText(ImVec2(tp.x + 18, tp.y + 8), IM_COL32(113, 113, 122, 255), label);
        d2->AddText(ImVec2(tp.x + 18, tp.y + 40), IM_COL32(24, 24, 27, 255), show);
    }
    ImGui::EndChild();
    ImGui::PopStyleColor(2);
    bx = x0 + boxw + 12;
    if (is_coord) {
        ImGui::SetCursorScreenPos(ImVec2(bx, vy));
        if (btn_light("取点", ImVec2(btnw, vbh))) {
            /* 取点接线照旧（先 cancel 再 request —— 清掉可能残留的旧请求，核心的 0→1 转变与 20s
             * 计时从这一次点按起算）；v3 回填进缓冲的一对坐标格（pick_ev_apply）。
             * T2.4：进取点态 = 编辑层自动收成底部条（「点屏幕上目标位置 · 点这里取消」）；
             * 回填 / 点条取消 / 20s 兜底超时 → 自动弹回编辑层（参数层保持）。 */
            g_pick_se = g_ope_se; g_pick_sf = g_ope_sf;
            vtouch_pick_cancel();
            vtouch_pick_request();
            g_pick = 1;
            g_pick_t0 = now_ms();                    /* 面板自带 20s 兜底计时起点 */
            g_ope_coll = 1;                          /* 自动收起（收起条 = 取消取点） */
            g_ne_msg[0] = 0;
            g_need = 1; g_force_frames = 2;
            ALOGI("取点 请求 第 %d 步 参数 %d/%d（编辑层自动收起）", g_ope_se + 1, g_ope_sf + 1, nf);
        }
        bx += btnw + 12;
    }
    if (var_ok) {
        ImGui::SetCursorScreenPos(ImVec2(bx, vy));
        if (btn_light("变量", ImVec2(btnw, vbh))) {          /* 弹变量列表（值 ↔ 变量切换，只改缓冲） */
            g_ope_vl = 1;
            g_ne_msg[0] = 0;
            g_need = 1; g_force_frames = 2;
            ALOGI("op edit 变量列表开 第 %d 步 参数 %d/%d", g_ope_se + 1, g_ope_sf + 1, nf);
        }
    }
    if (g_pick) {
        /* 取点态提示条（T2.8）：与错误提示共用固定槽位（键盘位置不动，防误点）。
         * T2.4 起正常流程这里画不到（进取点态即自动收起，参数层不画）—— 防御保留：
         * 若某路径让 g_pick 与展开态并存，提示仍指向正确出口（底部条）。 */
        ImVec2 p1 = ImVec2(x0, vy + vbh + 12), p2 = ImVec2(x0 + cw, p1.y + 46);
        dl->AddRectFilled(p1, p2, IM_COL32(219, 234, 254, 255), 8);
        dl->AddRect(p1, p2, IM_COL32(59, 130, 246, 255), 8, 0, 1.5f);
        dl->AddText(ImVec2(p1.x + 14, p1.y + 3), IM_COL32(29, 78, 216, 255), "点屏幕上目标位置（点底部条取消）");
    } else if (g_ne_msg[0]) {
        ImGui::SetCursorScreenPos(ImVec2(x0, vy + vbh + 12));
        ImGui::TextColored(ImVec4(0.863f, 0.149f, 0.149f, 1.00f), "%s", g_ne_msg);
    }
    /* 数字键：3 列 x 4 行（1-9 / ⌫ 0），编激活格；底部 [取消] 全丢 / [完成] 全字段校验全过一次写回。
     * 键盘与按钮自底向上锚（自适应：T2.4 全屏化后自然变大）。 */
    {
        static const char *nrow2[3] = { "123", "456", "789" };
        float gap = 10.0f, bw2;
        kh = kh_a;                                       /* 矮屏自适应键高（上算） */
        kw = (cw - 2 * gap) / 3.0f;
        by = ry - 24 - bth_a;                            /* [取消][完成] 行 */
        ky = by - 12 - (4 * kh + 3 * gap);               /* 键盘顶 */
        for (row = 0; row < 3; row++) {
            for (col = 0; col < 3; col++) {
                char lab[2] = { nrow2[row][col], 0 };
                ImGui::PushID(400 + row * 3 + col);
                ImGui::SetCursorScreenPos(ImVec2(x0 + (float)col * (kw + gap), ky + (float)row * (kh + gap)));
                if (btn_light(lab, ImVec2(kw, kh))) {
                    if (g_ne_vals[g_ope_sf] < 0) { g_ne_vals[g_ope_sf] = 0; g_ne_text[g_ope_sf][0] = 0; }  /* 按数字 = 弃变量、改字面输入 */
                    n = (int)strlen(g_ne_text[g_ope_sf]);
                    if (n < 6) {
                        g_ne_text[g_ope_sf][n] = lab[0]; g_ne_text[g_ope_sf][n + 1] = 0;
                        g_ne_vals[g_ope_sf] = ne_parse(g_ne_text[g_ope_sf]);
                    } else snprintf(g_ne_msg, sizeof g_ne_msg, "最多 6 位数字");
                    g_need = 1; g_force_frames = 2;
                }
                ImGui::PopID();
            }
        }
        /* 第 4 行：退格 + 0（右格留空） */
        ImGui::PushID(430);
        ImGui::SetCursorScreenPos(ImVec2(x0, ky + 3 * (kh + gap)));
        if (btn_light("退格", ImVec2(kw, kh))) {
            if (g_ne_vals[g_ope_sf] < 0) { g_ne_vals[g_ope_sf] = 0; g_ne_text[g_ope_sf][0] = 0; }      /* 同上：切回字面输入 */
            n = (int)strlen(g_ne_text[g_ope_sf]);
            if (n > 0) g_ne_text[g_ope_sf][n - 1] = 0;
            g_ne_vals[g_ope_sf] = ne_parse(g_ne_text[g_ope_sf]);
            g_ne_msg[0] = 0;
            g_need = 1; g_force_frames = 2;
        }
        ImGui::PopID();
        ImGui::PushID(431);
        ImGui::SetCursorScreenPos(ImVec2(x0 + kw + gap, ky + 3 * (kh + gap)));
        if (btn_light("0", ImVec2(kw, kh))) {
            if (g_ne_vals[g_ope_sf] < 0) { g_ne_vals[g_ope_sf] = 0; g_ne_text[g_ope_sf][0] = 0; }      /* 同上：切回字面输入 */
            n = (int)strlen(g_ne_text[g_ope_sf]);
            if (n < 6) {
                g_ne_text[g_ope_sf][n] = '0'; g_ne_text[g_ope_sf][n + 1] = 0;
                g_ne_vals[g_ope_sf] = ne_parse(g_ne_text[g_ope_sf]);
            } else snprintf(g_ne_msg, sizeof g_ne_msg, "最多 6 位数字");
            g_need = 1; g_force_frames = 2;
        }
        ImGui::PopID();
        /* [取消] 全丢 / [完成] 全字段校验全过才一次写回 */
        bw2 = (cw - gap) * 0.5f;
        ImGui::PushID(450);
        ImGui::SetCursorScreenPos(ImVec2(x0, by));
        if (btn_light("取消", ImVec2(bw2, bth_a))) {
            ALOGI("op edit 参数取消 第 %d 步", g_ope_se + 1);
            if (g_pick) { g_pick = 0; g_pick_t0 = 0; g_ope_coll = 0; vtouch_pick_cancel(); }   /* 防御：未回的取点请求也一并撤（全丢） */
            if (g_vis_num) { g_vis_num = 0; }        /* 视觉参数层开的数字键盘：回视觉层（g_ope_se 保持） */
            else g_ope_se = -1;
            g_ne_tgt = 0; g_ne_msg[0] = 0;
            g_need = 1; g_force_frames = 3;
        }
        ImGui::SetCursorScreenPos(ImVec2(x0 + bw2 + gap, by));
        if (btn_blue("完成", ImVec2(bw2, bth_a))) {
            bad = -1;
            {
                char why[72];
                if (g_ne_tgt) {
                    if (!ne_check_target(label, g_ne_vals[0], why, (int)sizeof why)) bad = 0;
                } else {
                    for (fi = 0; fi < nf; fi++) {
                        char f2[72];
                        if (!ne_check(ope_flabel[type - 1][fi], type, fi, g_ne_vals[fi], f2, (int)sizeof f2)) {
                            snprintf(why, sizeof why, "%s", f2);
                            bad = fi;
                            break;
                        }
                    }
                }
                if (bad >= 0) {                              /* 任一不过：提示该格 + 激活它 + 停留 */
                    g_ope_sf = bad;
                    snprintf(g_ne_msg, sizeof g_ne_msg, "%s", why);
                    ALOGI("op edit 参数拒收 第 %d 步 格 %d：%s", g_ope_se + 1, bad + 1, why);
                    g_force_frames = 2;
                }
            }
            if (bad < 0) {                                   /* 全过：一次写回全部字段（目标模式 = j1/j2 单格） */
                if (g_ne_tgt) {
                    g_ope_steps[g_ope_se][5 + g_ne_tgt] = g_ne_vals[0];
                    ALOGI("op edit 参数完成 第 %d 步 %s = %d", g_ope_se + 1, label, g_ne_vals[0]);
                } else {
                    for (fi = 0; fi < nf; fi++) ne_field_set(g_ope_se, type, fi, g_ne_vals[fi]);   /* 视觉容差走拆包写 */
                    ALOGI("op edit 参数完成 第 %d 步（%d 格）", g_ope_se + 1, nf);
                    if (g_vis_num)                           /* 视觉参数层：每格一行（T7.4：阈值/容差 + 超时 ms） */
                        for (fi = 0; fi < nf; fi++)
                            ALOGI("vis edit 第 %d 步 %s = %d", g_ope_se + 1, ope_flabel[type - 1][fi], g_ne_vals[fi]);
                }
                if (g_vis_num) { g_vis_num = 0; }            /* 视觉参数层开的数字键盘：回视觉层 */
                else g_ope_se = -1;
                g_ne_tgt = 0; g_ne_msg[0] = 0;
                g_need = 1; g_force_frames = 3;
            }
        }
        ImGui::PopID();
    }
}

/* 变量选择弹层（参数弹层的 [变量]）：9 项中文名（触发变量 5 + 结果1..结果4，spec §1.1 逐字 + v5 §6）
 * +「数值」回退 + [取消]。
 * v3：选中 / 回退只改本地缓冲（g_ne_vals / g_ne_text）—— [完成] 才随全字段一次写回，[取消] 全丢。
 * 选中变量 / 结果槽 → 缓冲值存 -9..-1（触发变量 -1..-5 / 结果槽 -6..-9）、文本清空（格 / 值框显示中文名）；
 * 「数值」→ 回字面输入（从引用切回才清文本；本来就是字面则保留已输入的数字）。
 * 矮屏自适应（v5）：11 条（9 变量 + 数值 + 取消）全可见全可点 —— 常规单列；放不下改**双列**
 * （左列 5 / 右列 4 + 底行 [数值][取消] 并排；条目高按可用高再收缩）。验收口径：最短边 ≥640。 */
static void draw_ope_vlist(void)
{
    ImDrawList *dl;
    ImVec2 wp, a, b;
    float ww, wh, x0, y0, cw, by, bww;
    int type, nf, idx, v, k, two_col;
    if (g_ope_se < 0 || g_ope_se >= g_ope_nsteps || g_ne_tgt) { g_ope_vl = 0; return; }
    type = g_ope_steps[g_ope_se][0];
    nf = ope_nfields(type);
    if (nf == 0 || g_ope_sf < 0 || g_ope_sf >= nf) { g_ope_vl = 0; g_ope_se = -1; return; }
    idx = ope_fidx[type - 1][g_ope_sf];
    if (idx < 0 || !ope_var_ok(type, idx)) { g_ope_vl = 0; return; }
    v = g_ne_vals[g_ope_sf];

    dl = ImGui::GetWindowDrawList();
    wp = ImGui::GetWindowPos();
    ww = ImGui::GetWindowWidth();
    wh = ImGui::GetWindowHeight();
    a = ImVec2(wp.x + 12, wp.y + 12);      /* T2.4 整屏窗：顶边距 12（不再避面板标题栏） */
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
    text_meta_s("选一个变量或结果槽；「数值」= 回退数字键盘输入");
    /* 矮屏自适应：条目高按窗高收缩（全屏层 wh = 屏高）；单列放不下 11 条 → 双列 */
    float ih = 84.0f;
    if (wh < 1000.0f) ih = 64.0f;
    if (wh < 760.0f) ih = 52.0f;
    {
        float avail = (b.y - 24) - (y0 + 100);          /* 列表起点（y0+100）到底部的可用高 */
        two_col = (11.0f * ih + 10.0f * 12.0f > avail) ? 1 : 0;
        if (two_col) {                                  /* 双列 = 6 行（5 变量行 + 底行）：条目高再收缩压进可用高 */
            float ihm = (avail - 5.0f * 12.0f) / 6.0f;
            if (ih > ihm) ih = ihm;
            if (ih < 36.0f) ih = 36.0f;                 /* 再矮接受溢出（最短边 ≥640 已保证全可见） */
        }
    }
    bww = two_col ? (cw - 12) * 0.5f : cw;              /* 钮宽：单列 = 全宽；双列 = 半宽 */
    for (k = 0; k < 9; k++) {
        int col = (two_col && k >= 5) ? 1 : 0;          /* 双列：左列 5 / 右列 4 */
        int row = (two_col && k >= 5) ? k - 5 : k;
        ImGui::PushID(600 + k);
        ImGui::SetCursorScreenPos(ImVec2(x0 + (float)col * (bww + 12), y0 + 100 + (float)row * (ih + 12)));
        if ((v == -(k + 1)) ? btn_blue(ope_vname_tab[k], ImVec2(bww, ih))
                            : btn_light(ope_vname_tab[k], ImVec2(bww, ih))) {
            g_ne_vals[g_ope_sf] = -(k + 1);          /* 缓冲存 -9..-1；显示交给中文名表 */
            g_ne_text[g_ope_sf][0] = 0;
            g_ope_vl = 0;
            g_need = 1; g_force_frames = 3;
            ALOGI("op edit 参数变量 第 %d 步 %s = %s", g_ope_se + 1,
                  ope_flabel[type - 1][g_ope_sf], ope_vname_tab[k]);
        }
        ImGui::PopID();
    }
    by = y0 + 100 + (two_col ? 5.0f : 9.0f) * (ih + 12);   /* 底行 y（双列 = 第 6 行；单列 = 第 10 行） */
    ImGui::PushID(610);
    ImGui::SetCursorScreenPos(ImVec2(x0, by));
    if ((v >= 0 ? btn_blue("数值", ImVec2(bww, ih)) : btn_light("数值", ImVec2(bww, ih)))) {
        if (v < 0) { g_ne_text[g_ope_sf][0] = 0; g_ne_vals[g_ope_sf] = 0; }   /* 变量/槽 → 字面：清文本重新输入；本来就是字面则保留 */
        g_ope_vl = 0;
        g_need = 1; g_force_frames = 3;
        ALOGI("op edit 参数变量 第 %d 步 %s → 数值输入", g_ope_se + 1, ope_flabel[type - 1][g_ope_sf]);
    }
    ImGui::PopID();
    ImGui::PushID(611);
    ImGui::SetCursorScreenPos(two_col ? ImVec2(x0 + bww + 12, by) : ImVec2(x0, by + ih + 12));
    if (btn_light("取消", ImVec2(bww, ih))) {
        g_ope_vl = 0;
        g_need = 1; g_force_frames = 2;
        ALOGI("op edit 变量列表取消 第 %d 步", g_ope_se + 1);
    }
    ImGui::PopID();
}

/* 表达式缓冲追加（字符键 / 插入 chips 共用）：token 原样追加（空白无所谓，spec §6）；追加后超
 * 63 字符 → 红字拒收提示、缓冲不动。 */
static void ope_expr_add(const char *tok)
{
    int n = (int)strlen(g_ope_expr_buf);
    int tl = (int)strlen(tok);
    if (n + tl > OPS_EXPR_MAX) {
        snprintf(g_ope_ex_msg, sizeof g_ope_ex_msg, "最多 %d 个字符", OPS_EXPR_MAX);
        return;
    }
    memcpy(g_ope_expr_buf + n, tok, (size_t)tl + 1);
    g_ope_ex_msg[0] = 0;
}

/* 表达式子层（v5 计算步，spec §6；v5.1 修订：键区补 `,`、退格移到 [清空] 旁、+ 变量图例与公式快捷行）：
 * 整屏卡片（照 draw_num_edit 的 T2.4 口径）——标题 + 槽 chips [r1..r4]（单选高亮）+ 显示框（当前文本 /
 * 「(空)」）+ 右侧 [清空] [退格] 并排（退格 = 删末字符）+ 变量图例行（小字，逐字照 spec）
 * + 字符键 6×3（1..6 / 7 8 9 0 + - / * / ( ) . ,；追加式）+ 插入 chips 2×8（tdx..r3 / r4 atan2( sin(
 * cos( abs( min( max( sqrt(；token 原样追加）+ 公式快捷行 5 键（小字；追加式插入整条公式）+ 底 [取消][确定]。
 * 矮屏自适应/拖滚兜底照 draw_char_kb（g_kb_sc / g_zone_kb / SCR_KB 互斥复用）：键高按键区
 * 可用高算、两级紧凑；仍放不下 → 键区手动拖滚（内容随 g_kb_sc 整体位移、裁剪在卡片内、隔帧重置）。
 * [确定] 走同源 vtouch_expr_check（不过 → 红字 why、层不关；过 → 写回 g_ope_exprs[g_ope_se]
 * 与槽选择 → 该步 a1，日志 `op edit 计算 第 N 步 r1 = <表达式>` 后关层）；[取消] 丢弃 + 日志。 */
static void draw_ope_expr(void)
{
    ImDrawList *dl;
    ImVec2 wp, a, b;
    float ww, wh, x0, y0, cw, sy, vy, vbh, boxw, btnw, msg_y, ky0, ky, kh, chh, bth, slot_h, msg_h, gap, kw, cw2, bw2, by;
    float khn, bbot, avail, content, kb_max, leg_h, leg_y, fy, fw5;
    int se = g_ope_se;
    int k, r, c;
    if (se < 0 || se >= g_ope_nsteps || !g_ope_ex) { g_ope_ex = 0; return; }
    if (g_ope_steps[se][0] != OP_STEP_CALC) { g_ope_ex = 0; return; }   /* 防御：非计算步不该开这层 */

    dl = ImGui::GetWindowDrawList();
    wp = ImGui::GetWindowPos();
    ww = ImGui::GetWindowWidth();
    wh = ImGui::GetWindowHeight();
    a = ImVec2(wp.x + 12, wp.y + 12);      /* 整屏窗：顶边距 12（同 draw_num_edit） */
    b = ImVec2(wp.x + ww - 12, wp.y + wh - 12);
    dl->AddRectFilled(a, b, IM_COL32(255, 255, 255, 253), 14);
    dl->AddRect(a, b, IM_COL32(228, 228, 231, 255), 14, 0, 1.5f);
    x0 = a.x + 26; y0 = a.y + 24; cw = (b.x - x0) - 26;

    /* —— 矮屏自适应（照 draw_char_kb 口径）：键高按键区可用高算、两级紧凑；仍放不下 → 拖滚兜底 —— */
    slot_h = 64.0f; vbh = 96.0f; msg_h = 46.0f; chh = 64.0f; bth = 92.0f; gap = 10.0f; leg_h = 24.0f;
    bbot = b.y - 12.0f;                    /* 键区可见底（内容贴不到卡边，留 12 边距） */
    ky0 = y0 + 44.0f + slot_h + 12.0f + vbh + 12.0f + leg_h + msg_h;
    khn = (bbot - ky0 - (3.0f * chh + 2.0f * gap) - 12.0f - 2.0f * gap - 12.0f - bth) / 3.0f;
    if (khn > 88.0f) khn = 88.0f;
    if (khn < 56.0f) {                     /* 紧凑档：槽行 / 显示框 / 小字行（chips/公式）全收一档 */
        slot_h = 56.0f; vbh = 72.0f; chh = 40.0f; bth = 70.0f;
        ky0 = y0 + 44.0f + slot_h + 12.0f + vbh + 12.0f + leg_h + msg_h;
        khn = (bbot - ky0 - (3.0f * chh + 2.0f * gap) - 12.0f - 2.0f * gap - 12.0f - bth) / 3.0f;
        if (khn > 88.0f) khn = 88.0f;
    }
    if (khn < 40.0f) khn = 40.0f;          /* 再矮由手动拖滚兜底 */
    kh = khn;

    /* 标题 */
    {
        char t[64];
        snprintf(t, sizeof t, "第 %d 步 · 计算 · 表达式", se + 1);
        ImGui::SetCursorScreenPos(ImVec2(x0, y0));
        text_meta_s(t);
    }
    /* 槽 chips [r1][r2][r3][r4]：单选，选中高亮（蓝）；写 g_ope_expr_slot（[确定] 才落该步 a1） */
    sy = y0 + 44.0f;
    {
        float sg = 12.0f, sw = (cw - 3.0f * sg) / 4.0f;
        for (k = 0; k < 4; k++) {
            char lab[8];
            snprintf(lab, sizeof lab, "r%d", k + 1);
            ImGui::PushID(5200 + k);
            ImGui::SetCursorScreenPos(ImVec2(x0 + k * (sw + sg), sy));
            if ((g_ope_expr_slot == k + 1) ? btn_blue(lab, ImVec2(sw, slot_h))
                                           : btn_light(lab, ImVec2(sw, slot_h))) {
                if (g_ope_expr_slot != k + 1) {
                    g_ope_expr_slot = k + 1;
                    ALOGI("op edit 表达式槽选 第 %d 步 → r%d", se + 1, k + 1);
                }
                g_ope_ex_msg[0] = 0;
                g_need = 1; g_force_frames = 2;
            }
            ImGui::PopID();
        }
    }
    /* 显示框（当前文本 /「(空)」）+ 右侧 [清空] [退格] 并排两键（框宽 = cw − 2×btnw − 2×gap；
     * 退格 = 删末字符，与旧键区退格同逻辑） */
    vy = sy + slot_h + 12.0f;
    btnw = 210.0f;
    boxw = cw - 2.0f * btnw - 2.0f * gap;
    ImGui::PushStyleColor(ImGuiCol_Border, BLUE500);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.976f, 0.980f, 0.984f, 1.00f));
    ImGui::SetCursorScreenPos(ImVec2(x0, vy));
    ImGui::BeginChild("##opexpr", ImVec2(boxw, vbh), ImGuiChildFlags_Border, ImGuiWindowFlags_NoScrollbar);
    {
        ImVec2 tp = ImGui::GetCursorScreenPos();
        ImDrawList *d2 = ImGui::GetWindowDrawList();
        char show[OPS_EXPR_MAX + 8];
        snprintf(show, sizeof show, "%s", g_ope_expr_buf[0] ? g_ope_expr_buf : "(空)");
        if (vbh >= 88.0f) {                /* 全尺寸：标签 + 值两行（T3.1 壳口径） */
            if (g_font_meta) d2->AddText(g_font_meta, g_font_meta->FontSize, ImVec2(tp.x + 18, tp.y + 8),
                                         IM_COL32(113, 113, 122, 255), "表达式");
            else d2->AddText(ImVec2(tp.x + 18, tp.y + 8), IM_COL32(113, 113, 122, 255), "表达式");
            d2->AddText(ImVec2(tp.x + 18, tp.y + 40), IM_COL32(24, 24, 27, 255), show);
        } else {                           /* 紧凑档：单行值（框矮，标签省） */
            d2->AddText(ImVec2(tp.x + 18, tp.y + 12), IM_COL32(24, 24, 27, 255), show);
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor(2);
    ImGui::SetCursorScreenPos(ImVec2(x0 + boxw + gap, vy));
    if (btn_light("清空", ImVec2(btnw, vbh))) {
        g_ope_expr_buf[0] = 0;
        g_ope_ex_msg[0] = 0;
        g_need = 1; g_force_frames = 2;
        ALOGI("op edit 表达式清空 第 %d 步", se + 1);
    }
    ImGui::SetCursorScreenPos(ImVec2(x0 + boxw + gap + btnw + gap, vy));
    if (btn_light("退格", ImVec2(btnw, vbh))) {    /* 删末字符（追加式，无光标移动） */
        int n = (int)strlen(g_ope_expr_buf);
        if (n > 0) g_ope_expr_buf[n - 1] = 0;
        g_ope_ex_msg[0] = 0;
        g_need = 1; g_force_frames = 2;
    }
    /* 变量图例行（spec §6 v5.1 逐字；小字层级，框下、键区上） */
    leg_y = vy + vbh + 12.0f;
    {
        const char *lg = "tdx,tdy 按下 · tux,tuy 弹起 · tms 按压时长(ms) · r1..r4 结果槽";
        if (g_font_meta) dl->AddText(g_font_meta, g_font_meta->FontSize, ImVec2(x0, leg_y), IM_COL32(113, 113, 122, 255), lg);
        else             dl->AddText(ImVec2(x0, leg_y), IM_COL32(113, 113, 122, 255), lg);
    }
    /* 拒收提示固定槽（出现/消失不动键区，防误点） */
    msg_y = leg_y + leg_h;
    if (g_ope_ex_msg[0]) {
        ImGui::SetCursorScreenPos(ImVec2(x0, msg_y));
        ImGui::TextColored(ImVec4(0.863f, 0.149f, 0.149f, 1.00f), "%s", g_ope_ex_msg);
    }
    ky0 = msg_y + msg_h;
    /* —— 键区：字符键 6×3 + 插入 chips 2×8 + 公式快捷行 + 底行（超高时手动拖滚：g_zone_kb + SCR_KB；
     * 整块随 g_kb_sc 位移，[取消][确定] 随块滚，同 draw_char_kb）—— */
    avail = bbot - ky0;
    content = 3.0f * kh + 2.0f * gap + 12.0f + 3.0f * chh + 2.0f * gap + 12.0f + bth;   /* 字符键 + 缝 + 插入 chips + 公式行 + 缝 + 底行 */
    kb_max = content - avail;
    {
        static int lf = -1;                    /* 隔帧重置：重开/换层 = 回顶（同 draw_char_kb） */
        if (ImGui::GetFrameCount() - lf > 1) g_kb_sc = 0;
        lf = ImGui::GetFrameCount();
        if (g_scroll_acc != 0 && g_scr_target == SCR_KB) { g_kb_sc += g_scroll_acc; g_scroll_acc = 0; }
        if (kb_max < 0) kb_max = 0;
        if (g_kb_sc < 0) g_kb_sc = 0;                  /* 下限 clamp：与上限对称（拖滚不为负） */
        if (g_kb_sc > kb_max) g_kb_sc = kb_max;
        pub_zone(g_zone_kb);
        ImGui::PushClipRect(ImVec2(a.x + 4, ky0 - 4), ImVec2(b.x - 4, b.y - 4), true);
    }
    ky = ky0 - g_kb_sc;
    {
        static const char *krow[3] = { "123456", "7890+-", "*/().," };   /* v5.1：补 `,`（语法全字符集 6×3）；退格移出键区 */
        kw = (cw - 5.0f * gap) / 6.0f;
        for (r = 0; r < 3; r++) {
            for (c = 0; c < 6; c++) {
                char lab[2] = { krow[r][c], 0 };
                ImGui::PushID(5300 + r * 6 + c);
                ImGui::SetCursorScreenPos(ImVec2(x0 + c * (kw + gap), ky + r * (kh + gap)));
                if (btn_light(lab, ImVec2(kw, kh))) {
                    ope_expr_add(lab);
                    g_need = 1; g_force_frames = 2;
                }
                ImGui::PopID();
            }
        }
    }
    {
        static const char *crow[2][8] = {
            { "tdx", "tdy", "tux", "tuy", "tms", "r1", "r2", "r3" },
            { "r4", "atan2(", "sin(", "cos(", "abs(", "min(", "max(", "sqrt(" },
        };
        float cy = ky + 3.0f * kh + 2.0f * gap + 12.0f;
        cw2 = (cw - 7.0f * gap) / 8.0f;
        if (g_font_meta) ImGui::PushFont(g_font_meta);   /* 小字层级：一排 8 枚，长 token 也放得下 */
        for (r = 0; r < 2; r++) {
            for (c = 0; c < 8; c++) {
                ImGui::PushID(5400 + r * 8 + c);
                ImGui::SetCursorScreenPos(ImVec2(x0 + c * (cw2 + gap), cy + r * (chh + gap)));
                if (btn_light(crow[r][c], ImVec2(cw2, chh))) {
                    ope_expr_add(crow[r][c]);
                    g_need = 1; g_force_frames = 2;
                }
                ImGui::PopID();
            }
        }
        /* 公式快捷行（5 键，小字；追加式插入整条公式 —— spec §6 v5.1 模板逐字） */
        {
            static const char *frow[5] = { "释放角度", "偏移X", "偏移Y", "延伸X", "延伸Y" };
            static const char *ftok[5] = {
                "atan2(tuy-tdy, tux-tdx)", "tux-tdx", "tuy-tdy",
                "tdx + cos(atan2(tuy-tdy, tux-tdx)) * 300",
                "tdy + sin(atan2(tuy-tdy, tux-tdx)) * 300",
            };
            fy = cy + 2.0f * (chh + gap);
            fw5 = (cw - 4.0f * gap) / 5.0f;
            for (c = 0; c < 5; c++) {
                ImGui::PushID(5600 + c);
                ImGui::SetCursorScreenPos(ImVec2(x0 + c * (fw5 + gap), fy));
                if (btn_light(frow[c], ImVec2(fw5, chh))) {
                    ope_expr_add(ftok[c]);
                    g_need = 1; g_force_frames = 2;
                }
                ImGui::PopID();
            }
        }
        if (g_font_meta) ImGui::PopFont();
    }
    /* 底：[取消] 丢弃 / [确定] 同源校验 → 写回（随键区块滚） */
    bw2 = (cw - 12.0f) * 0.5f;
    by = ky + 3.0f * kh + 2.0f * gap + 12.0f + 3.0f * chh + 2.0f * gap + 12.0f;   /* v5.1：+ 公式行 */
    ImGui::PushID(5500);
    ImGui::SetCursorScreenPos(ImVec2(x0, by));
    if (btn_light("取消", ImVec2(bw2, bth))) {
        ALOGI("op edit 表达式取消 第 %d 步", se + 1);
        g_ope_ex = 0; g_ope_ex_msg[0] = 0; g_ope_se = -1;
        g_need = 1; g_force_frames = 3;
    }
    ImGui::SetCursorScreenPos(ImVec2(x0 + bw2 + 12, by));
    if (btn_blue("确定", ImVec2(bw2, bth))) {
        char why[72];
        why[0] = 0;
        if (vtouch_expr_check(g_ope_expr_buf, why, (int)sizeof why) != 0) {
            snprintf(g_ope_ex_msg, sizeof g_ope_ex_msg, "%s", why[0] ? why : "表达式错");
            ALOGI("op edit 计算拒收 第 %d 步：%s", se + 1, g_ope_ex_msg);
            g_need = 1; g_force_frames = 2;
        } else {
            snprintf(g_ope_exprs[se], sizeof g_ope_exprs[se], "%s", g_ope_expr_buf);
            g_ope_steps[se][1] = g_ope_expr_slot;      /* 槽选择（chips）写回该步 a1 */
            ALOGI("op edit 计算 第 %d 步 r%d = %s", se + 1, g_ope_expr_slot, g_ope_exprs[se]);
            g_ope_ex = 0; g_ope_ex_msg[0] = 0; g_ope_se = -1;
            g_need = 1; g_force_frames = 3;
        }
    }
    ImGui::PopID();
    ImGui::PopClipRect();                  /* 键区裁剪到此（含 [取消][确定]，同 draw_char_kb） */
}

/* 区域选择弹层（条件步的 [区域] 按钮；T3.2 起视觉步 [区域] 也走它）：列表读区域表实时、可滚动；
 * 开关判断只列开关型（spec §7：运行时核心仍校验，双保险）。点一条 → 写进该步 ref、关层。
 * 视觉步（找图/找色）：ref 承载**区域名**（区域限定查找范围），另给「全屏」= 清空引用。 */
static void draw_ope_rlist(void)
{
    ImDrawList *dl;
    ImVec2 wp, a, b;
    float ww, wh, x0, y0, cw, list_top, list_bot, list_h;
    int i, n, rli = g_ope_rl, only_tg, vis, avail = 0;
    if (rli < 0 || rli >= g_ope_nsteps) { g_ope_rl = -1; return; }
    only_tg = (g_ope_steps[rli][0] == OP_STEP_COND_TOGGLE);
    vis = (g_ope_steps[rli][0] == OP_STEP_FINDIMAGE || g_ope_steps[rli][0] == OP_STEP_FINDCOLOR);   /* 视觉步：区域 = 查找范围 */

    dl = ImGui::GetWindowDrawList();
    wp = ImGui::GetWindowPos();
    ww = ImGui::GetWindowWidth();
    wh = ImGui::GetWindowHeight();
    a = ImVec2(wp.x + 12, wp.y + 12);      /* T2.4 整屏窗：顶边距 12（不再避面板标题栏） */
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
    text_meta_s(vis ? "点一个区域作为查找范围（不限定 = 「全屏」）"
              : only_tg ? "只列开关型区域（在区域页设）" : "点一个区域作为判定目标（悬空引用运行时报「区域不存在」）");
    list_top = y0 + 76;
    list_bot = b.y - 24 - 92 - 12;                   /* 底部给 [取消] 留位 */
    list_h = list_bot - list_top;
    if (list_h < 0) list_h = 0;                      /* 矮屏：列表让位（按钮优先；列表本就可滚） */
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ZINC50);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 12));
    ImGui::SetCursorScreenPos(ImVec2(x0, list_top));
    ImGui::BeginChild("##opregs", ImVec2(cw, list_h), ImGuiChildFlags_None,
                      ImGuiWindowFlags_AlwaysVerticalScrollbar);
    pub_zone(g_zone_list);                           /* 列表实区：拖它滚动（同区域/操作列表的口径） */
    drag_scroll_for(SCR_LIST);
    if (vis) {                                       /* 视觉步：可清空区域引用 = 全屏（T3.2） */
        ImGui::PushID(3050);
        if ((g_ope_exprs[rli][0] == 0) ? btn_blue("全屏（不限区域）", ImVec2(ImGui::GetContentRegionAvail().x, 76))
                                      : btn_light("全屏（不限区域）", ImVec2(ImGui::GetContentRegionAvail().x, 76))) {
            g_ope_exprs[rli][0] = 0;
            g_ope_rl = -1;
            g_need = 1; g_force_frames = 3;
            ALOGI("vis edit 第 %d 步 区域=全屏", rli + 1);
        }
        ImGui::PopID();
    }
    n = vtouch_region_count();
    for (i = 0; i < n; i++) {
        char id[16]; int t2, a1, a2, a3, a4, en;
        if (only_tg && vtouch_region_kind_get(i) != 1) continue;
        if (vtouch_get_region(i, id, sizeof id, &t2, &a1, &a2, &a3, &a4, &en) != 0) continue;
        avail++;
        ImGui::PushID(3000 + i);
        if ((strcmp(vis ? g_ope_exprs[rli] : g_ope_refs[rli], id) == 0) ? btn_blue(id, ImVec2(ImGui::GetContentRegionAvail().x, 76))
                                               : btn_light(id, ImVec2(ImGui::GetContentRegionAvail().x, 76))) {
            snprintf(vis ? g_ope_exprs[rli] : g_ope_refs[rli], vis ? sizeof g_ope_exprs[rli] : sizeof g_ope_refs[rli], "%s", id);
            g_ope_rl = -1;
            g_need = 1; g_force_frames = 3;
            if (vis) ALOGI("vis edit 第 %d 步 区域=%s", rli + 1, id);
            else ALOGI("op edit 区域选中 第 %d 步 = %s", rli + 1, id);
        }
        ImGui::PopID();
    }
    if (avail == 0)
        text_meta_w(vis ? "还没有区域：可用上面的「全屏」不限定范围"
                  : only_tg ? "没有开关型区域：先到区域页把某个区域设成开关型" : "还没有区域：先到区域页框一个");
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

/* ---- 预览页（T2.5，spec §5）：编辑层 [预览] → 全屏**只读**页（步骤总览 + 坐标小地图） ---- */

/* 分支目标文本（预览用）：档位词；跳转档的目标 = `第 N 步` / `结束`（与「跳转行」同口径）。 */
static void ope_pv_target(int tier, int tgt, char *out, int outcap)
{
    switch (tier) {
    case OP_COND_CONT: snprintf(out, (size_t)outcap, "继续下一步"); break;
    case OP_COND_SKIP: snprintf(out, (size_t)outcap, "跳过下一步"); break;
    case OP_COND_JUMP:
        if (tgt == 0) snprintf(out, (size_t)outcap, "结束");
        else          snprintf(out, (size_t)outcap, "第 %d 步", tgt);
        break;
    default:           snprintf(out, (size_t)outcap, "中止"); break;   /* OP_COND_ABORT（非法值兜底同款） */
    }
}

/* 预览清单行（T2.5）：`N. 类型` + 参数摘要（**逐字 ope_step_text** 出文本；meta 字体，超宽自动折行）；
 * 条件步再两行分支 `├ 成立 → …` / `└ 不成立 → …`（档位词；跳转档目标 = `第 N 步` / `结束`）；
 * 跳转步的摘要行 = `→ 第 N 步` / `→ 结束`（spec §5 的「跳转行」）。纯只读文本，无任何控件。 */
static void ope_preview_row(int i)
{
    const int *s6 = g_ope_steps[i];
    int t = s6[0];
    char tb[48], p[176], tg[32], b1[80], b2[80];
    ImGui::PushID(5000 + i);
    if (t == OP_STEP_JUMP) {
        if (s6[1] == 0) snprintf(tg, sizeof tg, "结束");
        else            snprintf(tg, sizeof tg, "第 %d 步", s6[1]);
        snprintf(tb, sizeof tb, "%d. %s → %s", i + 1, ope_tname(t), tg);
        ImGui::TextUnformatted(tb);
    } else {
        snprintf(tb, sizeof tb, "%d. %s", i + 1, ope_tname(t));
        ImGui::TextUnformatted(tb);
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ope_step_text(i, p, sizeof p);
        meta_push();
        ImGui::TextWrapped("%s", p);
        meta_pop();
    }
    if (t == OP_STEP_COND_REGION || t == OP_STEP_COND_TOGGLE ||
        t == OP_STEP_FINDIMAGE || t == OP_STEP_FINDCOLOR) {   /* 视觉步（T3.2）：同条件步两行分支 */
        ope_pv_target(s6[4], s6[6], tg, (int)sizeof tg);      /* 成立侧：档 a4 / 目标 j1 */
        snprintf(b1, sizeof b1, "├ 成立 → %s", tg);
        ope_pv_target(s6[3], s6[7], tg, (int)sizeof tg);      /* 不成立侧：档 a3 / 目标 j2 */
        snprintf(b2, sizeof b2, "└ 不成立 → %s", tg);
        meta_push();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 40);    /* 缩进一档（分支从属该步） */
        ImGui::TextUnformatted(b1);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 40);
        ImGui::TextUnformatted(b2);
        meta_pop();
    }
    ImGui::Dummy(ImVec2(0, 6));                               /* 行间缝 */
    ImGui::PopID();
}

/* 逻辑坐标 → 小地图坐标（越界贴边）：先把值夹回 [0,g_w-1]/[0,g_h-1] 再等比映射；标记中心再夹进
 * 地图内缘 16px（保标记完整可见）；oob 置 1 = 发生过越界（由标签加「越界」标注）。 */
static void pv_xy(int x, int y, float mx0, float my0, float mw, float mh,
                  float *ox, float *oy, int *oob)
{
    float fx = (float)x, fy = (float)y;
    int o = 0;
    if (fx < 0) { fx = 0; o = 1; } else if (fx > (float)(g_w - 1)) { fx = (float)(g_w - 1); o = 1; }
    if (fy < 0) { fy = 0; o = 1; } else if (fy > (float)(g_h - 1)) { fy = (float)(g_h - 1); o = 1; }
    *ox = mx0 + fx * mw / (float)g_w;
    *oy = my0 + fy * mh / (float)g_h;
    if (*ox < mx0 + 16) *ox = mx0 + 16;
    if (*ox > mx0 + mw - 16) *ox = mx0 + mw - 16;
    if (*oy < my0 + 16) *oy = my0 + 16;
    if (*oy > my0 + mh - 16) *oy = my0 + mh - 16;
    *oob = o;
}

/* 点标签（步号；越界加「越界」后缀）：meta 字体；避让 = 与已放标签太近就下移一行（自定口径：
 * 先到先占、后来者让位；≤32 点、最多下移 32 行）；右边缘翻到点左侧、顶边翻到点下方。 */
static void pv_label(float *lx, float *ly, int *nl, float px, float py, int step1, int oob,
                     float mx0, float mx1, float my0, float my1)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    char t[24];
    float tx, ty, tw;
    int i, k, hit;
    if (oob) snprintf(t, sizeof t, "%d 越界", step1);
    else     snprintf(t, sizeof t, "%d", step1);
    tw = g_font_meta ? g_font_meta->CalcTextSizeA(g_font_meta->FontSize, 1e9f, 0.0f, t).x
                     : ImGui::CalcTextSize(t).x;
    tx = px + 18; ty = py - 46;                       /* 默认：点右上角 */
    if (tx + tw > mx1 - 6) tx = px - 18 - tw;         /* 右边缘：翻到点的左侧 */
    if (tx < mx0 + 6) tx = mx0 + 6;
    if (ty < my0 + 6) ty = py + 26;                   /* 顶边：翻到点下方 */
    for (k = 0; k < OPE_MAX_STEPS; k++) {             /* 避让：与已放标签重叠 → 下移一行 */
        hit = 0;
        for (i = 0; i < *nl; i++)
            if (tx - lx[i] > -110 && tx - lx[i] < 110 && ty - ly[i] > -46 && ty - ly[i] < 46) { hit = 1; break; }
        if (!hit) break;
        ty += 46;
    }
    if (ty > my1 - 40) ty = my1 - 40;                 /* 兜底贴底（极端多标签才到） */
    if (*nl < OPE_MAX_STEPS) { lx[*nl] = tx; ly[*nl] = ty; (*nl)++; }
    if (g_font_meta) dl->AddText(g_font_meta, g_font_meta->FontSize, ImVec2(tx, ty), IM_COL32(24, 24, 27, 255), t);
    else             dl->AddText(ImVec2(tx, ty), IM_COL32(24, 24, 27, 255), t);
}

/* 小地图（T2.5）：竖屏逻辑比例图 —— g_w×g_h 等比缩入矩形 (mx0,my0,mw,mh)。
 * 标记：点按 = 蓝点 / 按下 = 橙点 / 区域判断判定点 = 紫叉 / 滑动 = 带箭头连线（箭头 = 终点）；
 * 每点旁标步号；越界坐标贴边 + 标签加「越界」；坐标格是变量 / 结果槽引用（-9..-1）→ 不画该点（列表照显示）。 */
static void draw_preview_map(float mx0, float my0, float mw, float mh)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    float lx[OPE_MAX_STEPS], ly[OPE_MAX_STEPS];
    int nl = 0, i;
    dl->AddRectFilled(ImVec2(mx0, my0), ImVec2(mx0 + mw, my0 + mh), IM_COL32(250, 250, 250, 255), 10.0f);
    dl->AddRect(ImVec2(mx0, my0), ImVec2(mx0 + mw, my0 + mh), IM_COL32(212, 212, 216, 255), 10.0f, 0, 2.0f);
    for (i = 0; i < g_ope_nsteps; i++) {
        const int *s6 = g_ope_steps[i];
        int t = s6[0], oob1 = 0, oob2 = 0;
        float px1 = 0, py1 = 0, px2 = 0, py2 = 0;
        if (t == OP_STEP_TAP || t == OP_STEP_DOWN || t == OP_STEP_COND_REGION) {
            if (ope_vname(s6[1]) || ope_vname(s6[2])) continue;   /* 变量引用坐标 → 不画该点 */
            pv_xy(s6[1], s6[2], mx0, my0, mw, mh, &px1, &py1, &oob1);
            if (t == OP_STEP_TAP) {
                dl->AddCircleFilled(ImVec2(px1, py1), 13.0f, PV_BLUE);
            } else if (t == OP_STEP_DOWN) {
                dl->AddCircleFilled(ImVec2(px1, py1), 13.0f, PV_ORANGE);
            } else {
                dl->AddLine(ImVec2(px1 - 14, py1 - 14), ImVec2(px1 + 14, py1 + 14), PV_PURPLE, 6.0f);
                dl->AddLine(ImVec2(px1 - 14, py1 + 14), ImVec2(px1 + 14, py1 - 14), PV_PURPLE, 6.0f);
            }
            pv_label(lx, ly, &nl, px1, py1, i + 1, oob1, mx0, mx0 + mw, my0, my0 + mh);
        } else if (t == OP_STEP_SWIPE) {
            float dx, dy, len;
            if (ope_vname(s6[1]) || ope_vname(s6[2]) || ope_vname(s6[3]) || ope_vname(s6[4])) continue;
            pv_xy(s6[1], s6[2], mx0, my0, mw, mh, &px1, &py1, &oob1);
            pv_xy(s6[3], s6[4], mx0, my0, mw, mh, &px2, &py2, &oob2);
            dx = px2 - px1; dy = py2 - py1; len = sqrtf(dx * dx + dy * dy);
            if (len >= 1.0f) {
                float ux = dx / len, uy = dy / len, ah = 24.0f;   /* 箭头长（沿单位向量 u） */
                dl->AddLine(ImVec2(px1, py1), ImVec2(px2, py2), PV_BLUE, 6.0f);
                dl->AddTriangleFilled(ImVec2(px2, py2),
                                      ImVec2(px2 - ux * ah - uy * ah * 0.55f, py2 - uy * ah + ux * ah * 0.55f),
                                      ImVec2(px2 - ux * ah + uy * ah * 0.55f, py2 - uy * ah - ux * ah * 0.55f),
                                      PV_BLUE);
            } else {
                dl->AddCircleFilled(ImVec2(px1, py1), 13.0f, PV_BLUE);   /* 零长滑动兜底：画成点 */
            }
            pv_label(lx, ly, &nl, px1, py1, i + 1, oob1 || oob2, mx0, mx0 + mw, my0, my0 + mh);
        }
        /* 等待 / 弹起 / 开关判断 / 跳转 / 计算：无坐标 → 图上无标记 */
    }
}

/* 预览页小字（meta 字体；无 meta 字体时退回默认字体，同其它手绘文字）。 */
static void pv_text(float x, float y, const char *s)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    if (g_font_meta) dl->AddText(g_font_meta, g_font_meta->FontSize, ImVec2(x, y), IM_COL32(63, 63, 70, 255), s);
    else             dl->AddText(ImVec2(x, y), IM_COL32(63, 63, 70, 255), s);
}

/* 小地图图例（预览页地图右侧）：左边画与地图同款的样本形状，右边文字说明
 * （形状自绘，不依赖特殊字形；特殊字形只有 ├ / └ / → / 「」，字库随源码自动收录）。 */
static void pv_legend(float x, float y)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    float cy = y;
    pv_text(x, cy, "小地图 · 竖屏逻辑坐标等比");
    cy += 46;
    dl->AddCircleFilled(ImVec2(x + 16, cy + 14), 13.0f, PV_BLUE);
    pv_text(x + 44, cy, "点按");
    cy += 52;
    dl->AddCircleFilled(ImVec2(x + 16, cy + 14), 13.0f, PV_ORANGE);
    pv_text(x + 44, cy, "按下");
    cy += 52;
    dl->AddLine(ImVec2(x + 4, cy + 2), ImVec2(x + 28, cy + 26), PV_PURPLE, 6.0f);
    dl->AddLine(ImVec2(x + 4, cy + 26), ImVec2(x + 28, cy + 2), PV_PURPLE, 6.0f);
    pv_text(x + 44, cy, "区域判断 判定点");
    cy += 52;
    dl->AddLine(ImVec2(x + 2, cy + 14), ImVec2(x + 26, cy + 14), PV_BLUE, 6.0f);
    dl->AddTriangleFilled(ImVec2(x + 44, cy + 14), ImVec2(x + 24, cy + 6), ImVec2(x + 24, cy + 22), PV_BLUE);
    pv_text(x + 60, cy, "滑动（箭头 = 终点）");
    cy += 52;
    pv_text(x, cy, "每点旁标步号；越界点标签带「越界」");
    cy += 40;
    pv_text(x, cy, "变量/结果槽引用坐标不画点（列表照显示）");
}

/* 预览页（T2.5，spec §5）：编辑层 [预览] → 全屏**只读**页（无编辑 / 无取点）——
 * 上 = 步骤总览（全宽可滚；摘要逐字 ope_step_text；条件双行分支；跳转行 → 第 N 步 / 结束）；
 * 下 = 小地图带（竖屏逻辑 g_w×g_h 等比缩入 + 图例）；[关闭] 回编辑层。
 * 只读口径：本页只画文本/图形 + 一个 [关闭]；不改 g_ope_steps、不碰取点态。
 * 吞触摸矩形照旧：三态判据只看 g_ope_i / g_ope_coll（编辑层开 = 整屏），本页不改它们。 */
static void draw_op_preview(void)
{
    ImDrawList *dl;
    ImVec2 wp, a, b;
    float ww, wh, x0, y0, cw, content_top, content_bot, content_h, band_h, list_h;
    int i;
    if (g_ope_i < 0) { g_ope_pv = 0; return; }       /* 防御：编辑层关着不该进这 */
    dl = ImGui::GetWindowDrawList();
    wp = ImGui::GetWindowPos();
    ww = ImGui::GetWindowWidth();
    wh = ImGui::GetWindowHeight();
    a = ImVec2(wp.x + 12, wp.y + 12);                /* T2.4 整屏窗：顶边距 12 */
    b = ImVec2(wp.x + ww - 12, wp.y + wh - 12);
    dl->AddRectFilled(a, b, IM_COL32(255, 255, 255, 253), 14);
    dl->AddRect(a, b, IM_COL32(228, 228, 231, 255), 14, 0, 1.5f);
    x0 = a.x + 26; y0 = a.y + 24; cw = (b.x - x0) - 26;
    {
        char t[96];
        snprintf(t, sizeof t, "预览 · %s · %d 步 · 只读", g_ope_name, g_ope_nsteps);
        ImGui::SetCursorScreenPos(ImVec2(x0, y0));
        text_meta_s(t);
    }
    ImGui::SetCursorScreenPos(ImVec2(x0, y0 + 40));
    text_meta_s("清单与编辑层同口径；小地图 = 竖屏逻辑坐标等比图（[关闭] 返回编辑）");

    content_top = y0 + 76;
    content_bot = b.y - 24 - 92 - 12;                /* 底部 [关闭] 92 高 + 12 缝 */
    content_h = content_bot - content_top;
    if (content_h < 320) content_h = 320;            /* 极窄窗兜底（正常 ≥2000） */
    band_h = content_h * 0.42f;                      /* 下带（小地图）自适应；上限防大屏空耗 */
    if (band_h < 340) band_h = 340;
    if (band_h > 1100) band_h = 1100;
    list_h = content_h - band_h - 12;
    if (list_h < 140) { list_h = 140; band_h = content_h - list_h - 12; if (band_h < 200) band_h = 200; }

    /* 上：步骤总览（全宽、可滚；只读文本 —— 拖列表滚动照常） */
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ZINC50);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 12));
    ImGui::SetCursorScreenPos(ImVec2(x0, content_top));
    ImGui::BeginChild("##opvlist", ImVec2(cw, list_h), ImGuiChildFlags_None,
                      ImGuiWindowFlags_AlwaysVerticalScrollbar);
    pub_zone(g_zone_list);
    drag_scroll_for(SCR_LIST);
    for (i = 0; i < g_ope_nsteps; i++) ope_preview_row(i);
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

    /* 下：小地图带（地图按 g_w:g_h 等比缩入左半区；右半区图例） */
    {
        float by0 = content_top + list_h + 12, bh = band_h;
        float legw = cw * 0.5f, mapw0 = cw - legw - 12;
        float sc = mapw0 / (float)g_w, sy = bh / (float)g_h, mw2, mh2, mx0, my0;
        if (sy < sc) sc = sy;                        /* 等比：取小者（缩入可用区域） */
        mw2 = (float)g_w * sc; mh2 = (float)g_h * sc;
        mx0 = x0 + (mapw0 - mw2) * 0.5f;             /* 地图在左半区居中 */
        my0 = by0 + (bh - mh2) * 0.5f;
        draw_preview_map(mx0, my0, mw2, mh2);
        pv_legend(x0 + mapw0 + 24, by0 + 6);
    }

    /* [关闭] 回编辑层 */
    ImGui::PushID(5100);
    ImGui::SetCursorScreenPos(ImVec2(x0, b.y - 24 - 92));
    if (btn_light("关闭", ImVec2(cw, 92))) {
        g_ope_pv = 0;
        g_need = 1; g_force_frames = 2;
        ALOGI("op edit 预览关（回编辑层）");
    }
    ImGui::PopID();
}

/* 横屏编辑器（T7.3，⑤）：左右分栏 —— 左 ~60% 步骤列表（主区，整高滚动）；右 ~40% 控制栏，
 * 自上而下：名字行 / 门控·自动关行 / 加步区（3 列 × 4 行竖排，按列分组：基础 / 控制 / 条件）/
 * [预览][收起] / [取消][完成]。名字与提示槽顶锚、两排钮底锚，余量留中间。
 * 窄档（44px 长文案放不下）门控/自动关/加步整组降 30px 小字（同标题栏「降一档字号」口径）；
 * 极窄只留「门控 / 自动关」；加步「＋」前缀与判断短文案按实算宽度定档（同竖屏 cw 阈值口径）。 */
static void draw_op_edit_land(float x0, float y0, float cw, float wh)
{
    float by = wh - 36.0f;                   /* 内容底（= b.y - 24） */
    float lw = (cw - 12.0f) * 0.6f;          /* 左：步骤列表（主区） */
    float rw = cw - 12.0f - lw;              /* 右：控制栏（~40%） */
    float rx = x0 + lw + 12.0f;
    float hn, hr, hd, bw3, gy, slot;         /* 名字 / 通用行 / [取消][完成] 行高；加步钮宽；门控行 y；提示槽高 */
    int sm, mini, plus, plus4, j4, sm2;
    char t[80];

    /* 右栏三档自适应（同竖屏「按可用高定档」口径；内容 = 名字 + 提示槽 + 6 行 + [取消][完成] + 8 缝）。
     * 提示槽：宽裕档 92（提示可两行换行，M-1 修复）；紧档 46（单行裁剪）。 */
    hn = 84.0f; hr = 76.0f; hd = 92.0f; slot = 92.0f;
    if (by - y0 < 84.0f + 92.0f + 6.0f * 76.0f + 92.0f + 8.0f * 12.0f) { hn = 64.0f; hr = 58.0f; hd = 70.0f; }
    if (by - y0 < 64.0f + 92.0f + 6.0f * 58.0f + 70.0f + 8.0f * 12.0f) { hn = 56.0f; hr = 48.0f; hd = 60.0f; slot = 46.0f; }
    /* 窄档判定（按实算宽度反推：「放得下才用长文案」；44px 字形宽 = 字号、按钮内缝 32） */
    sm    = ((rw - 12.0f) * 0.545f < 252.0f);   /* 「自动关：开」（44px）放不下 → 降 30px 小字档 */
    mini  = ((rw - 12.0f) * 0.455f < 152.0f);   /* 「门控：无」（30px）也放不下 → 只留「门控 / 自动关」 */
    bw3   = (rw - 24.0f) / 3.0f;
    plus  = (bw3 >= 164.0f);                    /* 加步「＋X」（三字 44px）放得下 */
    plus4 = (bw3 >= 252.0f);                    /* 「＋区域判断」（五字）放得下 */
    j4    = (bw3 >= 208.0f);                    /* 「区域判断」（四字）放得下 */
    sm2   = ((rw - 12.0f) * 0.5f < 120.0f);     /* 两字钮（44px）放不下 → 降 30px */

    /* 左栏：标题 + 列表（整高滚动；窄档缩短文案，防越栏被右栏盖住 —— M-1 修复） */
    ImGui::SetCursorScreenPos(ImVec2(x0, y0));
    if (lw < 640.0f) snprintf(t, sizeof t, "步骤 · %d 步", g_ope_nsteps);
    else snprintf(t, sizeof t, "步骤 · %d 步（最多 %d；列表可上下拖动滚）", g_ope_nsteps, OPE_MAX_STEPS);
    text_meta_s(t);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ZINC50);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 12));
    ImGui::SetCursorScreenPos(ImVec2(x0, y0 + 36.0f));
    ImGui::BeginChild("##opsteps", ImVec2(lw, by - y0 - 36.0f), ImGuiChildFlags_None,
                      ImGuiWindowFlags_AlwaysVerticalScrollbar);
    pub_zone(g_zone_list);                   /* 列表实区：拖它滚动（同竖屏口径） */
    drag_scroll_for(SCR_LIST);
    for (int i = 0; i < g_ope_nsteps; i++) ope_step_row(i);
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

    /* 右栏 · 名字行（顶锚；点它开改名子层） */
    ImGui::SetCursorScreenPos(ImVec2(rx, y0));
    if (btn_light(g_ope_name, ImVec2(rw, hn))) {
        g_ope_kb = 1; g_ope_kbmsg[0] = 0; g_ope_up = 0;
        g_need = 1; g_force_frames = 2;
        ALOGI("op edit 改名子层开 %s", g_ope_name);
    }
    /* 右栏 · 提示槽（固定占位：出现提示时下面整块不动，防误点；宽裕档两行换行 + 槽内裁剪，M-1 修复） */
    if (g_ope_msg[0]) {
        float my0 = y0 + hn + 12.0f;
        ImGui::PushClipRect(ImVec2(rx, my0), ImVec2(rx + rw, my0 + slot), true);
        ImGui::SetCursorScreenPos(ImVec2(rx, my0));
        if (slot >= 92.0f) ImGui::PushTextWrapPos(rx + rw);
        ImGui::TextColored(ImVec4(0.863f, 0.149f, 0.149f, 1.00f), "%s", g_ope_msg);
        if (slot >= 92.0f) ImGui::PopTextWrapPos();
        ImGui::PopClipRect();
    }
    /* 右栏 · 门控 · 自动关 · 加步（窄档整组 30px 小字） */
    gy = y0 + hn + 12.0f + slot + 12.0f;
    if (sm) meta_push();
    if (mini) snprintf(t, sizeof t, "门控");
    else snprintf(t, sizeof t, "门控：%s", g_ope_gate[0] ? g_ope_gate : "无");
    ImGui::SetCursorScreenPos(ImVec2(rx, gy));
    if (btn_light(t, ImVec2((rw - 12.0f) * 0.455f, hr))) ope_gate_cycle();
    if (mini) snprintf(t, sizeof t, "自动关");
    else snprintf(t, sizeof t, "自动关：%s", g_ope_autoff ? "开" : "关");
    ImGui::SetCursorScreenPos(ImVec2(rx + (rw - 12.0f) * 0.455f + 12.0f, gy));
    if (btn_light(t, ImVec2((rw - 12.0f) * 0.545f, hr))) {
        g_ope_autoff = !g_ope_autoff;
        g_need = 1; g_force_frames = 2;
        ALOGI("op edit 自动关 %s", g_ope_autoff ? "开" : "关");
    }
    {
        /* 加步（3 列 × 4 行竖排：按列分组 —— 基础 / 控制 / 条件；第 12 格空。加完开子层同竖屏口径） */
        float c2 = rx + bw3 + 12.0f, c3 = rx + 2.0f * (bw3 + 12.0f);
        float a1 = gy + hr + 12.0f;
        float a2 = a1 + hr + 12.0f, a3 = a2 + hr + 12.0f, a4 = a3 + hr + 12.0f;
        ImGui::SetCursorScreenPos(ImVec2(rx, a1));
        if (btn_light(plus ? "＋点按" : "点按", ImVec2(bw3, hr))) ope_add_step(OP_STEP_TAP);
        ImGui::SetCursorScreenPos(ImVec2(rx, a2));
        if (btn_light(plus ? "＋滑动" : "滑动", ImVec2(bw3, hr))) ope_add_step(OP_STEP_SWIPE);
        ImGui::SetCursorScreenPos(ImVec2(rx, a3));
        if (btn_light(plus ? "＋等待" : "等待", ImVec2(bw3, hr))) ope_add_step(OP_STEP_WAIT);
        ImGui::SetCursorScreenPos(ImVec2(rx, a4));
        if (btn_light(plus ? "＋找图" : "找图", ImVec2(bw3, hr))) {
            int n0 = g_ope_nsteps;
            ope_add_step(OP_STEP_FINDIMAGE);
            if (g_ope_nsteps > n0) ope_vis_open(g_ope_nsteps - 1);
        }
        ImGui::SetCursorScreenPos(ImVec2(c2, a1));
        if (btn_light(plus ? "＋按下" : "按下", ImVec2(bw3, hr))) ope_add_step(OP_STEP_DOWN);
        ImGui::SetCursorScreenPos(ImVec2(c2, a2));
        if (btn_light(plus ? "＋弹起" : "弹起", ImVec2(bw3, hr))) ope_add_step(OP_STEP_UP);
        ImGui::SetCursorScreenPos(ImVec2(c2, a3));
        if (btn_light(plus ? "＋跳转" : "跳转", ImVec2(bw3, hr))) ope_add_step(OP_STEP_JUMP);
        ImGui::SetCursorScreenPos(ImVec2(c2, a4));
        if (btn_light(plus ? "＋找色" : "找色", ImVec2(bw3, hr))) {
            int n0 = g_ope_nsteps;
            ope_add_step(OP_STEP_FINDCOLOR);
            if (g_ope_nsteps > n0) ope_vis_open(g_ope_nsteps - 1);
        }
        ImGui::SetCursorScreenPos(ImVec2(c3, a1));
        if (btn_light(plus4 ? "＋区域判断" : (j4 ? "区域判断" : "区域"), ImVec2(bw3, hr))) ope_add_step(OP_STEP_COND_REGION);
        ImGui::SetCursorScreenPos(ImVec2(c3, a2));
        if (btn_light(plus4 ? "＋开关判断" : (j4 ? "开关判断" : "开关"), ImVec2(bw3, hr))) ope_add_step(OP_STEP_COND_TOGGLE);
        ImGui::SetCursorScreenPos(ImVec2(c3, a3));
        if (btn_light(plus ? "＋计算" : "计算", ImVec2(bw3, hr))) {
            int n0 = g_ope_nsteps;
            ope_add_step(OP_STEP_CALC);
            if (g_ope_nsteps > n0) ope_expr_open(g_ope_nsteps - 1);
        }
        /* 第 3 列第 4 行：空（11 键 = 4+4+3） */
    }
    if (sm) meta_pop();
    /* 右栏 · [预览][收起] / [取消][完成]（底锚两排；窄档两字钮 30px） */
    {
        float bw = (rw - 12.0f) * 0.5f;
        float py = by - hd - 12.0f - hr;
        if (sm2) meta_push();
        ImGui::SetCursorScreenPos(ImVec2(rx, py));
        if (btn_light("预览", ImVec2(bw, hr))) {
            g_ope_pv = 1;                    /* 全屏只读页（T2.5）；吞触摸矩形照旧整屏 */
            g_need = 1; g_force_frames = 2;
            ALOGI("op edit 预览开（%d 步）", g_ope_nsteps);
        }
        ImGui::SetCursorScreenPos(ImVec2(rx + bw + 12.0f, py));
        if (btn_light("收起", ImVec2(bw, hr))) {
            g_ope_coll = 1;
            g_need = 1; g_force_frames = 2;
            ALOGI("op edit 收起（编辑状态保留；点条展开）");
        }
        ImGui::SetCursorScreenPos(ImVec2(rx, by - hd));
        if (btn_light("取消", ImVec2(bw, hd))) {    /* 取消 = 丢本轮编辑回操作页（改名弹层同款语义） */
            ALOGI("op edit 取消（丢编辑）");
            op_edit_close();
        }
        ImGui::SetCursorScreenPos(ImVec2(rx + bw + 12.0f, by - hd));
        if (btn_blue("完成", ImVec2(bw, hd))) op_edit_save();
        if (sm2) meta_pop();
    }
}

/* 编辑层主屏（T2.4 起按**当前屏整屏**绘制，见 build_edit_layer）：头部（名字行 + [预览][收起]）/
 * 步骤列表（可滚）/ 加步（v5 三行九类型 3×3）/ 门控循环 / 跑完自动关 / [取消][完成]。
 * 子层（名字键盘、数字弹层、变量选择、区域选择、表达式、预览页）开着时本屏不画（子层整面盖住）。
 * T7.3（⑤）：横屏（g_scr_w > g_scr_h）整屏改左右分栏（draw_op_edit_land）；竖屏本函数为现状布局
 * 收紧行高（加步 4×3 紧凑置底）；竖屏列表保底 120px 给不出时也退分栏（硬约束兜底）。 */
static void draw_op_edit(void)
{
    ImDrawList *dl;
    ImVec2 wp, a, b;
    float ww, wh, x0, y0, cw, msg_h, ly, by_bottom, done_y, autooff_y, gate_y, add_y1, add_y2, add_y3, list_top, list_bot, list_h;

    if (g_ope_pv) { draw_op_preview(); return; }     /* 预览页（T2.5）：只读层，最高优先（编辑层头 [预览] 进） */
    if (g_ope_kb) { draw_ope_name_kb(); return; }
    if (g_ope_vl) { draw_ope_vlist(); return; }      /* 变量选择弹层（数字弹层之上） */
    if (g_ope_rl >= 0) { draw_ope_rlist(); return; } /* 区域选择弹层（条件步） */
    if (g_ope_ex) { draw_ope_expr(); return; }       /* 表达式子层（v5 计算步）—— 在 g_ope_se 之前 */
    if (g_vis_ed) { draw_vis_edit(); return; }       /* 视觉步参数层（T3.2）—— 数字键盘在它内部（g_vis_num） */
    if (g_ope_se >= 0) { draw_num_edit(); return; }

    dl = ImGui::GetWindowDrawList();
    wp = ImGui::GetWindowPos();
    ww = ImGui::GetWindowWidth();
    wh = ImGui::GetWindowHeight();
    a = ImVec2(wp.x + 12, wp.y + 12);      /* T2.4 整屏窗：顶边距 12（不再避面板标题栏） */
    b = ImVec2(wp.x + ww - 12, wp.y + wh - 12);
    dl->AddRectFilled(a, b, IM_COL32(255, 255, 255, 253), 14);
    dl->AddRect(a, b, IM_COL32(228, 228, 231, 255), 14, 0, 1.5f);
    x0 = a.x + 26; y0 = a.y + 24; cw = (b.x - x0) - 26;
    /* T7.3 布局重排（⑤）：横屏（g_scr_w > g_scr_h）左右分栏 —— 左步骤列表（主区）/ 右控制栏。 */
    if (g_scr_w > g_scr_h) { draw_op_edit_land(x0, y0, cw, wh); return; }
    /* ---- 竖屏几何（T7.3：先算后画 —— 兜底判定要在落笔之前）----
     * 步骤区标题 + 底部按钮锚点（从底往上排，列表拿中间剩下的高度） */
    msg_h = 46.0f;                           /* 提示槽固定占位：出现提示时下面整块不动，防误点 */
    ly = y0 + 68 + 84 + 12 + msg_h;
    /* 底部按钮行高自适应（矮屏/横屏）：先压矮按钮、再让列表，保证 [取消]/[完成]/加步/门控/自动关
     * 永远在屏内可点（列表内部本就可滚）。两档紧凑，阈值按「底锚区可用高」算；v5 加步三行 → 5 行 bh_row。
     * T7.3：默认行高收紧（76→64 / 92→80）—— 加步区 4×3 紧凑置底，列表（主区）多拿高度。 */
    float bh_row = 64.0f, bh_done = 80.0f;
    {
        float avail_b = (b.y - 24.0f) - (ly + 36.0f);
        if (avail_b < 140.0f + 80.0f + 5.0f * (64.0f + 12.0f)) { bh_row = 56.0f; bh_done = 68.0f; }
        if (avail_b < 80.0f + 68.0f + 5.0f * (56.0f + 12.0f)) { bh_row = 48.0f; bh_done = 60.0f; }
    }
    by_bottom = b.y - 24;
    done_y = by_bottom - bh_done;
    autooff_y = done_y - 12 - bh_row;
    gate_y = autooff_y - 12 - bh_row;
    add_y3 = gate_y - 12 - bh_row;           /* 加步第三行（最下）：区域判断 / 开关判断 / 计算 */
    add_y2 = add_y3 - 12 - bh_row;           /* 加步第二行：按下 / 弹起 / 跳转 */
    add_y1 = add_y2 - 12 - bh_row;           /* 加步第一行（最上）：点按 / 滑动 / 等待 */
    list_top = ly + 36;
    list_bot = add_y1 - 12;
    list_h = list_bot - list_top;
    /* T7.3 硬约束兜底：竖屏连列表 120px 保底都给不出（极矮 / 方形屏）→ 退横屏分栏，保全元素可达 */
    if (list_h < 120.0f) { draw_op_edit_land(x0, y0, cw, wh); return; }
    /* 头部：名字小字 + 右 [收起] 钮（T2.4：收成底部条，编辑状态全保留；点条再展开 ——
     * 「一边看游戏一边改」用。收起态与子层叠加规则：收起只画条，子层状态原样保留，展开即回原层）。 */
    ImGui::SetCursorScreenPos(ImVec2(x0, y0));
    text_meta_s("名字（脚本按名字认它；与别的操作重名会被拒）");
    {
        float bw = 150.0f;                       /* 头排两钮：预览 + 收起（窄屏也放得下） */
        ImGui::SetCursorScreenPos(ImVec2(x0 + cw - 2 * bw - 12, y0 - 8));
        if (btn_light("预览", ImVec2(bw, 64))) {
            g_ope_pv = 1;                        /* 全屏只读页（T2.5）；吞触摸矩形照旧整屏（ui_rect_now 只看 g_ope_i/g_ope_coll） */
            g_need = 1; g_force_frames = 2;
            ALOGI("op edit 预览开（%d 步）", g_ope_nsteps);
        }
        ImGui::SetCursorScreenPos(ImVec2(x0 + cw - bw, y0 - 8));
        if (btn_light("收起", ImVec2(bw, 64))) {
            g_ope_coll = 1;
            g_need = 1; g_force_frames = 2;
            ALOGI("op edit 收起（编辑状态保留；点条展开）");
        }
    }
    /* 名字行（点它开字符键盘子层） */
    ImGui::SetCursorScreenPos(ImVec2(x0, y0 + 68));
    if (btn_light(g_ope_name, ImVec2(cw, 84))) {
        g_ope_kb = 1; g_ope_kbmsg[0] = 0; g_ope_up = 0;
        g_need = 1; g_force_frames = 2;
        ALOGI("op edit 改名子层开 %s", g_ope_name);
    }
    /* 就地提示（[完成] 拒收 / 步数门 / 门控提示） */
    if (g_ope_msg[0]) {
        ImGui::SetCursorScreenPos(ImVec2(x0, y0 + 68 + 84 + 12));
        ImGui::TextColored(ImVec4(0.863f, 0.149f, 0.149f, 1.00f), "%s", g_ope_msg);
    }
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
    /* 加步（v8 十一类型，三行 4×3：点按/滑动/等待/找图 · 按下/弹起/跳转/找色 · 区域判断/开关判断/计算；
     * 第 12 格空。行数不变 ⇒ 上面的 5 行自适应阈值口径不动）/ 门控 / 自动关 / 收尾 */
    {
        float bw4 = (cw - 3 * 12) / 4.0f;
        float cx2 = x0 + bw4 + 12, cx3 = x0 + 2 * (bw4 + 12), cx4 = x0 + 3 * (bw4 + 12);
        ImGui::SetCursorScreenPos(ImVec2(x0, add_y1));
        if (btn_light("＋点按", ImVec2(bw4, bh_row))) ope_add_step(OP_STEP_TAP);
        ImGui::SetCursorScreenPos(ImVec2(cx2, add_y1));
        if (btn_light("＋滑动", ImVec2(bw4, bh_row))) ope_add_step(OP_STEP_SWIPE);
        ImGui::SetCursorScreenPos(ImVec2(cx3, add_y1));
        if (btn_light("＋等待", ImVec2(bw4, bh_row))) ope_add_step(OP_STEP_WAIT);
        ImGui::SetCursorScreenPos(ImVec2(cx4, add_y1));
        if (btn_light("＋找图", ImVec2(bw4, bh_row))) {   /* 加完立即开视觉参数层（选模板，同计算步口径） */
            int n0 = g_ope_nsteps;
            ope_add_step(OP_STEP_FINDIMAGE);
            if (g_ope_nsteps > n0) ope_vis_open(g_ope_nsteps - 1);
        }
        ImGui::SetCursorScreenPos(ImVec2(x0, add_y2));
        if (btn_light("＋按下", ImVec2(bw4, bh_row))) ope_add_step(OP_STEP_DOWN);
        ImGui::SetCursorScreenPos(ImVec2(cx2, add_y2));
        if (btn_light("＋弹起", ImVec2(bw4, bh_row))) ope_add_step(OP_STEP_UP);
        ImGui::SetCursorScreenPos(ImVec2(cx3, add_y2));
        if (btn_light("＋跳转", ImVec2(bw4, bh_row))) ope_add_step(OP_STEP_JUMP);
        ImGui::SetCursorScreenPos(ImVec2(cx4, add_y2));
        if (btn_light("＋找色", ImVec2(bw4, bh_row))) {
            int n0 = g_ope_nsteps;
            ope_add_step(OP_STEP_FINDCOLOR);
            if (g_ope_nsteps > n0) ope_vis_open(g_ope_nsteps - 1);
        }
        ImGui::SetCursorScreenPos(ImVec2(x0, add_y3));
        if (btn_light(cw < 1050.0f ? "区域判断" : "＋区域判断", ImVec2(bw4, bh_row))) ope_add_step(OP_STEP_COND_REGION);
        ImGui::SetCursorScreenPos(ImVec2(cx2, add_y3));
        if (btn_light(cw < 1050.0f ? "开关判断" : "＋开关判断", ImVec2(bw4, bh_row))) ope_add_step(OP_STEP_COND_TOGGLE);
        ImGui::SetCursorScreenPos(ImVec2(cx3, add_y3));
        if (btn_light("＋计算", ImVec2(bw4, bh_row))) {   /* 加完立即开表达式子层（spec §6；步满加不进则不弹） */
            int n0 = g_ope_nsteps;
            ope_add_step(OP_STEP_CALC);
            if (g_ope_nsteps > n0) ope_expr_open(g_ope_nsteps - 1);
        }
        /* 第 4 列第 3 行：空（11 键 = 4+4+3） */
    }
    {
        char g[72];
        snprintf(g, sizeof g, "门控开关：%s", g_ope_gate[0] ? g_ope_gate : "无");
        ImGui::SetCursorScreenPos(ImVec2(x0, gate_y));
        if (btn_light(g, ImVec2(cw, bh_row))) ope_gate_cycle();
        ImGui::SetCursorScreenPos(ImVec2(x0, autooff_y));
        if (btn_light(g_ope_autoff ? "跑完自动关：开" : "跑完自动关：关", ImVec2(cw, bh_row))) {
            g_ope_autoff = !g_ope_autoff;
            g_need = 1; g_force_frames = 2;
            ALOGI("op edit 自动关 %s", g_ope_autoff ? "开" : "关");
        }
    }
    {
        float bw2 = (cw - 12) * 0.5f;
        ImGui::SetCursorScreenPos(ImVec2(x0, done_y));
        if (btn_light("取消", ImVec2(bw2, bh_done))) {    /* 取消 = 丢本轮编辑回操作页（改名弹层同款语义） */
            ALOGI("op edit 取消（丢编辑）");
            op_edit_close();
        }
        ImGui::SetCursorScreenPos(ImVec2(x0 + bw2 + 12, done_y));
        if (btn_blue("完成", ImVec2(bw2, bh_done))) op_edit_save();
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

/* 说明页（v8 视觉）：18 条术语 —— 1–15 承 v4 §12、16 为 v5 新增；第 12 条更新 + 17/18 新增逐字照
 * docs/VISION_PLAN.md §12（面板文案唯一来源）。数组内容从 spec 机器提取（条目为单行、无加粗标记，
 * 按行原样），提取结果逐字节复核 —— 改文案先改 spec、再按同一规则重提，别手改这里。 */
static const char *const g_help_lines[] = {
    "1. 触发：给区域绑一条操作；手指碰到这个区域就会跑那条操作。",
    "2. 时机·按下：手指碰到区域的那一刻就跑。",
    "3. 时机·完整按压：手指碰到、抬起后跑（按一下、抬起来，才算数）。",
    "4. 开关型：把区域当开关——每完整按压一次，「开/关」翻转一次（绿色 = 开）。",
    "5. 门控：操作的门禁——绑一个开关型区域，它开着，操作才允许跑。",
    "6. 跑完自动关：操作正常跑完后，自动把门控开关翻回「关」。",
    "7. 取点：点 [取点]，然后去屏幕上点一下——那个坐标对（x 和 y）一次填进正在编辑的位置，并在屏上标一下。",
    "8. 变量（触发数据）：这次触发的那根手指——触发按下x / 触发按下y（按下位置）、触发弹起x / 触发弹起y（抬起位置）、触发时长（按下到抬起的毫秒数）。「完整按压」触发时全都有； 「按下」触发只有按下位置；面板手动运行没有。",
    "9. 按下 / 弹起：两条分开的步骤——按下 = 按住不放；弹起 = 松开。中间可以夹「等待」「判断」。",
    "10. 区域判断：检查一个点在不在某个区域内；成立 / 不成立两侧各选接下来做什么：继续下一步、跳过下一步、跳到第 N 步、中止（可以只配一侧，另一侧走默认）。",
    "11. 开关判断：检查某个开关型区域现在是不是「开」；成立 / 不成立两侧的选项同「区域判断」。",
    "12. 中止原因速查：变量无值 / 结果无值 / 表达式错 / 槽占用 / 未按下 / 区域不存在 / 非开关型 / 条件不成立 / 条件中止 / 未命中 / 无画面 / 模板不存在 / 视觉错 / 跳转超限。",
    "13. 跳转：直接跳到指定步骤继续——往前跳 = 跳过中间步骤；往后跳 = 循环（比如跳回第 1 步重来）。目标也可以选「结束」直接完成操作；单次运行跳转超过 200 次会自动中止（防死循环）。",
    "14. 方案：把当前的区域和操作整体存成一个命名方案；切换方案 = 换成那一套（编辑会自动存回当前方案）。",
    "15. 方案管理：「方案」页可以新建（空白）、从当前另存为、重命名、删除；当前方案不能删（先切到别的方案再删）。",
    "16. 计算：算一个数存进结果槽（r1–r4）——用触发数据（tdx/tdy=按下坐标、tux/tuy=弹起坐标、tms=按压时长毫秒）、数字和结果槽做加减乘除，也能用 atan2、sin、cos、abs、min、max、sqrt（三角函数按度）。算好的槽可以当坐标、时长用在后面的步骤里。",
    "17. 找图：先存好模板（「模板」页截屏框选），步骤里选模板名——在当前画面里找这块图案（可以限定区域）；找到就把坐标填进结果槽 r1（x）、r2（y），走「成立」档；没找到走「不成立」档。",
    "18. 找色：按颜色找像素——填颜色（十六进制）和容差（0–255）；「多点找色」还要选一个点集（基准色 + 参考点，在「模板」页吸色点选生成）。找到同样写 r1/r2 走「成立」档，没找到走「不成立」档。",
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
                           g_name_msg, (int)sizeof g_name_msg, (float)TITLE_H + 10);   /* 面板窗：sheet 顶在标题栏下 */
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

/* ---- 方案页（v4 T2.1）：列表 + 切换 / 新建（空）/ 从当前另存为 / 重命名 / 删除 ----------------------
 * 布局照 spec §6：页头（当前 / 选中）+ 提示槽（固定占位）+ 列表（每行方案名 + [切换]；当前行高亮、
 * 按钮禁用；可滚 + SCR_LIST 拖滚）+ 底部四键；子层 = 名字键盘（draw_char_kb，ASCII）/ 删除确认
 * （小确认层）。接线（spec §5；执行器 = T1.2 的 scheme_switch，返回码见其段首注释）：
 *   切换 = scheme_switch，码表映射就地提示（9/10 是「已回滚」语义，提示里写明）；
 *   新建 = 建目录 + 两空合法文件（不自动切）；另存为 = live 两文件 → schemes/<名>/ + current 改新名；
 *   重命名 = 目录 rename +（current == 旧名时）current 同步，同步失败回滚目录名；
 *   删除 = 拒当前（提示）、确认后删目录。
 * 日志逐字 spec §8：`方案 新建 <名>` / `方案 另存为 <名>` / `方案 改名 <旧> → <新>` / `方案 删除 <名>`
 * （切换的成功日志在 scheme_switch 里；本页另加失败路径 `方案 …失败 …` 诊断词，成功路径零新增）。 */

static char g_scm_msg[128] = {0};       /* 页面提示槽（切换结果 / 拒收 / 操作成败；固定占位） */
static char g_scm_sel[16] = {0};        /* 列表选中行（重命名 / 删除的作用对象；空 = 未选） */
static int  g_scm_kb = 0;               /* 名字键盘子层：0=关 1=新建 2=另存为 3=重命名 */
static char g_scm_kbmsg[72] = {0};      /* 名字键盘子层里的拒收提示 */
static int  g_scm_kbup = 0;             /* 名字键盘子层大小写档 */
static char g_scm_name[16] = {0};       /* 名字键盘子层输入缓冲 */
static char g_scm_ren_old[16] = {0};    /* 重命名子层的旧名（键盘「原名」对照） */
static char g_scm_del[16] = {0};        /* 删除确认层：待删方案名（空 = 层关） */

/* 切换返回码 → 就地提示（码表 = scheme_switch 段首注释；9/10 写明「已回滚」防误读）。 */
static const char *scm_switch_why(int rc)
{
    switch (rc) {
    case 1:  return "切换失败：名字非法";
    case 2:  return "切换失败：方案目录或文件缺失";
    case 3:  return "切换失败：区域表版本不符";
    case 4:  return "切换失败：区域表有坏行";
    case 5:  return "切换失败：操作表版本不符";
    case 6:  return "切换失败：操作表有坏行";
    case 7:  return "切换失败：操作表某条步数超限";
    case 8:  return "切换失败：写区域文件失败（未切换，原状）";
    case 9:  return "切换失败：写操作文件失败（已回滚，未切换）";
    case 10: return "切换失败：写 current 失败（已整体回滚，未切换）";
    default: return "切换失败";
    }
}
/* CRUD 失败码 → 就地提示（新建 / 另存为 / 重命名 共用一套码，含义见各 helper 注释）。 */
static const char *scm_crud_why(int rc)
{
    switch (rc) {
    case 1: return "名字非法：只能 a-z A-Z 0-9 _ -（1..15 字，裸 - 不行）";
    case 2: return "已有同名方案";
    case 3: return "方案目录创建失败";
    case 4: return "方案文件写入失败";
    case 5: return "current 写入失败（已撤销，未另存）";
    case 6: return "方案不存在（可能已被删/改名）";
    case 7: return "目录改名失败";
    case 8: return "current 同步失败（目录名已回滚）";
    default: return "操作失败";
    }
}
/* 切换（列表 [切换] 键）：调 T1.2 执行器（预检 / 静默边界 / flush+写 live / 清表重放 / current /
 * 刷新都在里面）；失败只在这里映射提示 + 诊断日志（执行器对预检拒切不打日志 —— 返回值就是接口）。 */
static void scm_switch_to(const char *name)
{
    int rc = scheme_switch(name);
    if (rc == 0) {
        snprintf(g_scm_msg, sizeof g_scm_msg, "已切换到 %s", name);
    } else {
        snprintf(g_scm_msg, sizeof g_scm_msg, "%s", scm_switch_why(rc));
        ALOGW("方案 切换失败 %s rc=%d", name, rc);
    }
    g_need = 1; g_force_frames = 3;
}
/* 删方案目录（spec §5「确认后删目录」）：先删两文件（含 .tmp 残件）再 rmdir。0 / -1（errno 保留）。 */
static int scm_delete_dir(const char *name)
{
    char p[160];
    snprintf(p, sizeof p, "%s/%s/regions.conf", SCHEME_DIR, name); remove(p);
    snprintf(p, sizeof p, "%s/%s/regions.conf.tmp", SCHEME_DIR, name); remove(p);
    snprintf(p, sizeof p, "%s/%s/ops.conf", SCHEME_DIR, name); remove(p);
    snprintf(p, sizeof p, "%s/%s/ops.conf.tmp", SCHEME_DIR, name); remove(p);
    snprintf(p, sizeof p, "%s/%s", SCHEME_DIR, name);
    if (rmdir(p) != 0) return -1;
    return 0;
}
/* 新建（空）（spec §5）：建目录 + 两空合法文件；不自动切换；日志 `方案 新建 <名>`。返回 0 / 1 名字非法
 * / 2 撞名 / 3 建目录失败 / 4 写空文件失败（4 时清掉半建目录，重试不会撞名）。 */
static int scm_new_empty(const char *name)
{
    char path[160];
    if (scheme_name_ok(name) != 0) return 1;
    if (scheme_exists(name)) return 2;
    region_conf_dir();
    if (mkdir(SCHEME_DIR, 0775) < 0 && errno != EEXIST) return 3;
    snprintf(path, sizeof path, "%s/%s", SCHEME_DIR, name);
    if (mkdir(path, 0775) < 0) return 3;
    snprintf(path, sizeof path, "%s/%s/regions.conf", SCHEME_DIR, name);
    if (scheme_write_empty(path, 0) == 0) {
        snprintf(path, sizeof path, "%s/%s/ops.conf", SCHEME_DIR, name);
        if (scheme_write_empty(path, 1) == 0) {
            ALOGI("方案 新建 %s", name);
            return 0;
        }
    }
    scm_delete_dir(name);       /* 半建目录清掉：留半份会让下次重试撞名 */
    return 4;
}
/* 从当前另存为（spec §5）：live 两文件复制进 schemes/<名>/ → current 改新名（内容相同，无需动核心）；
 * 日志 `方案 另存为 <名>`。返回 0 / 1 名字非法 / 2 撞名 / 3 建目录失败 / 4 复制失败 / 5 current 写失败
 * （4/5 清掉新目录 = 撤销，重试不会撞名）。 */
static int scm_save_as(const char *name)
{
    char path[160];
    if (scheme_name_ok(name) != 0) return 1;
    if (scheme_exists(name)) return 2;
    region_conf_dir();
    if (mkdir(SCHEME_DIR, 0775) < 0 && errno != EEXIST) return 3;
    snprintf(path, sizeof path, "%s/%s", SCHEME_DIR, name);
    if (mkdir(path, 0775) < 0) return 3;
    snprintf(path, sizeof path, "%s/%s/regions.conf", SCHEME_DIR, name);
    if (scheme_copy_file(REGION_CONF_NEW, path, 0) != 0) { scm_delete_dir(name); return 4; }
    snprintf(path, sizeof path, "%s/%s/ops.conf", SCHEME_DIR, name);
    if (scheme_copy_file(OPS_CONF_FILE, path, 1) != 0) { scm_delete_dir(name); return 4; }
    if (scheme_cur_set(name) != 0) { scm_delete_dir(name); return 5; }
    ALOGI("方案 另存为 %s", name);
    return 0;
}
/* 重命名（spec §5）：目录 rename；若 current == 旧名 → current 写新名（写失败回滚目录名，保
 * live == schemes/<current> 不变量）；日志 `方案 改名 <旧> → <新>`。返回 0 / 1 名字非法 / 2 撞名 /
 * 6 旧方案不存在 / 7 目录改名失败 / 8 current 同步失败（目录名已回滚）。 */
static int scm_rename(const char *oldn, const char *newn)
{
    char po[160], pn[160], cur[16];
    if (scheme_name_ok(newn) != 0) return 1;
    if (!strcmp(oldn, newn)) return 0;          /* 同名 = 无操作（键盘 [确定] 直接收层） */
    if (scheme_exists(newn)) return 2;
    if (!scheme_exists(oldn)) return 6;
    snprintf(po, sizeof po, "%s/%s", SCHEME_DIR, oldn);
    snprintf(pn, sizeof pn, "%s/%s", SCHEME_DIR, newn);
    if (rename(po, pn) != 0) return 7;
    if (scheme_cur_get(cur) == 0 && !strcmp(cur, oldn)) {
        if (scheme_cur_set(newn) != 0) {
            if (rename(pn, po) != 0)
                ALOGW("方案 改名 %s → %s：current 同步失败且目录名回滚失败: %s", oldn, newn, strerror(errno));
            return 8;
        }
    }
    ALOGI("方案 改名 %s → %s", oldn, newn);
    return 0;
}
/* 打开名字键盘子层（1=新建 2=另存为 3=重命名）；重命名预填旧名（键盘「原名」对照 + 可直接改）。 */
static void scm_kb_open(int mode)
{
    g_scm_kb = mode;
    g_scm_kbmsg[0] = 0;
    g_scm_kbup = 0;
    g_scm_name[0] = 0;
    g_scm_ren_old[0] = 0;
    if (mode == 3) {
        snprintf(g_scm_name, sizeof g_scm_name, "%s", g_scm_sel);
        snprintf(g_scm_ren_old, sizeof g_scm_ren_old, "%s", g_scm_sel);
    }
    g_scm_msg[0] = 0;
    g_need = 1; g_force_frames = 3;
}
/* 点名字 = 选中（重命名 / 删除的作用对象）；换选中清旧提示。 */
static void scm_select(const char *name)
{
    snprintf(g_scm_sel, sizeof g_scm_sel, "%s", name);
    g_scm_msg[0] = 0;
    g_need = 1; g_force_frames = 2;
}
/* 底部 [重命名]：无选中 / 选中已失效 → 就地提示；否则开名字键盘（预填旧名）。 */
static void scm_act_rename(void)
{
    if (!g_scm_sel[0]) {
        snprintf(g_scm_msg, sizeof g_scm_msg, "先点一行名字选中要操作的方案");
        g_need = 1; g_force_frames = 2;
        return;
    }
    if (!scheme_exists(g_scm_sel)) {
        snprintf(g_scm_msg, sizeof g_scm_msg, "方案不存在（可能已被删/改名）：%s", g_scm_sel);
        g_scm_sel[0] = 0;
        g_need = 1; g_force_frames = 2;
        return;
    }
    scm_kb_open(3);
}
/* 底部 [删除]：无选中 / 失效 → 提示；选中 = 当前 → 拒（spec §5：不允许删除当前）；否则开确认层。 */
static void scm_act_delete(void)
{
    char cur[16];
    if (!g_scm_sel[0]) {
        snprintf(g_scm_msg, sizeof g_scm_msg, "先点一行名字选中要操作的方案");
        g_need = 1; g_force_frames = 2;
        return;
    }
    if (!scheme_exists(g_scm_sel)) {
        snprintf(g_scm_msg, sizeof g_scm_msg, "方案不存在（可能已被删/改名）：%s", g_scm_sel);
        g_scm_sel[0] = 0;
        g_need = 1; g_force_frames = 2;
        return;
    }
    scheme_cur_get(cur);
    if (cur[0] && !strcmp(cur, g_scm_sel)) {
        snprintf(g_scm_msg, sizeof g_scm_msg, "当前方案不能删除（先切到别的方案再删）");
        g_need = 1; g_force_frames = 2;
        return;
    }
    snprintf(g_scm_del, sizeof g_scm_del, "%s", g_scm_sel);
    g_scm_msg[0] = 0;
    g_need = 1; g_force_frames = 3;
}
/* 一行方案：名字键（点 = 选中）+ [切换]（当前行禁用）；当前行高亮（浅蓝底 + 蓝边，同操作卡运行态口径）。 */
static void scm_row(const char *name, const char *cur)
{
    int is_cur = cur[0] && !strcmp(name, cur);
    int is_sel = g_scm_sel[0] && !strcmp(name, g_scm_sel);
    ImGui::PushID(name);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, is_cur ? ImVec4(0.937f, 0.965f, 1.00f, 1.00f) : WHITE);
    ImGui::PushStyleColor(ImGuiCol_Border, is_cur ? BLUE500 : (is_sel ? BLUE500 : ZINC200));
    ImGui::BeginChild("row", ImVec2(0, 0), ImGuiChildFlags_Border | ImGuiChildFlags_AutoResizeY,
                      ImGuiWindowFlags_NoScrollbar);
    {
        float gap = 12.0f, bw = 150.0f;
        float nw = ImGui::GetContentRegionAvail().x - bw - gap;
        if (nw < 120) nw = 120;
        if (is_sel) { if (btn_blue(name, ImVec2(nw, 84))) scm_select(name); }
        else        { if (btn_light(name, ImVec2(nw, 84))) scm_select(name); }
        ImGui::SameLine();
        if (is_cur) {                       /* 当前行：按钮禁用（spec §6） */
            ImGui::BeginDisabled();
            btn_blue("切换", ImVec2(bw, 84));
            ImGui::EndDisabled();
        } else if (btn_blue("切换", ImVec2(bw, 84))) {
            scm_switch_to(name);
        }
        if (is_cur || is_sel) {
            char m[32];
            if (is_cur && is_sel) snprintf(m, sizeof m, "当前方案 · 已选中");
            else if (is_cur)      snprintf(m, sizeof m, "当前方案");
            else                  snprintf(m, sizeof m, "已选中");
            text_meta_s(m);
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor(2);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(12, 12));
    ImGui::Dummy(ImVec2(0, 0));   /* bento 卡片间隙 */
    ImGui::PopStyleVar();
    ImGui::PopID();
}
/* 「方案」页（spec §6）：页头（当前 / 选中）+ 提示槽（固定占位，同编辑层口径）+ 列表 + 底部四键。 */
static void page_scheme(void)
{
    char cur[16] = {0}, names[SCHEME_LIST_MAX][16], meta[40], l[64];
    int n, i;
    scheme_cur_get(cur);
    n = scheme_list(names, SCHEME_LIST_MAX);
    snprintf(meta, sizeof meta, "%d 个", n);
    page_header("方案", meta);
    snprintf(l, sizeof l, "当前：%s · 选中：%s", cur[0] ? cur : "—", g_scm_sel[0] ? g_scm_sel : "无");
    text_meta_s(l);
    ImGui::Dummy(ImVec2(0, 4));
    /* 提示槽固定占位：出现提示时下面整块不动，防误点（两行 30px 小字内放得下） */
    ImGui::BeginChild("##scmmsg", ImVec2(0, 96), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
    if (g_scm_msg[0]) {
        meta_push();
        ImGui::PushStyleColor(ImGuiCol_Text, RED600);
        ImGui::TextWrapped("%s", g_scm_msg);
        ImGui::PopStyleColor();
        meta_pop();
    }
    ImGui::EndChild();
    ImGui::Dummy(ImVec2(0, 4));
    /* 列表自成一格可滚容器（与区域 / 操作列表同款；拖动滚动目标同走 SCR_LIST） */
    {
        float list_h = ImGui::GetContentRegionAvail().y - (84.0f * 2 + 12.0f + 12.0f);
        if (list_h < 0) list_h = 0;   /* 矮屏：列表让位（底部四键优先可达；正常高屏不受影响） */
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ZINC50);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 12));
        ImGui::BeginChild("##schemes", ImVec2(0, list_h), ImGuiChildFlags_None,
                          ImGuiWindowFlags_AlwaysVerticalScrollbar);
        pub_zone(g_zone_list);
        drag_scroll_for(SCR_LIST);
        if (n == 0) ImGui::TextDisabled("还没有方案");
        for (i = 0; i < n; i++) scm_row(names[i], cur);
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
    }
    ImGui::Dummy(ImVec2(0, 6));
    /* 底部四键（2x2）：新建（空） / 从当前另存为 / 重命名 / 删除 */
    {
        float gap = 12.0f;
        float bw = (ImGui::GetContentRegionAvail().x - gap) * 0.5f;
        if (btn_blue("新建(空)", ImVec2(bw, 84))) scm_kb_open(1);
        ImGui::SameLine();
        if (btn_light("从当前另存为", ImVec2(bw, 84))) scm_kb_open(2);
        if (btn_light("重命名", ImVec2(bw, 84))) scm_act_rename();
        ImGui::SameLine();
        if (btn_red("删除", ImVec2(bw, 84))) scm_act_delete();
    }
}
/* 名字键盘子层（新建 / 另存为 / 重命名 共用；整面盖住面板 —— 本帧不画侧栏/内容页）：
 * [确定] 走各自语义；非法 / 撞名就地拒收（子层不关）；成功关层 + 页面提示 + 日志（spec §8）。 */
static void draw_scm_kb(void)
{
    const char *title = g_scm_kb == 1 ? "给新方案起个名字：只建空表，不切换当前（重名会被拒）"
                      : g_scm_kb == 2 ? "从当前另存为：把当前内容存成新方案并切过去（重名会被拒）"
                                      : "重命名方案：起个新名字（存储按名字认它）";
    int act = draw_char_kb(title, g_scm_kb == 3 ? g_scm_ren_old : NULL,
                           g_scm_name, (int)sizeof g_scm_name, &g_scm_kbup,
                           g_scm_kbmsg, (int)sizeof g_scm_kbmsg, (float)TITLE_H + 10);
    if (act == 1) {
        g_scm_kb = 0; g_scm_kbmsg[0] = 0;
        g_need = 1; g_force_frames = 3;
    } else if (act == 2) {
        int rc = g_scm_kb == 1 ? scm_new_empty(g_scm_name)
               : g_scm_kb == 2 ? scm_save_as(g_scm_name)
                               : scm_rename(g_scm_ren_old, g_scm_name);
        if (rc == 0) {
            if (g_scm_kb == 1) snprintf(g_scm_msg, sizeof g_scm_msg, "已新建方案 %s", g_scm_name);
            else if (g_scm_kb == 2) snprintf(g_scm_msg, sizeof g_scm_msg, "已另存为 %s（已切到新方案）", g_scm_name);
            else {
                if (strcmp(g_scm_ren_old, g_scm_name))
                    snprintf(g_scm_msg, sizeof g_scm_msg, "已改名 %s → %s", g_scm_ren_old, g_scm_name);
                else g_scm_msg[0] = 0;                       /* 同名 = 无操作：不提示 */
                snprintf(g_scm_sel, sizeof g_scm_sel, "%s", g_scm_name);   /* 选中跟着新名 */
            }
            g_scm_kb = 0; g_scm_kbmsg[0] = 0;
            g_need = 1; g_force_frames = 3;
        } else {
            snprintf(g_scm_kbmsg, sizeof g_scm_kbmsg, "%s", scm_crud_why(rc));
            if (g_scm_kb == 1) ALOGW("方案 新建失败 %s: %s", g_scm_name, scm_crud_why(rc));
            else if (g_scm_kb == 2) ALOGW("方案 另存为失败 %s: %s", g_scm_name, scm_crud_why(rc));
            else ALOGW("方案 改名失败 %s → %s: %s", g_scm_ren_old, g_scm_name, scm_crud_why(rc));
            g_need = 1; g_force_frames = 2;
        }
    }
}
/* 删除确认层（spec §6）：轻遮罩 + 小确认卡，文案逐字「删除方案 <名>？不可恢复」；[取消][删除]。
 * 确认时再核一次「不是当前」（防御；主拒在 [删除] 键入口）：成功才打日志 `方案 删除 <名>` + 提示；
 * 失败关层 + 页面提示（rmdir 失败无「可改的输入」，留层重试无意义）。 */
static void draw_scm_del(void)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 wp = ImGui::GetWindowPos();
    float ww = ImGui::GetWindowWidth(), wh = ImGui::GetWindowHeight();
    ImVec2 a(wp.x + 12, wp.y + (float)TITLE_H + 10), b(wp.x + ww - 12, wp.y + wh - 12);
    char t[64];
    float cw, wrapw, ch, cx, cy;
    ImVec2 tsz, ca, cb;
    dl->AddRectFilled(a, b, IM_COL32(24, 24, 27, 110), 14);   /* 轻遮罩：整面盖住（本帧不画底下） */
    snprintf(t, sizeof t, "删除方案 %s？不可恢复", g_scm_del);
    cw = 640.0f;
    if (cw > (b.x - a.x) - 48) cw = (b.x - a.x) - 48;
    wrapw = cw - 56;
    tsz = ImGui::CalcTextSize(t, NULL, false, wrapw);
    ch = tsz.y + 40 + 92 + 32;
    cx = (a.x + b.x) * 0.5f; cy = (a.y + b.y) * 0.5f;
    ca = ImVec2(cx - cw * 0.5f, cy - ch * 0.5f); cb = ImVec2(cx + cw * 0.5f, cy + ch * 0.5f);
    dl->AddRectFilled(ca, cb, IM_COL32(255, 255, 255, 253), 14);
    dl->AddRect(ca, cb, IM_COL32(228, 228, 231, 255), 14, 0, 1.5f);
    ImGui::SetCursorScreenPos(ImVec2(ca.x + 28, ca.y + 26));
    ImGui::PushTextWrapPos(ca.x + 28 + wrapw);
    ImGui::TextUnformatted(t);
    ImGui::PopTextWrapPos();
    {
        float gap = 12.0f, bw = (cw - 28 * 2 - gap) * 0.5f;
        float by = cb.y - 26 - 92;
        ImGui::SetCursorScreenPos(ImVec2(ca.x + 28, by));
        if (btn_light("取消", ImVec2(bw, 92))) {
            g_scm_del[0] = 0;
            g_need = 1; g_force_frames = 3;
        }
        ImGui::SetCursorScreenPos(ImVec2(ca.x + 28 + bw + gap, by));
        if (btn_red("删除", ImVec2(bw, 92))) {
            char cur[16];
            scheme_cur_get(cur);
            if (cur[0] && !strcmp(cur, g_scm_del)) {       /* 防御：主拒在 [删除] 键入口 */
                snprintf(g_scm_msg, sizeof g_scm_msg, "当前方案不能删除（先切到别的方案再删）");
            } else if (scm_delete_dir(g_scm_del) != 0) {
                snprintf(g_scm_msg, sizeof g_scm_msg, "删除失败：%s", strerror(errno));
                ALOGW("方案 删除失败 %s: %s", g_scm_del, strerror(errno));
            } else {
                ALOGI("方案 删除 %s", g_scm_del);
                if (!strcmp(g_scm_sel, g_scm_del)) g_scm_sel[0] = 0;
                snprintf(g_scm_msg, sizeof g_scm_msg, "已删除方案 %s", g_scm_del);
            }
            g_scm_del[0] = 0;
            g_need = 1; g_force_frames = 3;
        }
    }
}

/* ==== T3.2：视觉（找图/找色）—— 模板/点集管理 + 视觉步参数层 + 采集覆盖层 ====================
 * 三块：
 *   1) 模板页（nav 6）：截帧 → 框选/点选 → 命名 → 存 .tmpl / .pts；列表 / 删除。
 *   2) 采集覆盖层（整屏）：面板侧抓帧（ui_glue.c 旁路；不动核心帧区请求协议）→ ImGui 纹理显示
 *      （等比缩放）→ 模板 = 拖框；点集 = 吸基准色 + 点参考点（显示偏移/色）；吸色 = 点一下回填找色步。
 *   3) 视觉步参数层（编辑层子层，g_vis_ed）：找图 = 模板 / 区域 / 阈值；找色 = 模式 / 颜色 / 取点吸色 /
 *      容差 / 点集；成立/不成立四档在步骤行上复用条件步控件（ope_cond_side）；数字/字符键盘复用现有子层。
 * 文件格式（写端；逐字照实施计划「模板/点集文件格式」，与核心读端 op_vis_read_tmpl / op_vis_read_pts 对账）：
 *   .tmpl = "VTM1" + ver u32=1 + w u16 + h u16 + rot u8 + res u8=0 + gray[w*h]（灰度 = vt_vis_gray_px 单一来源）；
 *   .pts  = "VTP1" + ver u32=1 + n u16 + res u16=0 + base_rgb u32 + base_tol u16 + n×{dx i16, dy i16, rgb u32, tol u16}；
 *   目录 = /data/local/vtouch-runtime/templates/；名字同 vt_id_ok 尺子；重名拒绝（.tmpl / .pts 共用一个名字空间）。
 * 线程：本区块全部跑渲染线程（唯一 UI 线程）；帧数据跨线程只经 ui_glue.c 的 take/err（acquire 读）。
 */

#define VIS_TMPL_DIR REGION_CONF_DIR "/templates"
#define VIS_CAP_TMO_MS 3000              /* 面板侧抓帧等待上限（超时提示；真机抓帧 ≤25ms） */

/* 模板 / 点集名合法性（与区域 id / 操作名同一把尺子 vt_id_ok）：[A-Za-z0-9_-]、1..15、裸 `-` 除外。
 * 0 ok / 1 空 / 2 超长 / 3 非法字符。 */
static int vis_name_ok(const char *n)
{
    int i, len = (int)strlen(n);
    if (len < 1) return 1;
    if (len > 15) return 2;
    if (len == 1 && n[0] == '-') return 3;
    for (i = 0; i < len; i++) {
        char ch = n[i];
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') || ch == '_' || ch == '-')) return 3;
    }
    return 0;
}
static const char *vis_name_why(int rc)
{
    return rc == 1 ? "名字不能为空" : rc == 2 ? "最多 15 个字符" :
           rc == 3 ? "只能用 a-z A-Z 0-9 _ -" : "这个名字已被占用（模板/点集同名也不行）";
}
/* 名字是否已被占用（.tmpl / .pts 共用一个名字空间 —— 下拉里两个列表并存，同名会歧义）。 */
static int vis_name_taken(const char *name)
{
    char p[160];
    struct stat st;
    snprintf(p, sizeof p, "%s/%s.tmpl", VIS_TMPL_DIR, name);
    if (stat(p, &st) == 0) return 1;
    snprintf(p, sizeof p, "%s/%s.pts", VIS_TMPL_DIR, name);
    return stat(p, &st) == 0;
}
/* qsort 比较器：名字按字节序（names 每格 16 字节）。 */
static int vis_name_cmp(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}
/* 枚举 VIS_TMPL_DIR 下 *.ext（名字过 vis_name_ok），字母序装进 names（每格 16 字节），返回条数（≤ max）。 */
static int vis_list(const char *ext, char (*names)[16], int max)
{
    DIR *d = opendir(VIS_TMPL_DIR);
    struct dirent *e;
    size_t el;
    int n = 0;
    if (!d) return 0;
    el = strlen(ext);
    while (n < max && (e = readdir(d)) != NULL) {
        char nm[16];
        size_t l = strlen(e->d_name);
        if (l <= el || l > el + 15) continue;
        if (strcmp(e->d_name + l - el, ext) != 0) continue;
        memcpy(nm, e->d_name, l - el);
        nm[l - el] = 0;
        if (vis_name_ok(nm) != 0) continue;
        snprintf(names[n], 16, "%s", nm);
        n++;
    }
    closedir(d);
    qsort(names, (size_t)n, 16, vis_name_cmp);
    return n;
}
/* 小端写入（格式契约；设备 aarch64 本身小端，仍显式写字节防解释歧义）。 */
static void vis_put_u16le(FILE *f, unsigned v)
{
    fputc((int)(v & 0xFFu), f);
    fputc((int)((v >> 8) & 0xFFu), f);
}
static void vis_put_u32le(FILE *f, unsigned v)
{
    vis_put_u16le(f, v & 0xFFFFu);
    vis_put_u16le(f, (v >> 16) & 0xFFFFu);
}

/* ---- 面板侧抓帧（采集覆盖层用） ---- */

/* 关采集覆盖层：全部会话态复位（含命名子层 / 框选 / 点集 / 吸色目标）。 */
static void vis_cap_close(void)
{
    g_vis_cap = 0;
    g_vis_cap_wait = 0; g_vis_cap_err = 0;
    g_vis_kb = 0; g_vis_kb_msg[0] = 0;
    g_vis_drag = 0; g_vis_gest = 0; g_vis_sel_on = 0;
    g_vis_zoom = 1.0f; g_vis_pan_x = 0; g_vis_pan_y = 0;
    g_vis_base_x = -1; g_vis_base_y = -1; g_vis_base_rgb = 0; g_vis_base_tol = 8;
    g_vis_pts_n = 0;
    g_vis_cap_msg[0] = 0;
    g_vis_pick_se = -1;
    g_need = 1; g_force_frames = 3;
}
/* 开采集覆盖层：mode 1=模板框选 2=点集编辑 3=吸色（找色步 [取点]，回填目标 = g_vis_pick_se）。
 * 请求一帧（ui_glue.c 旁路）→ 渲染循环 vis_cap_tick 收帧/超时。 */
static void vis_cap_start(int mode)
{
    g_vis_cap = mode;
    g_vis_cap_wait = 1; g_vis_cap_err = 0; g_vis_cap_t0 = now_ms();
    g_vis_kb = 0; g_vis_kb_buf[0] = 0; g_vis_kb_msg[0] = 0; g_vis_kb_up = 0;
    g_vis_drag = 0; g_vis_gest = 0; g_vis_sel_on = 0;
    g_vis_zoom = 1.0f; g_vis_pan_x = 0; g_vis_pan_y = 0;
    g_vis_base_x = -1; g_vis_base_y = -1; g_vis_base_rgb = 0; g_vis_base_tol = 8;
    g_vis_pts_n = 0;
    g_vis_cap_msg[0] = 0;
    g_vis_img = 0; g_vis_img_w = 0; g_vis_img_h = 0; g_vis_img_rot = 0;
    vtouch_vis_panel_capture_req();
    g_need = 1; g_force_frames = 3;
    ALOGI("vis 采集开 模式=%s（请求面板侧抓帧）", mode == 1 ? "模板" : mode == 2 ? "点集" : "吸色");
}
/* 渲染循环每拍调（采集层开着才做事）：收新帧 / 收失败 / 3s 超时；等帧期间保持重画。
 * 非等待期也收帧：迟到的帧（重截竞态）照样刷新图像。 */
static void vis_cap_tick(void)
{
    int w = 0, h = 0, rot = 0, err = 0;
    const unsigned char *buf = 0;
    if (!g_vis_cap) return;
    if (vtouch_vis_panel_frame_take(&w, &h, &rot, &buf)) {
        g_vis_img = buf; g_vis_img_w = w; g_vis_img_h = h; g_vis_img_rot = rot;
        g_vis_cap_wait = 0; g_vis_cap_err = 0;
        g_vis_tex_dirty = 1;
        if (g_vis_sel_on) {   /* 新帧尺寸可能变（转屏后重新截帧）：已选框夹回图内（Task 7.2：框保留） */
            if (g_vis_sel[2] > g_vis_img_w - 1) g_vis_sel[2] = g_vis_img_w - 1;
            if (g_vis_sel[3] > g_vis_img_h - 1) g_vis_sel[3] = g_vis_img_h - 1;
            if (g_vis_sel[0] > g_vis_sel[2] - 7) g_vis_sel[0] = g_vis_sel[2] - 7;
            if (g_vis_sel[1] > g_vis_sel[3] - 7) g_vis_sel[1] = g_vis_sel[3] - 7;
            if (g_vis_sel[0] < 0) g_vis_sel[0] = 0;
            if (g_vis_sel[1] < 0) g_vis_sel[1] = 0;
        }
        ALOGI("vis 采集 帧就绪 %dx%d rot=%d", w, h, rot);
        g_need = 1; g_force_frames = 3;
        return;
    }
    if (g_vis_cap_wait) {
        if (vtouch_vis_panel_err_take(&err)) {
            g_vis_cap_wait = 0; g_vis_cap_err = err ? err : -1;
            ALOGW("vis 采集 抓帧失败 err=%d", g_vis_cap_err);
        } else if (now_ms() - g_vis_cap_t0 > VIS_CAP_TMO_MS) {
            g_vis_cap_wait = 0; g_vis_cap_err = -1;             /* -1 = 超时哨兵 */
            ALOGW("vis 采集 抓帧超时（%dms）", VIS_CAP_TMO_MS);
        }
        g_need = 1;                                             /* 等帧/提示期间保持重画 */
    }
}

/* ---- 试查（Task 7.1「试一下」；整条链在渲染线程）---- */

/* 结果码 → 原因词（词表 = 核心 op 中止词；码值 = 核心 src/vt_shm.h 的 VT_TEST_ERR_* ——
 * 面板不 include 核心头，独立定义）。 */
#define VIS_TEST_ERR_NOPIC  1            /* 无画面 */
#define VIS_TEST_ERR_VIS    2            /* 视觉错 */
#define VIS_TEST_ERR_REGION 3            /* 区域不存在 */
#define VIS_TEST_ERR_TMPL   4            /* 模板不存在 */
static const char *vis_test_err_word(int err)
{
    switch (err) {
    case VIS_TEST_ERR_NOPIC:  return "无画面";
    case VIS_TEST_ERR_VIS:    return "视觉错";
    case VIS_TEST_ERR_REGION: return "区域不存在";
    case VIS_TEST_ERR_TMPL:   return "模板不存在";
    default:                  return "未知错误";
    }
}

/* 结果行颜色：命中绿 / 未命中灰 / 等待灰 / 错误与超时红。 */
static ImVec4 vis_test_msg_col(void)
{
    if (g_vis_test_on || g_vis_test_err == -1) return ImVec4(0.45f, 0.45f, 0.48f, 1.0f);
    if (g_vis_test_err == 0) return ImVec4(0.09f, 0.64f, 0.29f, 1.0f);
    return ImVec4(0.863f, 0.149f, 0.149f, 1.00f);   /* 同 g_vis_edmsg 红 */
}

/* 读 .tmpl 头（w/h/rot；找图标记方框尺寸用）。0 = 成功；非 0 = 读不到（方框按 80×80 兜底）。 */
static int vis_test_read_tmpl(const char *name, int *tw, int *th, int *trot)
{
    char p[160];
    unsigned char hd[14];
    FILE *f;
    int w, h, rot;

    snprintf(p, sizeof p, "%s/%s.tmpl", VIS_TMPL_DIR, name);
    f = fopen(p, "rb");
    if (!f) return -1;
    if (fread(hd, 1, sizeof hd, f) != sizeof hd) { fclose(f); return -1; }
    fclose(f);
    if (hd[0] != 'V' || hd[1] != 'T' || hd[2] != 'M' || hd[3] != '1') return -1;
    w = (int)hd[8] | ((int)hd[9] << 8);
    h = (int)hd[10] | ((int)hd[11] << 8);
    rot = hd[12];
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096 || rot > 3) return -1;
    *tw = w; *th = h; *trot = rot;
    return 0;
}

/* 发起一次试查（面板→核心）：填参数 → release 写 test_req_seq → 写唤醒 pipe → 置等待态 + 请求日志。
 * 单请求在途（g_vis_test_on）；在途时点按只提示、不重发。kind 0 的模板 w×h 先在本地读好（标记用）。 */
static void vis_test_fire(int kind, const char *ref, const char *region, int a1, int a2)
{
    unsigned seq;
    if (g_vis_test_on) {
        snprintf(g_vis_test_msg, sizeof g_vis_test_msg, "上一发还在等结果…");
        g_need = 1; g_force_frames = 2;
        return;
    }
    seq = vtouch_vis_test_post(kind, ref, region, a1, a2);
    if (!seq) {                                        /* 没接核心（单跑模式 / 附着失败） */
        g_vis_test_err = VIS_TEST_NOCORE;
        snprintf(g_vis_test_msg, sizeof g_vis_test_msg, "没接核心（无法试查）");
        g_need = 1; g_force_frames = 2;
        return;
    }
    g_vis_test_on = 1;
    g_vis_test_seq = seq;
    g_vis_test_t0 = now_ms();
    g_vis_test_err = VIS_TEST_WAIT;
    snprintf(g_vis_test_msg, sizeof g_vis_test_msg, "等待结果…");
    g_vis_testmk_kind = kind;
    g_vis_testmk_tw = 80; g_vis_testmk_th = 80; g_vis_testmk_trot = g_rot;   /* 找色固定 80×80 */
    if (kind == 0)                                     /* 找图：模板 w×h（读不到按 80×80 兜底） */
        vis_test_read_tmpl(ref, &g_vis_testmk_tw, &g_vis_testmk_th, &g_vis_testmk_trot);
    if (kind == 0) ALOGI("vis 试查 请求 找图 %s", ref);
    else if (kind == 2) ALOGI("vis 试查 请求 找色 多点 %s", ref);
    else ALOGI("vis 试查 请求 找色 单点 #%06X 容差=%d",
               (unsigned)(((uint32_t)a2 >> 8) & 0xFFFFFFu), (int)((uint32_t)a2 & 0xFFu));
    g_need = 1; g_force_frames = 2;
}

/* 「试一下」发起（步骤参数层）：校验当前步（同源 ope_step_check，不过 → 就地提示、不投）→ 按类型填参数。
 * 参数 = 当前编辑缓冲（未落表的改动也照测）。 */
static void vis_test_start_se(int se)
{
    char why[96];
    int type = g_ope_steps[se][0], kind, a1, a2;
    if (!ope_step_check(se, g_ope_steps[se], why, (int)sizeof why)) {
        snprintf(g_vis_edmsg, sizeof g_vis_edmsg, "%s", why);
        g_need = 1; g_force_frames = 2;
        return;
    }
    if (type == OP_STEP_FINDIMAGE) {
        kind = 0; a1 = g_ope_steps[se][1]; a2 = 0;     /* 找图：a1 = 阈值 */
    } else if (g_ope_steps[se][1] == 1) {
        kind = 2; a1 = 1; a2 = 0;                      /* 找色多点：点集在 ref */
    } else {
        kind = 1; a1 = 0; a2 = g_ope_steps[se][2];     /* 找色单点：a2 = (颜色<<8)|容差 */
    }
    vis_test_fire(kind, g_ope_refs[se], g_ope_exprs[se], a1, a2);
}

/* 「试一下」发起（模板页）：按行内名字直接投（全屏；找图阈值 8 = 新步默认口径）。 */
static void vis_test_start_tmpl(const char *name, int is_pts)
{
    if (is_pts) vis_test_fire(2, name, "", 1, 0);
    else        vis_test_fire(0, name, "", 8, 0);
}

/* 渲染循环每拍调（试查在途才做事）：收结果（acquire 轮询 test_res_seq，≤10ms）→ 结果行 + 标记；
 * 1.5s 无响应 →「超时（无响应）」。晚到的陈旧结果（seq 不是本次）不认领、不打断在途。 */
static void vis_test_tick(void)
{
    unsigned seq;
    int x, y, err;
    if (!g_vis_test_on) return;
    if (vtouch_vis_test_take(&seq, &x, &y, &err) && seq == g_vis_test_seq) {
        g_vis_test_on = 0;
        g_vis_test_err = err;
        if (err == 0) {
            snprintf(g_vis_test_msg, sizeof g_vis_test_msg, "命中 (%d, %d)", x, y);
            g_vis_testmk_x = x; g_vis_testmk_y = y; g_vis_testmk_t = now_ms();
        } else if (err == -1) {
            snprintf(g_vis_test_msg, sizeof g_vis_test_msg, "未命中");
        } else {
            snprintf(g_vis_test_msg, sizeof g_vis_test_msg, "%s", vis_test_err_word(err));
        }
        g_need = 1; g_force_frames = 2;
        return;
    }
    if (now_ms() - g_vis_test_t0 > VIS_TEST_TMO_MS) {
        g_vis_test_on = 0;
        g_vis_test_err = VIS_TEST_TMO;
        snprintf(g_vis_test_msg, sizeof g_vis_test_msg, "超时（无响应）");
        ALOGI("vis 试查 超时");
        g_need = 1; g_force_frames = 2;
    }
}

/* 采集图像 GL 纹理上传（渲染线程；调用时 GL 上下文已 current）。尺寸变 = 重建，否则子更新。 */
static void vis_tex_update(const unsigned char *rgba, int w, int h)
{
    if (!g_vis_tex) glGenTextures(1, &g_vis_tex);
    if (!g_vis_tex) return;
    glBindTexture(GL_TEXTURE_2D, g_vis_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    g_vis_magf = 0;                       /* 滤波被重置为线性；缩放滤波（Task 7.2）下帧按需重设 */
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    if (w != g_vis_tex_w || h != g_vis_tex_h) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        g_vis_tex_w = w; g_vis_tex_h = h;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    }
}
/* 面板侧帧缓冲取像素（紧排 w*4，RGBA8888）；越界 → -1。 */
static int vis_frame_px(const unsigned char *buf, int w, int h, int x, int y, uint32_t *rgb)
{
    const unsigned char *p;
    if (!buf || x < 0 || y < 0 || x >= w || y >= h) return -1;
    p = buf + ((size_t)y * (size_t)w + (size_t)x) * 4u;
    if (rgb) *rgb = ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | (uint32_t)p[2];
    return 0;
}
/* 点集编辑：一次轻点 —— 首点 = 基准色；其后 = 参考点（≤ VIS_PTS_MAX）。 */
static void vis_pts_tap(int fx, int fy, uint32_t rgb)
{
    if (g_vis_base_x < 0) {
        g_vis_base_x = fx; g_vis_base_y = fy; g_vis_base_rgb = rgb & 0xFFFFFFu;
        g_vis_cap_msg[0] = 0;
        ALOGI("vis 采集 基准色 #%06X @%d,%d（容差 %d）", (unsigned)g_vis_base_rgb, fx, fy, g_vis_base_tol);
        return;
    }
    if (g_vis_pts_n >= VIS_PTS_MAX) {
        snprintf(g_vis_cap_msg, sizeof g_vis_cap_msg, "最多 %d 个参考点", VIS_PTS_MAX);
        return;
    }
    g_vis_pts_x[g_vis_pts_n] = fx; g_vis_pts_y[g_vis_pts_n] = fy;
    g_vis_pts_rgb[g_vis_pts_n] = rgb & 0xFFFFFFu;
    g_vis_pts_n++;
    g_vis_cap_msg[0] = 0;
    ALOGI("vis 采集 参考点 %d +%d,%d #%06X", g_vis_pts_n, fx - g_vis_base_x, fy - g_vis_base_y,
          (unsigned)(rgb & 0xFFFFFFu));
}
/* 吸色（找色步 [取点]）：把颜色回填该步 a2 的颜色段（容差段保留），关采集层回视觉参数层。 */
static void vis_pick_apply(int fx, int fy, uint32_t rgb)
{
    int se = g_vis_pick_se;
    if (se >= 0 && se < g_ope_nsteps && g_ope_steps[se][0] == OP_STEP_FINDCOLOR) {
        uint32_t packed = (uint32_t)g_ope_steps[se][2];
        g_ope_steps[se][2] = (int)(((rgb & 0xFFFFFFu) << 8) | (packed & 0xFFu));
        ALOGI("vis edit 找色 第 %d 步 颜色=#%06X（吸色 @%d,%d）", se + 1, (unsigned)(rgb & 0xFFFFFFu), fx, fy);
    }
    vis_cap_close();
}

/* ---- 存盘 / 删除 ---- */

/* 存模板（.tmpl 写端；格式逐字照实施计划：VTM1 + ver u32=1 + w u16 + h u16 + rot u8 + res u8=0 + gray[w*h]）。
 * 灰度用引擎同源公式 vt_vis_gray_px（vt_vision.h 单一来源，防两处漂移）；.tmp + rename（掉电不留半截）。
 * 返回 0 成功；1 名字非法 / 2 重名 / 3 无可用画面或框选 / 4 建目录失败 / 5 写失败。 */
static int vis_save_tmpl(const char *name)
{
    char path[160], tmppath[168];
    const unsigned char *buf = g_vis_img;
    int x0 = g_vis_sel[0], y0 = g_vis_sel[1], x1 = g_vis_sel[2], y1 = g_vis_sel[3];
    int w, h, x, y;
    unsigned char *gray;
    FILE *f;
    if (vis_name_ok(name) != 0) return 1;
    if (vis_name_taken(name)) return 2;
    if (!g_vis_sel_on || !buf || g_vis_img_w <= 0 || g_vis_img_h <= 0) return 3;
    w = x1 - x0 + 1; h = y1 - y0 + 1;
    if (w < 1 || h < 1 || x0 < 0 || y0 < 0 || x1 >= g_vis_img_w || y1 >= g_vis_img_h) return 3;
    gray = (unsigned char *)malloc((size_t)w * (size_t)h);
    if (!gray) return 5;
    for (y = 0; y < h; y++) {
        const unsigned char *p = buf + ((size_t)(y0 + y) * (size_t)g_vis_img_w + (size_t)x0) * 4u;
        for (x = 0; x < w; x++, p += 4)
            gray[(size_t)y * (size_t)w + (size_t)x] = vt_vis_gray_px(p[0], p[1], p[2]);
    }
    if (mkdir(VIS_TMPL_DIR, 0775) < 0 && errno != EEXIST) { free(gray); return 4; }
    snprintf(tmppath, sizeof tmppath, "%s/%s.tmpl.tmp", VIS_TMPL_DIR, name);
    f = fopen(tmppath, "wb");
    if (!f) { free(gray); return 5; }
    fputs("VTM1", f);
    vis_put_u32le(f, 1);
    vis_put_u16le(f, (unsigned)w);
    vis_put_u16le(f, (unsigned)h);
    fputc(g_vis_img_rot & 3, f);
    fputc(0, f);                                            /* res = 0（保留位） */
    if (fwrite(gray, 1, (size_t)w * (size_t)h, f) != (size_t)w * (size_t)h) {
        fclose(f); remove(tmppath); free(gray); return 5;
    }
    if (fclose(f) != 0) { remove(tmppath); free(gray); return 5; }
    free(gray);
    snprintf(path, sizeof path, "%s/%s.tmpl", VIS_TMPL_DIR, name);
    if (rename(tmppath, path) != 0) { remove(tmppath); return 5; }
    ALOGI("vis 模板存 %s %dx%d rot=%d（灰度 %uB）", name, w, h, g_vis_img_rot, (unsigned)(w * h));
    return 0;
}
/* 存点集（.pts 写端；格式逐字照实施计划：VTP1 + ver u32=1 + n u16 + res u16=0 + base_rgb u32 +
 * base_tol u16 + n×{dx i16, dy i16, rgb u32, tol u16}；每点 tol = 基准容差（v1 不逐点编辑））。
 * 返回 0 成功；1 名字非法 / 2 重名 / 3 基准或点数不合法 / 4 建目录失败 / 5 写失败。 */
static int vis_save_pts(const char *name)
{
    char path[160], tmppath[168];
    FILE *f;
    int i;
    if (vis_name_ok(name) != 0) return 1;
    if (vis_name_taken(name)) return 2;
    if (g_vis_base_x < 0 || g_vis_pts_n < 1 || g_vis_pts_n > VIS_PTS_MAX) return 3;
    if (mkdir(VIS_TMPL_DIR, 0775) < 0 && errno != EEXIST) return 4;
    snprintf(tmppath, sizeof tmppath, "%s/%s.pts.tmp", VIS_TMPL_DIR, name);
    f = fopen(tmppath, "wb");
    if (!f) return 5;
    fputs("VTP1", f);
    vis_put_u32le(f, 1);
    vis_put_u16le(f, (unsigned)g_vis_pts_n);
    vis_put_u16le(f, 0);                                    /* res = 0（保留位） */
    vis_put_u32le(f, g_vis_base_rgb & 0xFFFFFFu);
    vis_put_u16le(f, (unsigned)g_vis_base_tol);
    for (i = 0; i < g_vis_pts_n; i++) {
        int dx = g_vis_pts_x[i] - g_vis_base_x;
        int dy = g_vis_pts_y[i] - g_vis_base_y;
        vis_put_u16le(f, (unsigned)(dx & 0xFFFF));          /* i16 小端（负数补码） */
        vis_put_u16le(f, (unsigned)(dy & 0xFFFF));
        vis_put_u32le(f, g_vis_pts_rgb[i] & 0xFFFFFFu);
        vis_put_u16le(f, (unsigned)g_vis_base_tol);         /* v1：每点 tol = 基准容差 */
    }
    if (fclose(f) != 0) { remove(tmppath); return 5; }
    snprintf(path, sizeof path, "%s/%s.pts", VIS_TMPL_DIR, name);
    if (rename(tmppath, path) != 0) { remove(tmppath); return 5; }
    ALOGI("vis 点集存 %s n=%d 基准 #%06X 容差 %d", name, g_vis_pts_n,
          (unsigned)(g_vis_base_rgb & 0xFFFFFFu), g_vis_base_tol);
    return 0;
}
/* 删一个模板 / 点集文件（列表 [删除]）；失败进事件日志。 */
static void vis_del_file(const char *name, int is_pts)
{
    char p[160];
    snprintf(p, sizeof p, "%s/%s.%s", VIS_TMPL_DIR, name, is_pts ? "pts" : "tmpl");
    if (remove(p) != 0) {
        ALOGW("vis %s删 失败 %s: %s", is_pts ? "点集" : "模板", name, strerror(errno));
        ev_note("%s删除失败：%s", is_pts ? "点集" : "模板", name);
        return;
    }
    ALOGI("vis %s删 %s", is_pts ? "点集" : "模板", name);
    g_need = 1; g_force_frames = 3;
}
/* save 返回码 3..5 → 人话（1/2 = 名字域，走 vis_name_why）。 */
static const char *vis_save_why(int rc)
{
    return rc == 3 ? "没有可用的画面/点集内容" : rc == 4 ? "模板目录创建失败" : "写入失败（磁盘/权限）";
}

/* ---- 采集覆盖层（整屏）：模板框选 / 点集编辑 / 吸色 ---- */

/* 采集层命名键盘（子层）：[确定] 校验名字（同一把尺子 + 重名拒绝）→ 存盘 → 关层；
 * 保存失败留在层里提示（同编辑层口径）。 */
static void draw_vis_cap_kb(void)
{
    const char *title = g_vis_cap == 2 ? "给点集起个名字：多点找色步骤按名字引用（重名会被拒）"
                                       : "给模板起个名字：找图步骤按名字引用（重名会被拒）";
    int act = draw_char_kb(title, NULL, g_vis_kb_buf, (int)sizeof g_vis_kb_buf, &g_vis_kb_up,
                           g_vis_kb_msg, (int)sizeof g_vis_kb_msg, 12.0f);
    if (act == 1) {
        g_vis_kb = 0; g_vis_kb_msg[0] = 0;                  /* 回采集视图（框选/点选保留） */
        g_need = 1; g_force_frames = 3;
        ALOGI("vis 采集 命名取消");
    } else if (act == 2) {
        int rc = vis_name_ok(g_vis_kb_buf);
        if (rc == 0 && vis_name_taken(g_vis_kb_buf)) rc = 4;
        if (rc == 0) {
            int sr = (g_vis_cap == 2) ? vis_save_pts(g_vis_kb_buf) : vis_save_tmpl(g_vis_kb_buf);
            if (sr != 0) {
                snprintf(g_vis_kb_msg, sizeof g_vis_kb_msg, "%s", sr <= 2 ? vis_name_why(sr) : vis_save_why(sr));
                ALOGW("vis 采集 保存失败 %s rc=%d", g_vis_kb_buf, sr);
                g_need = 1; g_force_frames = 2;
                return;
            }
            ev_log_push(g_vis_cap == 2 ? "点集已保存" : "模板已保存");
            vis_cap_close();
            return;
        }
        snprintf(g_vis_kb_msg, sizeof g_vis_kb_msg, "%s", vis_name_why(rc));
        g_need = 1; g_force_frames = 2;
    }
}

/* ---- 采集层缩放/平移 + 选择框调整（Task 7.2） ---- */

/* 缩放值文本（步进 0.25 ⇒ 至多两位小数、尾零去掉）：1 → "1x"、1.25 → "1.25x"、1.5 → "1.5x"。 */
static void vis_zoom_str(char *b, int n)
{
    int v = (int)(g_vis_zoom * 100.0f + 0.5f);
    if (v % 100 == 0) snprintf(b, n, "%dx", v / 100);
    else if (v % 10 == 0) snprintf(b, n, "%d.%dx", v / 100, (v / 10) % 10);
    else snprintf(b, n, "%d.%02dx", v / 100, v % 100);
}
/* 缩放步进（[＋]/[－]）：步长 0.25、范围 1x..8x；锚点 = 画面中心 ⇒ 平移量按比例同步（视口中心那一点不动）；
 * 平移夹取留给每帧换算（那里才知道当前视口几何）。 */
static void vis_zoom_step(int dir)
{
    float nz = g_vis_zoom + (dir > 0 ? 0.25f : -0.25f);
    if (nz < 1.0f) nz = 1.0f;
    if (nz > 8.0f) nz = 8.0f;
    if (nz == g_vis_zoom) return;
    g_vis_pan_x *= nz / g_vis_zoom;
    g_vis_pan_y *= nz / g_vis_zoom;
    g_vis_zoom = nz;
    if (g_vis_zoom <= 1.0f) { g_vis_pan_x = 0; g_vis_pan_y = 0; }
    g_need = 1; g_force_frames = 2;
    {
        char zv[16];
        vis_zoom_str(zv, sizeof zv);
        ALOGI("vis 采集 缩放 %s", zv);
    }
}
/* 适应（1x）：缩放与平移一起复位。 */
static void vis_zoom_fit(void)
{
    if (g_vis_zoom == 1.0f && g_vis_pan_x == 0 && g_vis_pan_y == 0) return;
    g_vis_zoom = 1.0f; g_vis_pan_x = 0; g_vis_pan_y = 0;
    g_need = 1; g_force_frames = 2;
    ALOGI("vis 采集 适应（1x）");
}
/* 四角手柄命中（屏距口径）：命中半径 VIS_HANDLE_R（≥24px 要求）内取最近角；无 → -1。 */
#define VIS_HANDLE_R 28.0f
static int vis_sel_corner_hit(float mx, float my, float ix, float iy, float sc)
{
    float ex[4], ey[4], best = VIS_HANDLE_R * VIS_HANDLE_R;
    int c, hit = -1;
    ex[0] = ix + g_vis_sel[0] * sc;         ey[0] = iy + g_vis_sel[1] * sc;
    ex[1] = ix + (g_vis_sel[2] + 1) * sc;   ey[1] = iy + g_vis_sel[1] * sc;
    ex[2] = ix + g_vis_sel[0] * sc;         ey[2] = iy + (g_vis_sel[3] + 1) * sc;
    ex[3] = ix + (g_vis_sel[2] + 1) * sc;   ey[3] = iy + (g_vis_sel[3] + 1) * sc;
    for (c = 0; c < 4; c++) {
        float dx = mx - ex[c], dy = my - ey[c], d2 = dx * dx + dy * dy;
        if (d2 <= best) { best = d2; hit = c; }
    }
    return hit;
}
/* 框内命中（屏距口径）：整框（含端点像素边缘）。 */
static int vis_sel_hit(float mx, float my, float ix, float iy, float sc)
{
    float x0 = ix + g_vis_sel[0] * sc, y0 = iy + g_vis_sel[1] * sc;
    float x1 = ix + (g_vis_sel[2] + 1) * sc, y1 = iy + (g_vis_sel[3] + 1) * sc;
    return mx >= x0 && mx <= x1 && my >= y0 && my <= y1;
}
/* 放大时用最近邻（看清像素边界 = 精细选择）；1x 回线性。只在状态变化时动 GL（纹理重建会复位）。 */
static void vis_tex_magf(void)
{
    int want = g_vis_zoom > 1.0f ? 1 : 0;
    if (!g_vis_tex || want == g_vis_magf) return;
    glBindTexture(GL_TEXTURE_2D, g_vis_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, want ? GL_NEAREST : GL_LINEAR);
    g_vis_magf = want;
}
/* 采集覆盖层（整屏；T3.2；缩放/平移 + 框调整 = Task 7.2）：三态 —— 等帧（wait）/ 失败（err）/ 图像就绪。
 * 图像等比缩放居中（模板/点集可 [＋]/[－]/[适应] 缩放 1x..8x、>1x 拖画面平移）；模板 = 拖框选（松手 ≥8×8
 * 落定、不弹命名；四角手柄/框内拖动调整，[确认] 才命名保存）；点集 = 首点吸基准色、其后加点（显示偏移/色）；
 * 吸色 = 点一下回填找色步颜色。整屏吞触摸（ui_rect_now）；子层 = 命名键盘。 */
static void build_vis_cap(void)
{
    ImDrawList *dl;
    ImVec2 wp, a, b;
    float ww, wh, x0, y0, cw, img_top, img_bot, sc = 0, iw = 0, ih = 0, ix = 0, iy = 0;
    int mode = g_vis_cap;
    float sw = (float)g_scr_w, sh = (float)g_scr_h;
    if (sw <= 0 || sh <= 0) return;
    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(sw, sh), ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0));
    ImGui::Begin("##viscap", 0,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar |
                 ImGuiWindowFlags_NoScrollWithMouse);
    g_zone_title[0] = g_zone_title[1] = g_zone_title[2] = g_zone_title[3] = 0;
    g_zone_side[0] = g_zone_side[1] = g_zone_side[2] = g_zone_side[3] = 0;
    g_zone_sheet[0] = g_zone_sheet[1] = g_zone_sheet[2] = g_zone_sheet[3] = 0;
    g_zone_list[0] = g_zone_list[1] = g_zone_list[2] = g_zone_list[3] = 0;
    g_zone_kb[0] = g_zone_kb[1] = g_zone_kb[2] = g_zone_kb[3] = 0;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(PAD_X, PAD_Y));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(12, 10));
    if (g_vis_kb) {
        draw_vis_cap_kb();
    } else {
        dl = ImGui::GetWindowDrawList();
        wp = ImGui::GetWindowPos();
        ww = ImGui::GetWindowWidth();
        wh = ImGui::GetWindowHeight();
        a = ImVec2(wp.x + 12, wp.y + 12);
        b = ImVec2(wp.x + ww - 12, wp.y + wh - 12);
        dl->AddRectFilled(a, b, IM_COL32(255, 255, 255, 253), 14);
        dl->AddRect(a, b, IM_COL32(228, 228, 231, 255), 14, 0, 1.5f);
        x0 = a.x + 26; y0 = a.y + 24; cw = (b.x - x0) - 26;
        ImGui::SetCursorScreenPos(ImVec2(x0, y0));
        text_meta_s(mode == 1 ? "模板采集 · 拖动框选一块图案（拖四角调整），[确认] 命名保存"
                   : mode == 2 ? "点集编辑 · 先点一下吸基准色，再点参考点（最多 16 个）"
                               : "吸色 · 点画面里要取的颜色（回填找色步骤）");
        if (g_vis_cap_msg[0]) {
            ImGui::SetCursorScreenPos(ImVec2(x0, y0 + 48));
            ImGui::TextColored(ImVec4(0.863f, 0.149f, 0.149f, 1.00f), "%s", g_vis_cap_msg);
        }
        img_top = y0 + 96.0f;
        img_bot = b.y - 24.0f - 104.0f - (mode == 2 ? 88.0f : 0.0f);   /* 底：按钮 92+缝 12；点集再让容差行 76+12 */
        if (img_bot < img_top + 120.0f) img_bot = img_top + 120.0f;    /* 极矮兜底 */
        /* 缩放行（Task 7.2；模板/点集）：[＋] [－] [适应] + 当前值；插在标题/提示与画面之间（矮屏收一档） */
        if (mode != 3) {
            float zh = (img_bot - img_top) < 300.0f ? 46.0f : 64.0f;
            float zw = (cw - 3 * 12.0f) / 4.0f;
            char zv[16];
            ImGui::SetCursorScreenPos(ImVec2(x0, img_top));
            if (btn_light("＋", ImVec2(zw, zh))) vis_zoom_step(+1);
            ImGui::SetCursorScreenPos(ImVec2(x0 + zw + 12, img_top));
            if (btn_light("－", ImVec2(zw, zh))) vis_zoom_step(-1);
            ImGui::SetCursorScreenPos(ImVec2(x0 + 2 * (zw + 12), img_top));
            if (btn_light("适应", ImVec2(zw, zh))) vis_zoom_fit();
            vis_zoom_str(zv, sizeof zv);
            ImGui::SetCursorScreenPos(ImVec2(x0 + 3 * (zw + 12), img_top));
            btn_light(zv, ImVec2(zw, zh));     /* 当前值（只显示） */
            img_top += zh + 12.0f;
        }
        if (g_vis_cap_wait) {
            ImGui::SetCursorScreenPos(ImVec2(x0, img_top + 40));
            text_meta_w("抓帧中…（面板向系统要一帧；通常 5~25ms）");
        } else if (g_vis_cap_err) {
            char m[128];
            snprintf(m, sizeof m, "抓帧失败（%s，错误码 %d）：检查系统抓屏是否可用",
                     g_vis_cap_err == -1 ? "超时" : "错误", g_vis_cap_err);
            ImGui::SetCursorScreenPos(ImVec2(x0, img_top + 40));
            text_meta_w(m);
        } else if (g_vis_img && g_vis_img_w > 0 && g_vis_img_h > 0) {
            float vh = img_bot - img_top;
            sc = cw / (float)g_vis_img_w;
            {
                float sy = vh / (float)g_vis_img_h;
                if (sy < sc) sc = sy;
            }
            sc *= g_vis_zoom;                        /* 显示比例 = 适应比例 × 缩放（Task 7.2） */
            iw = (float)g_vis_img_w * sc;
            ih = (float)g_vis_img_h * sc;
            {   /* 平移夹取：画面不许比视口小（顶到边即停）；每帧按当前视口算（转屏/换窗自动纠正） */
                float mpx = (iw - cw) * 0.5f, mpy = (ih - vh) * 0.5f;
                if (mpx < 0) mpx = 0;
                if (mpy < 0) mpy = 0;
                if (g_vis_pan_x < -mpx) g_vis_pan_x = -mpx;
                if (g_vis_pan_x > mpx) g_vis_pan_x = mpx;
                if (g_vis_pan_y < -mpy) g_vis_pan_y = -mpy;
                if (g_vis_pan_y > mpy) g_vis_pan_y = mpy;
            }
            ix = x0 + (cw - iw) * 0.5f + g_vis_pan_x;
            iy = img_top + (vh - ih) * 0.5f + g_vis_pan_y;
            if (g_vis_tex_dirty) { vis_tex_update(g_vis_img, g_vis_img_w, g_vis_img_h); g_vis_tex_dirty = 0; }
            vis_tex_magf();                          /* 放大 → 最近邻（像素边界可见） */
            dl->PushClipRect(ImVec2(x0, img_top), ImVec2(x0 + cw, img_bot), true);   /* 放大时画面裁在视口内 */
            dl->AddRectFilled(ImVec2(ix - 2, iy - 2), ImVec2(ix + iw + 2, iy + ih + 2), IM_COL32(228, 228, 231, 255), 4);
            if (g_vis_tex)
                dl->AddImage((ImTextureID)(intptr_t)g_vis_tex, ImVec2(ix, iy), ImVec2(ix + iw, iy + ih));
            ImGui::PushID(9100);
            {
                /* 交互区 = 画面 ∩ 视口（1x 下与原来一致 = 画面本身；>1x 时铺满被画面盖住的视口） */
                float bx0 = ix > x0 ? ix : x0;
                float by0 = iy > img_top ? iy : img_top;
                float bx1 = (ix + iw) < (x0 + cw) ? (ix + iw) : (x0 + cw);
                float by1 = (iy + ih) < img_bot ? (iy + ih) : img_bot;
                ImGui::SetCursorScreenPos(ImVec2(bx0, by0));
                ImGui::InvisibleButton("##img", ImVec2(bx1 - bx0, by1 - by0));
            }
            {
                ImVec2 mp = ImGui::GetIO().MousePos;
                int act = ImGui::IsItemActive();
                int deact = ImGui::IsItemDeactivated();
                if (act && !g_vis_drag) {
                    /* 起手判手势（Task 7.2）：手柄 > 框内 > （缩放 >1x ? 平移 : 新框）；点集/吸色：待定（轻点 or 平移） */
                    g_vis_drag = 1;
                    g_vis_dx0 = mp.x; g_vis_dy0 = mp.y;
                    g_vis_pmx = mp.x; g_vis_pmy = mp.y;
                    g_vis_gest = 0;
                    if (mode == 1) {
                        int corner = -1;
                        if (g_vis_sel_on) corner = vis_sel_corner_hit(mp.x, mp.y, ix, iy, sc);
                        if (corner >= 0) {
                            g_vis_gest = 3; g_vis_gest_corner = corner;
                            g_vis_gest_box[0] = g_vis_sel[0]; g_vis_gest_box[1] = g_vis_sel[1];
                            g_vis_gest_box[2] = g_vis_sel[2]; g_vis_gest_box[3] = g_vis_sel[3];
                        } else if (g_vis_sel_on && vis_sel_hit(mp.x, mp.y, ix, iy, sc)) {
                            g_vis_gest = 2;
                            g_vis_gest_box[0] = g_vis_sel[0]; g_vis_gest_box[1] = g_vis_sel[1];
                            g_vis_gest_box[2] = g_vis_sel[2]; g_vis_gest_box[3] = g_vis_sel[3];
                        } else {
                            g_vis_gest = g_vis_zoom > 1.0f ? 4 : 1;
                        }
                    }
                }
                if (g_vis_drag && act) {
                    if (g_vis_gest == 1) {
                        float rx0 = g_vis_dx0 < mp.x ? g_vis_dx0 : mp.x;
                        float rx1 = g_vis_dx0 < mp.x ? mp.x : g_vis_dx0;
                        float ry0 = g_vis_dy0 < mp.y ? g_vis_dy0 : mp.y;
                        float ry1 = g_vis_dy0 < mp.y ? mp.y : g_vis_dy0;
                        dl->AddRect(ImVec2(rx0, ry0), ImVec2(rx1, ry1), IM_COL32(59, 130, 246, 255), 0, 0, 3.0f);
                    } else if (g_vis_gest == 2) {
                        /* 框内拖动 = 整体移动（起点框 + 总位移，帧坐标；夹在图内、尺寸不变） */
                        int w0 = g_vis_gest_box[2] - g_vis_gest_box[0], h0 = g_vis_gest_box[3] - g_vis_gest_box[1];
                        int nx = (int)lroundf(g_vis_gest_box[0] + (mp.x - g_vis_dx0) / sc);
                        int ny = (int)lroundf(g_vis_gest_box[1] + (mp.y - g_vis_dy0) / sc);
                        if (nx < 0) nx = 0;
                        if (nx > g_vis_img_w - 1 - w0) nx = g_vis_img_w - 1 - w0;
                        if (ny < 0) ny = 0;
                        if (ny > g_vis_img_h - 1 - h0) ny = g_vis_img_h - 1 - h0;
                        g_vis_sel[0] = nx; g_vis_sel[1] = ny;
                        g_vis_sel[2] = nx + w0; g_vis_sel[3] = ny + h0;
                        g_need = 1; g_force_frames = 2;
                    } else if (g_vis_gest == 3) {
                        /* 四角手柄拖动 = 缩放框（**起点角 + 位移增量，零跳变**；帧坐标取整、夹在图内、最小 8×8 含端点） */
                        int x0b = g_vis_gest_box[0], y0b = g_vis_gest_box[1];
                        int x1b = g_vis_gest_box[2], y1b = g_vis_gest_box[3];
                        int fx, fy;
                        if (g_vis_gest_corner == 0 || g_vis_gest_corner == 2)
                            fx = (int)lroundf(x0b + (mp.x - g_vis_dx0) / sc);
                        else
                            fx = (int)lroundf(x1b + (mp.x - g_vis_dx0) / sc);
                        if (g_vis_gest_corner == 0 || g_vis_gest_corner == 1)
                            fy = (int)lroundf(y0b + (mp.y - g_vis_dy0) / sc);
                        else
                            fy = (int)lroundf(y1b + (mp.y - g_vis_dy0) / sc);
                        if (fx < 0) fx = 0;
                        if (fx > g_vis_img_w - 1) fx = g_vis_img_w - 1;
                        if (fy < 0) fy = 0;
                        if (fy > g_vis_img_h - 1) fy = g_vis_img_h - 1;
                        if (g_vis_gest_corner == 0) { x0b = fx; y0b = fy; }
                        else if (g_vis_gest_corner == 1) { x1b = fx; y0b = fy; }
                        else if (g_vis_gest_corner == 2) { x0b = fx; y1b = fy; }
                        else { x1b = fx; y1b = fy; }
                        if (x1b - x0b < 7) { if (g_vis_gest_corner & 1) x1b = x0b + 7; else x0b = x1b - 7; }
                        if (y1b - y0b < 7) { if (g_vis_gest_corner & 2) y1b = y0b + 7; else y0b = y1b - 7; }
                        g_vis_sel[0] = x0b; g_vis_sel[1] = y0b;
                        g_vis_sel[2] = x1b; g_vis_sel[3] = y1b;
                        g_need = 1; g_force_frames = 2;
                    } else if (g_vis_gest == 4) {
                        /* 平移（缩放 >1x 时拖画面）：增量跟手；夹取交给每帧换算 */
                        g_vis_pan_x += mp.x - g_vis_pmx;
                        g_vis_pan_y += mp.y - g_vis_pmy;
                        g_need = 1; g_force_frames = 2;
                    } else if (mode != 1) {
                        /* 点集/吸色：位移超轻点阈值（14px）且缩放 >1x → 转平移（越阈前的位移不算） */
                        float ddx = mp.x - g_vis_dx0, ddy = mp.y - g_vis_dy0;
                        if (g_vis_zoom > 1.0f && ddx * ddx + ddy * ddy > 14.0f * 14.0f) {
                            g_vis_gest = 4;
                            g_vis_pmx = mp.x; g_vis_pmy = mp.y;
                        }
                    }
                    g_vis_pmx = mp.x; g_vis_pmy = mp.y;
                }
                if (deact && g_vis_drag) {
                    float ddx = mp.x - g_vis_dx0, ddy = mp.y - g_vis_dy0;
                    int gest = g_vis_gest;
                    g_vis_drag = 0; g_vis_gest = 0;
                    if (mode == 1) {
                        if (gest == 1) {
                            /* 新框落定：显示坐标 → 帧坐标 floor + 夹取；≥8×8 才收（照旧）。不弹命名键盘
                             * （Task 7.2：框保留、可继续调整，[确认] 才进命名）。 */
                            float mx0 = g_vis_dx0 < mp.x ? g_vis_dx0 : mp.x;
                            float mx1 = g_vis_dx0 < mp.x ? mp.x : g_vis_dx0;
                            float my0 = g_vis_dy0 < mp.y ? g_vis_dy0 : mp.y;
                            float my1 = g_vis_dy0 < mp.y ? mp.y : g_vis_dy0;
                            int fx0 = (int)((mx0 - ix) / sc), fx1 = (int)((mx1 - ix) / sc);
                            int fy0 = (int)((my0 - iy) / sc), fy1 = (int)((my1 - iy) / sc);
                            if (fx0 < 0) fx0 = 0;
                            if (fy0 < 0) fy0 = 0;
                            if (fx1 > g_vis_img_w - 1) fx1 = g_vis_img_w - 1;
                            if (fy1 > g_vis_img_h - 1) fy1 = g_vis_img_h - 1;
                            if (fx0 <= fx1 && fy0 <= fy1 && fx1 - fx0 + 1 >= 8 && fy1 - fy0 + 1 >= 8) {
                                g_vis_sel[0] = fx0; g_vis_sel[1] = fy0;
                                g_vis_sel[2] = fx1; g_vis_sel[3] = fy1;
                                g_vis_sel_on = 1;
                                ALOGI("vis 采集 框选 %d,%d-%d,%d（%dx%d）", fx0, fy0, fx1, fy1,
                                      fx1 - fx0 + 1, fy1 - fy0 + 1);
                            }
                        } else if ((gest == 2 || gest == 3) &&
                                   (g_vis_sel[0] != g_vis_gest_box[0] || g_vis_sel[1] != g_vis_gest_box[1] ||
                                    g_vis_sel[2] != g_vis_gest_box[2] || g_vis_sel[3] != g_vis_gest_box[3])) {
                            ALOGI("vis 采集 调整框 %d,%d-%d,%d（%dx%d）", g_vis_sel[0], g_vis_sel[1],
                                  g_vis_sel[2], g_vis_sel[3], g_vis_sel[2] - g_vis_sel[0] + 1,
                                  g_vis_sel[3] - g_vis_sel[1] + 1);
                        } else if (gest == 4) {
                            ALOGI("vis 采集 平移 %.0f,%.0f", (double)g_vis_pan_x, (double)g_vis_pan_y);
                        }
                    } else if (gest == 4) {
                        ALOGI("vis 采集 平移 %.0f,%.0f", (double)g_vis_pan_x, (double)g_vis_pan_y);
                    } else if (ddx * ddx + ddy * ddy <= 14.0f * 14.0f) {
                        /* 点选（点集 / 吸色）：轻点才算（拖动不算） */
                        int fx = (int)((mp.x - ix) / sc), fy = (int)((mp.y - iy) / sc);
                        uint32_t rgb = 0;
                        if (fx >= 0 && fy >= 0 && fx < g_vis_img_w && fy < g_vis_img_h &&
                            vis_frame_px(g_vis_img, g_vis_img_w, g_vis_img_h, fx, fy, &rgb) == 0) {
                            if (mode == 3) vis_pick_apply(fx, fy, rgb);
                            else vis_pts_tap(fx, fy, rgb);
                        }
                    }
                    g_need = 1; g_force_frames = 3;
                }
                if (mode == 1 && g_vis_sel_on) {          /* 已选态：框 + 四角手柄（16px 方块；命中半径见 VIS_HANDLE_R） */
                    ImVec2 q0 = ImVec2(ix + g_vis_sel[0] * sc, iy + g_vis_sel[1] * sc);
                    ImVec2 q1 = ImVec2(ix + (g_vis_sel[2] + 1) * sc, iy + (g_vis_sel[3] + 1) * sc);
                    ImVec2 hc[4];
                    int c;
                    dl->AddRect(q0, q1, IM_COL32(59, 130, 246, 255), 0, 0, 3.0f);
                    hc[0] = ImVec2(q0.x, q0.y); hc[1] = ImVec2(q1.x, q0.y);
                    hc[2] = ImVec2(q0.x, q1.y); hc[3] = ImVec2(q1.x, q1.y);
                    for (c = 0; c < 4; c++) {
                        dl->AddRectFilled(ImVec2(hc[c].x - 8, hc[c].y - 8), ImVec2(hc[c].x + 8, hc[c].y + 8),
                                          IM_COL32(255, 255, 255, 255), 2.0f);
                        dl->AddRect(ImVec2(hc[c].x - 8, hc[c].y - 8), ImVec2(hc[c].x + 8, hc[c].y + 8),
                                    IM_COL32(59, 130, 246, 255), 2.0f, 0, 2.5f);
                    }
                }
                if (mode == 2) {
                    /* 基准 + 参考点标记（显示层；帧坐标 → 屏坐标 = ix + fx*sc） */
                    char lb[44];
                    int k;
                    if (g_vis_base_x >= 0) {
                        float bx = ix + g_vis_base_x * sc, by2 = iy + g_vis_base_y * sc;
                        dl->AddCircle(ImVec2(bx, by2), 16.0f, IM_COL32(255, 140, 0, 255), 32, 4.0f);
                        dl->AddLine(ImVec2(bx - 22, by2), ImVec2(bx + 22, by2), IM_COL32(255, 140, 0, 255), 4.0f);
                        dl->AddLine(ImVec2(bx, by2 - 22), ImVec2(bx, by2 + 22), IM_COL32(255, 140, 0, 255), 4.0f);
                        snprintf(lb, sizeof lb, "基准 #%06X", (unsigned)g_vis_base_rgb);
                        dl->AddText(ImVec2(bx + 20, by2 + 12), IM_COL32(180, 83, 9, 255), lb);
                    }
                    for (k = 0; k < g_vis_pts_n; k++) {
                        float px = ix + g_vis_pts_x[k] * sc, py = iy + g_vis_pts_y[k] * sc;
                        ImU32 cc = IM_COL32((g_vis_pts_rgb[k] >> 16) & 0xFF, (g_vis_pts_rgb[k] >> 8) & 0xFF,
                                            g_vis_pts_rgb[k] & 0xFF, 255);
                        if (g_vis_base_x >= 0)
                            dl->AddLine(ImVec2(ix + g_vis_base_x * sc, iy + g_vis_base_y * sc), ImVec2(px, py),
                                        IM_COL32(59, 130, 246, 140), 2.0f);
                        dl->AddCircleFilled(ImVec2(px, py), 11.0f, cc);
                        dl->AddCircle(ImVec2(px, py), 11.0f, IM_COL32(24, 24, 27, 255), 24, 2.0f);
                        snprintf(lb, sizeof lb, "%d +%d,%d #%06X", k + 1,
                                 g_vis_pts_x[k] - g_vis_base_x, g_vis_pts_y[k] - g_vis_base_y,
                                 (unsigned)g_vis_pts_rgb[k]);
                        dl->AddText(ImVec2(px + 14, py + 10), IM_COL32(24, 24, 27, 255), lb);
                    }
                }
            }
            ImGui::PopID();
            dl->PopClipRect();                   /* 画面视口裁剪（Task 7.2）结束 */
        } else {
            ImGui::SetCursorScreenPos(ImVec2(x0, img_top + 40));
            text_meta_w("没有画面");
        }
        /* 底部按钮（状态相关） */
        {
            float by = b.y - 24.0f - 92.0f;
            float bw2 = (cw - 12.0f) * 0.5f;
            if (g_vis_cap_wait) {
                ImGui::SetCursorScreenPos(ImVec2(x0, by));
                if (btn_light("取消", ImVec2(cw, 92))) { ALOGI("vis 采集 取消（等帧中）"); vis_cap_close(); }
            } else if (g_vis_cap_err) {
                ImGui::SetCursorScreenPos(ImVec2(x0, by));
                if (btn_light("重试", ImVec2(bw2, 92))) {
                    g_vis_cap_err = 0; g_vis_cap_wait = 1; g_vis_cap_t0 = now_ms();
                    g_vis_img = 0; g_vis_img_w = 0; g_vis_img_h = 0;
                    vtouch_vis_panel_capture_req();
                    g_need = 1; g_force_frames = 3;
                    ALOGI("vis 采集 重试");
                }
                ImGui::SetCursorScreenPos(ImVec2(x0 + bw2 + 12, by));
                if (btn_light("返回", ImVec2(bw2, 92))) vis_cap_close();
            } else if (mode == 2) {
                float ty = by - 12.0f - 76.0f;
                float tw = 130.0f;
                char tv[24];
                ImGui::SetCursorScreenPos(ImVec2(x0, ty));
                if (btn_light("-", ImVec2(tw, 76))) { if (g_vis_base_tol > 0) g_vis_base_tol--; g_need = 1; g_force_frames = 2; }
                snprintf(tv, sizeof tv, "基准容差 %d", g_vis_base_tol);
                ImGui::SetCursorScreenPos(ImVec2(x0 + tw + 12, ty));
                btn_light(tv, ImVec2(cw - 2 * (tw + 12), 76));   /* 只显示（不可点） */
                ImGui::SetCursorScreenPos(ImVec2(x0 + cw - tw, ty));
                if (btn_light("+", ImVec2(tw, 76))) { if (g_vis_base_tol < 255) g_vis_base_tol++; g_need = 1; g_force_frames = 2; }
                {
                    float bw3 = (cw - 2 * 12) / 3.0f;
                    ImGui::SetCursorScreenPos(ImVec2(x0, by));
                    if (btn_light("取消", ImVec2(bw3, 92))) { ALOGI("vis 采集 取消（点集）"); vis_cap_close(); }
                    ImGui::SetCursorScreenPos(ImVec2(x0 + bw3 + 12, by));
                    if (btn_light("重来", ImVec2(bw3, 92))) {
                        g_vis_base_x = -1; g_vis_base_y = -1; g_vis_base_rgb = 0; g_vis_pts_n = 0;
                        g_vis_cap_msg[0] = 0;
                        g_need = 1; g_force_frames = 3;
                        ALOGI("vis 采集 点集重来");
                    }
                    ImGui::SetCursorScreenPos(ImVec2(x0 + 2 * (bw3 + 12), by));
                    if (g_vis_base_x >= 0 && g_vis_pts_n >= 1) {
                        if (btn_blue("存点集", ImVec2(bw3, 92))) {
                            g_vis_kb = 2; g_vis_kb_buf[0] = 0; g_vis_kb_msg[0] = 0; g_vis_kb_up = 0;
                            ALOGI("vis 采集 点集命名开（n=%d）", g_vis_pts_n);
                        }
                    } else {
                        ImGui::BeginDisabled();
                        btn_light("存点集", ImVec2(bw3, 92));
                        ImGui::EndDisabled();
                    }
                }
            } else if (mode == 1) {
                float bw3 = (cw - 2 * 12.0f) / 3.0f;
                ImGui::SetCursorScreenPos(ImVec2(x0, by));
                if (btn_light("取消", ImVec2(bw3, 92))) { ALOGI("vis 采集 取消（模板）"); vis_cap_close(); }
                ImGui::SetCursorScreenPos(ImVec2(x0 + bw3 + 12, by));
                if (btn_light("重新截帧", ImVec2(bw3, 92))) {
                    g_vis_cap_err = 0; g_vis_cap_wait = 1; g_vis_cap_t0 = now_ms();
                    g_vis_img = 0; g_vis_img_w = 0; g_vis_img_h = 0;   /* 框保留（Task 7.2：已选态跨截帧保留、可继续调整） */
                    vtouch_vis_panel_capture_req();
                    g_need = 1; g_force_frames = 3;
                    ALOGI("vis 采集 重新截帧");
                }
                ImGui::SetCursorScreenPos(ImVec2(x0 + 2 * (bw3 + 12), by));
                if (g_vis_sel_on) {
                    if (btn_blue("确认", ImVec2(bw3, 92))) {   /* 确认 → 命名键盘；取消命名回采集视图、框保留 */
                        g_vis_kb = 1; g_vis_kb_buf[0] = 0; g_vis_kb_msg[0] = 0; g_vis_kb_up = 0;
                        ALOGI("vis 采集 确认（模板命名开）");
                    }
                } else {
                    ImGui::BeginDisabled();
                    btn_light("确认", ImVec2(bw3, 92));
                    ImGui::EndDisabled();
                }
            } else {
                ImGui::SetCursorScreenPos(ImVec2(x0, by));
                if (btn_light("取消", ImVec2(cw, 92))) { ALOGI("vis 采集 取消（吸色）"); vis_cap_close(); }
            }
        }
    }
    ImGui::PopStyleVar(2);
    ImGui::End();
    ImGui::PopStyleColor(2);
    if (g_scr_target == SCR_NONE) g_scroll_acc = 0;   /* 与 build_panel 同款：非滚动容器按下丢弃累积量 */
}

/* ---- 视觉步参数层（编辑层子层；g_vis_ed） ---- */

/* 视觉步字段数字编辑入口（找图 = 阈值 a1 / 找色 = 容差 a2 低 8 位；T7.4 加第 2 格超时 ms）：复用数字键盘子层
 * （draw_num_edit；字段模型见 ope_fidx 的 10/11 行；[完成] 写回经 ne_field_set 拆包）。sf = 进层激活的格。 */
static void ne_open_vis_field(int se, int sf)
{
    if (se < 0 || se >= g_ope_nsteps) return;
    if (g_ope_steps[se][0] != OP_STEP_FINDIMAGE && g_ope_steps[se][0] != OP_STEP_FINDCOLOR) return;
    g_ope_se = se; g_ope_sf = sf; g_ne_tgt = 0;
    g_vis_num = 1;
    ope_num_load();
    g_ne_msg[0] = 0;
    g_need = 1; g_force_frames = 2;
    ALOGI("op edit 参数开（视觉） 第 %d 步 %s", se + 1, ope_tname(g_ope_steps[se][0]));
}
/* 模板列表子层（找图步 [模板]）：枚举 templates 下的 .tmpl，点一条写回该步 ref（模板名）。 */
static void draw_vis_tlist(void)
{
    ImDrawList *dl;
    ImVec2 wp, a, b;
    float ww, wh, x0, y0, cw, list_top, list_bot, list_h;
    char names[64][16];
    int i, n, se = g_ope_se;
    if (se < 0 || se >= g_ope_nsteps) { g_vis_tl = 0; return; }
    dl = ImGui::GetWindowDrawList();
    wp = ImGui::GetWindowPos();
    ww = ImGui::GetWindowWidth();
    wh = ImGui::GetWindowHeight();
    a = ImVec2(wp.x + 12, wp.y + 12);
    b = ImVec2(wp.x + ww - 12, wp.y + wh - 12);
    dl->AddRectFilled(a, b, IM_COL32(255, 255, 255, 253), 14);
    dl->AddRect(a, b, IM_COL32(228, 228, 231, 255), 14, 0, 1.5f);
    x0 = a.x + 26; y0 = a.y + 24; cw = (b.x - x0) - 26;
    ImGui::SetCursorScreenPos(ImVec2(x0, y0));
    {
        char t[80];
        snprintf(t, sizeof t, "第 %d 步 · 找图 · 选择模板", se + 1);
        text_meta_s(t);
    }
    ImGui::SetCursorScreenPos(ImVec2(x0, y0 + 40));
    text_meta_s("templates/ 下的 .tmpl（到「模板」页截帧框选生成）");
    list_top = y0 + 76;
    list_bot = b.y - 24 - 92 - 12;
    list_h = list_bot - list_top;
    if (list_h < 0) list_h = 0;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ZINC50);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 12));
    ImGui::SetCursorScreenPos(ImVec2(x0, list_top));
    ImGui::BeginChild("##vistl", ImVec2(cw, list_h), ImGuiChildFlags_None,
                      ImGuiWindowFlags_AlwaysVerticalScrollbar);
    pub_zone(g_zone_list);
    drag_scroll_for(SCR_LIST);
    n = vis_list(".tmpl", names, 64);
    for (i = 0; i < n; i++) {
        ImGui::PushID(3300 + i);
        if ((strcmp(g_ope_refs[se], names[i]) == 0) ? btn_blue(names[i], ImVec2(ImGui::GetContentRegionAvail().x, 76))
                                                   : btn_light(names[i], ImVec2(ImGui::GetContentRegionAvail().x, 76))) {
            snprintf(g_ope_refs[se], sizeof g_ope_refs[se], "%s", names[i]);
            g_vis_tl = 0;
            g_need = 1; g_force_frames = 3;
            ALOGI("vis edit 找图 第 %d 步 模板=%s", se + 1, names[i]);
        }
        ImGui::PopID();
    }
    if (n == 0) text_meta_w("还没有模板：先到「模板」页截帧框选生成");
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    ImGui::PushID(3400);
    ImGui::SetCursorScreenPos(ImVec2(x0, b.y - 24 - 92));
    if (btn_light("取消", ImVec2(cw, 92))) {
        g_vis_tl = 0;
        g_need = 1; g_force_frames = 2;
    }
    ImGui::PopID();
}
/* 点集列表子层（找色多点 [点集]）：枚举 templates 下的 .pts，点一条写回该步 ref（点集名）。 */
static void draw_vis_plist(void)
{
    ImDrawList *dl;
    ImVec2 wp, a, b;
    float ww, wh, x0, y0, cw, list_top, list_bot, list_h;
    char names[64][16];
    int i, n, se = g_ope_se;
    if (se < 0 || se >= g_ope_nsteps) { g_vis_pl = 0; return; }
    dl = ImGui::GetWindowDrawList();
    wp = ImGui::GetWindowPos();
    ww = ImGui::GetWindowWidth();
    wh = ImGui::GetWindowHeight();
    a = ImVec2(wp.x + 12, wp.y + 12);
    b = ImVec2(wp.x + ww - 12, wp.y + wh - 12);
    dl->AddRectFilled(a, b, IM_COL32(255, 255, 255, 253), 14);
    dl->AddRect(a, b, IM_COL32(228, 228, 231, 255), 14, 0, 1.5f);
    x0 = a.x + 26; y0 = a.y + 24; cw = (b.x - x0) - 26;
    ImGui::SetCursorScreenPos(ImVec2(x0, y0));
    {
        char t[80];
        snprintf(t, sizeof t, "第 %d 步 · 找色 · 选择点集", se + 1);
        text_meta_s(t);
    }
    ImGui::SetCursorScreenPos(ImVec2(x0, y0 + 40));
    text_meta_s("templates/ 下的 .pts（到「模板」页吸色点选生成）");
    list_top = y0 + 76;
    list_bot = b.y - 24 - 92 - 12;
    list_h = list_bot - list_top;
    if (list_h < 0) list_h = 0;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ZINC50);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 12));
    ImGui::SetCursorScreenPos(ImVec2(x0, list_top));
    ImGui::BeginChild("##vispl", ImVec2(cw, list_h), ImGuiChildFlags_None,
                      ImGuiWindowFlags_AlwaysVerticalScrollbar);
    pub_zone(g_zone_list);
    drag_scroll_for(SCR_LIST);
    n = vis_list(".pts", names, 64);
    for (i = 0; i < n; i++) {
        ImGui::PushID(3600 + i);
        if ((strcmp(g_ope_refs[se], names[i]) == 0) ? btn_blue(names[i], ImVec2(ImGui::GetContentRegionAvail().x, 76))
                                                   : btn_light(names[i], ImVec2(ImGui::GetContentRegionAvail().x, 76))) {
            snprintf(g_ope_refs[se], sizeof g_ope_refs[se], "%s", names[i]);
            g_vis_pl = 0;
            g_need = 1; g_force_frames = 3;
            ALOGI("vis edit 找色 第 %d 步 点集=%s", se + 1, names[i]);
        }
        ImGui::PopID();
    }
    if (n == 0) text_meta_w("还没有点集：先到「模板」页「截帧 · 做点集」生成");
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    ImGui::PushID(3700);
    ImGui::SetCursorScreenPos(ImVec2(x0, b.y - 24 - 92));
    if (btn_light("取消", ImVec2(cw, 92))) {
        g_vis_pl = 0;
        g_need = 1; g_force_frames = 2;
    }
    ImGui::PopID();
}
/* 颜色十六进制键盘子层（找色单点 [颜色]）：复用字符键盘；[确定] 校验 1..6 位十六进制 →
 * 写回 a2 的颜色段（容差段保留）→ 关层。 */
static void draw_vis_hex(void)
{
    int act = draw_char_kb("颜色（十六进制 RRGGBB，例 ff8800；0-9 a-f）", NULL, g_vis_hexbuf,
                           (int)sizeof g_vis_hexbuf, &g_vis_hexup, g_vis_hexmsg,
                           (int)sizeof g_vis_hexmsg, 12.0f);
    if (act == 1) {
        g_vis_hex = 0; g_vis_hexmsg[0] = 0;
        g_need = 1; g_force_frames = 2;
    } else if (act == 2) {
        int se = g_ope_se, n = (int)strlen(g_vis_hexbuf), i, ok = (n >= 1 && n <= 6);
        unsigned c = 0;
        for (i = 0; ok && i < n; i++) {
            char ch = g_vis_hexbuf[i];
            if (ch >= '0' && ch <= '9') c = c * 16u + (unsigned)(ch - '0');
            else if (ch >= 'a' && ch <= 'f') c = c * 16u + (unsigned)(ch - 'a' + 10);
            else if (ch >= 'A' && ch <= 'F') c = c * 16u + (unsigned)(ch - 'A' + 10);
            else ok = 0;
        }
        if (!ok || se < 0 || se >= g_ope_nsteps || g_ope_steps[se][0] != OP_STEP_FINDCOLOR) {
            snprintf(g_vis_hexmsg, sizeof g_vis_hexmsg, "颜色要是 1–6 位十六进制（0-9 a-f）");
            g_need = 1; g_force_frames = 2;
            return;
        }
        g_ope_steps[se][2] = (int)(((c & 0xFFFFFFu) << 8) | ((uint32_t)g_ope_steps[se][2] & 0xFFu));
        ALOGI("vis edit 找色 第 %d 步 颜色=#%06X", se + 1, c & 0xFFFFFFu);
        g_vis_hex = 0; g_vis_hexmsg[0] = 0;
        g_need = 1; g_force_frames = 3;
    }
}
/* 视觉步参数层（整屏卡片；T3.2；T7.4 加持续查找）：找图 = 模板 / 区域 / 阈值 / 持续查找 / 超时；
 * 找色 = 模式 / 颜色 / 取点吸色 / 容差（单点）或 点集（多点）/ 持续查找 / 超时。
 * 编辑直接落在编辑层快照（g_ope_steps / g_ope_refs / g_ope_exprs；外层 [取消] 全丢、[完成] 才落表）。
 * [完成] 走同源 ope_step_check（不过 → 就地提示、层不关）；日志 `vis edit …`（spec §8）。 */
static void draw_vis_edit(void)
{
    ImDrawList *dl;
    ImVec2 wp, a, b;
    float ww, wh, x0, y0, cw, ry, ry2, bw2, by;
    int se, type;
    char lab[96], t[80];
    if (g_vis_num) { draw_num_edit(); return; }      /* 数字键盘子层（复用；[完成] 写回经 ne_field_set） */
    if (g_vis_tl) { draw_vis_tlist(); return; }
    if (g_vis_pl) { draw_vis_plist(); return; }
    if (g_vis_hex) { draw_vis_hex(); return; }
    se = g_ope_se;
    if (se < 0 || se >= g_ope_nsteps) {
        g_vis_ed = 0; g_vis_num = 0; g_vis_tl = 0; g_vis_pl = 0; g_vis_hex = 0;
        g_ope_se = -1;
        return;
    }
    type = g_ope_steps[se][0];
    if (type != OP_STEP_FINDIMAGE && type != OP_STEP_FINDCOLOR) {
        g_vis_ed = 0; g_vis_num = 0; g_vis_tl = 0; g_vis_pl = 0; g_vis_hex = 0;
        g_ope_se = -1;
        return;
    }
    dl = ImGui::GetWindowDrawList();
    wp = ImGui::GetWindowPos();
    ww = ImGui::GetWindowWidth();
    wh = ImGui::GetWindowHeight();
    a = ImVec2(wp.x + 12, wp.y + 12);
    b = ImVec2(wp.x + ww - 12, wp.y + wh - 12);
    dl->AddRectFilled(a, b, IM_COL32(255, 255, 255, 253), 14);
    dl->AddRect(a, b, IM_COL32(228, 228, 231, 255), 14, 0, 1.5f);
    x0 = a.x + 26; y0 = a.y + 24; cw = (b.x - x0) - 26;
    snprintf(t, sizeof t, "第 %d 步 · %s · 参数", se + 1, ope_tname(type));
    ImGui::SetCursorScreenPos(ImVec2(x0, y0));
    text_meta_s(t);
    ImGui::SetCursorScreenPos(ImVec2(x0, y0 + 40));
    text_meta_s(type == OP_STEP_FINDIMAGE ? "选模板 / 区域 / 阈值；成立/不成立在步骤行上编辑"
                                          : "选模式与颜色（或点集）；成立/不成立在步骤行上编辑");
    ry = y0 + 84;
    if (type == OP_STEP_FINDIMAGE) {
        snprintf(lab, sizeof lab, "模板：%s", g_ope_refs[se][0] ? g_ope_refs[se] : "未选");
        ImGui::SetCursorScreenPos(ImVec2(x0, ry));
        if (btn_light(lab, ImVec2(cw, 84))) {
            g_vis_tl = 1;
            g_need = 1; g_force_frames = 2;
            ALOGI("vis edit 模板列表开 第 %d 步", se + 1);
        }
        snprintf(lab, sizeof lab, "区域：%s", g_ope_exprs[se][0] ? g_ope_exprs[se] : "全屏");
        ImGui::SetCursorScreenPos(ImVec2(x0, ry + 96));
        if (btn_light(lab, ImVec2(cw, 84))) {
            g_ope_rl = se;                           /* 复用区域选择弹层（含「全屏」选项） */
            g_need = 1; g_force_frames = 2;
            ALOGI("vis edit 区域列表开 第 %d 步", se + 1);
        }
        snprintf(lab, sizeof lab, "阈值：%d（0..255）", g_ope_steps[se][1]);
        ImGui::SetCursorScreenPos(ImVec2(x0, ry + 192));
        if (btn_light(lab, ImVec2(cw, 84))) ne_open_vis_field(se, 0);
        ry2 = ry + 288;                          /* 持续查找行（T7.4）：阈值之后 */
    } else {
        snprintf(lab, sizeof lab, "模式：%s", g_ope_steps[se][1] == 1 ? "多点" : "单点");
        ImGui::SetCursorScreenPos(ImVec2(x0, ry));
        if (btn_light(lab, ImVec2(cw, 84))) {        /* 模式循环：单点 ↔ 多点（切换时清/补 a2 的语义字段） */
            if (g_ope_steps[se][1] == 1) {
                g_ope_steps[se][1] = 0;
                g_ope_refs[se][0] = 0;                 /* 单点：清点集名（残留会拒保存「单点模式不能带点集」；修复轮） */
                if (g_ope_steps[se][2] == 0) g_ope_steps[se][2] = 8;   /* 颜色 #000000、容差 8 */
            } else {
                g_ope_steps[se][1] = 1;
                g_ope_steps[se][2] = 0;                /* 多点：a2 必须 0（基准色/容差/点表在 .pts） */
            }
            g_need = 1; g_force_frames = 2;
            ALOGI("vis edit 找色 第 %d 步 模式=%s", se + 1, g_ope_steps[se][1] ? "多点" : "单点");
        }
        if (g_ope_steps[se][1] == 0) {
            snprintf(lab, sizeof lab, "颜色：#%06X", (unsigned)(((uint32_t)g_ope_steps[se][2] >> 8) & 0xFFFFFFu));
            ImGui::SetCursorScreenPos(ImVec2(x0, ry + 96));
            if (btn_light(lab, ImVec2(cw, 84))) {
                g_vis_hex = 1;
                snprintf(g_vis_hexbuf, sizeof g_vis_hexbuf, "%06x",
                         (unsigned)(((uint32_t)g_ope_steps[se][2] >> 8) & 0xFFFFFFu));
                g_vis_hexmsg[0] = 0; g_vis_hexup = 0;
                g_need = 1; g_force_frames = 2;
            }
            ImGui::SetCursorScreenPos(ImVec2(x0, ry + 192));
            if (btn_light("取点吸色（去屏幕点一下）", ImVec2(cw, 84))) {
                g_vis_pick_se = se;
                vis_cap_start(3);
            }
            snprintf(lab, sizeof lab, "容差：%d（0..255）", (int)((uint32_t)g_ope_steps[se][2] & 0xFFu));
            ImGui::SetCursorScreenPos(ImVec2(x0, ry + 288));
            if (btn_light(lab, ImVec2(cw, 84))) ne_open_vis_field(se, 0);
            ry2 = ry + 384;                      /* 持续查找行（T7.4）：容差之后 */
        } else {
            snprintf(lab, sizeof lab, "点集：%s", g_ope_refs[se][0] ? g_ope_refs[se] : "未选");
            ImGui::SetCursorScreenPos(ImVec2(x0, ry + 96));
            if (btn_light(lab, ImVec2(cw, 84))) {
                g_vis_pl = 1;
                g_need = 1; g_force_frames = 2;
                ALOGI("vis edit 点集列表开 第 %d 步", se + 1);
            }
            ry2 = ry + 192;                      /* 持续查找行（T7.4）：点集之后 */
        }
    }
    /* 持续查找（T7.4）：开/关 + 超时 ms 格 —— 关 = ms 0；开（ms>0）默认 5000；超时格走数字键盘（第 2 格）。 */
    snprintf(lab, sizeof lab, "持续查找：%s", g_ope_steps[se][5] > 0 ? "开" : "关");
    ImGui::SetCursorScreenPos(ImVec2(x0, ry2));
    if (btn_light(lab, ImVec2(cw, 84))) {
        if (g_ope_steps[se][5] > 0) {
            g_ope_steps[se][5] = 0;
            ALOGI("vis edit %s 第 %d 步 持续=关", ope_tname(type), se + 1);
        } else {
            g_ope_steps[se][5] = 5000;
            ALOGI("vis edit %s 第 %d 步 持续=开 超时=5000", ope_tname(type), se + 1);
        }
        g_need = 1; g_force_frames = 2;
    }
    snprintf(lab, sizeof lab, "超时：%d ms（0 = 单次）", g_ope_steps[se][5]);
    ImGui::SetCursorScreenPos(ImVec2(x0, ry2 + 96));
    if (btn_light(lab, ImVec2(cw, 84))) ne_open_vis_field(se, 1);
    /* 底部锚（M1 修复）：试一下 / 结果行 / 提示槽都锚到 [取消]/[完成] 行上方，矮屏不越界。 */
    by = b.y - 24.0f - 92.0f;
    float ry_btn = by - 12.0f - 20.0f - 12.0f - 92.0f;   /* 试一下按钮顶（92px） */
    float ry_res = by - 12.0f - 20.0f;                    /* 结果行顶（文本 ~20px） */
    /* 提示槽（固定占位：出现/消失不动下面；矮屏夹到按钮上方）—— T7.4：锚到持续查找两行之后 */
    if (g_vis_edmsg[0]) {
        float y_edmsg = ry2 + 192.0f;
        if (y_edmsg > ry_btn - 32.0f) y_edmsg = ry_btn - 32.0f;
        ImGui::SetCursorScreenPos(ImVec2(x0, y_edmsg));
        ImGui::TextColored(ImVec4(0.863f, 0.149f, 0.149f, 1.00f), "%s", g_vis_edmsg);
    }
    /* 试一下（Task 7.1）：当场执行一次查找 → 结果行 + 屏幕标记（位置见上方底部锚，M1）。 */
    ImGui::PushID(9300);
    ImGui::SetCursorScreenPos(ImVec2(x0, ry_btn));
    if (btn_blue("试一下", ImVec2(cw, 92))) vis_test_start_se(se);
    ImGui::PopID();
    /* 试查结果行（固定槽：出现/消失不动下面） */
    ImGui::SetCursorScreenPos(ImVec2(x0, ry_res));
    if (g_vis_test_msg[0]) {
        ImGui::TextColored(vis_test_msg_col(), "%s", g_vis_test_msg);
    } else {
        text_meta_s("试一下：当场查找一次，命中位置画到屏幕上");
    }
    /* 底：[取消] 丢弃 / [完成] 同源预检 → 关层（外层 [完成] 才落表） */
    bw2 = (cw - 12.0f) * 0.5f;
    by = b.y - 24.0f - 92.0f;
    ImGui::PushID(9200);
    ImGui::SetCursorScreenPos(ImVec2(x0, by));
    if (btn_light("取消", ImVec2(bw2, 92))) {
        ALOGI("vis edit 取消 第 %d 步", se + 1);
        g_vis_ed = 0; g_vis_num = 0; g_vis_tl = 0; g_vis_pl = 0; g_vis_hex = 0; g_vis_edmsg[0] = 0;
        g_ope_se = -1;
        g_need = 1; g_force_frames = 3;
    }
    ImGui::SetCursorScreenPos(ImVec2(x0 + bw2 + 12, by));
    if (btn_blue("完成", ImVec2(bw2, 92))) {
        char why[96];
        if (!ope_step_check(se, g_ope_steps[se], why, (int)sizeof why)) {
            snprintf(g_vis_edmsg, sizeof g_vis_edmsg, "%s", why);
            g_need = 1; g_force_frames = 2;
        } else {
            if (type == OP_STEP_FINDIMAGE)
                ALOGI("vis edit 找图 第 %d 步 模板=%s 区域=%s 阈值=%d", se + 1, g_ope_refs[se],
                      g_ope_exprs[se][0] ? g_ope_exprs[se] : "全屏", g_ope_steps[se][1]);
            else if (g_ope_steps[se][1] == 1)
                ALOGI("vis edit 找色 第 %d 步 模式=多点 点集=%s 区域=%s", se + 1, g_ope_refs[se],
                      g_ope_exprs[se][0] ? g_ope_exprs[se] : "全屏");
            else
                ALOGI("vis edit 找色 第 %d 步 模式=单点 颜色=#%06X 容差=%d 区域=%s", se + 1,
                      (unsigned)(((uint32_t)g_ope_steps[se][2] >> 8) & 0xFFFFFFu),
                      (int)((uint32_t)g_ope_steps[se][2] & 0xFFu),
                      g_ope_exprs[se][0] ? g_ope_exprs[se] : "全屏");
            g_vis_ed = 0; g_vis_num = 0; g_vis_tl = 0; g_vis_pl = 0; g_vis_hex = 0; g_vis_edmsg[0] = 0;
            g_ope_se = -1;
            g_need = 1; g_force_frames = 3;
        }
    }
    ImGui::PopID();
}

/* ---- 模板页（nav 6）：截帧 / 列表 / 删除 ---- */

/* 模板页（T3.2）：[截帧 · 存模板] / [截帧 · 做点集] + 两个列表（模板 / 点集，各带 [删除]）。
 * 列表读目录实时（同区域/操作列表「每帧重读」口径）；删除立即生效（引用它的步骤运行时报「模板不存在」）。 */
static void page_template(void)
{
    char tms[64][16], pss[64][16];
    char meta[40];
    int nt, np, i;
    nt = vis_list(".tmpl", tms, 64);
    np = vis_list(".pts", pss, 64);
    snprintf(meta, sizeof meta, "%d 模板 / %d 点集", nt, np);
    page_header("模板", meta);
    text_meta_w("模板 = 截帧框选一块图案（找图用）；点集 = 基准色 + 参考点（多点找色用）。存到 "
                "/data/local/vtouch-runtime/templates/，重启保留、方案内共享。");
    ImGui::Dummy(ImVec2(0, 6));
    {
        float gap = 12.0f;
        float bw = (ImGui::GetContentRegionAvail().x - gap) * 0.5f;
        if (btn_blue("截帧 · 存模板", ImVec2(bw, 84))) {
            g_vis_pick_se = -1;
            vis_cap_start(1);
        }
        ImGui::SameLine();
        if (btn_light("截帧 · 做点集", ImVec2(bw, 84))) {
            g_vis_pick_se = -1;
            vis_cap_start(2);
        }
    }
    ImGui::Dummy(ImVec2(0, 6));
    /* 试查（Task 7.1）结果行：固定槽（出现/消失不动下面列表） */
    if (g_vis_test_msg[0]) {
        ImGui::TextColored(vis_test_msg_col(), "%s", g_vis_test_msg);
    } else {
        text_meta_s("「试一下」：当场执行一次查找，结果在此显示、命中位置画到屏幕上");
    }
    ImGui::Dummy(ImVec2(0, 6));
    /* 列表自成一格可滚容器（与区域 / 操作列表同款；拖动滚动目标同走 SCR_LIST） */
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ZINC50);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 12));
    ImGui::BeginChild("##tmpls", ImVec2(0, 0), ImGuiChildFlags_None,
                      ImGuiWindowFlags_AlwaysVerticalScrollbar);
    pub_zone(g_zone_list);
    drag_scroll_for(SCR_LIST);
    text_meta_s("模板（.tmpl）");
    if (nt == 0) text_meta_w("还没有模板：点「截帧 · 存模板」开始");
    for (i = 0; i < nt; i++) {
        ImGui::PushID(9400 + i);
        {
            float dw = 150.0f, qw = 150.0f;          /* [试一下] / [删除] 宽 */
            float nw = ImGui::GetContentRegionAvail().x - dw - qw - 24.0f;
            if (nw < 120.0f) nw = 120.0f;
            btn_light(tms[i], ImVec2(nw, 76));       /* 只显示（不可点） */
            ImGui::SameLine();
            if (btn_blue("试一下", ImVec2(qw, 76))) vis_test_start_tmpl(tms[i], 0);
            ImGui::SameLine();
            if (btn_red("删除", ImVec2(dw, 76))) vis_del_file(tms[i], 0);
        }
        ImGui::PopID();
    }
    ImGui::Dummy(ImVec2(0, 8));
    text_meta_s("点集（.pts）");
    if (np == 0) text_meta_w("还没有点集：点「截帧 · 做点集」开始");
    for (i = 0; i < np; i++) {
        ImGui::PushID(9500 + i);
        {
            float dw = 150.0f, qw = 150.0f;          /* [试一下] / [删除] 宽 */
            float nw = ImGui::GetContentRegionAvail().x - dw - qw - 24.0f;
            if (nw < 120.0f) nw = 120.0f;
            btn_light(pss[i], ImVec2(nw, 76));       /* 只显示（不可点） */
            ImGui::SameLine();
            if (btn_blue("试一下", ImVec2(qw, 76))) vis_test_start_tmpl(pss[i], 1);
            ImGui::SameLine();
            if (btn_red("删除", ImVec2(dw, 76))) vis_del_file(pss[i], 1);
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
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
    /* 方案页子层（T2.1：名字键盘 / 删除确认）：整面盖住面板 —— 同区域改名 / 操作编辑层的口径 */
    const int scm_layer = (g_scm_kb || g_scm_del[0]) ? 1 : 0;
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
    g_zone_kb[0] = g_zone_kb[1] = g_zone_kb[2] = g_zone_kb[3] = 0;   /* 键盘区：只在键盘帧发布 */
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(PAD_X, PAD_Y));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(12, 10));
    build_titlebar(ww);
    if (g_min) {   /* 收起态：只有标题条；清掉侧栏/内容页实区，免上一帧残留命中 */
        g_zone_side[0] = g_zone_side[1] = g_zone_side[2] = g_zone_side[3] = 0;
        g_zone_sheet[0] = g_zone_sheet[1] = g_zone_sheet[2] = g_zone_sheet[3] = 0;
    } else if (g_name_i >= 0 || g_ope_i >= 0 || scm_layer) {
        /* 覆盖层（区域改名 / 操作编辑 / 方案名字键盘 / 方案删除确认）整面盖住：同样清底下实区 —— 不然按下可能命中上一帧残留 → 误滚 */
        g_zone_side[0] = g_zone_side[1] = g_zone_side[2] = g_zone_side[3] = 0;
        g_zone_sheet[0] = g_zone_sheet[1] = g_zone_sheet[2] = g_zone_sheet[3] = 0;
    }
    if (!g_min) ImGui::Dummy(ImVec2(0, COL_GAP - 10));
    if (!g_min && scm_layer && g_scm_kb) draw_scm_kb();     /* 方案名字键盘子层：本帧不画侧栏/内容页 */
    if (!g_min && scm_layer && !g_scm_kb) draw_scm_del();   /* 方案删除确认层（小确认层） */
    if (!g_min && !scm_layer && g_name_i >= 0) draw_name_edit();   /* 改名弹层：本帧不画侧栏/内容页 */
    /* 操作编辑层（T2.6/T2.4）不在这里画：编辑层开着时 build_panel 整个不跑（见 draw_frame），
     * 由 build_edit_layer 按整屏画（展开 = 整屏 sheet；收起 = 底部条）。 */
    if (!g_min && !scm_layer && g_name_i < 0 && g_ope_i < 0) build_sidebar();
    if (!g_min && g_sheet && !scm_layer && g_name_i < 0 && g_ope_i < 0) {
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
        else if (g_nav == 5) page_scheme();
        else if (g_nav == 6) page_template();        /* 模板/点集页（T3.2） */
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

/* 编辑层收起条（取点 / 手动共用渲染）：底部居中一条（几何见 ope_bar_rect，与吞触摸矩形同源）。
 * 取点收起 = 「点屏幕上目标位置 · 点这里取消」（spec §4.2 逐字）—— 点按 = 取消，走快照侧
 * down_edge 拦截（snapshot_touches：按整条命中、不喂 ImGui，取消更稳），这里只画。
 * 手动收起 = 「编辑中 · 点这里展开」（措辞自定）—— 条本体是按钮，点按 = 展开（状态全保留）。 */
static void draw_ope_bar(void)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    float x1, y1, x2, y2;
    const char *t = g_pick ? "点屏幕上目标位置 · 点这里取消" : "编辑中 · 点这里展开";
    ope_bar_rect(&x1, &y1, &x2, &y2);
    dl->AddRectFilled(ImVec2(x1, y1), ImVec2(x2, y2), IM_COL32(24, 24, 27, 235), 14.0f);
    dl->AddRect(ImVec2(x1, y1), ImVec2(x2, y2), IM_COL32(82, 82, 91, 255), 14.0f, 0, 1.5f);
    {
        ImVec2 ts = ImGui::CalcTextSize(t);
        dl->AddText(ImVec2((x1 + x2 - ts.x) * 0.5f, (y1 + y2 - ts.y) * 0.5f),
                    IM_COL32(255, 255, 255, 255), t);
    }
    if (!g_pick) {
        ImGui::SetCursorScreenPos(ImVec2(x1, y1));
        if (ImGui::InvisibleButton("##opebar", ImVec2(x2 - x1, y2 - y1))) {
            g_ope_coll = 0;
            g_need = 1; g_force_frames = 2;
            ALOGI("op edit 展开（收起条点按）");
        }
    }
}

/* 编辑层窗口（T2.4）：编辑层及其全部子层按**当前屏整屏**绘制（不再局限面板窗口）。
 * 面板主窗口在编辑层开着时不画（build_panel 整个不跑，见 draw_frame）：展开时整屏 sheet 盖住一切；
 * 收起时只剩底部条，其余整屏看游戏（取点收起时 = 取点落点）。
 * 窗口本身整屏、背景透明（sheet / 条自画底色）；编辑层关后本窗不再 Begin（ImGui 自动失活，
 * 下次开层原位复活，无 ini 残留 —— NoSavedSettings）。 */
static void build_edit_layer(void)
{
    float sw = (float)g_scr_w, sh = (float)g_scr_h;
    if (sw <= 0 || sh <= 0) return;
    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(sw, sh), ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));   /* 整屏透明：sheet/条之外透出游戏 */
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0));     /* 窗口描边也隐掉（否则整屏一圈灰线） */
    ImGui::Begin("##opedit", 0,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar |
                 ImGuiWindowFlags_NoScrollWithMouse);
    /* 面板窗口本帧不画：四个实区清掉（免上一帧残留命中把滚动错路由）；编辑层自己的列表 child
     * 会按需重发布（pub_zone）。 */
    g_zone_title[0] = g_zone_title[1] = g_zone_title[2] = g_zone_title[3] = 0;
    g_zone_side[0] = g_zone_side[1] = g_zone_side[2] = g_zone_side[3] = 0;
    g_zone_sheet[0] = g_zone_sheet[1] = g_zone_sheet[2] = g_zone_sheet[3] = 0;
    g_zone_list[0] = g_zone_list[1] = g_zone_list[2] = g_zone_list[3] = 0;
    g_zone_kb[0] = g_zone_kb[1] = g_zone_kb[2] = g_zone_kb[3] = 0;   /* 键盘区：只在键盘帧发布 */
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(PAD_X, PAD_Y));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(12, 10));
    if (g_ope_coll) draw_ope_bar();          /* 收起条（取点 / 手动共用渲染） */
    else draw_op_edit();                     /* 展开：整屏编辑层（子层自盖） */
    ImGui::PopStyleVar(2);
    ImGui::End();
    ImGui::PopStyleColor(2);
    if (g_scr_target == SCR_NONE) g_scroll_acc = 0;   /* 与 build_panel 同款：非滚动容器按下丢弃累积量 */
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
    if (g_vis_cap) build_vis_cap();         /* 采集覆盖层整屏（T3.2）：模板/点集/吸色 —— 最高优先 */
    else if (g_ope_i >= 0) build_edit_layer();   /* 编辑层整屏（T2.4）：面板主窗口本帧不画 */
    else build_panel();
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
        /* 取点自动收起的 20s 兜底（T2.4，面板自带、与核心同长）：核心侧超时是惰性的（下次按下才判），
         * 面板这份保证到点就弹回 —— 超时 = 撤单 + 弹回编辑层（参数层保持）。放在快照前：
         * 超时后这一帧的 in_p / 吞触摸矩形立刻回到编辑层展开态。 */
        if (g_pick && g_pick_t0 && now_ms() - g_pick_t0 > 20000) {
            g_pick = 0; g_pick_t0 = 0;
            g_ope_coll = 0;
            vtouch_pick_cancel();
            g_need = 1; g_force_frames = 2;
            ALOGI("取点 超时（面板 20s 兜底）→ 弹回编辑层");
        }
        snapshot_touches();
        region_rot_step();        /* 区域跟随旋转：每帧推进一条（编辑邮箱单槽，必须一条一拍） */
        ops_run_watch();          /* 操作运行状态变了 → 请求重画（「运行中 · 第 k/n 步」实时读核心） */
        vis_cap_tick();           /* 视觉采集（T3.2）：收帧/失败/超时（等帧期间保持重画） */
        vis_test_tick();          /* 试查（Task 7.1）：收结果/超时（≤10ms 轮询 test_res_seq） */
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
                /* 试查标记（Task 7.1）同款：过期后补一帧擦掉（否则方框留在屏上） */
                if (!ov_active && g_vis_testmk_t && now_ms() - g_vis_testmk_t < VIS_TEST_MARK_MS) ov_active = 1;
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
    g_pan_x = (float)(w - panel_w() - 40); if (g_pan_x < 0) g_pan_x = 0;
    g_pan_y = 200;
    {   /* 矮屏（横屏 / 小屏）：默认顶距放不下整窗时上移，尽量多留高度（保底 16） */
        float ny = (float)h - panel_h() - WIN_BOTTOM_PAD;
        if (ny < 16) ny = 16;
        if (g_pan_y > ny) g_pan_y = ny;
    }
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
    {   /* v4 方案接管（spec §2）：兜底迁移 → 强制同步（保证 live == schemes/<current>）→ 照旧加载。
         * 同步失败只告警：按现有 live 文件继续（不阻塞启动、不影响注入），下次启动兜底再试。 */
        char scm[16];
        scheme_migrate(scm);
        if (scm[0] && scheme_sync_live(scm) != 0)
            ALOGW("方案 同步失败 %s: %s", scm, strerror(errno));
    }
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
