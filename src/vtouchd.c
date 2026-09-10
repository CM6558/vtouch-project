/* vtouchd: single-binary merged touchscreen merger + loopback WebSocket bridge.
 *
 * Combines vtouchmerge (EVIOCGRAB + uinput Type-B merge, logical->raw via -w/-h)
 * and vtouchws (127.0.0.1:27183, one WS text frame = one command line) into one
 * process and one poll loop, so there is no UDS hop and no half-alive state
 * (merge alive but bridge dead, or vice versa).
 *
 * Failure semantics: a bad WS client only closes that client (owner_reset +
 * close), the physical grab and uinput device stay alive. Only a full process
 * crash loses the grab; run under service.sh (restart on exit) for recovery.
 *
 * Protocol (unchanged): ping/res/reset/down/move/up/begin_frame/point/end_frame
 * Events (subscribe with "sub", stop with "unsub"): pev <slot> <down|move|up> <lx> <ly>
 * Build: same NDK line as vtouchmerge (no new dependencies).
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
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define MAX_PHYS 64
#define MAX_VIRT 32
#define MAX_LINE 512
#define MAX_PAYLOAD 4096
#define HTTP_MAX 8192

static volatile sig_atomic_t stop_flag;
static int input_fd = -1, u_fd = -1, listen_fd = -1, client_fd = -1;
static int ws_port = 27183;
static int vslots = 10, phys_slots, total_slots, axmin[2], axmax[2], selected_slot;
static int logical_width, logical_height;
struct contact { int id, x, y, down, pending_up; };
static int next_tracking_id = 1;
static struct contact phys[MAX_PHYS], virt[MAX_VIRT];
/* single-client frame staging */
static int frame_open;
static int frame_seen[MAX_VIRT];
static struct contact staged[MAX_VIRT];
static int staged_id;
/* pev 订阅 + 已上报状态（变化才广播） */
static int subscribed;
static int ps_down[MAX_PHYS], ps_x[MAX_PHYS], ps_y[MAX_PHYS];
static int ws_send(int fd, unsigned opcode, const unsigned char *p, size_t n);
static void drop_client(void);

static void on_signal(int s) { (void)s; stop_flag = 1; }

static int parse_long(const char *s, long lo, long hi, int *out)
{
    char *e; long v;
    if (!s || !*s) return -1;
    errno = 0; v = strtol(s, &e, 10);
    if (errno || *e || v < lo || v > hi) return -1;
    *out = (int)v; return 0;
}

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

/* raw -> logical：供 pev 事件上报把物理坐标转回逻辑坐标。 */
static int raw_to_logical(int raw, int axis, int *logical)
{
    int size = axis ? logical_height : logical_width;
    long span = (long)axmax[axis] - axmin[axis];
    long v;
    if (size < 2 || span <= 0) return -1;
    if (raw < axmin[axis]) raw = axmin[axis];
    if (raw > axmax[axis]) raw = axmax[axis];
    v = ((long)(raw - axmin[axis]) * (size - 1) + span / 2) / span;
    if (v < 0) v = 0; if (v > size - 1) v = size - 1;
    *logical = (int)v; return 0;
}

static int bit(const unsigned long *b, int n)
{
    return (int)((b[(unsigned)n / (8 * sizeof(unsigned long))] >> ((unsigned)n % (8 * sizeof(unsigned long)))) & 1UL);
}

static int validate_device(const char *p, int *slots, int *xmin, int *xmax, int *ymin, int *ymax)
{
    unsigned long ev[(EV_MAX + 8) / (8 * sizeof(unsigned long))];
    unsigned long abs[(ABS_MAX + 8) / (8 * sizeof(unsigned long))];
    unsigned long prop[(INPUT_PROP_MAX + 8) / (8 * sizeof(unsigned long))];
    struct input_absinfo a; int f;
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
    close(f); return 0;
}

static int discover(char *out, size_t n)
{
    int k;
    for (k = 0; k < 64; k++) {
        snprintf(out, n, "/dev/input/event%d", k);
        if (validate_device(out, &phys_slots, &axmin[0], &axmax[0], &axmin[1], &axmax[1]) == 0) return 0;
    }
    return -1;
}

