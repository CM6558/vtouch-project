/* vt_input.c（§7 物理输入与 uinput 设备） —— 模块地图见 vt_internal.h；私有状态就近放 static，共享状态走 g。 */
#include "vt_internal.h"

static int selected_slot;   /* 当前正被解析的物理槽（-1 = 忽略）*/

/**
 * (vtouch-doc: validate_device)
 * @brief 认一块设备是不是 Type-B 触摸屏（槽 + tracking id + XY 四轴 + 量程），并把它的能力声明整份抄进 cap_*（供 setup_uinput 镜像）。
 * @param   p        设备节点路径
 * @param   slots    输出物理槽数
 * @param   xmin     输出 X 下界
 * @param   xmax     输出 X 上界
 * @param   ymin     输出 Y 下界
 * @param   ymax     输出 Y 上界
 * @return  0 是；-1 不是或打不开。
 * @note    不写死 eventN：由 discover 扫 event0..63 逐个问。
 *
 * 为什么这么写（原有注释，逐字保留）：
 *   认一块设备是不是 Type-B 触摸屏：EV_ABS 里必须有槽/tracking id/XY 四轴，槽数合规，
 *   X/Y 量程有效；并把它的能力声明整份抄进 cap_*（供 setup_uinput 镜像）。
 */
int validate_device(const char *p, int *slots, int *xmin, int *xmax, int *ymin, int *ymax)
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
    g.has_pressure = 0; g.pressure_max = 0;
    if (bit(abs, ABS_MT_PRESSURE) && ioctl(f, EVIOCGABS(ABS_MT_PRESSURE), &a) == 0 && a.maximum > 0) {
        g.has_pressure = 1; g.pressure_max = a.maximum;
    }
    memcpy(g.cap_ev, ev, sizeof ev); memcpy(g.cap_abs, abs, sizeof abs); memcpy(g.cap_prop, prop, sizeof prop);
    memset(g.cap_key, 0, sizeof g.cap_key); memset(g.cap_ai_ok, 0, sizeof g.cap_ai_ok);
    if (ioctl(f, EVIOCGBIT(EV_KEY, sizeof g.cap_key), g.cap_key) < 0) { close(f); return -1; }
    for (c = 0; c <= ABS_MAX; c++)
        if (bit(g.cap_abs, c) && ioctl(f, EVIOCGABS(c), &g.cap_ai[c]) == 0) g.cap_ai_ok[c] = 1;
    memset(g.cap_name, 0, sizeof g.cap_name);
    if (ioctl(f, EVIOCGNAME(sizeof g.cap_name - 1), g.cap_name) < 0) g.cap_name[0] = 0;
    close(f);
    return 0;
}
/**
 * (vtouch-doc: discover)
 * @brief 扫 /dev/input/event0..63，找第一块 Type-B 触摸屏。
 * @param   out      输出设备节点路径
 * @param   n        缓冲长度
 * @return  0 找到；-1 没找到。
 *
 * 为什么这么写（原有注释，逐字保留）：
 *   动态发现：扫 event0..63 找第一块 Type-B 触摸屏，不写死节点号
 */
int discover(char *out, size_t n)
{
    int k;
    for (k = 0; k < 64; k++) {
        snprintf(out, n, "/dev/input/event%d", k);
        if (validate_device(out, &g.phys_slots, &g.axmin[0], &g.axmax[0], &g.axmin[1], &g.axmax[1]) == 0) return 0;
    }
    return -1;
}
/**
 * (vtouch-doc: setup_uinput)
 * @brief 建合并 uinput 设备：照抄物理屏的能力（EV / KEY / ABS+absinfo / props），只改 4 处真冲突（TOOL_TYPE 量程、槽数、tracking id 量程、名字与 bus），并强制 INPUT_PROP_DIRECT。
 * @return  0 成功；-1 失败（调用方以退出码 3 退出）。
 * @note    槽数 = phys_slots + vslots；tracking id 上限 = total_slots - 1。名字加 _vtouch 后缀，避免与物理设备同名。
 *
 * 为什么这么写（原有注释，逐字保留）：
 *   合并设备的能力声明 = 照抄物理屏；只有 4 处真冲突取相似值。
 */
