/* vt_internal.h —— vtouchd 的内部接口（单可执行、零依赖：src 目录下的 .c 一起链成一个可执行）。
 *
 * 模块划分（每个 .c 一个功能块；与 README 里的 § 编号一致）：
 *   vt_util.c    §2      小工具：参数解析 / 逻辑↔raw 换算 / 时钟
 *   vt_queue.c   §3+§4   事件队列（SPSC 无锁环）+ 出站发送队列
 *   vt_region.c  §5+§6   区域表 / 五事件判定 / 区域线程
 *   vt_input.c   §7      物理输入（认设备 / 读帧）+ 建 uinput 合并设备
 *   vt_frame.c   §8+§9   组帧（iovec / 一次 writev）+ 合帧、身份两段、转发
 *   vt_ws.c      §10     WebSocket（握手 / 帧解析 / 命令族）
 *   vtouchd.c    §0 总览 + §1 共享状态定义 + §11 进程（参数 / init / 主循环 / main）
 *
 * 共享状态只有一份：struct vt_state g（这里声明、vtouchd.c 定义）。字段靠注释说明「谁写谁读」；
 * 模块私有状态一律留在各自 .c 里当 static，不进这个结构。
 */
#ifndef VT_INTERNAL_H
#define VT_INTERNAL_H

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
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#define MAX_PHYS 64
#define MAX_VIRT 32
#define MAX_LINE 1024
#define MAX_PAYLOAD 1024
#define HTTP_MAX 4096
#define CAP_LONGS(n) (((n) + 1 + 8 * (int)sizeof(unsigned long) - 1) / (8 * (int)sizeof(unsigned long)))
#define WS_IN_MAX (MAX_PAYLOAD + 14)     /* 单帧上限 + 头（2 + 8 扩展长 + 4 掩码） */
#define SUB_PHYS   1      /* 订阅位：物理触摸流（按 slot 报 down/move/up，与区域无关） */
#define SUB_REGION 2      /* 订阅位：区域事件（五事件，按区域过滤） */
/* 事件位（订阅时的「可选订阅事件」；0 = 未设 ⇒ 全通，老脚本 sub phys / sub region 语义不变） */
#define SUBEV_DOWN  1u
#define SUBEV_ENTER 2u
#define SUBEV_MOVE  4u
#define SUBEV_EXIT  8u
#define SUBEV_UP    16u
#define SUBEV_TS    32u   /* 伪事件：随事件带墙钟毫秒（**默认带**，用户 2026-09-18 口径） */
#define SUBEV_NOTS  64u   /* 伪事件：显式**不要**时间戳（省流量时才写） */
#define VT_UP   0
#define VT_DOWN 1
#define VT_MOVE 2
#define VTQ_CAP 64
#define OUTQ_CAP 64
#define OUTQ_MSG (MAX_LINE + 8)          /* region list 这种多行响应也要放得下 */
#define MAX_REGIONS 32
#define REGION_ID_MAX 15
#define MAX_IOV 512
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

/* ---- 基本类型 ---- */
struct contact {
    int x, y, down, pending_up;          /* 来源状态：原始坐标与生命周期（身份不落字段） */
};

struct vt_ev {
    int slot;                /* 槽号（物理槽 or 虚拟槽） */
    int action;              /* 0=up 1=down 2=move */
    int x, y;                /* 逻辑坐标 */
    uint64_t ts;             /* down=按下时刻, move/up=帧到达时刻（手势识别预留） */
};

struct vtq {
    struct vt_ev buf[VTQ_CAP];
    unsigned head, tail;     /* 消费者只写 head，生产者只写 tail */
    unsigned long drops;     /* 溢出丢弃计数（仅诊断） */
};

struct region {
    char id[REGION_ID_MAX + 1];
    int type;              /* 0=rect 1=circle */
    int enabled;
    int a1, a2, a3, a4;    /* rect: x1 y1 x2 y2; circle: cx cy r */
    int mark;              /* 脚本侧"开关样式"标记（0=无 1=开）：核心存，面板**直接读共享内存**照着高亮。
                            * 由 WS 命令 `region mark <id> <0|1>` 设置（SDK: vt.mark / vt.toggle 自动调）。 */
};

struct sha1 { uint32_t h[5]; uint64_t bits; unsigned char block[64]; size_t used; };

