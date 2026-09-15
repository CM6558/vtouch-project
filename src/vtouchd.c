/* vtouchd —— 最小版：物理触摸合并 + 虚拟触摸注入（单文件、无 UI、无区域）。
 *
 * 它做什么（只有两件事）：
 *   1) 抓取物理触摸屏（EVIOCGRAB），把内核的 Type-B 触点解析进 phys[]；
 *   2) 在同一帧里把「物理触点 + 虚拟触点（来自 WS 命令的注入）」一起写进一个 uinput 设备，
 *      对外表现为一块统一的触摸屏：应用看到的手指既可能是真手，也可能是注入的。
 *
 * 它有意不做什么（从完整版删掉的功能，别在这里找）：
 *   区域匹配 / 事件推送 / 订阅(sub) / 面板(ImGui) / 区域持久化 / pev 上报 /
 *   出站队列线程 / UI 钩子 / 旋转坐标换算 / 落盘。这些都在 build/_backup_full_* 里的完整版。
 *
 * 用法: vtouchd -w <竖屏宽> -h <竖屏高> [-p 端口] [-v 虚拟槽数]
 * 协议: 一行一条命令（loopback WS，单客户端，新连接踢旧连接）。
 *   所有出站（应答/事件）都是 WS 文本帧，经出站队列由 POLLOUT 事件驱动写出，
 *   慢客户端只排队，永不阻塞触摸热路径。
 *   ping                     -> pong
 *   res                      -> res <lw> <lh> raw <xmin> <xmax> <ymin> <ymax>
 *   reset                    -> ok | err frame
 *   down <slot> <lx> <ly>    -> ok | err point      （各自成一帧）
 *   move <slot> <lx> <ly>    -> ok | err point      （各自成一帧）
 *   up   <slot>              -> ok | err point      （各自成一帧）
 *   begin_frame              -> ok | err frame
 *   point <slot> <down|move|up> <lx> <ly> -> ok | err point   （多指合并进同一帧）
 *   end_frame                -> ok | err frame
 *
 * 失败语义：坏客户端只影响它自己（关连接 + 抬掉它的虚拟触点）；grab 与 uinput 不受影响。
 * 构建: sh scripts/build.sh
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <time.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <unistd.h>

#define MAX_PHYS 64
#define MAX_VIRT 32
#define MAX_LINE 1024
#define MAX_PAYLOAD 1024
#define HTTP_MAX 4096

static volatile sig_atomic_t stop_flag;
static int input_fd = -1, u_fd = -1, listen_fd = -1, client_fd = -1;
static int ws_port = 27183;
static int vslots = 10;                 /* 客户端可用槽号 0..vslots-1 */
static int phys_slots;                  /* 物理屏声明的槽数 = 我们声明给系统的槽数 */
static int total_slots;                 /* = phys_slots（物理/虚拟共用同一个池） */
static int axmin[2], axmax[2];
static int selected_slot;               /* 当前正被解析的物理槽（-1 = 忽略） */
static int logical_width, logical_height;
static int has_pressure, pressure_max;
static int g_seq;                        /* 触点分配序号（池满时顶掉最新虚拟用） */

/* ---- 物理屏能力镜像：validate_device() 抄进来，setup_uinput() 原样搬过去 ----
 * 上层按设备的声明做分类与滤波（触摸大小/压力/掌拒），收窄声明会让注入的触点行为与真手指不一致。 */
#define CAP_LONGS(n) (((n) + 1 + 8 * (int)sizeof(unsigned long) - 1) / (8 * (int)sizeof(unsigned long)))
static unsigned long cap_ev[CAP_LONGS(EV_MAX)];
static unsigned long cap_key[CAP_LONGS(KEY_MAX)];
static unsigned long cap_abs[CAP_LONGS(ABS_MAX)];
static unsigned long cap_prop[CAP_LONGS(INPUT_PROP_MAX)];
static struct input_absinfo cap_ai[ABS_MAX + 1];
static unsigned char cap_ai_ok[ABS_MAX + 1];
static char cap_name[UINPUT_MAX_NAME_SIZE];
static int oid_mod = 32;                 /* tracking id 池大小（与物理 id 量程取小） */

/* 一根触点。oslot/oid 是**我们发给系统的身份**，由统一池分配：
 * 透传客户端槽号/面板 id 会撞号（Android 里 tracking id 就是 pointer id），
 * 而且「物理槽 + 偏移虚拟槽」会让设备声明出 phys+virt 个触点。 */
struct contact {
    int id, x, y, down, pending_up;      /* 来源状态：内核 id 与原始坐标 */
    int oslot, oid, seq;                 /* 下游身份：槽位 / tracking id / 分配序号 */
};
static struct contact phys[MAX_PHYS], virt[MAX_VIRT];

#define WS_IN_MAX (MAX_PAYLOAD + 14)     /* 单帧上限 + 头（2 + 8 扩展长 + 4 掩码） */
static unsigned char ws_in[WS_IN_MAX];
static size_t ws_in_len;
static int frame_open;                   /* begin_frame..end_frame 之间 */
static int frame_seen[MAX_VIRT];
static struct contact staged[MAX_VIRT];  /* 帧内暂存（提交前不碰 virt[]） */
static int next_tracking_id;
/* 整帧写失败时置位：抬手那帧丢了就再没有下一帧去补（phys[i].down 已变 0），
 * 系统里那根手指会永久按着 —— 所以必须重发同一帧，直到写成功或判定 uinput 真死。 */
static volatile int g_reemit;
static int g_emit_fail;

static int write_full(int fd, const void *buf, size_t len);
static void drop_client(void);
static void sendq_clear(void);   /* 定义在下方出站队列处 */
static int sendq_push(const unsigned char *p, size_t n);
static int alloc_oslot(void);
static int alloc_oid(void);

static int parse_long(const char *s, long lo, long hi, int *out)
{
    char *e; long v;
    if (!s || !*s) return -1;
    errno = 0; v = strtol(s, &e, 10);
    if (errno || *e || v < lo || v > hi) return -1;
    *out = (int)v; return 0;
}

static int bit(const unsigned long *b, int n)
{
    return (int)((b[(unsigned)n / (8 * sizeof(unsigned long))] >> ((unsigned)n % (8 * sizeof(unsigned long)))) & 1UL);
}

/* 逻辑坐标（竖屏，脚本用的那一套）→ 内核 raw 轴值 */
static int logical_to_raw(int logical, int axis, int *raw)
{
    int size = axis ? logical_height : logical_width;
    long span = (long)axmax[axis] - axmin[axis];
    long value;
    if (size < 2 || logical < 0 || logical >= size) return -1;
    value = (long)axmin[axis] + ((long)logical * span + (size - 1) / 2) / (size - 1);
    if (value < axmin[axis]) value = axmin[axis];
    if (value > axmax[axis]) value = axmax[axis];
    *raw = (int)value; return 0;
}

/* 认一块设备是不是 Type-B 触摸屏：EV_ABS 里必须有槽/tracking id/XY 四轴，槽数合规，
 * X/Y 量程有效；并把它的能力声明整份抄进 cap_*（供 setup_uinput 镜像）。 */
