/* vt_frame.c（§8+§9 组帧、合帧与转发） —— 模块地图见 vt_internal.h；私有状态就近放 static，共享状态走 g。 */
#include "vt_internal.h"

/* 帧缓冲与虚拟触点快照（只有本模块用）*/
static struct input_event ev_buf[MAX_IOV];
static struct iovec ev_iov[MAX_IOV];
static int ev_n;
static int vs_down[MAX_VIRT], vs_x[MAX_VIRT], vs_y[MAX_VIRT];
/**
 * (vtouch-doc: ev_add)
 * @brief 往本帧的 iovec 里追加一条 input_event（纯内存，不做系统调用）。
 * @param   t        事件类型
 * @param   c        事件码
 * @param   v        值
 * @note    上限 MAX_IOV，超出直接忽略。
 */

void ev_add(int t, int c, int v)
{
    if (ev_n >= MAX_IOV) return;
    ev_buf[ev_n] = (struct input_event){ .type = (unsigned short)t, .code = (unsigned short)c, .value = v };
    ev_iov[ev_n].iov_base = &ev_buf[ev_n];
    ev_iov[ev_n].iov_len = sizeof(struct input_event);
    ev_n++;
}
/**
 * (vtouch-doc: uinput_writev_retry)
 * @brief 把当前帧一次 writev 写进 uinput；EAGAIN 时等最多 3×20ms 再试。
 * @return  实际写出的字节数；-1 真错。
 * @note    uinput 是以 O_NONBLOCK 打开的。
 */

/* uinput 是 O_NONBLOCK 打开的：队列满会 EAGAIN，短暂等一等（3×20ms），
 * 只有一直不可写才算真错（调用方据此重发/收摊）。 */
ssize_t uinput_writev_retry(void)
{
    int k;
    ssize_t n;
    do { n = writev(g.u_fd, ev_iov, ev_n); } while (n < 0 && errno == EINTR);
    if (n >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) return n;
    for (k = 0; k < 3; k++) {
        struct pollfd p = { g.u_fd, POLLOUT, 0 };
        if (poll(&p, 1, 20) > 0 && (p.revents & POLLOUT)) {
            do { n = writev(g.u_fd, ev_iov, ev_n); } while (n < 0 && errno == EINTR);
            if (n >= 0) return n;
            if (errno != EAGAIN && errno != EWOULDBLOCK) return n;
        }
    }
    return -1;
}
/**
 * (vtouch-doc: emit_iov_writev)
 * @brief 提交本帧；**短写要把剩下的 iovec 补完**（只补一条会丢帧尾的 SYN_REPORT，系统里就成了半帧）。
 * @return  0 成功；-1 失败。
 */

/* 提交这一帧；短写要把剩下的 iovec 全补完（只补一个会丢帧尾 SYN_REPORT）。 */
int emit_iov_writev(void)
{
    ssize_t n, need = 0;
    size_t off;
    int i;
    if (g.u_fd < 0) return -1;
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
                    do { k = write(g.u_fd, p, left); } while (k < 0 && errno == EINTR);
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
                do { k = write(g.u_fd, p, left); } while (k < 0 && errno == EINTR);
                if (k <= 0) { ev_n = 0; return -1; }
                p += k; left -= (size_t)k;
            }
        }
    }
    ev_n = 0;
    return 0;
}
/**
 * (vtouch-doc: any_emitted)
 * @brief 本帧是否真的发了触点（决定 BTN_TOUCH / BTN_TOOL_FINGER 的值）。
 * @return  1 有触点；0 没有。
 * @note    判的是来源状态里有触点，不是 iovec 里有没有 —— 虚拟触点抬起时不能把真手指的 BTN_TOUCH 带下去。
 */

/* BTN_TOUCH 只认真的进了帧的触点（有下游身份） */
int any_emitted(void)
{
    int i;
    for (i = 0; i < g.phys_slots; i++) if (g.phys[i].down) return 1;
    for (i = 0; i < g.vslots; i++) if (g.virt[i].down) return 1;
    return 0;
}
/**
 * (vtouch-doc: emit_frame)
 * @brief 把 phys[]/virt[] 合成一帧并提交：待抬 → 物理 → 虚拟 → BTN → SYN，整帧一次 writev。
 * @return  0 提交成功；-1 提交失败（置 g_reemit，由主循环重发）。
 * @note    身份按下标算（物理 = i，虚拟 = phys_slots + i）；写失败绝不清 pending_up、绝不释放身份。
 */