/* ---- 共享状态（唯一定义在 vtouchd.c）---- */
/* 状态的初值表（唯一一份）：默认构建直接拿它初始化 g；VT_UI 构建拿它初始化引导副本，
 * 再由 vt_shm_create() 拷进共享内存。两者初始化表达式逐字相同 → 默认构建产物不变。 */
#define VT_STATE_DEFAULTS \
    .input_fd = -1, .u_fd = -1, .listen_fd = -1, .client_fd = -1, \
    .ws_port = 27183, .vslots = 10, .id_max = 31, \
    .region_lock = PTHREAD_MUTEX_INITIALIZER

struct vt_state {
    volatile sig_atomic_t stop_flag;
    int input_fd, u_fd, listen_fd, client_fd;
    int ws_port, vslots, phys_slots, total_slots;
    int axmin[2], axmax[2];
    int logical_width, logical_height;
    int has_pressure, pressure_max;
    int id_max;                                        /* 要给系统声明的 tracking id 上限 */
    unsigned long cap_ev[CAP_LONGS(EV_MAX)];           /* 物理屏能力镜像（validate_device 抄，setup_uinput 用）*/
    unsigned long cap_key[CAP_LONGS(KEY_MAX)];
    unsigned long cap_abs[CAP_LONGS(ABS_MAX)];
    unsigned long cap_prop[CAP_LONGS(INPUT_PROP_MAX)];
    struct input_absinfo cap_ai[ABS_MAX + 1];
    unsigned char cap_ai_ok[ABS_MAX + 1];
    char cap_name[UINPUT_MAX_NAME_SIZE];
    struct contact phys[MAX_PHYS], virt[MAX_VIRT], staged[MAX_VIRT];   /* 物理 / 虚拟 / 帧内暂存 */
    int frame_open, frame_seen[MAX_VIRT];
    volatile int g_reemit;                             /* 整帧写失败 → 主循环重发同一帧 */
    int g_emit_fail;
    int ps_down[MAX_PHYS], ps_x[MAX_PHYS], ps_y[MAX_PHYS];   /* 物理槽上一帧快照（转发判 down/up/move）*/
    uint64_t ps_press_ns[MAX_PHYS];
    int sub_mask;                                      /* 订阅位：SUB_REGION / 0 = 未订阅 */
    /* 订阅过滤器（2026-09-18「订阅时就过滤」）：0/空 = 全通（老脚本行为不变）。
     * 物理行占实测 97% 的量 ⇒ 脚本侧的 onTouch 默认只订 down,up、并可按槽过滤。 */
    unsigned sub_phys_mask;                            /* 槽位掩码；0 = 全部槽 */
    unsigned sub_phys_ev;                              /* SUBEV_* 位；0 = 全部事件 */
    char     sub_region_id[REGION_ID_MAX + 1];         /* 只看这个区域；"" = 全部区域 */
    unsigned sub_region_ev;                            /* SUBEV_* 位；0 = 全部事件 */
    int      sub_phys_ts, sub_region_ts;               /* 线路格式：事件行末是否带墙钟毫秒（裸 sub = 1，老客户端不受影响） */
    int      quiet;                                    /* 注入族（touch/down/move/up/frame）不回 ok：脚本侧不必白收白解析 */
    struct vtq region_q;                               /* 主线程 push / 区域线程 pop */
    struct region regions[MAX_REGIONS];
    int region_count;
    pthread_mutex_t region_lock;                       /* 只包住区域表读写，不在注入路径上 */
    pthread_t region_tid;
    int region_started;
};
#ifdef VT_UI
/* VT_UI：状态本体放在共享内存（面板只读映射同一份），g 只是「指向它的引用」——
 * 全库 200+ 处 g.xxx 调用点因此一行不用改。启动早期 g_ptr 指向引导副本，
 * vt_shm_create() 之后指向映射。（见 docs/UI_INTEGRATION.md §4） */
extern struct vt_state *g_ptr;
#define g (*g_ptr)
#else
extern struct vt_state g;
#endif

