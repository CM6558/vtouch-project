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

/* 单槽邮箱的自旋锁：只在「面板投一次编辑 / 核心吃一次编辑」时拿，不在注入热路径上。 */
static void shm_lock(struct vt_shm_b *b)
{
    int spins = 0;
    while (__sync_lock_test_and_set(&b->lock, 1)) {
        if (++spins > 2000000) break;          /* 极端情况不要死等，宁可丢这次编辑 */
    }
}
static void shm_unlock(struct vt_shm_b *b) { __sync_lock_release(&b->lock); }

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
    uint32_t total = off_c + size_c;
    struct vt_shm_header *h;
    void *base;
    int fd;

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
    h->logical_w = g.logical_width; h->logical_h = g.logical_height;
    h->core_pid = (int32_t)getpid();

    S_state = (struct vt_state *)((char *)base + off_state);
    S_b = (struct vt_shm_b *)((char *)base + off_b);
    S_c = (struct vt_shm_c *)((char *)base + off_c);
    S_base = base;

    /* 状态本身进共享内存：把当前状态（= 初值 + apply_args 已解析出的参数）拷进去，
     * 再把 g 指过去 —— 此后全库 200+ 处调用点一行不改。 */
    memcpy(S_state, g_ptr, sizeof(struct vt_state));
    g_ptr = S_state;

    fprintf(stderr, "vtouchd: 共享内存就绪 fd=%d total=%u state@%u(%u) b@%u c@%u\n",
            fd, total, off_state, size_state, off_b, off_c);
    return fd;                                  /* 返回 fd：fork 时原样传给面板子进程 */
}

void vt_shm_tick(void)
{
    if (S_c) ((struct vt_shm_header *)((char *)S_base))->hb++;
}

uint32_t vt_shm_ui_hb(void)
{
    struct vt_shm_header *h;
    if (!S_base) return 0;
    h = (struct vt_shm_header *)S_base;
    return h->ui_hb;
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
    shm_lock(S_b);
    memcpy(&e, (const void *)&S_b->edit, sizeof e);
    op = (int)e.op;
    switch (op) {
    case VT_EDIT_CLEAR:  regions_clear(); break;
    case VT_EDIT_ADD:    region_add(e.id, (int)e.type, (int)e.a1, (int)e.a2, (int)e.a3, (int)e.a4, (int)e.enabled); break;
    case VT_EDIT_DEL:    region_del(e.id); break;
    case VT_EDIT_RENAME: region_rename(e.id, e.new_id); break;
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
    if (h->off_state + h->size_state > total || h->off_b + h->size_b > total || h->off_c + h->size_c > total) {
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
    fprintf(stderr, "vtouch-ui: 共享内存附着成功 total=%u state@%u b@%u c@%u 逻辑=%dx%d core_pid=%d\n",
            total, h->off_state, h->off_b, h->off_c, h->logical_w, h->logical_h, h->core_pid);
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

void vt_shm_post_edit(const struct vt_shm_edit *e)
{
    if (!S_b) return;
    shm_lock(S_b);
    memcpy((void *)&S_b->edit, e, sizeof *e);
    S_b->edit.seq = e->seq;
    shm_unlock(S_b);
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

#endif /* VT_UI_PANEL */
#endif /* VT_UI */
