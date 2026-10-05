/* vt_shm.c —— 共享内存的建/附着与访问器（VT_UI 构建才参与编译）。
 *
 * 谁在里面：核心侧（建、心跳、事件环写、吃编辑邮箱、读面板矩形）；
 *           面板侧（附着、读状态、投编辑、发布矩形、心跳）用 -DVT_UI_PANEL 编同一份代码的另一半，
 *           这样两端对布局的理解永远只有一处来源（这里 + vt_shm.h）。
 *
 * 依赖：包含 vt_internal.h 之后，g 就是共享内存里的那份状态（见 vt_internal.h 的 g_ptr 宏）。
 */
#include "vt_internal.h"
#ifdef VT_UI

#include <sys/mman.h>
#include <sys/syscall.h>

/* 三个段的指针（各自只在一侧有意义） */
static struct vt_state   *S_state;
static struct vt_shm_b   *S_b;
static struct vt_shm_c   *S_c;
static void              *S_base;      /* 共享内存基址（两侧都用） */

/* 单槽邮箱的自旋锁：只在「面板投一次编辑 / 核心吃一次编辑」时拿，不在注入热路径上。
 * 2026-10-05 评审修复：上限从「循环计数」改**单调钟**（~20ms），且**返回成败** —— 原实现超限直接
 * break 后调用方在无锁下继续写、随后 shm_unlock 还会清掉对方仍持有的锁（互斥被第三方释放）。 */
static int shm_lock(struct vt_shm_b *b)
{
    uint64_t t0 = now_ns();
    while (__sync_lock_test_and_set(&b->lock, 1)) {
        if (now_ns() - t0 > 20000000ull) return -1;    /* 极端情况不要死等：真正放弃这次编辑 */
    }
    return 0;
}
static void shm_unlock(struct vt_shm_b *b) { __sync_lock_release(&b->lock); }

/* ===================== 帧区访问（核心 / 面板两侧共用） ===================== */
/* 区 D 布局（两侧同一来源）：[帧头（页对齐）][缓冲 0][缓冲 1]；偏移与缓冲尺寸由头部字段记录
 * （off_frame / off_fbuf / frame_buf_bytes），双方都按头部走、不写死（同全文件纪律）。 */

/**
 * (vtouch-doc: vt_shm_frame)
 * @brief 帧区头指针（核心读 / 面板写；区 D 布局见 vt_shm.h 与 spec VISION §3.1）。
 * @return  帧区头；未建 / 未附着时 NULL。
 */
struct vt_shm_frame_hdr *vt_shm_frame(void)
{
    struct vt_shm_header *h;
    if (!S_base) return NULL;
    h = (struct vt_shm_header *)S_base;
    return (struct vt_shm_frame_hdr *)((char *)S_base + h->off_frame);
}

/**
 * (vtouch-doc: vt_shm_frame_buf)
 * @brief 第 idx 块帧缓冲基址（idx = 0/1；双缓冲，spec VISION §3.1）。
 * @param   idx      缓冲下标：0 / 1
 * @return  缓冲基址；越界或未附着时 NULL。
 */
uint8_t *vt_shm_frame_buf(int idx)
{
    struct vt_shm_header *h;
    if (!S_base || idx < 0 || idx > 1) return NULL;
    h = (struct vt_shm_header *)S_base;
    return (uint8_t *)S_base + h->off_fbuf + (size_t)idx * h->frame_buf_bytes;
}

/* ===================== 核心侧 ===================== */
#ifndef VT_UI_PANEL

static uint32_t page_size(void)
{
    long p = sysconf(_SC_PAGESIZE);
    return (uint32_t)(p > 0 ? p : 4096);
}
static uint32_t pg_up(uint32_t n, uint32_t p) { return (n + p - 1) / p * p; }

