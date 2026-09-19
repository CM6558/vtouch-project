/* vt_region.c（§5+§6 区域表与区域线程） —— 模块地图见 vt_internal.h；私有状态就近放 static，共享状态走 g。 */
#include "vt_internal.h"

/* 区域线程私有状态（§4.4：主线程不再持有区域状态）*/
static unsigned region_gen;
static unsigned r_seen_gen;
static unsigned char r_slot_in[MAX_PHYS][MAX_REGIONS];
static unsigned char r_slot_hit[MAX_PHYS][MAX_REGIONS];
/* move 去重基准按 [slot][region] 分开存：多个区域重叠时，同一个 move 要给每个命中的区域各报一条 */
static int r_slot_last_x[MAX_PHYS][MAX_REGIONS], r_slot_last_y[MAX_PHYS][MAX_REGIONS];
/* 区域几何的宽松量程（见 region_add 里的说明）：面板「视口坐标不变」语义下，区域在某方向落屏外时
 * 竖屏坐标就是负数/超界 —— 合法；这里只挡住会让 dx*dx+dy*dy 溢出的离谱值。 */
#define VT_REGION_COORD_MAX 4096
/**
 * (vtouch-doc: regions_clear)
 * @brief 清空区域表，并把代次 +1（让区域线程重置它私有的状态表）。
 */
void regions_clear(void)
{
    pthread_mutex_lock(&g.region_lock);
    g.region_count = 0;
    memset(g.regions, 0, sizeof g.regions);
    region_gen++;                    /* 区域线程看到代次变化会自己清私有状态 */
    pthread_mutex_unlock(&g.region_lock);
}
/* 区域 id 合法性：字符集 [A-Za-z0-9_-]、长度 1..REGION_ID_MAX。
 * 与面板 id_name_ok（src-ui/vtouch_ui.cpp）的规则一致 —— 核心是**单点校验**：脚本经 WS 推的 id
 * 与面板经共享内存邮箱推的 id 都从这里过。核心放行而面板字形表里没有的字符（CJK / `!` 之流）
 * 只会被画成方框，所以这道门必须守在核心侧。
 */
static int id_ok(const char *id, size_t n)
{
    size_t i;
    if (n < 1 || n > REGION_ID_MAX) return 0;
    for (i = 0; i < n; i++) {
        char ch = id[i];
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') || ch == '_' || ch == '-')) return 0;
    }
    return 1;
}
/**
 * (vtouch-doc: region_add)
 * @brief 新增或覆盖一个区域（主线程持 region_lock 写表）。
 * @param   id       区域名（≤ REGION_ID_MAX 字符）
 * @param   type     0=矩形 1=圆
 * @param   a1       矩形 x1 / 圆 cx
 * @param   a2       矩形 y1 / 圆 cy
 * @param   a3       矩形 x2 / 圆 r
 * @param   a4       矩形 y2
 * @param   enabled  1 启用 0 禁用
 * @return  0 成功；-1 参数非法、id 去重失败或表满。
 * @note    几何只做宽松量程检查（±4096，防判定里 dx*dx 溢出）：区域跟着屏幕方向走时可以落在屏外，此时竖屏坐标允许负数/超界（面板「视口坐标不变」语义）；越界区域在核心侧天然不可命中（手指原生坐标恒在框内），除非半径探进可见区。
 */
