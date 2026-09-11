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
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "imgui.h"
#include "backends/imgui_impl_opengl3.h"

extern "C" {
void vtouch_set_event_cb(void (*ev_cb)(const char *));
void vtouch_set_region_cb(void (*cb)(void));
int vtouch_phys_slots(void);
int vtouch_phys_get(int i, int *down, int *lx, int *ly);
int vtouch_init(int argc, char **argv);
int vtouch_poll_step(int timeout_ms);
void vtouch_cleanup(void);
void vtouch_region_clear(void);
int vtouch_region_count(void);
int vtouch_region_add(const char *id, int type, int a1, int a2, int a3, int a4, int enabled);
int vtouch_get_region(int i, char *id, int idn, int *type,
                      int *a1, int *a2, int *a3, int *a4, int *enabled);
}

#define LOGT "VTouchUI"
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO, LOGT, __VA_ARGS__)
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOGT, __VA_ARGS__)

static int g_w = 1440, g_h = 3168;
static volatile int g_running = 1;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_render_th, g_poll_th;
static int g_poll_on = 0;

/* EGL（单 surface） */
static EGLDisplay g_dpy = EGL_NO_DISPLAY;
static EGLContext g_ctx = EGL_NO_CONTEXT;
static EGLConfig g_cfg = 0;
static ANativeWindow *g_win = 0;
static EGLSurface g_surf = EGL_NO_SURFACE;

/* 面板几何（唯一来源：下面 #define + panel_w()/in_panel() + build_panel() 三处同公式）
 * sidebar-fixed 骨架：固定侧栏 w-64(256) 不随内容滚，内容页自己滚。 */
static float g_pan_x = 780, g_pan_y = 200;
static int g_sheet = 1;                   /* 内容页开/合（合 = 只留侧栏） */
static int g_min = 0;                     /* 收起态：整窗只剩一条标题栏（会话内有效，重启展开） */
static int g_nav = 0;                     /* 0 区域 1 日志 2 设置 */
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
static int g_prev_on[64];
static int g_mdown = 0, g_mslot = -1, g_mup_pend = 0;
static float g_mx = -1, g_my = -1;
static long g_mdown_t = 0;
static int g_drag = 0;                 /* 标题栏几何拖拽（快照侧直驱，不走 ImGui 拖拽机） */
static float g_drag_ox = 0, g_drag_oy = 0;
/* g_need 无锁置位：只由渲染线程清零，其余线程只置 1（单字对齐存取原子；
 * 极小概率与清零竞态丢一次重画，按钮路径当前帧本就带新状态，WS 路径下次事件补画） */
