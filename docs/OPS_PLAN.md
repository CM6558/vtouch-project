# 操作编辑器 + 核心执行器（设计定稿）

> 状态：2026-10-01 与用户逐段确认。形式 =「面板内操作编辑器」（去掉 AutoJs 启动，入口改 su 脚本）；
> 跟踪采用「完整按压」数据档（不做跟手/按住跟随/轨迹复刻）；第一版步骤集 = **点按 / 滑动 / 等待**。
> 口径：本文件事实以写作时**现读代码**为准（行号会漂，实现前再读一遍）。改核心/发版的固定顺序
> （恢复点 → 重建 → 门禁 → 真机验证）见 skill `references/core-change-and-release.md`。
> 流程口径（用户定，2026-10-01）：**不前置真机探测** —— 日志在实现批里就带上（§8），
> 测试集中在后期测试阶段照 §9 清单执行。

## 0. 目标与非目标

三批（每批独立验收、独立回滚）：

| 批次 | 内容 |
|---|---|
| 批 1 | **su 脚本入口**：装 / 起 / 停 / 状态（自包含单文件）——先把「启动不依赖 AutoJs」落掉 |
| 批 2 | **操作数据模型 + 核心执行器 + 面板「操作」页**（编辑 + 手动运行）——主心骨 |
| 批 3 | **触发侧**：区域绑定（按下 / 完整按压）+ 开关型区域 + 门控（开关开着才跑 / 跑完自动关） |

非目标（本期不做，均在 §12 留了口）：多指同帧步骤、循环/重复、轨迹复刻/录制、WS 新增 ops 命令、
开机自启、虚拟槽避让、事件环双生产者修复（见 §7 读码发现）。

## 1. 架构总览

```
面板进程(C++, src-ui)                          核心进程(C, src/)
┌─────────────────┐                          ┌──────────────────────────────────────┐
│ 「操作」页        │  写:编辑邮箱(区B)─►唤醒   │ 区域线程: 五事件 + 触发判定 ─┐         │
│  编辑/运行/进度   │ ───────────────────►管  │ 主循环: poll(物理/客户端/面板/操作fd)│        │
│  直读 ◄─────────  │  读:区A g.ops[]+状态 ◄─  │  ├ 编辑应用(校验后落表,同 region_add)│      │
└─────────────────┘                          │  ├ 执行器 vt_ops.c: 快照→逐步→组帧注入│    │
                                             │  └ 进度/事件 → 事件环(区C) → 面板      │      │
                                             └──────────────────────────────────────┘
```

原则（与既有架构同口径）：

- **执行器在核心主线程**：毫秒级计时（不受面板渲染抖动影响）；区域触发在核心里直接起跑（零往返）；
  面板崩了操作照跑（看门狗会把面板拉回来）。
- **操作表进共享内存区 A**（`g` 的一部分，面板**只读直读**）；编辑走区 B 编辑邮箱；
  状态/事件回显走区 A + 事件环区 C。
- **不加线程、不加命令通道**；**WS 协议零改动**（命令族 / 推送格式 / 兼容行为全部不动）。
- ops 相关代码全部在 `VT_UI` 分支内 → 默认（无 UI）构建不受影响（与 `vt_shm.c` / `vt_panel.c` 同待遇）。
- 新增文件 `src/vt_ops.c` 会被构建自动带上（`build.sh` 是 `src/*.c` 通配：`scripts/build.sh:51/58`）。

## 2. 数据模型与编辑通路

### 2.1 结构（`src/vt_internal.h` 追加）

```c
#define MAX_OPS        16        /* 操作条数上限 */
#define MAX_STEPS      32        /* 单条操作步骤数上限 */
#define OP_NAME_MAX    15        /* 同区域 id 规则：[A-Za-z0-9_-]、1..15（同 id_ok 尺子；实现时提取共用） */
#define OP_STEP_TAP    1
#define OP_STEP_SWIPE  2
#define OP_STEP_WAIT   3

struct vt_step {
    int type;                    /* 1=点按 2=滑动 3=等待 */
    int a1, a2, a3, a4;          /* 点按: x,y；滑动: 起点 x1,y1 → 终点 x2,y2；等待: 不用 */
    int ms;                      /* 点按=按住时长；滑动=时长；等待=时长 */
};
struct vt_op {
    char name[OP_NAME_MAX + 1];
    int  step_count;
    char gate[REGION_ID_MAX + 1];   /* 门控开关的区域 id；""=无 */
    int  auto_off;                  /* 跑完自动关掉门控开关 */
    struct vt_step steps[MAX_STEPS];
};
```