static int emit(int t, int c, int v)
{
    struct input_event e; ssize_t n;
    memset(&e, 0, sizeof e); e.type = (unsigned short)t; e.code = (unsigned short)c; e.value = v;
    do n = write(u_fd, &e, sizeof e); while (n < 0 && errno == EINTR);
    return n == (ssize_t)sizeof e ? 0 : -1;
}

static int syn(void) { return emit(EV_SYN, SYN_REPORT, 0); }

static int setup_uinput(void)
{
    struct uinput_setup s; struct uinput_abs_setup a;
    u_fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (u_fd < 0) return -1;
    if (ioctl(u_fd, UI_SET_EVBIT, EV_SYN) < 0 || ioctl(u_fd, UI_SET_EVBIT, EV_KEY) < 0 ||
        ioctl(u_fd, UI_SET_EVBIT, EV_ABS) < 0 || ioctl(u_fd, UI_SET_KEYBIT, BTN_TOUCH) < 0 ||
        ioctl(u_fd, UI_SET_KEYBIT, BTN_TOOL_FINGER) < 0 ||
        ioctl(u_fd, UI_SET_PROPBIT, INPUT_PROP_DIRECT) < 0) goto fail;
    if (ioctl(u_fd, UI_SET_ABSBIT, ABS_MT_SLOT) < 0) goto fail;
    if (ioctl(u_fd, UI_SET_ABSBIT, ABS_MT_TRACKING_ID) < 0 ||
        ioctl(u_fd, UI_SET_ABSBIT, ABS_MT_POSITION_X) < 0 ||
        ioctl(u_fd, UI_SET_ABSBIT, ABS_MT_POSITION_Y) < 0 ||
        ioctl(u_fd, UI_SET_ABSBIT, ABS_MT_TOOL_TYPE) < 0) goto fail;
    memset(&s, 0, sizeof s); s.id.bustype = BUS_VIRTUAL;
    strncpy((char *)s.name, "vtouch-merged", UINPUT_MAX_NAME_SIZE - 1);
    if (ioctl(u_fd, UI_DEV_SETUP, &s) < 0) goto fail;
    memset(&a, 0, sizeof a); a.code = ABS_MT_SLOT; a.absinfo.maximum = total_slots - 1;
    if (ioctl(u_fd, UI_ABS_SETUP, &a) < 0) goto fail;
    a.code = ABS_MT_TRACKING_ID; a.absinfo.maximum = 65535;
    if (ioctl(u_fd, UI_ABS_SETUP, &a) < 0) goto fail;
    a.code = ABS_MT_POSITION_X; a.absinfo.minimum = axmin[0]; a.absinfo.maximum = axmax[0];
    if (ioctl(u_fd, UI_ABS_SETUP, &a) < 0) goto fail;
    a.code = ABS_MT_POSITION_Y; a.absinfo.minimum = axmin[1]; a.absinfo.maximum = axmax[1];
    if (ioctl(u_fd, UI_ABS_SETUP, &a) < 0) goto fail;
    a.code = ABS_MT_TOOL_TYPE; a.absinfo.maximum = MT_TOOL_PALM;
    if (ioctl(u_fd, UI_ABS_SETUP, &a) < 0) goto fail;
    if (ioctl(u_fd, UI_DEV_CREATE) < 0) goto fail;
    return 0;
fail:
    ioctl(u_fd, UI_DEV_DESTROY); close(u_fd); u_fd = -1; return -1;
}

static void cleanup(void)
{
    if (client_fd >= 0) { close(client_fd); client_fd = -1; }
    if (listen_fd >= 0) { close(listen_fd); listen_fd = -1; }
    if (input_fd >= 0) {
#ifndef VT_MERGE_TEST
        ioctl(input_fd, EVIOCGRAB, 0);
#endif
        close(input_fd); input_fd = -1;
    }
    if (u_fd >= 0) { ioctl(u_fd, UI_DEV_DESTROY); close(u_fd); u_fd = -1; }
}

static int any_down(void)
{
    int i;
    for (i = 0; i < phys_slots; i++) if (phys[i].down) return 1;
    for (i = 0; i < vslots; i++) if (virt[i].down) return 1;
    return 0;
}