int vt_shm_create(void)
{
    uint32_t p = page_size();
    uint32_t off_state = p;
    uint32_t size_state = pg_up((uint32_t)sizeof(struct vt_state), p);
    uint32_t off_b = off_state + size_state;
    uint32_t size_b = pg_up((uint32_t)sizeof(struct vt_shm_b), p);
    uint32_t off_c = off_b + size_b;
    uint32_t size_c = pg_up((uint32_t)sizeof(struct vt_shm_c), p);
    uint32_t off_frame = off_c + size_c;                 /* 区 D：帧区（帧头 + 双缓冲） */
    uint32_t fhdr = pg_up((uint32_t)sizeof(struct vt_shm_frame_hdr), p);
    uint64_t fbuf64 = (uint64_t)g.logical_width * (uint64_t)g.logical_height * 4u;
    uint32_t fbuf_bytes, size_frame, total;
    struct vt_shm_header *h;
    struct vt_shm_frame_hdr *fh;
    void *base;
    int fd;

    /* 帧缓冲 = 逻辑宽 × 逻辑高 × 4（两方向同字节数；spec VISION §3.1）。尺寸防御：逻辑尺寸没拿到 /
     * 荒唐大（uint32 偏移会回绕）就不建 —— 正常逻辑尺寸远小于上限（本机 1440×3168 → 18.2MB/块）。 */
    if (fbuf64 == 0 || fbuf64 > 0x40000000ull) {
        fprintf(stderr, "vtouchd: 逻辑尺寸异常（%dx%d）→ 共享内存不建\n", g.logical_width, g.logical_height);
        return -1;
    }
    fbuf_bytes = (uint32_t)fbuf64;
    size_frame = fhdr + 2u * fbuf_bytes;
    total = off_frame + size_frame;

    fd = (int)syscall(SYS_memfd_create, "vtouch-shm", 0u);
    if (fd < 0) { fprintf(stderr, "vtouchd: memfd_create 失败: %s\n", strerror(errno)); return -1; }
    if (ftruncate(fd, (off_t)total) != 0) {
        fprintf(stderr, "vtouchd: ftruncate(%u) 失败: %s\n", total, strerror(errno));
        close(fd); return -1;
    }
    base = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) {
        fprintf(stderr, "vtouchd: mmap 共享内存失败: %s\n", strerror(errno));
        close(fd); return -1;
    }
    /* 头部 + 三区清零后填契约 */
    memset(base, 0, total);
    h = (struct vt_shm_header *)base;
    h->magic = VT_SHM_MAGIC; h->version = VT_SHM_VERSION; h->page = p;
    h->off_state = off_state; h->size_state = size_state;
    h->off_b = off_b; h->size_b = size_b;
    h->off_c = off_c; h->size_c = size_c;
    h->off_frame = off_frame; h->size_frame = size_frame;
    h->off_fbuf = off_frame + fhdr; h->frame_buf_bytes = fbuf_bytes;
    h->logical_w = g.logical_width; h->logical_h = g.logical_height;
    h->core_pid = (int32_t)getpid();
    /* 帧头常量（核心建）：magic/version 标明「这一区已就绪」；其余字段由面板抓帧时写。 */
    fh = (struct vt_shm_frame_hdr *)((char *)base + off_frame);
    fh->magic = VT_FRAME_MAGIC;
    fh->version = VT_FRAME_VER;

    S_state = (struct vt_state *)((char *)base + off_state);
    S_b = (struct vt_shm_b *)((char *)base + off_b);
    S_c = (struct vt_shm_c *)((char *)base + off_c);
    S_base = base;

    /* 状态本身进共享内存：把当前状态（= 初值 + apply_args 已解析出的参数）拷进去，
     * 再把 g 指过去 —— 此后全库 200+ 处调用点一行不改。 */
    memcpy(S_state, g_ptr, sizeof(struct vt_state));
    g_ptr = S_state;

    fprintf(stderr, "vtouchd: 共享内存就绪 fd=%d total=%u state@%u(%u) b@%u c@%u frame@%u(%u)\n",
            fd, total, off_state, size_state, off_b, off_c, off_frame, size_frame);
    return fd;                                  /* 返回 fd：fork 时原样传给面板子进程 */
}