/* ---- vt_util.c ---- */
/* 把字符串解析成 [lo, hi] 区间内的整数（命令参数解析用）。 (vtouch-doc: parse_long) */
int parse_long(const char *s, long lo, long hi, int *out);
/* 取位图（cap_* 那几张能力位图）里的第 n 位。 (vtouch-doc: bit) */
int bit(const unsigned long *b, int n);
/* 逻辑坐标（设备像素）→ 触摸屏 raw 坐标。 (vtouch-doc: logical_to_raw) */
int logical_to_raw(int logical, int axis, int *raw);
/* raw 坐标 → 逻辑坐标（转发区域事件时用）。 (vtouch-doc: raw_to_logical) */
int raw_to_logical(int raw, int axis, int *logical);
/* 单调时钟（纳秒），事件时间戳用。 (vtouch-doc: now_ns) */
uint64_t now_ns(void);
/* 锚定「单调钟 ↔ 墙钟」偏移（启动时调一次）。 (vtouch-doc: wall_clock_anchor) */
void wall_clock_anchor(void);
/* 单调钟纳秒 → 墙钟毫秒（与脚本的 Date.now() 同基准）。 (vtouch-doc: wall_ms_from_mono) */
uint64_t wall_ms_from_mono(uint64_t mono_ns);
/* 自动探测逻辑尺寸（框架 wm size 优先、内核模式兜底；不传 -w/-h 时用）。 (vtouch-doc: detect_logical_size) */
int detect_logical_size(int *w, int *h, const char **src);

/* ---- vt_queue.c ---- */
/* 队列丢弃的诊断日志：前 3 次每次都打，之后每 100 次打一行。 (vtouch-doc: queue_drop_log) */
void queue_drop_log(const char *what, unsigned long n);
/* 事件入队（单生产者 = 主线程，消费者 = 区域线程）。 (vtouch-doc: vtq_push) */
void vtq_push(struct vtq *q, const struct vt_ev *ev);
/* 事件出队（单消费者 = 区域线程）。 (vtouch-doc: vtq_pop) */
int vtq_pop(struct vtq *q, struct vt_ev *ev);
/* 清空出站队列（新客户端接入 / 断连时）。 (vtouch-doc: outq_reset) */
void outq_reset(void);
/* 出站队列是否非空（主循环据此决定要不要挂 POLLOUT）。 (vtouch-doc: outq_pending) */
int outq_pending(void);
/* 把一段**已经成帧的字节**放入出站队列（生产者 = 主线程 / 区域线程，一把短锁只包住一次 memcpy）。 (vtouch-doc: outq_push) */
void outq_push(const char *p, size_t n);
/* 把一行文本按 WS 文本帧（未加掩码）补齐帧头后入队。 (vtouch-doc: outq_push_text) */
void outq_push_text(const char *s, size_t n);
/* 把一行文本按 WS 文本帧补齐帧头后入队；队满就不写这一帧（丢新、不丢旧）。 (vtouch-doc: outq_push_text_keep) */
int outq_push_text_keep(const char *s, size_t n);
/* 主循环唯一的刷出点：socket 可写才写，写不完留到下次；真错（EPIPE/ECONNRESET）才踢客户端。 (vtouch-doc: outq_flush) */
void outq_flush(void);

/* ---- vt_region.c ---- */
/* 清空区域表，并把代次 +1（让区域线程重置它私有的状态表）。 (vtouch-doc: regions_clear) */
void regions_clear(void);
/* 新增或覆盖一个区域（主线程持 region_lock 写表）。 (vtouch-doc: region_add) */
int region_add(const char *id, int type, int a1, int a2, int a3, int a4, int enabled);
#ifdef VT_UI
/* 按 id 删除区域（只面板用得到；region_gen 是 vt_region.c 的文件内静态，只能在这里改）。 (vtouch-doc: region_del) */
int region_del(const char *id);
/* 区域改名（目标 id 已存在则失败）。 (vtouch-doc: region_rename) */
int region_rename(const char *old_id, const char *new_id);
#endif
/* 点是否落在区域内（矩形含边界；圆按半径平方比较）。 (vtouch-doc: region_hit) */
int region_hit(const struct region *rg, int lx, int ly);
/* 发一条区域事件：订了 region 通道才入出站队列，没订就只打 (UNSUB) 日志。 (vtouch-doc: region_ev_send) */
void region_ev_send(const char *id, const char *ev, int slot, int lx, int ly, uint64_t ts_mono);
/* 物理触摸流（sub phys）：按 slot 报 down/move/up，不按区域过滤。 (vtouch-doc: phys_ev_send) */
void phys_ev_send(const struct vt_ev *ev);
unsigned vt_subev_bit(const char *ev);   /* 事件名 → SUBEV_* 位（订阅过滤器共用） */
/* 处理一个物理事件：先按 slot 报物理触摸流（sub phys），再做区域五事件判定。 (vtouch-doc: region_apply) */
void region_apply(const struct vt_ev *ev);
/* 区域线程主循环：pop region_q → region_apply；区域表代次变了就重置私有状态。 (vtouch-doc: region_thread_main) */
void *region_thread_main(void *arg);