static int validate_device(const char *p, int *slots, int *xmin, int *xmax, int *ymin, int *ymax)
{
    unsigned long ev[CAP_LONGS(EV_MAX)];
    unsigned long abs[CAP_LONGS(ABS_MAX)];
    unsigned long prop[CAP_LONGS(INPUT_PROP_MAX)];
    struct input_absinfo a;
    int f, c;
    memset(ev, 0, sizeof ev); memset(abs, 0, sizeof abs); memset(prop, 0, sizeof prop);
    f = open(p, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (f < 0) return -1;
    if (ioctl(f, EVIOCGBIT(0, sizeof ev), ev) < 0 ||
        ioctl(f, EVIOCGBIT(EV_ABS, sizeof abs), abs) < 0 ||
        ioctl(f, EVIOCGPROP(sizeof prop), prop) < 0) { close(f); return -1; }
    if (!bit(abs, ABS_MT_SLOT) || !bit(abs, ABS_MT_TRACKING_ID) ||
        !bit(abs, ABS_MT_POSITION_X) || !bit(abs, ABS_MT_POSITION_Y)) { close(f); return -1; }
    if (ioctl(f, EVIOCGABS(ABS_MT_SLOT), &a) < 0 || a.minimum < 0 || a.maximum >= MAX_PHYS) { close(f); return -1; }
    *slots = a.maximum - a.minimum + 1;
    if (ioctl(f, EVIOCGABS(ABS_MT_POSITION_X), &a) < 0 || a.maximum <= a.minimum) { close(f); return -1; }
    *xmin = a.minimum; *xmax = a.maximum;
    if (ioctl(f, EVIOCGABS(ABS_MT_POSITION_Y), &a) < 0 || a.maximum <= a.minimum) { close(f); return -1; }
    *ymin = a.minimum; *ymax = a.maximum;
    has_pressure = 0; pressure_max = 0;
    if (bit(abs, ABS_MT_PRESSURE) && ioctl(f, EVIOCGABS(ABS_MT_PRESSURE), &a) == 0 && a.maximum > 0) {
        has_pressure = 1; pressure_max = a.maximum;
    }
    memcpy(cap_ev, ev, sizeof ev); memcpy(cap_abs, abs, sizeof abs); memcpy(cap_prop, prop, sizeof prop);
    memset(cap_key, 0, sizeof cap_key); memset(cap_ai_ok, 0, sizeof cap_ai_ok);
    if (ioctl(f, EVIOCGBIT(EV_KEY, sizeof cap_key), cap_key) < 0) { close(f); return -1; }
    for (c = 0; c <= ABS_MAX; c++)
        if (bit(cap_abs, c) && ioctl(f, EVIOCGABS(c), &cap_ai[c]) == 0) cap_ai_ok[c] = 1;
    memset(cap_name, 0, sizeof cap_name);
    if (ioctl(f, EVIOCGNAME(sizeof cap_name - 1), cap_name) < 0) cap_name[0] = 0;
    if (cap_ai_ok[ABS_MT_TRACKING_ID]) {
        int mx = cap_ai[ABS_MT_TRACKING_ID].maximum;
        oid_mod = (mx >= 31) ? 32 : mx + 1;
        if (oid_mod < 2) oid_mod = 2;
    }
    close(f);
    return 0;
}

/* 动态发现：扫 event0..63 找第一块 Type-B 触摸屏，不写死节点号 */
static int discover(char *out, size_t n)
{
    int k;
    for (k = 0; k < 64; k++) {
        snprintf(out, n, "/dev/input/event%d", k);
        if (validate_device(out, &phys_slots, &axmin[0], &axmax[0], &axmin[1], &axmax[1]) == 0) return 0;
    }
    return -1;
}

/* ---- uinput 写帧：先组 iovec，末尾一次 writev ---- */
#define MAX_IOV 512
static struct input_event ev_buf[MAX_IOV];
static struct iovec ev_iov[MAX_IOV];
static int ev_n;

static void ev_add(int t, int c, int v)
{
    if (ev_n >= MAX_IOV) return;
    ev_buf[ev_n] = (struct input_event){ .type = (unsigned short)t, .code = (unsigned short)c, .value = v };
    ev_iov[ev_n].iov_base = &ev_buf[ev_n];
    ev_iov[ev_n].iov_len = sizeof(struct input_event);
    ev_n++;
}

/* uinput 是 O_NONBLOCK 打开的：队列满会 EAGAIN，短暂等一等（3×20ms），
 * 只有一直不可写才算真错（调用方据此重发/收摊）。 */
static ssize_t uinput_writev_retry(void)
{
    int k;
    ssize_t n;
    do { n = writev(u_fd, ev_iov, ev_n); } while (n < 0 && errno == EINTR);
    if (n >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) return n;
    for (k = 0; k < 3; k++) {
        struct pollfd p = { u_fd, POLLOUT, 0 };
        if (poll(&p, 1, 20) > 0 && (p.revents & POLLOUT)) {
            do { n = writev(u_fd, ev_iov, ev_n); } while (n < 0 && errno == EINTR);
            if (n >= 0) return n;
            if (errno != EAGAIN && errno != EWOULDBLOCK) return n;
        }
    }
    return -1;
}

/* 提交这一帧；短写要把剩下的 iovec 全补完（只补一个会丢帧尾 SYN_REPORT）。 */
static int emit_iov_writev(void)
{
    ssize_t n, need = 0;
    size_t off;
    int i;
    if (u_fd < 0) return -1;
    for (i = 0; i < ev_n; i++) need += ev_iov[i].iov_len;
    n = uinput_writev_retry();
    if (n < 0) { ev_n = 0; return -1; }
    if ((size_t)n < (size_t)need) {
        off = (size_t)n;
        for (i = 0; i < ev_n && off > 0; i++) {
            if (off < ev_iov[i].iov_len) {
                const unsigned char *p = (const unsigned char *)ev_iov[i].iov_base + off;
                size_t left = ev_iov[i].iov_len - off;
                while (left) {
                    ssize_t k;
                    do { k = write(u_fd, p, left); } while (k < 0 && errno == EINTR);
                    if (k <= 0) { ev_n = 0; return -1; }
                    p += k; left -= (size_t)k;
                }
                i++;
                break;
            }
            off -= ev_iov[i].iov_len;
        }
        for (; i < ev_n; i++) {
            const unsigned char *p = (const unsigned char *)ev_iov[i].iov_base;
            size_t left = ev_iov[i].iov_len;
            while (left) {
                ssize_t k;
                do { k = write(u_fd, p, left); } while (k < 0 && errno == EINTR);
                if (k <= 0) { ev_n = 0; return -1; }
                p += k; left -= (size_t)k;
            }
        }
    }
    ev_n = 0;
    return 0;
}

/* 合并设备的能力声明 = 照抄物理屏；只有 4 处真冲突取相似值。 */
static int setup_uinput(void)
{
    struct uinput_setup s; struct uinput_abs_setup a;
    int t, c, p;
    u_fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (u_fd < 0) return -1;
    for (t = 0; t <= EV_MAX; t++) if (bit(cap_ev, t) && ioctl(u_fd, UI_SET_EVBIT, t) < 0) goto fail;
    if (ioctl(u_fd, UI_SET_EVBIT, EV_SYN) < 0 || ioctl(u_fd, UI_SET_EVBIT, EV_KEY) < 0 ||
        ioctl(u_fd, UI_SET_EVBIT, EV_ABS) < 0) goto fail;
    for (c = 0; c <= KEY_MAX; c++) if (bit(cap_key, c) && ioctl(u_fd, UI_SET_KEYBIT, c) < 0) goto fail;
    if (ioctl(u_fd, UI_SET_KEYBIT, BTN_TOUCH) < 0 ||
        ioctl(u_fd, UI_SET_KEYBIT, BTN_TOOL_FINGER) < 0) goto fail;
    for (c = 0; c <= ABS_MAX; c++) {
        if (!bit(cap_abs, c)) continue;
        if (ioctl(u_fd, UI_SET_ABSBIT, c) < 0) goto fail;
        memset(&a, 0, sizeof a); a.code = (unsigned short)c;
        if (cap_ai_ok[c]) a.absinfo = cap_ai[c];               /* fuzz/flat/resolution 一起抄 */
        if (c == ABS_MT_POSITION_X) { a.absinfo.minimum = axmin[0]; a.absinfo.maximum = axmax[0]; }
        if (c == ABS_MT_POSITION_Y) { a.absinfo.minimum = axmin[1]; a.absinfo.maximum = axmax[1]; }
        /* 冲突① 真机 ABS_MT_TOOL_TYPE 量程常是 0..0，装不下 tool 值 → 抬到能装 PALM */
        if (c == ABS_MT_TOOL_TYPE && a.absinfo.maximum < MT_TOOL_PALM) a.absinfo.maximum = MT_TOOL_PALM;
        /* 冲突② 槽数上限不能小于我们真正要用的池 */
        if (c == ABS_MT_SLOT && a.absinfo.maximum < total_slots - 1) a.absinfo.maximum = total_slots - 1;
        /* 冲突③ tracking id 量程至少要装下 id 池 */
        if (c == ABS_MT_TRACKING_ID && a.absinfo.maximum < oid_mod - 1) a.absinfo.maximum = oid_mod - 1;
        if (ioctl(u_fd, UI_ABS_SETUP, &a) < 0) goto fail;
    }
    /* 物理屏万一没声明这四根轴也要补齐，否则合并设备发不出 MT 事件 */
    {
        static const int need[4] = { ABS_MT_SLOT, ABS_MT_TRACKING_ID, ABS_MT_POSITION_X, ABS_MT_POSITION_Y };
        for (t = 0; t < 4; t++) {
            int code = need[t];
            if (bit(cap_abs, code)) continue;
            if (ioctl(u_fd, UI_SET_ABSBIT, code) < 0) goto fail;
            memset(&a, 0, sizeof a); a.code = (unsigned short)code;
            if (code == ABS_MT_POSITION_X) { a.absinfo.minimum = axmin[0]; a.absinfo.maximum = axmax[0]; }
            else if (code == ABS_MT_POSITION_Y) { a.absinfo.minimum = axmin[1]; a.absinfo.maximum = axmax[1]; }
            else if (code == ABS_MT_SLOT) a.absinfo.maximum = total_slots - 1;
            else a.absinfo.maximum = oid_mod - 1;
            if (ioctl(u_fd, UI_ABS_SETUP, &a) < 0) goto fail;
        }
    }
    for (p = 0; p <= INPUT_PROP_MAX; p++) if (bit(cap_prop, p) && ioctl(u_fd, UI_SET_PROPBIT, p) < 0) goto fail;
    /* 冲突④（名字/ID）：什么都不声明时系统会把设备当触控板画出鼠标指针 → INPUT_PROP_DIRECT 必须有；
     * 名字加后缀、bus 用 BUS_VIRTUAL，避免被当成与物理屏同一设备而忽略。 */
    if (!bit(cap_prop, INPUT_PROP_DIRECT) && ioctl(u_fd, UI_SET_PROPBIT, INPUT_PROP_DIRECT) < 0) goto fail;
    memset(&s, 0, sizeof s); s.id.bustype = BUS_VIRTUAL;
    if (cap_name[0]) snprintf((char *)s.name, UINPUT_MAX_NAME_SIZE, "%s_vtouch", cap_name);
    else strncpy((char *)s.name, "vtouch-merged", UINPUT_MAX_NAME_SIZE - 1);
    if (ioctl(u_fd, UI_DEV_SETUP, &s) < 0) goto fail;
    if (ioctl(u_fd, UI_DEV_CREATE) < 0) goto fail;
    return 0;
fail:
    ioctl(u_fd, UI_DEV_DESTROY); close(u_fd); u_fd = -1; return -1;
}

/* region 线程句柄（定义在下方转发区；cleanup 在前，先声明） */
static pthread_t region_th;
static int region_started;

/* 收尾顺序固定：region 线程 → client → listen → 放 grab → 销毁 uinput。只跑一次。 */
static void cleanup(void)
{
    static int cleaned;
    if (cleaned) return;
    cleaned = 1;
    stop_flag = 1;   /* 先停 region 线程（init 失败路径下 poll 循环没跑过，也要保证它能退出） */
    if (region_started) { region_started = 0; pthread_join(region_th, NULL); }
    if (client_fd >= 0) { close(client_fd); client_fd = -1; }
    if (listen_fd >= 0) { close(listen_fd); listen_fd = -1; }
    if (input_fd >= 0) {
        ioctl(input_fd, EVIOCGRAB, 0);
        close(input_fd); input_fd = -1;
    }
    if (u_fd >= 0) { ioctl(u_fd, UI_DEV_DESTROY); close(u_fd); u_fd = -1; }
}

/* BTN_TOUCH 只认真的进了帧的触点（有下游身份） */
static int any_emitted(void)
{
    int i;
    for (i = 0; i < phys_slots; i++) if (phys[i].down && phys[i].oslot >= 0) return 1;
    for (i = 0; i < vslots; i++) if (virt[i].down && virt[i].oslot >= 0) return 1;
    return 0;
}

/* 池满且来的是物理手指：顶掉「最新分配的那个虚拟触点」，把身份让给真人 */
static int evict_newest_virtual(void)
{
    int i, best = -1;
    for (i = 0; i < vslots; i++)
        if (virt[i].down && (best < 0 || virt[i].seq > virt[best].seq)) best = i;
    if (best < 0) return 0;
    virt[best].down = 0; virt[best].pending_up = 1;
    fprintf(stderr, "vtouchd: 身份池满 → 顶掉虚拟槽 %d\n", best);
    return 1;
}

/* 一帧的固定顺序（每一步都有理由）：
 *   ① 待抬的触点先发 ABS_MT_TRACKING_ID=-1（用当前身份，身份要到这一帧写成功才释放）
 *   ② 物理触点（在这里才分配下游身份 —— 中途池满就下一轮补发）
 *   ③ 虚拟触点（身份在 WS 命令里就分好了）
 *   ④ BTN_TOUCH / BTN_TOOL_FINGER（只有真的发了触点才置 1）
 *   ⑤ SYN_REPORT，整个帧一次 writev 提交
 * 写失败：绝不清 pending_up、绝不释放身份，置 g_reemit 由 poll 循环重发。 */
static int emit_frame(void)
{
    int i, s2, nid;
    if (u_fd < 0) return -1;
    ev_n = 0;
    for (i = 0; i < phys_slots; i++) if (phys[i].pending_up && phys[i].oslot >= 0) {
        ev_add(EV_ABS, ABS_MT_SLOT, phys[i].oslot);
        ev_add(EV_ABS, ABS_MT_TRACKING_ID, -1);
    }
    for (i = 0; i < vslots; i++) if (virt[i].pending_up && virt[i].oslot >= 0) {
        ev_add(EV_ABS, ABS_MT_SLOT, virt[i].oslot);
        ev_add(EV_ABS, ABS_MT_TRACKING_ID, -1);
    }
    for (i = 0; i < phys_slots; i++) {
        if (!phys[i].down) continue;
        if (phys[i].oslot < 0) {
            s2 = alloc_oslot();
            if (s2 < 0) { if (evict_newest_virtual()) g_reemit = 1; continue; }
            nid = alloc_oid();
            if (nid < 0) { g_reemit = 1; continue; }
            phys[i].oslot = s2; phys[i].oid = nid; phys[i].seq = ++g_seq;
        }
        ev_add(EV_ABS, ABS_MT_SLOT, phys[i].oslot);
        ev_add(EV_ABS, ABS_MT_TRACKING_ID, phys[i].oid);
        ev_add(EV_ABS, ABS_MT_POSITION_X, phys[i].x);
        ev_add(EV_ABS, ABS_MT_POSITION_Y, phys[i].y);
        ev_add(EV_ABS, ABS_MT_TOOL_TYPE, MT_TOOL_FINGER);
        if (has_pressure) ev_add(EV_ABS, ABS_MT_PRESSURE, pressure_max);
    }
    for (i = 0; i < vslots; i++) {
        if (!virt[i].down || virt[i].oslot < 0) continue;
        ev_add(EV_ABS, ABS_MT_SLOT, virt[i].oslot);
        ev_add(EV_ABS, ABS_MT_TRACKING_ID, virt[i].oid);
        ev_add(EV_ABS, ABS_MT_POSITION_X, virt[i].x);
        ev_add(EV_ABS, ABS_MT_POSITION_Y, virt[i].y);
        ev_add(EV_ABS, ABS_MT_TOOL_TYPE, MT_TOOL_FINGER);
        if (has_pressure) ev_add(EV_ABS, ABS_MT_PRESSURE, pressure_max);
    }
    ev_add(EV_KEY, BTN_TOUCH, any_emitted());
    ev_add(EV_KEY, BTN_TOOL_FINGER, any_emitted());
    ev_add(EV_SYN, SYN_REPORT, 0);
    if (emit_iov_writev() < 0) { g_reemit = 1; return -1; }
    for (i = 0; i < phys_slots; i++) phys[i].pending_up = 0;
    for (i = 0; i < vslots; i++) virt[i].pending_up = 0;
    for (i = 0; i < phys_slots; i++) if (!phys[i].down) { phys[i].oslot = -1; phys[i].oid = -1; }
    for (i = 0; i < vslots; i++) if (!virt[i].down) { virt[i].oslot = -1; virt[i].oid = -1; }
    return 0;
}

/* 身份池（物理 + 虚拟共用）：槽池 = 物理槽数，id 池 = oid_mod。
 * 分配时机：虚拟触点在 WS 命令里（池满能如实回 err point）；物理触点在发帧时。
 * 释放时机：那一帧写成功之后。 */
static int slot_taken(int s)
{
    int i;
    for (i = 0; i < phys_slots; i++) if (phys[i].oslot == s) return 1;
    for (i = 0; i < vslots; i++) if (virt[i].oslot == s || staged[i].oslot == s) return 1;
    return 0;
}
static int id_taken(int id)
{
    int i;
    for (i = 0; i < phys_slots; i++) if (phys[i].oid == id) return 1;
    for (i = 0; i < vslots; i++) if (virt[i].oid == id || staged[i].oid == id) return 1;
    return 0;
}
static int alloc_oslot(void)
{
    int s;
    for (s = 0; s < phys_slots; s++) if (!slot_taken(s)) return s;
    return -1;
}
static int alloc_oid(void)
{
    int k, id;
    for (k = 0; k < oid_mod; k++) {
        id = (next_tracking_id + k) % oid_mod;
        if (!id_taken(id)) { next_tracking_id = (id + 1) % oid_mod; return id; }
    }
    return -1;
}

/* 单点命令与帧内命令共用这一段：state 只认 down/move/up */
static int set_virtual(struct contact *state, int slot, const char *name, int x, int y)
{
    if (!strcmp(name, "down")) {
        int s2, nid;
        if (state[slot].down || state[slot].pending_up) return -1;
        s2 = alloc_oslot();
        if (s2 < 0) return -1;
        nid = alloc_oid();
        if (nid < 0) return -1;
        state[slot].oslot = s2; state[slot].oid = nid; state[slot].seq = ++g_seq;
        state[slot].id = next_tracking_id;
        state[slot].down = 1;
    } else if (!strcmp(name, "move")) {
        if (!state[slot].down) return -1;
    } else if (!strcmp(name, "up")) {
        if (!state[slot].down) return -1;
        state[slot].down = 0; state[slot].pending_up = 1;
    } else return -1;
    state[slot].x = x; state[slot].y = y;
    return 0;
}

/* 客户端断开/被踢：抬掉它的虚拟触点并归还「帧内已分配但没提交」的身份。
 * 少了这段，客户端在 begin_frame..end_frame 中断开会永久占住池里的身份（之后注入全回 err point）。 */
static void owner_reset(void)
{
    int i;
    for (i = 0; i < vslots; i++) if (virt[i].down) { virt[i].down = 0; virt[i].pending_up = 1; }
    memcpy(staged, virt, sizeof staged);
    memset(frame_seen, 0, sizeof frame_seen);
    frame_open = 0;
    if (emit_frame() < 0) g_reemit = 1;
}

/* raw -> logical：供事件上报把物理坐标转回逻辑坐标（与 logical_to_raw 互逆的取整方向）。 */
static int raw_to_logical(int raw, int axis, int *logical)
{
    int size = axis ? logical_height : logical_width;
    long span = (long)axmax[axis] - axmin[axis];
    long v;
    if (size < 2 || span <= 0) return -1;
    if (raw < axmin[axis]) raw = axmin[axis];
    if (raw > axmax[axis]) raw = axmax[axis];
    v = ((long)(raw - axmin[axis]) * (size - 1) + span / 2) / span;
    if (v < 0) v = 0;
    if (v > size - 1) v = size - 1;
    *logical = (int)v;
    return 0;
}

static uint64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);   /* 单调时钟：系统时间调整不影响手势间隔 */
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

