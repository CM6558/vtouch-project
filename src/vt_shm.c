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
    struct vt_shm_header *h;
    uint32_t tail, next;
    if (!S_c || !S_base) return;
    h = (struct vt_shm_header *)S_base;
    if (n >= VT_RING_LINE) n = VT_RING_LINE - 1;
    tail = S_c->tail;
    next = (tail + 1) % VT_RING_SLOTS;
    if (next == S_c->head) {                    /* 环满：丢最旧（面板是观察者，丢它比堵核心好） */
        S_c->head = (S_c->head + 1) % VT_RING_SLOTS;
        S_c->drops++;
    }
    memcpy(S_c->line[tail], s, n);
    S_c->line[tail][n] = 0;
    __sync_synchronize();
    S_c->tail = next;
    (void)h;
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

int vt_shm_attach(int fd)
{
    struct vt_shm_header *h;
    uint32_t total;
    off_t sz;
    void *base;

    sz = lseek(fd, 0, SEEK_END);
    if (sz <= 0) { fprintf(stderr, "vtouch-ui: 共享内存 fd=%d 长度异常 (%ld)\n", fd, (long)sz); return -1; }
    total = (uint32_t)sz;
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
    S_state = (struct vt_state *)((char *)base + h->off_state);
    S_b = (struct vt_shm_b *)((char *)base + h->off_b);
    S_c = (struct vt_shm_c *)((char *)base + h->off_c);
    /* 面板对状态段是**只读**的（这里只保证不写；真正的 MMU 保护由 ui_glue 单独按 PROT_READ 重映射，
     * 但那会把整块拆成多段映射，P2 再收 —— 现在先把"不写"当纪律）。 */
    fprintf(stderr, "vtouch-ui: 共享内存附着成功 total=%u state@%u b@%u c@%u 逻辑=%dx%d core_pid=%d\n",
            total, h->off_state, h->off_b, h->off_c, h->logical_w, h->logical_h, h->core_pid);
    return 0;
}

struct vt_state *vt_shm_state(void) { return S_state; }
struct vt_shm_b *vt_shm_b(void)     { return S_b; }
struct vt_shm_c *vt_shm_c(void)     { return S_c; }

int vt_shm_ui_tick(void)
{
    struct vt_shm_header *h;
    static uint32_t last_core_hb;
    static int stalls;
    if (!S_base) return 0;
    h = (struct vt_shm_header *)S_base;
    h->ui_hb++;
    if (h->panel_pid == 0) h->panel_pid = (int32_t)getpid();
    if (h->hb == last_core_hb) {
        if (++stalls > 20) { fprintf(stderr, "vtouch-ui: 核心心跳停滞 → 自杀退出\n"); return -1; }
    } else {
        last_core_hb = h->hb; stalls = 0;
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
