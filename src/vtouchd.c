/*
 * vtouchd.c — Android uinput 虚拟触摸守护进程 (需 root / KernelSU)
 *
 * 原理：通过 /dev/uinput 创建一个【独立】的虚拟触摸屏设备。它有自己的
 * 设备节点与事件来源，与物理触摸屏的两条事件流在 InputReader 里并行、
 * 互不覆盖，因此模拟触摸不会打断/干扰真实物理触摸。
 *
 * 用法:
 *   vtouchd [-x 宽] [-y 高] [-s socket路径]
 * 默认分辨率 1440x3168，socket 路径 /data/local/tmp/vtouch.sock
 *
 * 文本协议（每行一个命令，\n 结尾，参数空格分隔；响应一行 ok/err）:
 *   ping                       -> pong
 *   res                        -> 1440x3168
 *   tap  <x> <y> [按下毫秒]     -> ok        (默认按住 60ms)
 *   down <slot> <x> <y>         -> ok        (手指按下，slot 0..9)
 *   move <slot> <x> <y>         -> ok        (移动该指)
 *   up   <slot>                 -> ok        (抬起)
 *   swipe <x1> <y1> <x2> <y2> [毫秒] -> ok   (默认 300ms)
 *   pinch <cx> <cy> <gap1> <gap2> [毫秒] -> ok (双指缩放)
 *   reset                       -> ok        (强制抬起所有触点)
 *   quit                        -> bye       (优雅退出)
 *
 * 每行命令前可加 "tag:" 前缀，响应会带 "tag:" 回显，便于请求/响应配对:
 *   42:tap 720 1584             -> 42:ok
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <stdint.h>
#include <time.h>
#include <linux/uinput.h>
#include <linux/input.h>

#define DEFAULT_SOCKET  "/data/local/tmp/vtouch.sock"
#define MAX_SLOTS       10
#define TOUCH_MAJOR     20
#define TOUCH_PRESSURE  255
#define DEFAULT_TAP_MS  60
#define DEFAULT_SWIPE_MS 300
#define SETTLE_US       350000   /* 设备建好后留给 InputReader 发现的等待 */

static int  uinput_fd = -1;
static int  max_x = 1440, max_y = 3168;
static char sock_path[256] = DEFAULT_SOCKET;
static int  listen_fd = -1;
static volatile sig_atomic_t g_stop = 0;

static int  slot_track[MAX_SLOTS];   /* 每 slot 当前 tracking id，-1 表示无触点 */
static unsigned slot_owner[MAX_SLOTS]; /* 0=空闲；非零=连接所有者 */
static int  next_tid = 1;
static unsigned next_owner = 1;
static unsigned active_owner = 0;

/* A frame is connection-owned and is committed atomically at end_frame. */
static unsigned frame_owner = 0;
static unsigned char frame_open = 0;
static unsigned char frame_seen[MAX_SLOTS];
static int frame_state[MAX_SLOTS]; /* 1=down, 2=move, 3=up */
static int frame_x[MAX_SLOTS], frame_y[MAX_SLOTS];

static int valid_xy(int x, int y) { return x >= 0 && x < max_x && y >= 0 && y < max_y; }
static int parse_int(char *s, int *out) {
    char *e; long v;
    if (!s || !*s) return -1;
    errno = 0; v = strtol(s, &e, 10);
    if (errno || *e || v < -2147483647L || v > 2147483647L) return -1;
    *out = (int)v; return 0;
}

static pthread_mutex_t inject_lock = PTHREAD_MUTEX_INITIALIZER;

static void frame_clear(void);

struct client_ctx { int fd; unsigned owner; };

/* ---------------- 日志 ---------------- */
static void log_line(const char *level, const char *fmt, ...) {
    char ts[32];
    struct timespec t;
    va_list ap;
    clock_gettime(CLOCK_REALTIME, &t);
    snprintf(ts, sizeof(ts), "%lld.%03ld",
             (long long)t.tv_sec, t.tv_nsec / 1000000L);
    fprintf(stderr, "[%s] %s: ", ts, level);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

/* ---------------- 底层事件写入 ---------------- */
static void emit(int type, int code, int value) {
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = type;
    ev.code = code;
    ev.value = value;
    if (write(uinput_fd, &ev, sizeof(ev)) != (ssize_t)sizeof(ev))
        log_line("ERR", "write uinput: %s", strerror(errno));
}

static void syn(void) { emit(EV_SYN, SYN_REPORT, 0); }

static void sleep_ms(int ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) { /* retry */ }
}