static int emit_frame(void)
{
    int i;
    if (u_fd < 0) return -1;
    for (i = 0; i < phys_slots; i++) if (phys[i].pending_up) {
        if (emit(EV_ABS, ABS_MT_SLOT, i) || emit(EV_ABS, ABS_MT_TRACKING_ID, -1)) return -1;
    }
    for (i = 0; i < phys_slots; i++) if (phys[i].down) {
        if (emit(EV_ABS, ABS_MT_SLOT, i) || emit(EV_ABS, ABS_MT_TRACKING_ID, phys[i].id) ||
            emit(EV_ABS, ABS_MT_POSITION_X, phys[i].x) || emit(EV_ABS, ABS_MT_POSITION_Y, phys[i].y) ||
            emit(EV_ABS, ABS_MT_TOOL_TYPE, MT_TOOL_FINGER)) return -1;
    }
    for (i = 0; i < vslots; i++) {
        if (virt[i].pending_up) {
            if (emit(EV_ABS, ABS_MT_SLOT, phys_slots + i) || emit(EV_ABS, ABS_MT_TRACKING_ID, -1)) return -1;
        } else if (virt[i].down) {
            if (emit(EV_ABS, ABS_MT_SLOT, phys_slots + i) || emit(EV_ABS, ABS_MT_TRACKING_ID, virt[i].id) ||
                emit(EV_ABS, ABS_MT_POSITION_X, virt[i].x) || emit(EV_ABS, ABS_MT_POSITION_Y, virt[i].y) ||
                emit(EV_ABS, ABS_MT_TOOL_TYPE, MT_TOOL_FINGER)) return -1;
        }
    }
    if (emit(EV_KEY, BTN_TOUCH, any_down()) || emit(EV_KEY, BTN_TOOL_FINGER, any_down()) || syn()) return -1;
    for (i = 0; i < phys_slots; i++) phys[i].pending_up = 0;
    for (i = 0; i < vslots; i++) virt[i].pending_up = 0;
    return 0;
}

static void owner_reset(void)
{
    int i;
    for (i = 0; i < vslots; i++) if (virt[i].down) { virt[i].down = 0; virt[i].pending_up = 1; }
    frame_open = 0;
    if (emit_frame() < 0) stop_flag = 1;
}

static int set_virtual(struct contact *state, int slot, const char *name, int x, int y)
{
    if (!strcmp(name, "down")) {
        if (state[slot].down || state[slot].pending_up) return -1;
        state[slot].id = next_tracking_id++;
        if (next_tracking_id > 65535) next_tracking_id = 1;
        state[slot].down = 1;
    } else if (!strcmp(name, "move")) {
        if (!state[slot].down) return -1;
    } else if (!strcmp(name, "up")) {
        if (!state[slot].down) return -1;
        state[slot].down = 0; state[slot].pending_up = 1;
    } else {
        return -1;
    }
    state[slot].x = x; state[slot].y = y; return 0;
}

/* ---- region 匹配（native，B 方案）：固定数组、无分配、纯只读匹配 ---- */
#define MAX_REGIONS 32
#define REGION_ID_MAX 15
struct region {
    char id[REGION_ID_MAX + 1];
    int type;              /* 0=rect 1=circle */
    int enabled;
    int a1, a2, a3, a4;    /* rect: x1 y1 x2 y2; circle: cx cy r */
};
static struct region regions[MAX_REGIONS];
static int region_count;
static unsigned char slot_in[MAX_PHYS][MAX_REGIONS];    /* 上一帧该槽是否在区域内 */
static unsigned char slot_hit[MAX_PHYS][MAX_REGIONS];   /* 本次按下时是否命中（onUp 代点依据） */
static int slot_last_x[MAX_PHYS], slot_last_y[MAX_PHYS]; /* 上次 move 推送位置（位置变化才推） */

static void regions_clear(void)
{
    region_count = 0;
    memset(regions, 0, sizeof regions);
    memset(slot_in, 0, sizeof slot_in);
    memset(slot_hit, 0, sizeof slot_hit);
    memset(slot_last_x, 0, sizeof slot_last_x);
    memset(slot_last_y, 0, sizeof slot_last_y);
}