int region_add(const char *id, int type, int a1, int a2, int a3, int a4, int enabled)
{
    struct region *rg;
    size_t n;
    int i, rc = 0;
    if (!id) return -1;
    n = strlen(id);
    /* 单点校验（见 id_ok）：字符集 [A-Za-z0-9_-] + 长度 1..REGION_ID_MAX ⇒ 非法一律 -1，
     * WS 侧据此自动回 err region（cmd_region 不用改）。 */
    if (!id_ok(id, n)) return -1;
    if (type != 0 && type != 1) return -1;
    /* 几何范围：**不再要求落在竖屏框内、也不再要求非负**（2026-09-18）。面板的「跟随屏幕方向」语义是
     * 「视口坐标不变」（以当前方向左上角为原点的坐标原样保留），区域在某个方向下可以落在屏外 ——
     * 用户口径：**屏外允许、屏幕自己裁就行**。这种点换算成竖屏坐标就是负数或超出逻辑尺寸，属合法状态。
     * 这里只留一个宽松量程（±4096），防的是判定里 dx*dx + dy*dy 溢出。
     * 顺带：越界区域在核心侧**天然不可命中** —— 手指原生坐标恒在 [0,W)×[0,H)，够不到框外的圆心
     * （除非圆的半径探进可见区，那正是「部分可见就部分可命中」想要的行为）。 */
    if (a1 < -VT_REGION_COORD_MAX || a1 > VT_REGION_COORD_MAX ||
        a2 < -VT_REGION_COORD_MAX || a2 > VT_REGION_COORD_MAX ||
        a3 < -VT_REGION_COORD_MAX || a3 > VT_REGION_COORD_MAX ||
        a4 < -VT_REGION_COORD_MAX || a4 > VT_REGION_COORD_MAX) return -1;
    pthread_mutex_lock(&g.region_lock);
    /* 同 id 查重：存在则原地更新（开关/挪区域只改属性，不新增） */
    for (i = 0; i < g.region_count; i++) {
        if (strcmp(g.regions[i].id, id) == 0) {
            rg = &g.regions[i];
            rg->type = type;
            rg->enabled = enabled ? 1 : 0;
            rg->a1 = a1; rg->a2 = a2; rg->a3 = a3; rg->a4 = a4;
            /* 原地更新（同 id、同表位）**不动 region_gen**：代次一变，区域线程会把四张私有状态表
             * 整表清零 —— 按下进行中的 slot_hit/slot_in 一起没了，手指抬起时判不出 up。
             * 触发场景（都是非人为的内部写）：面板拖改/重启回灌 regions.conf、区域跟随旋转的整表重算、
             * 脚本重连时重放自己那批区域（同名 = 走这条更新分支）—— 正好落在某次按住期间就丢 up。
             * 索引没移动 ⇒ 私有状态无需失效。只有结构变化（regions_clear / region_del 的移位）才 bump。 */
            fprintf(stderr, "vtouchd: region upd %s type%d %d,%d,%d,%d en%d (total %d)\n",
                    rg->id, type, a1, a2, a3, a4, rg->enabled, g.region_count);
            pthread_mutex_unlock(&g.region_lock);
            return 0;
        }
    }
    if (g.region_count >= MAX_REGIONS) rc = -1;
    else {
        rg = &g.regions[g.region_count++];
        memset(rg, 0, sizeof *rg);
        memcpy(rg->id, id, n);
        rg->type = type;
        rg->enabled = enabled ? 1 : 0;
        rg->a1 = a1; rg->a2 = a2; rg->a3 = a3; rg->a4 = a4;
        /* 追加分支同样不用 bump：新区域占的是**新表位**，已有 rid 的私有状态不受影响；
         * 而表位复用（del/clear 之后再加）必定先经过那两处的 bump + 整表清零。 */
        fprintf(stderr, "vtouchd: region add %s type%d %d,%d,%d,%d en%d (total %d)\n",
                rg->id, type, a1, a2, a3, a4, rg->enabled, g.region_count);
    }
    pthread_mutex_unlock(&g.region_lock);
    return rc;
}
#ifdef VT_UI
/**
 * (vtouch-doc: region_del)
 * @brief 按 id 删除区域，并把代次 +1（区域线程据此重置私有状态）。
 * @param   id       区域名
 * @return  0 成功；-1 没找到 / 参数非法。
 * @note    面板删除区域走这里 —— region_gen 是本文件静态，面板直接改表碰不到它，
 *          区域线程会拿过期私有状态（见 vt_region.c 顶部 region_gen 的用法）。
 */
