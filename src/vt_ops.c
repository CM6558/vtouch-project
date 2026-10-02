/* vt_ops.c —— 操作表落地（put/del/clear 三个编辑入口 + 核心单点校验）+ 执行器
 * （状态机 / 到点推进 / 触发槽 / 中止）（VT_UI 构建才参与编译）。
 *
 * 职责边界（与 docs/OPS_PLAN.md §2.7 / §3 一致）：
 *   · 操作表的唯一真相是区 A 里的 g.ops[] / g.op_count（面板只读直读，不自己改表）；
 *   · 面板的编辑走区 B 邮箱（VT_EDIT_OP_*）→ vt_shm_edit_apply → 本文件的入口；
 *   · **核心单点校验**：名字用与区域 id 同一把尺子（vt_id_ok），步数/类型/坐标/时长逐条过门 ——
 *     面板侧预检只做提示用，能不能落表由这里说了算。校验不过一律拒绝 + 日志 `op 被拒 <名>: <原因>`。
 *   · 执行器：起跑 = 整条快照（运行中改表 / 删表影响不到本次，spec §7）；一次只跑一条；
 *     状态机用单调钟绝对时间表；手指走 virt[]（身份段 = phys_slots + 槽号），与 WS 客户端
 *     同槽时按「不避让」记 `op 槽冲突`（spec §3.5）；到点由 poll 的第四档唤醒（不新起线程）。
 *   · 避让客户端帧事务（R1 + R2a 封口）：起跑遇帧窗丢弃（`op 丢弃 帧内`）；运行中遇帧窗**冻结**
 *     （vt_ops_tick 不推进 / 不自检 / 不消费触发），帧关后把 t0/deadline/t_start 一起平移暂停时长；
 *     中止（面板 STOP / 收尾）遇帧窗**推迟**：原因记账进 R.stop_why，帧关后的第一次 tick 在解冻点
 *     补执行 —— 执行器任何路径都不在帧窗内写 g.virt（R2a 封口 / L8）；
 *     冻结期 vt_ops_next_deadline_ms 返回 -1、不参与第四档（帧关靠 WS fd 活动唤醒）。
 *   · 操作表与执行器状态都只在主线程读写（编辑邮箱在 vtouch_poll_step 里吃、执行器也在主线程跑）——
 *     它们自己不需要锁；但门控检查（起跑）与自动关（完成）要读改**区域表**（gate 的存在性 /
 *     kind / toggle_on）—— 那部分照区域线程同款锁纪律：持 g.region_lock 判定 / 改值，锁外
 *     环行 + 日志（锁内不 I/O）。
 *
 * 依赖：包含 vt_internal.h 之后，g 就是共享内存里的那份状态（见 vt_internal.h 的 g_ptr 宏）。
 * 守卫：整文件在 VT_UI 分支内 —— 默认构建里本文件编成空 TU（build.sh 用通配把 src 下的 .c 一起链）。
 */
#include "vt_internal.h"
#ifdef VT_UI

#include <sys/eventfd.h>     /* 执行器的触发唤醒 fd（eventfd：写一下就叫醒主循环） */

/* 操作载荷校验（核心单点）：名字 / 步数 / 每步的类型、坐标与时长逐条过门；
 * 不过就把一句人话写进 why（调用方拼成 `op 被拒 <名>: <原因>` 日志）。
 *
 * 规则出处（spec §2.7）：名字与区域 id 同一把尺子（vt_id_ok：[A-Za-z0-9_-]、1..15）；
 * 步数 1..MAX_STEPS；坐标必须落在竖屏逻辑坐标内（0..logical_width-1 / 0..logical_height-1）——
 * 操作的手指是**注入**的，屏外的点没有意义：收下来也只是静默不命中，不如当场拒掉让面板报错；
 * 时长按类型分档：点按 0..60000（0 = 按下即抬）、滑动 1..60000（0 的滑动没有采样点）、等待 0..600000。
 * gate **不在这里校验**：允许悬空 —— 起跑时解析不到就丢弃 + 日志（安全侧，见 spec §4.3）。
 */
static int op_valid(const struct vt_op *op, char *why, size_t whycap)
{
    int i;
    size_t n;

    /* 名字来自邮箱载荷，先按数组长度找终止符：未终止（strnlen 顶到 name[] 尾）按非法拒 ——
     * 不让后面的 vt_id_ok / strcmp / 日志去读越界。 */
    n = strnlen(op->name, sizeof op->name);
    if (n < 1 || n > OP_NAME_MAX || !vt_id_ok(op->name, n)) {
        snprintf(why, whycap, "名字非法（[A-Za-z0-9_-]、1..%d）", OP_NAME_MAX);
        return 0;
    }
    if (op->step_count < 1 || op->step_count > MAX_STEPS) {
        snprintf(why, whycap, "步数 %d 不在 1..%d", op->step_count, MAX_STEPS);
        return 0;
    }
    for (i = 0; i < op->step_count; i++) {
        const struct vt_step *st = &op->steps[i];
        switch (st->type) {
        case OP_STEP_TAP:
            if (st->a1 < 0 || st->a1 >= g.logical_width ||
                st->a2 < 0 || st->a2 >= g.logical_height) {
                snprintf(why, whycap, "第 %d 步坐标越界", i + 1);
                return 0;
            }
            if (st->ms < 0 || st->ms > 60000) {
                snprintf(why, whycap, "第 %d 步时长越界", i + 1);
                return 0;
            }
            break;
        case OP_STEP_SWIPE:
            if (st->a1 < 0 || st->a1 >= g.logical_width ||
                st->a2 < 0 || st->a2 >= g.logical_height ||
                st->a3 < 0 || st->a3 >= g.logical_width ||
                st->a4 < 0 || st->a4 >= g.logical_height) {
                snprintf(why, whycap, "第 %d 步坐标越界", i + 1);
                return 0;
            }
            if (st->ms < 1 || st->ms > 60000) {
                snprintf(why, whycap, "第 %d 步时长越界", i + 1);
                return 0;
            }
            break;
        case OP_STEP_WAIT:
            if (st->ms < 0 || st->ms > 600000) {
                snprintf(why, whycap, "第 %d 步时长越界", i + 1);
                return 0;
            }
            break;
        default:
            snprintf(why, whycap, "第 %d 步类型非法", i + 1);
            return 0;
        }
    }
    return 1;
}

