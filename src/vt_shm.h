/* vt_shm.h —— 核心 ⇄ 面板的共享内存契约（VT_UI 构建才用到；默认构建里这个文件被忽略）。
 *
 * 设计要点（与 docs/UI_INTEGRATION.md §4 一致）：
 *   · 一个 memfd，三段映射；面板对「状态段」只有读权限（MMU 强制，越界写只死面板）；
 *   · 没有命令通道、没有 socketpair/eventfd/opcode/解析器 —— 面板的"操作"全是数据写入：
 *       改区域表 → 写区 B 的「编辑邮箱」（单槽，非请求-应答）；停引擎 → 写 stop_req + SIGTERM 父进程；
 *   · 区域的**唯一真相**永远是核心的 g.regions[]（面板只读）；面板把想做的编辑投进邮箱，
 *     核心下一轮自然吃掉并按既有 region_add/region_del/region_rename 语义生效（≤1ms）。
 *
 * 文件布局（页对齐；实际偏移由头部记录，双方都按头部走，不写死）：
 *   [0]                     struct vt_shm_header     （核心 RW / 面板 RO）
 *   [off_state]             struct vt_state          （核心 RW / 面板 RO）
 *   [off_b]                 struct vt_shm_b          （核心 RW / 面板 RW）
 *   [off_c]                 struct vt_shm_c          （核心 W  / 面板 RO）
 *
 * 依赖：本文件必须在 struct vt_state 定义之后包含（见 vt_internal.h 末尾）。
 */
#ifndef VT_SHM_H
#define VT_SHM_H
#ifdef VT_UI

#include <stdint.h>

#define VT_SHM_MAGIC    0x56544D31u   /* 'V' 'T' 'M' '1' */
#define VT_SHM_VERSION  1u            /* 布局语义版本：不匹配就拒绝启动面板 */
#define VT_SHM_FD       3             /* 传给面板子进程的固定 fd 号 */

#define VT_EDIT_NONE   0
#define VT_EDIT_ADD    1              /* 新增或同 id 覆盖（语义同 region_add） */
#define VT_EDIT_DEL    2
#define VT_EDIT_RENAME 3
#define VT_EDIT_CLEAR  4

#define VT_RING_SLOTS  64             /* 事件环：与核心 outq 容量同量级 */
#define VT_RING_LINE   96             /* 一条 region_ev 文本行长上限 */

/* 头部：双方都要先校验 magic/version，再按 off_* 定位各段。 */
struct vt_shm_header {
    uint32_t magic;
    uint32_t version;
    uint32_t page;                    /* 页大小（定位用，本机 4096） */
    uint32_t off_state, size_state;   /* 区 A：状态 */
    uint32_t off_b, size_b;           /* 区 B：双方可写 */
    uint32_t off_c, size_c;           /* 区 C：事件环 */
    volatile uint32_t hb;             /* 核心心跳（面板看它判断核心是否还活） */
    volatile uint32_t ui_hb;          /* 面板心跳（核心看它判断面板是否还活） */
    int32_t  logical_w, logical_h;    /* 竖屏逻辑尺寸（面板换算要用） */
    int32_t  core_pid;                /* 核心 pid（面板要停引擎时给它发信号） */
    int32_t  panel_pid;               /* 面板 pid（核心诊断用；面板起来后自己填） */
};

/* 编辑邮箱：面板写、核心吃。seq 从 1 单调递增；核心只看 seq 变没变。 */
struct vt_shm_edit {
    volatile uint32_t seq;
    uint32_t op;                      /* VT_EDIT_* */
    char     id[16];
    char     new_id[16];
    int32_t  type, enabled, a1, a2, a3, a4;
};

/* 区 B：面板矩形用 seq 奇偶校验（发布中=奇数），核心拿不准时保守「不吞」。 */
struct vt_shm_b {
    volatile int32_t  lock;           /* 自旋锁（只护邮箱，别在热路径上拿） */
    struct vt_shm_edit edit;
    volatile uint32_t edit_applied;   /* 核心已应用的 seq（诊断） */
    volatile int32_t  stop_req;       /* 面板请求停引擎（核心据此退出；面板另发 SIGTERM 兜底） */
    volatile uint32_t rect_seq;       /* 面板矩形发布序号：偶=稳定 */
    int32_t  panel_visible;           /* 面板当前是否可见（不可见 = 一律不吞） */
    int32_t  rot;                     /* 面板上报的当前显示方向（诊断/换算校验） */
    int32_t  rx1, ry1, rx2, ry2;      /* 面板矩形，**竖屏逻辑坐标**（外空间） */
};

/* 区 C：事件环（核心写 tail，面板读 head；SPSC，满则丢最旧）。 */
struct vt_shm_c {
    volatile uint32_t head;
    volatile uint32_t tail;
    uint32_t drops;
    char line[VT_RING_SLOTS][VT_RING_LINE];
};

/* ---- 核心侧 ---- */
/* 建 memfd、按契约映射、把初值状态拷进去，并把 g 指过来。 (vtouch-doc: vt_shm_create) */
int  vt_shm_create(void);
/* 每轮主循环调：推进核心心跳（面板据此判断核心死活）。 (vtouch-doc: vt_shm_tick) */
void vt_shm_tick(void);
/* 事件环入队（核心唯一写方）。 (vtouch-doc: vt_shm_ring_push) */
void vt_shm_ring_push(const char *s, size_t n);
/* 吃一次编辑邮箱（面板有编辑就应用）。 (vtouch-doc: vt_shm_edit_apply) */
void vt_shm_edit_apply(void);
/* 读面板矩形（seqlock 一次重试）；返回 0 = 可信，-1 = 拿不准（调用方应保守不吞）。 (vtouch-doc: vt_shm_panel_rect) */
int  vt_shm_panel_rect(int *x1, int *y1, int *x2, int *y2);
/* 面板是否可见且矩形可信（吞触摸判定的唯一入口）。 (vtouch-doc: vt_shm_should_eat) */
int  vt_shm_should_eat(int lx, int ly);
/* 面板心跳值（核心的看门狗用它判面板死活）。 (vtouch-doc: vt_shm_ui_hb) */
uint32_t vt_shm_ui_hb(void);
/* 面板是否请求停引擎。 (vtouch-doc: vt_shm_stop_req) */
int  vt_shm_stop_req(void);

/* ---- 面板侧 ---- */
/* 从固定 fd 附着共享内存、校验契约；返回 0 成功。 (vtouch-doc: vt_shm_attach) */
int  vt_shm_attach(int fd);
/* 取各段指针（面板侧 getter）。 (vtouch-doc: vt_shm_state) */
struct vt_state *vt_shm_state(void);
struct vt_shm_b *vt_shm_b(void);
struct vt_shm_c *vt_shm_c(void);
/* 面板心跳 + 读核心心跳（返回 0 = 核心还活）。 (vtouch-doc: vt_shm_ui_tick) */
int  vt_shm_ui_tick(void);
/* 面板投一条编辑进邮箱（单槽，覆盖式）。 (vtouch-doc: vt_shm_post_edit) */
void vt_shm_post_edit(const struct vt_shm_edit *e);
/* 面板发布矩形（seqlock 写侧）。 (vtouch-doc: vt_shm_publish_rect) */
void vt_shm_publish_rect(int visible, int rot, int x1, int y1, int x2, int y2);

#endif /* VT_UI */
#endif /* VT_SHM_H */