/* ---- 事件队列（方案 §4.3）：订阅者独立 SPSC，本分支唯一消费者是 region 线程。
 * 单生产者（poll 线程）+ 单消费者（region 线程）：tail 只由生产者写，head 正常只由
 * 消费者 CAS 推进；生产者只在满队丢最旧时 fetch_add head（与消费者 CAS 线性化，
 * 消费者可能 benign 地多处理一条已被跳过的边沿，状态机对此幂等）。
 * 容量 64，主线程 push 永不阻塞（纯内存操作，微秒级）。
 * 溢出：① 队尾同 slot move 就地覆盖合并；② 仍满则丢最旧（down/up 不丢——正常 1ms
 * 消费下到不了丢弃分支，事件流是增量状态，丢旧 move 不影响最终语义）。 */
#define VTQ_CAP 64
struct vt_ev {
    int slot;       /* 物理槽号（virt=0）或虚拟槽号（virt=1） */
    int action;     /* 0=up 1=down 2=move */
    int x, y;       /* 逻辑坐标 */
    uint64_t ts;    /* down=按下时刻，move/up=帧到达时刻（手势识别预留） */
    int virt;       /* 0=物理 1=虚拟（区域线程过滤 virt=1，不自激） */
};
static struct vt_ev vtq_buf[VTQ_CAP];
static atomic_uint vtq_h, vtq_t;