/* ---- vt_input.c ---- */
/* 认一块设备是不是 Type-B 触摸屏（槽 + tracking id + XY 四轴 + 量程）， (vtouch-doc: validate_device) */
int validate_device(const char *p, int *slots, int *xmin, int *xmax, int *ymin, int *ymax);
/* 扫 /dev/input/event0..63，找第一块 Type-B 触摸屏。 (vtouch-doc: discover) */
int discover(char *out, size_t n);
/* 建合并 uinput 设备： (vtouch-doc: setup_uinput) */
int setup_uinput(void);
/* 读物理流：解析 Type-B 事件进 phys[]（按槽），在 SYN_REPORT 处提交一帧并转发。 (vtouch-doc: physical_events) */
void physical_events(void);

/* ---- vt_frame.c ---- */
/* 往本帧的 iovec 里追加一条 input_event（纯内存，不做系统调用）。 (vtouch-doc: ev_add) */
void ev_add(int t, int c, int v);
/* 把当前帧一次 writev 写进 uinput；EAGAIN 时等最多 3×20ms 再试。 (vtouch-doc: uinput_writev_retry) */
ssize_t uinput_writev_retry(void);
/* 提交本帧；**短写要把剩下的 iovec 补完**（只补一条会丢帧尾的 SYN_REPORT，系统里就成了半帧）。 (vtouch-doc: emit_iov_writev) */
int emit_iov_writev(void);
/* 本帧是否真的发了触点（决定 BTN_TOUCH / BTN_TOOL_FINGER 的值）。 (vtouch-doc: any_emitted) */
int any_emitted(void);
/* 把 phys[]/virt[] 合成一帧并提交：待抬 → 物理 → 虚拟 → BTN → SYN，整帧一次 writev。 (vtouch-doc: emit_frame) */
int emit_frame(void);
/* 改一个虚拟触点的状态（down/move/up）—— 单点命令与帧内命令共用这一段。 (vtouch-doc: set_virtual) */
int set_virtual(struct contact *state, int slot, const char *name, int x, int y);
/* 客户端断连/被踢：抬掉它所有虚拟触点并立即提交一帧。 (vtouch-doc: owner_reset) */
void owner_reset(void);
/* 物理帧边界之后：每槽比快照判 down/up/move，把变化入 region_q 喂区域线程（不推客户端）。 (vtouch-doc: enqueue_phys_changes) */
void enqueue_phys_changes(void);

