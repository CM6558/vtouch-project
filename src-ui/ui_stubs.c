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
struct stu_region { char id[16]; int type, a1, a2, a3, a4, enabled; };
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