static int region_add(const char *id, int type, int a1, int a2, int a3, int a4, int enabled)
{
    struct region *rg;
    size_t n;
    if (region_count >= MAX_REGIONS || !id) return -1;
    n = strlen(id);
    if (n == 0 || n > REGION_ID_MAX) return -1;
    if (type != 0 && type != 1) return -1;
    if (a1 < 0 || a2 < 0 || a3 < 0 || a4 < 0) return -1;
    if (a1 >= logical_width || a2 >= logical_height) return -1;
    rg = &regions[region_count++];
    memset(rg, 0, sizeof *rg);
    memcpy(rg->id, id, n);
    rg->type = type;
    rg->enabled = enabled ? 1 : 0;
    rg->a1 = a1; rg->a2 = a2; rg->a3 = a3; rg->a4 = a4;
    fprintf(stderr, "vtouchd: region add %s type%d %d,%d,%d,%d en%d (total %d)\n",
        rg->id, type, a1, a2, a3, a4, rg->enabled, region_count);
    return 0;
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

/* 命中事件通知（低频：down/up/enter/exit，move 不推）；跟随 subscribed */
static void region_ev_send(const char *id, const char *ev, int slot, int lx, int ly)
{
    char msg[96];
    int n;
    if (client_fd < 0 || !subscribed) return;
    n = snprintf(msg, sizeof msg, "region_ev %s %s %d %d %d", id, ev, slot, lx, ly);
    if (n > 0 && (size_t)n < sizeof msg &&
        ws_send(client_fd, 1, (const unsigned char *)msg, (size_t)n) < 0)
        drop_client();
    fprintf(stderr, "vtouchd: ev %s %s slot%d %d,%d%s\n", id, ev, slot, lx, ly,
        (client_fd < 0 || !subscribed) ? " (NO-CLIENT/UNSUB)" : "");
}

/* 代点：区域中心注入一次 down+up。必须先于物理帧收尾后调用（调用方保证），
 * 内部两帧各自完整 emit_frame（SYN 边界正确），绝不插入物理帧中间。 */
static void region_tap(const struct region *rg)
{
    int i, x, y, lx, ly;
    if (u_fd < 0) return;
    lx = (rg->type == 1) ? rg->a1 : (rg->a1 + rg->a3) / 2;
    ly = (rg->type == 1) ? rg->a2 : (rg->a2 + rg->a4) / 2;
    if (logical_to_raw(lx, 0, &x) || logical_to_raw(ly, 1, &y)) return;
    for (i = 0; i < vslots; i++)
        if (!virt[i].down && !virt[i].pending_up) break;
    if (i >= vslots) return;                    /* 虚拟槽占满（10 指）则放弃，不硬插 */
    if (set_virtual(virt, i, "down", x, y) < 0) return;
    if (emit_frame() < 0) { set_virtual(virt, i, "up", x, y); emit_frame(); return; }
    set_virtual(virt, i, "up", x, y);
    if (emit_frame() < 0) stop_flag = 1;
    fprintf(stderr, "vtouchd: region tap %s %d,%d (slot %d)\n", rg->id, lx, ly, i);
}

/* SYN 后、物理帧注入完成后调用：五事件匹配（down/up/enter/exit + 代点）。
 * ps_down 此时还是上一帧状态（broadcast_phys 之后才更新），正好用于新按下/刚抬起判定。
 * 全部只读 phys[]，失败即跳过，绝不阻塞注入路径。 */
static void region_match(void)
{
    int i, rid, lx, ly, hit;
    struct region *rg;
    if (region_count <= 0) return;
    for (i = 0; i < phys_slots; i++) {
        if (raw_to_logical(phys[i].x, 0, &lx) < 0 || raw_to_logical(phys[i].y, 1, &ly) < 0) continue;
        for (rid = 0; rid < region_count; rid++) {
            rg = &regions[rid];
            if (!rg->enabled) { slot_in[i][rid] = 0; continue; }
            hit = region_hit(rg, lx, ly);
            if (phys[i].down) {
                if (!ps_down[i]) {
                    if (hit) { slot_hit[i][rid] = 1; region_ev_send(rg->id, "down", i, lx, ly); }
                    slot_last_x[i] = lx; slot_last_y[i] = ly;   /* 按下位置即 move 基准 */
                } else {
                    if (hit && !slot_in[i][rid]) region_ev_send(rg->id, "enter", i, lx, ly);
                    else if (!hit && slot_in[i][rid]) region_ev_send(rg->id, "exit", i, lx, ly);
                    /* move：仅在已在区域内时按位置变化推（enter/down 帧不重复推） */
                    if (hit && slot_in[i][rid] && (slot_last_x[i] != lx || slot_last_y[i] != ly)) {
                        slot_last_x[i] = lx; slot_last_y[i] = ly;
                        region_ev_send(rg->id, "move", i, lx, ly);
                    }
                }
            } else if (ps_down[i] && slot_hit[i][rid] && hit) {
                region_ev_send(rg->id, "up", i, lx, ly);
                region_tap(rg);              /* onUp 代点（区域中心） */
            }
            slot_in[i][rid] = (phys[i].down && hit) ? 1 : 0;
            if (!phys[i].down && !ps_down[i]) slot_hit[i][rid] = 0;
        }
    }
}

/* One command line -> one response line. Returns 0 on ok (resp filled). */
static int handle_line(char *line, char *resp, size_t cap)
{
    char *t, *st; int slot, x, y;
    line[strcspn(line, "\r\n")] = 0;
    t = strtok_r(line, " \t", &st);
    if (!t) { snprintf(resp, cap, "err empty"); return -1; }
    if (!strcmp(t, "ping")) { snprintf(resp, cap, "pong"); return 0; }
    if (!strcmp(t, "res")) {
        snprintf(resp, cap, "res %d %d raw %d %d %d %d",
            logical_width, logical_height, axmin[0], axmax[0], axmin[1], axmax[1]);
        return 0;
    }
    if (!strcmp(t, "reset")) {
        owner_reset(); snprintf(resp, cap, "ok"); return 0;
    }
    if (!strcmp(t, "region")) {
        char *op = strtok_r(NULL, " \t", &st);
        if (!op) { snprintf(resp, cap, "err region"); return -1; }
        if (!strcmp(op, "clear")) {
            regions_clear(); snprintf(resp, cap, "ok %d", region_count); return 0;
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
        subscribed = 1; snprintf(resp, cap, "ok"); return 0;
    }
    if (!strcmp(t, "unsub")) {
        subscribed = 0; snprintf(resp, cap, "ok"); return 0;
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
        memcpy(staged, virt, sizeof staged); staged_id = next_tracking_id;
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
        memcpy(virt, staged, sizeof virt); next_tracking_id = staged_id;
        if (emit_frame() < 0) { frame_open = 0; snprintf(resp, cap, "err frame"); return -1; }
        frame_open = 0; snprintf(resp, cap, "ok"); return 0;
    }
    snprintf(resp, cap, "err unknown"); return -1;
}

static void broadcast_phys(void)
{
    int i, lx, ly, n; char msg[64]; const char *st;
    for (i = 0; i < phys_slots; i++) {
        if (phys[i].down && !ps_down[i]) st = "down";
        else if (!phys[i].down && ps_down[i]) st = "up";
        else if (phys[i].down && (phys[i].x != ps_x[i] || phys[i].y != ps_y[i])) st = "move";
        else continue;
        if (raw_to_logical(phys[i].x, 0, &lx) < 0 || raw_to_logical(phys[i].y, 1, &ly) < 0) continue;
        if (phys[i].down) { ps_down[i] = 1; ps_x[i] = phys[i].x; ps_y[i] = phys[i].y; }
        else ps_down[i] = 0;
        n = snprintf(msg, sizeof msg, "pev %d %s %d %d", i, st, lx, ly);
        if (n <= 0 || (size_t)n >= sizeof msg ||
            ws_send(client_fd, 1, (const unsigned char *)msg, (size_t)n) < 0) {
            drop_client(); return;
        }
    }
}

static void physical_events(void)
{
    struct input_event e; ssize_t n; int syn_seen = 0;
    while ((n = read(input_fd, &e, sizeof e)) == (ssize_t)sizeof e) {
        if (e.type == EV_ABS && e.code == ABS_MT_SLOT) {
            selected_slot = e.value;
            if (selected_slot < 0 || selected_slot >= phys_slots) selected_slot = 0;
        } else if (e.type == EV_ABS && selected_slot < phys_slots) {
            if (e.code == ABS_MT_TRACKING_ID) {
                if (e.value < 0) { phys[selected_slot].down = 0; phys[selected_slot].pending_up = 1; }
                else { phys[selected_slot].id = e.value; phys[selected_slot].down = 1; }
            } else if (e.code == ABS_MT_POSITION_X) {
                phys[selected_slot].x = e.value;
            } else if (e.code == ABS_MT_POSITION_Y) {
                phys[selected_slot].y = e.value;
            } else if (e.code == ABS_X) {        /* type-A 兜底：部分模拟器 virtio 发 ABS_X/Y */
                phys[selected_slot].x = e.value;
            } else if (e.code == ABS_Y) {
                phys[selected_slot].y = e.value;
            }
        }
        if (e.type == EV_SYN && e.code == SYN_REPORT) {
            syn_seen = 1;
            if (emit_frame() < 0) { stop_flag = 1; return; }
            region_match();   /* 物理帧注入完成后才匹配+代点：帧边界安全，不阻塞注入 */
        }
    }
    if (syn_seen && client_fd >= 0 && subscribed) broadcast_phys();
    if (n < 0 && (errno == ENODEV || errno == EIO)) stop_flag = 1;
}

/* ---- WebSocket framing (loopback only) ---- */
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
    uint32_t w[80], a, b, c, d, e, f, k, t; int i;
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
    unsigned char pad[128]; size_t n, i; uint64_t bits = s->bits;
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
    size_t i = 0, o = 0; unsigned v;
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
    out[o] = 0; return (int)o;
}

static int header_value(const char *req, const char *name, char *out, size_t cap)
{
    const char *p = req, *e, *c; size_t nl = strlen(name), n;
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
    size_t n = strlen(token); const char *p = s;
    while (*p) {
        while (*p == ',' || *p == ' ' || *p == '\t') ++p;
        if (strncasecmp(p, token, n) == 0 && (p[n] == 0 || p[n] == ',' || p[n] == ' ' || p[n] == '\t')) return 1;
        while (*p && *p != ',') ++p;
    }
    return 0;
}

static int read_full(int fd, void *buf, size_t len)
{
    size_t off = 0;
    while (off < len) {
        ssize_t n = recv(fd, (char *)buf + off, len - off, 0);
        if (n == 0) return -1;
        if (n < 0) {
            if (errno == EINTR) { if (stop_flag) return -1; continue; }
            return -1;
        }
        off += (size_t)n;
    }
    return 0;
}

static int write_full(int fd, const void *buf, size_t len)
{
    size_t off = 0;
    while (off < len) {
        ssize_t n = send(fd, (const char *)buf + off, len - off, MSG_NOSIGNAL);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return -1;
        }
        off += (size_t)n;
    }
    return 0;
}