/* 拒绝日志：名字可能为空或未终止（载荷直接来自邮箱字节），统一按上限截断打印，绝不越界读。 */
static void op_reject_log(const char *name, const char *why)
{
    char buf[OP_NAME_MAX + 2];
    size_t n = name ? strnlen(name, OP_NAME_MAX + 1) : 0;

    if (n == 0) {
        snprintf(buf, sizeof buf, "(空)");
    } else {
        memcpy(buf, name, n);
        buf[n] = 0;
    }
    fprintf(stderr, "vtouchd: op 被拒 %s: %s\n", buf, why);
}

/**
 * (vtouch-doc: vt_ops_put)
 * @brief 新增或覆盖一条操作（重名覆盖；核心单点校验，不过拒绝）。
 * @param   op       整条操作载荷（名字 + 步数 + 步表）
 * @return  0 成功；-1 参数为空、校验不过或表满（表满只发生在新增）。
 * @note    校验全在核心这一处（与区域 id 同一把尺子）：名字 vt_id_ok（[A-Za-z0-9_-]、1..15；裸 `-` 除外）、步数 1..MAX_STEPS、类型 ∈ {点按,滑动,等待}、坐标 0..逻辑尺寸-1、时长按类型分档（点按 0..60000 / 滑动 1..60000 / 等待 0..600000）。拒绝打 `op 被拒 <名>: <原因>`、成功打 `op 编辑 put <名> 步数=N`；重名覆盖就地写（表位不变），要么整条生效、要么一点都不动。
 *
 * 为什么这么写（原有注释，逐字保留）：
 *   先整条校验、通过才落表：要么全收、要么一点都不动 —— 半条脏操作比拒绝更糟（面板回读只认
 *   「同名 + 步数一致」，半收会让回读看起来成功、实际不对）。
 *   覆盖 = 找到同名下标**原地整条写**，而不是删旧加新：表位稳定 ⇒ 运行中的操作继续按自己的快照跑
 *   （spec §7），面板卡片顺序也不会因为一次同名编辑而错位。表满只挡新增（覆盖不占新位）。
 */
int vt_ops_put(const struct vt_op *op)
{
    char why[64];
    struct vt_op *dst = NULL;
    int i;

    if (!op) return -1;
    if (!op_valid(op, why, sizeof why)) {
        op_reject_log(op->name, why);
        return -1;
    }
    for (i = 0; i < g.op_count; i++) {
        if (strcmp(g.ops[i].name, op->name) == 0) { dst = &g.ops[i]; break; }
    }
    if (!dst) {
        if (g.op_count >= MAX_OPS) {
            op_reject_log(op->name, "表满");
            return -1;
        }
        dst = &g.ops[g.op_count++];
    }
    *dst = *op;                                  /* 整条按值覆盖：每个字段都来自载荷，没有残留 */
    fprintf(stderr, "vtouchd: op 编辑 put %s 步数=%d\n", dst->name, dst->step_count);
    return 0;
}

/**
 * (vtouch-doc: vt_ops_del)
 * @brief 删除一条操作。
 * @param   name     操作名
 * @return  0 成功；-1 名字非法或表里没有同名条目。
 * @note    后面的条目整体前移一格（与 region_del 同套路，表尾清零）；只动表 —— 运行中的操作按自己的快照跑完，怎么收场是执行器侧的事。
 *
 * 为什么这么写（原有注释，逐字保留）：
 *   先校验名字、再找条目（与 region_del 同一道门）：非法名字直接拒 —— 两种拒绝在日志里分得清
 *   （`名字非法` vs `不存在`），面板回读也看得明白。
 *   只动表本身：运行中的操作按自己的快照跑完（spec §7），"运行中正被删掉"怎么收场是执行器
 *   侧的行为，不在这层掺和。
 */
int vt_ops_del(const char *name)
{
    size_t n;
    int i, k;

    if (!name) return -1;
    n = strnlen(name, OP_NAME_MAX + 1);
    if (n < 1 || n > OP_NAME_MAX || !vt_id_ok(name, n)) {
        op_reject_log(name, "名字非法");
        return -1;
    }
    for (i = 0; i < g.op_count; i++) {
        if (strcmp(g.ops[i].name, name) != 0) continue;
        for (k = i; k + 1 < g.op_count; k++) g.ops[k] = g.ops[k + 1];
        g.op_count--;
        memset(&g.ops[g.op_count], 0, sizeof g.ops[0]);
        fprintf(stderr, "vtouchd: op 编辑 del %s\n", name);
        return 0;
    }
    op_reject_log(name, "不存在");
    return -1;
}

/**
 * (vtouch-doc: vt_ops_clear)
 * @brief 清空操作表。
 * @note    整表归零（不只是 op_count=0），不留"幽灵操作"；只动表 —— 运行中的操作按自己的快照跑完。
 *
 * 为什么这么写（原有注释，逐字保留）：
 *   归零整表而不是只写 op_count = 0：表尾留着旧名字的话，将来若有人按 0..MAX_OPS 扫全表
 *   （而不是 0..op_count）就会读到"幽灵操作"。一次 memset 的代价，换掉一整类脏读。
 */
void vt_ops_clear(void)
{
    g.op_count = 0;
    memset(g.ops, 0, sizeof g.ops);
    fprintf(stderr, "vtouchd: op 编辑 clear\n");
}

/* ===================== 执行器（状态机 / 到点推进 / 触发槽 / 中止） ===================== */