### 2.2 共享状态（`struct vt_state` 追加字段，随区 A 进共享内存）

```c
struct vt_op ops[MAX_OPS];
int op_count;
volatile int op_run;          /* 运行中的操作下标；-1 = 空闲 */
volatile int op_run_step;     /* 当前步（0 起） */
volatile int op_run_state;    /* 0=空闲 1=运行 */
int ops_wake_fd;              /* eventfd：区域线程触发 → 叫醒主循环（-1 = 没有） */
volatile uint32_t op_trig_seq;   /* 触发槽（区域线程写、主线程读；SPSC） */
char op_trig_name[OP_NAME_MAX + 1];
int  op_trig_slot;               /* 触发来源手指的槽号（日志用） */
```

### 2.3 区域结构扩展（绑定挂在区域上）

```c
struct region {
    /* ……现有字段不动（含 mark——老 WS 语义保留）…… */
    char trig_op[OP_NAME_MAX + 1];   /* 绑定的操作名；"" = 无 */
    int  trig_ev;                    /* 0=无 1=按下 2=完整按压 */
    int  kind;                       /* 0=普通 1=开关型 */
    volatile int toggle_on;          /* 开关型状态：核心写（区域线程，持 region_lock）、面板读 */
};
```

- `mark` 字段**原样保留**：脚本 `vt.mark/vt.toggle` 照旧工作（WS 兼容不删）；面板的"绿样式"改为
  `mark || (kind==toggle && toggle_on)`，新旧两条来源都画得出来。

### 2.4 编辑邮箱扩展（`src/vt_shm.h`）

`struct vt_shm_edit` 追加一条操作载荷（单槽邮箱、覆盖式；区 B 一页装得下）：

```c
#define VT_EDIT_OP_PUT   5   /* 新增/覆盖一条操作（载荷 = struct vt_op；重名=覆盖） */
#define VT_EDIT_OP_DEL   6   /* 删一条（id=名字） */
#define VT_EDIT_OP_CLEAR 7   /* 清空 */
#define VT_EDIT_OP_RUN   8   /* 起跑（id=名字） */
#define VT_EDIT_OP_STOP  9   /* 中止运行中的操作 */
#define VT_EDIT_BIND    10   /* id=区域, new_id=操作名（"-"=解除）, type=时机(0/1/2) */
#define VT_EDIT_KIND    11   /* id=区域, type=kind(0/1) */
...
struct vt_shm_edit { /* ……现有字段…… */ struct vt_op payload; };
```

现有 4 个区域码（ADD/DEL/RENAME/CLEAR）语义不变。

### 2.5 契约版本

`VT_SHM_VERSION` **3 → 4**（`src/vt_shm.h:29`，注释写明 v4 = 操作表 + 邮箱载荷 + 取点字段）。
面板是内嵌在同一次构建里的（`build.sh ui` 把三件套链进核心），没有外部兼容压力；magic/ver 校验
（`src/vt_shm.c:207-211`）照旧。

### 2.6 提交链路（面板 → 核心）

与区域编辑**同一条路**：

1. 面板侧预检（只做提示用）：名字规则 / 坐标粗查 —— `id_name_ok`（`vtouch_ui.cpp:1468`）同款逻辑。
2. `glue_post()`（`ui_glue.c:132-163`）：投邮箱 → 写唤醒管道 → **等 `edit_applied == seq`**（上限 ~1s）。
3. 回读校验（`glue_verify` 同款，`ui_glue.c:115-129`）：操作类 = 找到同名且 `step_count` 一致；
   DEL = 找不到；CLEAR = `op_count==0`；**RUN/STOP = 已 applied 即算投递成功**（运行可能瞬间结束，
   状态已归位，不能在回读里判）。
4. 失败 → 面板报错提示（现有 `ev_note` 路径）；成功 → 面板存盘。

