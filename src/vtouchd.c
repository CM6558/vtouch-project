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
 * 协议: 一行一条命令，回一行（loopback WS，单客户端，新连接踢旧连接）
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
#include <signal.h>
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

/* 收尾顺序固定：client → listen → 放 grab → 销毁 uinput。只跑一次。 */
static void cleanup(void)
{
    static int cleaned;
    if (cleaned) return;
    cleaned = 1;
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
    if (!strcmp(t, "up")) {
        char *ss = strtok_r(NULL, " \t", &st);
        if (frame_open || !ss || strtok_r(NULL, " \t", &st) ||
            parse_long(ss, 0, vslots - 1, &slot) ||
            set_virtual(virt, slot, t, virt[slot].x, virt[slot].y) || emit_frame() < 0) {
            snprintf(resp, cap, "err point"); return -1;
        }
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
        if (!frame_open || strtok_r(NULL, " \t", &st)) { snprintf(resp, cap, "err frame"); return -1; }
        memcpy(virt, staged, sizeof virt);
        if (emit_frame() < 0) { frame_open = 0; snprintf(resp, cap, "err frame"); return -1; }
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

/* 丢掉当前客户端：关连接 + 抬掉它的虚拟触点（只 close 会把虚拟手指永久粘在设备上） */
static void drop_client(void)
{
    if (client_fd >= 0) {
        close(client_fd);
        client_fd = -1;
        fprintf(stderr, "vtouchd: ws client dropped\n");
    }
    ws_in_len = 0;
    owner_reset();
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
        if (write_full(client_fd, resp, strlen(resp)) < 0) return -1;
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
    if (rc != 0) return -rc;            /* 退出码 = 2/3/4/5/6（见 README 的失败出口表） */
    while (vtouch_poll_step() == 0)
        ;
    cleanup();
    return 0;
}