/* 取点（T2.8）的转变追踪：pick_mode 0→1 的时刻（惰性 20s 超时的判据）。
 * 为什么每轮看一眼（而不是只在 pick_wanted 里记）：pick_wanted 只在「新按下」时被调
 * （vt_frame.c）——若起点也在那时才记，首次观察必然落在按下这一刻，超时永不可达。
 * 这里 tick 每轮观察一次（核心空闲 1s 兜底一轮），0→1 即记起点；20s 的判定仍是惰性的
 * （下一次按下时判，见 vt_shm_pick_wanted）。 */
static uint64_t S_pick_t0_ns;        /* 单调纳秒；0 = 没在取点态 */

static void pick_track(void)
{
    if (!S_b) return;
    if (S_b->pick_mode) { if (!S_pick_t0_ns) S_pick_t0_ns = now_ns(); }
    else S_pick_t0_ns = 0;
}

void vt_shm_tick(void)
{
    if (S_c) ((struct vt_shm_header *)((char *)S_base))->hb++;
    pick_track();                       /* 取点转变追踪：0→1 记起点（见上） */
}

uint32_t vt_shm_ui_hb(void)
{
    struct vt_shm_header *h;
    if (!S_base) return 0;
    h = (struct vt_shm_header *)S_base;
    return h->ui_hb;
}

/**
 * (vtouch-doc: vt_shm_panel_rot)
 * @brief 面板上报的当前显示方向（0..3；视觉帧复用/换算校验用）。
 * @return  当前方向；-1 = 拿不到（没建共享内存）。
 * @note    面板每帧经 publish_rect 上报（g_rot）；与帧头里的 rotation（抓帧时方向）是两回事。
 */
int vt_shm_panel_rot(void)
{
    if (!S_b) return -1;
    return S_b->rot;                     /* 面板每帧经 publish_rect 上报的当前显示方向（0..3） */
}

int vt_shm_stop_req(void)
{
    return (S_b && S_b->stop_req) ? 1 : 0;
}

void vt_shm_ring_push(const char *s, size_t n)
{
    uint32_t tail, rd;
    if (!S_c || !S_b) return;
    if (n >= VT_RING_LINE) n = VT_RING_LINE - 1;
    tail = __atomic_load_n(&S_c->tail, __ATOMIC_RELAXED);
    rd = __atomic_load_n(&S_b->ring_read, __ATOMIC_ACQUIRE);   /* 消费者的单调计数 */
    /* 满 = 环里已有 VT_RING_SLOTS 行没被读走（无符号回绕安全）。**丢这一条新的**：
     * 绝不替消费者推进 ring_read —— 那会丢掉面板可能正在读的那一格（v2 的老行为）。 */
    if (tail - rd >= VT_RING_SLOTS) { S_c->drops++; return; }
    memcpy(S_c->line[tail % VT_RING_SLOTS], s, n);
    S_c->line[tail % VT_RING_SLOTS][n] = 0;
    __atomic_store_n(&S_c->tail, tail + 1u, __ATOMIC_RELEASE);  /* release：line 一定先于 tail 可见 */
}

void vt_shm_edit_apply(void)
{
    struct vt_shm_edit e;
    int op;
    if (!S_b) return;
    if (S_b->edit.seq == S_b->edit_applied) return;    /* 没新编辑，热路径零成本 */
    if (shm_lock(S_b) != 0) return;                    /* 锁拿不到：本轮跳过（不动 edit_applied，下轮重试） */
    memcpy(&e, (const void *)&S_b->edit, sizeof e);
    op = (int)e.op;
    switch (op) {
    case VT_EDIT_CLEAR:  regions_clear(); break;
    case VT_EDIT_ADD:    region_add(e.id, (int)e.type, (int)e.a1, (int)e.a2, (int)e.a3, (int)e.a4, (int)e.enabled); break;
    case VT_EDIT_DEL:    region_del(e.id); break;
    case VT_EDIT_RENAME: region_rename(e.id, e.new_id); break;
    case VT_EDIT_OP_PUT:   vt_ops_put(&e.payload); break;    /* 操作载荷在 e.payload（PUT 专用） */
    case VT_EDIT_OP_DEL:   vt_ops_del(e.id); break;
    case VT_EDIT_OP_CLEAR: vt_ops_clear(); break;
    case VT_EDIT_OP_RUN: {                   /* 起跑；名字按上限截断打印（邮箱载荷可能没终止符） */
        char nm[OP_NAME_MAX + 2];
        size_t nn = strnlen(e.id, sizeof e.id);
        if (nn > OP_NAME_MAX) nn = OP_NAME_MAX;
        memcpy(nm, e.id, nn); nm[nn] = 0;
        fprintf(stderr, "vtouchd: op 编辑 run %s\n", nm);
        vt_ops_run(e.id, NULL);                              /* 手动运行：无触发数据（全部变量无值） */
        break;
    }
    case VT_EDIT_OP_STOP:
        fprintf(stderr, "vtouchd: op 编辑 stop\n");
        vt_ops_abort("停止按钮");
        break;
    case VT_EDIT_BIND:   region_bind(e.id, e.new_id, (int)e.type); break;    /* id=区域, new_id=操作名（"-"=解除）, type=时机(0/1/2) */
    case VT_EDIT_KIND:   region_kind_set(e.id, (int)e.type); break;          /* id=区域, type=kind(0/1) */
    default: break;
    }
    S_b->edit_applied = e.seq;
    shm_unlock(S_b);
}