### 2.7 校验（**核心单点**，同区域口径）

- 操作名：`id_ok` 同一把尺子（`src/vt_region.c:65`，字符集 `[A-Za-z0-9_-]`、1..15）。
- 坐标：`0..logical_w-1 / 0..logical_h-1`；ms 范围：点按 0..60000、滑动 1..60000、等待 0..600000。
- 步数 `1..MAX_STEPS`；条数 `≤MAX_OPS`（表满新增拒绝）。
- 非法编辑：核心拒绝（回读校验会失败），日志 `op 被拒 <名>: <原因>`。
- 绑定（BIND）：引用不存在的操作名**允许暂存**（悬空）；触发时解析失败 → 丢弃 + 日志。

### 2.8 落盘

- **ops.conf**（新）：`/data/local/vtouch-runtime/ops.conf`（与 `regions.conf` 同目录，`vtouch_ui.cpp:526`）：

  ```
  #vtouch-ops v1
  op <名> gate <门控区域id|-> autooff <0|1>
  step <type> <a1> <a2> <a3> <a4> <ms>
  …（一条操作 = op 行 + N 条 step 行）
  ```

  保存：`.tmp` + `rename`（同 `save_regions` 套路，`vtouch_ui.cpp:543-577`），失败重试 1s（`save_failed` 同款）。
  加载：版本门（不符 = 丢弃改写空表）；**只补缺**（核心表里已有同名 → 跳过，与区域表 2026-09-19 口径一致）；
  一条坏记录只警告跳过，不带走全表。

- **regions.conf**（增量，保持 `REGION_CONF_VER=2` 不升）：行末追加**新行类型**（旧读方会静默忽略）：

  ```
  bind <区域id> <操作名|-> <down|press>     # 触发绑定
  kind <区域id> <0|1>                       # 开关型
  ```

## 3. 执行器（核心侧）

### 3.1 文件与调用点

- 新文件 `src/vt_ops.c`：执行器 + 校验 + 触发槽处理 + 日志；原型进 `vt_internal.h`。
- `vtouch_poll_step()` 顶部、`vt_shm_edit_apply()` 之后（`src/vtouchd.c:199-203` 那段 `#ifdef VT_UI`
  tick 的末尾）调用 `vt_ops_tick()`：
  ① 消费触发槽（有新 seq → 尝试起跑）；② 推进运行中的操作；③ 刷新"下一步到点"的 deadline。
- 初始化：`vtouch_init` 里建 `ops_wake_fd`（eventfd，参见 `region_q_init` 模式）；失败 = 记日志降级
  （触发最坏退回下一轮 poll 超时唤醒）。

### 3.2 状态机与步骤语义

- 空闲 → 运行（**起跑时把整条操作快照**到执行器私有内存）→ 逐步推进 → 完成/中止 → 空闲。
- **一次只跑一条**；忙时新触发丢弃 + 日志（§4.4）。
- 步骤语义（全部用**单调钟绝对时间表**，不累加误差）：
  - **点按**：t0 发 down@(x,y) → t0+hold 发 up（hold=0 时 up 安排到下一拍，至少隔一帧）。
  - **滑动**：t0 发 down@(x1,y1) → 每 10ms 一个采样点（N = max(2, dur/10)；首点=起点、末点=终点），
    第 k 个采样在 t0+k·dur/N 发 move@线性插值 → 终点发 up。
  - **等待**：deadline = t0+ms，到点直接进下一步。
- **每步的开始时刻 = 上一步的结束时刻**（不额外加间隔；要间隔就插「等待」步骤）。
- 写失败（`g_reemit` 待重发，`src/vtouchd.c:235-244`）：**执行器不推进状态机**，等重发消化完
  （不丢帧、不跳步）。

### 3.3 计时整合

- `poll` 超时现三档 5/1/1000ms（`src/vtouchd.c:204-214`）→ **第四档**：运行中取
  `min(现有档, max(0, next_deadline − now))`。毫秒级；不新起线程。
- `ops_wake_fd` 加入 poll 集：`p[5]` 条件挂载（现有 `p[4]` = 面板唤醒 fd，`src/vtouchd.c:222-227`；
  数组与 `np` 逻辑同步扩为 6，全部在 `VT_UI` 分支内）。

