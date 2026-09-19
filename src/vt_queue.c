/* vt_queue.c（§3+§4 事件队列 / 出站队列） —— 模块地图见 vt_internal.h；私有状态就近放 static，共享状态走 g。 */
#include "vt_internal.h"

/* 出站队列是模块私有的（外面只通过 outq_* 访问）*/
struct out_msg { int len, sent; char buf[OUTQ_MSG]; };
static struct out_msg outq[OUTQ_CAP];
static int outq_head, outq_tail;
static unsigned long outq_dropped;
static pthread_mutex_t outq_lock = PTHREAD_MUTEX_INITIALIZER;
/**
 * (vtouch-doc: queue_drop_log)
 * @brief 队列丢弃的诊断日志：前 3 次每次都打，之后每 100 次打一行。
 * @param   what     队列名/动作标签（如 "事件(丢新)" / "事件(合并旧move)" / "出站"）
 * @param   n        该队列累计丢弃数
 * @note    诊断不占热路径（代价只是一次取模比较）。
 *
 * 为什么这么写（原有注释，逐字保留）：
 *   溢出只在日志里留一行（首次 3 次 + 每 100 次；计数器本身在队列结构里）——
 *   诊断用，不进热路径的代价：一次整数取模比较。
 */
void queue_drop_log(const char *what, unsigned long n)
{
    if (n <= 3 || n % 100 == 0)
        fprintf(stderr, "vtouchd: %s：出/入队满，这是第 %lu 次（注入路径不受影响；丢了什么看标签）\n", what, n);
}
/**
 * (vtouch-doc: vtq_push)
 * @brief 事件入队（单生产者 = 主线程，消费者 = 区域线程）。
 * @param   q        队列
 * @param   ev       事件（按值拷入）
 * @note    永不阻塞、永不失败：队满先合并（队尾同槽 move，再退 8 格找同槽旧 move 原地覆盖），都不行就丢**这一条新的**。生产者**绝不推进 head**（那是消费者一个人的）—— 老实现满态 CAS 推 head「丢最旧」会与消费者抢 head（评审 C13）。丢的只影响事件条数，注入路径不受影响。
 *
 * 为什么这么写（原有注释，逐字保留）：
 *   溢出策略（§4.3）：① 队尾同槽同类的 move 原地合并（丢旧位置不影响增量语义）；
 *   ② 再往后最多看 8 格，同槽旧 move 原地覆盖（边缘事件也可以覆盖它）；③ 都不行 → 丢**这一条新的**。
 *   老写法第 ③ 步是「丢最旧」，靠 CAS 推进 head 腾格子 —— 那条路与消费者抢 head（评审 C13），
 *   会把消费者正在拷的事件覆盖掉、或把 head 写回去，所以我们把它整个去掉了：**head 只有消费者写**。
 *   队列满时丢的必然是 move 或极端拥挤下的边缘事件：正常负载（每帧 1~6 条、队列 64 格）根本到不了这里。
 */
void vtq_push(struct vtq *q, const struct vt_ev *ev)
{
    /* 生产者**只写 tail 与数据格**，head 永远是消费者一个人的（评审 C13：老实现在满态用 CAS 推进
     * head「丢最旧」，与消费者「读了 head、正在拷数据」竞态 —— 拷一半的事件被覆盖、或 head 被写回去，
     * 结果就是丢事件）。满态的新策略：先合并，合并不了就丢**这一条新的**。 */
    unsigned tail = __atomic_load_n(&q->tail, __ATOMIC_RELAXED);
    unsigned head = __atomic_load_n(&q->head, __ATOMIC_ACQUIRE);
    unsigned i;
    if (tail - head >= VTQ_CAP) {
        unsigned last = (tail - 1u) % VTQ_CAP;
        if (ev->action == VT_MOVE && q->buf[last].action == VT_MOVE && q->buf[last].slot == ev->slot) {
            q->buf[last].x = ev->x; q->buf[last].y = ev->y; q->buf[last].ts = ev->ts;
            return;                    /* 最常见的一路：同一根手指连续 move ⇒ 原地合并，一条都不丢 */
        }
        /* 再往后最多看 8 格：找**同槽的旧 move** 原地覆盖（另一根手指的事件与之交错时的常见情形）。
         * down/up 也允许覆盖旧 move —— 边缘事件比一条过期位置值重要得多（保「不丢 up」这条口径）。 */
        for (i = 0; i < 8 && i < VTQ_CAP; i++) {
            unsigned idx = (tail - 1u - i) % VTQ_CAP;
            if (q->buf[idx].action == VT_MOVE && q->buf[idx].slot == ev->slot) {
                q->buf[idx] = *ev;
                queue_drop_log(ev->action == VT_MOVE ? "事件(合并旧move)" : "事件(覆盖旧move)", ++q->drops);
                return;
            }
        }
        queue_drop_log("事件(丢新)", ++q->drops);
        return;
    }
    q->buf[tail % VTQ_CAP] = *ev;
    __atomic_store_n(&q->tail, tail + 1u, __ATOMIC_RELEASE);
}
/**
 * (vtouch-doc: vtq_pop)
 * @brief 事件出队（单消费者 = 区域线程）。
 * @param   q        队列
 * @param   ev       输出事件
 * @return  1 取到；0 队空。
 */
