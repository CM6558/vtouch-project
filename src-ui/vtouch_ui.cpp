/* vtouch_ui.cpp — 单进程 ImGui 运行悬浮窗（缩编 v0.5；「方案 B」单全屏图层）。
 * 职责：运行时观察（状态/日志/屏上描边·触发闪）+ 控制（运行/停止、方案切换/重载、显隐、关 UI、退出）+
 *   区域调整块（框选/拖动缩放/卡四键/触发行，「当场就能调」）+ 热生效-A（外部改盘 mtime → 自动重放）。
 * 编辑（操作编辑器 / 方案 CRUD / 模板 / 说明）已随缩编 v0.5 归编辑器 app（com.vtouch.editor）。
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
 * v11 起语义 = **字符数上限**；一切名字缓冲宽度用 VT_NAME_BUF。 */
#define REGION_ID_MAX 15
/* 名字规则（契约 v11，与核心 vt_id_ok / op_var_name_ok 同口径；面板只作预检，核心为最终门）。 */
#define VT_NAME_CHARS   15                  /* 名字字符数上限（= 核心 VT_NAME_CHARS） */
#define VT_NAME_BUF     48                  /* 名字缓冲字节数（= 核心 VT_NAME_BUF） */

/* 码点可见性（v11 面板镜像，与核心 vt_name_cp_visible 同表）。 */
static int ui_name_cp_vis(unsigned cp)
{
    if (cp <= 0x7F) return 0;
    if (cp == 0x00A0 || cp == 0x00AD || cp == 0x061C || cp == 0x1680 || cp == 0xFEFF) return 0;
    if (cp >= 0x0080 && cp <= 0x009F) return 0;
    if (cp >= 0x2000 && cp <= 0x200A) return 0;
    if (cp == 0x2028 || cp == 0x2029 || cp == 0x202F || cp == 0x205F || cp == 0x3000) return 0;
    if (cp >= 0x200B && cp <= 0x200F) return 0;
    if (cp >= 0x2060 && cp <= 0x2064) return 0;
    if (cp >= 0x2066 && cp <= 0x2069) return 0;
    if (cp >= 0xFE00 && cp <= 0xFE0F) return 0;
    return 1;
}
/* 名字合法性（v11）：可见 UTF-8 字符 1..15 个、字节 ≤ VT_NAME_BUF-1；ASCII 仅 [0-9A-Za-z_-]（裸 `-` 拒收）、
 * 非 ASCII 须合法 UTF-8 且非空白/控制/格式字符。var_mode=1 = 变量名口径（首字符不可数字/`-`、不含 `-`）。
 * 返回 0 ok / 1 空 / 2 超长 / 3 非法。 */
static int ui_name_ok(const char *s, int var_mode)
{
    size_t n = strlen(s), i = 0;
    int chars = 0;
    if (n < 1) return 1;
    if (n > VT_NAME_BUF - 1) return 2;
    if (!var_mode && n == 1 && s[0] == '-') return 3;
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x80) {
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '_' || (!var_mode && c == '-'))) return 3;
            if (var_mode && chars == 0 && !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'))
                return 3;
            i++;
        } else {
            unsigned b0 = c, cp = 0;
            int len, k;
            if ((b0 & 0xE0) == 0xC0) { len = 2; cp = b0 & 0x1Fu; }
            else if ((b0 & 0xF0) == 0xE0) { len = 3; cp = b0 & 0x0Fu; }
            else if ((b0 & 0xF8) == 0xF0) { len = 4; cp = b0 & 0x07u; }
            else return 3;
            if (i + (size_t)len > n) return 3;
            for (k = 1; k < len; k++) {
                unsigned char bc = (unsigned char)s[i + (size_t)k];
                if ((bc & 0xC0) != 0x80) return 3;
                cp = (cp << 6) | (bc & 0x3Fu);
            }
            if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) ||
                (len == 4 && (cp < 0x10000 || cp > 0x10FFFF)) ||
                (cp >= 0xD800 && cp <= 0xDFFF) || !ui_name_cp_vis(cp)) return 3;
            i += (size_t)len;
        }
        if (++chars > VT_NAME_CHARS) return 2;
    }
    return 0;
}
/* 计算步表达式上限（= 核心 src/vt_internal.h 的 VT_EXPR_MAX；同上独立定义）——
 * vtouch_op_put 的每步表达式表按它定宽。 */
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
int vtouch_region_clear(void);
int vtouch_region_count(void);
int vtouch_region_add(const char *id, int type, int a1, int a2, int a3, int a4, int enabled);
int vtouch_region_add_confirm(const char *id, int type, int a1, int a2, int a3, int a4, int enabled);   /* 转屏重算批：已存在 id 也等核心吃掉 */
int vtouch_region_del(const char *id);                      /* 单条删（不整表重写） */
int vtouch_region_rename(const char *old_id, const char *new_id);   /* 原地改名（位置不变） */
int vtouch_region_mark(int i);
int vtouch_get_region(int i, char *id, int idn, int *type,
                      int *a1, int *a2, int *a3, int *a4, int *enabled);
/* 接核心时代新增：把面板矩形推给核心（核心据此吞触摸）。入参是**当前屏坐标**，
 * 逆变换回竖屏逻辑坐标由胶水层做（见 src-ui/ui_glue.c）。单跑模式（ui_stubs.c）里是空实现。 */
void vtouch_ui_publish_rect(int visible, int rot, int scr_w, int scr_h, int x1, int y1, int x2, int y2);
/* 操作 / 绑定：胶水出口，原型逐字（定义见 src-ui/ui_glue.c，单跑模式见 src-ui/ui_stubs.c）。
 * 面板运行时用「操作」那一组（计数/读/运行/停止/状态）；绑定读/写（trig/bind/kind）供区域卡片与
 * 触发行；v5 表达式校验（expr_check）。取点 / 视觉旁路已随缩编 v0.5 删除。 */
int  vtouch_op_count(void);
/* v6D 提示通道（契约 v14）：取序号 / 认领一条新提示（文本 + 时长 ms）。 */
unsigned vtouch_toast_seq(void);
int  vtouch_toast_take(char *buf, int cap, int *ms);
int  vtouch_get_op(int i, char *name, int n, int *steps, char *gate, int gn, int *autoff);
int  vtouch_get_op_step(int i, int s, int *type, int *a1, int *a2, int *a3, int *a4, int *ms, char *ref, int refn,
                        int *j1, int *j2, char *expr, int exprn);   /* j1/j2 = 条件步跳转目标（成立/不成立侧；可 NULL）；expr = 计算步表达式（v5；可 NULL） */
int  vtouch_op_put(const char *name, const char *gate, int autoff, const int *steps8,
                   const char (*refs)[VT_NAME_BUF], const char (*exprs)[VT_EXPR_MAX + 1], int nsteps, int *out_err);   /* steps8 = flat 8/步（t,a1..a4,ms,j1,j2）；refs/exprs 可 NULL = 全空；每步空串 = 无；out_err 可 NULL */
int  vtouch_op_del(const char *name);
int vtouch_op_clear(void);
int  vtouch_op_run(const char *name);
void vtouch_op_stop(void);
int  vtouch_op_status(int *run_i, int *run_step, int *run_state);
int  vtouch_expr_check(const char *s, const char (*names)[VT_NAME_BUF], int nnames, char *why, int whycap);   /* v5：计算步表达式校验（转发核心 vt_expr_check；v10 起带自定义变量名表；0 = 合法、非 0 = why 填短中文原因） */
int  vtouch_region_kind_get(int i);
int  vtouch_region_toggle(int i);
int  vtouch_region_trig(int i, char *op, int n, int *ev);
int  vtouch_region_bind(const char *id, const char *opname, int ev);   /* 触发绑定（opname "-"/空 = 解除） */
int  vtouch_region_kind(const char *id, int kind);                     /* 开关型（0=普通 1=开关型） */
/* 视觉抓帧间隔（2026-10-05f；ui.conf 载入/改值时同步给抓帧线程）——面板侧其余视觉旁路随缩编 v0.5 删除。 */
void vtouch_vis_cap_interval_set(int ms);
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
static int g_cap_ms = 0;                 /* 抓帧间隔 ms（0 = 全速；2026-10-05f；抓帧线程读） */
static int g_uiconf_loaded = 0;
#define UI_CONF_PATH "/data/local/vtouch-runtime/ui.conf"
/* ui.conf：新格式 key=value 逐行（mark / cap_ms）；旧格式（单值 0/1 = mark）照读兼容。
 * 载入后把 cap_ms 同步给 glue（抓帧线程经 JNI 每轮取）。 */