### 3.4 触发请求（区域线程 → 主线程）

- 区域线程：写 `op_trig_name` / `op_trig_slot` → `__atomic` release 自增 `op_trig_seq` →
  `write(ops_wake_fd, 1)`。
- 主线程（`vt_ops_tick`）：acquire 读 seq，不等则取名字/槽、尝试起跑。
- 丢一次唤醒的兜底 = 下一轮 poll 超时（≤1s）捡起来；正常路径毫秒级。
- 突发多次触发：**只保留最新**（单槽覆盖），被覆盖的记 `op 丢弃 覆盖`。

### 3.5 虚拟槽

- 起跑时从 `0..vslots-1` 挑**第一个空闲虚拟槽**（`virt[i].down`/`staged[i].down` 均空），全程占用，
  结束/中止必抬指。挑不到 → 拒绝起跑 + 日志。
- 与 WS 客户端撞槽（客户端注入进来的槽 == 操作占用槽）→ 打一行 `op 槽冲突 k`（v1 不做避让）。

### 3.6 中止

- 触发点：面板「停止」（`OP_STOP`）/ 引擎停（stop_req、SIGTERM、设备挂断）/ WS `reset`、断连被踢
  （`owner_reset` 抬全部虚拟触点，`src/vt_frame.c:240` 一带）/ `cleanup()` 开头。
- 中止 = **抬掉操作的手指 + 立即提交一帧** + 状态字段归位 + 日志 `op 中止 <名> 步 i/N 原因=…`。
- `cleanup()` 里必须**先于 uinput 销毁**执行（`src/vtouchd.c:301-316` 开头加一行）。

### 3.7 状态回显

- 面板读区 A 的 `op_run / op_run_step / op_run_state` 画进度（运行中卡片：`运行中 · 第 k/n 步`）。
- 每次状态变化核心往事件环推一行 `op_ev <名> <run|done|abort> <i>/<N>` →
  面板记日志 + 立即重画（复用 `mark_ev` 那套：`vtouch_ui.cpp:709` 与 `vt_shm_ring_push`）。

## 4. 触发侧

### 4.1 区域绑定

- 语义（与 SDK 的 `onRegionPress` 默认口径一致）：
  - **按下**：down 命中区域的那一刻触发（一对一，不锁存）。
  - **完整按压**：down 命中 → **锁存该 (slot, 区域)** → 该手指抬起时触发一次（抬起位置不限）。
    锁存用一个**新增的**区域线程私有状态（与 `r_slot_hit` 并列，`src/vt_region.c` 内；不参与五事件判定）。
- 触发只由**物理手指**产生（虚拟触点不进队列：`src/vt_frame.c:192`，防自激不破）。
- 触发解析：写触发槽（§3.4），主线程解析操作名 → 不存在 = 丢弃 + 日志。

### 4.2 开关型区域

- `kind=1` 的区域内**完整按压**翻转 `toggle_on`（区域线程持 `region_lock` 写）+ 推
  `toggle_ev <id> <0|1>` 环行（面板重画）。
- 面板样式：`mark || (kind==toggle && toggle_on)` → 绿高亮/●开（沿用现有画法，
  `vtouch_ui.cpp:1142-1180` 一带）；`toggle_on` 面板直接读区 A。
- 翻转与「触发」独立：同一区域既当开关又绑触发也各算各的（翻转照常、触发按时机照发）。

### 4.3 门控与自动关

- 起跑时检查操作自己的门控：`gate` 非空 → 找该区域；必须是 `kind==toggle`；`toggle_on==1` 才放行，
  否则拒绝 + 日志 `op 丢弃 门控拦截`（解析失败同理——安全侧）。
- `auto_off=1`：运行正常结束时把门控开关翻回关（日志 + `toggle_ev`）。

### 4.4 丢弃规则

- 忙（已有操作在跑）→ 丢弃 + `op 丢弃 忙`（同时推 ring 行，面板可见）。
- 操作不存在 / 门控不存在或不是开关型 / 没空闲槽 → 丢弃 + 对应日志。

## 5. 面板页（「操作」页 + 取点 + 区域卡片扩展）