/* ---------------- 设备创建 ---------------- */
static void set_abs(int code, int min, int max) {
    struct uinput_abs_setup sa;
    memset(&sa, 0, sizeof(sa));
    sa.code = code;
    sa.absinfo.minimum = min;
    sa.absinfo.maximum = max;
    if (ioctl(uinput_fd, UI_ABS_SETUP, &sa) < 0)
        log_line("ERR", "UI_ABS_SETUP code=%d: %s", code, strerror(errno));
}

static int create_device(void) {
    struct uinput_setup us;
    int abits[] = {
        ABS_MT_SLOT, ABS_MT_TRACKING_ID, ABS_MT_POSITION_X, ABS_MT_POSITION_Y,
        ABS_MT_TOUCH_MAJOR, ABS_MT_PRESSURE, ABS_MT_TOOL_TYPE, ABS_X, ABS_Y
    };
    int n = (int)(sizeof(abits) / sizeof(abits[0]));
    int i;

    uinput_fd = open("/dev/uinput", O_WRONLY);
    if (uinput_fd < 0) {
        log_line("FATAL", "open /dev/uinput: %s", strerror(errno));
        return -1;
    }

    ioctl(uinput_fd, UI_SET_EVBIT, EV_KEY);
    ioctl(uinput_fd, UI_SET_EVBIT, EV_ABS);
    ioctl(uinput_fd, UI_SET_EVBIT, EV_SYN);
    ioctl(uinput_fd, UI_SET_KEYBIT, BTN_TOUCH);
    /* 明确声明接触工具是手指；否则 Android 16 可能把事件当作 hover。 */
    ioctl(uinput_fd, UI_SET_KEYBIT, BTN_TOOL_FINGER);
    ioctl(uinput_fd, UI_SET_PROPBIT, INPUT_PROP_DIRECT);   /* 标记为直接输入设备(触摸屏) */
    for (i = 0; i < n; i++)
        ioctl(uinput_fd, UI_SET_ABSBIT, abits[i]);

    memset(&us, 0, sizeof(us));
    us.id.bustype = BUS_VIRTUAL;
    us.id.vendor  = 0x1234;
    us.id.product = 0x5678;
    us.id.version = 1;
    strncpy((char *)us.name, "vtouch-virtual", UINPUT_MAX_NAME_SIZE - 1);
    if (ioctl(uinput_fd, UI_DEV_SETUP, &us) < 0) {
        log_line("FATAL", "UI_DEV_SETUP: %s", strerror(errno));
        return -1;
    }

    set_abs(ABS_MT_SLOT, 0, MAX_SLOTS - 1);
    set_abs(ABS_MT_TRACKING_ID, 0, 65535);
    set_abs(ABS_MT_POSITION_X, 0, max_x);
    set_abs(ABS_MT_POSITION_Y, 0, max_y);
    set_abs(ABS_MT_TOUCH_MAJOR, 0, 255);
    set_abs(ABS_MT_PRESSURE, 0, 255);
    set_abs(ABS_MT_TOOL_TYPE, 0, MT_TOOL_FINGER);
    set_abs(ABS_X, 0, max_x);
    set_abs(ABS_Y, 0, max_y);

    if (ioctl(uinput_fd, UI_DEV_CREATE) < 0) {
        log_line("FATAL", "UI_DEV_CREATE: %s", strerror(errno));
        return -1;
    }

    for (i = 0; i < MAX_SLOTS; i++) slot_track[i] = -1;
    log_line("INFO", "虚拟触摸设备已创建 (name=vtouch-virtual, res=%dx%d)", max_x, max_y);
    return 0;
}

/* ---------------- 触点操作 ---------------- */
static void slot_select(int slot) { emit(EV_ABS, ABS_MT_SLOT, slot); }

static int any_down(void) {
    int i;
    for (i = 0; i < MAX_SLOTS; i++)
        if (slot_track[i] != -1) return 1;
    return 0;
}