int region_del(const char *id)
{
    int i, k;
    if (!id) return -1;
    pthread_mutex_lock(&g.region_lock);
    for (i = 0; i < g.region_count; i++) {
        if (strcmp(g.regions[i].id, id) != 0) continue;
        for (k = i; k + 1 < g.region_count; k++) g.regions[k] = g.regions[k + 1];
        g.region_count--;
        memset(&g.regions[g.region_count], 0, sizeof g.regions[0]);
        region_gen++;
        fprintf(stderr, "vtouchd: region del %s (total %d)\n", id, g.region_count);
        pthread_mutex_unlock(&g.region_lock);
        return 0;
    }
    pthread_mutex_unlock(&g.region_lock);
    return -1;
}
/**
 * (vtouch-doc: region_rename)
 * @brief 区域改名（目标 id 已被别的区域占用则失败），代次 +1。
 * @param   old_id   原区域名
 * @param   new_id   新区域名（≤ REGION_ID_MAX）
 * @return  0 成功；-1 找不到 / 重名 / 参数非法。
 */
int region_rename(const char *old_id, const char *new_id)
{
    size_t n;
    int i;
    if (!old_id || !new_id) return -1;
    n = strlen(new_id);
    /* 同一把尺子（见 id_ok）：字符集/长度非法直接拒。
     * 「改成同名」不走重名分支 —— 先比 old/new 再查重的顺序在下面，原样保留。 */
    if (!id_ok(new_id, n)) return -1;
    pthread_mutex_lock(&g.region_lock);
    if (strcmp(old_id, new_id) != 0) {
        for (i = 0; i < g.region_count; i++) {
            if (strcmp(g.regions[i].id, new_id) == 0) {   /* 目标 id 已被占 */
                pthread_mutex_unlock(&g.region_lock);
                return -1;
            }
        }
    }
    for (i = 0; i < g.region_count; i++) {
        if (strcmp(g.regions[i].id, old_id) != 0) continue;
        memset(g.regions[i].id, 0, sizeof g.regions[i].id);
        memcpy(g.regions[i].id, new_id, n);
        region_gen++;
        fprintf(stderr, "vtouchd: region rename %s -> %s\n", old_id, new_id);
        pthread_mutex_unlock(&g.region_lock);
        return 0;
    }
    pthread_mutex_unlock(&g.region_lock);
    return -1;
}
#endif /* VT_UI */

/**
 * (vtouch-doc: region_hit)
 * @brief 点是否落在区域内（矩形含边界；圆按半径平方比较）。
 * @param   rg       区域
 * @param   lx       逻辑 x
 * @param   ly       逻辑 y
 * @return  1 命中；0 未命中。
 */
int region_hit(const struct region *rg, int lx, int ly)
{
    if (!rg->enabled) return 0;
    if (rg->type == 1) {
        int dx = lx - rg->a1, dy = ly - rg->a2;
        return dx * dx + dy * dy <= rg->a3 * rg->a3;
    }
    return lx >= rg->a1 && lx <= rg->a3 && ly >= rg->a2 && ly <= rg->a4;
}
/**
 * (vtouch-doc: vt_subev_bit)
 * @brief 事件名 → SUBEV_* 位（订阅过滤器共用；未知名字返回 0）。
 * @param   ev       事件名：down/enter/move/exit/up
 * @return  对应位；未知名字 0。
 * @note    vt_region.c 推送时判、vt_ws.c 解析 sub 命令时用。
 */
unsigned vt_subev_bit(const char *ev)
{
    if (!strcmp(ev, "down"))  return SUBEV_DOWN;
    if (!strcmp(ev, "enter")) return SUBEV_ENTER;
    if (!strcmp(ev, "move"))  return SUBEV_MOVE;
    if (!strcmp(ev, "exit"))  return SUBEV_EXIT;
    if (!strcmp(ev, "up"))    return SUBEV_UP;
    if (!strcmp(ev, "ts"))    return SUBEV_TS;     /* 伪事件：显式要时间戳（本来就是默认） */
    if (!strcmp(ev, "nots"))  return SUBEV_NOTS;   /* 伪事件：显式不要时间戳 */
    return 0;
}
static int subev_want(unsigned mask, const char *ev)
{
    unsigned b;
    if (mask == 0) return 1;
    b = vt_subev_bit(ev);
    return (b != 0 && (mask & b)) ? 1 : 0;
}
/**
 * (vtouch-doc: region_ev_send)
 * @brief 发一条区域事件：订了 region 通道才入出站队列，没订就只打 (UNSUB) 日志。
 * @param   id       区域名
 * @param   ev       down/enter/move/exit/up
 * @param   slot     物理槽号
 * @param   lx       逻辑 x
 * @param   ly       逻辑 y
 * @param   ts_mono  事件时间戳（单调钟纳秒；发出去时换算成墙钟毫秒）
 * @note    低频事件；只报物理手指。报文末尾带 <ms>：事件发生的墙钟毫秒（与脚本 Date.now() 同基准），由 ts_mono 换算而来 —— 脚本算按压时长/防抖/看延迟用它。
 */