- **导航**：侧栏「页面」组插一行 → 区域列表 / **操作** / 事件日志 / 设置
  （`vtouch_ui.cpp:1586-1588` 三行变四行，g_nav 重排；分派 `:1923-1925`）。
- **操作页**：
  - 页头：`操作 · N 条` + 运行状态小字（`运行中: <名> 第 k/n 步` / `空闲`）+ `＋新建`
    （自动起名 op1/op2…，规则同区域 id）。
  - 卡片列表：`名字 · N 步 · 门控 <r1|无> · 跑完自动关 <开|关>`；按钮 `[运行]/[停止]`（运行时互变）、
    `[编辑]`、`[删除]`。
  - **编辑覆盖层**（像 `draw_name_edit`（`:1773`）那样整面盖住、底下不吃点击）：名字行、
    步骤列表（可滚动；每行 `1. 点按 540,1200 按住50ms [参数][↑][↓][删]`）、
    底部 `[＋点按] [＋滑动] [＋等待]`、门控行 `门控开关: [无|r1|…]`（点击循环）、`跑完自动关: [开/关]`、
    `[完成]`。
  - **数字键盘弹层**（新）：0-9 / ⌫ / 取消 / 确定 —— 坐标与毫秒都走它（面板无输入法，沿用改名弹层
    的自绘键盘路子，`:1807-1872`）；坐标字段旁多一个 `[取点]`。
- **取点**：
  1. 点 `[取点]` → 面板写区 B `pick_mode=1` + 画提示条「点屏幕上目标位置（点面板里取消）」。
  2. 核心在按下判定处（`src/vt_frame.c:155-163`，和「面板吞掉」**同一处、先面板矩形后取点**）：
     未被面板矩形吞掉的新按下 && `pick_mode` → **吞掉整段手势**（复用 `ui_eaten` 锁存）+ 回填
     逻辑坐标 + `pick_seq++` + 自动清 `pick_mode` + 日志 `取点 捕获 x,y`。
  3. 面板在 `vtouch_poll_step` 检查 `pick_seq` 变化 → 合成 `pick_ev <x> <y>` 交给现有事件回调 →
     回填字段、退取点态。
  4. 防呆：**20s 超时**（核心侧按自己的单调钟）+ **面板死亡清理**（核心看门狗面板死亡分支清
     `pick_mode`）—— 不会留下"永远吞触摸"的状态。
- **区域卡片扩展**（区域页）：每张卡片加一行 `触发: <无|操作名>`（点击循环切换）、
  `时机: <按下|完整按压>`、`开关型: <关|开>`；开关型显示开/关状态。
- **落盘**：见 §2.8；ops 的 pending 标志与区域表分开、共用同一套重试节奏。

## 6. su 脚本入口（批 1）

- **模板** `clients/vtouch.sh`（可读源码 + 占位符 `<<PAYLOAD>>`/`<<PAYLOAD_MD5>>`/`<<PAYLOAD_SIZE>>`，
  与 `clients/vtouch.js` 同款套路）→ `python scripts/pack_su.py` → **`build/vtouch.sh`**
  （自包含：脚本 + base64 内嵌 `build/vtouchd_ui`，~3.7MB）。
- **子命令**：`install` / `start`（内含 install）/ `stop` / `status`；无参数 = 用法说明。
- 细节：
  - 要求 root（`id -u` 非 0 → 提示用 `su -c` 重跑，退出 1）。
  - install：从自身解码载荷 → md5 对账（与 `/data/local/tmp/vtouchd_ui` 比）→ 不符则 `.tmp` 写入 +
    `mv` 原子替换 + `chmod 755` + 回读 md5 复核；相符 = `已是最新`。
  - start：装好后 `cd /data/local/tmp && nohup ./vtouchd_ui >/data/local/tmp/vt_ui_core.log 2>&1 </dev/null &`，
    等 ≤5s：pidof + 端口 6A2F LISTEN（`/proc/net/tcp`）→ 打印核心 pid + 面板 pid。
  - stop：SIGTERM → 等 ≤3s → 未退则 SIGKILL（并注明 grab 由进程退出释放）；停完回读 pidof 为空。
  - status：核心/面板 pid、端口状态、二进制 md5、日志尾 ~15 行。