static void ui_conf_load_once(void)
{
    FILE *f;
    const char *e;
    if (g_uiconf_loaded) return;
    g_uiconf_loaded = 1;
    e = getenv("VTOUCH_UI_MARK");
    if (e && (e[0] == '1' || e[0] == 'o' || e[0] == 'O')) g_show_mark = 1;
    f = fopen(UI_CONF_PATH, "r");
    if (f) {
        char line[64];
        if (fgets(line, sizeof line, f)) {
            if (strchr(line, '=') == NULL) {                 /* 旧格式：单值 = mark */
                if (line[0] == '1' || line[0] == 'o' || line[0] == 'O') g_show_mark = 1;
            } else {
                do {
                    char *eq = strchr(line, '=');
                    if (eq) {
                        *eq = 0;
                        if (!strcmp(line, "mark")) g_show_mark = (eq[1] == '1');
                        else if (!strcmp(line, "cap_ms")) {
                            int v = atoi(eq + 1);
                            if (v < 0) v = 0;
                            if (v > 2000) v = 2000;
                            g_cap_ms = v;
                        }
                    }
                } while (fgets(line, sizeof line, f));
            }
        }
        fclose(f);
    }
    vtouch_vis_cap_interval_set(g_cap_ms);                   /* 同步给抓帧线程（2026-10-05f） */
}
static void ui_conf_save(void)
{
    FILE *f = fopen(UI_CONF_PATH, "w");
    if (!f) return;
    fprintf(f, "mark=%d\ncap_ms=%d\n", g_show_mark, g_cap_ms);
    fclose(f);
}
static int g_prev_on[64];
static int g_mdown = 0, g_mslot = -1, g_mup_pend = 0;
static float g_mx = -1, g_my = -1;
static long g_mdown_t = 0;
static int g_drag = 0;                 /* 标题栏几何拖拽（快照侧直驱，不走 ImGui 拖拽机） */
static float g_drag_ox = 0, g_drag_oy = 0;
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
 * 两块文件、两条时间线 —— 一块写失败不该把另一块的改动一起卡住。表变更只置位，
 * 实际写盘同样在渲染线程（唯一写者，节奏照 g_save_pending 那套）。 */
static volatile int g_ops_save_pending = 0;
static long g_ops_save_retry_t = 0;
/* 面板状态里引用了「已不存在的区域 id」要清理（面板自身改表：单条删/改名是面板自带能力，
 * WS 命令族只有 clear/list/add）：WS 线程只置请求，
 * 真正的清理放渲染线程做 —— g_hidden/g_sel_id 归它管，跨线程改同一份数组才是新问题。 */
static volatile int g_prune_req = 0;
static void ui_region_changed(void) { g_prune_req = 1; g_need = 1; g_force_frames = 2; g_save_pending = 1; }
static void hr_files_note(void);   /* T8 热生效-A：面板自写后刷新轮询基线（定义见「方案切换」段后） */

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
struct rr_snap { char id[VT_NAME_BUF]; int type, a1, a2, a3, a4, en; };
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
    char id[VT_NAME_BUF];
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
            if (vtouch_region_add_confirm(g_rr_snap[g_rr_i].id, g_rr_snap[g_rr_i].type, n1, n2, n3, n4,
                                          g_rr_snap[g_rr_i].en) != 0) g_rr_fail++;   /* 评审修复：批必须条条落地 */
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
static char g_flash_id[VT_NAME_BUF] = {0};
static long g_flash_t = 0;
struct Ring { int on, x, y; long t; };
static Ring g_rings[8];
static int g_ring_i = 0;
static char g_ex_id[VT_NAME_BUF] = {0};
static long g_ex_t = 0;
static int g_ex_enter = 1;
static char g_evlog[8][96];
static int g_evlog_n = 0;
static double g_frame_ms = 16.0;

/* 可视化编辑状态：框选新增 / 选中拖改 / UI 侧显隐（core 表不动，只管画不画） */
static int g_cap_mode = 0;            /* 0 无 1 矩形框选 2 圆形框选 */
static int g_cap_slot = -1, g_cap_x0 = 0, g_cap_y0 = 0, g_cap_x1 = 0, g_cap_y1 = 0;
static char g_sel_id[VT_NAME_BUF] = {0};
static int g_edit_slot = -1, g_edit_kind = 0;  /* 0 无 1 移动 2 缩放 */
static int g_edit_corner = 0;                 /* 缩放：矩形角 0..3（左上/右上/左下/右下） */
static int g_edit_dx = 0, g_edit_dy = 0, g_edit_e[4];
static long g_edit_last = 0;
static char g_hidden[32][VT_NAME_BUF];
static int g_nhide = 0;

