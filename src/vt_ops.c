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
#include <math.h>            /* llround：槽引用取整（v5；spec §5.3） */
#include "vt_vision.h"       /* 视觉匹配引擎（找图/找色 + 坐标映射；spec VISION §4/§6.1） */

/* 允许变量的数值字段判据（op_valid v2；v5 扩 -9..-1）：字段 = 字面值 v ∈ [lo, hi]，或负数编码引用
 * -9..-1（-1..-5 = 触发变量 OP_VAR_TDX..TMS、-6..-9 = 结果槽 OP_VAR_R1..R4）。
 * 坐标与时长共用（lo/hi 每档不同：坐标 = 0..逻辑尺寸-1；时长 = 各类型区间，见下）。 */
static int op_num_ok(int v, int lo, int hi)
{
    if (v >= lo && v <= hi) return 1;
    if (v >= OP_VAR_R4 && v <= OP_VAR_TDX) return 1;
    return 0;
}

/* 操作载荷校验（核心单点）：名字 / 步数 / 每步的类型、字段与引用逐条过门；
 * 不过就把一句人话写进 why（调用方拼成 `op 被拒 <名>: <原因>` 日志）。
 *
 * 规则出处（spec OPS_PLAN_V3 §6.3 / OPS_PLAN_V2 §1.4 / §2.1 / §3 / OPS_PLAN_V5 §4 / VISION §6.1）：名字与区域 id 同一把尺子
 * （vt_id_ok：[A-Za-z0-9_-]、1..15；裸 `-` 除外）；步数 1..MAX_STEPS；类型 ∈ 1..11
 * （点按/滑动/等待/按下/弹起/区域判断/开关判断/跳转/计算/找图/找色）。
 * 【允许变量的字段】坐标（点按 a1,a2；滑动 a1..a4；按下 a1,a2；区域判断 a1,a2）与时长
 * （点按/滑动/等待的 ms）：字面值（坐标 0..logical-1；时长——点按 0..60000（0 = 按下即抬）、
 * 滑动 1..60000（0 的滑动没有采样点）、等待 0..600000），或负数编码引用 -9..-1（-1..-5 = 触发变量、
 * -6..-9 = 结果槽；spec V2 §1.4 / V5 §4）——操作的手指是**注入**的，屏外的点没有意义：收下来也只是
 * 静默不命中，不如当场拒掉让面板报错。
 * 条件步（6/7）：两档位 a3/a4 ∈ 0..3（不成立侧/成立侧）；档位 = 跳转时该侧目标 ∈ 0..step_count
 * （0 = 结束、1..step_count = 目标步骤；j1 = 成立侧、j2 = 不成立侧），其余档位目标**忽略**（不校验、不拒收）；
 * ref 长度 1..REGION_ID_MAX 且过 vt_id_ok —— 存在性不校验（允许悬空，运行时按 `区域不存在` 收场，安全侧）。
 * 跳转步（8）：a1 ∈ 0..step_count（0 = 结束）；其余字段忽略。弹起（5）字段全忽略；其余步照 v2 不变。
 * 计算步（9，v5）：a1 ∈ 1..4（槽号）；expr 非空、≤63、且过 vt_expr_check（不过 → 拒收 `表达式错: <why>`）；
 * 防御：a2..a4/ms/j1/j2 必须 0、ref 必须空。
 * 视觉步（10/11，v8；T7.4 扩 ms）：字段映射照 spec VISION §6.1 定稿 —— 找图 ref=模板名（必填）、找色 ref=点集名
 * （多点必填、单点必空）、expr=区域名（空或 vt_id_ok 尺子；存在性不校验，运行时按 `区域不存在` 收场）、
 * 找图 a1=阈值 0..255、找色 a1=模式 0/1 且单点 a2=(颜色<<8)|容差（按无符号解读：打包域 = 全部 32 位，
 * 颜色高位使 int 为负 —— **不拒负值**，拒了会误杀纯红 0xFF0000 等）、多点 a2=0、a3/a4=不成立/成立档位（0..3）、
 * ms=0..60000（0 = 单次；>0 = 持续查找超时毫秒，T7.4）、j1/j2=该侧跳转目标（域同条件步）。
 * **其余所有类型**的 expr 必须为空（防御：非空 → 拒收 `表达式错`）——表达式只属于计算步、区域名只属于视觉步，
 * 别让它们静默挂在别的步上（spec V5 §4 / VISION §6.1）。
 * gate 只做终止符/长度防御（超长/未终止即拒）；存在性不校验 —— 允许悬空，起跑时解析不到就丢弃 + 日志（安全侧，见 spec §4.3）。
 */