static void vtq_push(struct vt_ev ev)
{
    unsigned h = atomic_load_explicit(&vtq_h, memory_order_acquire);
    unsigned t = atomic_load_explicit(&vtq_t, memory_order_relaxed);
    unsigned i;
    if (t - h < VTQ_CAP) {
        vtq_buf[t % VTQ_CAP] = ev;
        atomic_store_explicit(&vtq_t, t + 1, memory_order_release);
        return;
    }
    if (ev.action == 2) {
        for (i = t; i > h; i--) {   /* 新 move：队尾同 slot move 直接覆盖 */
            struct vt_ev *q = &vtq_buf[(i - 1) % VTQ_CAP];
            if (q->slot == ev.slot && q->virt == ev.virt && q->action == 2) { *q = ev; return; }
        }
        if (vtq_buf[h % VTQ_CAP].action == 2) {   /* 丢最旧（是个 move 才丢） */
            atomic_fetch_add_explicit(&vtq_h, 1, memory_order_acq_rel);
            t = atomic_load_explicit(&vtq_t, memory_order_relaxed);
            vtq_buf[t % VTQ_CAP] = ev;
            atomic_store_explicit(&vtq_t, t + 1, memory_order_release);
            return;
        }
        fprintf(stderr, "vtouchd: 事件队列满且无 move 可合并，丢新 move slot%d\n", ev.slot);
        return;
    }
    fprintf(stderr, "vtouchd: 事件队列满，丢最旧（action=%d）\n", vtq_buf[h % VTQ_CAP].action);
    atomic_fetch_add_explicit(&vtq_h, 1, memory_order_acq_rel);
    t = atomic_load_explicit(&vtq_t, memory_order_relaxed);
    vtq_buf[t % VTQ_CAP] = ev;
    atomic_store_explicit(&vtq_t, t + 1, memory_order_release);
}

static int vtq_pop(struct vt_ev *out)
{
    unsigned h, t;
    for (;;) {
        h = atomic_load_explicit(&vtq_h, memory_order_relaxed);
        t = atomic_load_explicit(&vtq_t, memory_order_acquire);
        if (h >= t) return 0;
        if (atomic_compare_exchange_weak_explicit(&vtq_h, &h, h + 1,
                memory_order_acq_rel, memory_order_relaxed)) break;
    }
    *out = vtq_buf[h % VTQ_CAP];
    return 1;
}

/* ---- 区域表（方案语义零变化：结构与判定照搬完整版，读写加锁）。
 * 写者：poll 线程（region clear/add 命令）。读者：region 线程（每事件拷贝快照）。
 * 状态表（slot_in/slot_hit/slot_last）从主线程移入 region 线程私有，外加 r_down
 * 做边沿（完整版用主线程 ps_down 判定新按/刚抬起，增量驱动改用本线程私有 r_down）。 */
#define MAX_REGIONS 32
#define REGION_ID_MAX 15
struct region {
    char id[REGION_ID_MAX + 1];
    int type;              /* 0=rect 1=circle */
    int enabled;
    int a1, a2, a3, a4;    /* rect: x1 y1 x2 y2; circle: cx cy r（a4 占位） */
};
static struct region regions[MAX_REGIONS];
static int region_count;
static pthread_mutex_t region_mu = PTHREAD_MUTEX_INITIALIZER;

static unsigned char slot_in[MAX_PHYS][MAX_REGIONS];    /* 上一事件该槽是否在区域内（region 线程私有） */
static unsigned char slot_hit[MAX_PHYS][MAX_REGIONS];   /* 本次按下是否命中（up 依据，region 线程私有） */
static int slot_last_x[MAX_PHYS], slot_last_y[MAX_PHYS]; /* 上次推送位置（region 线程私有） */
static unsigned char r_down[MAX_PHYS];                   /* region 线程视角的槽按下边沿 */

static void regions_clear(void)
{
    pthread_mutex_lock(&region_mu);
    region_count = 0;
    memset(regions, 0, sizeof regions);
    pthread_mutex_unlock(&region_mu);
    memset(slot_in, 0, sizeof slot_in);
    memset(slot_hit, 0, sizeof slot_hit);
    memset(slot_last_x, 0, sizeof slot_last_x);
    memset(slot_last_y, 0, sizeof slot_last_y);
    memset(r_down, 0, sizeof r_down);
}

static int region_add(const char *id, int type, int a1, int a2, int a3, int a4, int enabled)
{
    struct region *rg;
    size_t n;
    int i, rc = -1;
    if (!id) return -1;
    n = strlen(id);
    if (n == 0 || n > REGION_ID_MAX) return -1;
    if (type != 0 && type != 1) return -1;
    if (a1 < 0 || a2 < 0 || a3 < 0 || a4 < 0) return -1;
    if (a1 >= logical_width || a2 >= logical_height) return -1;
    pthread_mutex_lock(&region_mu);
    for (i = 0; i < region_count; i++) {   /* 同 id 原地更新（开关/显隐只改属性） */
        if (strcmp(regions[i].id, id) == 0) {
            rg = &regions[i];
            rg->type = type;
            rg->enabled = enabled ? 1 : 0;
            rg->a1 = a1; rg->a2 = a2; rg->a3 = a3; rg->a4 = a4;
            fprintf(stderr, "vtouchd: region upd %s type%d %d,%d,%d,%d en%d (total %d)\n",
                rg->id, type, a1, a2, a3, a4, rg->enabled, region_count);
            rc = 0;
            break;
        }
    }
    if (rc != 0 && region_count < MAX_REGIONS) {
        rg = &regions[region_count++];
        memset(rg, 0, sizeof *rg);
        memcpy(rg->id, id, n);
        rg->type = type;
        rg->enabled = enabled ? 1 : 0;
        rg->a1 = a1; rg->a2 = a2; rg->a3 = a3; rg->a4 = a4;
        fprintf(stderr, "vtouchd: region add %s type%d %d,%d,%d,%d en%d (total %d)\n",
            rg->id, type, a1, a2, a3, a4, rg->enabled, region_count);
        rc = 0;
    }
    pthread_mutex_unlock(&region_mu);
    return rc;
}

static int region_hit(const struct region *rg, int lx, int ly)
{
    if (!rg->enabled) return 0;
    if (rg->type == 1) {
        int dx = lx - rg->a1, dy = ly - rg->a2;
        return dx * dx + dy * dy <= rg->a3 * rg->a3;
    }
    return lx >= rg->a1 && lx <= rg->a3 && ly >= rg->a2 && ly <= rg->a4;
}

/* 订阅通道位掩码（0 = 未订阅）：裸 sub 两个通道都订，sub region 只订区域事件、
 * sub phys 只订原始轨迹——纯区域脚本不必再白收 pev。 */
#define SUB_REGION 1
#define SUB_PHYS   2
static int sub_mask;   /* 只由 poll 线程写；region 线程读（最坏少推/多推一条，见 region_ev_send） */

/* pev 边沿跟踪（主线程私有）：每 SYN 必走，与订阅无关——无订阅时也不能冻住，
 * 否则按住的手指每帧重报 down，enter/move/up 全丢。 */
static int ps_down[MAX_PHYS], ps_x[MAX_PHYS], ps_y[MAX_PHYS];

/* 区域事件上报（region 线程上下文）：只进出站队列，绝不直写 socket。
 * client_fd/sub_mask 是 poll 线程写的，读到过渡值最多导致一条事件走/留，
 * 下一条即恢复——不为此加锁（锁会把 WS 抖动传回判断线程）。 */
static void region_ev_send(const char *id, const char *ev, int slot, int lx, int ly)
{
    char msg[96];
    int n;
    n = snprintf(msg, sizeof msg, "region_ev %s %s %d %d %d", id, ev, slot, lx, ly);
    if (n <= 0 || (size_t)n >= sizeof msg) return;
    if (client_fd < 0 || !(sub_mask & SUB_REGION)) {
        fprintf(stderr, "vtouchd: ev %s %s slot%d %d,%d (NO-CLIENT/UNSUB)\n",
            id, ev, slot, lx, ly);
        return;
    }
    sendq_push((const unsigned char *)msg, (size_t)n);
    fprintf(stderr, "vtouchd: ev %s %s slot%d %d,%d\n", id, ev, slot, lx, ly);
}

/* 五事件判定（方案 §4.4：完整版 region_match 逻辑原样搬入，驱动从“每 SYN 扫全表”
 * 改为“逐 vt_ev 增量处理”；重复 down 按 move 走、空闲 slot 的 up/move 忽略，
 * 使队列溢出时的 benign 重复不产生错误事件）。 */