/* ---- vt_ws.c ---- */
/* 32 位循环左移（SHA-1 内部用）。 (vtouch-doc: rol32) */
uint32_t rol32(uint32_t x, unsigned n);
/* 读 4 字节大端整数（SHA-1 内部用）。 (vtouch-doc: be32) */
uint32_t be32(const unsigned char *p);
/* 处理一个 64 字节块（SHA-1 内部）。 (vtouch-doc: sha1_block) */
void sha1_block(struct sha1 *s, const unsigned char *p);
/* SHA-1 初始化。 (vtouch-doc: sha1_init) */
void sha1_init(struct sha1 *s);
/* SHA-1 追加数据。 (vtouch-doc: sha1_update) */
void sha1_update(struct sha1 *s, const unsigned char *p, size_t n);
/* SHA-1 收尾，输出 20 字节摘要（WS 握手用）。 (vtouch-doc: sha1_final) */
void sha1_final(struct sha1 *s, unsigned char out[20]);
/* 标准 Base64 编码。 (vtouch-doc: base64) */
int base64(const unsigned char *in, size_t n, char *out, size_t cap);
/* 从 HTTP 请求头里取某个头的值（头名大小写不敏感）。 (vtouch-doc: header_value) */
int header_value(const char *req, const char *name, char *out, size_t cap);
/* 在请求头值里按逗号分词找 token（大小写不敏感，用于 Connection: Upgrade）。 (vtouch-doc: has_token) */
int has_token(const char *s, const char *token);
/* 把 len 字节写满（EINTR、短写自动续写）。 (vtouch-doc: write_full) */
int write_full(int fd, const void *buf, size_t len);
/* 读 HTTP 请求、校验 Upgrade 与 Sec-WebSocket-Key，回 101。 (vtouch-doc: websocket_handshake) */
int websocket_handshake(int fd);
/* 直接发一个 WS 帧（控制帧：close / pong 用）。 (vtouch-doc: ws_send) */
int ws_send(int fd, unsigned opcode, const unsigned char *p, size_t n);
/* 丢弃当前客户端：关连接 + 抬掉它的虚拟触点 + 清订阅位 + 清出站队列。 (vtouch-doc: drop_client) */
void drop_client(void);
/* 试着从接收缓冲里解析出一个完整帧的表头。 (vtouch-doc: ws_peek_frame) */
int ws_peek_frame(size_t *frame_len, unsigned *opcode, size_t *payload_off);
/* 取出一个完整帧的正文（必要时继续收）。 (vtouch-doc: ws_next_frame) */
int ws_next_frame(unsigned char *payload, size_t *plen, unsigned *opcode);
/* 处理客户端可读事件：一轮最多 32 个帧，解帧 → handle_line → 响应入出站队列。 (vtouch-doc: client_frame) */
int client_frame(void);
/* 建监听 socket，只绑 127.0.0.1（回环），不对外暴露。 (vtouch-doc: make_listen) */
int make_listen(void);
/* 一行命令 → 一行回包：按命令族分派（每族一个 cmd_* 函数）。 (vtouch-doc: handle_line) */
int handle_line(char *line, char *resp, size_t cap);

/* ---- vt_ws.c 的对外小接口（输入缓冲状态）---- */
/* WS 输入缓冲里是否还有没解析完的半包数据（主循环据此继续挂 POLLIN）。 (vtouch-doc: ws_has_pending) */
int ws_has_pending(void);
/* 复位 WS 输入缓冲（新客户端接入前清掉上一个客户端的残包）。 (vtouch-doc: ws_input_reset) */
void ws_input_reset(void);

#ifdef VT_UI
/* ---- vt_panel.c（核心拉起面板子进程）---- */
/* 拉起面板子进程（fork/exec app_process）并把共享内存 fd 传下去；-1 = 没起来（按无 UI 继续）。 (vtouch-doc: vt_panel_start) */
int  vt_panel_start(int shm_fd);
/* 主循环每轮调：回收子进程、判心跳、按策略重启。 (vtouch-doc: vt_panel_watchdog) */
void vt_panel_watchdog(void);
/* 停面板：SIGTERM → 800ms → SIGKILL。 (vtouch-doc: vt_panel_stop) */
void vt_panel_stop(void);
#endif

/* ---- vtouchd.c ---- */
/* 解析命令行：-w 宽 -h 高（可选，不给就自动探测）、-p 端口、-v 虚拟槽数。 (vtouch-doc: apply_args) */
void apply_args(int argc, char **argv);
/* 初始化：锚墙钟 → 尺寸门 → 清表 → 认设备 → 建 uinput → 先起监听 → 最后 EVIOCGRAB → 起区域线程。 (vtouch-doc: vtouch_init) */
int vtouch_init(int argc, char **argv);
/* 主循环一轮：poll 四路 fd（物理 / 监听 / 客户端 / 出站）→ 各自处理 → 唯一刷出点。 (vtouch-doc: vtouch_poll_step) */
int vtouch_poll_step(void);
/* 释放资源：关客户端 → 关监听 → 放 EVIOCGRAB → 关设备。 (vtouch-doc: cleanup) */
void cleanup(void);
/* 信号处理器：置退出标志，让主循环下一轮自己收尾（不在信号里做清理）。 (vtouch-doc: on_signal) */
void on_signal(int s);
/* 进程入口：装信号 → init → 主循环 → 置 stop_flag 并 join 区域线程 → cleanup。 (vtouch-doc: main) */
int main(int argc, char **argv);
/* 共享内存契约（VT_UI 构建才展开内容；必须放在 struct vt_state 定义之后）。 */
#include "vt_shm.h"

#endif /* VT_INTERNAL_H */