static int websocket_handshake(int fd)
{
    char req[HTTP_MAX], key[128], upgrade[64], connection[128], version[32], accept[64];
    unsigned char digest[20]; struct sha1 s; size_t used = 0; ssize_t n;
    const char *guid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    /* bounded by the socket RCVTIMEO set at accept; no infinite stall of touch */
    while (used + 1 < sizeof req) {
        n = recv(fd, req + used, 1, 0);
        if (n <= 0) return -1;
        used += (size_t)n; req[used] = 0;
        if (used >= 4 && memcmp(req + used - 4, "\r\n\r\n", 4) == 0) break;
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
        char response[256];
        int len = snprintf(response, sizeof response,
            "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
            "Connection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n", accept);
        return (len > 0 && (size_t)len < sizeof response &&
            write_full(fd, response, (size_t)len) == 0) ? 0 : -1;
    }
}

static int ws_send(int fd, unsigned opcode, const unsigned char *p, size_t n)
{
    unsigned char h[10]; size_t hn;
    if (n > MAX_PAYLOAD || (opcode >= 8 && n > 125)) return -1;
    h[0] = (unsigned char)(0x80 | (opcode & 15));
    if (n < 126) { h[1] = (unsigned char)n; hn = 2; }
    else { h[1] = 126; h[2] = (unsigned char)(n >> 8); h[3] = (unsigned char)n; hn = 4; }
    return write_full(fd, h, hn) || write_full(fd, p, n);
}

