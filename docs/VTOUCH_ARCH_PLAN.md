# vtouchd 重构方案:合并/转发 与 判断/推送 分离 (Plan B)

> 状态: 设计评审通过, 待实施
> 目标: 让"判断和推送"完全脱离触摸注入热路径; 顺带修复虚拟 tracking id 冲突隐患
> 语义零变化: 五事件判定逻辑、WS 协议、区域表结构均不动

## 1. 动机

当前 vtouchd 是单线程 poll 循环, SYN 帧边界后依次执行:

```
emit_frame()      // 合并注入  —— 微秒级, 必须快
region_match()    // 区域判断  —— 纯 int 比较 + 静态数组, 微秒级
vtouch_ui_sync()  // UI 回调    —— 进程内函数指针, 快
ws_send()         // 事件推送  —— 阻塞直写 client_fd ← 真正可能卡住整个循环的
```

- `region_match` 本身对注入路径影响可忽略, 但"推送"(`ws_send` 阻塞写)在客户端 TCP 缓冲满时会
  堵住整个 poll 循环, 物理触摸注入跟着停 —— 这是需要分离的**真实瓶颈**。
- 区域判断与合并注入耦合在同一循环, 状态表(`slot_in/slot_hit/slot_last`)散在主线程,
  无法独立演进(手势识别、多客户端、UI 面板)。

## 2. 架构总览

```
【合并方案 — 进】                        【转发方案 — 出】
                                         每消费者独立 SPSC 队列
物理驱动 ──▶ phys[] ──┐       ┌────▶ 队列[区域线程] ──▶ 五事件状态机 ──▶ region_ev
                      ├▶ emit_frame() ─▶ writev ─▶ uinput ─▶ Android       │
WS 命令  ──▶ virt[] ──┘       │         └── broadcast_pev() ─▶ 队列[UI面板] ──▶ overlay 手指
        (立即/原子帧)          │                        └────▶ 队列[外部客户端] ──▶ WS 帧
                              │
      出站: 响应/pev/region_ev ──▶ 发送队列 ──(poll POLLOUT)──▶ 客户端 socket
```

- **主线程只做两类动作**: 合成帧写 uinput、非阻塞 push 队列。任何"慢"都发生在消费者自己的队列里。
- 判断(region)和推送(WS 写)全部移出热路径。

## 3. 合并方案(现状, 保持不变)

### 3.1 两张槽表

```c
struct contact { int id, x, y, down, pending_up; };
struct contact phys[MAX_PHYS];   // 物理手指: /dev/input/eventX 读入, ≤64
struct contact virt[MAX_VIRT];   // 虚拟手指: WS 注入, ≤32
```

物理: `physical_events()` 逐条消费 24B input_event, `ABS_MT_SLOT` 选槽,
`TRACKING_ID(<0 抬起/≥0 按下)`、`POSITION_X/Y` 写槽。
虚拟: `down/move/up <slot> <x> <y>` 逻辑坐标 → `logical_to_raw` → 写槽。

### 3.2 槽位编排

```
uinput「vtouch-merged」声明 total_slots = phys_slots + vslots
    槽 0..phys_slots-1      → 物理手指 (原样转发)
    槽 phys_slots..+N-1     → 虚拟手指
```

同一个 uinput 设备、同一个 ABS_MT_SLOT 空间 —— Android 看到一块支持 96 指的触摸屏,
不区分物理/虚拟。

### 3.3 帧组装 (emit_frame, vtouchd.c:272)

顺序刻意: 物理抬起 → 物理按下/移动 → 虚拟抬起/按下 → BTN_TOUCH/BTN_TOOL_FINGER(any_down) → SYN_REPORT。

- 物理 raw 坐标原样写回, 零精度损失
- 虚拟坐标经 logical_to_raw 换算
- 整帧 ≤483 事件, 单次 `writev` 提交, 单次 SYN 原子生效

### 3.4 触发时机

| 触发 | 行为 |
|---|---|
| 物理帧 | 攒表, SYN 时才 emit(尊重物理驱动帧边界) |
| 虚拟命令 | 每条 down/move/up 立即 emit(响应即时) |
| 原子多指帧 | begin_frame 暂存 → point×N → end_frame 一次 emit(单 SYN) |
| 复位 | owner_reset: 虚拟全抬起并立即 emit, 物理不受影响 |

## 4. 转发方案(新增)

### 4.1 转发内容与时机

```c
if (e.type == EV_SYN && e.code == SYN_REPORT) {
    emit_frame();        // ① 合并注入
    broadcast_pev();     // ② 状态变化 → 推每个订阅者队列 (原 region_match 移出)
}
```

只推状态变化(down/up/move), 静止不刷屏。广播在 SYN 之后 → 快照是完整帧状态,
消费者永远看不到半更新(优于共享数组直读)。

