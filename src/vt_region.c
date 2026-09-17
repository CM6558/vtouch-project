/* vt_region.c（§5+§6 区域表与区域线程） —— 模块地图见 vt_internal.h；私有状态就近放 static，共享状态走 g。 */
#include "vt_internal.h"

/* 区域线程私有状态（§4.4：主线程不再持有区域状态）*/
static unsigned region_gen;
static unsigned r_seen_gen;
static unsigned char r_slot_in[MAX_PHYS][MAX_REGIONS];
static unsigned char r_slot_hit[MAX_PHYS][MAX_REGIONS];
/* move 去重基准按 [slot][region] 分开存：多个区域重叠时，同一个 move 要给每个命中的区域各报一条 */
static int r_slot_last_x[MAX_PHYS][MAX_REGIONS], r_slot_last_y[MAX_PHYS][MAX_REGIONS];
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
 * @note    几何合法性（是否超出逻辑尺寸等）也在这里判。
 */
int region_add(const char *id, int type, int a1, int a2, int a3, int a4, int enabled)
{
    struct region *rg;
    size_t n;
    int i, rc = 0;
    if (!id) return -1;
    n = strlen(id);
    if (n == 0 || n > REGION_ID_MAX) return -1;
    if (type != 0 && type != 1) return -1;
    if (a1 < 0 || a2 < 0 || a3 < 0 || a4 < 0) return -1;
    if (a1 >= g.logical_width || a2 >= g.logical_height) return -1;
    pthread_mutex_lock(&g.region_lock);
    /* 同 id 查重：存在则原地更新（开关/挪区域只改属性，不新增） */
    for (i = 0; i < g.region_count; i++) {
        if (strcmp(g.regions[i].id, id) == 0) {
            rg = &g.regions[i];
            rg->type = type;
            rg->enabled = enabled ? 1 : 0;
            rg->a1 = a1; rg->a2 = a2; rg->a3 = a3; rg->a4 = a4;
            region_gen++;
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
        region_gen++;
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
    if (n == 0 || n > REGION_ID_MAX) return -1;
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
 * (vtouch-doc: region_ev_send)
 * @brief 发一条区域事件：订了 region 通道才入出站队列，没订就只打 (UNSUB) 日志。
 * @param   id       区域名
 * @param   ev       down/enter/move/exit/up
 * @param   slot     物理槽号
 * @param   lx       逻辑 x
 * @param   ly       逻辑 y
 * @note    低频事件；只报物理手指。
 *
 * 为什么这么写（原有注释，逐字保留）：
 *   命中事件通知（低频：down/up/enter/exit/move）；§4.5：进出发送队列，绝不直写 socket。
 *   没订区域通道就不白推 region_ev（判定的账照记，只打日志）。
 */
void region_ev_send(const char *id, const char *ev, int slot, int lx, int ly)
{
    char msg[96];
    int n = snprintf(msg, sizeof msg, "region_ev %s %s %d %d %d", id, ev, slot, lx, ly);
#ifdef VT_UI
    /* 面板的事件环：独立通道，和「脚本有没有订阅」无关（面板不该因为没脚本就看不到事件）。 */
    if (n > 0 && (size_t)n < sizeof msg) vt_shm_ring_push(msg, (size_t)n);
#endif
    if (g.sub_mask & SUB_REGION) {
        if (n > 0 && (size_t)n < sizeof msg) outq_push_text(msg, (size_t)n);
        fprintf(stderr, "vtouchd: ev %s %s slot%d %d,%d\n", id, ev, slot, lx, ly);
    } else {
        fprintf(stderr, "vtouchd: ev %s %s slot%d %d,%d (UNSUB)\n", id, ev, slot, lx, ly);
    }
}
/**
 * (vtouch-doc: region_apply)
 * @brief 五事件判定（区域线程）：按本轮事件更新 slot_in/slot_hit/slot_last，并决定发哪条事件。
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
    int rid, hit, lx = ev->x, ly = ev->y, slot = ev->slot;
    if (slot < 0 || slot >= MAX_PHYS) return;
    pthread_mutex_lock(&g.region_lock);
    if (r_seen_gen != region_gen) {                 /* region clear/add：重置本线程私有状态 */
        r_seen_gen = region_gen;
        memset(r_slot_in, 0, sizeof r_slot_in);
        memset(r_slot_hit, 0, sizeof r_slot_hit);
        memset(r_slot_last_x, 0, sizeof r_slot_last_x);
        memset(r_slot_last_y, 0, sizeof r_slot_last_y);
    }
    for (rid = 0; rid < g.region_count; rid++) {
        struct region *rg = &g.regions[rid];
        int was_in = r_slot_in[slot][rid];
        if (!rg->enabled) { r_slot_in[slot][rid] = 0; continue; }
        hit = region_hit(rg, lx, ly);
        if (ev->action == VT_DOWN) {
            if (hit) { r_slot_hit[slot][rid] = 1; region_ev_send(rg->id, "down", slot, lx, ly); }
            r_slot_last_x[slot][rid] = lx; r_slot_last_y[slot][rid] = ly;
        } else if (ev->action == VT_MOVE) {
            if (hit && !was_in) region_ev_send(rg->id, "enter", slot, lx, ly);
            else if (!hit && was_in) region_ev_send(rg->id, "exit", slot, lx, ly);
            if (hit && was_in && (r_slot_last_x[slot][rid] != lx || r_slot_last_y[slot][rid] != ly)) {
                r_slot_last_x[slot][rid] = lx; r_slot_last_y[slot][rid] = ly;
                region_ev_send(rg->id, "move", slot, lx, ly);
            }
        } else {
            if ((r_slot_hit[slot][rid] || was_in) && hit) region_ev_send(rg->id, "up", slot, lx, ly);
            r_slot_hit[slot][rid] = 0;
        }
        r_slot_in[slot][rid] = (ev->action != VT_UP && hit) ? 1 : 0;
    }
    pthread_mutex_unlock(&g.region_lock);
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