static void region_on_ev(const struct vt_ev *ev)
{
    struct region tbl[MAX_REGIONS];
    int n, rid, hit, s;
    s = ev->slot;
    if (s < 0 || s >= MAX_PHYS) return;
    pthread_mutex_lock(&region_mu);
    n = region_count;
    if (n > MAX_REGIONS) n = MAX_REGIONS;
    if (n > 0) memcpy(tbl, regions, (size_t)n * sizeof tbl[0]);
    pthread_mutex_unlock(&region_mu);
    if (n <= 0) return;
    if (ev->action == 1) memset(slot_hit[s], 0, sizeof slot_hit[s]);   /* 新按下一轮：命中从零算 */
    for (rid = 0; rid < n; rid++) {
        if (!tbl[rid].enabled) { slot_in[s][rid] = 0; continue; }
        hit = region_hit(&tbl[rid], ev->x, ev->y);
        if (ev->action == 1) {
            if (!r_down[s]) {
                r_down[s] = 1;
                if (hit) { slot_hit[s][rid] = 1; region_ev_send(tbl[rid].id, "down", s, ev->x, ev->y); }
                slot_last_x[s] = ev->x; slot_last_y[s] = ev->y;   /* 按下位置即 move 基准 */
            } else goto do_move;   /* 重复 down：当 move 处理 */
        } else if (ev->action == 2) {
            if (!r_down[s]) continue;
do_move:
            if (hit && !slot_in[s][rid]) region_ev_send(tbl[rid].id, "enter", s, ev->x, ev->y);
            else if (!hit && slot_in[s][rid]) region_ev_send(tbl[rid].id, "exit", s, ev->x, ev->y);
            /* move：仅在已在区域内时按位置变化推（enter/down 帧不重复推） */
            if (hit && slot_in[s][rid] && (slot_last_x[s] != ev->x || slot_last_y[s] != ev->y)) {
                slot_last_x[s] = ev->x; slot_last_y[s] = ev->y;
                region_ev_send(tbl[rid].id, "move", s, ev->x, ev->y);
            }
        } else {
            if (r_down[s] && slot_hit[s][rid] && hit)
                region_ev_send(tbl[rid].id, "up", s, ev->x, ev->y);   /* 纯监听：只推事件，不代点 */
            r_down[s] = 0;
        }
        slot_in[s][rid] = (r_down[s] && hit) ? 1 : 0;
    }
}

static void *region_thread(void *arg)
{
    struct vt_ev ev;
    struct timespec idle = { 0, 1000000 };   /* 队空 1ms 轮询（方案 §4.4 草图）：无事件零消耗 */
    (void)arg;
    pthread_setname_np(pthread_self(), "vt-region");   /* 线程命名：top -H 归因用 */
    while (!stop_flag) {
        if (!vtq_pop(&ev)) { nanosleep(&idle, NULL); continue; }
        if (ev.virt) continue;                    // 虚拟触摸不匹配（不自激）
        region_on_ev(&ev);
    }
    return NULL;
}

/* 每 SYN 必走（方案 §4.1）：emit 之后广播，消费者看到的是完整帧快照。
 * 只推状态变化（down/up/move），静止不刷屏；pev 进出站队列（订阅才推），
 * ps_* 边沿跟踪无条件更新。纯内存操作，不碰 socket，不阻塞注入。 */
static void broadcast_phys(void)
{
    int i, lx, ly;
    uint64_t ts = now_ns();
    for (i = 0; i < phys_slots; i++) {
        const char *st;
        int action;
        if (phys[i].down && !ps_down[i]) { st = "down"; action = 1; }
        else if (!phys[i].down && ps_down[i]) { st = "up"; action = 0; }
        else if (phys[i].down && (phys[i].x != ps_x[i] || phys[i].y != ps_y[i])) { st = "move"; action = 2; }
        else continue;
        if (raw_to_logical(phys[i].x, 0, &lx) < 0 || raw_to_logical(phys[i].y, 1, &ly) < 0) continue;
        if (phys[i].down) { ps_down[i] = 1; ps_x[i] = phys[i].x; ps_y[i] = phys[i].y; }
        else ps_down[i] = 0;
        vtq_push((struct vt_ev){ .slot = i, .action = action, .x = lx, .y = ly, .ts = ts, .virt = 0 });
        if (client_fd >= 0 && (sub_mask & SUB_PHYS)) {
            char msg[64];
            int n = snprintf(msg, sizeof msg, "pev %d %s %d %d", i, st, lx, ly);
            if (n > 0 && (size_t)n < sizeof msg)
                sendq_push((const unsigned char *)msg, (size_t)n);
        }
    }
}

/* 虚拟触摸进事件队列（virt=1，区域线程过滤不自激）：单条命令在 emit 成功后推，
 * end_frame 在提交成功后按差分推。 */
static void push_virt_ev(int slot, int action, int lx, int ly)
{
    vtq_push((struct vt_ev){ .slot = slot, .action = action, .x = lx, .y = ly,
        .ts = now_ns(), .virt = 1 });
}

/* 一行命令 -> 一行回包 */
static int handle_line(char *line, char *resp, size_t cap)
{
    char *t, *st;
    int slot, x, y;
    line[strcspn(line, "\r\n")] = 0;
    t = strtok_r(line, " \t", &st);
    if (!t) { snprintf(resp, cap, "err empty"); return -1; }
    if (!strcmp(t, "ping")) { snprintf(resp, cap, "pong"); return 0; }
    if (!strcmp(t, "res")) {
        snprintf(resp, cap, "res %d %d raw %d %d %d %d", logical_width, logical_height,
                 axmin[0], axmax[0], axmin[1], axmax[1]);
        return 0;
    }
    if (!strcmp(t, "reset")) {
        if (frame_open) { snprintf(resp, cap, "err frame"); return -1; }
        owner_reset(); snprintf(resp, cap, "ok"); return 0;
    }
    if (!strcmp(t, "region")) {
        char *op = strtok_r(NULL, " \t", &st);
        if (!op) { snprintf(resp, cap, "err region"); return -1; }
        if (!strcmp(op, "clear")) {
            regions_clear(); snprintf(resp, cap, "ok %d", region_count); return 0;
        }
        if (!strcmp(op, "list")) {
            /* 查询当前配置：每行 region <id> <type> <a1..a4> <en>，末行 end <总数> */
            size_t used = 0;
            int i;
            struct region tbl[MAX_REGIONS];
            int n;
            pthread_mutex_lock(&region_mu);
            n = region_count;
            if (n > MAX_REGIONS) n = MAX_REGIONS;
            if (n > 0) memcpy(tbl, regions, (size_t)n * sizeof tbl[0]);
            pthread_mutex_unlock(&region_mu);
            for (i = 0; i < n && used + 64 < cap; i++) {
                used += (size_t)snprintf(resp + used, cap - used, "region %s %d %d %d %d %d %d\n",
                                         tbl[i].id, tbl[i].type,
                                         tbl[i].a1, tbl[i].a2,
                                         tbl[i].a3, tbl[i].a4,
                                         tbl[i].enabled);
            }
            if (used == 0) { snprintf(resp, cap, "ok 0"); return 0; }
            snprintf(resp + used, cap - used, "end %d", n);
            return 0;
        }
        if (!strcmp(op, "add")) {
            char *sid = strtok_r(NULL, " \t", &st), *stype = strtok_r(NULL, " \t", &st);
            char *sa1 = strtok_r(NULL, " \t", &st), *sa2 = strtok_r(NULL, " \t", &st);
            char *sa3 = strtok_r(NULL, " \t", &st), *sa4 = strtok_r(NULL, " \t", &st);
            char *sen = strtok_r(NULL, " \t", &st);
            int type, a1, a2, a3, a4, en;
            if (!sid || !stype || !sa1 || !sa2 || !sa3 || !sa4 || !sen ||
                strtok_r(NULL, " \t", &st) ||
                parse_long(stype, 0, 1, &type) ||
                parse_long(sa1, 0, logical_width - 1, &a1) ||
                parse_long(sa2, 0, logical_height - 1, &a2) ||
                parse_long(sa3, 0, 100000, &a3) ||
                parse_long(sa4, 0, logical_height - 1, &a4) ||
                parse_long(sen, 0, 1, &en) ||
                region_add(sid, type, a1, a2, a3, a4, en) < 0) {
                snprintf(resp, cap, "err region"); return -1;
            }
            snprintf(resp, cap, "ok %d", region_count); return 0;
        }
        snprintf(resp, cap, "err region"); return -1;
    }
    if (!strcmp(t, "sub")) {
        /* sub [region|phys|all]...：不带参数 = 两个通道都订（向后兼容） */
        char *sm = strtok_r(NULL, " \t", &st);
        int mask = SUB_REGION | SUB_PHYS;
        if (sm) {
            mask = 0;
            do {
                if (!strcmp(sm, "region")) mask |= SUB_REGION;
                else if (!strcmp(sm, "phys")) mask |= SUB_PHYS;
                else if (!strcmp(sm, "all") || !strcmp(sm, "both")) mask = SUB_REGION | SUB_PHYS;
                else { snprintf(resp, cap, "err sub"); return -1; }
            } while ((sm = strtok_r(NULL, " \t", &st)));
        }
        if (!mask) { snprintf(resp, cap, "err sub"); return -1; }
        sub_mask = mask;
        snprintf(resp, cap, "ok"); return 0;
    }
    if (!strcmp(t, "unsub")) {
        sub_mask = 0; snprintf(resp, cap, "ok"); return 0;
    }
    if (!strcmp(t, "up")) {
        char *ss = strtok_r(NULL, " \t", &st);
        int lx, ly;
        if (frame_open || !ss || strtok_r(NULL, " \t", &st) ||
            parse_long(ss, 0, vslots - 1, &slot) ||
            set_virtual(virt, slot, t, virt[slot].x, virt[slot].y) || emit_frame() < 0) {
            snprintf(resp, cap, "err point"); return -1;
        }
        if (raw_to_logical(virt[slot].x, 0, &lx) == 0 && raw_to_logical(virt[slot].y, 1, &ly) == 0)
            push_virt_ev(slot, 0, lx, ly);
        snprintf(resp, cap, "ok"); return 0;
    }
    if (!strcmp(t, "down") || !strcmp(t, "move")) {
        char *ss = strtok_r(NULL, " \t", &st), *sx = strtok_r(NULL, " \t", &st), *sy = strtok_r(NULL, " \t", &st);
        int lx, ly;
        if (frame_open || !ss || !sx || !sy || strtok_r(NULL, " \t", &st) ||
            parse_long(ss, 0, vslots - 1, &slot) || parse_long(sx, 0, logical_width - 1, &lx) ||
            parse_long(sy, 0, logical_height - 1, &ly) ||
            logical_to_raw(lx, 0, &x) || logical_to_raw(ly, 1, &y) ||
            set_virtual(virt, slot, t, x, y) || emit_frame() < 0) {
            snprintf(resp, cap, "err point"); return -1;
        }
        push_virt_ev(slot, !strcmp(t, "down") ? 1 : 2, lx, ly);
        snprintf(resp, cap, "ok"); return 0;
    }
    if (!strcmp(t, "begin_frame")) {
        if (frame_open || strtok_r(NULL, " \t", &st)) { snprintf(resp, cap, "err frame"); return -1; }
        memcpy(staged, virt, sizeof staged);
        frame_open = 1; memset(frame_seen, 0, sizeof frame_seen);
        snprintf(resp, cap, "ok"); return 0;
    }
    if (!strcmp(t, "point")) {
        char *ss = strtok_r(NULL, " \t", &st), *state = strtok_r(NULL, " \t", &st);
        char *sx = strtok_r(NULL, " \t", &st), *sy = strtok_r(NULL, " \t", &st);
        int lx, ly;
        if (!frame_open || !ss || !state || !sx || !sy || strtok_r(NULL, " \t", &st) ||
            parse_long(ss, 0, vslots - 1, &slot) || parse_long(sx, 0, logical_width - 1, &lx) ||
            parse_long(sy, 0, logical_height - 1, &ly) ||
            logical_to_raw(lx, 0, &x) || logical_to_raw(ly, 1, &y) ||
            frame_seen[slot] || set_virtual(staged, slot, state, x, y)) {
            snprintf(resp, cap, "err point"); return -1;
        }
        frame_seen[slot] = 1; snprintf(resp, cap, "ok"); return 0;
    }
    if (!strcmp(t, "end_frame")) {
        unsigned char was_down[MAX_VIRT];
        int i;
        if (!frame_open || strtok_r(NULL, " \t", &st)) { snprintf(resp, cap, "err frame"); return -1; }
        for (i = 0; i < vslots; i++) was_down[i] = frame_seen[i] ? virt[i].down : 0;
        memcpy(virt, staged, sizeof virt);
        if (emit_frame() < 0) { frame_open = 0; snprintf(resp, cap, "err frame"); return -1; }
        for (i = 0; i < vslots; i++) {   /* 提交成功才推：暂存帧中断不产生事件 */
            int lx, ly, act;
            if (!frame_seen[i]) continue;
            if (!was_down[i] && virt[i].down) act = 1;
            else if (was_down[i] && !virt[i].down) act = 0;
            else if (was_down[i] && virt[i].down) act = 2;
            else continue;
            if (raw_to_logical(virt[i].x, 0, &lx) < 0 || raw_to_logical(virt[i].y, 1, &ly) < 0) continue;
            push_virt_ev(i, act, lx, ly);
        }
        frame_open = 0; snprintf(resp, cap, "ok"); return 0;
    }
    snprintf(resp, cap, "err unknown"); return -1;
}