int vt_shm_panel_rect(int *x1, int *y1, int *x2, int *y2)
{
    uint32_t s1, s2;
    int v, ax1, ay1, ax2, ay2, tries;
    if (!S_b) return -1;
    for (tries = 0; tries < 2; tries++) {               /* seqlock：一次重试 */
        s1 = S_b->rect_seq;
        if (s1 & 1u) continue;                          /* 面板正在发布 */
        v = S_b->panel_visible;
        ax1 = S_b->rx1; ay1 = S_b->ry1; ax2 = S_b->rx2; ay2 = S_b->ry2;
        __sync_synchronize();
        s2 = S_b->rect_seq;
        if (s1 != s2) continue;                         /* 发布过了，重来一次 */
        if (!v) return -1;                              /* 不可见 = 不吞 */
        *x1 = ax1; *y1 = ay1; *x2 = ax2; *y2 = ay2;
        return 0;
    }
    return -1;                                          /* 拿不准：保守不吞 */
}

int vt_shm_should_eat(int lx, int ly)
{
    int x1, y1, x2, y2;
    if (!S_b) return 0;
    if (S_b->stop_req) return 0;                        /* 面板要退出：别吞，让手指回系统 */
    if (vt_shm_panel_rect(&x1, &y1, &x2, &y2) != 0) return 0;
    if (lx < x1 || lx > x2 || ly < y1 || ly > y2) return 0;
    return 1;
}

/* ---- 取点（T2.8）：面板 [取点] → 核心吞一次触摸、回填竖屏逻辑坐标 ---- */

#define VT_PICK_TIMEOUT_NS (20ull * 1000ull * 1000ull * 1000ull)   /* 20s（spec §5 防呆） */

int vt_shm_pick_wanted(void)
{
    if (!S_b) return 0;
    if (!S_b->pick_mode) { S_pick_t0_ns = 0; return 0; }
    if (!S_pick_t0_ns) S_pick_t0_ns = now_ns();   /* tick 还没看过（按下先到）：从这一刻起算 */
    if (now_ns() - S_pick_t0_ns > VT_PICK_TIMEOUT_NS) {
        S_b->pick_mode = 0;                       /* 惰性清除：下一次按下时判（spec §5 防呆） */
        S_pick_t0_ns = 0;
        fprintf(stderr, "vtouchd: 取点 超时清除\n");
        return 0;
    }
    return 1;
}

void vt_shm_pick_captured(int lx, int ly)
{
    if (!S_b) return;
    S_b->pick_x = lx;
    S_b->pick_y = ly;
    __sync_synchronize();                         /* 坐标先于 pick_seq 可见（面板据 seq 变化取坐标） */
    S_b->pick_seq++;
    S_b->pick_mode = 0;                           /* 自动清（spec §5） */
    S_pick_t0_ns = 0;                             /* 转变追踪复位：下一次 request 的 0→1 重新起算 */
    fprintf(stderr, "vtouchd: 取点 捕获 %d,%d\n", lx, ly);
}