/* 运行中操作的**快照**（私有 static，不进共享内存）：起跑时整条抄下来，此后表怎么改、怎么删都不影响
 * 本次运行（spec §7）。字段与用途：
 *   active  1 = 有一条操作在跑；idx = 起跑时的表下标（回显 g.op_run）
 *   name    快照名（表被删了日志也得有名字）；nsteps/steps[] = 快照步表
 *   gate/auto_off  门控字段：起跑快照带入；起跑门控检查与跑完自动关用它（批 3 落地，spec §4.3）
 *   step    当前步下标（0 起）；t0 = 本步名义开始时刻；deadline = 下一次动作的到点（单调毫秒）
 *   phase   步内阶段（PH_*）；slot = 全程占用的虚拟槽
 *   hold/dur/nsamp/sample  点按按住时长 / 滑动时长、采样点数、下一个采样点下标
 *   rx/ry   上一次写进 g.virt[slot] 的 raw 坐标（撞槽检测：别人动过它就知道）
 *   conflict  撞槽日志只打一次（v1 不做避让，spec §3.5）
 *   trig_seen  已消费到的触发序号（触发槽 SPSC 的消费者一侧）
 *   t_start   起跑时刻（完成日志的「用时」）
 *   frozen / frozen_since  帧冻结标记与冻结起点（帧窗内暂停推进；帧关后把 t0/deadline/t_start
 *              一起平移暂停时长 —— 见 vt_ops_tick）；起跑 / 完成 / 中止都清零，不泄漏到下一次运行
 *   stop_pending / stop_why  帧窗内被推迟的中止（R2a 封口 / L8）：原因快照进 stop_why（短缓冲、
 *              安全截断，不存裸指针）；帧关后的第一次 tick 在解冻点补执行（见 vt_ops_tick）；
 *              起跑 / 完成 / 中止正常路径三处清零，不泄漏到下一次运行
 * 全部只有主线程碰（编辑邮箱在 poll_step 里吃、执行器也在主线程跑）—— 不需要锁。
 */
static struct {
    int      active;
    int      idx;
    char     name[OP_NAME_MAX + 1];
    int      nsteps;
    struct vt_step steps[MAX_STEPS];
    char     gate[REGION_ID_MAX + 1];
    int      auto_off;
    int      step;
    uint64_t t0;
    uint64_t deadline;
    int      phase;
    int      slot;
    int      hold;
    int      dur;
    int      nsamp;
    int      sample;
    int      rx, ry;
    int      conflict;
    uint32_t trig_seen;
    uint64_t t_start;
    int      frozen;                                         /* 帧冻结中（R1 封口）：帧窗内暂停推进 */
    uint64_t frozen_since;                                   /* 冻结起点（单调毫秒；仅 frozen 时有意义） */
    int      stop_pending;                                   /* 帧窗内被推迟的中止（R2a 封口）：帧关后补执行；三处清零 */
    char     stop_why[32];                                   /* 推迟中止的原因快照（短缓冲、安全截断；不存裸指针） */
} R;

/* 步内阶段：BEGIN=本步的起始动作还没发；TAP_UP=等 hold 到点抬指；SWIPE_MOVE=等下一个采样点；
 * SWIPE_UP=采样发完等终点抬指；WAIT=等到点直接进下一步。 */
enum { PH_BEGIN = 0, PH_TAP_UP, PH_SWIPE_MOVE, PH_SWIPE_UP, PH_WAIT };

/* 单调毫秒（执行器的时间全走它：不累加拍数、不混墙钟，到点判据不漂）。 */
static uint64_t op_now_ms(void) { return now_ns() / 1000000ull; }

/* 高频明细开关（L9 / spec §8 第 10 条）：滑动每采样点一行默认**不**打，env `VTOUCH_OPS_TRACE=1`
 * 才开。env 读一次存静态 —— 之后只是一次分支判断，不碰环境、不影响时序；默认零输出。
 * 首次求值发现开着时打一行提示：测试阶段一眼确认开关生效。 */
static int op_trace_on(void)
{
    static int on = -1;                                      /* -1 = env 还没读过 */
    if (on < 0) {
        const char *v = getenv("VTOUCH_OPS_TRACE");
        on = (v && strcmp(v, "1") == 0) ? 1 : 0;
        if (on) fprintf(stderr, "vtouchd: op trace 开（VTOUCH_OPS_TRACE=1）\n");
    }
    return on;
}

/* 往共享内存事件环推一行（面板日志 + 立即重画；与 mark_ev 同源 —— 都是主线程）。 */
static void op_ring_line(const char *s)
{
    size_t n = strlen(s);
    if (n) vt_shm_ring_push(s, n);
}

/* op_ev 环行：`op_ev <名> <run|done|abort> <i>/<N>`（spec §3.7；i 从 1 起算）。 */
static void op_ev_push(const char *what, int i, int n)
{
    char line[VT_RING_LINE];
    int k = snprintf(line, sizeof line, "op_ev %s %s %d/%d\n", R.name, what, i, n);
    if (k > 0 && (size_t)k < sizeof line) vt_shm_ring_push(line, (size_t)k);
}

/* 丢弃日志统一出口：`op 丢弃 <原因>[ <名>]`。名字可能未终止（邮箱 / 触发槽的裸字节），
 * 一律按上限截断打印，绝不越界读。 */
static void op_drop(const char *reason, const char *name)
{
    char nm[OP_NAME_MAX + 2];
    size_t n = name ? strnlen(name, OP_NAME_MAX + 1) : 0;
    if (n > OP_NAME_MAX) n = OP_NAME_MAX;
    if (n) { memcpy(nm, name, n); nm[n] = 0; }
    if (n) fprintf(stderr, "vtouchd: op 丢弃 %s %s\n", reason, nm);
    else   fprintf(stderr, "vtouchd: op 丢弃 %s\n", reason);
}

/* 忙丢弃：面板按了「运行」却跑不了，环行也要推一份 —— 面板日志里得有个回音（spec §4.4）。 */
static void op_drop_busy(void)
{
    fprintf(stderr, "vtouchd: op 丢弃 忙\n");
    op_ring_line("op 丢弃 忙\n");
}

/* 帧内丢弃：客户端帧事务（begin_frame..end_frame）开着时不能起跑操作。
 * 机制：起跑会直接写 g.virt 并 emit，而 end_frame 的 memcpy(g.virt, g.staged) 会按 begin_frame 时的
 * 快照把「当时为空」的槽抹回空 ⇒ 设备侧永远拿不到 tracking_id=-1 的收尾（悬空触点）；
 * abort 的抬指也因为 down 已被抹掉而发不出去。全库不变式：直写 g.virt 的写方必须避让帧事务
 * （先例 vt_ws.c 的 up/down/move 命令门；粘触点由 vt_frame.c 的 owner_reset 治）。
 * 日志形态与「忙」一致（stderr + 环行）。 */