static void touch_down(int slot, int x, int y) {
    int was_down = any_down();
    int id = next_tid++;
    slot_track[slot] = id;
    slot_owner[slot] = active_owner;
    slot_select(slot);
    emit(EV_ABS, ABS_MT_TRACKING_ID, id);
    emit(EV_ABS, ABS_MT_POSITION_X, x);
    emit(EV_ABS, ABS_MT_POSITION_Y, y);
    emit(EV_ABS, ABS_MT_TOUCH_MAJOR, TOUCH_MAJOR);
    emit(EV_ABS, ABS_MT_PRESSURE, TOUCH_PRESSURE);
    emit(EV_ABS, ABS_MT_TOOL_TYPE, MT_TOOL_FINGER);
    emit(EV_ABS, ABS_X, x);
    emit(EV_ABS, ABS_Y, y);
    if (!was_down) {
        emit(EV_KEY, BTN_TOUCH, 1);
        emit(EV_KEY, BTN_TOOL_FINGER, 1);
    }
    syn();
}

static void touch_move(int slot, int x, int y) {
    slot_select(slot);
    emit(EV_ABS, ABS_MT_POSITION_X, x);
    emit(EV_ABS, ABS_MT_POSITION_Y, y);
    emit(EV_ABS, ABS_X, x);
    emit(EV_ABS, ABS_Y, y);
    syn();
}

static void touch_up(int slot) {
    slot_select(slot);
    emit(EV_ABS, ABS_MT_TRACKING_ID, -1);
    slot_track[slot] = -1;
    slot_owner[slot] = 0;
    if (!any_down()) {
        emit(EV_KEY, BTN_TOOL_FINGER, 0);
        emit(EV_KEY, BTN_TOUCH, 0);
    }
    syn();
}

static void reset_all(void) {
    int i;
    for (i = 0; i < MAX_SLOTS; i++)
        if (slot_track[i] != -1) touch_up(i);
    next_tid = 1;
}

static void frame_clear(void);

static void reset_owner(unsigned owner) {
    int i;
    if (frame_open && frame_owner == owner) frame_clear();
    for (i = 0; i < MAX_SLOTS; i++)
        if (slot_track[i] != -1 && slot_owner[i] == owner) touch_up(i);
}

/* Commit a validated batch with exactly one SYN_REPORT. */
static void frame_clear(void) {
    memset(frame_seen, 0, sizeof(frame_seen));
    memset(frame_state, 0, sizeof(frame_state));
    frame_open = 0;
    frame_owner = 0;
}

static void frame_commit(void) {
    int i, before = any_down(), after;
    for (i = 0; i < MAX_SLOTS; i++) {
        if (!frame_seen[i]) continue;
        slot_select(i);
        if (frame_state[i] == 1) {
            int id = next_tid++;
            slot_track[i] = id; slot_owner[i] = active_owner;
            emit(EV_ABS, ABS_MT_TRACKING_ID, id);
            emit(EV_ABS, ABS_MT_POSITION_X, frame_x[i]);
            emit(EV_ABS, ABS_MT_POSITION_Y, frame_y[i]);
            emit(EV_ABS, ABS_MT_TOUCH_MAJOR, TOUCH_MAJOR);
            emit(EV_ABS, ABS_MT_PRESSURE, TOUCH_PRESSURE);
            emit(EV_ABS, ABS_MT_TOOL_TYPE, MT_TOOL_FINGER);
            emit(EV_ABS, ABS_X, frame_x[i]); emit(EV_ABS, ABS_Y, frame_y[i]);
        } else if (frame_state[i] == 2) {
            emit(EV_ABS, ABS_MT_POSITION_X, frame_x[i]);
            emit(EV_ABS, ABS_MT_POSITION_Y, frame_y[i]);
            emit(EV_ABS, ABS_X, frame_x[i]); emit(EV_ABS, ABS_Y, frame_y[i]);
        } else {
            emit(EV_ABS, ABS_MT_TRACKING_ID, -1);
            slot_track[i] = -1; slot_owner[i] = 0;
        }
    }
    after = any_down();
    if (!before && after) { emit(EV_KEY, BTN_TOUCH, 1); emit(EV_KEY, BTN_TOOL_FINGER, 1); }
    if (before && !after) { emit(EV_KEY, BTN_TOOL_FINGER, 0); emit(EV_KEY, BTN_TOUCH, 0); }
    syn();
    frame_clear();
}

/* ---------------- 手势 ---------------- */

static void gesture_tap(int x, int y, int ms) {
    touch_down(0, x, y);
    sleep_ms(ms);
    touch_up(0);
}