static volatile int g_need = 1;
static volatile int g_force_frames = 0;  /* 强制连画 N 帧（切换显隐/开关后兜底，免单帧被吞） */
static int g_need_mouse_evt = 0, g_mouse_evt_down = 0;
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
static void save_regions(void);   /* 定义见下：WS 线程只置位，实际写盘在渲染线程（单一写者，避免并发写） */
static volatile int g_save_pending = 0;
static void ui_region_changed(void) { g_need = 1; g_force_frames = 2; g_save_pending = 1; }

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
static void save_regions(void);   /* hide_toggle 先用，后定义 */
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
}
/* regions.conf 格式版本：只有版本一致的才认，否则整份丢弃（防旧版本残留区域被反复加载） */
#define REGION_CONF_VER 2
static void save_regions(void)
{
    FILE *f = fopen("/data/local/tmp/vtouch-runtime/regions.conf", "w");
    int i;
    if (!f) return;
    fprintf(f, "#vtouch-regions v%d\n", REGION_CONF_VER);
    int n = vtouch_region_count();
    for (i = 0; i < n; i++) {
        char id[16]; int t, a1, a2, a3, a4, en;
        if (vtouch_get_region(i, id, sizeof id, &t, &a1, &a2, &a3, &a4, &en) != 0) continue;
        fprintf(f, "region %s %d %d %d %d %d %d\n", id, t, a1, a2, a3, a4, en);
    }
    for (i = 0; i < g_nhide; i++) fprintf(f, "hide %s\n", g_hidden[i]);
    fclose(f);
}
static void load_regions(void)
{
    FILE *f = fopen("/data/local/tmp/vtouch-runtime/regions.conf", "r");
    char line[128];
    int ver = 0;
    if (!f) return;
    /* 版本门：无版本行 / 版本不符 = 旧版本残留（如已删掉的 demo 区域）→ 丢弃并立刻改写成空表 */
    if (!fgets(line, sizeof line, f) || sscanf(line, "#vtouch-regions v%d", &ver) != 1 || ver != REGION_CONF_VER) {
        fclose(f);
        ALOGI("regions.conf 旧格式/版本不符 → 丢弃清空");
        save_regions();
        return;
    }
    while (fgets(line, sizeof line, f)) {
        char id[16]; int t, a1, a2, a3, a4, en;
        if (sscanf(line, "region %15s %d %d %d %d %d %d", id, &t, &a1, &a2, &a3, &a4, &en) == 7) {
            vtouch_region_add(id, t, a1, a2, a3, a4, en);
        } else if (sscanf(line, "hide %15s", id) == 1) {
            if (g_nhide < 32 && !is_hidden(id)) snprintf(g_hidden[g_nhide++], 16, "%s", id);
        }
    }
    fclose(f);
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
static int in_panel(float x, float y)
{
    /* 普通浮动窗：g_pan 起点，宽高随内容页开合 / 收起态 */
    return x >= g_pan_x && x < g_pan_x + panel_w() && y >= g_pan_y && y < g_pan_y + panel_h();
}
static void push_ring(int x, int y)
{
    Ring *r = &g_rings[g_ring_i++ & 7];
    r->on = 1; r->x = x; r->y = y; r->t = now_ms();
}

/* ev 回调（poll 线程）：无锁写。单写者（poll）+单读者（render），定界数组，
 * 行内 snprintf 自带 NUL，最坏一帧内看到新旧混排的一行日志/闪错一次 id，
 * 纯装饰性；换来 poll 线程永不因渲染阻塞（此前 move 事件高频取锁饿死输入，整屏顿）。 */
static void ui_ev_cb(const char *line)
{
    char id[16], ev[32];
    int slot, lx, ly;
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
    g_need = 1;
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
        }
    } else if (g_cap_mode == 2) {
        int dx = x1 - x0, dy = y1 - y0;
        int r = (int)sqrt((double)(dx * dx + dy * dy));
        if (r < 20 || x0 < 0 || x0 >= g_w || y0 < 0 || y0 >= g_h) return;
        gen_id(id, 1);
        if (vtouch_region_add(id, 1, x0, y0, r, 0, 1) == 0) {
            save_regions();
            ALOGI("cap add circle %s %d,%d r%d", id, x0, y0, r);
        }
    }
}
/* 拖改应用（移动/缩放影子值；50ms 节流直播进表，up 时提交落盘） */
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
    if (now_ms() - g_edit_last >= 50) {
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
        if (g_pan_x < 0) g_pan_x = 0;
        if (g_pan_y < 0) g_pan_y = 0;
        if (g_pan_x > g_w - 160) g_pan_x = (float)(g_w - 160);
        if (g_pan_y > g_h - 120) g_pan_y = (float)(g_h - 120);
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
    if (n > 64) n = 64;
    for (i = 0; i < n; i++) {
        int d, x, y;
        if (vtouch_phys_get(i, &d, &x, &y) != 0) d = 0;
        int down_edge = d && !g_prev_on[i];
        int up_edge = !d && g_prev_on[i];
        int in_p = d && in_panel((float)x, (float)y);
        if (d) {
            any = 1;
            if (!g_dots[i].on) g_dots[i].tn = 0;
            g_dots[i].on = 1; g_dots[i].x = x; g_dots[i].y = y;
            int k = g_dots[i].tn % 12;
            g_dots[i].tx[k] = x; g_dots[i].ty[k] = y;
            g_dots[i].tn++;
            /* 手势优先于鼠标：框选 / 选中拖改（面板内一律走鼠标，保证按钮可用） */
            if (down_edge && !in_p) {
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
            if (i == g_cap_slot) { g_cap_x1 = x; g_cap_y1 = y; }
            if (i == g_edit_slot) edit_apply_live(x, y);
            if (g_mslot < 0 && in_p && i != g_cap_slot && i != g_edit_slot) {
                panel_press(i, x, y);
            } else if (i == g_mslot) {
                panel_drag_move(x, y);
            }
        } else {
            if (g_prev_on[i] && g_dots[i].on) push_ring(g_dots[i].x, g_dots[i].y);
            g_dots[i].on = 0;
            if (i == g_mslot) panel_release();
            if (i == g_mslot && !d) g_mslot = -1;
            if (up_edge && i == g_cap_slot) {
                cap_commit();
                g_cap_slot = -1;
            }
            if (up_edge && i == g_edit_slot) {
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
        if (vtouch_poll_step(200) != 0) break;
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
 * 两档字号：正文 44（触摸可读）、元信息 30（卡片坐标/页头状态 = bento 的小字层级） ---- */
static ImFont *g_font_meta = 0;
static void font_probe(void)
{
    static const char *path = "/system/fonts/SysSans-Hans-Regular.ttf";
    ImGuiIO &io = ImGui::GetIO();
    const ImWchar *rg = io.Fonts->GetGlyphRangesChineseFull();
    ImFont *f = io.Fonts->AddFontFromFileTTF(path, 44.0f, 0, rg);
    if (f) g_font_meta = io.Fonts->AddFontFromFileTTF(path, 30.0f, 0, rg);
    else io.Fonts->AddFontDefault();
    unsigned char *px; int pw, ph;
    io.Fonts->GetTexDataAsRGBA32(&px, &pw, &ph);   /* 建图集后才可判字形 */
    if (f && f->FindGlyphNoFallback(0x4E2D)) {
        ALOGI("font ok %s 44%s", path, g_font_meta ? "+30" : "");
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
    if (sw == g_w && sh == g_h) {
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
            ImU32 col = is_flash ? IM_COL32(0, 255, 0, 255)
                        : is_sel ? IM_COL32(255, 200, 0, 255)
                        : is_ex ? (g_ex_enter ? IM_COL32(0, 220, 255, 255) : IM_COL32(180, 180, 180, 255))
                        : inside ? IM_COL32(0, 220, 0, 255) : IM_COL32(255, 40, 40, 255);
            float lw = (is_flash || is_ex || is_sel) ? 6.0f : 3.0f;
            ImVec2 p0 = (type == 1 ? ImVec2((float)(a1 - a3), (float)(a2 - a3))
                                   : ImVec2((float)a1, (float)a2));
            ImVec2 p1 = (type == 1 ? ImVec2((float)(a1 + a3), (float)(a2 + a3))
                                   : ImVec2((float)a3, (float)a4));
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
            dl->AddText(ImVec2((float)(type == 1 ? a1 : a1), (float)(type == 1 ? a2 - a3 - 34 : a2 - 34)), col, id);
        }
        /* 框选橡皮筋 + 提示 */
        if (g_cap_mode && g_cap_slot >= 0) {
            ImU32 cc = IM_COL32(0, 220, 255, 255);
            if (g_cap_mode == 1) {
                int xa = g_cap_x0 < g_cap_x1 ? g_cap_x0 : g_cap_x1;
                int xb = g_cap_x0 < g_cap_x1 ? g_cap_x1 : g_cap_x0;
                int ya = g_cap_y0 < g_cap_y1 ? g_cap_y0 : g_cap_y1;
                int yb = g_cap_y0 < g_cap_y1 ? g_cap_y1 : g_cap_y0;
                dl->AddRect(ImVec2((float)xa, (float)ya), ImVec2((float)xb, (float)yb), cc, 0, 0, 4.0f);
            } else {
                int dx = g_cap_x1 - g_cap_x0, dy = g_cap_y1 - g_cap_y0;
                float r = sqrtf((float)(dx * dx + dy * dy));
                dl->AddCircle(ImVec2((float)g_cap_x0, (float)g_cap_y0), r, cc, 64, 4.0f);
                dl->AddCircleFilled(ImVec2((float)g_cap_x0, (float)g_cap_y0), 10.0f, cc);
            }
        }
        if (g_cap_mode) {
            dl->AddText(ImVec2(60, 300), IM_COL32(0, 220, 255, 255),
                        g_cap_mode == 1 ? "\xE6\x8B\x96\xE6\x8B\xBD\xE6\xA1\x86\xE9\x80\x89\xE7\x9F\xA9\xE5\xBD\xA2"
                                        : "\xE6\x8B\x96\xE6\x8B\xBD\xE5\x9C\x86\xE5\xBF\x83\xE6\x8B\x96\xE5\x8D\x8A\xE5\xBE\x84");
        }
        /* 轨迹 + 蓝点（面板内手指不画蓝点，绿点另画） */
        for (int di = 0; di < 64; di++) {
            if (!g_dots[di].on) continue;
            int in_p = in_panel((float)g_dots[di].x, (float)g_dots[di].y);
            int cnt = g_dots[di].tn < 12 ? g_dots[di].tn : 12;
            int base = g_dots[di].tn < 12 ? 0 : g_dots[di].tn - cnt;
            for (int k = 1; k < cnt; k++) {
                int i0 = (base + k - 1) % 12, i1 = (base + k) % 12;
                int a = 200 * k / cnt;
                dl->AddLine(ImVec2((float)g_dots[di].tx[i0], (float)g_dots[di].ty[i0]),
                            ImVec2((float)g_dots[di].tx[i1], (float)g_dots[di].ty[i1]),
                            IM_COL32(60, 140, 255, a), 5.0f);
            }
            if (!in_p) {
                dl->AddCircleFilled(ImVec2((float)g_dots[di].x, (float)g_dots[di].y),
                                    16.0f, IM_COL32(60, 140, 255, 255));
                char nb[8]; snprintf(nb, sizeof nb, "%d", di);
                dl->AddText(ImVec2((float)g_dots[di].x + 20, (float)g_dots[di].y - 16),
                            IM_COL32(60, 140, 255, 255), nb);
            }
        }
        /* 抬起扩散圈 */
        for (int ri = 0; ri < 8; ri++) {
            if (!g_rings[ri].on) continue;
            long age = t - g_rings[ri].t;
            if (age > 400) { g_rings[ri].on = 0; continue; }
            float r = 10.0f + age * 0.25f;
            dl->AddCircle(ImVec2((float)g_rings[ri].x, (float)g_rings[ri].y), r,
                          IM_COL32(0, 200, 0, (int)(220 * (400 - age) / 400)), 32, 4.0f);
        }
        /* 面板触摸绿点 */
        if (g_mdown) {
            dl->AddCircleFilled(ImVec2(g_mx, g_my), 18.0f, IM_COL32(0, 200, 0, 255));
        }
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
    /* core 无单删：全清后回填（保序），清选中/显隐残留 */
    char k_id[32][16]; int k_t[32], k_a[32][4], k_en[32], kn = 0;
    int n2 = vtouch_region_count();
    for (int j = 0; j < n2 && kn < 32; j++) {
        char jd[16]; int jt, ja1, ja2, ja3, ja4, jen;
        if (vtouch_get_region(j, jd, sizeof jd, &jt, &ja1, &ja2, &ja3, &ja4, &jen) != 0) continue;
        if (!strcmp(jd, id)) continue;
        snprintf(k_id[kn], 16, "%s", jd);
        k_t[kn] = jt; k_a[kn][0] = ja1; k_a[kn][1] = ja2;
        k_a[kn][2] = ja3; k_a[kn][3] = ja4; k_en[kn] = jen; kn++;
    }
    vtouch_region_clear();
    for (int j = 0; j < kn; j++)
        vtouch_region_add(k_id[j], k_t[j], k_a[j][0], k_a[j][1], k_a[j][2], k_a[j][3], k_en[j]);
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
    ALOGI("del %s", id);
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
/* core 无改名：全清后回填（保序），同步选中/隐藏引用，落盘 */
static void rename_region(const char *old_id, const char *new_id)
{
    char k_id[32][16]; int k_t[32], k_a[32][4], k_en[32], kn = 0;
    int n = vtouch_region_count();
    for (int j = 0; j < n && kn < 32; j++) {
        char jd[16]; int jt, ja1, ja2, ja3, ja4, jen;
        if (vtouch_get_region(j, jd, sizeof jd, &jt, &ja1, &ja2, &ja3, &ja4, &jen) != 0) continue;
        snprintf(k_id[kn], 16, "%s", strcmp(jd, old_id) ? jd : new_id);
        k_t[kn] = jt; k_a[kn][0] = ja1; k_a[kn][1] = ja2;
        k_a[kn][2] = ja3; k_a[kn][3] = ja4; k_en[kn] = jen; kn++;
    }
    vtouch_region_clear();
    for (int j = 0; j < kn; j++)
        vtouch_region_add(k_id[j], k_t[j], k_a[j][0], k_a[j][1], k_a[j][2], k_a[j][3], k_en[j]);
    if (!strcmp(g_sel_id, old_id)) snprintf(g_sel_id, sizeof g_sel_id, "%s", new_id);
    for (int j = 0; j < g_nhide; j++)
        if (!strcmp(g_hidden[j], old_id)) snprintf(g_hidden[j], 16, "%s", new_id);
    save_regions();
    g_force_frames = 3;
    ALOGI("rename %s -> %s n=%d", old_id, new_id, kn);
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
                if (g_pan_x + panel_w() > (float)g_w) g_pan_x = (float)g_w - panel_w();
                if (g_pan_y + panel_h() > (float)g_h) g_pan_y = (float)g_h - panel_h();
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
        if (nav_btn("事件日志", g_nav == 1 && g_sheet, bw)) { g_nav = 1; g_sheet = 1; g_need = 1; }
        if (nav_btn("设置",     g_nav == 2 && g_sheet, bw)) { g_nav = 2; g_sheet = 1; g_need = 1; }
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
        if (btn_light(g_sheet ? "收起内容" : "展开内容", ImVec2(bw, 76))) { g_sheet = !g_sheet; g_need = 1; }
        if (btn_red("退出", ImVec2(bw, 76))) { vtouch_cleanup(); _exit(0); }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

/* ---- 区域卡片（bento：白卡 + zinc-100 边 + xl 圆角 + 等宽四键） ---- */
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

/* 改名弹层：面板收不到系统输入法（composer 图层没有 IME），字符全靠点。
 * 画在面板窗内并盖住侧栏与内容页；那两块本帧干脆不画，免得底下按钮还能吃点击。 */
static void draw_name_edit(void)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 wp = ImGui::GetWindowPos();
    float ww = ImGui::GetWindowWidth(), wh = ImGui::GetWindowHeight();
    ImVec2 a(wp.x + 12, wp.y + (float)TITLE_H + 10), b(wp.x + ww - 12, wp.y + wh - 12);
    dl->AddRectFilled(a, b, IM_COL32(255, 255, 255, 253), 14);
    dl->AddRect(a, b, IM_COL32(228, 228, 231, 255), 14, 0, 1.5f);
    float x0 = a.x + 26, y0 = a.y + 24, cw = (b.x - x0) - 26;
    ImGui::SetCursorScreenPos(ImVec2(x0, y0));
    text_meta_s("给区域起个名字：脚本就按这个名字认它（默认 r1/c1…，可改）");
    /* 当前输入 + 原名对照 */
    ImGui::PushStyleColor(ImGuiCol_Border, BLUE500);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.976f, 0.980f, 0.984f, 1.00f));
    ImGui::BeginChild("##nameval", ImVec2(cw, 96), ImGuiChildFlags_Border, ImGuiWindowFlags_NoScrollbar);
    {
        char show[48];
        snprintf(show, sizeof show, "%s", g_name_buf[0] ? g_name_buf : "(空)");
        ImVec2 tp = ImGui::GetCursorScreenPos();
        ImDrawList *d2 = ImGui::GetWindowDrawList();
        d2->AddText(ImVec2(tp.x + 18, tp.y + 28), IM_COL32(24, 24, 27, 255), show);
        if (g_name_old[0] && strcmp(g_name_old, g_name_buf)) {
            char old[48];
            snprintf(old, sizeof old, "原名 %s", g_name_old);
            d2->AddText(ImVec2(tp.x + 22 + ImGui::CalcTextSize(show).x + 26, tp.y + 34),
                        IM_COL32(161, 161, 170, 255), old);
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor(2);
    if (g_name_msg[0]) {
        ImGui::SetCursorScreenPos(ImVec2(x0, y0 + 146));
        ImGui::TextColored(ImVec4(0.863f, 0.149f, 0.149f, 1.00f), "%s", g_name_msg);
    }
    /* 字符键：6 列 x 6 行 = a-z + 0-9 */
    static const char *krow[6] = { "abcdef", "ghijkl", "mnopqr", "stuvwx", "yz0123", "456789" };
    float gap = 10.0f, kw = (cw - 5 * gap) / 6.0f, kh = 76.0f;
    float ky = y0 + 196;
    for (int r = 0; r < 6; r++) {
        for (int c = 0; c < 6; c++) {
            char lab[2] = { krow[r][c], 0 };
            if (lab[0] >= 'a' && lab[0] <= 'z' && g_name_up) lab[0] = (char)(lab[0] - 'a' + 'A');
            ImGui::PushID(100 + r * 6 + c);
            ImGui::SetCursorScreenPos(ImVec2(x0 + c * (kw + gap), ky + r * (kh + gap)));
            if (btn_light(lab, ImVec2(kw, kh))) {
                int n = (int)strlen(g_name_buf);
                if (n < 15) { g_name_buf[n] = lab[0]; g_name_buf[n + 1] = 0; }
                else snprintf(g_name_msg, sizeof g_name_msg, "最多 15 个字符");
                g_need = 1; g_force_frames = 2;
            }
            ImGui::PopID();
        }
    }
    /* 特殊键：_ - 大小写 退格 */
    float fy = ky + 6 * (kh + gap);
    {
        float sw2 = (cw - 3 * gap) / 4.0f;
        const char *sp[4] = { "_", "-", g_name_up ? "小写" : "大写", "退格" };
        for (int c = 0; c < 4; c++) {
            ImGui::PushID(200 + c);
            ImGui::SetCursorScreenPos(ImVec2(x0 + c * (sw2 + gap), fy));
            if (btn_light(sp[c], ImVec2(sw2, 76))) {
                if (c == 0 || c == 1) {
                    int n = (int)strlen(g_name_buf);
                    if (n < 15) { g_name_buf[n] = sp[c][0]; g_name_buf[n + 1] = 0; }
                } else if (c == 2) {
                    g_name_up = !g_name_up;
                } else {
                    int n = (int)strlen(g_name_buf);
                    if (n > 0) g_name_buf[n - 1] = 0;
                }
                g_name_msg[0] = 0;
                g_need = 1; g_force_frames = 2;
            }
            ImGui::PopID();
        }
    }
    /* 取消 / 确定 */
    float by = fy + 76 + gap + 12, bw2 = (cw - gap) * 0.5f;
    ImGui::PushID(300);
    ImGui::SetCursorScreenPos(ImVec2(x0, by));
    if (btn_light("取消", ImVec2(bw2, 92))) {
        g_name_i = -1; g_name_msg[0] = 0; g_need = 1; g_force_frames = 3;
        ALOGI("name cancel");
    }
    ImGui::SetCursorScreenPos(ImVec2(x0 + bw2 + gap, by));
    if (btn_blue("确定", ImVec2(bw2, 92))) {
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
    ImGui::PopID();
}

/* ---- 面板：标题栏（唯一拖动区）/ 固定侧栏 / 可滚内容页；收起态只剩标题栏 ---- */
static void build_panel(void)
{
    float ww = panel_w();
    if (g_pan_x < 0) g_pan_x = 0;
    if (g_pan_y < 0) g_pan_y = 0;
    if (g_pan_x > g_w - 200) g_pan_x = (float)(g_w - 200);
    if (g_pan_y > g_h - 200) g_pan_y = (float)(g_h - 200);
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
    /* 列表实区每帧先清空：只在区域页发布，其它页不命中 → 不会误滚 */
    g_zone_list[0] = g_zone_list[1] = g_zone_list[2] = g_zone_list[3] = 0;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(PAD_X, PAD_Y));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(12, 10));
    build_titlebar(ww);
    if (g_min) {   /* 收起态：只有标题条；清掉侧栏/内容页实区，免上一帧残留命中 */
        g_zone_side[0] = g_zone_side[1] = g_zone_side[2] = g_zone_side[3] = 0;
        g_zone_sheet[0] = g_zone_sheet[1] = g_zone_sheet[2] = g_zone_sheet[3] = 0;
    }
    if (!g_min) ImGui::Dummy(ImVec2(0, COL_GAP - 10));
    if (!g_min && g_name_i >= 0) draw_name_edit();   /* 改名弹层：本帧不画侧栏/内容页 */
    if (!g_min && g_name_i < 0) build_sidebar();
    if (!g_min && g_sheet && g_name_i < 0) {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_ChildBg, WHITE);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 12));
        ImGui::BeginChild("##sheet", ImVec2(0, 0), ImGuiChildFlags_None);
        pub_zone(g_zone_sheet);
        drag_scroll_for(SCR_SHEET);
        if (g_nav == 0) page_regions();
        else if (g_nav == 1) page_log();
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
    if (g_save_pending) { g_save_pending = 0; save_regions(); }   /* 脚本经 WS 改过区域 → 落盘 */
    ImGuiIO &io = ImGui::GetIO();
    eglMakeCurrent(g_dpy, g_surf, g_surf, g_ctx);
    io.DisplaySize = ImVec2((float)sw, (float)sh);
    io.DisplayFramebufferScale = ImVec2(1, 1);
    int fd = -1;
    for (int fi = 0; fi < 64; fi++) if (g_dots[fi].on) { fd = fi; break; }
    if (g_mdown || g_mup_pend || fd >= 0) {
        io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
        io.AddMousePosEvent(g_mdown ? g_mx : (float)g_dots[fd].x,
                            g_mdown ? g_my : (float)g_dots[fd].y);
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
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    eglSwapBuffers(g_dpy, g_surf);
}

static void *render_thread_fn(void *)
{
    int gl_ready = 0;
    while (g_running) {
        int sw = 0, sh = 0, go = 0, i;
        long t0 = now_ms();
        pthread_mutex_lock(&g_mu);
        snapshot_touches();
        go = g_need;
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
            flash_fresh = (now_ms() - g_flash_t < 450 || now_ms() - g_ex_t < 350);
            if (flash_fresh) go = 1;
            for (i = 0; i < 8; i++)
                if (g_rings[i].on && now_ms() - g_rings[i].t < 450) { ring_fresh = 1; go = 1; break; }
        }
        g_need = 0;
        /* 静止跳过：纯 dots 且位置签名不变就不画（按住不动零开销）；
         * 有位移每 tick 都画（mailbox 平滑），按钮/事件/闪烁照常即时。 */
        int urgent = go && (g_mdown || g_mup_pend || g_need_mouse_evt || flash_fresh || ring_fresh
                            || g_force_frames > 0);
        static int last_sig = 0;
        int sig = 0;
        for (i = 0; i < 64; i++) if (g_dots[i].on) sig += g_dots[i].x * 3 + g_dots[i].y * 5 + i + 1;
        if (go && !urgent && sig == last_sig) go = 0;
        else last_sig = sig;
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
                ALOGI("gl ready");
            } else {
                ALOGE("egl init failed");
            }
        }
        if (gl_ready && g_surf == EGL_NO_SURFACE && g_win) {
            ANativeWindow_setBuffersGeometry(g_win, 0, 0, WINDOW_FORMAT_RGBA_8888);
            g_surf = eglCreateWindowSurface(g_dpy, g_cfg, g_win, 0);
        }
        if (gl_ready && go && g_surf != EGL_NO_SURFACE && sw > 0) {
            long b0 = now_ms();
            draw_frame(sw, sh);
            long dt = now_ms() - b0;
            g_frame_ms = g_frame_ms * 0.8 + (double)dt * 0.2;
            if (g_force_frames > 0) g_force_frames--;
            int more = g_mdown || g_mup_pend || g_need_mouse_evt;
            if (g_ov_show) {
                if (!more) for (int k = 0; k < 64; k++) if (g_dots[k].on) { more = 1; break; }
                if (!more && (now_ms() - g_flash_t < 450 || now_ms() - g_ex_t < 350)) more = 1;
                if (!more) for (int k = 0; k < 8; k++)
                    if (g_rings[k].on && now_ms() - g_rings[k].t < 450) { more = 1; break; }
            }
            if (more) g_need = 1;
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

/* ---- JNI ---- */
extern "C" {

JNIEXPORT jint JNICALL Java_VTouchUI_nativeInit(JNIEnv *env, jclass, jint w, jint h)
{
    g_w = w; g_h = h;
    g_pan_x = (float)(w - WIN_W - 40); if (g_pan_x < 0) g_pan_x = 0;
    g_pan_y = 200;
    /* 帧 0 之前的兜底拖动区（渲染后每帧由实区覆盖） */
    g_zone_title[0] = g_pan_x + PAD_X; g_zone_title[1] = g_pan_y + PAD_Y;
    g_zone_title[2] = g_pan_x + WIN_W - PAD_X; g_zone_title[3] = g_pan_y + PAD_Y + TITLE_H;
    char ws[16], hs[16];
    snprintf(ws, sizeof ws, "%d", w); snprintf(hs, sizeof hs, "%d", h);
    char *argv[] = {(char *)"vtouch-ui", (char *)"-w", ws, (char *)"-h", hs,
                    (char *)"-p", (char *)"27183", 0};
    vtouch_set_event_cb(ui_ev_cb);
    vtouch_set_region_cb(ui_region_changed);
    if (vtouch_init(7, argv) != 0) return -2;
    load_regions();   /* 上次落盘的表（regions.conf），没有则空表 */
    ALOGI("panel init %dx%d regions=%d", w, h, vtouch_region_count());
    g_poll_on = 1;
    pthread_create(&g_render_th, 0, render_thread_fn, 0);
    if (g_poll_on) pthread_create(&g_poll_th, 0, poll_thread_fn, 0);
    return 0;
}

JNIEXPORT void JNICALL Java_VTouchUI_nativeOnSurface(JNIEnv *env, jclass, jint id, jobject surf)
{
    if (id != 0) return;
    pthread_mutex_lock(&g_mu);
    if (g_win) ANativeWindow_release(g_win);
    g_win = ANativeWindow_fromSurface(env, surf);
    g_need = 1;
    pthread_mutex_unlock(&g_mu);
    ALOGI("surface ready");
}

JNIEXPORT void JNICALL Java_VTouchUI_nativeDestroy(JNIEnv *, jclass)
{
    g_running = 0;
    pthread_join(g_render_th, 0);
    if (g_poll_on) pthread_join(g_poll_th, 0);
    vtouch_cleanup();
}

} /* extern "C" */