static void op_drop_frame(void)
{
    fprintf(stderr, "vtouchd: op 丢弃 帧内\n");
    op_ring_line("op 丢弃 帧内\n");
}

/* 现在的阶段手指按着没有（撞槽自检 / 中止抬指都看它）。 */
static int op_finger_down(void)
{
    return R.active && (R.phase == PH_TAP_UP || R.phase == PH_SWIPE_MOVE || R.phase == PH_SWIPE_UP);
}

/* 改操作的手指状态（raw 坐标）并提交一帧。返回 0 正常；-1 = set_virtual 拒绝
 * （手指被外力抬了 / 占了 —— 调用方按撞槽收场）。写失败照旧置 g_reemit 等主循环重发。
 * 注意 set_virtual 的约定：第一个参数是**数组基址**、第二个才是槽号（全库调用点都这么传）。 */
static int op_finger_raw(const char *act, int rx, int ry)
{
    if (set_virtual(g.virt, R.slot, act, rx, ry) != 0) return -1;
    R.rx = rx; R.ry = ry;
    if (emit_frame() < 0) g.g_reemit = 1;
    return 0;
}

/* 逻辑坐标版（down / 滑动采样点用；up 原地抬，走 op_finger_raw）。 */
static int op_finger_ll(const char *act, int lx, int ly)
{
    int rx, ry;
    if (logical_to_raw(lx, 0, &rx) != 0 || logical_to_raw(ly, 1, &ry) != 0) return -1;
    return op_finger_raw(act, rx, ry);
}

/* 线性插值：滑动的第 k 个采样点（k=0..n；k=n 时精确落在终点）。 */
static int op_lerp(int a, int b, int k, int n) { return a + (b - a) * k / n; }

/* 撞槽收场：记一行 `op 槽冲突 k`（只记一次）+ 顺带中止。原因只能记到「reset/断连」这一层：
 * 能外力动我们手指的只有 WS 客户端侧（reset 命令 / 断连被踢抬掉全部虚拟触点 / 对同槽注入 up），
 * 执行器侧分不出是哪一个。 */
static void op_conflict_abort(void)
{
    if (!R.conflict) {
        R.conflict = 1;
        fprintf(stderr, "vtouchd: op 槽冲突 %d\n", R.slot);
    }
    vt_ops_abort("reset/断连");
}

/* 自动关（spec §4.3；批 3）：正常完成（op_finish）且快照 auto_off=1 时把门控开关翻回关。
 * 门控 id 取起跑快照 R.gate（运行中改表 / 删表影响不到本次，spec §7）；找不到区域 / 区域不是
 * 开关型 → 跳过（悬空引用允许 —— 同起跑门控的安全侧口径；不投递、不改值）。
 * 锁纪律与 T3.1 §4.2 翻转同款：持 region_lock 改 toggle_on，解锁后再投递
 * （toggle_ev 环行 + 日志）—— 锁内改值、锁外环行/日志。 */
static void op_auto_off(void)
{
    char id[REGION_ID_MAX + 1];
    int i, hit = 0;

    if (!R.auto_off || !R.gate[0]) return;                   /* 没开自动关 / 没门控：无事可做 */
    pthread_mutex_lock(&g.region_lock);
    for (i = 0; i < g.region_count; i++) {
        if (strcmp(g.regions[i].id, R.gate) != 0) continue;
        if (g.regions[i].kind == 1) {                        /* 只有开关型有 toggle_on 可翻 */
            g.regions[i].toggle_on = 0;
            memcpy(id, g.regions[i].id, sizeof id);
            hit = 1;
        }
        break;
    }
    pthread_mutex_unlock(&g.region_lock);
    if (!hit) return;                                        /* 未找到 / 非开关型：跳过（悬空引用允许：不改值、不投递） */
    {
        char msg[48];
        int n = snprintf(msg, sizeof msg, "toggle_ev %s %d", id, 0);   /* 环行格式与 T3.1 §4.2 逐字同款 */
        if (n > 0 && (size_t)n < sizeof msg) vt_shm_ring_push(msg, (size_t)n);
        fprintf(stderr, "vtouchd: 区域 %s 开关 → 关\n", id);
    }
}

/* 完成（最后一步走完）：日志 + 环行 + 自动关（auto_off 翻回门控开关）+ 状态归位。 */
static void op_finish(void)
{
    uint64_t ms = op_now_ms() - R.t_start;
    fprintf(stderr, "vtouchd: op 完成 %s 用时=%llums\n", R.name, (unsigned long long)ms);
    op_ev_push("done", R.nsteps, R.nsteps);
    op_auto_off();                                           /* 仅正常完成翻回；中止不翻（中止≠跑完，spec §4.3） */
    R.active = 0;
    R.slot = -1;                                             /* 与 abort 归位一致：清槽防下一次起跑读到陈旧槽号 */
    R.frozen = 0;                                            /* 冻结态清零：绝不泄漏到下一次运行（R1 封口） */
    R.stop_pending = 0;                                      /* 推迟账清零：绝不泄漏到下一次运行（R2a 封口） */
    g.op_run = -1;
    g.op_run_step = 0;
    g.op_run_state = 0;
}

/* 进入下一步：上一步的**结束时刻 = 本步的开始时刻**（spec §3.2：不额外加间隔、不累加误差）。 */
static void op_next_step(void)
{
    R.t0 = R.deadline;
    R.step++;
    R.phase = PH_BEGIN;
    R.deadline = R.t0;
}

