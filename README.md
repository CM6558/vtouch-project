# vtouch-project（最小核心 + 转发引擎 · Plan B）

Android 上把**真实手指**和**注入的虚拟手指**合成**一条**触摸流的用户态方案，只需要 root。
一个 C 文件、无 UI。这一支在最小版之上按 `docs/VTOUCH_ARCH_PLAN.md`（Plan B）重建了引擎：
**「合并 / 转发」与「判断 / 推送」分离** —— 注入路径只往队列里塞事件，判断与推送都在别的路子上，互不阻塞。

```
src/vtouchd.c                   daemon：EVIOCGRAB 抓物理触摸屏 + uinput 合并 + WS 注入
                               + 事件队列 → 区域线程（五事件判定）→ 出站队列 → 客户端
clients/vtouch.js               AutoJs6 客户端（Finger API：down/move/up/tap/swipe/frame）
clients/region_demo.js          示例：建区域 + 订阅，逐条打印 down/enter/move/exit/up
clients/touchback_demo.js       示例：命中区域就回触（自带「不自激」自检）
scripts/build.sh                NDK 交叉编译（arm64）
scripts/deploy.sh               adb 推二进制到 /data/local/tmp + 起/停/状态（含回读 md5 对账）
tests/ws_smoke.py               主机侧 smoke（握手 + 注入命令链路）
tests/ws_planb_regression.py    主机侧回归：命令面 / 订阅 / 五事件 / 回触不自激 / ws_kick
docs/VTOUCH_ARCH_PLAN.md        本分支所依据的方案原文（§8 四步 = 本分支的四段改动）
docs/                           完整版（面板那一代）的走读与工程图，与本分支无关，可删
```

## 前提

- 设备已 root（daemon 需要 `/dev/uinput` 与 `EVIOCGRAB`）。
- 本机构建：Android NDK（默认 `C:/Users/21102/android-ndk-r27d`，可用 `NDK_ROOT` 覆盖）。
- git-bash / Linux 均可跑脚本（脚本按 `uname` 选工具链后缀）。

## 三步跑起来

```sh
sh scripts/build.sh                 # 出 build/vtouchd（arm64，零告警）
sh scripts/deploy.sh deploy         # 推到 /data/local/tmp/vtouchd 并回读 md5 对账
sh scripts/deploy.sh start          # 起（自动按 wm size 归一化成竖屏尺寸）
adb forward tcp:27183 tcp:27183
python tests/ws_smoke.py            # 主机侧验证握手 + 注入链路
python tests/ws_planb_regression.py # 主机侧验证引擎（--skip-phys 可跳过需要伪造物理手指的用例）
sh scripts/deploy.sh stop           # 停：EVIOCGRAB 随进程退出释放，物理触摸立刻回系统
```

AutoJs6 里：

```js
var vt = require("/sdcard/vtouch.js");
vt.start();
var c = vt.connect();
vt.finger().tap(540, 1200);                        // 自动挑空闲 slot
vt.finger(1).down(100, 200).move(140, 240).up();   // 显式 slot
vt.frame([{slot:0,state:"down",x:100,y:200},       // 多指合并进同一帧
          {slot:1,state:"down",x:300,y:200}]);
c.close(); vt.stop();                              // 收尾（务必：否则一直抓着物理触摸）
```

看区域事件、看回触：

```js
// clients/region_demo.js   ：建一个中央区域 → sub region → 打印五事件
// clients/touchback_demo.js：命中区域就注入一路虚拟触点回触；注入期间收到 region_ev 会报「自激!」
```

## 线协议（loopback WS，一行一条命令，单客户端，新连接踢旧连接）

| 命令 | 应答 | 说明 |
|---|---|---|
| `ping` | `pong` | 探活 |
| `res` | `res <宽> <高> raw <xmin> <xmax> <ymin> <ymax>` | 逻辑尺寸与内核轴量程 |
| `reset` | `ok` / `err frame` | 抬掉全部虚拟触点（帧中途拒绝） |
| `down <slot> <x> <y>` | `ok` / `err point` | 按下（**各自成一帧**） |
| `move <slot> <x> <y>` | `ok` / `err point` | 移动（各自成一帧） |
| `up <slot>` | `ok` / `err point` | 抬起（各自成一帧） |
| `begin_frame` | `ok` / `err frame` | 开始一帧多指 |
| `point <slot> <down\|move\|up> <x> <y>` | `ok` / `err point` | 帧内一步（同一 slot 一帧一次） |
| `end_frame` | `ok` / `err frame` | 提交这一帧 |
| `region add <id> <0矩形\|1圆形> <a1> <a2> <a3> <a4> <0\|1>` | `ok <总数>` / `err region` | rect: `x1 y1 x2 y2`；circle: `cx cy r 0`；同 id 重加 = 原地更新 |
| `region list` | 每行 `region <id> <type> <a1..a4> <en>` + 末行 `end <n>` | 表很小（≤32），一次回全量 |
| `region clear` | `ok 0` | 清空（区域线程自己重置私有状态） |
| `sub [phys\|region\|all]` | `ok` / `err sub` | 裸 `sub` = all（老脚本语义不变） |
| `unsub` | `ok` / `err sub` | 退订所有推送 |

坐标是**竖屏逻辑坐标**（`-w/-h` 那一套）。推送是**单向**的，混在同一条 WS 里：

| 推送 | 形状 | 什么时候来 |
|---|---|---|
| `pev` | `pev <slot> <down\|move\|up> <x> <y>` | **物理手指**的轨迹（订阅 `phys` / `all`） |
| `region_ev` | `region_ev <id> <ev> <slot> <x> <y>` | 区域事件，`ev` ∈ `down/enter/move/exit/up`（订阅 `region` / `all`） |