static int op_valid(const struct vt_op *op, char *why, size_t whycap)
{
    int i;
    size_t n;

    /* 名字来自邮箱载荷，先按数组长度找终止符：未终止（strnlen 顶到 name[] 尾）按非法拒 ——
     * 不让后面的 vt_id_ok / strcmp / 日志去读越界。 */
    n = strnlen(op->name, sizeof op->name);
    if (n < 1 || n > OP_NAME_MAX || !vt_id_ok(op->name, n)) {
        snprintf(why, whycap, "名字非法（[A-Za-z0-9_-]、1..%d；裸 `-` 除外）", OP_NAME_MAX);
        return 0;
    }
    /* 门控 id 同款防御（收口）：载荷同样来自邮箱字节 —— 先按数组长度找终止符，超长/未终止即拒；
     * 不查存在性（允许悬空，起跑时解析不到再丢弃 + 日志，安全侧，spec §4.3）。 */
    n = strnlen(op->gate, sizeof op->gate);
    if (n > REGION_ID_MAX) {
        snprintf(why, whycap, "门控名非法（[A-Za-z0-9_-]、1..%d 或空；裸 `-` 除外）", REGION_ID_MAX);
        return 0;
    }
    if (op->step_count < 1 || op->step_count > MAX_STEPS) {
        snprintf(why, whycap, "步数 %d 不在 1..%d", op->step_count, MAX_STEPS);
        return 0;
    }
    for (i = 0; i < op->step_count; i++) {
        const struct vt_step *st = &op->steps[i];
        /* v5/v8 防御：表达式只属于计算步、区域名只属于视觉步 —— 其余类型带 expr 一律拒收 `表达式错`。 */
        if (st->type != OP_STEP_CALC && st->type != OP_STEP_FINDIMAGE &&
            st->type != OP_STEP_FINDCOLOR && st->expr[0] != 0) {
            snprintf(why, whycap, "表达式错");
            return 0;
        }
        switch (st->type) {
        case OP_STEP_TAP:
            if (!op_num_ok(st->a1, 0, g.logical_width - 1) ||
                !op_num_ok(st->a2, 0, g.logical_height - 1)) {
                snprintf(why, whycap, "第 %d 步坐标越界", i + 1);
                return 0;
            }
            if (!op_num_ok(st->ms, 0, 60000)) {
                snprintf(why, whycap, "第 %d 步时长越界", i + 1);
                return 0;
            }
            break;
        case OP_STEP_SWIPE:
            if (!op_num_ok(st->a1, 0, g.logical_width - 1) ||
                !op_num_ok(st->a2, 0, g.logical_height - 1) ||
                !op_num_ok(st->a3, 0, g.logical_width - 1) ||
                !op_num_ok(st->a4, 0, g.logical_height - 1)) {
                snprintf(why, whycap, "第 %d 步坐标越界", i + 1);
                return 0;
            }
            if (!op_num_ok(st->ms, 1, 60000)) {
                snprintf(why, whycap, "第 %d 步时长越界", i + 1);
                return 0;
            }
            break;
        case OP_STEP_WAIT:
            if (!op_num_ok(st->ms, 0, 600000)) {
                snprintf(why, whycap, "第 %d 步时长越界", i + 1);
                return 0;
            }
            break;
        case OP_STEP_DOWN:                       /* 按下：a1,a2 = 坐标（可变量），按下并保持 */
            if (!op_num_ok(st->a1, 0, g.logical_width - 1) ||
                !op_num_ok(st->a2, 0, g.logical_height - 1)) {
                snprintf(why, whycap, "第 %d 步坐标越界", i + 1);
                return 0;
            }
            break;
        case OP_STEP_UP:                         /* 弹起：字段全忽略 */
            break;
        case OP_STEP_COND_REGION:                /* 区域判断：a1,a2 = 判定点（可变量）；a3/a4 = 不成立/成立档位；j1/j2 = 成立/不成立侧跳转目标；ref = 区域 id */
            if (!op_num_ok(st->a1, 0, g.logical_width - 1) ||
                !op_num_ok(st->a2, 0, g.logical_height - 1)) {
                snprintf(why, whycap, "第 %d 步坐标越界", i + 1);
                return 0;
            }
            if (st->a3 < OP_COND_ABORT || st->a3 > OP_COND_JUMP ||
                st->a4 < OP_COND_ABORT || st->a4 > OP_COND_JUMP) {
                snprintf(why, whycap, "第 %d 步档位非法", i + 1);
                return 0;
            }
            if (st->a4 == OP_COND_JUMP && (st->j1 < 0 || st->j1 > op->step_count)) {
                snprintf(why, whycap, "第 %d 步跳转目标越界", i + 1);
                return 0;
            }
            if (st->a3 == OP_COND_JUMP && (st->j2 < 0 || st->j2 > op->step_count)) {
                snprintf(why, whycap, "第 %d 步跳转目标越界", i + 1);
                return 0;
            }
            n = strnlen(st->ref, sizeof st->ref);
            if (n < 1 || n > REGION_ID_MAX || !vt_id_ok(st->ref, n)) {
                snprintf(why, whycap, "第 %d 步区域引用非法（[A-Za-z0-9_-]、1..15）", i + 1);
                return 0;
            }
            break;
        case OP_STEP_COND_TOGGLE:                /* 开关判断：a3/a4 = 不成立/成立档位；j1/j2 = 成立/不成立侧跳转目标；ref = 区域 id（须开关型——运行时不在这查） */
            if (st->a3 < OP_COND_ABORT || st->a3 > OP_COND_JUMP ||
                st->a4 < OP_COND_ABORT || st->a4 > OP_COND_JUMP) {
                snprintf(why, whycap, "第 %d 步档位非法", i + 1);
                return 0;
            }
            if (st->a4 == OP_COND_JUMP && (st->j1 < 0 || st->j1 > op->step_count)) {
                snprintf(why, whycap, "第 %d 步跳转目标越界", i + 1);
                return 0;
            }
            if (st->a3 == OP_COND_JUMP && (st->j2 < 0 || st->j2 > op->step_count)) {
                snprintf(why, whycap, "第 %d 步跳转目标越界", i + 1);
                return 0;
            }
            n = strnlen(st->ref, sizeof st->ref);
            if (n < 1 || n > REGION_ID_MAX || !vt_id_ok(st->ref, n)) {
                snprintf(why, whycap, "第 %d 步区域引用非法（[A-Za-z0-9_-]、1..15）", i + 1);
                return 0;
            }
            break;
        case OP_STEP_JUMP:                       /* 跳转：a1 = 目标步骤（0 = 结束、1..step_count = 目标）；其余字段忽略 */
            if (st->a1 < 0 || st->a1 > op->step_count) {
                snprintf(why, whycap, "第 %d 步跳转目标越界", i + 1);
                return 0;
            }
            break;
        case OP_STEP_CALC: {                     /* 计算（v5）：a1 = 槽号 1..4；expr 非空且可解析；其余字段/ref 必须空（spec V5 §4） */
            char w[40];
            if (st->a1 < 1 || st->a1 > 4) {
                snprintf(why, whycap, "第 %d 步槽号非法（1..4）", i + 1);
                return 0;
            }
            if (st->a2 || st->a3 || st->a4 || st->ms || st->j1 || st->j2) {
                snprintf(why, whycap, "第 %d 步字段必须为 0", i + 1);
                return 0;
            }
            if (st->ref[0]) {
                snprintf(why, whycap, "第 %d 步区域引用必须为空", i + 1);
                return 0;
            }
            n = strnlen(st->expr, sizeof st->expr);
            if (n >= sizeof st->expr) {          /* 未终止（防御，载荷来自邮箱字节）→ 不喂给解析器 */
                snprintf(why, whycap, "表达式错: 未终止");
                return 0;
            }
            if (vt_expr_check(st->expr, w, sizeof w) != 0) {   /* 空 / 超长 / 语法 / 未知名字都由它拦（spec V5 §4） */
                snprintf(why, whycap, "表达式错: %s", w);
                return 0;
            }
            break;
        }
        case OP_STEP_FINDIMAGE: {                /* 找图（v8；T7.4 扩 ms）：ref = 模板名（必填）；expr = 区域名（空 = 全屏）；
                                                  * a1 = 阈值 0..255；a2 = 0；ms = 0..60000（0 = 单次、>0 = 持续超时）；
                                                  * a3/a4 = 不成立/成立档位；j1/j2 = 该侧跳转目标（域同条件步）。spec VISION §6.1 */
            if (st->a1 < 0 || st->a1 > 255) {
                snprintf(why, whycap, "第 %d 步阈值越界（0..255）", i + 1);
                return 0;
            }
            if (st->a2 != 0) {
                snprintf(why, whycap, "第 %d 步字段必须为 0", i + 1);
                return 0;
            }
            if (st->ms < 0 || st->ms > 60000) {
                snprintf(why, whycap, "第 %d 步超时越界（0..60000）", i + 1);
                return 0;
            }
            if (st->a3 < OP_COND_ABORT || st->a3 > OP_COND_JUMP ||
                st->a4 < OP_COND_ABORT || st->a4 > OP_COND_JUMP) {
                snprintf(why, whycap, "第 %d 步档位非法", i + 1);
                return 0;
            }
            if (st->a4 == OP_COND_JUMP && (st->j1 < 0 || st->j1 > op->step_count)) {
                snprintf(why, whycap, "第 %d 步跳转目标越界", i + 1);
                return 0;
            }
            if (st->a3 == OP_COND_JUMP && (st->j2 < 0 || st->j2 > op->step_count)) {
                snprintf(why, whycap, "第 %d 步跳转目标越界", i + 1);
                return 0;
            }
            n = strnlen(st->ref, sizeof st->ref);
            if (n < 1 || n > REGION_ID_MAX || !vt_id_ok(st->ref, n)) {
                snprintf(why, whycap, "第 %d 步模板引用非法（[A-Za-z0-9_-]、1..15）", i + 1);
                return 0;
            }
            n = strnlen(st->expr, sizeof st->expr);
            if (n > 0 && (n > REGION_ID_MAX || !vt_id_ok(st->expr, n))) {
                snprintf(why, whycap, "第 %d 步区域引用非法（[A-Za-z0-9_-]、1..15）", i + 1);
                return 0;
            }
            break;
        }
        case OP_STEP_FINDCOLOR: {                /* 找色（v8；T7.4 扩 ms）：a1 = 模式 0 单点 / 1 多点；ref = 点集名（多点必填、
                                                  * 单点必空）；expr = 区域名；单点 a2 = (颜色<<8)|容差（两段：
                                                  * 颜色 24 位 / 容差 8 位；按无符号解读 —— 颜色高位使 int 为负，
                                                  * 不拒负值）；多点 a2 = 0；a3/a4 = 档位；ms = 0..60000（0 = 单次、>0 = 持续超时）；
                                                  * j1/j2 = 目标 */
            uint32_t packed;
            if (st->a1 != 0 && st->a1 != 1) {
                snprintf(why, whycap, "第 %d 步模式非法（0/1）", i + 1);
                return 0;
            }
            if (st->ms < 0 || st->ms > 60000) {
                snprintf(why, whycap, "第 %d 步超时越界（0..60000）", i + 1);
                return 0;
            }
            if (st->a3 < OP_COND_ABORT || st->a3 > OP_COND_JUMP ||
                st->a4 < OP_COND_ABORT || st->a4 > OP_COND_JUMP) {
                snprintf(why, whycap, "第 %d 步档位非法", i + 1);
                return 0;
            }
            if (st->a4 == OP_COND_JUMP && (st->j1 < 0 || st->j1 > op->step_count)) {
                snprintf(why, whycap, "第 %d 步跳转目标越界", i + 1);
                return 0;
            }
            if (st->a3 == OP_COND_JUMP && (st->j2 < 0 || st->j2 > op->step_count)) {
                snprintf(why, whycap, "第 %d 步跳转目标越界", i + 1);
                return 0;
            }
            n = strnlen(st->ref, sizeof st->ref);
            if (st->a1 == 1) {                   /* 多点：点集名必填 + a2 必须 0（基准色/容差/点表在 .pts） */
                if (n < 1 || n > REGION_ID_MAX || !vt_id_ok(st->ref, n)) {
                    snprintf(why, whycap, "第 %d 步点集引用非法（多点必填；[A-Za-z0-9_-]、1..15）", i + 1);
                    return 0;
                }
                if (st->a2 != 0) {
                    snprintf(why, whycap, "第 %d 步字段必须为 0", i + 1);
                    return 0;
                }
            } else {                             /* 单点：点集名必须空；a2 = (颜色<<8)|容差 —— 两段校验：
                                                  * 颜色段 = (uint32)a2>>8 ∈ 0..0xFFFFFF、容差段 = a2&0xFF ∈ 0..255。
                                                  * 打包填满整个 32 位域 ⇒ 两段各自按无符号解读恒在域内（本检查是
                                                  * 形式化留痕；实现口径 = 接受全部 32 位，含 int 为负的高颜色值）。 */
                if (n != 0) {
                    snprintf(why, whycap, "第 %d 步点集引用必须为空（单点）", i + 1);
                    return 0;
                }
                packed = (uint32_t)st->a2;
                if ((packed >> 8) > 0xffffffu || (packed & 0xffu) > 0xffu) {
                    snprintf(why, whycap, "第 %d 步打包值非法", i + 1);
                    return 0;
                }
            }
            n = strnlen(st->expr, sizeof st->expr);
            if (n > 0 && (n > REGION_ID_MAX || !vt_id_ok(st->expr, n))) {
                snprintf(why, whycap, "第 %d 步区域引用非法（[A-Za-z0-9_-]、1..15）", i + 1);
                return 0;
            }
            break;
        }
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
 * @note    校验全在核心这一处（与区域 id 同一把尺子）：名字 vt_id_ok（[A-Za-z0-9_-]、1..15；裸 `-` 除外）、步数 1..MAX_STEPS、类型 ∈ {点按,滑动,等待,按下,弹起,区域判断,开关判断,跳转,计算,找图,找色}、坐标字段（点按/滑动/按下/区域判断）0..逻辑尺寸-1 或负数编码引用（-9..-1：-1..-5 = 触发变量、-6..-9 = 结果槽）、时长字段（点按/滑动/等待）按类型分档（点按 0..60000 / 滑动 1..60000 / 等待 0..600000）或负数编码引用（同上）、计算步 a1 ∈ 1..4 且 expr 非空、过 vt_expr_check（其余字段/ref 必须空；不过拒 `表达式错: <原因>`）、其余类型的 expr 必须为空（防御：非空拒 `表达式错`；计算步与视觉步除外）、条件步两档位 a3/a4 ∈ 0..3（不成立侧/成立侧），档位 = 跳转时该侧目标（不成立侧 j2 / 成立侧 j1）∈ 0..步数（0 = 结束）、视觉步（找图/找色，v8）：ref = 模板名（找图，必填）/ 点集名（找色多点必填、单点必空）、expr = 区域名（空或 [A-Za-z0-9_-]、1..15；存在性不校验，运行时按 `区域不存在` 收场）、找图 a1 = 阈值 0..255、找色 a1 = 模式 0/1 且单点 a2 = (颜色<<8)|容差（按无符号解读、域 = 全部 32 位）/ 多点 a2 = 0、a3/a4 = 档位 0..3、ms = 0..60000（0 = 单次、>0 = 持续查找超时毫秒，T7.4）、跳转目标域同条件步、跳转步 a1 ∈ 0..步数、ref 长度 1..15 且过 vt_id_ok（存在性不校验，允许悬空）。拒绝打 `op 被拒 <名>: <原因>`、成功打 `op 编辑 put <名> 步数=N`；重名覆盖就地写（表位不变），要么整条生效、要么一点都不动。
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
 *   sx1..sy2  滑动本步解析后的起终点（负数编码在起一步时解析；采样点用它插值）
 *   rx/ry   上一次写进 g.virt[slot] 的 raw 坐标（撞槽检测：别人动过它就知道）
 *   conflict  撞槽日志只打一次（v1 不做避让，spec §3.5）
 *   jumps   本次运行的跳转计数（起跑清零；条件跳转 + 跳转步共用一枚；超限中止 `跳转超限`，spec §2.3）
 *   trig_seen  已消费到的触发序号（触发槽 SPSC 的消费者一侧）
 *   trig    触发数据快照（起跑时整组拷入，运行中不回填；td=NULL 的手动运行 = 全零 ⇒ 全部变量无值）
 *   slots/slot_mask  结果槽 r1..r4 与已写位（v5）：起跑清零；计算步写、槽引用（-6..-9）读（spec V5 §5）
 *   t_start   起跑时刻（完成日志的「用时」）
 *   frozen / frozen_since  帧冻结标记与冻结起点（帧窗内暂停推进；帧关后把 t0/deadline/t_start
 *              一起平移暂停时长 —— 见 vt_ops_tick）；起跑 / 完成 / 中止都清零，不泄漏到下一次运行
 *   stop_pending / stop_why  帧窗内被推迟的中止（R2a 封口 / L8）：原因快照进 stop_why（短缓冲、
 *              安全截断，不存裸指针）；帧关后的第一次 tick 在解冻点补执行（见 vt_ops_tick）；
 *              起跑 / 完成 / 中止正常路径三处清零，不泄漏到下一次运行
 *   held    按下步的持有态（1 = 有按下步的手指还按着，等弹起步或收尾释放；spec §2.2）：
 *           按住期只允许 等待 / 弹起（及条件步 / 跳转 / 计算 / 视觉步）—— 点按 / 滑动 / 按下在步入口
 *           统一中止 `槽占用`；收尾（正常完成 / 中止）还按着 → 自动松开 + `op 收尾 松开`；
 *           起跑 / 完成 / 中止三处清零
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
    int      held;                                           /* 按下步持有态（spec §2.2）：1 = 手指还按着；弹起步 / 收尾释放负责清 */
    int      hold;
    int      dur;
    int      nsamp;
    int      sample;
    int      sx1, sy1, sx2, sy2;                             /* 滑动本步解析后的起终点（变量快照的解析结果） */
    int      rx, ry;
    int      conflict;
    int      jumps;                                          /* 本次运行的跳转计数（起跑清零；条件跳转 + 跳转步共用；spec §2.3）：超限中止 `跳转超限` */
    uint32_t trig_seen;
    struct vt_trig_data trig;                                /* 触发数据快照（起跑时整组拷入；spec §1.5） */
    double   slots[4];                                       /* 结果槽 r1..r4（v5 计算步写、槽引用读；起跑清零，spec V5 §5.1）——
                                                              * 名字用复数避与上面的虚拟槽号 R.slot 撞名 */
    unsigned slot_mask;                                      /* 槽已写位（位 0..3 = r1..r4；1=已写；起跑清零） */
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

/* 现在的**阶段**手指按着没有（撞槽自检 / 中止抬指都看它；按下步的持有态 held 不在此列 —— 走 op_release_held）。 */
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

/**
 * (vtouch-doc: op_release_held)
 * @brief 收尾释放：还按着（held）就补一笔 up 并记 `op 收尾 松开`。
 * @note    **静态**，只在执行器内用；正常完成（op_finish）与中止（vt_ops_abort）两条收尾路径共用 —— 结束仍按着 → 自动松开（spec §2.2）。帧窗纪律：调用点都在帧关路径上（abort 撞帧窗走 stop_pending 推迟、帧关后才执行）—— 执行器任何路径不在帧窗内写 g.virt 的不变式不破；写失败忽略（槽已不在手里时无事可做，与 abort 抬指同款）。
 */
static void op_release_held(void)
{
    if (!R.held) return;
    op_finger_raw("up", R.rx, R.ry);                         /* 写失败忽略：槽已不在手里时无事可做（与 abort 抬指同款） */
    fprintf(stderr, "vtouchd: op 收尾 松开\n");
}

/**
 * (vtouch-doc: op_resolve)
 * @brief 解析一个可变量字段：字面值原样出；负数编码查触发快照或结果槽。
 * @param   v        字段原值：字面值（≥0）或负数编码引用 -9..-1（-1..-5 = 触发变量、-6..-9 = 结果槽）
 * @param   lo       结果槽取整后的夹取下界（坐标 0 / 时长按类型档；spec V5 §5.3）
 * @param   hi       夹取上界（坐标 逻辑尺寸-1 / 时长按类型档）
 * @param   out      成功时写入解析结果
 * @return  0 成功；-1 失败（已按码中止：`变量无值` / `结果无值`）。
 * @note    **静态**，只在执行器内用：v>=0 直接出；-1..-5 查 R.trig.mask 的对应位（OP_TRIGB_TDX << idx），未设即中止 `变量无值` —— **不静默当 0**（spec §1.3/D6）；-6..-9 查 R.slot_mask（未写即中止 `结果无值`），已写则 llround 取整后夹取 [lo,hi]（静默语义，spec V5 §5.3）；越界负值不会到达（op_valid 已拒，防御按无值中止）。
 */
static int op_resolve(int v, int lo, int hi, int *out)
{
    const int val[OP_VAR_N] = { R.trig.dx, R.trig.dy, R.trig.ux, R.trig.uy, R.trig.ms };
    int idx;

    if (v >= 0) { *out = v; return 0; }                      /* 字面值：原样出 */
    if (v >= OP_VAR_R4 && v <= OP_VAR_R1) {                  /* -6..-9 = r1..r4（v5 结果槽引用；spec §5.3） */
        idx = OP_VAR_R1 - v;                                 /* -6→0(r1) … -9→3(r4) */
        if (!(R.slot_mask & (1u << idx))) {                  /* 槽未写 ⇒ 结果无值（绝不静默当 0；spec V5 §5.2） */
            vt_ops_abort("结果无值");
            return -1;
        }
        {
            double d = R.slots[idx];                         /* 先夹 double 域再取整：llround 超范围行为未指定（spec §5.3） */
            if (d < (double)lo) d = lo;                      /* 夹取 [lo,hi]：静默语义，不因越界中止（spec §5.3） */
            else if (d > (double)hi) d = hi;
            *out = (int)llround(d);
        }
        return 0;
    }
    if (v < OP_VAR_TMS || v > OP_VAR_TDX) {                  /* 不会到达：op_valid 只放行 -9..-1；防御按无值 */
        vt_ops_abort("变量无值");
        return -1;
    }
    idx = OP_VAR_TDX - v;                                    /* -1→0(tdx) … -5→4(tms)，与 OP_TRIGB_* 位序一致 */
    if (!(R.trig.mask & (OP_TRIGB_TDX << idx))) {            /* 本次触发没带这个变量 ⇒ 无值（绝不静默当 0） */
        vt_ops_abort("变量无值");
        return -1;
    }
    *out = val[idx];
    return 0;
}

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

/* 完成（最后一步走完）：收尾释放（还按着 → 自动松开）+ 日志 + 环行 + 自动关（auto_off 翻回门控开关）+ 状态归位。 */
static void op_finish(void)
{
    uint64_t ms;

    op_release_held();                                       /* 收尾兜底（spec §2.2）：结束仍按着 → 写 up + `op 收尾 松开` */
    ms = op_now_ms() - R.t_start;
    fprintf(stderr, "vtouchd: op 完成 %s 用时=%llums\n", R.name, (unsigned long long)ms);
    op_ev_push("done", R.nsteps, R.nsteps);
    op_auto_off();                                           /* 仅正常完成翻回；中止不翻（中止≠跑完，spec §4.3） */
    R.active = 0;
    R.slot = -1;                                             /* 与 abort 归位一致：清槽防下一次起跑读到陈旧槽号 */
    R.frozen = 0;                                            /* 冻结态清零：绝不泄漏到下一次运行（R1 封口） */
    R.stop_pending = 0;                                      /* 推迟账清零：绝不泄漏到下一次运行（R2a 封口） */
    R.held = 0;                                              /* 持有态清零：收尾已释放（上面），绝不泄漏到下一次运行 */
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

/**
 * (vtouch-doc: op_jump_apply)
 * @brief 跳转收口（条件跳转 / 跳转步共用）：0 = 结束 → op_finish；否则过守卫后落位目标步骤。
 * @param   target   跳转目标：0 = 结束、1..步数 = 目标步骤
 * @note    **静态**，只在执行器内用。0 = 结束 → 正常完成（收尾释放 / 自动关照走），不占跳转计数、不判上限（spec §2.3）；否则跳转计数（R.jumps，起跑清零，条件跳转 + 跳转步共用一枚）+1，超过 VT_OPS_JUMP_MAX（200）→ 中止 `跳转超限`（防死循环）；通过 → R.step = 目标-2 → PH_WAIT（本动作单拍结束，下一拍 op_next_step 的正常推进 +1 精确落在目标步：R.step = 目标-1 —— 预置与「跳过下一步」同款）。VTOUCH_OPS_TRACE=1 时每跳一行 `op 跳转 <名> 第 A 步 → 第 B 步`（B=0 打 `→ 结束`；L9 默认零输出）。
 *
 * 为什么这么写（原有注释，逐字保留）：
 *   跳转收口（条件跳转 / 跳转步共用；spec §2.2/§2.3）：目标 0 = 结束 → 直接 op_finish()（正常完成；收尾释放 /
 *   自动关照走）—— 不占跳转计数、不判上限；否则过跳转守卫（计数 +1，超过 VT_OPS_JUMP_MAX → 中止 `跳转超限`，
 *   防死循环）→ R.step = 目标-2 → PH_WAIT（本动作单拍结束，下一拍 op_next_step 的正常推进 +1 精确落在目标步：
 *   R.step = 目标-1）。预置值取「目标-2」的由来：目标步下标 = 目标-1（0 基）、本动作单拍结束全靠正常推进落位
 *   —— 与「跳过下一步」的「预置 + 正常推进」同款（照设计稿字面预置 目标-1 会越过目标一步：第 1 步永远到不了、
 *   「目标 = 自身」不成自环，与 spec §2.2 的循环 / 自环口径矛盾）。TRACE（op_trace_on）：每跳一行
 *   `op 跳转 <名> 第 A 步 → 第 B 步`（B=0 打 `→ 结束`；L9 默认零输出）。
 */
static void op_jump_apply(int target)
{
    if (op_trace_on()) {                                     /* TRACE：每跳一行（判定只一次分支；L9 默认零输出） */
        if (target == 0)
            fprintf(stderr, "vtouchd: op 跳转 %s 第 %d 步 → 结束\n", R.name, R.step + 1);
        else
            fprintf(stderr, "vtouchd: op 跳转 %s 第 %d 步 → 第 %d 步\n", R.name, R.step + 1, target);
    }
    if (target == 0) { op_finish(); return; }                /* 0 = 结束：正常完成（收尾释放 / 自动关照走）；不占计数、不判上限（spec §2.3） */
    R.jumps++;
    if (R.jumps > VT_OPS_JUMP_MAX) {                         /* 防死循环守卫（spec §2.3）：超限走既有中止机制 */
        vt_ops_abort("跳转超限");
        return;
    }
    R.step = target - 2;                                     /* 目标步下标 = 目标-1；下一拍正常推进 +1 落位（见上注） */
    R.phase = PH_WAIT;                                       /* 单拍动作到此为止，下一拍进目标步 */
    R.deadline = R.t0;
}

/**
 * (vtouch-doc: op_cond_apply)
 * @brief 条件判定收口（两侧四档）：按侧记日志，再执行 继续 / 跳过 / 跳转 / 中止。
 * @param   st       当前条件 / 视觉步（读本侧档位与跳转目标）
 * @param   hit      判定结果：1 = 成立、0 = 不成立
 * @param   word     日志词：`区域判断` / `开关判断` / `找图` / `找色`
 * @param   arg      日志第二词：条件步 / 找图 = 区域 / 模板名；找色 = NULL（不打印）
 * @param   no_word  不成立侧中止词：条件步 = `条件不成立`；视觉步 = `未命中`
 * @note    **静态**，只在执行器内用（区域判断 / 开关判断 / 找图 / 找色共用：两侧四档一处实现防两处漂移）。档位与目标取本侧（成立侧 = a4/j1、不成立侧 = a3/j2）：继续（单拍结束，下一拍进下一步）；跳过（步序额外 +1：跳过的那一步不执行也不求值；越过末步 = 正常完成）；跳转（0 = 结束 → op_finish；其余交 op_jump_apply 过守卫后落位）；中止（不成立侧 = no_word 参数、成立侧 `条件中止`）。日志（spec §1.3 / VISION §8）：不成立侧恒打、成立侧档位 ≠ 继续才打；不成立行先于中止行。调用点都在 region_lock 之外（spec §3.2 锁纪律：持锁判定、解锁后记日志）。
 */
static void op_cond_apply(const struct vt_step *st, int hit, const char *word,
                          const char *arg, const char *no_word)
{
    int tier   = hit ? st->a4 : st->a3;                      /* 本侧档位（成立侧 = a4 / 不成立侧 = a3） */
    int target = hit ? st->j1 : st->j2;                      /* 本侧跳转目标（仅档位 = 跳转时有意义） */

    /* 日志（spec §1.3）：不成立侧恒打（四档全列）；成立侧档位 ≠ 继续才打（三档）。
     * 第二词（arg）只有条件步与找图有（区域 / 模板名）；找色没有 —— 不打（spec VISION §8 逐字）。 */
    if (!hit || tier != OP_COND_CONT) {
        char act[32];
        if (tier == OP_COND_JUMP && target == 0) snprintf(act, sizeof act, "跳到结束");
        else if (tier == OP_COND_JUMP)           snprintf(act, sizeof act, "跳到第 %d 步", target);
        else if (tier == OP_COND_SKIP)           snprintf(act, sizeof act, "跳过下一步");
        else if (tier == OP_COND_CONT)           snprintf(act, sizeof act, "继续下一步");   /* 只可能到不成立侧（成立侧上面已滤掉） */
        else                                     snprintf(act, sizeof act, "中止");
        if (arg && arg[0])
            fprintf(stderr, "vtouchd: op 条件 %s %s %s → %s\n", word, arg, hit ? "成立" : "不成立", act);
        else
            fprintf(stderr, "vtouchd: op 条件 %s %s → %s\n", word, hit ? "成立" : "不成立", act);
    }
    switch (tier) {
    case OP_COND_CONT:                                       /* 继续下一步：单拍动作到此为止（现状） */
        R.phase = PH_WAIT;
        R.deadline = R.t0;
        break;
    case OP_COND_SKIP:                                       /* 跳过下一步：推进量额外 +1（下一拍 op_next_step 再 +1 ⇒ 共 +2） */
        R.step++;
        R.phase = PH_WAIT;
        R.deadline = R.t0;
        break;
    case OP_COND_JUMP:                                       /* 跳转：0 = 结束 / 其余过守卫落位（与跳转步共用 op_jump_apply） */
        op_jump_apply(target);
        break;
    default:                                                 /* OP_COND_ABORT：不成立行已先记；中止走既有机制（spec §3.3） */
        vt_ops_abort(hit ? "条件中止" : no_word);             /* 成立侧新词 `条件中止`；不成立侧按调用方给词（条件步 `条件不成立` / 视觉步 `未命中`） */
        break;
    }
}

/* ===================== 视觉步骤（找图 / 找色；spec VISION §6 / §8） ===================== */

/* 一帧多步复用的缓存：上次成功帧的（时间 / 方向 / 缓冲 / 几何）。now-ts < VT_VIS_REUSE_MS 且
 * 帧头没被换过（buf_idx 同）、方向一致（与面板上报的当前方向相同）→ 复用不重抓（spec §9）。
 * 全部只有主线程碰（执行器在主循环里跑）—— 不需要锁。 */
struct op_vis_frame {
    const uint8_t *rgba;             /* 帧缓冲（直指 shm；原地匹配、不拷贝，spec §3.2） */
    int w, h, stride;                /* 帧尺寸 / 行跨距（字节） */
    int rot;                         /* 抓帧时方向（0..3） */
    uint64_t ts_ns;                  /* 抓帧完成时刻（单调钟；复用窗口判据） */
};
static uint32_t S_vis_req;           /* 抓帧请求序号（单调递增；写进 req_pending） */
static int      S_vis_have;          /* 有缓存帧（复用候选） */
static int      S_vis_buf;           /* 缓存帧所在缓冲（0/1） */
static struct op_vis_frame S_vis_f;  /* 缓存帧 */

/**
 * (vtouch-doc: op_vis_panel_dead)
 * @brief 「面板不在」判据：ui_hb 冻结 ≥3s（或从未有过心跳）= 面板不在。
 * @return  1 = 面板不在；0 = 心跳在动。
 * @note    **静态**，只在执行器内用；判据同 vt_panel.c 看门狗（单调钟 3s，不是拍数；不扩 vt_panel 接口）。从未有过心跳（ui_hb == 0）直接算不在：面板没起来过，抓帧无从谈起 —— 视觉步立即 `无画面`，不空等。
 */
static int op_vis_panel_dead(void)
{
    static uint32_t last_hb;
    static uint64_t hb_at_ms;
    uint32_t hb = vt_shm_ui_hb();
    uint64_t now = op_now_ms();

    if (hb == 0) return 1;                                   /* 从未有过面板心跳：面板没起来过 */
    if (hb != last_hb) { last_hb = hb; hb_at_ms = now; return 0; }
    return now - hb_at_ms >= VT_VIS_PANEL_STALL_MS;
}

/**
 * (vtouch-doc: op_vis_capture)
 * @brief 取帧：复用缓存（≤50ms / 帧未变 / 方向一致）或发抓帧请求并轮询等待完成。
 * @param   fr       输出：帧句柄（rgba 直指 shm 缓冲；调用方在使用期间保证不再发新请求）
 * @param   force    1 = 强制抓新帧（持续查找用：复用缓存会原地空转同一画面）；0 = 允许复用（单次路径照旧）
 * @param   err      输出：失败原因词（`无画面` / `视觉错`）
 * @return  0 成功；-1 失败（err 已置原因词 —— 调用方中止或按码报错；本函数不再直接中止）。
 * @note    **静态**，只在执行器内用。协议（spec §2.2/§3.1）：写 req_pending = ++请求序号 → 轮询等 req_seq == 该序号（usleep(1000)，总超时 VT_VIS_CAPTURE_TIMEOUT_MS）→ 校验 flags 无错 + 尺寸合法 → 按 seqlock 读一次稳定帧（读 seq → 读字段 → 再读 seq；奇/变 → 重试至超时）。面板不在（ui_hb 冻结 ≥3s）→ 立即 `无画面`；超时 / flags 报错 → `无画面`；头损坏 / 尺寸非法 → `视觉错`。TRACE（VTOUCH_OPS_TRACE=1）：`vis 抓帧 请求 → 完成 <ms>`。
 */
static int op_vis_capture(struct op_vis_frame *fr, int force, const char **err)
{
    struct vt_shm_frame_hdr *h = vt_shm_frame();
    uint64_t t0, t1, fbuf = (uint64_t)g.logical_width * (uint64_t)g.logical_height * 4u;
    uint32_t req, s1, s2, buf, w, hh, stride, rot;
    uint64_t ts;

    if (!h) { *err = "视觉错"; return -1; }

    /* 一帧多步复用（spec §9）：≤50ms、帧仍有效、缓冲没被换过、方向一致（与面板上报的当前方向相同）。
     * 持续查找（T7.4）force=1：每拍都要新帧 —— 复用同一帧会原地空转（匹配同一画面到 50ms 窗口过期）。 */
    if (!force && S_vis_have &&
        now_ns() - S_vis_f.ts_ns < (uint64_t)VT_VIS_REUSE_MS * 1000000ull &&
        (h->flags & VT_FRAME_F_VALID) && !(h->flags & VT_FRAME_F_ERR) &&
        h->buf_idx == (uint32_t)S_vis_buf && h->rotation == (uint32_t)S_vis_f.rot &&
        vt_shm_panel_rot() == S_vis_f.rot) {
        *fr = S_vis_f;
        return 0;
    }

    if (op_vis_panel_dead()) { *err = "无画面"; return -1; }
    req = ++S_vis_req;                                       /* 单调请求序号（0 留给「无请求」初值） */
    __atomic_store_n(&h->req_pending, req, __ATOMIC_RELEASE);
    t0 = now_ns();
    for (;;) {                                               /* 完成判定 = req_seq == 请求序号（最后写） */
        if (__atomic_load_n(&h->req_seq, __ATOMIC_ACQUIRE) == req) break;
        if (op_vis_panel_dead()) { *err = "无画面"; return -1; }
        if (now_ns() - t0 >= (uint64_t)VT_VIS_CAPTURE_TIMEOUT_MS * 1000000ull) {
            *err = "无画面";
            return -1;
        }
        usleep(1000);
    }
    t1 = now_ns();
    /* 抓帧失败（flags bit1）/ 没有有效帧（bit0 未置）→ `无画面`（spec §6.2：抓帧失败/超时/面板不在）。
     * 只看完成之后的 flags：等待循环里不看 —— 上一发的失败码可能还挂着，等 req_seq 到了才可信
     * （面板每次完成时两位一起重写，见 vt_shm.h 的 flags 契约）。 */
    if (!(h->flags & VT_FRAME_F_VALID) || (h->flags & VT_FRAME_F_ERR)) {
        *err = "无画面";
        return -1;
    }
    /* 按 seqlock 读一次稳定帧头（读 seq → 读字段 → load-load 屏障 → 再读 seq；奇/变 → 重试至超时，
     * spec §3.2）。屏障必须有：第二次 seq 读的 acquire 不约束其前访问（ARMv8 LDAR 只挡其后的访问），
     * 字段读与 s2 之间缺 load-load 序 ⇒ 撕裂场景 s1 == s2 仍可能成立（同 vt_shm.c 的 panel_rect）。
     * 数据缓冲不拷贝：完成观察之后到下一次请求之前面板不会碰它（双缓冲 + 单请求在途），
     * 直接原地匹配（spec §3.2「或直接原地匹配，匹配只读不改帧」）。 */
    for (;;) {
        s1 = __atomic_load_n(&h->seq, __ATOMIC_ACQUIRE);
        if (!(s1 & 1u)) {                                    /* 偶 = 稳定；奇 = 面板写入中 → 重试 */
            buf = h->buf_idx;
            w = h->width; hh = h->height; stride = h->stride; rot = h->rotation;
            ts = h->ts_ns;
            __sync_synchronize();                            /* load-load 屏障：字段读先于 s2 读（同 panel_rect） */
            s2 = __atomic_load_n(&h->seq, __ATOMIC_ACQUIRE);
            if (s1 == s2) break;                             /* 读期间没变过：这份帧头可信 */
        }
        if (now_ns() - t0 >= (uint64_t)VT_VIS_CAPTURE_TIMEOUT_MS * 1000000ull) {
            *err = "无画面";
            return -1;
        }
        usleep(1000);
    }
    /* 尺寸合法（spec §6.1「校验」；越界按「缓冲损坏 / 尺寸超限」中止 `视觉错`）。
     * 帧按 1:1 口径（spec §3.3：物理 vs 逻辑通常 1:1）；帧字节跨度必须装进缓冲（两方向同字节数）。 */
    if (w == 0 || hh == 0 || w > 4096 || hh > 4096 ||
        stride < w * 4u || (uint64_t)stride * (hh - 1u) + (uint64_t)w * 4u > fbuf ||
        buf > 1u || rot > 3u) {
        *err = "视觉错";
        return -1;
    }
    fr->rgba = vt_shm_frame_buf((int)buf);
    if (!fr->rgba) { *err = "视觉错"; return -1; }
    fr->w = (int)w; fr->h = (int)hh; fr->stride = (int)stride;
    fr->rot = (int)rot; fr->ts_ns = ts;
    S_vis_f = *fr; S_vis_buf = (int)buf; S_vis_have = 1;
    if (op_trace_on())                                       /* TRACE：抓帧全链耗时（L9 默认零输出） */
        fprintf(stderr, "vtouchd: vis 抓帧 请求 → 完成 %llums\n",
                (unsigned long long)((t1 - t0) / 1000000ull));
    return 0;
}

/**
 * (vtouch-doc: op_vis_region)
 * @brief 区域换算：区域名 → 区域几何 → 逻辑矩形 → 帧矩形（空 = 全屏）。
 * @param   name     区域名（空 = 全屏；调用方保证 NUL 结尾）
 * @param   fr       帧句柄（读 rot / w / h）
 * @param   rx,ry,rw,rh 输出帧矩形（含端点；空 name = 全屏）
 * @return  0 成功；-1 失败（区域名不存在 —— 调用方按 `区域不存在` 收场；本函数不再直接中止）。
 * @note    **静态**，只在执行器内用。区域几何：矩形取两角归一（含端点）、圆取外接矩形（cx±r）；停用不影响（这里是「搜索范围」语义，不是判定）；锁纪律同条件步（持 region_lock 取几何、解锁后再换算）。
 */
static int op_vis_region(const char *name, const struct op_vis_frame *fr,
                         int *rx, int *ry, int *rw, int *rh)
{
    int lx, ly, lw, lh;
    int i, found = 0;

    *rx = 0; *ry = 0; *rw = fr->w; *rh = fr->h;              /* 空 = 全屏（帧坐标） */
    if (!name[0]) return 0;
    pthread_mutex_lock(&g.region_lock);                      /* 锁纪律同条件步：持锁取几何、解锁后再换算 */
    for (i = 0; i < g.region_count; i++) {
        const struct region *rg = &g.regions[i];
        if (strcmp(rg->id, name) != 0) continue;
        found = 1;
        if (rg->type == 1) {                                 /* 圆：外接矩形（含端点） */
            lx = rg->a1 - rg->a3; ly = rg->a2 - rg->a3;
            lw = 2 * rg->a3 + 1; lh = 2 * rg->a3 + 1;
        } else {                                             /* 矩形：两角归一（含端点；区域允许任意两角顺序） */
            int x1 = rg->a1, y1 = rg->a2, x2 = rg->a3, y2 = rg->a4;
            lx = x1 < x2 ? x1 : x2; ly = y1 < y2 ? y1 : y2;
            lw = (x1 < x2 ? x2 - x1 : x1 - x2) + 1;
            lh = (y1 < y2 ? y2 - y1 : y1 - y2) + 1;
        }
        break;
    }
    pthread_mutex_unlock(&g.region_lock);
    if (!found) return -1;                                   /* 悬空引用：调用方报 `区域不存在`（同条件步先例） */
    vt_vis_logic_rect_to_frame(fr->rot, fr->w, fr->h, lx, ly, lw, lh, rx, ry, rw, rh);
    return 0;
}

/**
 * (vtouch-doc: op_vis_read_tmpl)
 * @brief 读模板文件（.tmpl，小端）："VTM1" + ver=1 + w/h + rot + res + 灰度。
 * @param   name     模板名（vt_id_ok 尺子；调用方已校验）
 * @param   gray     输出：灰度缓冲（malloc；调用方 free）
 * @param   tw,th    输出：模板尺寸
 * @param   trot     输出：模板抓取方向（0..3）
 * @return  0 成功；-1 文件问题（调用方中止 `模板不存在`）；-2 内存失败（调用方中止 `视觉错`）。
 * @note    **静态**，只在执行器内用。目录 /data/local/vtouch-runtime/templates/（spec §5.1）；格式逐字照实施计划「模板/点集文件格式」（T2.1 读端）。
 */
static int op_vis_read_tmpl(const char *name, uint8_t **gray, int *tw, int *th, int *trot)
{
    char path[128];
    unsigned char hdr[14];
    uint8_t *buf;
    size_t n;
    uint32_t ver;
    int w, h, rot;
    FILE *f;

    snprintf(path, sizeof path, "%s/%s.tmpl", VT_VIS_TMPL_DIR, name);
    f = fopen(path, "rb");
    if (!f) return -1;
    if (fread(hdr, 1, sizeof hdr, f) != sizeof hdr) { fclose(f); return -1; }
    if (hdr[0] != 'V' || hdr[1] != 'T' || hdr[2] != 'M' || hdr[3] != '1') { fclose(f); return -1; }
    ver = (uint32_t)hdr[4] | ((uint32_t)hdr[5] << 8) | ((uint32_t)hdr[6] << 16) | ((uint32_t)hdr[7] << 24);
    if (ver != 1u) { fclose(f); return -1; }
    w = (int)hdr[8] | ((int)hdr[9] << 8);
    h = (int)hdr[10] | ((int)hdr[11] << 8);
    rot = hdr[12];
    /* hdr[13] = res（保留位；本版恒 0，不校验） */
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096 || rot > 3) { fclose(f); return -1; }
    n = (size_t)w * (size_t)h;
    buf = malloc(n);
    if (!buf) { fclose(f); return -2; }                      /* 内存失败与文件问题分开（中止词不同） */
    if (fread(buf, 1, n, f) != n) { free(buf); fclose(f); return -1; }
    fclose(f);
    *gray = buf; *tw = w; *th = h; *trot = rot;
    return 0;
}

/**
 * (vtouch-doc: op_vis_read_pts)
 * @brief 读点集文件（.pts，小端）："VTP1" + ver=1 + n + res + base_rgb + base_tol + n×{dx,dy,rgb,tol}。
 * @param   name     点集名
 * @param   base     输出：基准色
 * @param   base_tol 输出：基准容差（0..255）
 * @param   pts      输出：参考点数组（容量 ≥ VT_VIS_PTS_MAX）
 * @param   n        输出：参考点个数（1..16）
 * @return  0 成功；-1 失败（缺文件 / 格式坏 —— 调用方中止 `模板不存在`）。
 * @note    **静态**，只在执行器内用；n / 容差 / 偏移越界都按格式坏拒收（引擎的域校验兜底相同，这里先拒 = 明确的 `模板不存在`）。
 */
static int op_vis_read_pts(const char *name, uint32_t *base, int *base_tol,
                           struct vt_vis_pt *pts, int *n)
{
    char path[128];
    unsigned char hdr[18];
    FILE *f;
    uint32_t ver, b;
    int cnt, i, bt;

    snprintf(path, sizeof path, "%s/%s.pts", VT_VIS_TMPL_DIR, name);
    f = fopen(path, "rb");
    if (!f) return -1;
    if (fread(hdr, 1, sizeof hdr, f) != sizeof hdr) { fclose(f); return -1; }
    if (hdr[0] != 'V' || hdr[1] != 'T' || hdr[2] != 'P' || hdr[3] != '1') { fclose(f); return -1; }
    ver = (uint32_t)hdr[4] | ((uint32_t)hdr[5] << 8) | ((uint32_t)hdr[6] << 16) | ((uint32_t)hdr[7] << 24);
    if (ver != 1u) { fclose(f); return -1; }
    cnt = (int)hdr[8] | ((int)hdr[9] << 8);
    /* hdr[10..11] = res（保留位；不校验） */
    b = (uint32_t)hdr[12] | ((uint32_t)hdr[13] << 8) | ((uint32_t)hdr[14] << 16) | ((uint32_t)hdr[15] << 24);
    bt = (int)hdr[16] | ((int)hdr[17] << 8);
    if (cnt < 1 || cnt > VT_VIS_PTS_MAX || bt > 255) { fclose(f); return -1; }
    for (i = 0; i < cnt; i++) {
        unsigned char p[10];                                 /* 点记录 = {dx i16, dy i16, rgb u32, tol u16}（10B，定稿） */
        int dx, dy, tol;
        uint32_t rgb;
        if (fread(p, 1, 10, f) != 10) { fclose(f); return -1; }
        dx = (int)((uint16_t)p[0] | ((uint16_t)p[1] << 8));      /* dx i16（小端） */
        if (dx >= 32768) dx -= 65536;
        dy = (int)((uint16_t)p[2] | ((uint16_t)p[3] << 8));
        if (dy >= 32768) dy -= 65536;
        rgb = (uint32_t)p[4] | ((uint32_t)p[5] << 8) | ((uint32_t)p[6] << 16) | ((uint32_t)p[7] << 24);
        tol = (int)p[8] | ((int)p[9] << 8);
        if (tol > 255 || dx < -4096 || dx > 4096 || dy < -4096 || dy > 4096) { fclose(f); return -1; }
        pts[i].dx = dx; pts[i].dy = dy; pts[i].rgb = rgb; pts[i].tol = tol;
    }
    fclose(f);
    *base = b; *base_tol = bt; *n = cnt;
    return 0;
}

/**
 * (vtouch-doc: op_vis_rot_gray)
 * @brief 灰度模板旋转（90° 数组变换；spec VISION §11-#7 模板方向对齐）。
 * @param   src      源灰度（sw×sh 紧凑）
 * @param   sw,sh    源尺寸
 * @param   steps    顺时针步数：1 = 90°、2 = 180°、3 = 270°（调用方保证 1..3）
 * @param   ow,oh    输出：旋转后尺寸
 * @return  新缓冲（malloc；调用方 free）；内存失败 NULL。
 * @note    **静态**，只在执行器内用。旋转方向 = (模板 rot - 帧 rot) & 3 顺时针步（推导：帧→逻辑的映射是纯旋转；模板先回逻辑再进当前帧 —— 两段相消后净旋转 = 两方向差）。
 */
static uint8_t *op_vis_rot_gray(const uint8_t *src, int sw, int sh, int steps, int *ow, int *oh)
{
    uint8_t *dst;
    int x, y;

    if (steps == 2) { *ow = sw; *oh = sh; }
    else { *ow = sh; *oh = sw; }                             /* 90° / 270°：宽高互换 */
    dst = malloc((size_t)*ow * (size_t)*oh);
    if (!dst) return NULL;
    for (y = 0; y < *oh; y++) {
        for (x = 0; x < *ow; x++) {
            int sx, sy;
            if (steps == 1)      { sx = y;             sy = sh - 1 - x; }   /* 顺时针 90° */
            else if (steps == 2) { sx = sw - 1 - x;    sy = sh - 1 - y; }   /* 180° */
            else                 { sx = sw - 1 - y;    sy = x; }            /* 逆时针 90°（= 顺时针 270°） */
            dst[(size_t)y * *ow + x] = src[(size_t)sy * sw + sx];
        }
    }
    return dst;
}

/**
 * (vtouch-doc: vis_exec_find)
 * @brief 查找执行（找图 / 找色；op 视觉步与面板试查共用）：抓帧（或复用）→ 帧视图 → 区域换算 → 匹配；持续模式循环到命中或超时。
 * @param   kind     0 = 找图 / 1 = 找色单点 / 2 = 找色多点
 * @param   ref      模板名（kind 0）/ 点集名（kind 2）；kind 1 忽略
 * @param   region   区域名（空 = 全屏）
 * @param   a1       找图 = 阈值 0..255；其余不用
 * @param   a2       找色单点 = (颜色<<8)|容差；其余不用
 * @param   timeout_ms 持续查找超时毫秒：0 = 单次（现状）；1..60000 = 循环抓帧查到命中或超时（T7.4）
 * @param   x,y      输出：命中点**竖屏逻辑坐标**（仅命中有效）
 * @param   ms       输出：单次 = 匹配耗时毫秒（只计匹配调用，不含抓帧等待 —— 保 op 路径 `耗时` 语义逐字不变）；持续 = 全程耗时（含抓帧等待；可 NULL）
 * @param   attempts 输出：尝试次数（抓帧 + 匹配的轮数；单次 = 1；可 NULL）
 * @param   err      输出：失败原因词（`无画面` / `视觉错` / `区域不存在` / `模板不存在`；命中 / 未命中置 NULL）
 * @return  0 = 命中 / -1 = 未命中 / -2 = 失败（err 已置原因词，调用方中止或按码报错）。
 * @note    **静态**，op 执行器与面板「试一下」共用（op 路径单次行为逐字不变：中止词 / 日志 / 结果槽口径照旧）。持续模式（T7.4）：循环 { 抓新帧（force=1，不复用缓存）→ 匹配 → 命中 break } 到 deadline；硬失败（无画面 / 视觉错 / 区域不存在 / 模板不存在）立即按 -2 收场，不等超时；超时按未命中（-1）返回。持续循环占着主循环：每轮手动 vt_shm_tick 喂核心心跳（面板心跳停滞 ≥3s 会自杀退出；单次 ≤1s 不越线）。链路（spec VISION §6.1）：抓帧失败 → `无画面`；内部错 → `视觉错`；区域名不存在 → `区域不存在`；模板 / 点集读不到 → `模板不存在`。命中点从帧坐标换算成竖屏逻辑坐标（vt_vis_frame_to_logic）；找图先读 .tmpl、方向不同先旋转模板；找色多点先读 .pts（单点 a2 = (颜色<<8)|容差）。
 */
static int vis_exec_find(int kind, const char *ref, const char *region, int a1, int a2, int timeout_ms,
                         int *x, int *y, uint64_t *ms, int *attempts, const char **err)
{
    struct op_vis_frame fr;
    int rx, ry, rw, rh, ox = 0, oy = 0, rc, tries = 0;
    uint64_t t0, dt = 0, t_start;

    *err = NULL;
    if (ms) *ms = 0;
    if (attempts) *attempts = 0;
    if (timeout_ms < 0) timeout_ms = 0;                      /* 防御：域外值按单次（op_valid 已挡；试查恒传 0） */
    t_start = op_now_ms();
    for (;;) {                                               /* 持续（T7.4）：循环抓新帧查到命中或超时；单次跑一轮 */
        tries++;
        /* 持续循环占着主循环：手动喂核心心跳 —— 否则面板 vt_shm_ui_tick 见心跳停滞 ≥3s 判「核心死了」自杀退出
         *（单次路径 ≤1s（抓帧超时）不越线，不动）。每轮一喂：轮间隔 ≤ 抓帧超时 1000ms + 匹配，恒 < 3s。 */
        if (timeout_ms > 0) vt_shm_tick();
        /* 持续 force=1：每轮都要新帧（复用缓存会原地空转同一画面）；单次照旧允许复用（行为逐字不变）。 */
        if (op_vis_capture(&fr, timeout_ms > 0, err) != 0) break;   /* 原因词已置（`无画面` / `视觉错`） */
        if (vt_vis_frame_prepare(fr.rgba, fr.w, fr.h, fr.stride) != VT_VIS_OK) {
            *err = "视觉错";
            break;
        }
        if (op_vis_region(region, &fr, &rx, &ry, &rw, &rh) != 0) {  /* 区域名不存在（空 = 全屏） */
            *err = "区域不存在";
            break;
        }

        if (kind == 0) {                                     /* 找图：ref = 模板名（spec §6.1） */
            uint8_t *gray = NULL;
            int tw = 0, th = 0, trot = 0, steps;

            rc = op_vis_read_tmpl(ref, &gray, &tw, &th, &trot);
            if (rc != 0) { *err = (rc == -2) ? "视觉错" : "模板不存在"; break; }
            steps = (trot - fr.rot) & 3;                     /* 模板 → 当前帧的顺时针步数（spec §11-#7） */
            if (steps) {
                int nw, nh;
                uint8_t *rot = op_vis_rot_gray(gray, tw, th, steps, &nw, &nh);
                free(gray);
                if (!rot) { *err = "视觉错"; break; }
                gray = rot; tw = nw; th = nh;
            }
            t0 = op_now_ms();
            rc = vt_vis_find_image(rx, ry, rw, rh, gray, tw, th, a1, &ox, &oy);
            dt = op_now_ms() - t0;
            free(gray);
        } else if (kind == 2) {                              /* 找色多点：ref = 点集名（spec §6.1） */
            uint32_t base = 0;
            int base_tol = 0, n = 0;
            struct vt_vis_pt pts[VT_VIS_PTS_MAX];
            if (op_vis_read_pts(ref, &base, &base_tol, pts, &n) != 0) {
                *err = "模板不存在";
                break;
            }
            t0 = op_now_ms();
            rc = vt_vis_find_color_multi(rx, ry, rw, rh, base, base_tol, pts, n, &ox, &oy);
            dt = op_now_ms() - t0;
        } else {                                             /* 找色单点：a2 = (颜色<<8)|容差（spec §6.1） */
            t0 = op_now_ms();
            rc = vt_vis_find_color(rx, ry, rw, rh, (uint32_t)a2 >> 8, (int)((uint32_t)a2 & 0xffu), &ox, &oy);
            dt = op_now_ms() - t0;
        }
        if (rc == VT_VIS_BAD) { *err = "视觉错"; break; }    /* 防御：参数域已过门，真到这不硬撑 */
        if (rc == VT_VIS_OK) {                               /* 命中：写坐标 + 返回（持续 = 命中即停） */
            vt_vis_frame_to_logic(fr.rot, fr.w, fr.h, ox, oy, x, y); /* 命中点 → 竖屏逻辑坐标 */
            if (ms) *ms = (timeout_ms > 0) ? op_now_ms() - t_start : dt;   /* 持续：耗时 = 全程（含抓帧等待） */
            if (attempts) *attempts = tries;
            return 0;
        }
        if (timeout_ms <= 0) {                               /* 单次：未命中（不写坐标） */
            if (ms) *ms = dt;
            if (attempts) *attempts = tries;
            return -1;
        }
        if (op_now_ms() - t_start >= (uint64_t)timeout_ms) { /* 持续：到 deadline 仍未见命中 → 未命中（超时） */
            if (ms) *ms = op_now_ms() - t_start;
            if (attempts) *attempts = tries;
            return -1;
        }
    }
    if (attempts) *attempts = tries;                         /* 失败：-2（err 已置原因词） */
    return -2;
}

/**
 * (vtouch-doc: op_vis_run)
 * @brief 视觉步骤执行（找图 / 找色）：转调 vis_exec_find → 结果槽 + 四档分支。
 * @param   st       当前视觉步（type = OP_STEP_FINDIMAGE / OP_STEP_FINDCOLOR）
 * @note    **静态**，只在执行器内用；单拍完成（返回时 phase/deadline 已落，或已中止）。链路（spec §6.1）：失败 → 按 vis_exec_find 的原因词中止（`无画面` / `视觉错` / `区域不存在` / `模板不存在`）；命中 → r1/r2 = 命中点**竖屏逻辑坐标** + 成立侧四档；未命中 → 不成立侧四档（中止词 `未命中`）。ms > 0 = 持续查找（T7.4）：vis_exec_find 循环抓帧查到命中或超时（超时 = 未命中）；日志（spec §8）：`vis 找图 <模板> 命中 x,y (耗时 <ms>)` / `未命中 (耗时 <ms>)`；`vis 找色 命中 x,y` / `未命中`；持续增量：`vis 找图 <模板> 持续 <ms> 命中 x,y（尝试 N 次 / 耗时 M ms）` / `… 未命中（超时 <ms>，尝试 N 次）`（找色同款去模板名）。按住期允许（纯读屏不碰手指，spec §6.2）。
 */
static void op_vis_run(const struct vt_step *st)
{
    int kind, ox = 0, oy = 0, rc, hit, n = 0;
    uint64_t dt = 0;
    const char *err = NULL;

    /* kind：找图 = 0；找色按模式（a1 == 1 = 多点）。字段映射照 spec §6.1（a1 = 阈值 / 模式；a2 = 单点打包色容差）。
     * ms（T7.4）：0 = 单次（现状）；>0 = 持续查找超时 —— vis_exec_find 循环抓帧查到命中或超时。 */
    kind = (st->type == OP_STEP_FINDIMAGE) ? 0 : (st->a1 == 1 ? 2 : 1);
    rc = vis_exec_find(kind, st->ref, st->expr, st->a1, st->a2, st->ms, &ox, &oy, &dt, &n, &err);
    if (rc == -2) { vt_ops_abort(err); return; }             /* `无画面` / `视觉错` / `区域不存在` / `模板不存在` */
    hit = (rc == 0);
    if (hit) {
        R.slots[0] = ox; R.slots[1] = oy; R.slot_mask |= 1u | 2u;      /* r1/r2 = 命中点竖屏逻辑坐标（spec §6.2） */
        if (st->type == OP_STEP_FINDIMAGE) {
            if (st->ms > 0)                                  /* 持续（T7.4）：尝试计数 + 全程耗时 */
                fprintf(stderr, "vtouchd: vis 找图 %s 持续 %d 命中 %d,%d（尝试 %d 次 / 耗时 %llu ms）\n",
                        st->ref, st->ms, ox, oy, n, (unsigned long long)dt);
            else
                fprintf(stderr, "vtouchd: vis 找图 %s 命中 %d,%d (耗时 %llums)\n",
                        st->ref, ox, oy, (unsigned long long)dt);
        } else {
            if (st->ms > 0)
                fprintf(stderr, "vtouchd: vis 找色 持续 %d 命中 %d,%d（尝试 %d 次 / 耗时 %llu ms）\n",
                        st->ms, ox, oy, n, (unsigned long long)dt);
            else
                fprintf(stderr, "vtouchd: vis 找色 命中 %d,%d\n", ox, oy);
        }
    } else {
        if (st->type == OP_STEP_FINDIMAGE) {
            if (st->ms > 0)                                  /* 持续超时：不成立档（未命中） */
                fprintf(stderr, "vtouchd: vis 找图 %s 持续 %d 未命中（超时 %d，尝试 %d 次）\n",
                        st->ref, st->ms, st->ms, n);
            else
                fprintf(stderr, "vtouchd: vis 找图 %s 未命中 (耗时 %llums)\n",
                        st->ref, (unsigned long long)dt);
        } else {
            if (st->ms > 0)
                fprintf(stderr, "vtouchd: vis 找色 持续 %d 未命中（超时 %d，尝试 %d 次）\n",
                        st->ms, st->ms, n);
            else
                fprintf(stderr, "vtouchd: vis 找色 未命中\n");
        }
    }
    op_cond_apply(st, hit, st->type == OP_STEP_FINDIMAGE ? "找图" : "找色",
                  st->type == OP_STEP_FINDIMAGE ? st->ref : NULL, "未命中");   /* 四档：成立/不成立（spec §6.2） */
}

/* ===================== 试查（契约 v9；Task 7.1「试一下」） ===================== */

/* 面板写请求 / 核心写结果，单请求在途（区 D 头 test_* 块；内存序照区 D 口径：
 * 面板参数先写、seq 最后 release；这里 acquire 读 req_seq、写结果后 release res_seq）。 */
static uint32_t S_test_seen;         /* 已处理的请求序号（本地 seen；启动时对齐当前值，跳过陈旧请求） */
static int      S_test_seen_ok;      /* 启动对齐完成（首次调用） */

/**
 * (vtouch-doc: vis_test_err_code)
 * @brief 试查失败原因词 → 结果错误码（词表同 op 中止词；面板按码显示）。
 * @param   why      失败原因词（vis_exec_find 输出；可 NULL = 防御）
 * @return  结果错误码（VT_TEST_ERR_*；未知词按 `视觉错` 兜底）。
 */
static int vis_test_err_code(const char *why)
{
    if (why && !strcmp(why, "无画面")) return VT_TEST_ERR_NOPIC;
    if (why && !strcmp(why, "区域不存在")) return VT_TEST_ERR_REGION;
    if (why && !strcmp(why, "模板不存在")) return VT_TEST_ERR_TMPL;
    return VT_TEST_ERR_VIS;              /* 视觉错 / 未知（防御） */
}

/**
 * (vtouch-doc: vt_ops_test_poll)
 * @brief 主循环每轮调：面板「试一下」请求 → 执行一次查找 → 写结果（x/y/err，release test_res_seq）。
 * @note    **单请求在途**：面板写参数（参数先写、seq 最后 release），这里 acquire 读 test_req_seq 与本地 seen 比对；新请求 → 执行一次 vis_exec_find → 结果写回（命中 0 / 未命中 -1 / 其余 = 错误码，词表同 op 中止词）。**启动时 seen 对齐当前值**（首次调用）：跳过陈旧请求。防御（面板已预检）：kind 必须 0/1/2、找图 / 找色多点 ref 过 vt_id_ok 尺子 —— 真漏进来按词表收场，不硬撑。日志：`vis 试查 找图 <模板> 命中 x,y (耗时 ms)` / `… 未命中` / `… 失败 <原因>`。
 */
void vt_ops_test_poll(void)
{
    struct vt_shm_frame_hdr *h = vt_shm_frame();
    char ref[sizeof h->test_ref + 1];
    char region[sizeof h->test_region + 1];
    char what[40];
    uint32_t req;
    size_t n;
    int kind, a1, a2, x = 0, y = 0, rc;
    uint64_t ms = 0;
    const char *err = NULL;

    if (!h) return;
    req = __atomic_load_n(&h->test_req_seq, __ATOMIC_ACQUIRE);
    if (!S_test_seen_ok) {                                   /* 启动对齐（首次调用）：跳过陈旧请求 */
        S_test_seen_ok = 1;
        S_test_seen = req;
        return;
    }
    if (req == S_test_seen) return;                          /* 没有新请求：热路径零成本 */
    S_test_seen = req;
    /* acquire 之后读参数（面板写序：参数先写、seq 最后 release）—— 拷进本地，防执行期间被下一发改写。 */
    kind = (int)h->test_kind;
    a1 = (int)h->test_a1;
    a2 = (int)h->test_a2;
    n = strnlen(h->test_ref, sizeof h->test_ref);            /* 载荷防御：未终止按数组长截断（同 op_valid 口径） */
    if (n > sizeof h->test_ref) n = sizeof h->test_ref;
    memcpy(ref, h->test_ref, n); ref[n] = 0;
    n = strnlen(h->test_region, sizeof h->test_region);
    if (n > sizeof h->test_region) n = sizeof h->test_region;
    memcpy(region, h->test_region, n); region[n] = 0;

    if (kind == 0) snprintf(what, sizeof what, "找图 %s", ref);
    else if (kind == 2) snprintf(what, sizeof what, "找色 多点 %s", ref);
    else snprintf(what, sizeof what, "找色 单点");

    /* 防御（面板已预检，真漏进来按词表收场，不硬撑）：kind 必须 0/1/2；找图 / 找色多点的 ref 过 vt_id_ok 尺子。 */
    if (kind != 0 && kind != 1 && kind != 2) { err = "视觉错"; rc = -2; }
    else if (kind != 1 && !vt_id_ok(ref, strnlen(ref, sizeof ref))) { err = "模板不存在"; rc = -2; }
    else rc = vis_exec_find(kind, ref, region, a1, a2, 0, &x, &y, &ms, NULL, &err);   /* 试查恒单次（契约 v9 无 ms 字段） */

    if (rc == 0) {
        h->test_x = x; h->test_y = y; h->test_err = VT_TEST_HIT;
        fprintf(stderr, "vtouchd: vis 试查 %s 命中 %d,%d (耗时 %llums)\n", what, x, y, (unsigned long long)ms);
    } else if (rc == -1) {
        h->test_x = 0; h->test_y = 0; h->test_err = VT_TEST_MISS;
        fprintf(stderr, "vtouchd: vis 试查 %s 未命中 (耗时 %llums)\n", what, (unsigned long long)ms);
    } else {
        h->test_x = 0; h->test_y = 0; h->test_err = vis_test_err_code(err);
        fprintf(stderr, "vtouchd: vis 试查 %s 失败 %s\n", what, err ? err : "?");
    }
    __atomic_store_n(&h->test_res_seq, req, __ATOMIC_RELEASE);   /* 结果先写、seq 最后 release（契约） */
}

/* 起一步：按住期门禁（持有中只允许 等待 / 弹起（及条件步 / 跳转 / 计算 / 视觉步）；点按 / 滑动 / 按下 →
 * 中止 `槽占用`）+ 解析本步数值字段（字面值 / 负数编码引用；变量无值 → 中止 `变量无值`、
 * 槽未写 → 中止 `结果无值`）+ 打步日志 + 发这一步的起始动作（点按 / 滑动 / 按下先 down；
 * 弹起 up；等待 / 条件步 / 跳转 / 计算 / 视觉步不动手）。 */
static void op_begin_step(void)
{
    const struct vt_step *st;
    if (R.step >= R.nsteps) { op_finish(); return; }
    st = &R.steps[R.step];
    g.op_run_step = R.step;                                  /* 面板进度（0 起） */
    /* 按住期门禁（spec §2.2/D4 + V5 §5.2 + VISION §6.2）：持有中只允许 等待 / 弹起（及条件步 / 跳转 /
     * 计算 / 视觉步——视觉步纯读屏不碰手指）——点按 / 滑动 / 按下 都会另起一根手指，统一在步入口中止
     * `槽占用`（判定不逐 case 散落）。 */
    if (R.held && (st->type == OP_STEP_TAP || st->type == OP_STEP_SWIPE || st->type == OP_STEP_DOWN)) {
        vt_ops_abort("槽占用");
        return;
    }
    switch (st->type) {
    case OP_STEP_TAP: {
        int x, y, ms;
        if (op_resolve(st->a1, 0, g.logical_width - 1, &x) != 0 ||
            op_resolve(st->a2, 0, g.logical_height - 1, &y) != 0 ||
            op_resolve(st->ms, 0, 60000, &ms) != 0)
            return;                                          /* 失败已中止（变量无值 / 结果无值；不静默当 0，spec §1.3/D6） */
        fprintf(stderr, "vtouchd: op 步 %d/%d 点按 %d,%d 按住%dms\n",
                R.step + 1, R.nsteps, x, y, ms);
        R.hold = ms;
        if (op_finger_ll("down", x, y) != 0) { op_conflict_abort(); return; }
        R.phase = PH_TAP_UP;
        R.deadline = R.t0 + (uint64_t)R.hold;
        break;
    }
    case OP_STEP_SWIPE: {
        int x1, y1, x2, y2, ms;
        if (op_resolve(st->a1, 0, g.logical_width - 1, &x1) != 0 ||
            op_resolve(st->a2, 0, g.logical_height - 1, &y1) != 0 ||
            op_resolve(st->a3, 0, g.logical_width - 1, &x2) != 0 ||
            op_resolve(st->a4, 0, g.logical_height - 1, &y2) != 0 ||
            op_resolve(st->ms, 1, 60000, &ms) != 0)
            return;                                          /* 失败已中止（变量无值 / 结果无值）；滑动 ms 域 ≥1（spec §4） */
        fprintf(stderr, "vtouchd: op 步 %d/%d 滑动 %d,%d→%d,%d %dms\n",
                R.step + 1, R.nsteps, x1, y1, x2, y2, ms);
        R.sx1 = x1; R.sy1 = y1; R.sx2 = x2; R.sy2 = y2;      /* 采样点插值用解析结果（负数编码已展开） */
        R.dur = ms;
        R.nsamp = ms / 10;                                   /* N = max(2, dur/10)：10ms 一采样 */
        if (R.nsamp < 2) R.nsamp = 2;
        if (op_finger_ll("down", x1, y1) != 0) { op_conflict_abort(); return; }
        R.sample = 1;
        R.phase = PH_SWIPE_MOVE;
        R.deadline = R.t0 + (uint64_t)R.sample * (uint64_t)R.dur / (uint64_t)R.nsamp;
        break;
    }
    case OP_STEP_WAIT: {
        int ms;
        if (op_resolve(st->ms, 0, 600000, &ms) != 0)
            return;                                          /* 失败已中止（变量无值 / 结果无值） */
        fprintf(stderr, "vtouchd: op 步 %d/%d 等待 %dms\n", R.step + 1, R.nsteps, ms);
        R.phase = PH_WAIT;
        R.deadline = R.t0 + (uint64_t)ms;
        break;
    }
    case OP_STEP_DOWN: {                                     /* 按下：a1,a2 = 坐标（可变量），按下并保持（spec §2.2） */
        int x, y;
        if (op_resolve(st->a1, 0, g.logical_width - 1, &x) != 0 ||
            op_resolve(st->a2, 0, g.logical_height - 1, &y) != 0)
            return;                                          /* 失败已中止（变量无值 / 结果无值；不静默当 0，spec §1.3/D6） */
        fprintf(stderr, "vtouchd: op 步 %d/%d 按下 %d,%d\n", R.step + 1, R.nsteps, x, y);
        if (op_finger_ll("down", x, y) != 0) { op_conflict_abort(); return; }
        R.held = 1;                                          /* 持有态：弹起步 / 收尾释放负责清（spec §2.2） */
        R.phase = PH_WAIT;                                   /* 单拍动作：本步到此为止，下一拍进下一步 */
        R.deadline = R.t0;
        break;
    }
    case OP_STEP_UP: {                                       /* 弹起：松开当前按住的手指（无字段） */
        if (!R.held) {
            vt_ops_abort("未按下");                           /* 没有按住的手指：安全侧中止（spec §2.2） */
            return;
        }
        fprintf(stderr, "vtouchd: op 步 %d/%d 弹起\n", R.step + 1, R.nsteps);
        if (op_finger_raw("up", R.rx, R.ry) != 0) { op_conflict_abort(); return; }   /* 抬指（按住那一点） */
        R.held = 0;
        R.phase = PH_WAIT;
        R.deadline = R.t0;
        break;
    }
    case OP_STEP_COND_REGION: {                              /* 区域判断：a1,a2 的点 ∈ ref 区域？（spec §3.1） */
        int x, y, k, found = 0, hit = 0;
        if (op_resolve(st->a1, 0, g.logical_width - 1, &x) != 0 ||
            op_resolve(st->a2, 0, g.logical_height - 1, &y) != 0)
            return;                                          /* 失败已中止（变量无值 / 结果无值；不静默当 0，spec §1.3/D6） */
        fprintf(stderr, "vtouchd: op 步 %d/%d 区域判断 %s %d,%d\n",
                R.step + 1, R.nsteps, st->ref, x, y);
        pthread_mutex_lock(&g.region_lock);                  /* 锁纪律（spec §3.2）：持锁判定、解锁后记日志（锁内不 I/O） */
        for (k = 0; k < g.region_count; k++) {
            if (strcmp(g.regions[k].id, st->ref) != 0) continue;
            found = 1;
            hit = region_hit(&g.regions[k], x, y);           /* 平坦函数复用：停用 = 不命中（enabled 守卫在它里面，spec §3.1） */
            break;
        }
        pthread_mutex_unlock(&g.region_lock);
        if (!found) { vt_ops_abort("区域不存在"); return; }   /* 悬空引用：运行时报（编辑期允许，spec §3.1） */
        op_cond_apply(st, hit, "区域判断", st->ref, "条件不成立");   /* 两侧四档统一收口（spec §1.2；锁外记日志 / 收场） */
        break;
    }
    case OP_STEP_COND_TOGGLE: {                              /* 开关判断：ref 区域须开关型且开着（spec §3.2） */
        int k, found = 0, is_toggle = 0, on = 0;
        fprintf(stderr, "vtouchd: op 步 %d/%d 开关判断 %s\n", R.step + 1, R.nsteps, st->ref);
        pthread_mutex_lock(&g.region_lock);                  /* 锁纪律同门控检查：持锁判定、解锁后记日志（锁内不 I/O） */
        for (k = 0; k < g.region_count; k++) {
            if (strcmp(g.regions[k].id, st->ref) != 0) continue;
            found = 1;
            is_toggle = (g.regions[k].kind == 1);
            on = (g.regions[k].toggle_on == 1);
            break;
        }
        pthread_mutex_unlock(&g.region_lock);
        if (!found) { vt_ops_abort("区域不存在"); return; }   /* 悬空引用：运行时报（编辑期允许，spec §3.1） */
        if (!is_toggle) { vt_ops_abort("非开关型"); return; } /* 运行时校验（编辑期不查 kind，spec §3.2） */
        op_cond_apply(st, on, "开关判断", st->ref, "条件不成立");    /* 两侧四档统一收口（spec §1.2；锁外记日志 / 收场） */
        break;
    }
    case OP_STEP_JUMP: {                                     /* 跳转：a1 = 目标（0 = 结束、1..步数 = 目标）；spec §2.2 */
        int target = st->a1;                                 /* 目标不是变量字段（op_valid 只收字面值）：直读 */
        if (target == 0)
            fprintf(stderr, "vtouchd: op 步 %d/%d 跳转 结束\n", R.step + 1, R.nsteps);
        else
            fprintf(stderr, "vtouchd: op 步 %d/%d 跳转 第 %d 步\n", R.step + 1, R.nsteps, target);
        op_jump_apply(target);                               /* 0 = 结束（op_finish）/ 否则守卫 + 落位（spec §2.3） */
        break;
    }
    case OP_STEP_CALC: {                                     /* 计算（v5）：结果槽 rN = 表达式（spec §5.2） */
        const int vals[OP_VAR_N] = { R.trig.dx, R.trig.dy, R.trig.ux, R.trig.uy, R.trig.ms };
        double out;
        int v, rc = vt_expr_eval(st->expr, vals, R.trig.mask, R.slots, R.slot_mask, &out);
        if (rc == VT_EXPR_NO_VAR)  { vt_ops_abort("变量无值"); return; }   /* 触发数据无值（沿用 v2 词；spec §5.2） */
        if (rc == VT_EXPR_NO_SLOT) { vt_ops_abort("结果无值"); return; }   /* 槽未写（v5 新词；spec §5.2） */
        if (rc != VT_EXPR_OK)      { vt_ops_abort("表达式错"); return; }   /* 防御：除零/域错/非有限——编辑期已拦（spec §5.2） */
        R.slots[st->a1 - 1] = out;                           /* 写槽（覆盖；spec §5.2） */
        R.slot_mask |= 1u << (st->a1 - 1);
        {
            double dv = out;                                 /* 日志值 = 取整显示（spec §7） */
            if (dv < (double)INT_MIN) dv = INT_MIN;          /* 先夹 int 域再取整：llround 超范围行为未指定（spec §5.3） */
            else if (dv > (double)INT_MAX) dv = INT_MAX;
            v = (int)llround(dv);
        }
        fprintf(stderr, "vtouchd: op 步 %d/%d 计算 r%d = %d\n", R.step + 1, R.nsteps, st->a1, v);
        if (op_trace_on())                                   /* TRACE：回显表达式原文（spec §7；L9 默认零输出） */
            fprintf(stderr, "vtouchd: op 计算 r%d = %s = %d\n", st->a1, st->expr, v);
        R.phase = PH_WAIT;                                   /* 单拍动作：本步到此为止，下一拍进下一步 */
        R.deadline = R.t0;
        break;
    }
    case OP_STEP_FINDIMAGE:                                  /* 找图（v8；T7.4：ms>0 持续=循环抓帧到命中/超时）：单拍（内部含抓帧等待；spec VISION §6.1） */
    case OP_STEP_FINDCOLOR:                                  /* 找色（同款） */
        op_vis_run(st);                                      /* 命中/未命中 → 槽 + 四档；失败已按码中止 */
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
        lx = op_lerp(R.sx1, R.sx2, R.sample, R.nsamp);       /* 起终点 = 起一步解析后的快照值 */
        ly = op_lerp(R.sy1, R.sy2, R.sample, R.nsamp);
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
 * 名字/触发数据读取与生产者写入之间有个纳秒级窗口：混读最多让这一发起跑查无此名（丢弃 + 日志），
 * 不会跑去动别的状态 —— 触发是物理手指级别的低频事件，值不当 seqlock。 */
static void op_consume_trigger(void)
{
    uint32_t seq = __atomic_load_n(&g.op_trig_seq, __ATOMIC_ACQUIRE);
    uint32_t delta, k;
    char name[OP_NAME_MAX + 1];
    size_t n;
    struct vt_trig_data td;
    if (seq == R.trig_seen) return;                          /* 没有新触发：热路径零成本 */
    delta = seq - R.trig_seen;
    R.trig_seen = seq;
    /* 生产者先写 name/slot/触发数据、release 才自增 seq ⇒ acquire 看到新 seq 就一定能看到它们 */
    n = strnlen(g.op_trig_name, OP_NAME_MAX + 1);
    if (n > OP_NAME_MAX) n = OP_NAME_MAX;
    memcpy(name, g.op_trig_name, n); name[n] = 0;
    /* 触发数据（v2）：acquire 之后一次读全 6 个字段（写序由 vt_ops_trigger_post 保证） */
    td.mask = g.op_trig_mask;
    td.dx = g.op_trig_dx;
    td.dy = g.op_trig_dy;
    td.ux = g.op_trig_ux;
    td.uy = g.op_trig_uy;
    td.ms = g.op_trig_ms;
    for (k = 1; k < delta; k++) fprintf(stderr, "vtouchd: op 丢弃 覆盖\n");
    if (op_trace_on())                                       /* L9：默认零输出（判定只一次分支） */
        fprintf(stderr, "vtouchd: op 触发 %s 槽=%d\n", name, g.op_trig_slot);
    vt_ops_run(name, &td);                                   /* 忙 / 不存在 / 没空闲槽由它丢弃 + 日志 */
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
 * @param   td       触发数据快照（mask/dx/dy/ux/uy/ms；spec §1.5）；NULL = 手动运行（全零 ⇒ 全部变量无值）
 * @note    起跑 = 整条快照进执行器私有内存（运行中改表 / 删表不影响本次，spec §7）+ 触发数据拷进 R.trig（起跑瞬间快照、运行中不回填，spec §1.2）+ 结果槽清零（值 + 未写标记，spec V5 §5.1）+ 挑第一个空闲虚拟槽（virt[]/staged[] 都空）全程占用，并写 g.op_run / op_run_step / op_run_state 供面板回显；日志 `op 启动 <名> 步数=N 槽=K 门控=<r1|无>`；VTOUCH_OPS_TRACE=1 时再一行 `op 变量 tdx=… tdy=… tux=… tuy=… tms=…`（未设字段打 `-`，值取快照）。门控 / 自动关（spec §4.3）：gate 非空先过门控检查 —— 区域须存在、是开关型且开着，否则 `op 丢弃 门控拦截`；auto_off=1 在正常完成时把门控开关翻回关（中止不翻）。
 *
 * 为什么这么写（原有注释，逐字保留）：
 *   起跑的三件事（顺序有讲究）：
 *   ① 忙就先丢（一次只跑一条，spec §3.2）—— 忙检查放最前：「运行中」本身就是明确的拒绝理由；
 *   ② 查表命中后整条**快照**进 R（步骤表 + 触发数据一起抄）：运行中面板改表 / 删表、再触发都影响不到本次（spec §7）；
 *   ③ 挑槽同时看 virt[] / staged[] / pending_up —— 帧内暂存与待抬的触点都还占着槽（down 会被
 *   set_virtual 拒），全空才真的空闲；挑不到就丢弃（不排队、不等待）。
 *   门控（gate / auto_off）批 3 起生效：gate 非空先过门控检查 —— 锁内判定（必须存在且
 *   kind==toggle 且 toggle_on==1，否则 `op 丢弃 门控拦截`），锁外记日志；检查插在②与③之间：
 *   先问「能不能跑」，再问「有没有槽」。auto_off 只在正常完成（op_finish）翻回门控开关，
 *   中止不翻（中止≠跑完）；翻回的锁纪律与 T3.1 §4.2 同款（锁内改值、锁外环行/日志）。
 */
void vt_ops_run(const char *name, const struct vt_trig_data *td)
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
    R.trig = td ? *td : (struct vt_trig_data){0};            /* 触发数据快照：NULL = 手动运行（全零 ⇒ 全部变量无值） */
    R.slot = slot;
    R.step = 0;
    R.conflict = 0;
    R.jumps = 0;                                             /* 跳转计数清零：条件跳转 + 跳转步共用一枚，每次起跑重新计（spec §2.3） */
    R.frozen = 0;                                            /* 冻结态清零：绝不泄漏进新一次运行（R1 封口） */
    R.stop_pending = 0;                                      /* 推迟账清零：绝不泄漏进新一次运行（R2a 封口） */
    R.held = 0;                                              /* 持有态清零：绝不泄漏进新一次运行（收尾兜底已释放；这里防御） */
    memset(R.slots, 0, sizeof R.slots);                      /* 结果槽起跑清零（值 + 未写标记；spec V5 §5.1） */
    R.slot_mask = 0;
    R.hold = R.dur = R.nsamp = R.sample = 0;
    R.sx1 = R.sy1 = R.sx2 = R.sy2 = 0;
    R.t_start = op_now_ms();
    R.t0 = R.t_start;
    R.deadline = R.t0;                                       /* 到点：本拍末尾就进入第 1 步 */
    R.phase = PH_BEGIN;
    g.op_run = R.idx;
    g.op_run_step = 0;
    g.op_run_state = 1;
    fprintf(stderr, "vtouchd: op 启动 %s 步数=%d 槽=%d 门控=%s\n",
            R.name, R.nsteps, R.slot, R.gate[0] ? R.gate : "无");
    if (op_trace_on()) {                                     /* TRACE：起跑一行变量快照（未设打 `-`；L9 默认零输出） */
        const int val[OP_VAR_N] = { R.trig.dx, R.trig.dy, R.trig.ux, R.trig.uy, R.trig.ms };
        char b[OP_VAR_N][16];
        int k;
        for (k = 0; k < OP_VAR_N; k++) {
            if (R.trig.mask & (OP_TRIGB_TDX << k)) snprintf(b[k], sizeof b[k], "%d", val[k]);
            else snprintf(b[k], sizeof b[k], "-");
        }
        fprintf(stderr, "vtouchd: op 变量 tdx=%s tdy=%s tux=%s tuy=%s tms=%s\n",
                b[0], b[1], b[2], b[3], b[4]);
    }
    op_ev_push("run", 1, R.nsteps);
}

/**
 * (vtouch-doc: vt_ops_abort)
 * @brief 中止运行中的操作（抬指 + 状态归位 + 日志原因）。
 * @param   why      中止原因（写进日志；如 停止按钮 / 引擎收尾 / reset/断连）
 * @note    **幂等**：没在跑（含没初始化、init 失败路径的 cleanup）就是空操作 —— cleanup() 无条件调它。抬指帧走 emit_frame：写失败置 g_reemit 等主循环重发；进程退出路径由随后 uinput 销毁兜底（触点随设备消失，物理触摸回系统）。按下步还按着（held）时收尾同样补一笔 up + `op 收尾 松开`（spec §2.2，走 op_release_held）。
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
    op_release_held();                                       /* 收尾兜底（spec §2.2）：中止时若还按着 → 同样释放 + `op 收尾 松开` */
    fprintf(stderr, "vtouchd: op 中止 %s 步 %d/%d 原因=%s\n", R.name, i, R.nsteps, why ? why : "?");
    op_ev_push("abort", i, R.nsteps);
    R.active = 0;
    R.slot = -1;
    R.frozen = 0;                                            /* 冻结态清零：绝不泄漏到下一次运行（R1 封口） */
    R.stop_pending = 0;                                      /* 推迟账清零：绝不泄漏到下一次运行（R2a 封口） */
    R.held = 0;                                              /* 持有态清零：收尾已释放（上面），绝不泄漏到下一次运行 */
    g.op_run = -1;
    g.op_run_step = 0;
    g.op_run_state = 0;
}

/**
 * (vtouch-doc: vt_ops_trigger_post)
 * @brief 区域线程投一次触发（写触发槽 → release 自增 seq → 写唤醒 fd）。
 * @param   name     要起跑的操作名
 * @param   slot     触发来源手指的物理槽号（日志用）
 * @param   td       触发数据（mask/dx/dy/ux/uy/ms；spec §1.5）：先写各字段、最后 release 自增 seq 发布
 * @note    触发槽是**单槽覆盖**：主线程还没消费就被下一发盖掉时，tick 按 seq 差值记 `op 丢弃 覆盖`。写出次序：name/slot/触发数据全部先写、seq 最后 release 自增（消费端 acquire 读全）；名字按上限截断写；唤醒 fd 没建成（-1）时只丢这次唤醒 —— seq 还在，≤1s 的兜底 poll 会捡起。
 *
 * 为什么这么写（原有注释，逐字保留）：
 *   链路：写 name/slot/触发数据（普通写）→ release 自增 seq（这一下之后名字与数据才算「可见」）→ 写唤醒 fd。
 *   主线程 acquire 读 seq，变了才取名字/数据 —— 单槽覆盖，中间被盖掉的由 seq 差值看出来。
 *   fd 没建成（-1）也不丢功能：seq 已经涨了，兜底 poll（≤1s）会自己来读。
 */
void vt_ops_trigger_post(const char *name, int slot, const struct vt_trig_data *td)
{
    size_t n = name ? strnlen(name, OP_NAME_MAX + 1) : 0;
    if (n > OP_NAME_MAX) n = OP_NAME_MAX;
    if (n) memcpy(g.op_trig_name, name, n);
    g.op_trig_name[n] = 0;
    g.op_trig_slot = slot;
    /* 触发数据（v2，spec §1.5）：先写全 6 个字段（普通写）—— 主线程 acquire 读到新 seq 后一次读全。 */
    g.op_trig_mask = td->mask;
    g.op_trig_dx = td->dx;
    g.op_trig_dy = td->dy;
    g.op_trig_ux = td->ux;
    g.op_trig_uy = td->uy;
    g.op_trig_ms = td->ms;
    __atomic_add_fetch(&g.op_trig_seq, 1u, __ATOMIC_RELEASE);   /* 最后 release：上面各字段先可见 */
    if (g.ops_wake_fd >= 0) {
        uint64_t one = 1;
        ssize_t r;
        do { r = write(g.ops_wake_fd, &one, sizeof one); } while (r < 0 && errno == EINTR);
        (void)r;                     /* 非阻塞：写满（几乎到不了）只丢这次唤醒，seq 还在、兜底会捡 */
    }
}

#endif /* VT_UI */