/* 起一步：打步日志 + 发这一步的起始动作（点按 / 滑动先 down；等待不动手）。 */
static void op_begin_step(void)
{
    const struct vt_step *st;
    if (R.step >= R.nsteps) { op_finish(); return; }
    st = &R.steps[R.step];
    g.op_run_step = R.step;                                  /* 面板进度（0 起） */
    switch (st->type) {
    case OP_STEP_TAP:
        fprintf(stderr, "vtouchd: op 步 %d/%d 点按 %d,%d 按住%dms\n",
                R.step + 1, R.nsteps, st->a1, st->a2, st->ms);
        R.hold = st->ms;
        if (op_finger_ll("down", st->a1, st->a2) != 0) { op_conflict_abort(); return; }
        R.phase = PH_TAP_UP;
        R.deadline = R.t0 + (uint64_t)R.hold;
        break;
    case OP_STEP_SWIPE:
        fprintf(stderr, "vtouchd: op 步 %d/%d 滑动 %d,%d→%d,%d %dms\n",
                R.step + 1, R.nsteps, st->a1, st->a2, st->a3, st->a4, st->ms);
        R.dur = st->ms;
        R.nsamp = st->ms / 10;                               /* N = max(2, dur/10)：10ms 一采样 */
        if (R.nsamp < 2) R.nsamp = 2;
        if (op_finger_ll("down", st->a1, st->a2) != 0) { op_conflict_abort(); return; }
        R.sample = 1;
        R.phase = PH_SWIPE_MOVE;
        R.deadline = R.t0 + (uint64_t)R.sample * (uint64_t)R.dur / (uint64_t)R.nsamp;
        break;
    case OP_STEP_WAIT:
        fprintf(stderr, "vtouchd: op 步 %d/%d 等待 %dms\n", R.step + 1, R.nsteps, st->ms);
        R.phase = PH_WAIT;
        R.deadline = R.t0 + (uint64_t)st->ms;
        break;
    default:                                                 /* op_valid 已挡住；真漏进来就跳过，绝不卡死 */
        R.phase = PH_WAIT;
        R.deadline = R.t0;
        break;
    }
}

/* 到点了：按阶段做**一个**动作，做完就回去让 poll 重算超时 —— 下一拍再做下一个动作。
 * 「hold=0 的点按」的 up 与「滑动终点」的 up 因此天然落到下一拍（至少隔一帧，spec §3.2）；
 * 落后于时间表时也一样一拍一个动作地追（第四档的 0ms 超时把拍子拉满，不会一口气堆一串帧）。 */
static void op_advance(void)
{
    const struct vt_step *st;
    int lx, ly;
    switch (R.phase) {
    case PH_BEGIN:
        op_begin_step();
        break;
    case PH_TAP_UP:
        if (op_finger_raw("up", R.rx, R.ry) != 0) { op_conflict_abort(); return; }   /* 抬指（按下那一点） */
        op_next_step();
        break;
    case PH_SWIPE_MOVE:
        st = &R.steps[R.step];
        lx = op_lerp(st->a1, st->a3, R.sample, R.nsamp);
        ly = op_lerp(st->a2, st->a4, R.sample, R.nsamp);
        if (op_finger_ll("move", lx, ly) != 0) { op_conflict_abort(); return; }
        if (op_trace_on())                                   /* L9：默认零输出（判定只一次分支） */
            fprintf(stderr, "vtouchd: op 采样 %s %d/%d %d,%d\n", R.name, R.sample, R.nsamp, lx, ly);
        if (R.sample >= R.nsamp) {                           /* 末点 = 终点；up 下一拍（隔一帧） */
            R.phase = PH_SWIPE_UP;
            R.deadline = R.t0 + (uint64_t)R.dur;
        } else {
            R.sample++;
            R.deadline = R.t0 + (uint64_t)R.sample * (uint64_t)R.dur / (uint64_t)R.nsamp;
        }
        break;
    case PH_SWIPE_UP:
        if (op_finger_raw("up", R.rx, R.ry) != 0) { op_conflict_abort(); return; }   /* 终点抬指 */
        op_next_step();
        break;
    case PH_WAIT:
        op_next_step();                                      /* 等到点：直接进下一步 */
        break;
    }
}

/* 撞槽 / 断连自检（只在运行中、手指按着时做；spec §3.5 / §7）：
 *   · 手指该按着却已被抬掉 —— 只有外力能做到（WS reset / 断连被踢时 owner_reset 抬掉全部虚拟
 *     触点，或客户端对同槽注入 up）⇒ 记 `op 槽冲突 k` + 顺带中止操作；
 *   · 手指还按着但坐标被外力改掉（客户端对同槽注入 move）⇒ 只记一次 `op 槽冲突 k`，本次运行照跑
 *     —— v1 不做避让：下一个动作就把位置盖回来。
 * 坐标比的是我们写回的 raw（R.rx/R.ry 与 g.virt[slot] 同步；别人插过手，差值一眼可见）。 */
static void op_selfcheck(void)
{
    if (!op_finger_down()) return;
    if (!g.virt[R.slot].down) { op_conflict_abort(); return; }
    if (g.virt[R.slot].x != R.rx || g.virt[R.slot].y != R.ry) {
        if (!R.conflict) {
            R.conflict = 1;
            fprintf(stderr, "vtouchd: op 槽冲突 %d\n", R.slot);
        }
    }
}

/* 触发槽消费：单槽覆盖、只保留最新（spec §3.4）；主线程按 seq 的差值分辨「被盖掉的」那几发。
 * 名字读取与生产者写入之间有个纳秒级窗口：混读最多让这一发起跑查无此名（丢弃 + 日志），
 * 不会跑去动别的状态 —— 触发是物理手指级别的低频事件，值不当 seqlock。 */
static void op_consume_trigger(void)
{
    uint32_t seq = __atomic_load_n(&g.op_trig_seq, __ATOMIC_ACQUIRE);
    uint32_t delta, k;
    char name[OP_NAME_MAX + 1];
    size_t n;
    if (seq == R.trig_seen) return;                          /* 没有新触发：热路径零成本 */
    delta = seq - R.trig_seen;
    R.trig_seen = seq;
    /* 生产者先写 name/slot、release 才自增 seq ⇒ acquire 看到新 seq 就一定能看到它们 */
    n = strnlen(g.op_trig_name, OP_NAME_MAX + 1);
    if (n > OP_NAME_MAX) n = OP_NAME_MAX;
    memcpy(name, g.op_trig_name, n); name[n] = 0;
    for (k = 1; k < delta; k++) fprintf(stderr, "vtouchd: op 丢弃 覆盖\n");
    vt_ops_run(name);                                        /* 忙 / 不存在 / 没空闲槽由它丢弃 + 日志 */
}

