# vtouch-project（最小核心 + 转发引擎 · Plan B）

Android 上把**真实手指**和**注入的虚拟手指**合成**一条**触摸流的用户态方案，只需要 root。
一个 C 文件、无 UI。这一支在最小版之上按 `docs/VTOUCH_ARCH_PLAN.md`（Plan B）重建了引擎：
**「合并 / 转发」与「判断 / 推送」分离** —— 注入路径只往队列里塞事件，判断与推送都在别的路子上，互不阻塞。

```
src/vt_internal.h               模块地图 + 共享类型 + struct vt_state g + 各模块原型
src/vtouchd.c                   进程：共享状态定义 / 参数 / init / poll 主循环 / main
src/vt_input.c                  物理输入（认设备 / 读帧）+ 建 uinput 合并设备
src/vt_frame.c                  组帧（iovec / 一次 writev）+ 合帧、身份两段、转发
src/vt_ws.c                     WebSocket（握手 / 帧解析 / 命令族）
src/vt_region.c                 区域表 / 五事件判定 / 区域线程
src/vt_queue.c                  事件队列（SPSC 无锁环）+ 出站发送队列
src/vt_util.c                   小工具（参数解析 / 逻辑↔raw 换算 / 时钟）
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

## 代码结构（按功能分模块，链成一个可执行）

| 模块 | 职责 |
|---|---|
| `src/vt_internal.h` | 模块地图 + 共享类型 + `struct vt_state g` + 各模块原型（**先看这个**）|
| `src/vtouchd.c` | §1 共享状态定义 + §11 进程（参数 / init / poll 主循环 / 收尾 / main）|
| `src/vt_input.c` | §7 物理输入（认设备 / 读帧）+ 建 uinput 合并设备 |
| `src/vt_frame.c` | §8+§9 组帧（一次 `writev`）+ 合帧、身份两段、转发 |
| `src/vt_ws.c` | §10 WebSocket（握手 / 帧解析 / `cmd_*` 命令族）|
| `src/vt_region.c` | §5+§6 区域表 / 五事件判定 / 区域线程 |
| `src/vt_queue.c` | §3+§4 事件队列（SPSC 无锁环）+ 出站队列 |
| `src/vt_util.c` | §2 小工具（解析 / 坐标换算 / 时钟）|

共享状态只有一份：`struct vt_state g`（`vt_internal.h` 声明、`vtouchd.c` 定义）；各模块私有状态留在
自己的 `.c` 里当 `static`。构建就是 `src/*.c` 一起链（`sh scripts/build.sh`）。

### 函数文档（Doxygen 风格，机器可维护）

**每个函数定义的正上方就是它唯一的文档块**（中间不留空行），一块说完接口契约 + 为什么这么写；
`src/vt_internal.h` 里每个原型上方还有一句话（`@brief` 首句），所以 VSCode 悬停和读源码看到的是同一份说明。
当前 **63/63 个函数**都有文档，脚本会在每次运行后断言这条不变式。

```c
/**
 * (vtouch-doc: emit_frame)     <- 机器标记：脚本靠它判断这块写过没有（所以可反复跑）
 * @brief 把 phys[]/virt[] 合成一帧并提交：待抬 -> 物理 -> 虚拟 -> BTN -> SYN，整帧一次 writev。
 * @return  0 提交成功；-1 提交失败（置 g_reemit，由主循环重发）。
 * @note    身份按下标算（物理 = i，虚拟 = phys_slots + i）；写失败绝不清 pending_up、绝不释放身份。
 *
 * 为什么这么写（原有注释，逐字保留）：
 *   一帧的固定顺序（每一步都有理由）：
 *   ① 待抬的触点先发 ABS_MT_TRACKING_ID=-1 ...
 */