- 用法：`su -c 'sh /sdcard/vtouch.sh start'`；设备侧仍只需「脚本一个文件 + 核心自解包面板」（核心自解包面板的既有形态——见 README「设备上有什么」）。
- **老通道不删**：AutoJs onefile（`pack_client.py`）、`ui-deploy.sh` / `ui_ondev.sh` 全部保持；
  将来要不要把 ui-deploy 也统一到新脚本，见 §12。

## 7. 并发与失败面

| 情况 | 行为 |
|---|---|
| 运行中又来触发（区域/按钮） | 丢弃 + 日志（一次只跑一条） |
| 运行中编辑/删除该操作 | 本次跑快照，不受影响；下次用新表 |
| 面板「停止」/ 引擎停（stop_req、SIGTERM、设备挂断） | 收尾前先中止：**抬掉操作的手指**再走原有清理 |
| 面板崩溃/被杀 | 操作与触发照跑（核心不依赖面板）；重启后重载 ops.conf（同名只补缺） |
| 核心崩溃/重启 | 触点随 uinput 销毁；重启后由面板重载 |
| WS 客户端 `reset` / 断连、被踢 | 它会把虚拟触点全抬掉 → **顺带中止操作**（日志注明原因） |
| WS 客户端与操作同槽 | 起跑挑空闲槽；撞上打一行 `op 槽冲突 k`（v1 不做避让） |
| uinput 写失败（g_reemit 待重发） | 执行器不推进状态机，等重发消化完（不丢帧、不跳步） |
| 触发时操作不存在 / 门控不存在或不是开关型 | 丢弃 + 日志（安全侧） |
| 编辑被核心拒（名字非法/坐标越界/步数超限/表满） | 面板回读校验失败 → 现有错误提示路径 |
| 取点异常（面板死/超时） | 核心侧两重兜底（20s + 面板死亡清理） |

**读码发现（现状、非本设计引入）**：事件环推送有**两个生产者** —— 主线程（`vt_ws.c:680` 的
`mark_ev`）和区域线程（`vt_region.c:289`）；`vt_shm_ring_push`（`src/vt_shm.c:109-122`）的 tail 是
"读-改-写"，并发下可能丢行。本设计让 `op_ev` 走主线程（与 mark_ev 同源），**不放大它**；
修不修另行拍板（§12）。

## 8. 日志清单（预留 —— 实现批内建好，测试阶段直接看）

核心 stderr（→ `/data/local/tmp/vt_ui_core.log`），统一 `op ` 前缀：

1. `op 启动 <名> 步数=N 槽=K 门控=<r1|无>`
2. `op 步 <i>/<N> <点按 x,y 按住m|滑动 x1,y1→x2,y2 Tm|等待 Tm>`
3. `op 完成 <名> 用时=NNNms`
4. `op 中止 <名> 步 i/<N> 原因=<停止按钮|引擎停|reset|断连|引擎收尾>`
5. `op 触发 <区域id> → <名>（按下|完整按压）` / `op 丢弃 <忙|覆盖|操作不存在|门控拦截|没空闲槽>`
6. `op 编辑 <put|del|clear|run|stop|bind|kind> <名/区域>`（镜像现有 `region add` 风格）
7. `op 被拒 <名>: <原因>`
8. `op 槽冲突 k`
9. `区域 <id> 开关 → 开|关`；`取点 捕获 x,y` / `取点 超时清除` / `取点 面板死亡清除`
10. 高频明细（滑动每采样点、每帧 emit）默认**不**打；`VTOUCH_OPS_TRACE=1` 时开（不干扰时序）

面板侧：`ops.conf 已存 N 条`；错误提示走现有 `ev_note`。su 脚本：每步 `[vtouch] …` + md5/pid 回读。

## 9. 测试清单（后期测试阶段照单执行）

构建/静态门（每批都跑）：

- `sh scripts/build.sh`（默认）+ `sh scripts/build.sh ui`（-Werror 零告警）；
  新文件单查：`sh scripts/check_syntax.sh src/vt_ops.c`。
- `python scripts/apply_funcdoc.py --check`（必须 0 处 —— **新函数文案先写进 `scripts/funcdoc_data.py`**）。
- `python scripts/ci_check.py`（SDK 逐字节门，核心变了要重跑 `pack_client.py`）。
- su 脚本：`sh -n build/vtouch.sh`（语法）；真机四命令（见下）。