/**
 * (vtouch-doc: vt_ops_init)
 * @brief 初始化操作执行器（建触发唤醒 eventfd；失败降级 -1）。
 * @return  0 成功（或已建过）；-1 eventfd 创建失败（不致命：触发最坏退回下一轮 poll 超时唤醒）。
 * @note    唤醒 fd 存进区 A 的 g.ops_wake_fd（面板只读得到、不参与）：区域线程投触发时写它一下，主循环立刻醒；没有它时触发晚 ≤1s 被 poll 兜底捡起，功能不丢。
 *
 * 为什么这么写（原有注释，逐字保留）：
 *   建唤醒 fd 前先看有没有建过（幂等）：重复调用不该把旧的 eventfd 换掉（谁都可能还持着它）。
 *   失败**不致命**：触发槽照写（seq 照增），主循环最坏在下一个 poll 超时（≤1s）里把它捡起。
 */
int vt_ops_init(void)
{
    if (g.ops_wake_fd >= 0) return 0;
    g.ops_wake_fd = (int)eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (g.ops_wake_fd < 0) {
        fprintf(stderr, "vtouchd: op 唤醒 eventfd 失败: %s → 触发退回 poll 超时（≤1s）唤醒\n", strerror(errno));
        return -1;
    }
    return 0;
}

/**
 * (vtouch-doc: vt_ops_tick)
 * @brief 主循环每轮调：消费触发槽 → 推进运行中的操作 → 刷新到点 deadline。
 * @note    按序三件事：① acquire 读触发槽（seq 变了 → 起跑最新一发，被盖掉的记 `op 丢弃 覆盖`；忙 / 操作不存在 / 门控拦截 / 没空闲槽由 vt_ops_run 丢弃 + 日志）；② 撞槽自检（手指被外力抬掉 → `op 槽冲突 k` + 顺带中止）；③ 到点做**一个**动作（步入 / 抬指 / 采样点）—— 下一拍做下一个，靠 poll 第四档的 0ms 超时追拍。上一帧没写出去（g_reemit）时整拍不动：不丢帧、不跳步。
 *
 * 为什么这么写（原有注释，逐字保留）：
 *   每轮主循环只推进一步（写失败时整拍不动）：投触发、撞槽自检、到点动作三件事按序做完就回去，
 *   下一拍的 poll 超时由 vt_ops_next_deadline_ms 压到「恰好在下一步到点时醒」。
 */
void vt_ops_tick(void)
{
    uint64_t now;
    if (g.g_reemit) return;                                  /* 上一帧没写出去：不推进（不丢帧、不跳步） */

    /* 帧冻结（R1 封口）：客户端帧事务（begin_frame..end_frame）开着时，本次 tick 直接返回 ——
     * 不推进 / 不自检 / 不消费触发。机制与起跑门（op_drop_frame）同源：帧窗内执行器写 g.virt 的
     * 每一笔都会被 end_frame 的 memcpy(g.virt, g.staged) 按 begin 快照回填冲击（down 被抹 ⇒ 悬空
     * 触点；up 被抹 ⇒ 设备侧已抬、core 侧还占着槽）。冻结 ⇒ 帧窗内执行器零 g.virt 写入，既不产生
     * 悬空触点、也不会被回填扰乱；主循环单线程（poll_step 串行）⇒ 没有检查-使用窗口。
     * 帧关由 WS fd 活动唤醒（不依赖第四档 —— 冻结期 vt_ops_next_deadline_ms 返回 -1）。 */
    if (R.active && g.frame_open) {
        if (!R.frozen) {
            R.frozen = 1;                                    /* 首次进入：记住暂停起点（每段冻结/解冻各一行日志） */
            R.frozen_since = op_now_ms();
            fprintf(stderr, "vtouchd: op 暂停 帧内\n");
        }
        return;
    }
    if (R.frozen) {
        /* 解冻（帧关后的第一次 tick）：把本步起点 t0、到点 deadline、起跑时刻 t_start 一起平移
         * 暂停时长 —— 操作从暂停点原样继续（剩余时间不变），完成日志的「用时」因此不含暂停。
         * deadline 必须跟着一起平移：它是先前按旧 t0 算好的绝对值，只动 t0 会让下一步动作立刻
         * 补发，而且 op_next_step 的 t0 = deadline 会把这笔平移又拉回去。 */
        uint64_t pause_ms;
        now = op_now_ms();
        pause_ms = now - R.frozen_since;
        R.t0 += pause_ms;
        R.deadline += pause_ms;
        R.t_start += pause_ms;
        R.frozen = 0;
        fprintf(stderr, "vtouchd: op 恢复 帧内\n");
    }
    /* 帧窗内被推迟的中止（R2a 封口 / L8）：帧关后在这里补执行 —— 上面的守卫保证走到这里时
     * frame_open 已为 0（帧还开着的话 R.active 的 tick 在前面就 return 了），vt_ops_abort 走
     * 正常路径（抬指 + 归位 + 日志）。该轮到此为止：不再消费触发 / 自检 / 推进（该轮不再做别的）。
     * 单线程论证：面板 STOP 在 poll_step 的 vt_shm_edit_apply 里落账、紧接着就是本次 tick（同一轮、
     * 任何 WS 处理之前）—— 帧还开着 ⇒ 上面冻结分支已把 frozen 置起，这里必然在解冻之后。 */
    if (R.stop_pending) {
        R.stop_pending = 0;                                  /* 先清账再执行：abort 内不再看到这笔 */
        vt_ops_abort(R.stop_why);
        return;
    }
    op_consume_trigger();                                    /* 可能起跑一条操作（deadline = 现在） */
    if (!R.active) return;
    op_selfcheck();                                          /* 手指被外力动过：该记的记、该中止的中止 */
    if (!R.active) return;
    now = op_now_ms();
    if (now >= R.deadline) op_advance();
}