### 4.2 事件格式

```c
struct vt_ev {
    int      slot;    // 槽号
    int      action;  // 0=up 1=down 2=move
    int      x, y;    // 逻辑坐标
    uint64_t ts;      // down=按下时刻, move/up=帧到达时刻 (手势识别预留)
    int      virt;    // 0=物理 1=虚拟 — 区域线程过滤 virt=1 (不自激)
};
```

### 4.3 队列机制

- **每订阅者一个独立队列**(区域线程/UI 面板/外部客户端), 一个消费者慢不拖累其他
- **SPSC 无锁环形队列**: 每队列单生产者(主线程)+ 单消费者 → 无锁, push 微秒级
- **容量 64**, 有界, 主线程永不阻塞
- **溢出**: ① 队尾同 slot move 直接覆盖合并（再退 8 格找同槽旧 move 原地覆盖）; ② 仍满 → 丢**这一条新的**
  （2026-09-19 按评审 C13 修订：生产者**绝不推进 head** —— 老写法「丢最旧」靠 CAS 推 head 腾格子，会与消费者抢 head）
  —— 事件流是增量状态, 丢一条位置值不影响最终语义; down/up 只有在队列挤满且找不到可覆盖的旧 move 时才会被丢

### 4.4 区域线程(原 region_match 逻辑原样搬入)

```c
for (;;) {
    struct vt_ev ev;
    if (!spsc_pop(&region_q, &ev)) { sleep(1); continue; }
    if (ev.virt) continue;                    // 虚拟触摸不匹配 (不自激)
    // down/up/enter/exit/move 五事件判定 (slot_in/slot_hit/slot_last 移到本线程私有)
    // region_ev 进"出站发送队列", 不直写 socket
}
```

`slot_in/slot_hit/slot_last` 状态表从主线程移入消费者线程私有 —— 主线程彻底不再持有区域状态。

### 4.5 出站发送队列

所有出站消息(命令响应 / pev / region_ev)统一进发送队列, 主循环 poll 第 4 路 fd 以 `POLLOUT`
事件驱动写出:

```c
poll(p, 4, ...);
if (p[3].revents & POLLOUT) flush_send_queue();   // socket 可写才写
```

效果: 客户端接收慢/TCP 缓冲满时, 出站消息排队, 主线程照常处理触摸 —— 注入路径永不被慢客户端堵住。

### 4.6 订阅语义

- 现有 `sub/unsub` 协议不变: sub = 在订阅者列表注册一个队列
- 断连/被踢: 队列销毁, 订阅清零(与现状语义一致)

## 5. 随附修复: 虚拟 tracking id 冲突

现状(vtouchd.c:321): `state[slot].id = next_tracking_id++` 从 1 起自增。
物理驱动通常也从 0/1 起分配 id → **虚拟第 1 指(id=1)与物理第 2 指(id=1)同时按下时撞车**,
Android InputReader 会误认为同一触点。

修复(分配时避开活跃物理 id):

```c
do { id = next_tracking_id++; } while (phys_id_active(id));
```

## 6. 改动清单

| 位置 | 改动 |
|---|---|
| vtouchd.c | 广播函数(broadcast_phys)→ 改入队; 新增 SPSC 队列实现(~80 行) |
| vtouchd.c | 新增区域线程入口(搬入 region_match 逻辑 + 状态表) |
| vtouchd.c | 新增发送队列 + poll 第 4 路 fd(出站写事件驱动) |
| vtouchd.c | tracking id 避让(见 §5) |
| 协议 | 零变化 |
| clients/bundle | 零变化 (读线程/事件分发不变) |

## 7. 风险与边界

- 物理半帧窗口(虚拟命令插进物理帧中间): 维持现状语义(虚拟 emit 用 phys[] 当前值合成, last-value),
  实测无害, 不为此加复杂度
- 消费者崩溃: push 时发现队列满且无法合并 → 丢这一条新的（2026-09-19 按评审 C13 修订；head 只由消费者推进）, 不影响注入
- 内存: 每队列 64 × sizeof(vt_ev) ≈ 2KB, 三个消费者 ≈ 6KB, 可忽略
- 时间戳时钟: `clock_gettime(CLOCK_MONOTONIC)` 单调时钟, 不受系统时间调整影响

## 8. 实施顺序(小步验证)

1. tracking id 避让(独立, 15 行, 先合)
2. 出站发送队列 + poll POLLOUT(消灭主线程阻塞写, 独立可验证)
3. 区域线程 + 事件队列(region_match 逻辑原样搬入, 语义零变化)
4. AVD 回归: ws_smoke.py / ws_kick.js / 五事件示例 / 回触示例