/* 一帧的固定顺序（每一步都有理由）：
 *   ① 待抬的触点先发 ABS_MT_TRACKING_ID=-1（身份静态，帧写失败也不用「保住」它）
 *   ② 物理触点（身份 = 它所在的物理槽号，直接发）
 *   ③ 虚拟触点（身份在 WS 命令里就分好了）
 *   ④ BTN_TOUCH / BTN_TOOL_FINGER（只有真的发了触点才置 1）
 *   ⑤ SYN_REPORT，整个帧一次 writev 提交
 * 写失败：绝不清 pending_up、绝不释放身份，置 g.g_reemit 由 poll 循环重发。 */
int emit_frame(void)
{
    int i;
    if (g.u_fd < 0) return -1;
    ev_n = 0;
    for (i = 0; i < g.phys_slots; i++) if (g.phys[i].pending_up) {
        ev_add(EV_ABS, ABS_MT_SLOT, i);                    /* 物理身份 = 下标 */
        ev_add(EV_ABS, ABS_MT_TRACKING_ID, -1);
    }
    for (i = 0; i < g.vslots; i++) if (g.virt[i].pending_up) {
        ev_add(EV_ABS, ABS_MT_SLOT, g.phys_slots + i);       /* 虚拟身份 = g.phys_slots + 下标 */
        ev_add(EV_ABS, ABS_MT_TRACKING_ID, -1);
    }
    for (i = 0; i < g.phys_slots; i++) {
        if (!g.phys[i].down) continue;
        ev_add(EV_ABS, ABS_MT_SLOT, i);
        ev_add(EV_ABS, ABS_MT_TRACKING_ID, i);
        ev_add(EV_ABS, ABS_MT_POSITION_X, g.phys[i].x);
        ev_add(EV_ABS, ABS_MT_POSITION_Y, g.phys[i].y);
        ev_add(EV_ABS, ABS_MT_TOOL_TYPE, MT_TOOL_FINGER);
        if (g.has_pressure) ev_add(EV_ABS, ABS_MT_PRESSURE, g.pressure_max);
    }
    for (i = 0; i < g.vslots; i++) {
        if (!g.virt[i].down) continue;
        ev_add(EV_ABS, ABS_MT_SLOT, g.phys_slots + i);
        ev_add(EV_ABS, ABS_MT_TRACKING_ID, g.phys_slots + i);
        ev_add(EV_ABS, ABS_MT_POSITION_X, g.virt[i].x);
        ev_add(EV_ABS, ABS_MT_POSITION_Y, g.virt[i].y);
        ev_add(EV_ABS, ABS_MT_TOOL_TYPE, MT_TOOL_FINGER);
        if (g.has_pressure) ev_add(EV_ABS, ABS_MT_PRESSURE, g.pressure_max);
    }
    ev_add(EV_KEY, BTN_TOUCH, any_emitted());
    ev_add(EV_KEY, BTN_TOOL_FINGER, any_emitted());
    ev_add(EV_SYN, SYN_REPORT, 0);
    if (emit_iov_writev() < 0) { g.g_reemit = 1; return -1; }
    for (i = 0; i < g.phys_slots; i++) g.phys[i].pending_up = 0;
    for (i = 0; i < g.vslots; i++) g.virt[i].pending_up = 0;
    /* 身份不再在帧末释放（静态两段，§5 改）——上面两行清 pending_up 就够了。 */
    /* §4.1：虚拟状态变化也入队（消费者按 g.virt 位自己过滤）——这样「注入不会自激」是可断言的，
     * 而不是靠「反正没把虚拟触点喂回去」的口头保证。 */
    broadcast_virt();
    return 0;
}
/**
 * (vtouch-doc: set_virtual)
 * @brief 改一个虚拟触点的状态（down/move/up）—— 单点命令与帧内命令共用这一段。
 * @param   state    virt[] 或 staged[]
 * @param   slot     客户端槽号
 * @param   name     "down"/"move"/"up"
 * @param   x        raw x
 * @param   y        raw y
 * @return  0 成功；-1 状态非法（重复 down、没 down 就 move/up）。
 * @note    只改来源状态，不写身份字段（身份发射时按下标算）。
 */