两条推送的边界是对称的：**`pev` 只报物理手指**（客户端自己注入的轨迹不会被回灌成事件），
**`region_ev` 只由物理手指产生**（虚拟触点进得了队列、进不了判定）—— 后者就是「回触不会自己触发自己」的保证。

## 引擎是怎么做的（六点，都在 `src/vtouchd.c`）

1. **转发（§4.1）**：物理帧边界（每个 `SYN_REPORT`）在 `emit_frame()` 之后做一次状态比较，
   只把**变化**（down/up/move）打包成事件推给队列 —— 推的是「完整帧状态的快照」，静止不刷屏。
   虚拟触点的状态变化也入队（带 `virt` 位），消费者按位过滤。
2. **队列（§4.2）**：事件队列是定长 SPSC 环形，满了**丢弃并计数**，绝不阻塞生产者（注入路径）。
3. **线程（§4.3）**：区域线程在 `EVIOCGRAB` 成功**之后**才起（`pthread_create` 失败 → 退出码 7，
   不会留下「抓了却没人判定」的状态）；它只消费队列、只写自己的状态表、只往出站队列塞 `region_ev`，
   **绝不注入、绝不直写 socket、绝不碰 `phys[]/virt[]`**。
4. **判定（§4.4）**：五事件语义与完整版 `region_match` 逐分支等价 ——
   `down`（按下且命中）· `enter`（由外入内）· `move`（已在区域内且位置变化）· `exit`（由内出外）· `up`（按下时命中过且抬起仍在内）。
   区域表由主线程写（短锁），区域线程读 —— 这把锁不在注入路径上。
5. **出站（§4.5）**：响应 / `region_ev` / `pev` 全部进**出站队列**，主循环 `poll` 里只有一个刷出点
   （队列非空才挂 `POLLOUT`），socket 置 `O_NONBLOCK` —— **客户端慢只是堆队列，注入路径照常跑**。
6. **订阅与踢（§4.6）**：订阅位决定推哪一路；断连/被踢时清订阅位并丢弃残包（不会串给下一个客户端）。

## 合并是怎么做的（四点）

1. **抓**：扫 `/dev/input/event0..63` 找 Type-B 触摸屏（认槽/tracking id/XY 四轴），`EVIOCGRAB` 抓走它。
2. **镜像声明**：把物理屏的 EV/KEY/ABS(+absinfo)/props 整份照抄到 uinput 设备，只有 4 处真冲突取相似值
   （tool 量程、槽数、id 池、名字/bus）；`INPUT_PROP_DIRECT` 无条件声明，否则系统会把它当触控板画鼠标指针。
3. **同帧合并**：先发待抬触点（`TRACKING_ID=-1`）→ 物理触点 → 虚拟触点 →
   `BTN_TOUCH/BTN_TOOL_FINGER` → `SYN_REPORT`，**整个帧一次 `writev`**。物理与虚拟共用一套下游身份池
   （槽与 tracking id 都重新分配，不透传客户端编号 → 不会撞号）。
4. **失败兜底**：写帧失败绝不丢「抬手」那一帧，置重发标志 5ms 后再发（连续 200 次才认 uinput 真死并退出）。
   客户端挂断/被踢时，抬掉它的虚拟触点并归还帧内身份（否则池会被泄漏的身份占死）。

启动顺序是 **先起监听、最后 grab、区域线程再最后**：任何失败路径都不会留下「抓着触摸却没人能控制」的状态。

| 退出码 | 含义 |
|---|---|
| `2` | 逻辑尺寸缺失 / 扫不到 Type-B 触摸屏 |
| `3` | uinput 建不起设备 |
| `4` | 打不开触摸设备 |
| `5` | `EVIOCGRAB` 失败 |
| `6` | 端口被占（127.0.0.1:27183） |
| `7` | 区域线程创建失败 |

## 日志怎么看（几条「引擎在按设计工作」的证据）

| 日志行 | 含义 |
|---|---|
| `region 0x… 代次 N`（区域线程启动时） | 区域线程起来了，开始等事件 / 等区域表 |
| `vtouchd: ev <区域> <事件> slot<槽> <x>,<y>` | 区域事件已进**出站队列**（有客户端订阅 `region`） |
| `vtouchd: ev … (UNSUB)` | 判定出来了但没人订阅 `region` —— 正常，不是故障 |
| `vtouchd: 事件队列满/出站队列满，丢弃第 N 条` | 走了 §4.3 / §4.5 的「丢最旧」；**注入路径不受影响**，客户端少收一条事件 |

## 本分支还没做的（完整版里有）

ImGui 面板与 overlay、`regions.conf` 持久化、面板区吞触摸、旋转坐标换算、
host 侧桩测与产物对账脚本、CI。区域匹配与订阅、出站队列在本分支是**有**的（按 Plan B 重建）。

完整版连同它的文档、工程图都在 `build/_backup_full_<时间戳>/`
（`files/` 是逐份拷贝，`from_head/` 是 HEAD 里被删前的文件，`RESTORE.md` 写了回滚步骤）；
`docs/CODE_WALKTHROUGH.md` 与 `docs/diagrams/` 讲的也是完整版。

## 已知边界

- **不做旋转换算**：daemon 恒为竖屏坐标系，横屏时由调用方自行换算（完整版有 c2p/p2c）。
- **单客户端**：新连接会踢掉旧连接；被踢的一方要自己发现（本客户端不做保活/重连）。
- **区域判定只认物理手指**：注入的虚拟触点不会产生 `region_ev` —— 这是防自激的**设计**，不是缺失。
- **`move` 只在位置变化时报**：同一位置重复推不算事件（完整版同一规则）。
- 注入与物理同一坐标时会出现两根触点共存（按坐标判定的应用会表现为「等注入结束才弹起」）。