static void gesture_swipe(int x1, int y1, int x2, int y2, int ms) {
    const int steps = 60;
    int i;
    touch_down(0, x1, y1);
    for (i = 1; i <= steps; i++) {
        int x = x1 + (x2 - x1) * i / steps;
        int y = y1 + (y2 - y1) * i / steps;
        touch_move(0, x, y);
        sleep_ms(ms / steps);
    }
    touch_up(0);
}

static void gesture_pinch(int cx, int cy, int g1, int g2, int ms) {
    const int steps = 60;
    int s1 = cx - g1 / 2, s2 = cx + g1 / 2;
    int e1 = cx - g2 / 2, e2 = cx + g2 / 2;
    int i;
    touch_down(0, s1, cy);
    touch_down(1, s2, cy);
    for (i = 1; i <= steps; i++) {
        int f1 = s1 + (e1 - s1) * i / steps;
        int f2 = s2 + (e2 - s2) * i / steps;
        touch_move(0, f1, cy);
        touch_move(1, f2, cy);
        sleep_ms(ms / steps);
    }
    touch_up(1);
    touch_up(0);
}

/* ---------------- 命令执行（持锁） ---------------- */
/* 取下一个 token 并转 int；无 token 时返回缺省值 */
static int next_int(char **save, int dflt) {
    char *t = strtok_r(NULL, " ", save);
    return t ? atoi(t) : dflt;
}