/* 单点命令与帧内命令共用这一段：state 只认 down/move/up */
int set_virtual(struct contact *state, int slot, const char *name, int x, int y)
{
    if (!strcmp(name, "down")) {
        if (state[slot].down || state[slot].pending_up) return -1;
        /* 身份不落字段：发射时按下标算（虚拟段 = g.phys_slots + 客户端槽号）。 */
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
/**
 * (vtouch-doc: owner_reset)
 * @brief 客户端断连/被踢：抬掉它所有虚拟触点并立即提交一帧。
 * @note    少了这段，客户端在 begin_frame..end_frame 中间断开会把虚拟手指永久粘在设备上。
 */

/* 客户端断开/被踢：抬掉它的虚拟触点并归还「帧内已分配但没提交」的身份。
 * 少了这段，客户端在 begin_frame..end_frame 中断开会永久占住池里的身份（之后注入全回 err point）。 */
void owner_reset(void)
{
    int i;
    for (i = 0; i < g.vslots; i++) if (g.virt[i].down) { g.virt[i].down = 0; g.virt[i].pending_up = 1; }
    memcpy(g.staged, g.virt, sizeof g.staged);
    memset(g.frame_seen, 0, sizeof g.frame_seen);
    g.frame_open = 0;
    if (emit_frame() < 0) g.g_reemit = 1;
}
/**
 * (vtouch-doc: broadcast_phys)
 * @brief 物理帧边界之后转发物理变化：每槽比快照判 down/up/move，入 region_q（喂区域线程），并在订了 phys 时推 pev。
 * @note    推的是「完整帧状态的快照」；静止不刷屏；必须在 emit_frame 之后调用（§4.1）。
 */

/* §4.1 转发内容与时机：物理帧边界（SYN）、emit_frame() 之后 —— 推的是「完整帧状态的快照」。
 * 只推状态变化（down/up/move），静止不刷屏。
 * 注意：完整版只在 subscribed 时才广播（广播只服务客户端）；现在广播还负责喂区域线程，
 * 所以每帧都跑，订阅与否只决定 pev 那一路（纯内存比较，不进热路径的写）。 */
void broadcast_phys(void)
{
    int i, lx, ly, action;
    struct vt_ev ev;
    for (i = 0; i < g.phys_slots; i++) {
        if (g.phys[i].down && !g.ps_down[i]) action = VT_DOWN;
        else if (!g.phys[i].down && g.ps_down[i]) action = VT_UP;
        else if (g.phys[i].down && (g.phys[i].x != g.ps_x[i] || g.phys[i].y != g.ps_y[i])) action = VT_MOVE;
        else continue;
        if (raw_to_logical(g.phys[i].x, 0, &lx) < 0 || raw_to_logical(g.phys[i].y, 1, &ly) < 0) continue;
        if (g.phys[i].down) { g.ps_down[i] = 1; g.ps_x[i] = g.phys[i].x; g.ps_y[i] = g.phys[i].y; }
        else g.ps_down[i] = 0;
        ev.slot = i; ev.action = action; ev.x = lx; ev.y = ly; ev.virt = 0;
        ev.ts = (action == VT_DOWN) ? g.ps_press_ns[i] : now_ns();
        vtq_push(&g.region_q, &ev);                              /* 区域线程（队列唯一消费者） */
        if (g.sub_mask & SUB_PHYS) {                             /* 外部客户端（走出站队列） */
            char msg[64];
            int n = snprintf(msg, sizeof msg, "pev %d %s %d %d", i,
                             action == VT_DOWN ? "down" : (action == VT_UP ? "up" : "move"), lx, ly);
            if (n > 0 && (size_t)n < sizeof msg) outq_push_text(msg, (size_t)n);
        }
    }
}
/**
 * (vtouch-doc: broadcast_virt)
 * @brief 虚拟触点的状态变化也入队（带 virt=1），消费者按位过滤。
 * @note    「回触不自激」可断言的那一半：区域线程遇到 virt=1 直接跳过；虚拟轨迹不进 pev（不回灌客户端自己的轨迹）。
 */

void broadcast_virt(void)
{
    int i, lx, ly, action;
    struct vt_ev ev;
    for (i = 0; i < g.vslots; i++) {
        if (g.virt[i].down && !vs_down[i]) action = VT_DOWN;
        else if (!g.virt[i].down && vs_down[i]) action = VT_UP;
        else if (g.virt[i].down && (g.virt[i].x != vs_x[i] || g.virt[i].y != vs_y[i])) action = VT_MOVE;
        else continue;
        vs_down[i] = g.virt[i].down;
        vs_x[i] = g.virt[i].x; vs_y[i] = g.virt[i].y;
        if (raw_to_logical(g.virt[i].x, 0, &lx) < 0 || raw_to_logical(g.virt[i].y, 1, &ly) < 0) continue;
        ev.slot = i; ev.action = action; ev.x = lx; ev.y = ly;
        ev.ts = now_ns(); ev.virt = 1;
        vtq_push(&g.region_q, &ev);
        /* 虚拟轨迹不进 pev：pev 只报真手指（客户端自己注入的轨迹不该被回灌） */
    }
}