/**
 * (vtouch-doc: vt_ops_next_deadline_ms)
 * @brief 下一步到点的剩余毫秒数（下限 0）；-1 = 空闲。
 * @return  剩余毫秒数（已到点 = 0）；-1 = 没有运行中的操作。
 * @note    主循环拿它当 poll 超时的**第四档**（与 5/1/1000ms 取最紧的那一档）：到点就醒，不早不晚；空闲不参与。
 *
 * 为什么这么写（原有注释，逐字保留）：
 *   剩余毫秒给 poll 当第四档用；已到点（包括落后）给 0 —— 0ms 超时把追拍拉满，
 *   但每拍只做一个动作，不会在同一个 tick 里连发 down/up。
 */
int vt_ops_next_deadline_ms(void)
{
    uint64_t now;
    if (!R.active) return -1;
    /* 帧冻结（R1 封口）：帧窗内不做 deadline 唤醒 —— 剩余毫秒已被暂停冻住（甚至为负），再返回 0
     * 会把主循环压成 0ms 超时自旋；返回 -1 让主循环维持别的档位，帧关由 WS fd 活动唤醒后自会重算。 */
    if (R.frozen || g.frame_open) return -1;
    now = op_now_ms();
    if (R.deadline <= now) return 0;
    if (R.deadline - now > (uint64_t)INT_MAX) return INT_MAX;   /* 防御：步长上限 10min，正常够不到 */
    return (int)(R.deadline - now);
}

/**
 * (vtouch-doc: vt_ops_run)
 * @brief 起跑一条操作（忙时丢弃 + 日志）。
 * @param   name     操作名；表里查不到 / 没空闲槽 / 已有操作在跑 = 丢弃 + 对应日志
 * @note    起跑 = 整条快照进执行器私有内存（运行中改表 / 删表不影响本次，spec §7）+ 挑第一个空闲虚拟槽（virt[]/staged[] 都空）全程占用，并写 g.op_run / op_run_step / op_run_state 供面板回显；日志 `op 启动 <名> 步数=N 槽=K 门控=<r1|无>`。门控 / 自动关（spec §4.3）：gate 非空先过门控检查 —— 区域须存在、是开关型且开着，否则 `op 丢弃 门控拦截`；auto_off=1 在正常完成时把门控开关翻回关（中止不翻）。
 *
 * 为什么这么写（原有注释，逐字保留）：
 *   起跑的三件事（顺序有讲究）：
 *   ① 忙就先丢（一次只跑一条，spec §3.2）—— 忙检查放最前：「运行中」本身就是明确的拒绝理由；
 *   ② 查表命中后整条**快照**进 R（步骤表一起抄）：运行中面板改表 / 删表都影响不到本次（spec §7）；
 *   ③ 挑槽同时看 virt[] / staged[] / pending_up —— 帧内暂存与待抬的触点都还占着槽（down 会被
 *   set_virtual 拒），全空才真的空闲；挑不到就丢弃（不排队、不等待）。
 *   门控（gate / auto_off）批 3 起生效：gate 非空先过门控检查 —— 锁内判定（必须存在且
 *   kind==toggle 且 toggle_on==1，否则 `op 丢弃 门控拦截`），锁外记日志；检查插在②与③之间：
 *   先问「能不能跑」，再问「有没有槽」。auto_off 只在正常完成（op_finish）翻回门控开关，
 *   中止不翻（中止≠跑完）；翻回的锁纪律与 T3.1 §4.2 同款（锁内改值、锁外环行/日志）。
 */
void vt_ops_run(const char *name)
{
    char nm[OP_NAME_MAX + 1];
    size_t n;
    int i, k, slot = -1;

    if (R.active) { op_drop_busy(); return; }
    if (g.frame_open) { op_drop_frame(); return; }           /* 帧事务开着：直写 g.virt 会被 end_frame 的 staged 回填抹掉（悬空触点） */
    n = name ? strnlen(name, OP_NAME_MAX + 1) : 0;
    if (n < 1 || n > OP_NAME_MAX) { op_drop("操作不存在", NULL); return; }
    memcpy(nm, name, n); nm[n] = 0;
    for (i = 0; i < g.op_count; i++) if (!strcmp(g.ops[i].name, nm)) break;
    if (i >= g.op_count) { op_drop("操作不存在", nm); return; }
    /* 门控检查（spec §4.3；批 3）：gate 非空 → 找区域；必须存在且 kind==toggle 且 toggle_on==1
     * 才放行，否则拒绝 + `op 丢弃 门控拦截`（解析失败同理 —— 安全侧）。放在查表命中后、挑槽前：
     * 先问「能不能跑」（门控），再问「有没有槽」（资源）。判定要读区域表（区域线程会写 toggle_on），
     * 照 T3.1 锁纪律：持 region_lock 判定、解锁后再记日志（锁内不 I/O）。 */
    if (g.ops[i].gate[0]) {
        int gate_on = 0;
        const char *gate_why = "区域不存在";                 /* 拦截原因（stderr 明细用；放行时置 NULL） */
        pthread_mutex_lock(&g.region_lock);
        for (k = 0; k < g.region_count; k++) {
            if (strcmp(g.regions[k].id, g.ops[i].gate) != 0) continue;
            if (g.regions[k].kind != 1) gate_why = "非开关型";
            else if (g.regions[k].toggle_on != 1) gate_why = "未开";
            else { gate_on = 1; gate_why = NULL; }
            break;
        }
        pthread_mutex_unlock(&g.region_lock);
        if (!gate_on) {
            op_drop("门控拦截", nm);
            if (gate_why) fprintf(stderr, "vtouchd: op 门控 %s 拦截原因=%s\n", nm, gate_why);
            return;
        }
    }
    for (k = 0; k < g.vslots; k++) {
        if (g.virt[k].down || g.virt[k].pending_up ||
            g.staged[k].down || g.staged[k].pending_up) continue;
        slot = k; break;
    }
    if (slot < 0) { op_drop("没空闲槽", nm); return; }

    R.active = 1;
    R.idx = i;
    memcpy(R.name, g.ops[i].name, sizeof R.name);
    R.nsteps = g.ops[i].step_count;
    memcpy(R.steps, g.ops[i].steps, sizeof R.steps);
    memcpy(R.gate, g.ops[i].gate, sizeof R.gate);
    R.auto_off = g.ops[i].auto_off;
    R.slot = slot;
    R.step = 0;
    R.conflict = 0;
    R.frozen = 0;                                            /* 冻结态清零：绝不泄漏进新一次运行（R1 封口） */
    R.stop_pending = 0;                                      /* 推迟账清零：绝不泄漏进新一次运行（R2a 封口） */
    R.hold = R.dur = R.nsamp = R.sample = 0;
    R.t_start = op_now_ms();
    R.t0 = R.t_start;
    R.deadline = R.t0;                                       /* 到点：本拍末尾就进入第 1 步 */
    R.phase = PH_BEGIN;
    g.op_run = R.idx;
    g.op_run_step = 0;
    g.op_run_state = 1;
    fprintf(stderr, "vtouchd: op 启动 %s 步数=%d 槽=%d 门控=%s\n",
            R.name, R.nsteps, R.slot, R.gate[0] ? R.gate : "无");
    op_ev_push("run", 1, R.nsteps);
}