/* 物理流：一次 read() 可能攒好几帧，所以边沿（按下/抬起）在每一帧处理完就清。 */
static void physical_events(void)
{
    struct input_event e;
    ssize_t n;
    while ((n = read(input_fd, &e, sizeof e)) == (ssize_t)sizeof e) {
        if (e.type == EV_ABS && e.code == ABS_MT_SLOT) {
            selected_slot = e.value;
            if (selected_slot < 0 || selected_slot >= phys_slots) selected_slot = -1;   /* 越界 = 忽略后续槽事件 */
        } else if (e.type == EV_ABS && selected_slot >= 0 && selected_slot < phys_slots) {
            if (e.code == ABS_MT_TRACKING_ID) {
                if (e.value < 0) {                       /* 抬手 */
                    phys[selected_slot].down = 0; phys[selected_slot].pending_up = 1;
                } else {                                 /* 按下 */
                    phys[selected_slot].id = e.value;
                    phys[selected_slot].down = 1;
                }
            } else if (e.code == ABS_MT_POSITION_X) phys[selected_slot].x = e.value;
            else if (e.code == ABS_MT_POSITION_Y) phys[selected_slot].y = e.value;
        }
        if (e.type == EV_SYN && e.code == SYN_DROPPED) {
            /* 内核环形缓冲溢出：后续事件有空洞，保守地把所有槽当抬起，等下一帧重建 */
            int k;
            for (k = 0; k < phys_slots; k++) if (phys[k].down) { phys[k].down = 0; phys[k].pending_up = 1; }
            continue;
        }
        if (e.type == EV_SYN && e.code == SYN_REPORT) {
            if (emit_frame() < 0) g_emit_fail++;
            else g_emit_fail = 0;
            broadcast_phys();   /* 注入完成后广播：消费者看到的是完整帧快照（方案 §4.1） */
        }
    }
    if (n < 0 && (errno == ENODEV || errno == EIO)) {
        fprintf(stderr, "vtouchd: 输入设备消失/出错 errno=%d (%s) → 停止\n", errno, strerror(errno));
        stop_flag = 1;
    }
}

/* ---- WebSocket（只绑回环；自带 SHA-1/Base64，不引依赖）---- */
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