static void drop_client(void)
{
    if (client_fd >= 0) { close(client_fd); client_fd = -1; }
    subscribed = 0;
    owner_reset();
}

/* Handle exactly one WS frame on client_fd. Returns 0 to keep, -1 to drop. */
static int client_frame(void)
{
    unsigned char hdr[2], ext[8], mask[4], payload[MAX_PAYLOAD];
    unsigned opcode, len7, fin, masked; uint64_t len = 0;
    size_t i;
    char line[MAX_LINE], resp[MAX_LINE];
    if (read_full(client_fd, hdr, 2) < 0) return -1;
    fin = hdr[0] >> 7; opcode = hdr[0] & 15; masked = hdr[1] >> 7; len7 = hdr[1] & 127;
    if (!masked || !fin || (opcode != 1 && opcode != 8 && opcode != 9 && opcode != 10)) {
        ws_send(client_fd, 8, (const unsigned char *)"\x03\xea", 2); return -1;
    }
    len = len7;
    if (len7 == 126) {
        if (read_full(client_fd, ext, 2) < 0) return -1;
        len = ((uint64_t)ext[0] << 8) | ext[1];
    } else if (len7 == 127) {
        if (read_full(client_fd, ext, 8) < 0) return -1;
        len = 0; for (i = 0; i < 8; i++) len = (len << 8) | ext[i];
    }
    if (len > MAX_PAYLOAD || (opcode >= 8 && len > 125)) {
        ws_send(client_fd, 8, (const unsigned char *)"\x03\xef", 2); return -1;
    }
    if (read_full(client_fd, mask, 4) < 0 || read_full(client_fd, payload, (size_t)len) < 0) return -1;
    for (i = 0; i < (size_t)len; i++) payload[i] ^= mask[i & 3];
    if (opcode == 8) { ws_send(client_fd, 8, payload, (size_t)len); return -1; }
    if (opcode == 9) { if (ws_send(client_fd, 10, payload, (size_t)len) < 0) return -1; return 0; }
    if (opcode == 10) return 0;
    for (i = 0; i < (size_t)len; i++) if (payload[i] == '\n' || payload[i] == '\r') payload[i] = ' ';
    if ((size_t)len >= sizeof line) { ws_send(client_fd, 1, (const unsigned char *)"err line too long", 15); return 0; }
    memcpy(line, payload, (size_t)len); line[len] = 0;
    handle_line(line, resp, sizeof resp);
    if (ws_send(client_fd, 1, (const unsigned char *)resp, strlen(resp)) < 0) return -1;
    return 0;
}