真机（**不需要手指**）：

- 起核心 → 面板「操作」页：新建/编辑/删除操作 → 核心日志 `op 编辑 …` 逐条对上。
- 运行（操作里放「等待 + 点按」）：日志 `op 启动 → 步 → 完成` 序列完整；重复跑、越界坐标被拒。
- 运行中按「停止」/ 运行中停引擎：`op 中止` + 触摸回系统。
- 取点：进取点模式 → 点屏幕 → `取点 捕获` + 面板回填；点面板内 = 取消；放着不点 = 20s 超时。
- 开关：点开关区域 → `开关 → 开/关` + 屏幕样式翻转；门控操作被挡的日志。
- su 脚本：install（首次/已最新两态）、start（pid + 端口回读）、stop（pidof 空 + 触摸回系统）、
  status（md5/日志尾）—— 一律**回读**，不认命令回执。

真机（**需要手指/用户本人**）：区域「按下/完整按压」触发手感、门控连招、取点手感、连招节奏
（真机 + 目标 App 实测）。

## 10. 批次与验收

| 批次 | 内容 | 验收门 | 回滚 |
|---|---|---|---|
| 1 | su 脚本（`pack_su.py` + 模板 + README 段） | `sh -n`；真机四命令（后期测试） | 删产物/退回提交；不碰核心 |
| 2 | 操作模型 + 执行器 + 操作页（§2-3、§5 的操作部分） | build 门 + funcdoc 0 处 + ci_check；日志齐（§8） | 退提交重编重推；ops.conf 属新增文件，删除即净 |
| 3 | 触发侧（§4、§5 的区域卡片行） | 同上 | 同上 |

- 每批开工前：**全量备份 + 恢复点**（house 规矩）；每批独立提交（中文提交走 `-F` 文件）。
- `README.md` / `AGENTS.md` 随批更新（AGENTS.md **两份同步**：家目录 + 仓库根，`md5sum` 对齐）。

## 11. 修改清单（逐文件，预估）

| 文件 | 改动 |
|---|---|
| `src/vt_shm.h` | ver 4；邮箱载荷 + 新码；`vt_shm_b` 加 pick 字段 |
| `src/vt_internal.h` | `vt_step`/`vt_op`；`vt_state` 加 ops 等字段；区域结构扩展；原型 |
| `src/vt_ops.c` | **新**：执行器 + 校验 + 触发槽 + 日志（~350 行） |
| `src/vt_region.c` | 触发判定 / 开关翻转 / BIND·KIND 落地 / `id_ok` 复用 |
| `src/vt_frame.c` | 取点吞（与面板矩形吞同一处） |
| `src/vt_shm.c` | `edit_apply` 扩展；pick 访问器 |
| `src/vtouchd.c` | `vt_ops_tick()` 调用；poll 第四档 + `p[5]`；eventfd；`cleanup` 先中止 |
| `src-ui/ui_glue.c` | 操作 API（count/get/put/del/clear/run/stop）、BIND/KIND、取点、状态（~15 个函数） |
| `src-ui/vtouch_ui.cpp` | 操作页 + 编辑层 + 数字键盘 + 取点 UI + 区域卡片行 + 落盘 |
| `clients/vtouch.sh` + `scripts/pack_su.py` | **新**：su 脚本与打包器 |
| `scripts/funcdoc_data.py` | 新函数文案（先文案后代码） |
| `README.md` / `AGENTS.md` | 新功能与 su 脚本用法段落 |

## 12. 待拍板与留口

1. **事件环双生产者**（§7 读码发现）：修（核心内加把小锁）还是另开一轮？默认：**不修、不放大**。
2. `ui-deploy.sh` / `ui_ondev.sh` 要不要统一收编到新 su 脚本？默认：**不动**（后续可选）。
3. 上限常量（MAX_OPS 16 / MAX_STEPS 32）与滑动采样 10ms —— 可随用随调。
4. 留口（后加不返工）：多指同帧步骤、循环、翻转当触发、槽位避让、录制回填编辑器（形式 B）。