struct sha1 { uint32_t h[5]; uint64_t bits; unsigned char block[64]; size_t used; };
static uint32_t rol32(uint32_t x, unsigned n) { return (x << n) | (x >> (32U - n)); }
static uint32_t be32(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static void sha1_block(struct sha1 *s, const unsigned char *p)
{
    uint32_t w[80], a, b, c, d, e, f, k, t;
    int i;
    for (i = 0; i < 16; ++i) w[i] = be32(p + i * 4);
    for (i = 16; i < 80; ++i) w[i] = rol32(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
    a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3]; e = s->h[4];
    for (i = 0; i < 80; ++i) {
        if (i < 20) { f = (b & c) | ((~b) & d); k = 0x5a827999U; }
        else if (i < 40) { f = b ^ c ^ d; k = 0x6ed9eba1U; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8f1bbcdcU; }
        else { f = b ^ c ^ d; k = 0xca62c1d6U; }
        t = rol32(a, 5) + f + e + k + w[i]; e = d; d = c; c = rol32(b, 30); b = a; a = t;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e;
}
static void sha1_init(struct sha1 *s)
{
    s->h[0] = 0x67452301U; s->h[1] = 0xefcdab89U; s->h[2] = 0x98badcfeU;
    s->h[3] = 0x10325476U; s->h[4] = 0xc3d2e1f0U; s->bits = 0; s->used = 0;
}
static void sha1_update(struct sha1 *s, const unsigned char *p, size_t n)
{
    s->bits += (uint64_t)n * 8;
    while (n) {
        size_t take = 64 - s->used;
        if (take > n) take = n;
        memcpy(s->block + s->used, p, take); s->used += take; p += take; n -= take;
        if (s->used == 64) { sha1_block(s, s->block); s->used = 0; }
    }
}
static void sha1_final(struct sha1 *s, unsigned char out[20])
{
    unsigned char pad[128];
    size_t n, i;
    uint64_t bits = s->bits;
    memset(pad, 0, sizeof pad); pad[0] = 0x80;
    n = (s->used < 56) ? (56 - s->used) : (120 - s->used);
    sha1_update(s, pad, n);
    for (i = 0; i < 8; ++i) pad[i] = (unsigned char)(bits >> (56 - i * 8));
    sha1_update(s, pad, 8);
    for (i = 0; i < 5; ++i) {
        out[i*4] = (unsigned char)(s->h[i] >> 24); out[i*4+1] = (unsigned char)(s->h[i] >> 16);
        out[i*4+2] = (unsigned char)(s->h[i] >> 8); out[i*4+3] = (unsigned char)s->h[i];
    }
}
static int base64(const unsigned char *in, size_t n, char *out, size_t cap)
{
    static const char tab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i = 0, o = 0;
    unsigned v;
    if (cap < ((n + 2) / 3) * 4 + 1) return -1;
    while (i < n) {
        unsigned char b0 = in[i++], b1 = 0, b2 = 0;
        int nb = 1;
        if (i < n) { b1 = in[i++]; nb = 2; }
        if (i < n) { b2 = in[i++]; nb = 3; }
        v = ((unsigned)b0 << 16) | ((unsigned)b1 << 8) | b2;
        out[o++] = tab[(v >> 18) & 63]; out[o++] = tab[(v >> 12) & 63];
        out[o++] = (nb > 1) ? tab[(v >> 6) & 63] : '=';
        out[o++] = (nb > 2) ? tab[v & 63] : '=';
    }
    out[o] = 0;
    return (int)o;
}
static int header_value(const char *req, const char *name, char *out, size_t cap)
{
    const char *p = req, *e, *c;
    size_t nl = strlen(name), n;
    while (*p) {
        e = strstr(p, "\r\n"); if (!e) break;
        c = memchr(p, ':', (size_t)(e - p));
        if (c && (size_t)(c - p) == nl && strncasecmp(p, name, nl) == 0) {
            p = c + 1; while (p < e && (*p == ' ' || *p == '\t')) ++p;
            n = (size_t)(e - p); while (n && (p[n-1] == ' ' || p[n-1] == '\t')) --n;
            if (n == 0 || n + 1 > cap) return -1;
            memcpy(out, p, n); out[n] = 0; return 0;
        }
        p = e + 2;
    }
    return -1;
}
static int has_token(const char *s, const char *token)
{
    size_t n = strlen(token);
    const char *p = s;
    while (*p) {
        while (*p == ',' || *p == ' ' || *p == '\t') ++p;
        if (strncasecmp(p, token, n) == 0 && (p[n] == 0 || p[n] == ',' || p[n] == ' ' || p[n] == '\t')) return 1;
        while (*p && *p != ',') ++p;
    }
    return 0;
}

/* 立刻可写才发：socket 当刻不可写就失败，由调用方踢掉这个客户端。
 * 为什么不等（哪怕 20ms）：这条路径跑在触摸线程上，等客户端 = 用户感到「点一下先顿一下」。 */
static int write_full(int fd, const void *buf, size_t len)
{
    size_t off = 0;
    while (off < len) {
        struct pollfd pw = { fd, POLLOUT, 0 };
        ssize_t n;
        if (!(poll(&pw, 1, 0) > 0 && (pw.revents & POLLOUT))) { errno = EAGAIN; return -1; }
        n = send(fd, (const char *)buf + off, len - off, MSG_NOSIGNAL);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return -1;
        }
        off += (size_t)n;
    }
    return 0;
}

/* 握手：socket 上已设 SO_RCVTIMEO（300ms），所以慢客户端最多拖这么久；
 * 校验 5 个头 + 回 101 + Sec-WebSocket-Accept。 */
static int websocket_handshake(int fd)
{
    char req[HTTP_MAX], key[128], upgrade[64], connection[128], version[32], accept[64];
    unsigned char digest[20];
    struct sha1 s;
    size_t used = 0;
    ssize_t n;
    const char *guid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    while (used + 1 < sizeof req) {
        n = recv(fd, req + used, 1, 0);
        if (n <= 0) return -1;                       /* 超时/断开都算失败 */
        used += (size_t)n; req[used] = 0;
        if (used >= 4 && req[used - 4] == 13 && req[used - 3] == 10 &&
            req[used - 2] == 13 && req[used - 1] == 10) break;   /* CRLFCRLF（用数值避开转义） */
    }
    if (used + 1 >= sizeof req || strncmp(req, "GET ", 4) != 0 ||
        header_value(req, "Sec-WebSocket-Key", key, sizeof key) < 0 ||
        header_value(req, "Upgrade", upgrade, sizeof upgrade) < 0 ||
        header_value(req, "Connection", connection, sizeof connection) < 0 ||
        header_value(req, "Sec-WebSocket-Version", version, sizeof version) < 0 ||
        strcasecmp(upgrade, "websocket") != 0 || !has_token(connection, "Upgrade") ||
        strcmp(version, "13") != 0 || strlen(key) != 24) return -1;
    sha1_init(&s);
    sha1_update(&s, (const unsigned char *)key, strlen(key));
    sha1_update(&s, (const unsigned char *)guid, strlen(guid));
    sha1_final(&s, digest);
    if (base64(digest, 20, accept, sizeof accept) < 0) return -1;
    {
        char response[512];
        int len = snprintf(response, sizeof response,
            "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
            "Connection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n", accept);
        return (len > 0 && (size_t)len < sizeof response &&
                write_full(fd, response, (size_t)len) == 0) ? 0 : -1;
    }
}

static int ws_send(int fd, unsigned opcode, const unsigned char *p, size_t n)
{
    unsigned char h[10];
    size_t hn;
    if (n > MAX_PAYLOAD || (opcode >= 8 && n > 125)) return -1;
    h[0] = (unsigned char)(0x80 | (opcode & 15));
    if (n < 126) { h[1] = (unsigned char)n; hn = 2; }
    else { h[1] = 126; h[2] = (unsigned char)(n >> 8); h[3] = (unsigned char)n; hn = 4; }
    return write_full(fd, h, hn) || write_full(fd, p, n);
}

/* 丢掉当前客户端：关连接 + 清出站队列 + 抬掉它的虚拟触点（只 close 会把虚拟手指永久粘在设备上） */
static void drop_client(void)
{
    if (client_fd >= 0) {
        close(client_fd);
        client_fd = -1;
        fprintf(stderr, "vtouchd: ws client dropped\n");
    }
    ws_in_len = 0;
    sendq_clear();
    sub_mask = 0;   /* 断连即退订：下个客户端从干净状态开始 */
    owner_reset();
}

/* ---- 出站发送队列（方案 §4.5）：所有出站 WS 文本统一进队列，主循环 POLLOUT 事件驱动写出。
 * 慢客户端只会让队列积压（满则丢最旧并计数），永不阻塞触摸热路径。
 * MPSC：poll 线程与 region 线程生产，poll 线程消费。 */
#define SENDQ_CAP 64
#define SENDQ_MSG 1024
static char sendq_buf[SENDQ_CAP][SENDQ_MSG];
static size_t sendq_len[SENDQ_CAP];
static unsigned sendq_h, sendq_t;
static pthread_mutex_t sendq_mu = PTHREAD_MUTEX_INITIALIZER;

static void sendq_clear(void)
{
    pthread_mutex_lock(&sendq_mu);
    sendq_h = sendq_t;
    pthread_mutex_unlock(&sendq_mu);
}

static int sendq_empty(void)
{
    int e;
    pthread_mutex_lock(&sendq_mu);
    e = (sendq_h == sendq_t);
    pthread_mutex_unlock(&sendq_mu);
    return e;
}

/* n 为文本长度（不含 NUL）；超长拒绝（调用方回 err），满队丢最旧 */
static int sendq_push(const unsigned char *p, size_t n)
{
    if (!p || n == 0 || n > SENDQ_MSG) return -1;
    pthread_mutex_lock(&sendq_mu);
    if (sendq_t - sendq_h >= SENDQ_CAP) {
        sendq_h++;
        fprintf(stderr, "vtouchd: sendq 满，丢最旧一条\n");
    }
    memcpy(sendq_buf[sendq_t % SENDQ_CAP], p, n);
    sendq_len[sendq_t % SENDQ_CAP] = n;
    sendq_t++;
    pthread_mutex_unlock(&sendq_mu);
    return 0;
}

/* POLLOUT 就绪时由 poll 线程调用：一帧一次 writev，要么整帧走完，要么 EAGAIN 留到下轮。
 * 返回 0 = 排空或对端暂时不可写，-1 = 连接已坏（调用方 drop_client）。短写按坏连接处理：
 * WS 帧写一半已破损，重试只会让客户端解析错位。 */
static int flush_send_queue(void)
{
    int rc = 0;
    pthread_mutex_lock(&sendq_mu);
    while (sendq_h != sendq_t) {
        char *m = sendq_buf[sendq_h % SENDQ_CAP];
        size_t n = sendq_len[sendq_h % SENDQ_CAP];
        unsigned char h[4];
        size_t hn;
        struct iovec iv[2];
        ssize_t k;
        h[0] = 0x81;   /* FIN + text */
        if (n < 126) { h[1] = (unsigned char)n; hn = 2; }
        else { h[1] = 126; h[2] = (unsigned char)(n >> 8); h[3] = (unsigned char)n; hn = 4; }
        iv[0].iov_base = h; iv[0].iov_len = hn;
        iv[1].iov_base = m; iv[1].iov_len = n;
        do { k = writev(client_fd, iv, 2); } while (k < 0 && errno == EINTR);
        if (k == (ssize_t)(hn + n)) { sendq_h++; continue; }
        if (k < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        fprintf(stderr, "vtouchd: 出站写坏 fd=%d k=%zd errno=%d → 断开\n", client_fd, k, errno);
        rc = -1;
        break;
    }
    pthread_mutex_unlock(&sendq_mu);
    return rc;
}

/* 输入缓冲：半包不消费，留到下一轮 poll 继续拼（以前逐字段 recv，跨 TCP 段就误判断线） */
static int ws_peek_frame(size_t *frame_len, unsigned *opcode, size_t *payload_off)
{
    unsigned len7, fin, masked, op;
    size_t off = 2, i;
    uint64_t len;
    if (ws_in_len < 2) return 0;
    fin = ws_in[0] >> 7; op = ws_in[0] & 15;
    masked = ws_in[1] >> 7; len7 = ws_in[1] & 127;
    if (!masked || !fin || (op != 1 && op != 8 && op != 9 && op != 10)) return -1;
    len = len7;
    if (len7 == 126) {
        if (ws_in_len < 4) return 0;
        len = ((uint64_t)ws_in[2] << 8) | ws_in[3];
        off = 4;
    } else if (len7 == 127) {
        if (ws_in_len < 10) return 0;
        len = 0;
        for (i = 0; i < 8; i++) len = (len << 8) | ws_in[2 + i];
        off = 10;
    }
    if (len > MAX_PAYLOAD) return -2;
    if (op >= 8 && len > 125) return -2;
    *opcode = op;
    *payload_off = off + 4;
    *frame_len = off + 4 + (size_t)len;
    return (ws_in_len < *frame_len) ? 0 : 1;
}

static int ws_next_frame(unsigned char *payload, size_t *plen, unsigned *opcode)
{
    for (;;) {
        size_t frame_len = 0, poff = 0, i, len;
        unsigned op = 0;
        int r = ws_peek_frame(&frame_len, &op, &poff);
        if (r < 0) return r;
        if (r == 0) {
            ssize_t n;
            if (ws_in_len >= sizeof ws_in) { ws_in_len = 0; return -1; }
            do { n = recv(client_fd, ws_in + ws_in_len, sizeof ws_in - ws_in_len, 0); }
            while (n < 0 && errno == EINTR && !stop_flag);
            if (n > 0) { ws_in_len += (size_t)n; continue; }
            if (n == 0) return -1;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 1;
            return -1;
        }
        len = frame_len - poff;
        {
            const unsigned char *mask = ws_in + poff - 4;
            for (i = 0; i < len; i++) payload[i] = (unsigned char)(ws_in[poff + i] ^ mask[i & 3]);
        }
        memmove(ws_in, ws_in + frame_len, ws_in_len - frame_len);
        ws_in_len -= frame_len;
        *plen = len;
        *opcode = op;
        return 0;
    }
}

/* 单轮最多处理 32 帧：一个 TCP 段里挤多条命令不会被「下一轮 poll」饿死，
 * 也不会让一整批命令长时间占住 poll 循环。返回 0 = 保持连接，-1 = 断开。 */
static int client_frame(void)
{
    unsigned char payload[MAX_PAYLOAD];
    char line[MAX_LINE], resp[MAX_LINE];
    int k;
    for (k = 0; k < 32; k++) {
        size_t len = 0, i;
        unsigned opcode = 0;
        int r = ws_next_frame(payload, &len, &opcode);
        if (r == 1) return 0;
        if (r < 0) {
            static const unsigned char c_proto[2] = { 0x03, 0xea };   /* 1002 */
            static const unsigned char c_big[2]   = { 0x03, 0xf1 };   /* 1009 */
            ws_send(client_fd, 8, r == -2 ? c_big : c_proto, 2);
            ws_in_len = 0;
            return -1;
        }
        if (opcode == 8) { ws_send(client_fd, 8, payload, len); return -1; }
        if (opcode == 9) {   /* ping → pong，可写才发 */
            struct pollfd pw = { client_fd, POLLOUT, 0 };
            if (poll(&pw, 1, 0) > 0 && (pw.revents & POLLOUT) && ws_send(client_fd, 10, payload, len) < 0) return -1;
            continue;
        }
        if (opcode == 10) continue;
        for (i = 0; i < len; i++) if (payload[i] == 10 || payload[i] == 13) payload[i] = ' ';
        if (len >= sizeof line) return -1;
        memcpy(line, payload, len); line[len] = 0;
        handle_line(line, resp, sizeof resp);
        /* 回包走出站队列（WS 文本帧，见 flush_send_queue）：慢客户端只排队，不拖触摸线程 */
        if (sendq_push((const unsigned char *)resp, strlen(resp)) < 0) return -1;
    }
    return 0;
}

static int make_listen(void)
{
    int fd, opt = 1;
    struct sockaddr_in a;
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof opt);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)ws_port);
    if (inet_pton(AF_INET, "127.0.0.1", &a.sin_addr) != 1 ||      /* 只绑回环 */
        bind(fd, (struct sockaddr *)&a, sizeof a) < 0 || listen(fd, 8) < 0) {
        close(fd); return -1;
    }
    return fd;
}

