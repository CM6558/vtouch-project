/* vt_region.c（§5+§6 区域表与区域线程） —— 模块地图见 vt_internal.h；私有状态就近放 static，共享状态走 g。 */
#include "vt_internal.h"

/* 区域线程私有状态（§4.4：主线程不再持有区域状态）*/
static unsigned region_gen;
static unsigned r_seen_gen;
static unsigned char r_slot_in[MAX_PHYS][MAX_REGIONS];
static unsigned char r_slot_hit[MAX_PHYS][MAX_REGIONS];
static int r_slot_last_x[MAX_PHYS], r_slot_last_y[MAX_PHYS];
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
 *   只订了 g.phys 通道就不白推 region_ev（和 pev 的开关对称）。
 */
void region_ev_send(const char *id, const char *ev, int slot, int lx, int ly)
{
    char msg[96];
    int n = snprintf(msg, sizeof msg, "region_ev %s %s %d %d %d", id, ev, slot, lx, ly);
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
 * @note    virt=1 的事件直接跳过（这就是「回触不自激」）；三张状态表是线程私有的，只在 region_lock 里读区域表。
 *
 * 为什么这么写（原有注释，逐字保留）：
 *   五事件判定的事件化版本（§4.4）。与完整版 region_match 逐分支等价：
 *   DOWN → 命中就 slot_hit=1 并报 down；无论命中与否都把按下位置记为 move 基准
 *   （完整版：`if (g.phys.down) { if (!g.ps_down) {...} }`）
 *   MOVE → 先用「上一事件的 slot_in」比 enter/exit，再在「此前已在区域内且位置变化」时报 move
 *   （完整版：`if (hit && !slot_in) enter; else if (!hit && slot_in) exit;` + move 条件）
 *   UP   → slot_hit && 命中 → 报 up，随后清 slot_hit
 *   （完整版在「该槽空闲后的下一帧」清；事件模型里没有空闲帧，就地在抬起事件清 ——
 *   up 的判定还要 g.ps_down（只有抬起事件才进这分支），清早了不会误报）
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
            r_slot_last_x[slot] = lx; r_slot_last_y[slot] = ly;
        } else if (ev->action == VT_MOVE) {
            if (hit && !was_in) region_ev_send(rg->id, "enter", slot, lx, ly);
            else if (!hit && was_in) region_ev_send(rg->id, "exit", slot, lx, ly);
            if (hit && was_in && (r_slot_last_x[slot] != lx || r_slot_last_y[slot] != ly)) {
                r_slot_last_x[slot] = lx; r_slot_last_y[slot] = ly;
                region_ev_send(rg->id, "move", slot, lx, ly);
            }
        } else {
            if (r_slot_hit[slot][rid] && hit) region_ev_send(rg->id, "up", slot, lx, ly);
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
        if (ev.virt) continue;                   /* 虚拟触摸不参与匹配（防自激） */
        region_apply(&ev);
    }
    return NULL;
}