void region_ev_send(const char *id, const char *ev, int slot, int lx, int ly, uint64_t ts_mono)
{
    char msg[128];
    /* 末尾这个 <ms> 是**事件发生的墙钟毫秒**（与脚本的 Date.now() 同基准），
     * 由事件自带的单调时间戳换算而来 —— 用它算按压时长/做防抖/看延迟都够。 */
    int n;
    /* 线路格式（2026-09-18「不必要的数据不传」）：
     *   只订了一个区域 ⇒ 不重复发 id（SDK 侧自己知道订的是谁）；
     *   时间戳按需（默认不发）—— 脚本侧少一次 parseInt + 十来个字符。 */
    if (g.sub_region_id[0])
        n = snprintf(msg, sizeof msg, "region_ev %s %d %d %d", ev, slot, lx, ly);
    else
        n = snprintf(msg, sizeof msg, "region_ev %s %s %d %d %d", id, ev, slot, lx, ly);
    if (n > 0 && (size_t)n < sizeof msg && g.sub_region_ts)
        n += snprintf(msg + n, sizeof msg - (size_t)n, " %llu", (unsigned long long)wall_ms_from_mono(ts_mono));
#ifdef VT_UI
    /* 面板的事件环：独立通道，和「脚本有没有订阅」无关（面板不该因为没脚本就看不到事件）。 */
    if (n > 0 && (size_t)n < sizeof msg) vt_shm_ring_push(msg, (size_t)n);
#endif
    if ((g.sub_mask & SUB_REGION) && (g.sub_region_id[0] == 0 || !strcmp(g.sub_region_id, id)) &&
        subev_want(g.sub_region_ev, ev)) {
        if (n > 0 && (size_t)n < sizeof msg) outq_push_text(msg, (size_t)n);
        fprintf(stderr, "vtouchd: ev %s %s slot%d %d,%d t=%llu\n", id, ev, slot, lx, ly,
                (unsigned long long)wall_ms_from_mono(ts_mono));
    } else {
        fprintf(stderr, "vtouchd: ev %s %s slot%d %d,%d (UNSUB) t=%llu\n", id, ev, slot, lx, ly,
                (unsigned long long)wall_ms_from_mono(ts_mono));
    }
}
/**
 * (vtouch-doc: phys_ev_send)
 * @brief 物理触摸流（sub phys）：按 slot 报 down/move/up，不按区域过滤。
 * @param   ev       来自 region_q 的事件（带逻辑坐标与时间戳）
 * @note    「按下之后一路跟到抬起」的底座：区域事件出了区域就断了，这条流不断。只订 SUB_PHYS 才发；只报物理手指，虚拟触点不进（防自激）。
 */
void phys_ev_send(const struct vt_ev *ev)
{
    char msg[96];
    const char *act;
    int n;
    act = (ev->action == VT_DOWN) ? "down" : (ev->action == VT_UP) ? "up" : "move";
    if (!(g.sub_mask & SUB_PHYS)) return;
    if (g.sub_phys_mask != 0 && (ev->slot < 0 || ev->slot >= 32 ||
                                 !(g.sub_phys_mask & (1u << ev->slot)))) return;
    if (!subev_want(g.sub_phys_ev, act)) return;
    /* 时间戳按需（默认不发；裸 sub phys = 老格式，老客户端不受影响） */
    if (g.sub_phys_ts)
        n = snprintf(msg, sizeof msg, "phys_ev %s %d %d %d %llu", act, ev->slot, ev->x, ev->y,
                     (unsigned long long)wall_ms_from_mono(ev->ts));
    else
        n = snprintf(msg, sizeof msg, "phys_ev %s %d %d %d", act, ev->slot, ev->x, ev->y);
    if (n > 0 && (size_t)n < sizeof msg) outq_push_text(msg, (size_t)n);
    fprintf(stderr, "vtouchd: phys %s slot%d %d,%d t=%llu\n", act, ev->slot, ev->x, ev->y,
            (unsigned long long)wall_ms_from_mono(ev->ts));
}