void vt_shm_pick_panel_died(void)
{
    if (!S_b) return;
    if (!S_b->pick_mode) return;                  /* 没在取点态：清无可清（不打日志，避免每次面板退出都多一行） */
    S_b->pick_mode = 0;
    S_pick_t0_ns = 0;
    fprintf(stderr, "vtouchd: 取点 面板死亡清除\n");
}

#endif /* !VT_UI_PANEL */

/* ===================== 面板侧 ===================== */
#ifdef VT_UI_PANEL

/* 单调毫秒（面板侧自用：心跳判据这类东西都该用单调钟/墙钟，别用拍数）。 */
static uint64_t mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)(ts.tv_nsec / 1000000);
}

int vt_shm_attach(int fd)
{
    struct vt_shm_header *h;
    uint32_t total;
    off_t sz;
    void *base;

    sz = lseek(fd, 0, SEEK_END);
    if (sz <= 0) { fprintf(stderr, "vtouch-ui: 共享内存 fd=%d 长度异常 (%ld)\n", fd, (long)sz); return -1; }
    total = (uint32_t)sz;
    /* 头部 + 区 B 是面板可写的：头部里 ui_hb / panel_pid 本来就是面板写的（核心只看不改），
     * 区 B 是编辑邮箱 + 面板矩形。**状态段（区 A）保持只读** —— "面板改不了核心状态"靠的就是它。
     * 踩过的坑：把头部也映射成只读 → 面板写 ui_hb 直接 SIGSEGV(SEGV_ACCERR)，backtrace 落在
     * vt_shm_ui_tick+32、fault addr = 头部页 + 0x28（ui_hb 的偏移）。 */
    base = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) {
        fprintf(stderr, "vtouch-ui: mmap(fd=%d) 失败: %s\n", fd, strerror(errno));
        return -1;
    }
    h = (struct vt_shm_header *)base;
    if (h->magic != VT_SHM_MAGIC || h->version != VT_SHM_VERSION) {
        fprintf(stderr, "vtouch-ui: 共享内存契约不匹配 magic=0x%x version=%u（预期 0x%x/%u）\n",
                h->magic, h->version, VT_SHM_MAGIC, VT_SHM_VERSION);
        munmap(base, total); return -1;
    }
    if (h->off_state + h->size_state > total || h->off_b + h->size_b > total ||
        h->off_c + h->size_c > total || h->off_frame + h->size_frame > total) {
        fprintf(stderr, "vtouch-ui: 共享内存偏移越界（头被写坏了）\n");
        munmap(base, total); return -1;
    }
    S_base = base;
    /* 区 A 单独再映射一段只读并**盖住**刚才的可写映射：状态只能读，"写它=SIGSEGV"由 MMU 保证。 */
    {
        void *ro = mmap(NULL, h->size_state, PROT_READ, MAP_SHARED, fd, (off_t)h->off_state);
        if (ro == MAP_FAILED) {
            fprintf(stderr, "vtouch-ui: mmap(区A 只读) 失败: %s\n", strerror(errno));
            munmap(base, total); return -1;
        }
        S_state = (struct vt_state *)ro;
    }
    S_c = (struct vt_shm_c *)((char *)base + h->off_c);              /* 只读 */
    /* 区 B 单独再映射一段 RW：这是面板唯一能写的地方（编辑邮箱 / 面板矩形 / 停引擎 / 环读下标）。
     * 三段分开映射之后，"面板改不了状态"是 MMU 强制的，不是纪律。 */
    S_b = (struct vt_shm_b *)mmap(NULL, h->size_b, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)h->off_b);
    if (S_b == MAP_FAILED) {
        fprintf(stderr, "vtouch-ui: mmap(区B) 失败: %s\n", strerror(errno));
        munmap(base, total); S_b = NULL; return -1;
    }
    g_ptr = S_state;                       /* 只读辅助函数（raw_to_logical 等）直接用 g */
    fprintf(stderr, "vtouch-ui: 共享内存附着成功 total=%u state@%u b@%u c@%u frame@%u 逻辑=%dx%d core_pid=%d\n",
            total, h->off_state, h->off_b, h->off_c, h->off_frame, h->logical_w, h->logical_h, h->core_pid);
    return 0;
}