/* 区域改名：id 就是脚本监听的键。改名入口已归编辑器 app（缩编 v0.5），面板只按 id 显示/监听。 */
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
            memmove(g_hidden[i], g_hidden[i + 1], (size_t)(g_nhide - i - 1) * VT_NAME_BUF);
            g_nhide--;
            save_regions();
            return;
        }
    }
    if (g_nhide < 32) { snprintf(g_hidden[g_nhide++], VT_NAME_BUF, "%s", id); save_regions(); }
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
        char id[VT_NAME_BUF]; int t, a1, a2, a3, a4, en;
        char op[VT_NAME_BUF]; int ev, kd;
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
    hr_files_note();         /* T8 自写豁免：刷新热生效轮询基线 */
    return 0;
}
/* 核心表里有没有这个 id（面板只读区 A 的现役表；启动回灌「只补缺」靠它）。 */
static int region_exists(const char *id)
{
    int i, n = vtouch_region_count();
    for (i = 0; i < n; i++) {
        char cur[VT_NAME_BUF]; int t, a1, a2, a3, a4, en;
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
        char id[VT_NAME_BUF], op[VT_NAME_BUF], tms[16]; int t, a1, a2, a3, a4, en, fr, fw, fh, ev, kd;
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
        } else if (sscanf(line, "region %47s %d %d %d %d %d %d", id, &t, &a1, &a2, &a3, &a4, &en) == 7) {
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
        } else if (sscanf(line, "hide %47s", id) == 1) {
            if (g_nhide < 32 && !is_hidden(id)) snprintf(g_hidden[g_nhide++], VT_NAME_BUF, "%s", id);
        } else if (sscanf(line, "bind %47s %47s %15s", id, op, tms) == 3) {
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
        } else if (sscanf(line, "kind %47s %d", id, &kd) == 2) {
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
#define OP_STEP_CALC        9          /* 计算（v10；a1 = 变量索引 0..15、ref = 变量名、expr = 表达式） */
#define OP_STEP_FINDIMAGE  10          /* 找图（T3.2 v8；ref=模板名、expr=区域名（空=全屏）、a1=阈值 0..255、
                                        * a2=0、a3/a4=不成立/成立档、j1/j2=该侧目标） */
#define OP_STEP_FINDCOLOR  11          /* 找色（T3.2 v8；a1=模式 0 单点/1 多点；ref=点集名（多点必填/单点必空）；
                                        * expr=区域名；单点 a2=(颜色<<8)|容差、多点 a2=0；a3/a4/j1/j2 同上） */
#define OP_COND_ABORT       0          /* 档位：中止（条件步成立/不成立侧共用；不成立侧默认） */
#define OP_COND_SKIP        1          /* 档位：跳过下一步 */
#define OP_COND_CONT        2          /* 档位：继续下一步（成立侧默认） */
#define OP_COND_JUMP        3          /* 档位：跳到…（目标 = 该侧 j1/j2：0 = 结束；跑前守卫兜底） */
#define OP_VAR_TDX (-1)                /* 变量编码（v10）：-1..-5 = 触发按下x / 触发按下y / 触发弹起x / 触发弹起y / 触发时长 */
#define OP_VAR_TDY (-2)
#define OP_VAR_TUX (-3)
#define OP_VAR_TUY (-4)
#define OP_VAR_TMS (-5)
#define OP_VAR_FX  (-6)                /* fx/fy：最近一次找图/找色命中坐标（视觉步改写；起跑清零） */
#define OP_VAR_FY  (-7)
#define OP_VAR_RET1 (-8)               /* 退役槽编码（v5 的 r3/r4）：悬空 → 结果无值（v4 字段重映射失败时原样保留） */
#define OP_VAR_RET2 (-9)
#define OP_VAR_V0  (-10)               /* 自定义变量索引 0（编码 = -10 - 索引；索引 0..15 → -10..-25） */
#define OP_VAR_V15 (-25)
#define OP_VAR_IDX_N 16                /* 自定义变量表条数（同核心 OP_VAR_IDX_N） */
#define OP_VAR_NAME_MAX VT_NAME_CHARS  /* 自定义变量名字符数上限（v11：UTF-8 可见字符，首字符不可数字/`-`） */

/* ---- 操作表落盘（ops.conf，T2.7；v2 T3.3；v3 T2.1；v4 T3.1 v5；v5 T7.5 v10）------------------------------------
 * 格式（docs/EDITOR_V2_PLAN.md §Task 7.5 + OPS_PLAN_V5.md §3，逐字）：`#vtouch-ops v5` 起头；一条操作 = op 行 + N 条 step 行 ——
 *   op <名> gate <门控区域id|-> autooff <0|1>
 *   step <type> <a1> <a2> <a3> <a4> <ms> <ref> <j1> <j2> <expr>
 * ref / expr 空写 `-`（读回还原空串）；变量照写负数（如 -1，含 -10..-25）。**v5 与 v4 的行格式相同**
 * （step 10 字段不变）—— 差别只在语义：计算步（t=9）ref = 变量名、a1 = 变量索引 0..15（v4 是槽号 1..4、
 * ref 空）；字段负数编码扩到 -25（-6/-7 = fx/fy、-8/-9 = 退役槽、-10..-25 = 自定义变量）。
 * 读端兼容 v1（6 字段行）/ v2（7 字段行）/ v3（9 字段行）/ v4：缺省 j1=j2=0、expr 空；**类型感知翻译**：
 * 旧行的条件步（t=6/7）成立档 a4 缺省 → OP_COND_CONT（继续下一步，老文件里 a4=0 只是「没有该字段」）；
 * 非条件步照读——尤其滑动步 a3/a4 = 终点坐标，绝不能动。v4 的 expr 可含空白（照写）⇒ 读端取 j2 之后的
 * 行尾整段（`-` = 空）。**v1..v4 → v5 翻译矩阵**（ops_translate_old，逐条记录在结算前跑一次）：
 *   · v4 计算步（a1=1..4 槽号、ref 空）→ 名字 `r<k>`；索引按步序分配（同名复用）；a1/ref 改写；
 *   · v4 字段 -6..-9 → 该操作存在对应 `r<k>` 计算步则重映射到其变量索引（-10-索引）；否则原样保留
 *     （-6/-7 原样 = fx/fy 同值语义；-8/-9 悬空 = 结果无值）。
 * 保存：.tmp + rename（同 save_regions，掉电不会留半截文件）；失败挂 g_ops_save_pending、
 * 1s 后退避重试（节奏同 save_failed）；成功一行「ops.conf 已存 N 条」。
 * 加载：版本门（v1/v2/v3/v4 兼容读入 + 翻译 / v5 本格式；其余 = 整份跳过 + 改写当前表，同 load_regions 的丢弃清空口径）；
 * 只补缺（核心表已有同名 → 跳过，不覆盖现役定义）；一条坏记录只警告并继续，不带走全表。 */
#define OPS_CONF_VER  5
#define OPS_CONF_FILE REGION_CONF_DIR "/ops.conf"
#define OPS_MAX_STEPS 512       /* 同核心 MAX_STEPS（v13 起 512；面板不 include 核心头，独立定义） */
#define OPS_EXPR_MAX  63        /* 计算步表达式上限（同核心 VT_EXPR_MAX；conf 第 10 字段按它定宽） */
static_assert(OPS_EXPR_MAX == VT_EXPR_MAX, "同核心 VT_EXPR_MAX");

/* 核心操作表里有没有这个名字（启动回灌「只补缺」靠它；与 region_exists 同款）。 */
static int op_exists(const char *name)
{
    int i, n = vtouch_op_count();
    for (i = 0; i < n; i++) {
        char cur[VT_NAME_BUF];
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
        char name[VT_NAME_BUF], gate[VT_NAME_BUF];
        int steps = 0, autoff = 0, s;
        if (vtouch_get_op(i, name, sizeof name, &steps, gate, sizeof gate, &autoff) != 0) continue;
        /* gate 空串 = 无门控 → 按 spec 写占位符 `-`（读回时还原空串）；悬空 id 原样写（核心允悬空） */
        fprintf(f, "op %s gate %s autooff %d\n", name, gate[0] ? gate : "-", autoff ? 1 : 0);
        for (s = 0; s < steps; s++) {
            int t, a1, a2, a3, a4, ms, j1, j2;
            char ref[VT_NAME_BUF], expr[OPS_EXPR_MAX + 1];
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
    hr_files_note();             /* T8 自写豁免：刷新热生效轮询基线 */
    return 0;
}
/* 该字段允不允许变量引用（spec §2.1）：点按/滑动/等待的时长；点按/滑动/按下/区域判断的坐标。
 * v2 里凡存在的数值字段都可变量 —— 仍按类型/下标写死，防以后加字段时悄悄放行。
 * （缩编 v0.5：定义从编辑层上移——旧表翻译 ops_translate_old 还在用。） */
static int ope_var_ok(int type, int idx)
{
    if (idx == 6 || idx == 7) return type == OP_STEP_SWIPE;   /* v2.1：滑动两段停顿也可用变量 */
    if (idx == 5) return type == OP_STEP_TAP || type == OP_STEP_SWIPE || type == OP_STEP_WAIT;
    if (idx >= 1 && idx <= 4)
        return type == OP_STEP_TAP || type == OP_STEP_SWIPE || type == OP_STEP_DOWN || type == OP_STEP_COND_REGION;
    return 0;
}

/* v1..v4 → v5 翻译（spec EDITOR_V2 §Task 7.5「读端兼容」矩阵；一条记录一份，结算前调用一次）：
 *   · 计算步（t=9）：名字 = ref 非空 ? ref : "r<a1>"（v4 的 a1=1..4 是槽号）；索引按步序分配（同名复用）；
 *     a1 改写为变量索引、ref 改写为名字。
 *   · 字段 -6..-9：该操作存在对应 r<k> 变量 → 重映射到其索引（编码 -10-索引）；否则原样保留
 *     （-6/-7 原样 = fx/fy 同值语义；-8/-9 悬空 = 结果无值）。
 *   只翻「允许变量」的字段（点按/滑动/按下/区域判断的 a1..a4 + 点按/滑动/等待的 ms）—— 找色的 a2
 *   是打包色值，可为任意 32 位负值，绝不参与翻译。 */
static void ops_translate_old(int *flat, char (*refs)[VT_NAME_BUF], int nsteps)
{
    char names[OP_VAR_IDX_N][VT_NAME_BUF];
    int nv = 0, i, k, c;
    for (i = 0; i < nsteps; i++) {                       /* 1) 计算步：定名 + 分配索引（按步序；同名复用） */
        char vn[VT_NAME_BUF];
        if (flat[i * 8] != OP_STEP_CALC) continue;
        if (refs[i][0]) snprintf(vn, sizeof vn, "%s", refs[i]);
        else if (flat[i * 8 + 1] >= 1 && flat[i * 8 + 1] <= 4) snprintf(vn, sizeof vn, "r%d", flat[i * 8 + 1]);
        else continue;                                   /* 既无名字又无合法槽号：留给核心拒（坏记录） */
        for (k = 0; k < nv; k++) if (!strcmp(names[k], vn)) break;
        if (k == nv) {
            if (nv >= OP_VAR_IDX_N) continue;            /* 表满：留给核心拒（坏记录） */
            snprintf(names[nv], sizeof names[nv], "%s", vn);
            nv++;
        }
        flat[i * 8 + 1] = k;                             /* a1 = 变量索引 */
        snprintf(refs[i], VT_NAME_BUF, "%s", vn);        /* ref = 变量名 */
    }
    for (i = 0; i < nsteps; i++) {                       /* 2) 字段 -6..-9：有对应 r<k> 变量 → 重映射 */
        for (c = 1; c <= 5; c++) {
            int v = flat[i * 8 + c];
            if (v >= -9 && v <= -6 && ope_var_ok(flat[i * 8], c)) {
                char rn[8];
                snprintf(rn, sizeof rn, "r%d", -v - 5);  /* -6→r1 … -9→r4 */
                for (k = 0; k < nv; k++) if (!strcmp(names[k], rn)) break;
                if (k < nv) flat[i * 8 + c] = -10 - k;   /* 重映射到该变量索引 */
            }
        }
    }
}

/* 结算一条从文件读到的记录（下一条 op 行 / EOF 时调用）。返回 0=补入 / 1=跳过（核心已有）/
 * -1=坏记录（已警告）。一条坏记录不影响后面的记录。 */
static int ops_load_put(const char *name, const char *gate, int autoff,
                        const int *flat, const char (*refs)[VT_NAME_BUF],
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
    char nm[VT_NAME_BUF] = {0};
    char gt[VT_NAME_BUF] = {0};
    int  ao = 0, nst = 0, bad = 0;
    int  flat[OPS_MAX_STEPS * 8];
    char refs[OPS_MAX_STEPS][VT_NAME_BUF];   /* v3 每步 ref（旧版行读入时缺省空） */
    char exprs[OPS_MAX_STEPS][OPS_EXPR_MAX + 1];   /* v4 每步表达式（v1/v2/v3 行缺省空；`-` = 空） */
    FILE *f = fopen(OPS_CONF_FILE, "r");
    if (!f) return;          /* 没有文件 = 没有历史操作（首次运行），什么都不做、也不写盘 */
    /* 版本门：v1 / v2 / v3 / v4（兼容读入 + v5 翻译，spec §7 / EDITOR_V2 §Task 7.5）与 v5（本格式）都认；
     * 无版本行 / 其他版本 = 旧版本残留 → 整份丢弃；照 regions.conf 口径改写当前表 */
    if (!fgets(line, sizeof line, f) || sscanf(line, "#vtouch-ops v%d", &ver) != 1 ||
        (ver != 1 && ver != 2 && ver != 3 && ver != 4 && ver != OPS_CONF_VER)) {
        fclose(f);
        ALOGI("ops.conf 旧格式/版本不符 → 丢弃清空");
        save_ops();
        return;
    }
    while (fgets(line, sizeof line, f)) {
        char a[VT_NAME_BUF], b[VT_NAME_BUF];
        int ao2 = 0, t, a1, a2, a3, a4, ms;
        int pn = sscanf(line, "op %47s gate %47s autooff %d", a, b, &ao2);
        if (pn >= 1) {                    /* op 行（字段不全会只匹配 1/2 个）—— 先结算上一条 */
            if (nm[0]) {
                nrec++;
                if (ver < OPS_CONF_VER) ops_translate_old(flat, refs, nst);   /* v1..v4 翻译（矩阵见上） */
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
            char rf[VT_NAME_BUF];
            int sn, j1 = 0, j2 = 0, pos = 0;
            rf[0] = 0;                     /* ref 缺省空（v1 6 字段行） */
            /* v4 行 10 字段：step <t> <a1> <a2> <a3> <a4> <ms> <ref> <j1> <j2> <expr>；v3 = 9、v2 = 7、v1 = 6 ——
             * 短行 sscanf 提前收工（sn = 实际匹配数），缺的 ref/j1/j2/expr 照上面的缺省留空 / 0。
             * %n 记 j2 之后的位置：其后还有内容 = v4 的 expr 段（v1/v2/v3 行没有；expr 可含空白 → 取行尾整段） */
            sn = sscanf(line, "step %d %d %d %d %d %d %47s %d %d %n", &t, &a1, &a2, &a3, &a4, &ms, rf, &j1, &j2, &pos);
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
        if (ver < OPS_CONF_VER) ops_translate_old(flat, refs, nst);   /* v1..v4 翻译（矩阵见上） */
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

/* 方案名合法性（用户输入口；与区域 id / 操作名同一把尺子 vt_id_ok）：v11 名字规则（UTF-8 可见字符
 * 1..15、字节 ≤47）、裸 `-` 除外。
 * 0 ok / 1 空 / 2 超长 / 3 非法字符（含裸 `-`；`.` 等一律非法，天然排除 `.`/`..` 目录名）。
 * 无「重名」位 —— 撞目录由 scheme_exists / scheme_list 单独判（T2.1 新建/改名用）。 */
static int scheme_name_ok(const char *n)
{
    return ui_name_ok(n, 0);                 /* v11：同核心 vt_id_ok 尺子（含裸 `-` 拒收） */
}
/* current 读取与目录枚举的「承认」规则：长度/路径安全先过，再认合法用户名或内部兜底名「默认[N]」。 */
static int scheme_name_known(const char *n)
{
    int len = (int)strlen(n);
    if (len < 1 || len > VT_NAME_BUF - 1 || strchr(n, '/')) return 0;
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
/* qsort 比较器：方案名按字节序（strcmp）排（names 每格 VT_NAME_BUF 字节）。 */
static int scheme_name_cmp(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}
/* 枚举 schemes/ 下的方案（readdir；只收承认名 scheme_name_known 的目录），字母序装进 names（每格 VT_NAME_BUF 字节），
 * 返回条数（≤ max）。目录不存在/读不了 = 0 条（面板退化为空列表，不报错）。 */
static int scheme_list(char (*names)[VT_NAME_BUF], int max)
{
    DIR *d = opendir(SCHEME_DIR);
    struct dirent *e;
    int n = 0;
    if (!d) return 0;
    while (n < max && (e = readdir(d)) != NULL) {
        if (!scheme_name_known(e->d_name)) continue;    /* 滤 `.`/`..`、超长、非法名、外来目录 */
        if (!scheme_exists(e->d_name)) continue;        /* 只收目录（dirent 的类型字段不可靠，用 stat） */
        snprintf(names[n], sizeof names[0], "%s", e->d_name);
        n++;
    }
    closedir(d);
    qsort(names, (size_t)n, sizeof names[0], scheme_name_cmp);
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
    snprintf(out, VT_NAME_BUF, "%s", p);
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
    hr_files_note();             /* T8 自写豁免：刷新热生效轮询基线 */
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
    hr_files_note();             /* T8 自写豁免：本文件被写（live/方案侧）后刷新轮询基线 */
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
    char cur[VT_NAME_BUF], name[VT_NAME_BUF], path[160];
    char names[SCHEME_LIST_MAX][VT_NAME_BUF];
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
        if (have_r && have_o) { snprintf(out, VT_NAME_BUF, "%s", cur); return; }    /* 已是有效方案 */
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
    snprintf(out, VT_NAME_BUF, "%s", name);
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
    char cur[VT_NAME_BUF], sp[160];
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
 *   5 = ops.conf 版本门不过（缺版本行 / 非 v1..v5）
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
    char id[VT_NAME_BUF], op[VT_NAME_BUF], tms[16];
    int t, a1, a2, a3, a4, en, fr, fw, fh, kd;
    if (scheme_line_kw(line, "#frame")) {
        if (sscanf(line, "#frame %d %d %d", &fr, &fw, &fh) != 3) return 1;
        return !(fr >= 0 && fr <= 3 && fw > 0 && fh > 0);
    }
    if (scheme_line_kw(line, "region"))
        return sscanf(line, "region %47s %d %d %d %d %d %d", id, &t, &a1, &a2, &a3, &a4, &en) != 7;
    if (scheme_line_kw(line, "hide"))
        return sscanf(line, "hide %47s", id) != 1;
    if (scheme_line_kw(line, "bind")) {
        if (sscanf(line, "bind %47s %47s %15s", id, op, tms) != 3) return 1;
        return strcmp(tms, "down") != 0 && strcmp(tms, "press") != 0;
    }
    if (scheme_line_kw(line, "kind")) {
        if (sscanf(line, "kind %47s %d", id, &kd) != 2) return 1;
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
        (ver != 1 && ver != 2 && ver != 3 && ver != 4 && ver != OPS_CONF_VER)) {
        fclose(f);
        return 5;
    }
    {
        /* ops 段逐行扫（只读）：与 load_ops 同结构 —— 记录边界 = op 行；记录内 step 行 6/7/9 字段
         * （v4 第 10 字段 expr 不参与预检：坏 expr 走 load 的「坏记录单条跳过」口径，不算切换失败）。
         * 坏行 → 6；单条 step 数超 OPS_MAX_STEPS → 7；记录无步骤行 → 6。 */
        int have = 0, nst = 0, badline = 0, ovf = 0, rc = 0;
        while (!rc && fgets(line, sizeof line, f)) {
            char a[VT_NAME_BUF], b[VT_NAME_BUF];
            int ao2 = 0, t, a1, a2, a3, a4, ms, j1, j2, sn;
            char rf[VT_NAME_BUF];
            int pn = sscanf(line, "op %47s gate %47s autooff %d", a, b, &ao2);
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
                sn = sscanf(line, "step %d %d %d %d %d %d %47s %d %d", &t, &a1, &a2, &a3, &a4, &ms, rf, &j1, &j2);
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
/* 切换失败回滚（评审修复 2026-10-05 抽公共）：live ← schemes/<cur> 两文件 + 核心表 ← 同一份（clear+load）。
 * cur 空 = 无源可回（仅告警）。⑤ 清空未生效 / ⑥ current 未写定 共用。 */
static void scheme_rollback_to(const char *cur)
{
    char dp[160];
    if (!cur[0]) return;
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
}

static int S_scm_force = 0;   /* T8 热生效：强制重放标志（仅 scheme_reload 置位；绕过同名 no-op） */
int scheme_switch(const char *name)
{
    char cur[VT_NAME_BUF], sp[160], dp[160];
    int rc, rst = 0;

    if (!name || !scheme_name_known(name)) return 1;
    if (!S_scm_force && scheme_cur_get(cur) == 0 && strcmp(cur, name) == 0) return 0;    /* 已是当前 → no-op（S_scm_force=1 时强制重放，T8） */
    rc = scheme_precheck(name);                                          /* ① 预检：坏 → 拒切、原状 */
    if (rc != 0) return rc;

    /* ② 静默边界（spec §4-2）：操作在跑 → 停（VT_EDIT_OP_STOP 的胶水入口 = vtouch_op_stop）。
     * （缩编 v0.5：操作编辑层 / 取点态 / 区域改名弹层已删，相应复位分支一并移除。） */
    if (vtouch_op_status(NULL, NULL, &rst) == 0 && rst) vtouch_op_stop();

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
     * （见 load_regions 段首：hide 表按文件重建；load_ops 无跨调用状态）。
     * 2026-10-05 评审修复：换表前先**弃转屏重算批**（旧快照不许写回新表）；清空必须真落地
     * （两个 clear 原来丢返回值 —— 核心被占时会静默跳过、load 的「只补缺」再放大成混合表）。 */
    g_rr_active = 0; g_rr_i = 0; g_rr_n = 0; g_rr_fail = 0; g_rr_retry = 0;
    if (vtouch_region_clear() != 0 || vtouch_op_clear() != 0) {
        ALOGW("方案 切换 %s：核心清空未生效（编辑超时/核心被占）→ 回滚 %s", name, cur[0] ? cur : "-");
        scheme_rollback_to(cur);
        return 11;
    }
    load_regions();
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
            scheme_rollback_to(cur);
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
/* T8 热生效-A：强制重放「当前方案」（绕过同名 no-op）——面板 mtime 轮询与方案页 [重载] 键共用。
 * 返回 = scheme_switch 码表（0 成功）；无 current → -1。 */
static int scheme_reload(void)
{
    char cur[VT_NAME_BUF];
    if (scheme_cur_get(cur) != 0 || !cur[0]) return -1;
    S_scm_force = 1;
    int rc = scheme_switch(cur);
    S_scm_force = 0;
    if (rc == 0) ALOGI("方案 重载 %s（热生效）", cur);
    return rc;
}

/* ---- T8 热生效-A（缩编 v0.5）：live 两文件 + current 的 mtime/size 轮询 → 防抖 → 强制重放当前方案。
 * 让「app 侧修改」无需重启核心即生效（~1–2s）；面板自身写盘经 hr_files_note() 刷新基线（自写豁免）。 ---- */
#define HR_POLL_MS     1000           /* 轮询节律 */
#define HR_DEBOUNCE_MS 600            /* 变化稳定窗口（写入可能还在进行；原子 rename 通常瞬时） */
static long hr_next_t = 0;            /* 下次轮询时刻（now_ms） */
static long hr_chg_t = 0;             /* 首次发现变化的时刻（0 = 无）；防抖用 */
static long hr_mtime[3] = {-2, -2, -2};   /* 基线：regions/ops/current 的 mtime（ms 精度） */
static long hr_size[3] = {-1, -1, -1};    /* 基线：size */

static void hr_stat3(long *mt, long *sz)
{
    struct stat st;
    const char *paths[3] = { REGION_CONF_NEW, OPS_CONF_FILE, SCHEME_CUR };
    for (int i = 0; i < 3; i++) {
        if (stat(paths[i], &st) != 0) { mt[i] = -1; sz[i] = -1; continue; }
        mt[i] = (long)st.st_mtime * 1000L + (long)(st.st_mtim.tv_nsec / 1000000L);
        sz[i] = (long)st.st_size;
    }
}
/* 面板自写后调：把基线刷成「刚写完」的状态 ⇒ 自写不会触发重放（自写豁免）。 */
static void hr_files_note(void)
{
    hr_stat3(hr_mtime, hr_size);
    hr_chg_t = 0;
}
/* 渲染线程每拍调（内部自节流）：外部变化稳定后 → 强制重放当前方案。 */
static void hr_poll(void)
{
    long now = now_ms(), mt[3], sz[3];
    if (now < hr_next_t) return;
    hr_next_t = now + HR_POLL_MS;
    hr_stat3(mt, sz);
    if (hr_mtime[0] == -2) {                       /* 首巡：只记基线 */
        for (int i = 0; i < 3; i++) { hr_mtime[i] = mt[i]; hr_size[i] = sz[i]; }
        return;
    }
    int diff = 0;
    for (int i = 0; i < 3; i++) if (mt[i] != hr_mtime[i] || sz[i] != hr_size[i]) diff = 1;
    if (!diff) { hr_chg_t = 0; return; }
    if (hr_chg_t == 0) { hr_chg_t = now; return; } /* 首次发现：进防抖窗 */
    if (now - hr_chg_t < HR_DEBOUNCE_MS) return;   /* 未稳 */
    {
        int rc = scheme_reload();
        if (rc != 0) ALOGW("热生效：重载失败 rc=%d（保持现状，等下次变化）", rc);
        hr_files_note();                           /* 成功/失败都刷基线（同一次变化不重试） */
    }
}

static void gen_id(char *out, int circle)
{
    for (int k = 1; k < 100; k++) {
        char tmp[16];
        int n = vtouch_region_count(), used = 0;
        snprintf(tmp, sizeof tmp, circle ? "c%d" : "r%d", k);
        for (int i = 0; i < n; i++) {
            char id[VT_NAME_BUF]; int t, a1, a2, a3, a4, en;
            if (vtouch_get_region(i, id, sizeof id, &t, &a1, &a2, &a3, &a4, &en) == 0 &&
                !strcmp(id, tmp)) { used = 1; break; }
        }
        if (!used) { snprintf(out, VT_NAME_BUF, "%s", tmp); return; }
    }
    snprintf(out, VT_NAME_BUF, circle ? "c99" : "r99");
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

/* 「面板占屏」状态矩形（当前屏坐标）：吞触摸与喂 ImGui 共用同一个判据 —— 缩编 v0.5 后恒为
 * 面板窗口（编辑层 / 采集覆盖层已删；区域框选直接在面板窗内做，不改变矩形）。 */
static void ui_rect_now(float *x1, float *y1, float *x2, float *y2)
{
    /* 吞触摸矩形 = 面板窗（缩编 v0.5：采集覆盖层/编辑层已删；区域框选不改变矩形）。 */
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

/* ev 回调（poll 线程）：无锁写。单写者（poll）+单读者（render），定界数组，
 * 行内 snprintf 自带 NUL，最坏一帧内看到新旧混排的一行日志/闪错一次 id，
 * 纯装饰性；换来 poll 线程永不因渲染阻塞（此前 move 事件高频取锁饿死输入，整屏顿）。 */
static void ui_ev_cb(const char *line)
{
    char id[VT_NAME_BUF], ev[32];
    int slot, lx, ly;
    if (!strncmp(line, "mark_ev ", 8)) {          /* 核心改了区域的"开关样式"标记 */
        g_need = 1; g_force_frames = 2; g_ov_need = 1;   /* 立刻重画（面板按需重绘，这一步是"事件驱动那一下"）*/
    }
    if (!strncmp(line, "toggle_ev ", 10)) {       /* 核心翻转了开关型区域的开关态（T3.3）：照 mark_ev —— 记日志（下面统一入环）+ 立刻重画 */
        g_need = 1; g_force_frames = 2; g_ov_need = 1;
    }
    if (g_evlog_n < 8) snprintf(g_evlog[g_evlog_n++], 96, "%s", line);
    else { memmove(g_evlog[0], g_evlog[1], 96 * 7); snprintf(g_evlog[7], 96, "%s", line); }
    if (sscanf(line, "region_ev %47s %31s %d %d %d", id, ev, &slot, &lx, &ly) == 5) {
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
        char id[VT_NAME_BUF]; int t, b1, b2, b3, b4, en;
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
    char id[VT_NAME_BUF];
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
    char id[VT_NAME_BUF];
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
        /* 缩编 v0.5：键盘/编辑层已删，落在任何实区即可拖滚 */
        g_scr_on = (g_scr_target != SCR_NONE);
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
     * 必须**每帧**推 —— 拖面板/换方向/关 UI 都会改变这块矩形。缩编 v0.5：矩形 = 面板窗口
     * （编辑层 / 采集覆盖层已删，见 ui_rect_now）。 */
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
    for (i = 0; i < n; i++) {
        int d = 0, x = 0, y = 0;
        if (vtouch_phys_get(i, &d, &x, &y) != 0) d = 0;
        int sx, sy;                        /* 当前屏坐标：面板命中/鼠标喂给 ImGui 用这个；
                                            * x,y 仍是竖屏逻辑坐标（点/轨迹/框选/拖改都用它） */
        p2c(x, y, &sx, &sy);
        int down_edge = d && !g_prev_on[i];
        int up_edge = !d && g_prev_on[i];
        int in_p = d && ui_live && ui_hit_rect((float)sx, (float)sy);   /* 隐藏时不认面板命中 */
        if (d) {
            any = 1;
            if (!g_dots[i].on) g_dots[i].tn = 0;
            g_dots[i].on = 1; g_dots[i].x = x; g_dots[i].y = y;
            int k = g_dots[i].tn % 12;
            g_dots[i].tx[k] = x; g_dots[i].ty[k] = y;
            g_dots[i].tn++;
            /* 手势优先于鼠标：框选 / 选中拖改（面板内一律走鼠标，保证按钮可用）。
             * 面板窗外按下 = 框选/拖改入口（缩编 v0.5：编辑层已删，不再有「整屏给编辑」
             * 的模态；隐藏中由 ui_live 拦掉，见上一行）。 */
            if (down_edge && ui_live && !in_p) {   /* 隐藏中不开始框选/拖改（否则会改表并落盘） */
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
            if (ui_live && g_mslot < 0 && in_p && i != g_cap_slot && i != g_edit_slot) {
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
                char id[VT_NAME_BUF];
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
static int g_font_have_ttf = 0;          /* TTF 装载成功（fallback 时不折腾）；font_probe 先于 v11 块使用 → 声明在家 */
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
        g_font_have_ttf = 1;                       /* v11 字形扩集就绪（见下） */
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

/* ---- v11 字形按需扩集：用户名字可以是任意中文 —— 每帧扫「会显示的文本」（区域/操作/事件日志；
 * 方案/素材名单带目录 IO、约 2s 一巡）里的码点，缺字追加进扩集区段，帧间重建图集 + 重传纹理
 * （烘字只随集合增量发生）。防死循环：进过扩集仍缺字（TTF 里没有该字形）不再重复重建。
 * 说明：ImWchar = 16 位 → 只覆盖 BMP（汉字在 BMP；emoji 等超出 BMP 不进图集，显示兜底字形）。 ---- */
static ImVector<ImWchar> g_font_extra;   /* 追加码点（= 已尝试集；不重复 push） */
static int g_font_dirty = 0;             /* 有新增码点待进图集（帧间重建） */
static int g_glyph_tick = 0;

static int ui_extra_has(unsigned cp)
{
    for (int i = 0; i < g_font_extra.Size; i++)
        if ((unsigned)g_font_extra[i] == cp) return 1;
    return 0;
}
/* 扫一段 UTF-8：BMP 内、字体没有、扩集里也没有 → 记录（手写解码，与核心同规则）。 */
static void ui_glyph_scan(const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    if (!g_font_have_ttf || !s || !*s) return;
    while (*p) {
        unsigned cp, len;
        if (*p < 0x80) { p++; continue; }
        if ((*p & 0xE0) == 0xC0)      { len = 2; cp = *p & 0x1Fu; }
        else if ((*p & 0xF0) == 0xE0) { len = 3; cp = *p & 0x0Fu; }
        else if ((*p & 0xF8) == 0xF0) { len = 4; cp = *p & 0x07u; }
        else { p++; continue; }
        for (unsigned k = 1; k < len; k++) {
            if ((p[k] & 0xC0) != 0x80) { len = 0; break; }
            cp = (cp << 6) | (p[k] & 0x3Fu);
        }
        if (len == 0 || cp > 0xFFFF || cp < 0x80) { p++; continue; }
        p += len;
        if (ui_extra_has(cp)) continue;                                  /* 已尝试过（含字体缺字的顽固集） */
        if (g_font_meta && g_font_meta->FindGlyphNoFallback((ImWchar)cp)) continue;   /* 已有字形 */
        g_font_extra.push_back((ImWchar)cp);
        g_font_dirty = 1;
    }
}
static void ui_glyph_scan_all(int heavy)
{
    int i, n, k;
    if (!g_font_have_ttf) return;
    n = vtouch_region_count();
    for (i = 0; i < n; i++) {
        char idb[VT_NAME_BUF]; int t, a1, a2, a3, a4, en;
        if (vtouch_get_region(i, idb, sizeof idb, &t, &a1, &a2, &a3, &a4, &en) == 0) ui_glyph_scan(idb);
    }
    n = vtouch_op_count();
    for (i = 0; i < n; i++) {
        char nmb[VT_NAME_BUF];
        if (vtouch_get_op(i, nmb, sizeof nmb, NULL, NULL, 0, NULL) == 0) ui_glyph_scan(nmb);
    }
    for (i = 0; i < g_evlog_n; i++) ui_glyph_scan(g_evlog[i]);
    if (!heavy) return;                          /* 下面带目录 IO：约 2s 一巡 */
    {
        char cur[VT_NAME_BUF];
        char names[SCHEME_LIST_MAX][VT_NAME_BUF];
        int sn = scheme_list(names, SCHEME_LIST_MAX);
        for (k = 0; k < sn; k++) ui_glyph_scan(names[k]);
        if (scheme_cur_get(cur) == 0) ui_glyph_scan(cur);
    }
}
/* 帧间重建：Clear → 基础盘 + 源码字 + 扩集 → 两档重加 → Build → 重传纹理。 */
static void ui_font_rebuild(void)
{
    static const char *path = "/system/fonts/SysSans-Hans-Regular.ttf";
    ImGuiIO &io = ImGui::GetIO();
    static ImVector<ImWchar> rg2;
    ImFontGlyphRangesBuilder b;
    long b0 = now_ms();

    b.AddRanges(io.Fonts->GetGlyphRangesDefault());
    if (k_ui_chars[0]) b.AddText(k_ui_chars);
    for (int i = 0; i < g_font_extra.Size; i++) b.AddChar(g_font_extra[i]);
    rg2.clear();
    b.BuildRanges(&rg2);
    io.Fonts->ClearInputData();
    io.Fonts->ClearTexData();
    io.Fonts->ClearFonts();
    {
        ImFont *f = io.Fonts->AddFontFromFileTTF(path, 44.0f, 0, rg2.Data);
        if (f) g_font_meta = io.Fonts->AddFontFromFileTTF(path, 30.0f, 0, rg2.Data);
        else { io.Fonts->AddFontDefault(); g_font_meta = 0; g_font_have_ttf = 0; }
    }
    io.Fonts->Build();
    ImGui_ImplOpenGL3_DestroyDeviceObjects();
    ImGui_ImplOpenGL3_CreateDeviceObjects();
    ALOGI("font rebuild +%d chars bake=%.0fms", g_font_extra.Size, (double)(now_ms() - b0));
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
            char id[VT_NAME_BUF]; int type, a1, a2, a3, a4, en;
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
        char id[VT_NAME_BUF]; int t, a1, a2, a3, a4, en;
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
            memmove(g_hidden[j], g_hidden[j + 1], (size_t)(g_nhide - j - 1) * VT_NAME_BUF);
            g_nhide--;
            break;
        }
    }
    save_regions();
    g_force_frames = 3;
    ALOGI("del %s n=%d", id, vtouch_region_count());
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
        if (nav_btn("事件日志", g_nav == 2 && g_sheet, bw)) { g_nav = 2; g_sheet = 1; g_need = 1; }
        if (nav_btn("设置",     g_nav == 3 && g_sheet, bw)) { g_nav = 3; g_sheet = 1; g_need = 1; }
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
    char ops[32][VT_NAME_BUF], nxt[VT_NAME_BUF];
    int n = 0, k, ev, cnt = vtouch_op_count();
    for (k = 0; k < cnt && n < 32; k++) {
        char nm[VT_NAME_BUF];
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
        /* id 显示为普通文本（改名归编辑器 app，缩编 v0.5）；脚本按 id 监听。 */
        {
            ImGui::AlignTextToFramePadding();
            char head[128];
            snprintf(head, sizeof head, "%s   %s%s%s", id, type == 1 ? "圆形" : "矩形",
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
            char op[VT_NAME_BUF] = {0}; int ev = 0, kd = 0, tg = 0;
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
            char id[VT_NAME_BUF]; int type, a1, a2, a3, a4, en;
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
/* ---- 操作页（缩编 v0.5）：只读卡片 + [运行]/[停止]；数据全部实时读核心（只读区 A）----
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

/* 一张操作卡片（缩编 v0.5）：名字 + 元信息行（步数/门控/自动关）+ 运行中行 + [运行]/[停止] 单键。
 * 新建/编辑/删除已归编辑器 app。run_i/run_step/run_state 是页头本帧读好传下来的 op_status 读数。 */
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
        /* 单键全宽（键高 76 保手指可点）：运行 / 停止 */
        float bw = ImGui::GetContentRegionAvail().x;
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
        char rn[VT_NAME_BUF] = {0}, m[64];
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
    ImGui::Dummy(ImVec2(0, 8));
    if (n == 0) {
        text_meta_w("还没有操作（在编辑器 app 里新建）；这里可以看运行状态、手动运行/停止。");
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
            char name[VT_NAME_BUF], gate[VT_NAME_BUF];
            int steps = 0, autoff = 0;
            if (vtouch_get_op(i, name, sizeof name, &steps, gate, sizeof gate, &autoff) != 0) continue;
            op_card(i, name, steps, gate, autoff, run_i, run_step, run_state);
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
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
    ImGui::TextDisabled("视觉抓帧");
    /* 抓帧间隔（2026-10-05f；自模板页移入，缩编 v0.5）：抓帧线程与面板渲染解耦后的抓帧节奏（0 = 全速）；
     * 点一下循环预设（ui.conf 持久化）。 */
    {
        static const int presets[8] = { 0, 5, 10, 20, 33, 50, 100, 200 };
        char lab[80];
        int k, cur = 0;
        for (k = 0; k < 8; k++) if (presets[k] == g_cap_ms) { cur = k; break; }
        snprintf(lab, sizeof lab, "抓帧间隔：%d ms%s（点一下切换）", g_cap_ms, g_cap_ms == 0 ? " = 全速" : "");
        if (btn_light(lab, ImVec2(ImGui::GetContentRegionAvail().x, 84))) {
            g_cap_ms = presets[(cur + 1) % 8];
            vtouch_vis_cap_interval_set(g_cap_ms);
            ui_conf_save();
            ALOGI("vis 抓帧间隔 → %d ms", g_cap_ms);
            g_need = 1; g_force_frames = 2;
        }
        ImGui::Dummy(ImVec2(0, 6));
    }
    text_meta_w("其它设置在编辑器 app（com.vtouch.editor）；面板 = 运行时窗口（观察 + 控制 + 区域调整）。");
}



/* ---- 方案页（缩编 v0.5）：列表 + 切换 + [重载当前方案] 兜底键 --------------------------------------
 * 布局：页头（当前）+ 提示槽（固定占位）+ 列表（方案名 + [切换]；当前行高亮、按钮禁用；
 * 可滚 + SCR_LIST 拖滚）+ 底部 [重载当前方案]（T8 热生效-A 的手动版）。
 * CRUD（新建/另存为/重命名/删除）与外层名字键盘已删（归编辑器 app）。
 * 接线：切换 = scheme_switch；重载 = scheme_reload（绕过同名 no-op 的强制重放）。
 * 失败路径就地图示（码表 = scheme_switch 段首注释；9/10 是「已回滚」语义，提示里写明）。 */

static char g_scm_msg[128] = {0};       /* 页面提示槽（切换结果 / 拒收 / 操作成败；固定占位） */

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
    case 11: return "切换失败：核心清空未生效（已回滚，未切换）";
    default: return "切换失败";
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
/* 一行方案：名字（普通文本）+ [切换]（当前行禁用）；当前行高亮（浅蓝底 + 蓝边，同操作卡运行态口径）。
 * 缩编 v0.5：选中 / 新建 / 另存为 / 重命名 / 删除已归编辑器 app，面板只留切换。 */
static void scm_row(const char *name, const char *cur)
{
    int is_cur = cur[0] && !strcmp(name, cur);
    ImGui::PushID(name);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, is_cur ? ImVec4(0.937f, 0.965f, 1.00f, 1.00f) : WHITE);
    ImGui::PushStyleColor(ImGuiCol_Border, is_cur ? BLUE500 : ZINC200);
    ImGui::BeginChild("row", ImVec2(0, 0), ImGuiChildFlags_Border | ImGuiChildFlags_AutoResizeY,
                      ImGuiWindowFlags_NoScrollbar);
    {
        float gap = 12.0f, bw = 150.0f;
        float nw = ImGui::GetContentRegionAvail().x - bw - gap;
        if (nw < 120) nw = 120;
        {
            /* 名字：普通文本，垂直居中在 84 高的行里（缩编 v0.5；原「点名字选中」已删） */
            ImVec2 p = ImGui::GetCursorScreenPos();
            ImVec2 ts = ImGui::CalcTextSize(name);
            ImGui::GetWindowDrawList()->AddText(ImVec2(p.x + 16, p.y + (84.0f - ts.y) * 0.5f),
                                                ImGui::GetColorU32(ImGuiCol_Text), name);
            ImGui::Dummy(ImVec2(nw, 84));
        }
        ImGui::SameLine();
        if (is_cur) {                       /* 当前行：按钮禁用（spec §6） */
            ImGui::BeginDisabled();
            btn_blue("切换", ImVec2(bw, 84));
            ImGui::EndDisabled();
        } else if (btn_blue("切换", ImVec2(bw, 84))) {
            scm_switch_to(name);
        }
        if (is_cur) text_meta_s("当前方案");
    }
    ImGui::EndChild();
    ImGui::PopStyleColor(2);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(12, 12));
    ImGui::Dummy(ImVec2(0, 0));   /* bento 卡片间隙 */
    ImGui::PopStyleVar();
    ImGui::PopID();
}
/* 「方案」页（缩编 v0.5）：页头 + 提示槽（固定占位）+ 列表（切换）+ [重载当前方案] 兜底键。
 * CRUD（新建/另存/重命名/删除）与外层名字键盘已删（归编辑器 app）。 */
static void page_scheme(void)
{
    char cur[VT_NAME_BUF] = {0}, names[SCHEME_LIST_MAX][VT_NAME_BUF], meta[40], l[64];
    int n, i;
    scheme_cur_get(cur);
    n = scheme_list(names, SCHEME_LIST_MAX);
    snprintf(meta, sizeof meta, "%d 个", n);
    page_header("方案", meta);
    snprintf(l, sizeof l, "当前：%s", cur[0] ? cur : "—");
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
        float list_h = ImGui::GetContentRegionAvail().y - (84.0f + 12.0f + 12.0f);
        if (list_h < 0) list_h = 0;
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
    /* [重载当前方案]（缩编 v0.5/T8）：把外部（app）对方案的修改立即生效（等效「切走再切回」一键版）。 */
    if (btn_light("重载当前方案（应用外部修改）", ImVec2(ImGui::GetContentRegionAvail().x, 84))) {
        int rc = scheme_reload();
        if (rc == 0) {
            char c2[VT_NAME_BUF] = {0};
            if (scheme_cur_get(c2) == 0) snprintf(g_scm_msg, sizeof g_scm_msg, "已重载当前方案 %s", c2);
        } else {
            snprintf(g_scm_msg, sizeof g_scm_msg, "%s", scm_switch_why(rc));
            ALOGW("方案 重载失败 rc=%d", rc);
        }
        g_need = 1; g_force_frames = 3;
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
    /* 列表实区每帧先清空：只在列表页发布（区域列表 / 操作 / 方案），其它页不命中 → 不会误滚 */
    g_zone_list[0] = g_zone_list[1] = g_zone_list[2] = g_zone_list[3] = 0;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(PAD_X, PAD_Y));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(12, 10));
    build_titlebar(ww);
    if (g_min) {   /* 收起态：只有标题条；清掉侧栏/内容页实区，免上一帧残留命中 */
        g_zone_side[0] = g_zone_side[1] = g_zone_side[2] = g_zone_side[3] = 0;
        g_zone_sheet[0] = g_zone_sheet[1] = g_zone_sheet[2] = g_zone_sheet[3] = 0;
    }
    if (!g_min) ImGui::Dummy(ImVec2(0, COL_GAP - 10));
    if (!g_min) build_sidebar();          /* 缩编 v0.5：名字/编辑层覆盖层已删，条件只剩收起态 */
    if (!g_min && g_sheet) {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_ChildBg, WHITE);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 12));
        ImGui::BeginChild("##sheet", ImVec2(0, 0), ImGuiChildFlags_None);
        pub_zone(g_zone_sheet);
        drag_scroll_for(SCR_SHEET);
        if (g_nav == 0) page_regions();
        else if (g_nav == 1) page_ops();
        else if (g_nav == 2) page_log();
        else if (g_nav == 5) page_scheme();
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

/* v6D：提示浮层（核心 → 状态块 toast 通道，契约 v14）：顶部居中、深底白字、末 300ms 淡出。
 * 认领新提示时扩字形（动态文本 → 下一帧字体重建，首帧可能缺字形、随即恢复）。 */
static void draw_toast(float sw, float sh)
{
    static char t_text[256];
    static long t_until = 0;
    char buf[256];
    int  ms = 0;
    if (vtouch_toast_take(buf, (int)sizeof buf, &ms)) {
        snprintf(t_text, sizeof t_text, "%s", buf);
        t_until = now_ms() + (long)((ms > 0) ? ms : 2000);
        ui_glyph_scan(t_text);
    }
    if (!t_text[0]) return;
    long now = now_ms();
    if (now >= t_until) { t_text[0] = 0; return; }
    long rem = t_until - now;
    int alpha = 255;
    if (rem < 300) alpha = (int)(255 * rem / 300);
    ImVec2 ts = ImGui::CalcTextSize(t_text);
    float x = (sw - ts.x) * 0.5f;
    float y = sh * 0.12f;
    ImDrawList *dl = ImGui::GetForegroundDrawList();
    dl->AddRectFilled(ImVec2(x - 18, y - 10), ImVec2(x + ts.x + 18, y + ts.y + 10),
                      IM_COL32(16, 18, 20, (int)(0.74f * alpha)), 10.0f);
    dl->AddText(ImVec2(x, y), IM_COL32(244, 246, 248, alpha), t_text);
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
    /* v11：用户中文名的字形按需扩集（扫描 → 帧间重建；TTF 兜底时不动） */
    ui_glyph_scan_all((g_glyph_tick++ & 127) == 0);
    if (g_font_dirty) { g_font_dirty = 0; ui_font_rebuild(); }
    ImGui_ImplOpenGL3_NewFrame();
    ImGui::NewFrame();
    if (g_ov_show) build_overlay(sw, sh);
    build_panel();                         /* 缩编 v0.5：采集覆盖层/编辑层已删，只剩叠加层 + 面板 */
    draw_toast((float)sw, (float)sh);      /* v6D：提示浮层（核心 toast 通道） */
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
    char jd[VT_NAME_BUF]; int t, a1, a2, a3, a4, en;
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
            if (k != i) snprintf(g_hidden[k], VT_NAME_BUF, "%s", g_hidden[i]);
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
        hr_poll();                /* T8 热生效-A：外部改盘 → 防抖 → 自动重放当前方案（自写豁免） */
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
        char scm[VT_NAME_BUF];
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