int vtq_pop(struct vtq *q, struct vt_ev *ev)
{
    unsigned head = __atomic_load_n(&q->head, __ATOMIC_RELAXED);
    unsigned tail = __atomic_load_n(&q->tail, __ATOMIC_ACQUIRE);
    if (head == tail) return 0;
    *ev = q->buf[head % VTQ_CAP];
    __atomic_store_n(&q->head, head + 1u, __ATOMIC_RELEASE);
    return 1;
}
/**
 * (vtouch-doc: outq_reset)
 * @brief 清空出站队列（新客户端接入 / 断连时）。
 * @note    残包不许串给下一个客户端（§4.6）。
 */
void outq_reset(void)
{
    pthread_mutex_lock(&outq_lock);
    outq_head = outq_tail = 0;
    pthread_mutex_unlock(&outq_lock);
}
/**
 * (vtouch-doc: outq_pending)
 * @brief 出站队列是否非空（主循环据此决定要不要挂 POLLOUT）。
 * @return  1 有；0 无。
 */
int outq_pending(void)
{
    int r;
    pthread_mutex_lock(&outq_lock);
    r = (outq_head != outq_tail);
    pthread_mutex_unlock(&outq_lock);
    return r;
}
/**
 * (vtouch-doc: outq_push)
 * @brief 把一段**已经成帧的字节**放入出站队列（生产者 = 主线程 / 区域线程，一把短锁只包住一次 memcpy）。
 * @param   p        数据
 * @param   n        长度
 * @note    队满丢最旧；**唯一的例外是正在续写的那条**（sent>0，它的前半截已经进了客户端 socket，扔掉会让后续字节接到错的帧头 ⇒ 解帧错位）：这时改为丢这一条新的。文本请用 outq_push_text，不要直接调这个。
 */
void outq_push(const char *p, size_t n)
{
    int next;
    if (g.client_fd < 0 || n == 0 || n >= (size_t)OUTQ_MSG) return;
    pthread_mutex_lock(&outq_lock);
    next = (outq_tail + 1) % OUTQ_CAP;
    if (next == outq_head) {
        /* 队满 = 丢最旧（保新鲜，有意设计）。**唯一的例外是「正在续写的那条」**（sent > 0）：
         * 它的前半截已经写进客户端 socket 了，扔掉它会让客户端把**下一帧的帧头**当成它的尾巴
         * 接上去 ⇒ 解帧错位 / 假掉线（比丢一条新事件坏得多）。这时改成丢这一条**新的**。
         * 只有队首可能是半写的（outq_flush 只写 head，写完才推进），所以判 head 就够。 */
        if (outq[outq_head].sent > 0) { queue_drop_log("出站（半帧保护）", ++outq_dropped); pthread_mutex_unlock(&outq_lock); return; }
        outq_head = (outq_head + 1) % OUTQ_CAP;
        queue_drop_log("出站", ++outq_dropped);
    }
    outq[outq_tail].len = (int)n;
    outq[outq_tail].sent = 0;
    memcpy(outq[outq_tail].buf, p, n);
    outq_tail = next;
    pthread_mutex_unlock(&outq_lock);
}
/* outq_push 的「不驱逐」版：队列无空位就一格不动地返回 -1（不推进 head、不覆盖旧格）。 */
static int outq_push_keep(const char *p, size_t n)
{
    int next;
    if (g.client_fd < 0 || n == 0 || n >= (size_t)OUTQ_MSG) return -1;
    pthread_mutex_lock(&outq_lock);
    next = (outq_tail + 1) % OUTQ_CAP;
    if (next == outq_head) { pthread_mutex_unlock(&outq_lock); return -1; }   /* 满：这一帧不写，旧数据一格不动 */
    outq[outq_tail].len = (int)n;
    outq[outq_tail].sent = 0;
    memcpy(outq[outq_tail].buf, p, n);
    outq_tail = next;
    pthread_mutex_unlock(&outq_lock);
    return 0;
}
/* 两个文本推入接口共用的「成帧 + 入队」核：成帧只写一份，只把「满队列怎么办」参数化。
   满时策略：evict=1 丢最旧（事件帧的有意设计，见 outq_push_text）；evict=0 不写这一帧（见 outq_push_text_keep）。 */