/* region_apply 攒事件的本地槽位：id **必须拷进本地缓冲** —— region_ev_send 收的是 rg->id
 * 指针，而解锁后区域表可能已被改名/删除/清空。事件名是静态字面量，不必拷；坐标/slot/
 * 时间戳一并攒起来。 */
struct region_pend_ev {
    char id[REGION_ID_MAX + 1];
    const char *name;
    int slot, lx, ly;
    uint64_t ts;
};
/**
 * (vtouch-doc: region_apply)
 * @brief 处理一个物理事件：先按 slot 报物理触摸流（sub phys），再做区域五事件判定。
 * @param   ev       来自 region_q 的事件
 * @note    三张状态表是线程私有的，只在 region_lock 里读区域表。
 *
 * 为什么这么写（原有注释，逐字保留）：
 *   五事件判定的事件化版本（§4.4）。与完整版 region_match 逐分支等价：
 *   DOWN → 命中就 slot_hit=1 并报 down；无论命中与否都把按下位置记为 move 基准
 *   （完整版：`if (g.phys.down) { if (!g.ps_down) {...} }`）
 *   MOVE → 先用「上一事件的 slot_in」比 enter/exit，再在「此前已在区域内且位置变化」时报 move
 *   （完整版：`if (hit && !slot_in) enter; else if (!hit && slot_in) exit;` + move 条件）
 *   UP   → (slot_hit || 上一事件还在区域内) && 命中 → 报 up，随后清 slot_hit
 *   （完整版在「该槽空闲后的下一帧」清；事件模型里没有空闲帧，就地在抬起事件清 ——
 *   up 的判定还要 g.ps_down（只有抬起事件才进这分支），清早了不会误报）
 *   为什么 up 还认「上一事件还在区域内」：这样"从区域外滑进来、在里面抬起"也能收到收尾事件
 *   （enter → move… → up）。那种 up 没有配对的 down，业务要配对就用 enter ↔ up。
 *   move 去重基准与 enter/exit 状态一样按 [slot][region] 分开存：区域重叠时，
 *   同一个 move 要给每个命中的区域各报一条（旧版按 slot 共享，只有第一个区域收得到）。
 *   每事件末 slot_in = (down && hit)，与完整版每帧末的赋值一致。
 */