static void apply_args(int argc, char **argv)
{
    int i, n;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-w")) {
            if (i + 1 >= argc || parse_long(argv[++i], 2, 100000, &logical_width) != 0)
                fprintf(stderr, "vtouchd: -w 取值无效，保留默认 %d\n", logical_width);
        } else if (!strcmp(argv[i], "-h")) {
            if (i + 1 >= argc || parse_long(argv[++i], 2, 100000, &logical_height) != 0)
                fprintf(stderr, "vtouchd: -h 取值无效，保留默认 %d\n", logical_height);
        } else if (!strcmp(argv[i], "-v")) {
            if (i + 1 >= argc || parse_long(argv[++i], 1, MAX_VIRT, &n) != 0)
                fprintf(stderr, "vtouchd: -v 取值无效（1~%d），保留默认 %d\n", MAX_VIRT, vslots);
            else vslots = n;
        } else if (!strcmp(argv[i], "-p")) {
            if (i + 1 >= argc || parse_long(argv[++i], 1, 65535, &ws_port) != 0)
                fprintf(stderr, "vtouchd: -p 取值无效，保留默认 %d\n", ws_port);
        } else if (strcmp(argv[i], "-w") && strcmp(argv[i], "-h") && strcmp(argv[i], "-v") && strcmp(argv[i], "-p")) {
            fprintf(stderr, "用法: %s -w 宽 -h 高 [-v 虚拟槽数] [-p 端口]\n", argv[0]);
        }
    }
}

static int vtouch_init(int argc, char **argv)
{
    char dev[PATH_MAX];
    int z;
    setvbuf(stderr, NULL, _IONBF, 0);   /* 日志实时落盘，别被全缓冲吞掉 */
    apply_args(argc, argv);
    if (logical_width < 2 || logical_height < 2) {
        fprintf(stderr, "vtouchd: 需要逻辑尺寸（-w 宽 -h 高）\n");
        return -2;
    }
    memset(phys, 0, sizeof phys); memset(virt, 0, sizeof virt);
    for (z = 0; z < MAX_PHYS; z++) { phys[z].oslot = -1; phys[z].oid = -1; }
    for (z = 0; z < MAX_VIRT; z++) {
        virt[z].oslot = -1; virt[z].oid = -1;
        staged[z].oslot = -1; staged[z].oid = -1;
    }
    if (discover(dev, sizeof dev) < 0) {
        fprintf(stderr, "vtouchd: 没找到 Type-B 触摸屏（扫了 /dev/input/event0..63）\n");
        return -2;
    }
    total_slots = phys_slots;
    if (total_slots > MAX_PHYS) total_slots = MAX_PHYS;
    if (setup_uinput() < 0) {
        fprintf(stderr, "vtouchd: uinput 建设备失败: %s\n", strerror(errno));
        return -3;
    }
    input_fd = open(dev, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (input_fd < 0) { cleanup(); return -4; }
    /* 先起监听、最后 grab：任何失败路径都不会留下「抓了却没人能控制」的状态 */
    listen_fd = make_listen();
    if (listen_fd < 0) { cleanup(); return -6; }
    /* 先起监听与 region 线程、最后 grab：任何失败路径都不会留下「抓了却没人能控制」的状态 */
    if (pthread_create(&region_th, NULL, region_thread, NULL) != 0) { cleanup(); return -7; }
    region_started = 1;
    if (ioctl(input_fd, EVIOCGRAB, 1) < 0) { cleanup(); return -5; }
    fprintf(stderr, "vtouchd: dev=%s pool=%d virt_max=%d pressure=%s ws=127.0.0.1:%d size=%dx%d\n",
            dev, total_slots, vslots, has_pressure ? "on" : "off", ws_port, logical_width, logical_height);
    return 0;
}

/* 单轮 poll：返回 0 = 继续，-1 = 停止 */
static int vtouch_poll_step(void)
{
    struct pollfd p[3];
    int to = g_reemit ? 5 : 1000;      /* 有待重发的整帧：5ms 一轮，尽快把手抬起来 */
    int r;
    if (stop_flag) return -1;
    p[0] = (struct pollfd){ input_fd, POLLIN | POLLHUP | POLLERR, 0 };
    p[1] = (struct pollfd){ listen_fd, POLLIN, 0 };
    p[2] = (struct pollfd){ client_fd, client_fd >= 0 ? (POLLIN | POLLHUP | POLLERR) : 0, 0 };
    r = poll(p, client_fd >= 0 ? 3 : 2, to);
    if (r < 0) {
        if (errno == EINTR) return 0;
        fprintf(stderr, "vtouchd: poll 失败 errno=%d (%s) → 停止\n", errno, strerror(errno));
        return -1;
    }
    if (p[0].revents & POLLIN) physical_events();
    if (g_reemit && u_fd >= 0) {
        if (emit_frame() == 0) { g_reemit = 0; g_emit_fail = 0; }
        else if (++g_emit_fail >= 200) {
            fprintf(stderr, "vtouchd: uinput 连续 %d 次写失败 → 停止（物理触摸回系统）\n", g_emit_fail);
            return -1;
        }
    }
    if (p[0].revents & (POLLHUP | POLLERR)) {
        fprintf(stderr, "vtouchd: 输入设备挂断 (revents=0x%x) → 停止\n", p[0].revents);
        return -1;
    }
    if (p[1].revents & POLLIN) {
        int ncf = accept4(listen_fd, NULL, NULL, SOCK_CLOEXEC);
        if (ncf >= 0) {
            struct timeval rtv = { .tv_sec = 0, .tv_usec = 300000 };   /* 握手最多被拖 300ms */
            struct timeval stv = { .tv_sec = 0, .tv_usec = 20000 };    /* 写超时（第二道保险） */
            int one = 1;
            if (client_fd >= 0) { fprintf(stderr, "vtouchd: 新连接，踢掉旧客户端\n"); drop_client(); }
            setsockopt(ncf, SOL_SOCKET, SO_RCVTIMEO, &rtv, sizeof rtv);
            setsockopt(ncf, SOL_SOCKET, SO_SNDTIMEO, &stv, sizeof stv);
            setsockopt(ncf, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
            fcntl(ncf, F_SETFL, O_NONBLOCK);   /* 出站只看 POLLOUT 就绪写，从不等慢客户端 */
            if (websocket_handshake(ncf) != 0) {
                fprintf(stderr, "vtouchd: ws 握手失败\n");
                close(ncf);
            } else {
                client_fd = ncf;
                ws_in_len = 0;
                fprintf(stderr, "vtouchd: ws client connected\n");
            }
        }
    }
    if (client_fd >= 0 && (p[2].revents & (POLLHUP | POLLERR))) drop_client();
    /* ws_in_len > 0 也要进来：已经读进缓冲、还没处理完的帧不能让 POLLIN 决定生死 */
    if (client_fd >= 0 && ((p[2].revents & POLLIN) || ws_in_len > 0)) {
        if (client_frame() < 0) drop_client();
    }
    /* 出站刷盘（方案 §4.5）：socket 可写才写；写坏则断开，不影响抓与 uinput */
    if (client_fd >= 0 && !sendq_empty()) {
        struct pollfd pw = { client_fd, POLLOUT, 0 };
        if (poll(&pw, 1, 0) > 0 && (pw.revents & POLLOUT)) {
            if (flush_send_queue() < 0) drop_client();
        }
    }
    return 0;
}

static void on_signal(int s) { (void)s; stop_flag = 1; }

int main(int argc, char **argv)
{
    struct sigaction sa;
    int rc;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, 0);
    sigaction(SIGINT, &sa, 0);
    signal(SIGPIPE, SIG_IGN);

    rc = vtouch_init(argc, argv);
    if (rc != 0) return -rc;            /* 退出码 = 2/3/4/5/6/7（见 README 的失败出口表） */
    while (vtouch_poll_step() == 0)
        ;
    cleanup();
    return 0;
}