int emit_frame(void)
```

段落横幅（`/* ---- §10.2 命令族 ---- */` 这类章节头）留在文档块上方，不并入。原有注释一律逐字保留，
只是搬进文档块里，所以**文案没丢**。文档的**唯一来源**是 `scripts/funcdoc_data.py`
（函数名 -> brief / params / return / note），改文案后：

```sh
python scripts/apply_funcdoc.py --check   # 先看要改哪些（不改文件）
python scripts/apply_funcdoc.py           # 幂等写入；再来一遍必须是「共调整 0 处」
sh scripts/build.sh                       # 注释改动不该改出机器码
sha256sum build/vtouchd                   # 应与改前一致 —— 这就是「只动了注释」的机器证据
```
### 重构/搬迁这类改动怎么证明没改行为

同一批命令分别打给改动前/后的二进制，把合并设备的事件流按内容逐条比对：

```sh
sh build/_equiv_run.sh pre  build/_vtouchd_v2_flat_handle.bin   # 旧二进制（留档）
sh build/_equiv_run.sh new  build/vtouchd                       # 新二进制
# 比对：事件逐条相同 + 24/24 响应相同（本仓库模块化那轮的结果）
```

## 引擎是怎么做的（六点；实现现在分在 `vt_frame.c` / `vt_region.c` / `vt_queue.c` / `vt_ws.c` 里）

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
   队列里存的是**已经加好 WS 帧头**的完整帧（`outq_push_text()`）：成帧漏在入队这一层，
   客户端会收到裸文本、一帧都解不出来，而注入照常生效 —— 所以这个漏法最不容易被发现。
6. **订阅与踢（§4.6）**：订阅位决定推哪一路；断连/被踢时清订阅位并丢弃残包（不会串给下一个客户端）。

## 合并是怎么做的（四点）

1. **抓**：扫 `/dev/input/event0..63` 找 Type-B 触摸屏（认槽/tracking id/XY 四轴），`EVIOCGRAB` 抓走它。
2. **镜像声明**：把物理屏的 EV/KEY/ABS(+absinfo)/props 整份照抄到 uinput 设备，只有 4 处真冲突取相似值
   （tool 量程、槽数、id 池、名字/bus）；`INPUT_PROP_DIRECT` 无条件声明，否则系统会把它当触控板画鼠标指针。
3. **同帧合并**：先发待抬触点（`TRACKING_ID=-1`）→ 物理触点 → 虚拟触点 →
   `BTN_TOUCH/BTN_TOOL_FINGER` → `SYN_REPORT`，**整个帧一次 `writev`**。身份是**静态两段**
   （物理直接用物理槽号，虚拟从 `phys_slots` 起，见下一节）—— 两段不可能撞号，所以不需要任何避让表。
4. **失败兜底**：写帧失败绝不丢「抬手」那一帧，置重发标志 5ms 后再发（连续 200 次才认 uinput 真死并退出）。
   客户端挂断/被踢时，抬掉它的虚拟触点并归还帧内身份（否则池会被泄漏的身份占死）。

## 身份两段（§5 改：静态分配，不做避让）

物理占一段、虚拟占一段，两段**不可能**撞号，所以「查表找不撞的 id」这一层从设计上就不存在：

| | 槽位 `ABS_MT_SLOT` | tracking id | 结果 |
|---|---|---|---|
| 物理触点 | 物理槽号 `0..phys_slots-1` | **= 槽号** | 原样透传；驱动报的 id 不再搬进系统 |
| 虚拟触点 | `phys_slots + 客户端槽号` | **= 槽号** | 10 槽机器 → `10..19`（物理段被跳过） |

- 合并设备因此要声明 `phys_slots + vslots` 个槽（10 槽机器 = 20 个），tracking id 上限 = 两段之和。
- 删掉整套避让：`alloc_oid / alloc_oslot / id_taken / slot_taken / next_tracking_id / g_seq / evict_newest_virtual`
  （`grep` 计数应为 0）。副产品：物理段永远空着给物理手指，**物理手指不会再被虚拟顶掉**。
- **身份不落字段**：`struct contact` 只存来源状态（原始坐标 + 生命周期），槽位与 tracking id 在发射点按
  下标算出来。所以没有哨兵值、没有分配失败、没有「字段与范围不同步」这类 bug —— 上一代那三个字段
  （`id/oslot/oid`）、`-1` 哨兵、`oid_mod` 与 5 处恒真判据都已删除。
- **真机实测（每条这行都必须自己量一遍）**：OnePlus PJZ110 的触摸屏驱动报的 tracking id 是 **槽号 + 16**
  （`slot0→id16` … `slot5→id21`），不是 0..9。所以「物理 id 就取槽号」成立的前提是**我们重新编号**：
  驱动原始值 16..25 在合并设备流里一次都不该出现（这正是我们要丢掉的）。
- **天花板**：Android 的 pointer id 是 32 位 BitSet（上限 31）。`phys_slots + vslots - 1 ≤ 31` 时 id 原样可见
  （10 + 10 正好到 19）；超了由框架自分配 pointer id —— 功能不坏，但 app 看到的 id 不再等于 tracking id。

验证四步（都可重跑，脚本在 `scripts/` 与 `tests/`）：

```sh
adb push build/vtouchd /sdcard/vtouchd && adb push scripts/ondev-run.sh /sdcard/
adb shell "su -c 'sh /sdcard/ondev-run.sh 900'"      # 起 daemon（900 秒后自杀，不留住物理触摸）
adb forward tcp:27183 tcp:27183
python tests/ws_inject_hold.py --hold 120 &          # 保持一根虚拟触点（id 应为 10）
adb push scripts/ondev-capture.sh /sdcard/ && adb shell "su -c 'sh /sdcard/ondev-capture.sh 300'"
#   ↑ 这 300 秒里用真手指按屏幕（下半部），然后：
python tests/id_split_check.py --require-both        # 断言 id==slot / 两段不重叠 / 无同 id 并发 / 都 ≤31
```

实测那一轮（PJZ110）：9 个触点会话，id 集合 `{0,1,2,10}` —— 物理 `0/1/2`、虚拟 `10`，
其中 `slot0/id0 + slot1/id1 + slot10/id10` 有 **8.1 秒同时在按**；驱动原值 `16..25` 一次未出现。

**行为等价性**（重构这类「只删不放」的改动用它兜底）：同一批 14 条命令分别打给改动前/后的两个二进制，
把合并设备的事件流按内容逐条比对 —— 51/51 行相同、14/14 响应相同（`build/_equiv_drive.py` 可重跑）。

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
- **虚拟段受 pointer id 上限约束**：`phys_slots + vslots - 1 > 31` 后，超出那段的 id 会被框架改成自分配（见「身份两段」）。
- **物理槽数与驱动的 id 偏置是逐设备量的**：别假设驱动 id == 槽号（本机是 +16）；换机器先抓一次原始设备
  （`scripts/ondev-capture.sh`，改设备名前缀即可）再定虚拟起点。