int setup_uinput(void)
{
    struct uinput_setup s; struct uinput_abs_setup a;
    int t, c, p;
    g.u_fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (g.u_fd < 0) return -1;
    for (t = 0; t <= EV_MAX; t++) if (bit(g.cap_ev, t) && ioctl(g.u_fd, UI_SET_EVBIT, t) < 0) goto fail;
    if (ioctl(g.u_fd, UI_SET_EVBIT, EV_SYN) < 0 || ioctl(g.u_fd, UI_SET_EVBIT, EV_KEY) < 0 ||
        ioctl(g.u_fd, UI_SET_EVBIT, EV_ABS) < 0) goto fail;
    for (c = 0; c <= KEY_MAX; c++) if (bit(g.cap_key, c) && ioctl(g.u_fd, UI_SET_KEYBIT, c) < 0) goto fail;
    if (ioctl(g.u_fd, UI_SET_KEYBIT, BTN_TOUCH) < 0 ||
        ioctl(g.u_fd, UI_SET_KEYBIT, BTN_TOOL_FINGER) < 0) goto fail;
    for (c = 0; c <= ABS_MAX; c++) {
        if (!bit(g.cap_abs, c)) continue;
        if (ioctl(g.u_fd, UI_SET_ABSBIT, c) < 0) goto fail;
        memset(&a, 0, sizeof a); a.code = (unsigned short)c;
        if (g.cap_ai_ok[c]) a.absinfo = g.cap_ai[c];               /* fuzz/flat/resolution 一起抄 */
        if (c == ABS_MT_POSITION_X) { a.absinfo.minimum = g.axmin[0]; a.absinfo.maximum = g.axmax[0]; }
        if (c == ABS_MT_POSITION_Y) { a.absinfo.minimum = g.axmin[1]; a.absinfo.maximum = g.axmax[1]; }
        /* 冲突① 真机 ABS_MT_TOOL_TYPE 量程常是 0..0，装不下 tool 值 → 抬到能装 PALM */
        if (c == ABS_MT_TOOL_TYPE && a.absinfo.maximum < MT_TOOL_PALM) a.absinfo.maximum = MT_TOOL_PALM;
        /* 冲突② 槽数上限不能小于两段身份之和 */
        if (c == ABS_MT_SLOT && a.absinfo.maximum < g.total_slots - 1) a.absinfo.maximum = g.total_slots - 1;
        /* 冲突③ tracking id 量程至少要装下两段身份（物理段 + 虚拟段） */
        if (c == ABS_MT_TRACKING_ID && a.absinfo.maximum < g.id_max) a.absinfo.maximum = g.id_max;
        if (ioctl(g.u_fd, UI_ABS_SETUP, &a) < 0) goto fail;
    }
    /* 物理屏万一没声明这四根轴也要补齐，否则合并设备发不出 MT 事件 */
    {
        static const int need[4] = { ABS_MT_SLOT, ABS_MT_TRACKING_ID, ABS_MT_POSITION_X, ABS_MT_POSITION_Y };
        for (t = 0; t < 4; t++) {
            int code = need[t];
            if (bit(g.cap_abs, code)) continue;
            if (ioctl(g.u_fd, UI_SET_ABSBIT, code) < 0) goto fail;
            memset(&a, 0, sizeof a); a.code = (unsigned short)code;
            if (code == ABS_MT_POSITION_X) { a.absinfo.minimum = g.axmin[0]; a.absinfo.maximum = g.axmax[0]; }
            else if (code == ABS_MT_POSITION_Y) { a.absinfo.minimum = g.axmin[1]; a.absinfo.maximum = g.axmax[1]; }
            else if (code == ABS_MT_SLOT) a.absinfo.maximum = g.total_slots - 1;
            else a.absinfo.maximum = g.id_max;
            if (ioctl(g.u_fd, UI_ABS_SETUP, &a) < 0) goto fail;
        }
    }
    for (p = 0; p <= INPUT_PROP_MAX; p++) if (bit(g.cap_prop, p) && ioctl(g.u_fd, UI_SET_PROPBIT, p) < 0) goto fail;
    /* 冲突④（名字/ID）：什么都不声明时系统会把设备当触控板画出鼠标指针 → INPUT_PROP_DIRECT 必须有；
     * 名字加后缀、bus 用 BUS_VIRTUAL，避免被当成与物理屏同一设备而忽略。 */
    if (!bit(g.cap_prop, INPUT_PROP_DIRECT) && ioctl(g.u_fd, UI_SET_PROPBIT, INPUT_PROP_DIRECT) < 0) goto fail;
    memset(&s, 0, sizeof s); s.id.bustype = BUS_VIRTUAL;
    if (g.cap_name[0]) snprintf((char *)s.name, UINPUT_MAX_NAME_SIZE, "%s_vtouch", g.cap_name);
    else strncpy((char *)s.name, "vtouch-merged", UINPUT_MAX_NAME_SIZE - 1);
    if (ioctl(g.u_fd, UI_DEV_SETUP, &s) < 0) goto fail;
    if (ioctl(g.u_fd, UI_DEV_CREATE) < 0) goto fail;
    return 0;
fail:
    ioctl(g.u_fd, UI_DEV_DESTROY); close(g.u_fd); g.u_fd = -1; return -1;
}
/**
 * (vtouch-doc: phys_event_one)
 * @brief 分发单条 input_event（槽选择 / 按下抬起 / 位置 / SYN_DROPPED 兜底 / SYN_REPORT 结帧）。
 * @param   e        一条 input_event（来自批量读的缓冲）
 * @note    从 physical_events 里抽出来的同一段逻辑（批量读之后一次要处理一批）；边沿语义与逐条 read 的旧版逐字一致。
 *
 * 为什么这么写（原有注释，逐字保留）：
 *   单条 input_event 的分发（**从 physical_events 里原样抽出**：批量读之后一次要处理一批，
 *   边沿语义与逐条 read 的旧版逐字一致 —— 槽选择、按下/抬起、位置、SYN_DROPPED 兜底、SYN_REPORT 结帧）。
 */