/* 面板侧定义 g_ptr 并指向**只读**状态映射：vt_util.c 里的 raw_to_logical() 等只读辅助函数
 * 因此可以直接复用，而任何写操作都会被 MMU 拦成 SIGSEGV（只死面板，不动核心）。 */
struct vt_state *g_ptr;

int vt_shm_ring_pop(char *out, size_t cap)
{
    uint32_t rd, tail;
    if (!S_c || !S_b || !out || cap == 0) return 0;
    rd = __atomic_load_n(&S_b->ring_read, __ATOMIC_RELAXED);
    /* v3：tail 是**单调计数**，槽位 = 计数 % VT_RING_SLOTS；空判据就是两个计数相等。
     * acquire：拿到 tail 之后读到的 line 一定是写它那条的完整内容（生产者 release 发布）。 */
    tail = __atomic_load_n(&S_c->tail, __ATOMIC_ACQUIRE);
    if (rd == tail) return 0;
    snprintf(out, cap, "%s", S_c->line[rd % VT_RING_SLOTS]);
    __atomic_store_n(&S_b->ring_read, rd + 1u, __ATOMIC_RELEASE);
    return 1;
}

uint32_t vt_shm_ring_drops(void) { return S_c ? S_c->drops : 0; }

struct vt_shm_header *vt_shm_hdr(void) { return (struct vt_shm_header *)S_base; }

struct vt_state *vt_shm_state(void) { return S_state; }
struct vt_shm_b *vt_shm_b(void)     { return S_b; }
struct vt_shm_c *vt_shm_c(void)     { return S_c; }

int vt_shm_ui_tick(void)
{
    struct vt_shm_header *h;
    static uint32_t last_core_hb;
    static uint64_t hb_at_ms;      /* 上次看到核心心跳变化的**单调毫秒** */
    static int seen;
    uint64_t now;
    if (!S_base) return 0;
    h = (struct vt_shm_header *)S_base;
    h->ui_hb++;
    if (h->panel_pid == 0) h->panel_pid = (int32_t)getpid();
    /* 判据是**单调时间**（3 秒），不是拍数：面板的 poll 线程 5ms 一拍，原来「20 拍」只有 100ms ——
     * 核心把 poll 超时放长之后（事件驱动化：空闲 1s 一轮、心跳 1 次/s）这条会误判「核心死了」，
     * 面板自己退出去。3 秒窗口对两边都够宽（核心心跳 1 次/s，判死要连丢 3 次才有话）。 */
    now = mono_ms();
    if (!seen || h->hb != last_core_hb) { seen = 1; last_core_hb = h->hb; hb_at_ms = now; return 0; }
    if (now - hb_at_ms >= 3000) {
        fprintf(stderr, "vtouch-ui: 核心心跳停滞 3s → 自杀退出\n");
        return -1;
    }
    return 0;
}

int vt_shm_post_edit(const struct vt_shm_edit *e)
{
    if (!S_b) return -1;
    if (shm_lock(S_b) != 0) return -1;                 /* 锁拿不到：不写（调用方按失败处理；评审修复 2026-10-05） */
    memcpy((void *)&S_b->edit, e, sizeof *e);
    S_b->edit.seq = e->seq;
    shm_unlock(S_b);
    return 0;
}

void vt_shm_publish_rect(int visible, int rot, int x1, int y1, int x2, int y2)
{
    if (!S_b) return;
    S_b->rect_seq++;                 /* 奇 = 发布中 */
    __sync_synchronize();
    S_b->panel_visible = visible;
    S_b->rot = rot;
    S_b->rx1 = x1; S_b->ry1 = y1; S_b->rx2 = x2; S_b->ry2 = y2;
    __sync_synchronize();
    S_b->rect_seq++;                 /* 偶 = 稳定 */
}

/* ---- 试查（契约 v9；Task 7.1「试一下」）：面板写请求 / 读结果（核心侧执行见 vt_ops.c 的 vt_ops_test_poll） ---- */

