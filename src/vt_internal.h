/* vt_internal.h —— vtouchd 的内部接口（单可执行、零依赖：src 目录下的 .c 一起链成一个可执行）。
 *
 * 模块划分（每个 .c 一个功能块；与 README / 工程图里的 § 编号一致）：
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
#define SUB_PHYS   1
#define SUB_REGION 2
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
    int virt;                /* 0=物理 1=虚拟 —— 区域线程按这一位过滤（防自激） */
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
};

struct sha1 { uint32_t h[5]; uint64_t bits; unsigned char block[64]; size_t used; };

/* ---- 共享状态（唯一定义在 vtouchd.c）---- */
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
    int sub_mask;                                      /* SUB_PHYS | SUB_REGION */
    struct vtq region_q;                               /* 主线程 push / 区域线程 pop */
    struct region regions[MAX_REGIONS];
    int region_count;
    pthread_mutex_t region_lock;                       /* 只包住区域表读写，不在注入路径上 */
    pthread_t region_tid;
    int region_started;
};
extern struct vt_state g;

/* ---- vt_util.c ---- */
int parse_long(const char *s, long lo, long hi, int *out);
int bit(const unsigned long *b, int n);
int logical_to_raw(int logical, int axis, int *raw);
int raw_to_logical(int raw, int axis, int *logical);
uint64_t now_ns(void);

/* ---- vt_queue.c ---- */
void queue_drop_log(const char *what, unsigned long n);
void vtq_push(struct vtq *q, const struct vt_ev *ev);
int vtq_pop(struct vtq *q, struct vt_ev *ev);
void outq_reset(void);
int outq_pending(void);
void outq_push(const char *p, size_t n);
void outq_push_text(const char *s, size_t n);
void outq_flush(void);

/* ---- vt_region.c ---- */
void regions_clear(void);
int region_add(const char *id, int type, int a1, int a2, int a3, int a4, int enabled);
int region_hit(const struct region *rg, int lx, int ly);
void region_ev_send(const char *id, const char *ev, int slot, int lx, int ly);
void region_apply(const struct vt_ev *ev);
void *region_thread_main(void *arg);

/* ---- vt_input.c ---- */
int validate_device(const char *p, int *slots, int *xmin, int *xmax, int *ymin, int *ymax);
int discover(char *out, size_t n);
int setup_uinput(void);
void physical_events(void);

/* ---- vt_frame.c ---- */
void ev_add(int t, int c, int v);
ssize_t uinput_writev_retry(void);
int emit_iov_writev(void);
int any_emitted(void);
int emit_frame(void);
int set_virtual(struct contact *state, int slot, const char *name, int x, int y);
void owner_reset(void);
void broadcast_phys(void);
void broadcast_virt(void);

/* ---- vt_ws.c ---- */
uint32_t rol32(uint32_t x, unsigned n);
uint32_t be32(const unsigned char *p);
void sha1_block(struct sha1 *s, const unsigned char *p);
void sha1_init(struct sha1 *s);
void sha1_update(struct sha1 *s, const unsigned char *p, size_t n);
void sha1_final(struct sha1 *s, unsigned char out[20]);
int base64(const unsigned char *in, size_t n, char *out, size_t cap);
int header_value(const char *req, const char *name, char *out, size_t cap);
int has_token(const char *s, const char *token);
int write_full(int fd, const void *buf, size_t len);
int websocket_handshake(int fd);
int ws_send(int fd, unsigned opcode, const unsigned char *p, size_t n);
void drop_client(void);
int ws_peek_frame(size_t *frame_len, unsigned *opcode, size_t *payload_off);
int ws_next_frame(unsigned char *payload, size_t *plen, unsigned *opcode);
int client_frame(void);
int make_listen(void);
int handle_line(char *line, char *resp, size_t cap);

/* ---- vt_ws.c 的对外小接口（输入缓冲状态）---- */
int ws_has_pending(void);
void ws_input_reset(void);

/* ---- vtouchd.c ---- */
void apply_args(int argc, char **argv);
int vtouch_init(int argc, char **argv);
int vtouch_poll_step(void);
void cleanup(void);
void on_signal(int s);
int main(int argc, char **argv);
#endif /* VT_INTERNAL_H */