/**
 * (vtouch-doc: vt_ops_abort)
 * @brief 中止运行中的操作（抬指 + 状态归位 + 日志原因）。
 * @param   why      中止原因（写进日志；如 停止按钮 / 引擎收尾 / reset/断连）
 * @note    **幂等**：没在跑（含没初始化、init 失败路径的 cleanup）就是空操作 —— cleanup() 无条件调它。抬指帧走 emit_frame：写失败置 g_reemit 等主循环重发；进程退出路径由随后 uinput 销毁兜底（触点随设备消失，物理触摸回系统）。
 *
 * 为什么这么写（原有注释，逐字保留）：
 *   幂等是硬要求：cleanup() 在 **init 失败路径**上也会被调（那时执行器可能根本没初始化），
 *   所以「没在跑」必须原样安全返回 —— 不碰 R 的字段、不发帧。
 *   抬指用 R.rx/R.ry（我们最后写回的位置）：抬指帧与别的帧一样走 emit_frame，
 *   写失败置 g_reemit 由主循环重发；真到进程退出路径，随后的 uinput 销毁会把触点一并收走。
 */
void vt_ops_abort(const char *why)
{
    int i;
    if (!R.active) return;
    /* 帧窗避让（R2a 封口 / L8）：帧事务开着时抬指会被 end_frame 的 memcpy(g.virt, g.staged) 按
     * begin 快照回填（设备侧已抬、virt 又变回「按着」并 emit ⇒ 粘指）—— 执行器任何路径都不在
     * 帧窗内写 g.virt。改成记账：原因按短缓冲安全截断存进 R.stop_why（不存裸指针），帧关后的
     * 第一次 tick 在解冻点补执行（见 vt_ops_tick）。收尾（cleanup）撞帧窗时同样推迟且不再执行 ——
     * 进程随即 teardown，触点由 uinput 设备销毁一并释放（日志可能少一条 `op 中止`，测试阶段核）。 */
    if (g.frame_open) {
        R.stop_pending = 1;
        snprintf(R.stop_why, sizeof R.stop_why, "%s", why ? why : "?");
        fprintf(stderr, "vtouchd: op 中止推迟 帧内 原因=%s\n", R.stop_why);
        return;                                              /* 不动 virt / 状态：帧窗内零 g.virt 写入 */
    }
    i = R.step < R.nsteps ? R.step + 1 : R.nsteps;
    if (op_finger_down()) op_finger_raw("up", R.rx, R.ry);
    fprintf(stderr, "vtouchd: op 中止 %s 步 %d/%d 原因=%s\n", R.name, i, R.nsteps, why ? why : "?");
    op_ev_push("abort", i, R.nsteps);
    R.active = 0;
    R.slot = -1;
    R.frozen = 0;                                            /* 冻结态清零：绝不泄漏到下一次运行（R1 封口） */
    R.stop_pending = 0;                                      /* 推迟账清零：绝不泄漏到下一次运行（R2a 封口） */
    g.op_run = -1;
    g.op_run_step = 0;
    g.op_run_state = 0;
}

/**
 * (vtouch-doc: vt_ops_trigger_post)
 * @brief 区域线程投一次触发（写触发槽 → release 自增 seq → 写唤醒 fd）。
 * @param   name     要起跑的操作名
 * @param   slot     触发来源手指的物理槽号（日志用）
 * @note    触发槽是**单槽覆盖**：主线程还没消费就被下一发盖掉时，tick 按 seq 差值记 `op 丢弃 覆盖`。名字按上限截断写；唤醒 fd 没建成（-1）时只丢这次唤醒 —— seq 还在，≤1s 的兜底 poll 会捡起。
 *
 * 为什么这么写（原有注释，逐字保留）：
 *   链路：写 name/slot（普通写）→ release 自增 seq（这一下之后名字才算「可见」）→ 写唤醒 fd。
 *   主线程 acquire 读 seq，变了才取名字 —— 单槽覆盖，中间被盖掉的由 seq 差值看出来。
 *   fd 没建成（-1）也不丢功能：seq 已经涨了，兜底 poll（≤1s）会自己来读。
 */
void vt_ops_trigger_post(const char *name, int slot)
{
    size_t n = name ? strnlen(name, OP_NAME_MAX + 1) : 0;
    if (n > OP_NAME_MAX) n = OP_NAME_MAX;
    if (n) memcpy(g.op_trig_name, name, n);
    g.op_trig_name[n] = 0;
    g.op_trig_slot = slot;
    __atomic_add_fetch(&g.op_trig_seq, 1u, __ATOMIC_RELEASE);
    if (g.ops_wake_fd >= 0) {
        uint64_t one = 1;
        ssize_t r;
        do { r = write(g.ops_wake_fd, &one, sizeof one); } while (r < 0 && errno == EINTR);
        (void)r;                     /* 非阻塞：写满（几乎到不了）只丢这次唤醒，seq 还在、兜底会捡 */
    }
}

#endif /* VT_UI */