static int outq_push_text_core(const char *s, size_t n, int evict)
{
    char buf[OUTQ_MSG];
    size_t hl;
    if (!s || n == 0) return -1;
    buf[0] = (char)0x81;                       /* FIN + opcode=1（text） */
    if (n < 126) { buf[1] = (char)n; hl = 2; }
    else { buf[1] = 126; buf[2] = (char)((n >> 8) & 0xff); buf[3] = (char)(n & 0xff); hl = 4; }
    if (hl + n >= sizeof buf) { fprintf(stderr, "vtouchd: 出站帧过长 %zu 字节 → 丢弃\n", n); return -1; }
    memcpy(buf + hl, s, n);
    if (evict) { outq_push(buf, hl + n); return 0; }
    return outq_push_keep(buf, hl + n);
}
/**
 * (vtouch-doc: outq_push_text)
 * @brief 把一行文本按 WS 文本帧（未加掩码）补齐帧头后入队。
 * @param   s        文本
 * @param   n        长度
 * @note    成帧必须在这一层做 —— 队列里存的就是完整帧，刷出端只负责写字节。漏了这步的症状：客户端收到裸文本、一帧都解不出来，而注入照常生效（所以最难发现）。
 *
 * 为什么这么写（原有注释，逐字保留）：
 *   服务端 → 客户端：**文本帧**（未加掩码）。成帧必须发生在入队这里 ——
 *   队列里存的就是「完整的 WS 帧」，刷出端只负责把字节写出去（它支持半包续写）。
 *   少了这一步的症状：客户端收到裸文本，一帧都解不出来（响应 / region_ev 全哑），
 *   而注入本身照常生效 —— 所以很容易漏掉（§4.5 改队列时就是这么漏的）。
 */
void outq_push_text(const char *s, size_t n)
{
    (void)outq_push_text_core(s, n, 1);        /* 行为与改动前逐字一致：满时丢最旧（保新鲜的有意设计） */
}
/**
 * (vtouch-doc: outq_push_text_keep)
 * @brief 把一行文本按 WS 文本帧（未加掩码）补齐帧头后入队；**队满就不写这一帧**（丢新、不丢旧）。
 * @param   s        文本
 * @param   n        长度
 * @return  0 已入队；-1 没写进去（队满 / 无客户端 / 空串 / 单帧超长）。
 * @note    成帧与 outq_push_text 共用同一份实现，只有「满时策略」不同；调用方据此数被丢的行数并自己告警。
 */
int outq_push_text_keep(const char *s, size_t n)
{
    /* 为什么要有这个接口（而不是直接用 outq_push_text）：
     *   事件帧满时丢最旧是**有意**设计（保新鲜：旧事件比新事件没用）。但一张带自证末行的表不能这么丢：
     *   丢最旧会挤掉队列里**已经排着**的数据（很可能是客户端还没读走的事件帧），而末行 end N 照发
     *   ⇒ 客户端拿到「少几行、却自称 N 条」的半张表，而且被挤掉的事件帧还是静默丢的（I2 的另一半）。
     *   这里改成「队满就不写这一帧」：表可能不完整，但 ① 客户端不会静默接受半张表 —— 它按行数与
     *   末行 end N 对账，末行自己也被丢时走它既有的「没见过末行 → 回包不完整」那条老判据，
     *   ② 这次推表一条已排队的事件帧都不会挤掉。 */
    return outq_push_text_core(s, n, 0);
}
/**
 * (vtouch-doc: outq_flush)
 * @brief 主循环唯一的刷出点：socket 可写才写，写不完留到下次；真错（EPIPE/ECONNRESET）才踢客户端。
 * @note    客户端 socket 是非阻塞的，这里绝不阻塞主线程。
 *
 * 为什么这么写（原有注释，逐字保留）：
 *   主循环专用：socket 可写才写（客户端 socket 握手后设为非阻塞，所以这里绝不阻塞主线程）。
 *   写不完留着，下一次 POLLOUT 继续；真错（EPIPE/ECONNRESET）才踢客户端。
 */
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