/**
 * (vtouch-doc: vt_shm_ui_test_post)
 * @brief 面板发起一次试查：填参数 → **最后** release 写 test_req_seq（序号 = 当前值 + 1）。
 * @param   kind     0 = 找图 / 1 = 找色单点 / 2 = 找色多点
 * @param   ref      模板名 / 点集名（找色单点不读；按 test_ref 上限截断）
 * @param   region   区域名（空 = 全屏；按 test_region 上限截断）
 * @param   a1       找图 = 阈值 0..255 / 找色 = 模式 0/1
 * @param   a2       找色单点 = (颜色<<8)|容差；其余 0
 * @return  本次请求序号（≥1）；0 = 帧区没附着（没接核心）。
 * @note    内存序照区 D 口径（写端）：参数先写、seq 最后 release 存 —— 核心侧 acquire 读 test_req_seq 后
 *          一定能看到全部参数。单请求在途由调用方（面板 UI）保证（等待期间不重发）。
 */
unsigned vt_shm_ui_test_post(int kind, const char *ref, const char *region, int a1, int a2)
{
    struct vt_shm_frame_hdr *h = vt_shm_frame();
    uint32_t seq;
    size_t n;

    if (!h) return 0;
    h->test_kind = (uint8_t)kind;
    n = ref ? strnlen(ref, sizeof h->test_ref) : 0;      /* 未终止（strnlen 顶到数组尾）→ 截到 15 + NUL */
    if (n >= sizeof h->test_ref) n = sizeof h->test_ref - 1;
    if (n) memcpy(h->test_ref, ref, n);
    h->test_ref[n] = 0;
    n = region ? strnlen(region, sizeof h->test_region) : 0;
    if (n >= sizeof h->test_region) n = sizeof h->test_region - 1;
    if (n) memcpy(h->test_region, region, n);
    h->test_region[n] = 0;
    h->test_a1 = a1;
    h->test_a2 = a2;
    seq = __atomic_load_n(&h->test_req_seq, __ATOMIC_RELAXED) + 1u;
    if (seq == 0) seq = 1u;                              /* 防御：uint32 回绕（到不了）不留 0 哨兵 */
    __atomic_store_n(&h->test_req_seq, seq, __ATOMIC_RELEASE);   /* 参数先写、seq 最后 release（契约） */
    return seq;
}

/**
 * (vtouch-doc: vt_shm_ui_test_take)
 * @brief 面板取一次试查结果（acquire 读 test_res_seq vs 本地 seen；有新结果返回 1）。
 * @param   seq      输出：结果序号（= 核心已应答的请求序号；调用方与自己的在途序号比对认领；可 NULL）
 * @param   x,y      输出：命中点**竖屏逻辑坐标**（仅命中有效；可 NULL）
 * @param   err      输出：0 = 命中 / -1 = 未命中 / 其余 = 错误码（VT_TEST_ERR_*；可 NULL）
 * @return  1 有新结果（本次取走）；0 没有。
 * @note    同一次结果只回报一次（内部 seen）；面板重启后 seen 归零，会把当前结果当「新」报一次 ——
 *          调用方（在途序号比对）自行丢弃陈旧结果。读侧以 acquire 读 res_seq（配核心侧 release 写：
 *          读到新 res_seq 必能读到配对 x/y/err）。
 */
int vt_shm_ui_test_take(unsigned *seq, int *x, int *y, int *err)
{
    struct vt_shm_frame_hdr *h = vt_shm_frame();
    static uint32_t seen;
    uint32_t s;

    if (!h) return 0;
    s = __atomic_load_n(&h->test_res_seq, __ATOMIC_ACQUIRE);
    if (s == 0 || s == seen) return 0;                   /* 0 = 还没应答过任何请求 */
    seen = s;
    if (seq) *seq = s;
    if (x) *x = (int)h->test_x;
    if (y) *y = (int)h->test_y;
    if (err) *err = (int)h->test_err;
    return 1;
}

#endif /* VT_UI_PANEL */
#endif /* VT_UI */
