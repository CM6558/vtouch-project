/* vt_queue.c（§3+§4 事件队列 / 出站队列） —— 模块地图见 vt_internal.h；私有状态就近放 static，共享状态走 g。 */
#include "vt_internal.h"

/* 出站队列是模块私有的（外面只通过 outq_* 访问）*/
struct out_msg { int len, sent; char buf[OUTQ_MSG]; };
static struct out_msg outq[OUTQ_CAP];
static int outq_head, outq_tail;
static unsigned long outq_dropped;
static pthread_mutex_t outq_lock = PTHREAD_MUTEX_INITIALIZER;

/* 溢出只在日志里留一行（首次 3 次 + 每 100 次；计数器本身在队列结构里）——
 * 诊断用，不进热路径的代价：一次整数取模比较。 */
void queue_drop_log(const char *what, unsigned long n)
{
    if (n <= 3 || n % 100 == 0)
        fprintf(stderr, "vtouchd: %s队列满，丢弃第 %lu 条（§4.3/§4.5 丢最旧，注入路径不受影响）\n", what, n);
}

/* 溢出策略（§4.3）：① 队尾同槽同类的 move 原地合并（丢旧位置不影响增量语义）；
 * ② 仍然满 → 丢最旧（CAS 推 head，因为消费者也在推它）。
 * 队列满时丢的必然是 move：同时按下的物理槽 ≤ g.phys_slots，down/up 事件在手指抬起前
 * 每槽只会出现一次，不可能把 64 格塞满；而且就算真丢，注入路径也照常（只是事件少一条）。 */
void vtq_push(struct vtq *q, const struct vt_ev *ev)
{
    unsigned tail = __atomic_load_n(&q->tail, __ATOMIC_RELAXED);
    unsigned head = __atomic_load_n(&q->head, __ATOMIC_ACQUIRE);
    unsigned i;
    if (tail - head >= VTQ_CAP) {
        struct vt_ev *last = &q->buf[(tail - 1u) % VTQ_CAP];
        if (ev->action == VT_MOVE && last->action == VT_MOVE &&
            last->slot == ev->slot && last->virt == ev->virt) {
            last->x = ev->x; last->y = ev->y; last->ts = ev->ts;
            return;
        }
        for (i = 0; i < 4; i++) {
            if (__atomic_compare_exchange_n(&q->head, &head, head + 1u, 0,
                                            __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) { queue_drop_log("事件", ++q->drops); break; }
            if (__atomic_load_n(&q->tail, __ATOMIC_RELAXED) - head < VTQ_CAP) break;   /* 消费者已腾出格子 */
        }
        if (__atomic_load_n(&q->tail, __ATOMIC_RELAXED) - head >= VTQ_CAP) { queue_drop_log("事件", ++q->drops); return; }
    }
    q->buf[tail % VTQ_CAP] = *ev;
    __atomic_store_n(&q->tail, tail + 1u, __ATOMIC_RELEASE);
}

int vtq_pop(struct vtq *q, struct vt_ev *ev)
{
    unsigned head = __atomic_load_n(&q->head, __ATOMIC_RELAXED);
    unsigned tail = __atomic_load_n(&q->tail, __ATOMIC_ACQUIRE);
    if (head == tail) return 0;
    *ev = q->buf[head % VTQ_CAP];
    __atomic_store_n(&q->head, head + 1u, __ATOMIC_RELEASE);
    return 1;
}

void outq_reset(void)
{
    pthread_mutex_lock(&outq_lock);
    outq_head = outq_tail = 0;
    pthread_mutex_unlock(&outq_lock);
}

int outq_pending(void)
{
    int r;
    pthread_mutex_lock(&outq_lock);
    r = (outq_head != outq_tail);
    pthread_mutex_unlock(&outq_lock);
    return r;
}

void outq_push(const char *p, size_t n)
{
    int next;
    if (g.client_fd < 0 || n == 0 || n >= (size_t)OUTQ_MSG) return;
    pthread_mutex_lock(&outq_lock);
    next = (outq_tail + 1) % OUTQ_CAP;
    if (next == outq_head) { outq_head = (outq_head + 1) % OUTQ_CAP; queue_drop_log("出站", ++outq_dropped); }
    outq[outq_tail].len = (int)n;
    outq[outq_tail].sent = 0;
    memcpy(outq[outq_tail].buf, p, n);
    outq_tail = next;
    pthread_mutex_unlock(&outq_lock);
}

/* 服务端 → 客户端：**文本帧**（未加掩码）。成帧必须发生在入队这里 ——
 * 队列里存的就是「完整的 WS 帧」，刷出端只负责把字节写出去（它支持半包续写）。
 * 少了这一步的症状：客户端收到裸文本，一帧都解不出来（响应 / pev / region_ev 全哑），
 * 而注入本身照常生效 —— 所以很容易漏掉（§4.5 改队列时就是这么漏的）。 */
void outq_push_text(const char *s, size_t n)
{
    char buf[OUTQ_MSG];
    size_t hl;
    if (!s || n == 0) return;
    buf[0] = (char)0x81;                       /* FIN + opcode=1（text） */
    if (n < 126) { buf[1] = (char)n; hl = 2; }
    else { buf[1] = 126; buf[2] = (char)((n >> 8) & 0xff); buf[3] = (char)(n & 0xff); hl = 4; }
    if (hl + n >= sizeof buf) { fprintf(stderr, "vtouchd: 出站帧过长 %zu 字节 → 丢弃\n", n); return; }
    memcpy(buf + hl, s, n);
    outq_push(buf, hl + n);
}

/* 主循环专用：socket 可写才写（客户端 socket 握手后设为非阻塞，所以这里绝不阻塞主线程）。
 * 写不完留着，下一次 POLLOUT 继续；真错（EPIPE/ECONNRESET）才踢客户端。 */
void outq_flush(void)
{
    for (;;) {
        struct out_msg *m;
        ssize_t n;
        pthread_mutex_lock(&outq_lock);
        if (g.client_fd < 0 || outq_head == outq_tail) { pthread_mutex_unlock(&outq_lock); return; }
        m = &outq[outq_head];
        n = send(g.client_fd, m->buf + m->sent, (size_t)(m->len - m->sent), MSG_NOSIGNAL);
        if (n > 0) {
            m->sent += (int)n;
            if (m->sent >= m->len) outq_head = (outq_head + 1) % OUTQ_CAP;
        }
        pthread_mutex_unlock(&outq_lock);
        if (n <= 0) {
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return;
            drop_client();
            return;
        }
    }
}