void region_apply(const struct vt_ev *ev)
{
    /* B2：锁内**只判定**，事件攒进 pend[]，pthread_mutex_unlock() 之后再逐条 region_ev_send()。
     * region_ev_send 里有同步 fprintf + 面板 shm 环 push + 出站队列 push，全在 I/O 路径上；
     * 以前在 region_lock 里发 ⇒ 区域线程写日志时会挡住 poll 线程的
     * vt_shm_edit_apply → region_add（src/vtouchd.c:194）与 region list（src/vt_ws.c:576）。
     * pend[] 按 MAX_REGIONS * 2 预留：一次 region_apply 里每个区域最多 2 条（MOVE 时
     * enter|exit 与 move 可能同时成立）。
     * 判定顺序与发事件的相对顺序逐字不变；**唯一语义变化**是「事件在解锁后才投递」：判定与投递
     * 之间表可能已被 clear/删除/改名，事件仍按**判定时**的 id 投递（id 已拷进 pend[].id，读不到被
     * 改写的缓冲）⇒ 「该区域在表里已不存在了，还收到它的 up/exit」这种交错在改动前不可能出现、
     * 现在可能（顺序/内容判定仍用当拍锁内读到的表）。 */
    struct region_pend_ev pend[MAX_REGIONS * 2];
    int np = 0, rid, hit, i, lx = ev->x, ly = ev->y, slot = ev->slot;
    if (slot < 0 || slot >= MAX_PHYS) return;
/* 攒一条待发事件（只在锁内调用）；满了丢最末这条（新的），绝不越界。 */
#define PEND(_id, _name) do {                                                \
        if (np < (int)(sizeof pend / sizeof pend[0])) {                      \
            memcpy(pend[np].id, (_id), strlen(_id) + 1);                     \
            pend[np].name = (_name);                                         \
            pend[np].slot = slot; pend[np].lx = lx; pend[np].ly = ly;        \
            pend[np].ts = ev->ts;                                            \
            np++;                                                            \
        }                                                                    \
    } while (0)
    phys_ev_send(ev);                                /* ① 物理触摸流：按 slot 报，与区域无关 */
    pthread_mutex_lock(&g.region_lock);              /* ② 区域五事件判定 */
    if (r_seen_gen != region_gen) {                 /* region clear/del（结构变化才 bump）：重置本线程私有状态 */
        r_seen_gen = region_gen;
        memset(r_slot_in, 0, sizeof r_slot_in);
        memset(r_slot_hit, 0, sizeof r_slot_hit);
        memset(r_slot_last_x, 0, sizeof r_slot_last_x);
        memset(r_slot_last_y, 0, sizeof r_slot_last_y);
    }
    for (rid = 0; rid < g.region_count; rid++) {
        struct region *rg = &g.regions[rid];
        int was_in = r_slot_in[slot][rid];
        if (!rg->enabled) { r_slot_in[slot][rid] = 0; r_slot_hit[slot][rid] = 0; continue; }
        hit = region_hit(rg, lx, ly);
        if (ev->action == VT_DOWN) {
            if (hit) { r_slot_hit[slot][rid] = 1; PEND(rg->id, "down"); }
            r_slot_last_x[slot][rid] = lx; r_slot_last_y[slot][rid] = ly;
        } else if (ev->action == VT_MOVE) {
            if (hit && !was_in) PEND(rg->id, "enter");
            else if (!hit && was_in) PEND(rg->id, "exit");
            if (hit && was_in && (r_slot_last_x[slot][rid] != lx || r_slot_last_y[slot][rid] != ly)) {
                r_slot_last_x[slot][rid] = lx; r_slot_last_y[slot][rid] = ly;
                PEND(rg->id, "move");
            }
        } else {
            if ((r_slot_hit[slot][rid] || was_in) && hit) PEND(rg->id, "up");
            r_slot_hit[slot][rid] = 0;
        }
        r_slot_in[slot][rid] = (ev->action != VT_UP && hit) ? 1 : 0;
    }
    pthread_mutex_unlock(&g.region_lock);
    /* ③ 解锁后再发（不再占着 region_lock 做 I/O）：顺序与原来的锁内调用顺序逐字一致 ——
     * 按 rid 升序，每个区域内先 down / enter|exit，后 move / up。 */
    for (i = 0; i < np; i++)
        region_ev_send(pend[i].id, pend[i].name, pend[i].slot, pend[i].lx, pend[i].ly, pend[i].ts);
#undef PEND
}
/**
 * (vtouch-doc: region_thread_main)
 * @brief 区域线程主循环：pop region_q → region_apply；区域表代次变了就重置私有状态。
 * @param   arg      未使用
 * @return  NULL（线程不主动退出）。
 * @note    只消费队列、只写自己的状态表、只往出站队列塞 region_ev；绝不注入、绝不直写 socket、绝不碰 phys[]/virt[]。
 *
 * 为什么这么写（原有注释，逐字保留）：
 *   区域线程（§4.4）：只消费队列、只写自己的状态表、只把 region_ev 塞进出站队列。
 *   绝不注入、绝不直写 socket、绝不碰 g.phys[]/g.virt[]。
 */
void *region_thread_main(void *arg)
{
    struct vt_ev ev;
    (void)arg;
    for (;;) {
        if (!vtq_pop(&g.region_q, &ev)) {
            if (g.stop_flag) break;
            usleep(1000);                        /* 空闲 1ms 一轮：不烧 CPU，也不给事件加延迟 */
            continue;
        }
        region_apply(&ev);
    }
    return NULL;
}