static void phys_event_one(const struct input_event *e)
{
    if (e->type == EV_ABS && e->code == ABS_MT_SLOT) {
        selected_slot = e->value;
        if (selected_slot < 0 || selected_slot >= g.phys_slots) selected_slot = -1;   /* 越界 = 忽略后续槽事件 */
    } else if (e->type == EV_ABS && selected_slot >= 0 && selected_slot < g.phys_slots) {
        if (e->code == ABS_MT_TRACKING_ID) {
            if (e->value < 0) {                       /* 抬手 */
                g.phys[selected_slot].down = 0; g.phys[selected_slot].pending_up = 1;
            } else {                                 /* 按下（原值不用存：身份按下标算） */
                g.phys[selected_slot].down = 1;
                g.ps_press_ns[selected_slot] = now_ns();   /* §4.2：down 上报的是「按下时刻」 */
            }
        } else if (e->code == ABS_MT_POSITION_X) g.phys[selected_slot].x = e->value;
        else if (e->code == ABS_MT_POSITION_Y) g.phys[selected_slot].y = e->value;
    }
    if (e->type == EV_SYN && e->code == SYN_DROPPED) {
        /* 内核环形缓冲溢出：后续事件有空洞，保守地把所有槽当抬起，等下一帧重建 */
        int k;
        for (k = 0; k < g.phys_slots; k++) if (g.phys[k].down) { g.phys[k].down = 0; g.phys[k].pending_up = 1; }
        return;
    }
    if (e->type == EV_SYN && e->code == SYN_REPORT) {
        if (emit_frame() < 0) g.g_emit_fail++;
        else g.g_emit_fail = 0;
        /* §4.1 时机：帧边界、emit_frame() 之后入队（快照 = 完整帧状态）。
         * 入队只喂区域线程（不再推客户端），所以每帧都跑（纯内存比较）。 */
        enqueue_phys_changes();
    }
}
/**
 * (vtouch-doc: physical_events)
 * @brief 读物理流：解析 Type-B 事件进 phys[]（按槽），在 SYN_REPORT 处提交一帧并转发。
 * @note    一次 read 取一批（最多 64 条 input_event）再循环解析，不是每条事件一次 read()（syscall 降一个量级）；一次读可能攒好几帧，边沿在每帧处理完就清；SYN_DROPPED 保守地把所有槽当抬起。
 *
 * 为什么这么写（原有注释，逐字保留）：
 *   物理流：一次 read() 可能攒好几帧，所以边沿（按下/抬起）在每一帧处理完就清。
 *   批量读（一次 read 取一整个突发，最多 64 条 input_event）而不是每条事件一次 read()：
 *   实测 250 帧/s、每帧 6~10 条事件 ≈ 1500~2500 次 read()/s，而这些调用就在分发线程上；
 *   evdev 的标准做法是一次取一批再循环解析，syscall 数量降一个量级。
 *   半条事件（内核理论上不会返回，返回了就留着）用 carry 缓存接在下次读的前面，绝不丢字节。
 */
void physical_events(void)
{
    static struct input_event evs[64];
    static size_t carry_n;                 /* 上一次读剩的尾字节数（半条事件；内核理论上不会这么给）*/
    unsigned char *raw = (unsigned char *)evs;
    size_t i;
    for (;;) {
        size_t total, cnt;
        ssize_t n = read(g.input_fd, raw + carry_n, sizeof evs - carry_n);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            if (errno == ENODEV || errno == EIO) {
                fprintf(stderr, "vtouchd: 输入设备消失/出错 errno=%d (%s) → 停止\n", errno, strerror(errno));
                g.stop_flag = 1;
            }
            break;
        }
        if (n == 0) break;
        total = carry_n + (size_t)n;
        cnt = total / sizeof(struct input_event);
        for (i = 0; i < cnt; i++) phys_event_one(&evs[i]);
        carry_n = total - cnt * sizeof(struct input_event);
        if (carry_n) memmove(raw, raw + cnt * sizeof(struct input_event), carry_n);
        if (cnt == 0) break;        /* 只进了半条事件：等下一次可读（不空转） */
    }
}