static int make_listen(void)
{
    int fd, opt = 1; struct sockaddr_in a;
    struct timeval tv = { .tv_sec = 0, .tv_usec = 500000 };
    (void)tv;
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof opt);
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_CLOEXEC);
    memset(&a, 0, sizeof a); a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)ws_port);
    if (inet_pton(AF_INET, "127.0.0.1", &a.sin_addr) != 1 ||
        bind(fd, (struct sockaddr *)&a, sizeof a) < 0 || listen(fd, 8) < 0) {
        close(fd); return -1;
    }
    return fd;
}

static void apply_args(int argc, char **argv)
{
    int i, n;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-v") && i + 1 < argc && parse_long(argv[++i], 1, MAX_VIRT, &n) == 0) {
            vslots = n;
        } else if (!strcmp(argv[i], "-w") && i + 1 < argc && parse_long(argv[++i], 2, 100000, &logical_width) == 0) {
        } else if (!strcmp(argv[i], "-h") && i + 1 < argc && parse_long(argv[++i], 2, 100000, &logical_height) == 0) {
        } else if (!strcmp(argv[i], "-p") && i + 1 < argc && parse_long(argv[++i], 1, 65535, &ws_port) == 0) {
        } else if (!strcmp(argv[i], "-s") && i + 1 < argc) {
            ++i; /* accepted for compatibility with vtouchmerge start lines; unused */
        } else if (strcmp(argv[i], "-v") && strcmp(argv[i], "-w") && strcmp(argv[i], "-h") &&
                   strcmp(argv[i], "-p") && strcmp(argv[i], "-s")) {
            fprintf(stderr, "usage: %s -w width -h height [-v slots] [-p port]\n", argv[0]);
        }
    }
}