/* 返回 0=ok  1=err(已填写msg)  2=quit */
static int exec_cmd(char *line, char *out, size_t outsz) {
    char *tok, *tag = NULL, *cmd;
    char *save = NULL;
    char *p;

    /* 剥掉行尾换行 */
    line[strcspn(line, "\r\n")] = 0;
    tok = strtok_r(line, " ", &save);
    if (!tok) return 1; /* 空行 */

    /* tag: 前缀 */
    if ((p = strchr(tok, ':')) != NULL && p[1] == '\0' && (p - tok) > 0) {
        tag = tok;
        tok = strtok_r(NULL, " ", &save);
        if (!tok) { snprintf(out, outsz, "%s:err empty", tag); return 1; }
    }
    cmd = tok;

#define TAGGED(_buf, _sz, _fmt, ...) \
    do { \
        if (tag) { int _w = snprintf(_buf, _sz, "%s:", tag); \
                   snprintf(_buf + _w, _sz - _w, _fmt, ##__VA_ARGS__); } \
        else snprintf(_buf, _sz, _fmt, ##__VA_ARGS__); \
    } while (0)

    if (strcmp(cmd, "begin_frame") == 0) {
        if (strtok_r(NULL, " ", &save) || frame_open) {
            TAGGED(out, outsz, "err frame already open"); return 1;
        }
        frame_clear(); frame_open = 1; frame_owner = active_owner;
        TAGGED(out, outsz, "ok"); return 0;
    }

    if (strcmp(cmd, "point") == 0) {
        char *ssl = strtok_r(NULL, " ", &save), *sst = strtok_r(NULL, " ", &save);
        char *sx = strtok_r(NULL, " ", &save), *sy = strtok_r(NULL, " ", &save);
        char *extra = strtok_r(NULL, " ", &save);
        int slot, x, y, state;
        if (!frame_open || frame_owner != active_owner) { TAGGED(out,outsz,"err frame owner"); return 1; }
        if (!ssl || !sst || !sx || !sy || extra || parse_int(ssl, &slot) || parse_int(sx, &x) ||
            parse_int(sy, &y) || slot < 0 || slot >= MAX_SLOTS || !valid_xy(x, y)) {
            TAGGED(out,outsz,"err point slot state x y"); return 1;
        }
        if (frame_seen[slot]) { TAGGED(out,outsz,"err duplicate slot %d", slot); return 1; }
        if (strcmp(sst, "down") == 0) state = 1;
        else if (strcmp(sst, "move") == 0) state = 2;
        else if (strcmp(sst, "up") == 0) state = 3;
        else { TAGGED(out,outsz,"err state down|move|up"); return 1; }
        frame_seen[slot] = 1; frame_state[slot] = state;
        frame_x[slot] = x; frame_y[slot] = y;
        TAGGED(out, outsz, "ok"); return 0;
    }

    if (strcmp(cmd, "end_frame") == 0) {
        int i;
        if (strtok_r(NULL, " ", &save) || !frame_open || frame_owner != active_owner) {
            TAGGED(out,outsz,"err frame owner or syntax"); return 1;
        }
        for (i = 0; i < MAX_SLOTS; i++) if (frame_seen[i]) {
            if (frame_state[i] == 1 && slot_track[i] != -1) {
                frame_clear(); TAGGED(out,outsz,"err slot %d already down",i); return 1;
            }
            if (frame_state[i] == 2 && (slot_track[i] == -1 || slot_owner[i] != active_owner)) {
                frame_clear(); TAGGED(out,outsz,"err slot %d owner or not down",i); return 1;
            }
            if (frame_state[i] == 3 && (slot_track[i] == -1 || slot_owner[i] != active_owner)) {
                frame_clear(); TAGGED(out,outsz,"err slot %d owner or not down",i); return 1;
            }
        }
        frame_commit(); TAGGED(out, outsz, "ok"); return 0;
    }

    if (strcmp(cmd, "ping") == 0) { TAGGED(out, outsz, "pong"); return 0; }

    if (strcmp(cmd, "res") == 0) { TAGGED(out, outsz, "%dx%d", max_x, max_y); return 0; }

    if (strcmp(cmd, "reset") == 0) { reset_all(); TAGGED(out, outsz, "ok"); return 0; }

    if (strcmp(cmd, "quit") == 0) { TAGGED(out, outsz, "bye"); return 2; }

    if (strcmp(cmd, "tap") == 0) {
        int x, y;
        if (parse_int(strtok_r(NULL, " ", &save), &x) || parse_int(strtok_r(NULL, " ", &save), &y) || !valid_xy(x,y)) { TAGGED(out,outsz,"err coordinates"); return 1; }
        char *m = strtok_r(NULL, " ", &save);
        int ms = m ? atoi(m) : DEFAULT_TAP_MS;
        gesture_tap(x, y, ms > 0 ? ms : DEFAULT_TAP_MS);
        TAGGED(out, outsz, "ok"); return 0;
    }

    if (strcmp(cmd, "down") == 0 || strcmp(cmd, "move") == 0 || strcmp(cmd, "up") == 0) {
        int slot = next_int(&save, -1);
        if (slot < 0 || slot >= MAX_SLOTS) { TAGGED(out, outsz, "err slot 0..%d", MAX_SLOTS - 1); return 1; }
        if (strcmp(cmd, "up") == 0) {
            if (slot_track[slot] == -1) { TAGGED(out, outsz, "err slot %d already up", slot); return 1; }
            if (slot_owner[slot] != active_owner) { TAGGED(out,outsz,"err slot owner"); return 1; }
            touch_up(slot);
        } else {
            int x = next_int(&save, 0);
            int y = next_int(&save, 0);
            if (!valid_xy(x, y)) { TAGGED(out,outsz,"err coordinates"); return 1; }
            if (strcmp(cmd, "down") == 0) {
                if (slot_track[slot] != -1) { TAGGED(out, outsz, "err slot %d already down", slot); return 1; }
                touch_down(slot, x, y);
            } else {
                if (slot_track[slot] == -1) { TAGGED(out, outsz, "err slot %d not down", slot); return 1; }
                if (slot_owner[slot] != active_owner) { TAGGED(out,outsz,"err slot owner"); return 1; }
                touch_move(slot, x, y);
            }
        }
        TAGGED(out, outsz, "ok"); return 0;
    }

    if (strcmp(cmd, "swipe") == 0) {
        int x1 = next_int(&save, 0);
        int y1 = next_int(&save, 0);
        int x2 = next_int(&save, 0);
        int y2 = next_int(&save, 0);
        char *m = strtok_r(NULL, " ", &save);
        int ms = m ? atoi(m) : DEFAULT_SWIPE_MS;
        gesture_swipe(x1, y1, x2, y2, ms > 0 ? ms : DEFAULT_SWIPE_MS);
        TAGGED(out, outsz, "ok"); return 0;
    }

    if (strcmp(cmd, "pinch") == 0) {
        int cx = next_int(&save, 0);
        int cy = next_int(&save, 0);
        int g1 = next_int(&save, 0);
        int g2 = next_int(&save, 0);
        char *m = strtok_r(NULL, " ", &save);
        int ms = m ? atoi(m) : DEFAULT_SWIPE_MS;
        gesture_pinch(cx, cy, g1, g2, ms > 0 ? ms : DEFAULT_SWIPE_MS);
        TAGGED(out, outsz, "ok"); return 0;
    }

    TAGGED(out, outsz, "err unknown cmd %s", cmd);
    return 1;

#undef TAGGED
}

/* ---------------- 连接处理线程 ---------------- */
static void *client_thread(void *arg) {
    struct client_ctx *ctx = (struct client_ctx *)arg;
    int cfd = ctx->fd;
    unsigned owner = ctx->owner;
    char line[512];
    size_t n = 0;
    char buf;
    ssize_t r;

    log_line("INFO", "客户端已连接 (fd=%d)", cfd);
    while (!g_stop) {
        r = recv(cfd, &buf, 1, 0);   /* 逐字节读，简单可靠 */
        if (r <= 0) break;
        if (buf == '\n') {
            char out[256];
            int rc;
            line[n] = 0;
            if (n > 0) {
                pthread_mutex_lock(&inject_lock);
                active_owner = owner;
                rc = exec_cmd(line, out, sizeof(out));
                active_owner = 0;
                pthread_mutex_unlock(&inject_lock);
                if (rc == 1) log_line("WARN", "命令被拒: %s -> %s", line, out);
                out[strcspn(out, "\n")] = 0;
                dprintf(cfd, "%s\n", out);
                if (rc == 2) { g_stop = 1; }
            }
            n = 0;
        } else {
            if (n < sizeof(line) - 1) line[n++] = buf;
        }
    }
    pthread_mutex_lock(&inject_lock);
    reset_owner(owner);
    pthread_mutex_unlock(&inject_lock);
    close(cfd);
    log_line("INFO", "客户端断开 (fd=%d)", cfd);
    free(ctx);
    return NULL;
}

/* ---------------- socket 服务 ---------------- */
static int setup_socket(void) {
    struct sockaddr_un addr;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { log_line("FATAL", "socket: %s", strerror(errno)); return -1; }

    unlink(sock_path);
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        log_line("FATAL", "bind %s: %s", sock_path, strerror(errno));
        close(fd); return -1;
    }
    chmod(sock_path, 0666);
    if (listen(fd, 16) < 0) {
        log_line("FATAL", "listen: %s", strerror(errno));
        close(fd); return -1;
    }
    listen_fd = fd;
    log_line("INFO", "监听 %s", sock_path);
    return 0;
}

/* ---------------- 信号处理 ---------------- */
static void on_signal(int s) { (void)s; g_stop = 1; }

static void cleanup(void) {
    if (uinput_fd >= 0) { ioctl(uinput_fd, UI_DEV_DESTROY); close(uinput_fd); uinput_fd = -1; }
    if (listen_fd >= 0) { close(listen_fd); listen_fd = -1; }
    unlink(sock_path);
    log_line("INFO", "已退出，设备与 socket 已清理");
}

/* ---------------- main ---------------- */
static void usage(const char *p) {
    fprintf(stderr,
        "用法: %s [-x 宽] [-y 高] [-s socket路径]\n"
        "默认: 分辨率 1440x3168, socket %s\n", p, DEFAULT_SOCKET);
}

int main(int argc, char **argv) {
    struct sigaction sa;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-x") == 0 && i + 1 < argc) max_x = atoi(argv[++i]);
        else if (strcmp(argv[i], "-y") == 0 && i + 1 < argc) max_y = atoi(argv[++i]);
        else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) { strncpy(sock_path, argv[++i], sizeof(sock_path) - 1); }
        else { usage(argv[0]); return 2; }
    }

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    if (create_device() < 0) return 1;
    usleep(SETTLE_US);
    if (setup_socket() < 0) { cleanup(); return 1; }

    log_line("INFO", "vtouchd 就绪，等待命令...");

    while (!g_stop) {
        struct sockaddr_un cli;
        socklen_t clen = sizeof(cli);
        int cfd = accept(listen_fd, (struct sockaddr *)&cli, &clen);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            if (g_stop) break;
            log_line("ERR", "accept: %s", strerror(errno));
            sleep_ms(100);
            continue;
        }
        {
            pthread_t th;
            struct client_ctx *ctx = (struct client_ctx *)calloc(1, sizeof(*ctx));
            if (!ctx) { close(cfd); continue; }
            ctx->fd = cfd;
            ctx->owner = next_owner++;
            if (ctx->owner == 0) ctx->owner = next_owner++;
            pthread_attr_t a;
            pthread_attr_init(&a);
            pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
            if (pthread_create(&th, &a, client_thread, ctx) != 0) {
                log_line("ERR", "pthread_create");
                free(ctx);
                close(cfd);
            }
            pthread_attr_destroy(&a);
        }
    }

    cleanup();
    return 0;
}