int main(int argc, char **argv)
{
    char dev[PATH_MAX];
    struct pollfd p[3];
    struct sigaction sa;
    int one = 1;
    struct timeval tv = { .tv_sec = 0, .tv_usec = 500000 };
    memset(&sa, 0, sizeof sa); sa.sa_handler = on_signal; sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, 0); sigaction(SIGINT, &sa, 0); signal(SIGPIPE, SIG_IGN);
    setvbuf(stderr, NULL, _IONBF, 0);   /* 日志实时落盘，避免全缓冲掩盖诊断 */
    apply_args(argc, argv);
    if (logical_width < 2 || logical_height < 2) {
        fprintf(stderr, "vtouchd: logical display size required (-w width -h height)\n");
        return 2;
    }
    memset(phys, 0, sizeof phys); memset(virt, 0, sizeof virt);
    if (discover(dev, sizeof dev) < 0) {
        fprintf(stderr, "vtouchd: no Type-B touchscreen found\n");
        return 2;
    }
    total_slots = phys_slots + vslots;
    if (total_slots > MAX_PHYS + MAX_VIRT) total_slots = MAX_PHYS + MAX_VIRT;
    if (setup_uinput() < 0) {
        fprintf(stderr, "vtouchd: uinput setup failed: %s\n", strerror(errno));
        return 3;
    }
    input_fd = open(dev, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (input_fd < 0) { cleanup(); return 4; }
    /* Socket first, grab last: a socket failure must never leave touch grabbed. */
    listen_fd = make_listen();
    if (listen_fd < 0) { cleanup(); return 6; }
#ifndef VT_MERGE_TEST
    if (ioctl(input_fd, EVIOCGRAB, 1) < 0) { cleanup(); return 5; }
#endif
    setsockopt(listen_fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    fprintf(stderr, "vtouchd: dev=%s phys=%d virt=%d ws=127.0.0.1:%d size=%dx%d\n",
        dev, phys_slots, vslots, ws_port, logical_width, logical_height);
    while (!stop_flag) {
        p[0] = (struct pollfd){ input_fd, POLLIN | POLLHUP | POLLERR, 0 };
        p[1] = (struct pollfd){ listen_fd, POLLIN, 0 };
        p[2] = (struct pollfd){ client_fd, -1, 0 };
        if (client_fd >= 0) p[2].events = POLLIN | POLLHUP | POLLERR;
        int r = poll(p, client_fd >= 0 ? 3 : 2, 1000);
        if (r < 0) { if (errno == EINTR) continue; break; }
        if (p[0].revents & POLLIN) physical_events();
        if (p[0].revents & (POLLHUP | POLLERR)) break;
        if (p[1].revents & POLLIN) {
            int ncf = accept4(listen_fd, NULL, NULL, SOCK_CLOEXEC);
            if (ncf >= 0) {
                if (client_fd >= 0) {
                    fprintf(stderr, "vtouchd: kicking old ws client\n");
                    close(client_fd); client_fd = -1; frame_open = 0; subscribed = 0;
                }
                setsockopt(ncf, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
                setsockopt(ncf, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
                if (websocket_handshake(ncf) != 0) {
                    fprintf(stderr, "vtouchd: ws handshake failed\n");
                    close(ncf);
                } else {
                    client_fd = ncf;
                    fprintf(stderr, "vtouchd: ws client connected\n");
                }
            }
        }
        if (client_fd >= 0 && (p[2].revents & (POLLHUP | POLLERR))) {
            fprintf(stderr, "vtouchd: ws client hung up\n");
            drop_client(); continue;
        }
        if (client_fd >= 0 && (p[2].revents & POLLIN)) {
            if (client_frame() < 0) {
                fprintf(stderr, "vtouchd: ws client dropped\n");
                drop_client();
            }
        }
    }
    cleanup();
    return 0;
}
