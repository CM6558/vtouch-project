# vtouch 单文件包使用手册（`vtouch_bundle.js`）

手机上只有 **一个文件**：`/sdcard/vtouch_bundle.js`。它内含 headless 后端、ImGui 面板的
`classes.dex` 与 `libtestimgui.so`，以及一层薄 JS API。AutoJs6 `require` 它就能起面板、
注入触摸、读写区域、订阅物理手指事件。设备侧不需要任何 `.sh`、不需要额外 APK。

本文件是这个包的使用手册：**使用细节 + 注意事项**。行号/常量/协议行均取自当前源码与真机回读，
可逐条核对。

| 项 | 值 |
|---|---|
| 生成物 | `clients/vtouch_bundle.js`（753,715 B，md5 `61bb5fd9…`） |
| 唯一来源 | `scripts/build_bundle.py`（822 行；装配在第 811 行） |
| 面板段 md5 常量 | `VTOUCH_UI_DEX_MD5` = `271cf78e…`、`VTOUCH_UI_SO_MD5` = `da1f0940…` |
| 后端 | 面板进程本身就是 daemon：EVIOCGRAB + uinput 合并 + WS `127.0.0.1:27183` + `regions.conf` |
| 设备侧目录 | 部署 `/data/local/tmp/vtouch-ui/`；运行 `/data/local/tmp/vtouch-runtime/` |

---

## 0. 30 秒上手

> 工程图（全流程总览 / 优化前后对照）见 **`docs/diagrams/`**（每张图 = JSON 源 + SVG/PNG，可重渲）。

```js
var vt = require("/sdcard/vtouch_bundle.js");

vt.onRegion("s3", "down", function (h) {       // 区域 id = 面板卡片名（h = {id,ev,slot,x,y}）
    vt.finger().tap(h.x, h.y);                 // 库内自动：uiStart → connect → sub → 读线程
});                                            //          → 过滤 → 回调丢子线程 → exit 收尾 → 保活
```

**事件（第二参）**：**不指定 = `down`/`up`/`enter`/`exit`（默认不含 `move`）**；指定就只传指定的，
多个用 `"down,move"` 或 `["down","move"]`，`"*"`/`"any"` = 全部（含 `move`）。
默认把 `move` 排除的理由是实测的：拖着手指 15 秒 = 104 条 move（§10.1 第 14 条）。
**不限区域**：`vt.onRegion(function (h) { … })` 或 `vt.onRegion(function (h) { … }, "up")`。
**被新实例顶掉时旧实例自退**（并让出面板），不会留下「活着但收不到事件」的僵尸（§4.3）。

就这两行。**「起后端 + 连接 + 订阅 + 常驻读循环 + 解析分发 + 并发保护 + 退出收尾」全在库内**
（`vt.onRegion` `build_bundle.py`，真机实测：脚本启动 → 手指按进区域 → 回调拿到 `{id,ev,slot,x,y}`
→ `tap` 落地；脚本常驻不退出，面板不动）。

需要自己控读循环时（要 `pev` 轨迹、要自己算命中）才用底层写法：
`vt.uiStart()` → `vt.connect()` → `vt.sub(c)` → `while(true){ c.recv(); … }`，见 §9.2 / §9.4 / §9.5
（退出收尾同样自动，不必手写 `events.on("exit", vt.stop)`）。

完整可运行版：`clients/vtouch_region_min.js`（已推到 `/sdcard/vtouch_region_min.js` 与 `/sdcard/脚本/`）。

---

## 1. 一次触碰的完整旅程（从手指按下到 AutoJs6 任务落地）

### 1.1 全景

```
物理手指按下
  │ (1) 触摸屏 /dev/input/eventN 发出 Type-B 帧（ABS_MT_*）
  ▼
daemon 进程（= 面板进程本体；EVIOCGRAB 独占该设备）
  │ (2) physical_events() 解析进 phys[]            ← 此刻系统还看不到这颗手指
  ├─(3) emit_frame() → uinput 重放「物理槽 + 虚拟槽 + SYN」
  │        └────────────→ Android InputReader/Dispatcher（物理触摸照常穿透生效）
  └─(4) region_match() 命中判定 → region_ev_send() ─→ WS 文本帧 @127.0.0.1:27183
                                                          │
                                          (5) conn.recv()（非阻塞，需自己轮询）
                                                          ▼
                                               脚本读循环（必须常驻）
                                                          │ (6) vt.rgParseEv(line) → {id, ev, slot, x, y}
                                                          ▼
                                               业务分发（按 id 判区域 / slot 判手指）
                                                          │ (7) threads.start(...) → vt.finger().tap(x, y)
                                                          ▼
                                        down / move / up 三行命令 → WS → (8) handle_line → set_virtual
                                                          │        → emit_frame() 立即提交一次 SYN
                                                          ▼
                                        uinput → 系统 → 目标 App 收到真实点击
```

要点：**第 3 步和第 4 步是同一帧内的两件事**（都挂在 `SYN_REPORT` 上），所以物理触摸的穿透转发
永远先发生，区域事件是"顺便抄一份给你"，不会因为你脚本慢而拖住物理触摸（除非你不消费 WS 回包，
见 §1.5 第 4 条）。

### 1.2 八个阶段（谁在做 / 代码位置 / 判据）

| # | 阶段 | 执行者 | 关键代码 | 做对了的判据 |
|---|---|---|---|---|
| 0 | 起后端 | 脚本主线程 | `vt.uiStart()`（`vt.onRegion` 内部自动调） | `pidof vtouch-ui` 有值 |
| 0b | 连接 + 订阅 | 脚本 | `vt.connect()` / `vt.sub(c)`（`vt.onRegion` 内部自动做完） | 日志里没有 `NO-CLIENT/UNSUB` |
| 1 | 采集物理触摸 | daemon poll 线程 | `physical_events()` `vtouchd.c:606`，`EVIOCGRAB` `:921` | 静默（无日志） |
| 2 | 合并转发到系统 | 同上 | `emit_frame()` `:246`（末尾一次 `writev` `:277`） | 手指照常操作系统 |
| 3 | 区域命中判定 | 同上，SYN 帧内 | `region_match()` `:436`、`region_hit()` `:372` | 面板上区域圈闪一下 |
| 4 | 推事件给脚本 | 同上 | `region_ev_send()` `:416` → `ws_send()` `:800` | `vtouch-ui.log` 出现 `vtouchd: ev s3 down slot0 720,1584` |
| 5 | 读事件 | 脚本读线程 | `conn.recv()` `build_bundle.py:177`（非阻塞） | 脚本能 print 出 `region_ev …` |
| 6 | 解析 + 分发 | 脚本 | `rgParseEv()` `build_bundle.py:545` | `{id:"s3", ev:"down", slot:0, x:720, y:1584}` |
| 7 | 注入虚拟触摸 | 子线程 | `Finger.tap` `:300` → `vtC2P` `:240` → `down/move/up` | 目标 App 真的响应 |

### 1.3 逐阶段要点

1. **采集**：`ABS_MT_SLOT` 选槽 → `ABS_MT_TRACKING_ID` 定按下/抬起 → `ABS_MT_POSITION_X/Y` 存原始值
   （`:610-620`）。`EVIOCGRAB` 在 init 阶段就抓（`:921`），**抓上之后系统收不到原始触摸**，一切都
   依赖第 2 步转发——这也是"daemon 死了物理触摸就哑"的原因（进程退出 fd 关闭，内核自动解抓）。
2. **转发**：一帧的内容 = 所有 `pending_up` 的 `TRACKING_ID=-1` → 所有按下的物理槽（占 `slot 0…phys_slots-1`）
   → 所有虚拟槽（占 `phys_slots+i`）→ `BTN_TOUCH`/`BTN_TOOL_FINGER` → `SYN_REPORT`，**一次 `writev` 提交**
   （`:251-277`）。物理与虚拟因此在同一个 uinput 设备里天然合并，顺序恒为"先物理后虚拟"。
3. **匹配**：`raw_to_logical()` 把原始轴值按启动参数 `-w/-h`（= `wm size`）归一化到竖屏逻辑坐标；
   `region_hit()` 支持矩形/圆；命中判定用的 `ps_down[i]` 是**上一帧**状态（`broadcast_phys()` 在读循环
   退出后才更新，`:633`），所以"新按下"和"刚抬起"能精确区分；`slot_hit[i][rid]` 保证 `up` 只推给
   "按下时确实命中过"的区域；`move` 只在区域内且坐标变化时推（`:455`）。
4. **推送**：`region_ev_send()` 先调 `vtouch_ev_cb`（同进程面板据此闪圈 + 写日志），再判断
   `client_fd < 0 || !(sub_mask & SUB_REGION)` → 只写一行 `(NO-CLIENT/UNSUB)` 就 return（`pev` 那条门是 `sub_mask & SUB_PHYS`）。**没 `sub` 就一个字节都收不到**（而且可以只订要的通道：`sub region` 的脚本一条 `pev` 都不收）
   （§6.1）。写失败即 `drop_client()`（顺带 `owner_reset()` 把虚拟触点抬掉）。同一帧命中多个区域/多指会推多条。
5. **读**：`recv()` 用 `available() < 2` 提前返回 `null`，所以循环里必须自己 `sleep()` 让位；
   收到 `opcode 8` 会抛"服务端关闭"；`ok`/`region …`/`end`/`pong` 与事件行**同一条流**，`vt.sub` 之后
   `c.watch = true`，库不再自动 `drain()`（`:196`），回包要你自己的循环消费。
6. **分发**：`h.id` 永远是面板里的**最新**名字（daemon 直接取 `rg->id`，§7.1）；`h.ev` ∈
   `down/up/enter/exit/move`；`h.slot` = 物理槽号，用来区分多指；`x,y` 已经是竖屏逻辑坐标。
7. **注入**：`tap()` = `down` → `sleep(60)` → `up`（`:300`）；`vtC2P()` 用 `device.rotation` 把"当前屏
   逻辑坐标"转成竖屏逻辑坐标（`:240`）。`down/move/up` **每行各自成帧**（`:538`、`:550` 各调一次
   `emit_frame()`），只有 `begin_frame/point/end_frame` 才是"多指合并成同一帧"。`up` 先记
   `pending_up`，下一帧才把 `TRACKING_ID` 置 `-1`。

### 1.4 延迟构成

| 环节 | 量级 | 说明 |
|---|---|---|
| 手指 → 内核 → poll → SYN 转发 | 一帧（约 8–16 ms 内） | 同一 `SYN_REPORT` 内联完成，无额外排队 |
| 脚本读到行 | 你的轮询间隔（示例 `sleep(10)`） | 想更快就缩短；纯自旋会占满 AutoJs6 的 Looper |
| 业务线程启动 + `tap` | 60 ms 按住 + 注入 | 按住时长由参数定（`tap(x, y, ms)`） |
| WS 往返（loopback） | < 1 ms | `TCP_NODELAY` 已开（`vtouchd.c:952`） |

端到端（手指按下 → 点击落地）≈ 一帧 + 轮询间隔 + 按住时长。

### 1.5 这条链上的七个断点（按命中频率排）

1. **没订阅** → 日志刷 `(NO-CLIENT/UNSUB)`，脚本收不到任何事件 → 补 `vt.sub(c)`（§6.1）。
2. **脚本没有常驻逻辑** → 脚本"运行结束" → `events.on("exit")` → `vt.stop()` 把后端一起收掉（§4.2）。
3. **开了第二个订阅脚本** → 新连接踢掉旧的并清订阅（`vtouchd.c:947-949`）→ 同一时刻只有一个订阅者。
4. **读循环忙等 / 业务跑在主线程** → WS 回包堆积 → daemon 的 `write_full` 阻塞在 poll 线程 →
   触摸转发变卡（表现为输入延迟）。长任务一律 `threads.start()`。
5. **区域被停用**（`enabled = 0`）→ `:445` 直接 `continue`，该区域永不推事件（面板开关关掉了）。
6. **按住的同时改区域名** → 那一下 `up` 不推（§7.2），下一帧 `enter` 自愈。
7. **后端没在跑 / 换后端后没重连** → `pidof` 为空、`connect()` 超时。

### 1.6 分段自测（一条命令对一个判据）

| 想确认 | 命令 | 期望 |
|---|---|---|
| 后端在跑 | `adb shell su -c "pidof vtouch-ui"` | 一个 pid |
| WS 在听 | 脚本里 `var c = vt.connect()` | 不抛错，返回 conn |
| 订阅生效 | `adb shell su -c "tail -n 5 /data/local/tmp/vtouch-runtime/vtouch-ui.log"` | 有 `vtouchd: ev …`，且无 `NO-CLIENT/UNSUB` |
| 区域命中 | 手指按进区域，看面板 + 上面那条日志 | 区域闪圈 + 一行 `ev <id> down slotN x,y` |
| 注入生效 | 脚本里 `vt.finger().tap(720, 1584)` | 屏上出现一次点击 |
| 物理穿透 | 常规操作系统界面 | 正常；同帧日志还会有 `pev` 行（订阅状态下） |

### 1.7 「区域监听 → 脚本任务」这条链上：哪些步是必需的，哪些是仪式

把八个阶段按「谁必须记住」重排一遍。判据是：**删掉它会不会丢功能**——不丢功能的都是仪式，
应该收进库里（脚本只留业务）。脚本侧的现状写法见 `clients/vtouch_region_min.js`（35 行）。

| 步 | 现状（脚本必须自己写） | 判定 | 为什么 / 能怎么收 |
|---|---|---|---|
| 起后端 | `vt.uiStart()` | **必需** | 面板 = daemon：grab + WS + `regions.conf` 都在它身上（`build_bundle.py:453`） |
| 连接 + 订阅 | `vt.connect()` + `vt.sub(c)` | 必需机制、**仪式化写法** | 不 `sub` 一个字节都收不到（`vtouchd.c:422`）；但这是「打开通道」，不是业务，应由监听入口一次做完 |
| ~~收尾~~ | ~~`events.on("exit")` + `vt.stop()`~~ | **已收进库** | 库在首次 `connect`/`uiStart` 就注册退出钩子（`vt.autoStop(false)` 可关），底层写法也不再漏 |
| 常驻 | 主线程 `while(true){ recv; sleep(10) }` 或 `setInterval` | 必需机制、**不该由每个脚本重写** | 得有人读 socket、有人保活；读线程与保活 `setInterval` 都归库（子线程保不住，见 §4.2） |
| 解析分发 | `vt.rgParseEv(line)` + `h.id === …` + `h.ev === "down"` | **仪式** | 库可做 `onRegion(id, fn)`，id/ev 过滤放在库里 |
| 并发保护 | `threads.start(function () { vt.finger().tap(…) })` | 必需机制、**不该由每个脚本重写** | `tap` 含 sleep，占着读线程会堵住后续事件；分发侧统一丢子线程 |
| 业务 | 真正的任务 | **必需** | 只应该留这一条 |

结论（三个可核对的事实）：

1. 保留「起后端 + 业务」两行是可能的：其余四步（连订 / 收尾 / 常驻 / 分发+并发）是**同一套机制**，
   每个脚本重抄一遍，抄错就是 §1.5 的断点 1 / 2 / 4。
2. `vt.createEngine()` 在「按区域触发任务」的用法里是**冗余的**：daemon 的原生 `region_ev` 已经把
   `id` 与 `down/up/enter/exit/move` 都算好推过来了（`vtouchd.c:436-468`），本地引擎等于把同一份命中判定
   再算一遍；且 `rs` 是闭包快照，改名后回调里的 `r.id` 仍是旧名（§7.1 路线 B 那一行）。
3. ~~还有一层流量上的多余~~（**C 已消掉**：`sub region` 后 daemon 根本不发 `pev`）：`sub` 之后 `pev`（每根手指每帧）与 `region_ev` 走同一条流
   （`vtouchd.c:633` 与 `:422` 同受 `subscribed` 管）。纯区域脚本不消费 `pev`，却要为它付解析成本，
   TCP 缓冲也更容易被顶住（§1.5 第 4 条）。原生侧「按通道选择性订阅」是一处小改动。

优化档位（**A 已实施**，B / C 待拍板；A 不动任何既有 API 的语义）：

| 档 | 改动 | 脚本形态 | 代价 |
|---|---|---|---|
| **A 已实施** | 库内监听入口 `vt.onRegion(id, [ev], fn)`（`build_bundle.py` ON_REGION 段）：`uiStart` → `connect` → `sub region` → 读线程 → 分发（过滤 id/ev、回调丢子线程）→ 退出钩子收尾 → 主线程 `setInterval` 保活；返回 `{stop()}` | `vt.onRegion("s3", function (h) { vt.finger().tap(h.x, h.y); })` | 只增不改；老写法（`sub` + 自写循环）继续可用。验证：主机侧桩测（`tests/onregion_harness.js`，现 **46/46**）+ 真机 PJZ110 回调落值一致 |
| **B 已实施** | A + 启动时用私有探针（`vtouchRegionProbe`）读面板区域表校验 id：**写错 / 被禁用 / 面板一个都没有** 三种都当场 toast；探针没答复则静默（不误报） | 同上，id 写错立刻报错，不再静默 | 一次 `region list`（<600 ms，只在读线程启动前调）。验证：桩测 + 真机 `面板里没有区域 "nope123"；现有：s3, r1` |
| **C 已实施** | A + 原生选择性订阅：`vtouchd.c` 的 `subscribed` → `sub_mask` 位掩码（`SUB_REGION`/`SUB_PHYS`），`sub region` / `sub phys` / 裸 `sub`（两者都订，向后兼容）；`vt.onRegion` 内部用 `sub region` | 语义不变，纯区域脚本一条 `pev` 都不收 | 改 C + 重编面板 `.so`（md5 `da1f09407856db4a373d88c84da8eda5`）+ bundle；真机复核：`sub region` 20 s 得 region_ev=219/pev=0，`sub phys` 得 pev=454/region_ev=0 |

全流程总览图（交付 → 面板进程 → 脚本 → 落地，含物理穿透 / 区域事件 / 虚拟回注三条流）：
`docs/diagrams/vtouch-full-flow.svg`（见该目录 README）。

另有一处**不是仪式而是耦合**：面板框选自动给的 id（`r1/r2/c1…`）要靠人手抄进脚本的 `REGION_ID`
（`clients/vtouch_region_min.js:14`）。根治办法是让脚本「声明自己的区域」（按稳定 id upsert 进面板表），
面板仍可改名 / 停用——这条要不要做，一并拍板。

## 2. 文件与设备布局

| 路径 | 谁写 | 用途 |
|---|---|---|
| `/sdcard/vtouch_bundle.js` | 你（adb push / 构建脚本） | **唯一交付物**。AutoJs6 `require` 的根正本，删了所有调用脚本断链 |
| `/data/local/tmp/vtouch-ui/classes.dex` | `vt.uiStart()` 内嵌释放（root） | 面板 Java 层（`VTouchUI`），`app_process` 的 CLASSPATH |
| `/data/local/tmp/vtouch-ui/libtestimgui.so` | 同上（gz 解压后） | 面板本体：ImGui + `vtouchd.o` 核心 |
| `/data/local/tmp/vtouch-runtime/regions.conf` | **面板**（唯一持有者） | 区域表，首行版本门 `#vtouch-regions v2` |
| `/data/local/tmp/vtouch-runtime/vtouch-ui.log` | 面板 | **只装 stderr**（native 启动日志、ImGui 断言、崩溃），`ALOGI` 去 logcat |
| `/data/local/tmp/vtouch-runtime/vtouch-ui.pid` | `uiStart()` | 只是书签，**不作存活依据**（见 §4.1） |
| `/data/local/tmp/vtouchd` | `vt.install()`（headless 兜底） | 形态 A 后端，见 §3.2 |
| `/data/local/tmp/vtouch-runtime/vtouchd.log` `/vtouchd.pid` | 同上 | headless 后端日志/PID |

`/data/local/tmp` 是 tmpfs：**重启即清空**。重启后第一次 `uiStart()` 会自动重新释放（md5 门会判断），
所以不需要任何开机脚本。

---

## 3. 两个形态与进程模型

同一份 C 源码（`src/vtouchd.c`，995 行，单编译单元）编出两种形态，**后端能力完全相同**：

| | 形态 B（带 UI，日常用） | 形态 A（headless 兜底） |
|---|---|---|
| 产物 | `build/ui/libtestimgui.so`（936,360 B） | `build/vtouchd`（31,968 B） |
| 构成 | `vtouchd.o` + `src-ui/vtouch_ui.cpp` + ImGui，链成 `.so` | 同一个 `vtouchd.c` 直接编可执行 |
| 启动 | `app_process ... VTouchUI $W $H`（Java 层 load so → SurfaceControl composer 图层 → shell 循环） | `nohup /data/local/tmp/vtouchd -w $W -h $H -p 27183` |
| 观察窗口 | 有（悬浮面板） | 无 |
| md5 门 | 有（`vtouchUiDeployed()` 比对 dex/so md5，一致就一个字节不写） | **没有**（只在文件缺失时释放） |

**面板进程跑的就是 daemon 的命令行入口本身**（`src-ui/vtouch_ui.cpp:1499-1503`）：
`char *argv[] = {"vtouch-ui","-w",ws,"-h",hs,"-p","27183",0}; vtouch_set_event_cb(ui_ev_cb); … vtouch_init(7, argv);`
所以脚本 `vt.connect()` 连 27183 时**不区分后端**，切换形态脚本一字不改。

### 3.1 进程与释放

- 面板是独立 `app_process` 进程，**不是 AutoJs6 的插件**：关掉 AutoJs6 它照样活着（但对脚本而言它是"业务进程"，见 §4.2）。
- 逻辑坐标：daemon 坐标空间 = 启动时 `wm size` 归一化后的**竖屏**尺寸（`logical_width/logical_height`），
  注入命令收逻辑坐标、`pev`/`region_ev` 回的也是逻辑坐标（`raw_to_logical()`）。
- 触摸合并：物理触摸经 `EVIOCGRAB` 抓取 → 与虚拟触点一起从 uinput 的 `vtouch-merged` 设备发出 → 下层应用照常收到。
  面板自身**只观测不拦截**（没有 consume 谓词，不置位），代价是面板区域的点按会同时落到下层应用。

### 3.2 形态 A 现在的地位（要不要删）

`vt.ensure()` 第一行就是 `if (vtouchUiAlive()) return;` —— **面板在跑就直接复用，只有面板没起才落回 headless**。
`vt.run()` 内部同样是 `ensure() → connect() → fn() → stop()`，所以只要先 `uiStart()`，`run()` 也不会起 headless。

也就是说：**只用带 UI 的面板时，形态 A 只会被 `vt.ensure()` / `vt.run()` 走到**。它还挂在这些地方：

- bundle 里内嵌的 `VTOUCH_BIN_SIZE/B64`（42,432 B base64，占包体 5.7%）+ `vtouchInstall()`；
- CI（`.github/workflows/build.yml`）编 arm64 **与 x86_64** 两版 vtouchd，出两个 bundle ——
  x86_64/AVD 只有 headless 能跑（面板 `.so` 是 arm64-only）；
- `AGENTS.md` / `README.md` 的命令行说明。

删掉它可省 42,432 B，并同时消掉 §13 的隐患 #1。改法（约 -30 行 `build_bundle.py` + -12 行 workflow）：
删二进制常量与 `vtouchInstall()`，`vtouchEnsure()` 直接转调 `vtouchUiStart()`。
**语义不变**（还是"保证有个后端在跑"），唯一可见变化是 `vt.run()` 从静默无窗口变成会弹面板。
（此项待拍板，未实施。）

---

## 4. 生命周期契约（三条 + 自动收尾）

### 4.1 存活判定只认 `pidof`

`app_process --nice-name=vtouch-ui` 之后 `/proc/<pid>/comm` 是 `main`、`cmdline` 只剩 nice-name：

- ✅ `pidof vtouch-ui` → `vt.uiAlive()` / `vt.uiPid()`
- ❌ `killall vtouch-ui`（按 comm 匹配，永远不命中）
- ❌ 遍历 `/proc/*/comm` 扫进程
- ❌ pid 文件（`vtouch-ui.pid` 只是书签：有进程没文件、有文件没进程都可能）

库里的清理是 `vtouchKillAll()`：`kill -9 <pids>` → 等 150 ms → 复查，最多 8 轮，清不掉就抛错。

### 4.2 面板的存活绑定脚本生命周期

`vt.stop()`（或脚本 `exit`）会把面板一起收掉 —— 这是设计（脚本结束即释放 EVIOCGRAB）。
后果：**任何调用 `uiStart()` 的脚本都必须有常驻逻辑**，否则：

```
uiStart() → 面板起来 → 脚本没有 while/setInterval → 运行结束（≈2 s）
          → events.on("exit") → vt.stop() → 面板被杀 → 肉眼"窗口根本不弹"
```

库侧有守卫：面板起来不到 2 s 就被收，会 `toastLog` 喊
`面板起来不到 2s 就被脚本退出收掉了：脚本末尾缺少常驻逻辑（读循环 / setInterval）`。

常驻写法二选一：

- 主线程读循环（`while (true) { … sleep(10) }`，见 §0）；
- 主线程 `setInterval(...)`（**主线程**；子线程 `while(true)` 保不住，主脚本结束会被连带掐掉）。

**现在默认就是自动收尾**：库在第一次 `connect()` / `uiStart()` 时注册（只注册一次）一个退出钩子——
脚本一结束就 `vt.stop()`（收面板 + 释放 `EVIOCGRAB`）。所以 `uiStart` / `connect` / `sub` 的底层写法
**不需要再手写** `events.on("exit", vt.stop)`；`vt.onRegion` 走的也是同一个钩子
（`HANDOVER` 时不动面板，见 §4.3）。

只想"起面板、干完事就走"（面板留着继续用）才需要逃生门：

```js
vt.autoStop(false);     // 退出只关连接；面板留着（代价：物理触摸一直被它抓着）
vt.autoStop();          // 恢复默认（退出手收面板）
```

**强杀例外**：`am force-stop` / 用户在系统里"强行停止"跳过 exit 事件，钩子不会执行，面板会留在后台
抓着触摸——此时手动 `kill -9 $(pidof vtouch-ui)`。

### 4.3 单客户端：新连接踢旧连接

daemon 只有一个 WS 客户端位。新连接会 `kicking old ws client` 并把旧的 `subscribed` 清 0。
所以**同时只能有一个脚本在收事件**，两个订阅脚本一起跑会互踢（表现为事件忽有忽无）。

用 `vt.onRegion` 起的脚本**已经处理了这条**：旧实例每 2 s 发一条 `ping`、6 s 收不到 `pong` 就判定断线
（不能指望读异常——`recv()` 用 `available()` 判断，对端只 `close()` 时它会永远返回 `null`），
发现通道被顶掉且面板还活着 → 判定为"被接管" → `toastLog` 提示后 `vtouchListenStop()` 并 `exit()` 自退，
**不动面板**（`HANDOVER` 标志让 `exit` 钩子跳过 `vt.stop()`）。判据是"面板进程还在"，
所以面板自己死掉的那种断开仍然照实报「事件通道已断开」，不会误判成接管。

底层写法（自己 `sub` + 读循环）仍需手动"一进一出"：跑新脚本前先停旧的，否则旧实例会变成
活着的僵尸，之后你停它还会把新实例正在用的面板一起收走。

---

## 5. API 全表

`module.exports` 共 32 项（`build_bundle.py:350-382`）。凡标"阻塞"的调用都要在**业务线程**跑，主线程调会卡 Looper。

### 5.1 后端与连接

| API | 说明 |
|---|---|
| `vt.uiStart()` | 起面板。幂等（已在跑直接 `true`）。流程：清残留 → `uiDeploy()` → `nohup app_process … &` → 轮询 `pidof`（24×150 ms）。起不来抛错并附日志尾部。实测 ~452 ms |
| `vt.uiStop()` | `kill -9` 面板 + 删 pid 文件，返回是否已停 |
| `vt.uiRestart()` | `uiStop()` + `uiStart()` |
| `vt.uiAlive()` / `vt.uiPid()` | 面板存活 / 首个 pid（无则 `null`） |
| `vt.uiDeploy()` | 释放内嵌 dex/so。**返回 `true` 表示这次真的写了**（md5 不符），`false` = 设备已是最新。写后复查 md5，不符抛错 |
| `vt.uiTail(n=20)` | 读 `/data/local/tmp/vtouch-runtime/vtouch-ui.log` 尾 n 行（排障用） |
| `vt.ensure()` | 幂等启动后端：面板在跑直接返回；否则按 pid 判断 headless 是否在跑；都没有则释放并拉起 headless（不等端口） |
| `vt.install()` | 释放 headless 二进制（一般不手调，`ensure()` 内部会调） |
| `vt.connect(timeout=10000)` | 连 WS 并完成握手，**阻塞**（失败每 300 ms 重试到超时）。返回 `conn`，同时设为"当前连接" |
| `vt.run(fn)` | 全包入口：唤醒屏幕 / 保活 60 s / `ensure()` / `connect()` / `fn()` / 关连接 / `stop()` / `exit()`，内部在子线程跑。**结束会收面板** |
| `vt.stop()` | 关当前连接；面板在跑就收面板；再 `kill vtouchd` + 删 pid。**这是"释放 EVIOCGRAB"的开关** |
| `vt.autoStop(on?)` | 退出自动收尾开关（默认 `true`；首次 `connect`/`uiStart` 自动注册退出钩子）。`vt.autoStop(false)` = 脚本结束时只关连接、面板留着；`vt.autoStop()` 恢复默认 |
| `vt.HOST` `vt.PORT` `vt.BIN` `vt.UI_DIR` | 常量：`127.0.0.1` / `27183` / `/data/local/tmp/vtouchd` / `/data/local/tmp/vtouch-ui` |

`conn` 对象：

| 成员 | 说明 |
|---|---|
| `c.send(text)` | 发一帧 masked 文本（内部 `SEND_LOCK` 串行，多线程写安全）。若未订阅会先 `drain()` 排空回包 |
| `c.recv()` | **非阻塞**读一条文本消息；`available()<2` 返回 `null`。收到关闭帧抛 `服务端关闭` |
| `c.drain()` | 排空已到的回包（长会话防 TCP 缓冲撑满） |
| `c.close()` | 关 socket 并清"当前连接"（**不会**收面板） |
| `c.watch` | 订阅标志，库内部用（`sub` 置 `true` 后发包不再 `drain`） |

### 5.2 触摸注入

| API | 说明 |
|---|---|
| `vt.finger(slot?)` | 取 Finger 对象。不传 slot = **自动分配** 0~9 中第一个未按下的（都占满抛 `无空闲 slot`）；显式传 0~9 |
| `f.down(x,y)` / `f.move(x,y)` / `f.up()` | 单点注入，链式返回 `this`。`move`/`up` 在未按下时是 no-op |
| `f.tap(x,y,ms=60)` | down → `sleep(ms)` → up |
| `f.swipe(x1,y1,x2,y2,ms=300)` | down → 以 16.7 ms 步进插值 move → up（**含 sleep，必须在业务线程/子线程**） |
| `f.frame(state,x,y)` | 该手指单点成帧（内部 `vt.frame`） |
| `f.state()` | `"down"` / `"up"` |
| `vt.frame([{slot,state,x,y},…])` | 多点同帧下发：`begin_frame` → 每个 `point` → `end_frame`。`state ∈ {down,move,up}` |
| `vt.reset()` | 清 daemon 侧虚拟触点状态（异常中断后复位用） |

坐标：`down/move/up/point` 传的都是**当前屏幕的逻辑坐标**（与 `device.width/height` 同量纲），
Finger/frame 内部已做 `c2p` 换算，脚本不用管旋转。

### 5.3 旋转坐标

| API | 说明 |
|---|---|
| `vt.rot()` | `Display.getRotation()` → 0/1/2/3 |
| `vt.c2p(x,y)` | 当前屏逻辑 → 竖屏逻辑（注入前；Finger 内部已调） |
| `vt.p2c(x,y)` | 竖屏逻辑 → 当前屏逻辑（**画图/对屏幕坐标时才需要**） |

daemon 坐标系恒为竖屏；区域一律存竖屏坐标，native 匹配不动。
验证状态：R1（90°，`PJZ110` 横屏真机闭环，tap 落点误差 ≤1 px）、R2（180°，无歧义）、R3（镜像推导，待真机复核）。

### 5.4 区域（读写面板的表）

| API | 说明 |
|---|---|
| `vt.rgParseEv(line)` | `region_ev` 行 → `{id, ev, slot, x, y}`；非该行返回 `null`。**纯事件路线，id 永远最新** |
| `vt.createEngine(rs, handlers)` | 本地引擎：吃 `pev` 行（`eng.feed({slot,action,x,y})`）自己算命中，回调 `(region, finger)`。`handlers` 键：`onDown/onUp/onMove/onEnter/onExit`。`eng.setRegions(rs)` 换表、`eng.fingers()` 列当前手指 |
| `vt.rgPush(c, rs)` | 整表下发：`region clear` + 逐条 `region add`，面板收到即生效并落盘 |
| `vt.rgList(c)` | 回读面板当前表，**600 ms 超时**，失败/无连接返回 `[]`。**必须在开读包循环之前调**（它会消费回包行） |
| `vt.sub(c, mode?)` / `vt.unsub(c)` | 开/关事件通道（见 §6.1）。**`mode` 选通道**：省略 = `region`+`phys` 都订（老语义）；`"region"` = 只收区域事件（`vt.onRegion` 用的就是它）；`"phys"` = 只收原始轨迹。`sub` 会先 `drain()` |
| `vt.parseEv(line)` | `pev` 行 → `{slot, action, x, y}`；非 `pev` 行返回 `null` |

区域对象格式（两种，坐标都是**竖屏逻辑**）：

```js
{ id: "s3", name: "s3", x1: 100, y1: 200, x2: 500, y2: 900, enabled: true }        // 矩形
{ id: "c1", name: "c1", type: "circle", cx: 700, cy: 1200, r: 120, enabled: true } // 圆形
```

`name` 只活在脚本侧（线协议只有 id + 几何），所以 `rgList()` 回的 `name` 就等于 `id`。

### 5.5 事件（daemon → 脚本）

| 行 | 何时推 | 解析 |
|---|---|---|
| `region_ev <id> <down\|up\|move\|enter\|exit> <slot> <lx> <ly>` | 物理手指在区域内的状态变化 | `vt.rgParseEv(line)` |
| `pev <slot> <down\|move\|up> <lx> <ly>` | 物理手指原始轨迹（订阅后） | `vt.parseEv(line)` |
| `ok` / `ok N` / `err …` / `pong` / `res …` / `region …` / `end N` | 命令回包 | 一般忽略（`rgList` 内部消费） |

| `vt.onRegion(id, [ev], fn)` | 不是行协议，是**库侧监听入口**：内部完成 uiStart + connect + sub + 读线程，只把匹配的 `region_ev` 交给 `fn(h)`（回调在子线程）。`id` 省略=所有区域；**`ev` 省略=`down`/`up`/`enter`/`exit`（不含 `move`）**，指定只传指定的（`"up"`/`"move"`/`"down,move"`/数组；`"*"`/`"any"`=全部含 move） |

**两个维度分工**：`id` = 哪个区域，`slot` = 哪根手指（0~9）。
**只报物理手指**：虚拟触点不产生 `pev`/`region_ev`，所以"回触自己"不会自激。

---

## 6. 线协议（WS 文本，`ws://127.0.0.1:27183`）

一条命令 → 一条响应行（`vtouchd.c:471 handle_line()`）：

| 命令 | 响应 | 说明 |
|---|---|---|
| `ping` | `pong` | 探活（读线程每 3 s 一次） |
| `res` | `res <lw> <lh> raw <axmin0> <axmax0> <axmin1> <axmax1>` | 查逻辑尺寸与 raw 轴范围 |
| `reset` | `ok` | 清虚拟触点状态 |
| `sub [region\|phys\|all]…` / `unsub` | `ok` / `err sub` | **事件通道总开关**，`sub_mask` 位掩码唯一来源：不带参数 = 两个通道都订（老客户端语义不变），`sub region` 只订区域事件、`sub phys` 只订原始轨迹 |
| `down <slot> <x> <y>` | `ok` | 逻辑坐标，越界 → `err point` |
| `move <slot> <x> <y>` | `ok` | 同上 |
| `up <slot>` | `ok` | 成帧前（`begin_frame` 与 `end_frame` 之间）调用 → `err point` |
| `begin_frame` / `point <slot> <state> <x> <y>` / `end_frame` | `ok` | 多点同帧 |
| `region clear` | `ok <n>` | 清空 |
| `region list` | 每条 `region <id> <type> <a1> <a2> <a3> <a4> <en>` + 结尾 `end <n>`；空表 `ok 0` | 回读 |
| `region add <id> <type> <a1> <a2> <a3> <a4> <en>` | `ok <n>` / `err region` | 必带 9 个 token，多一个即 `err` |

- 矩形：`type=0`，`a1..a4 = x1 y1 x2 y2`（`a1 ≤ lw-1`、`a4 ≤ lh-1`）。
- 圆形：`type=1`，`a1 a2 a3 a4 = cx cy r 0`（`a3` 上限 100000；第 5 个几何位是占位 0）。
- `id ≤ 15` 字符（`REGION_ID_MAX`），最多 **32** 个区域（`MAX_REGIONS`）；越界 → `err region`。

### 6.1 `sub` 为什么是必须的（不是可选优化）+ 通道选择

```c
/* src/vtouchd.c:416 */
static void region_ev_send(const char *id, const char *ev, int slot, int lx, int ly) {
    if (vtouch_ev_cb) vtouch_ev_cb(msg);          /* 同进程面板照样闪 */
    if (client_fd < 0 || !subscribed) {           /* ← 没订阅就只打 stderr */
        fprintf(stderr, "vtouchd: ev %s %s slot%d %d,%d (NO-CLIENT/UNSUB)\n", …);
        return; }
    ws_send(client_fd, 1, msg, n);
}
```

`subscribed` 只有客户端发 `sub` 才置 1（`unsub`/断开/被新客户端踢掉清零）。
不 `sub` → daemon 内部匹配照跑（日志里能看到 `ev s3 down slot0 …`），**但脚本一个字节都收不到**。
同一个开关也打开 `pev`（`broadcast_phys(client_fd >= 0 && subscribed)`），
所以纯区域脚本的读循环只留 `rgParseEv`、把 `pev` 行丢掉即可。

**日志判据**：面板日志里 `ev <id> …` 不带 `(NO-CLIENT/UNSUB)` = 有脚本连着且订阅着；
全是该标记 = 没有订阅者（排查"脚本收不到事件"先看这个）。

---

## 7. 区域系统

- **唯一归属是面板**：`/data/local/tmp/vtouch-runtime/regions.conf`。脚本侧不落库、不在 AutoJs6 `storages` 里存区域。
  这样不存在第二份状态，也不会把旧版本存下来的区域再捞回来。
- **版本门**：首行必须是 `#vtouch-regions v2`；缺失或版本不符 → 整份丢弃并立刻改写成空表（防"旧版本区域残留"）。
- **落盘由渲染线程单写者执行**：WS/面板改表只置 `g_save_pending`，避免两线程并发写。
  症状参考：脚本下发后 `regions.conf` 只剩版本头 = 钩子没接落盘。
- **上限 32 个**，id ≤15 字符、面板保证唯一（重名直接拒并红字提示）。

### 7.1 两条"按 id 分发"的路线

| | 路线 A：`rgParseEv`（推荐） | 路线 B：`createEngine` |
|---|---|---|
| 数据源 | daemon 原生 `region_ev`（带 id） | `pev` 原始轨迹 + 脚本侧命中判定 |
| 脚本要不要存区域表 | 不要 | 要（`rs` 快照） |
| 命中判定 | daemon（native，零 JS 开销） | JS（每点 × 每区域） |
| **运行中改名后 `id`** | **永远最新** | **旧名**（闭包快照，只有 `setRegions()` 能换） |
| 适合 | 按 id 分发、UI/逻辑与区域一一对应 | 需要自定义命中（重叠区域优先级、区域外手势等） |

要"永远读到最新数据"就只走路线 A：不存表、不算命中，`h.id` 直接是面板里的当前名字。

### 7.2 改名的真实副作用（只在"按住的同时改名"可见）

面板改名 = `region clear` + 逐条重加（`src-ui/vtouch_ui.cpp:924-940`），
而 `regions_clear()`（`vtouchd.c:325-332`）会连 `slot_in/slot_hit` 一起清零：

- 好处：索引不错位、不会出幽灵 `enter/exit`；
- 代价：**改名那一刻正按住的手指，之后的 `up` 不会推**（`slot_hit` 已清零，不满足推送条件）；
  下一帧会补一个 `enter` 自愈，所以只丢那一个 `up`。

### 7.3 `rgList` 的两个坑

1. **必须在开读循环之前调**：它自带 600 ms recv 循环，会和读线程抢行。
2. **紧跟 `rgPush` 后调用会返回 `[]`**（`ok N` 回包被当成区域行）：库里已加 `drain()`，
   脚本侧仍要兜底 `if (!rs.length) return;` —— 否则整表下发会**清空用户手绘的区域**。

---

### 7.4 消费侧：谁在收、有多快（实测）

事件由服务端（daemon）生成，客户端**没有**内置自动派发器——`module.exports` 里没有 `on(...)`／注册回调，
库只给你「解析函数 + 可选本地引擎」，循环归脚本自己写。这是有意的：AutoJs6 只有主线程 Looper，
库若自己起监听线程会和你的 `sleep`／业务时序打架（也违背"仪式收进库、业务只写触摸"的边界）。

| 层 | 入口 | 谁维护状态 | 适用 |
|---|---|---|---|
| 原始事件行（推荐） | `vt.sub(c)` + `conn.recv()` + `vt.rgParseEv(line)` | 无（daemon 已在服务端判命中） | 按 id 分发；**id 永远最新**（§7.1） |
| 本地引擎 | `vt.createEngine(rs, handlers)` + `eng.feed(vt.parseEv(line))` | 引擎（`inside`/`fingers`），自己算命中 | 需要在脚本侧改区域语义／算自定义命中；`rs` 是快照，改名不跟（§7.1） |
| 写侧 | `vt.rgPush(c, rs)` / `vt.rgList(c)` | 面板（`regions.conf`） | 脚本下发/回读区域表；`rgList` 内含 `drain()`，会吃掉排队中的事件（§7.3） |
| 原语 | `vt.sub/unsub`、`conn.drain()`、`vt.parseEv` | — | 开关通道、排空回包、解析 `pev` 行 |

随包发布的消费者实例：`clients/vtouch_region_min.js`（`rgParseEv` 路线）、`clients/vtouch_touchback.js`（订阅 + 注入）。

**实测（本机 node v22，各 1e6 次，`build/_parse_bench.js`）**

| 调用 | 单次耗时 | 折算 |
|---|---|---|
| `rgParseEv("region_ev s3 down slot0 720 1584")` | 92.6 ns | ~1080 万行/秒 |
| `rgParseEv("region_ev c8 move slot1 336 2655")` | 108.2 ns | ~920 万行/秒 |
| `rgParseEv("pev 0 move 720 1584")`（非区域行快退） | 17.9 ns | ~5600 万行/秒 |
| `vt.parseEv("pev 0 move 720 1584")` | 52.1 ns | ~1900 万行/秒 |

Rhino 比 V8 慢一个量级，按 5–20× 估约 0.5–2 µs/行，仍是**每秒几十万行**级别——远超触摸屏报点率
（120 Hz → 8.3 ms/帧，240 Hz → 4.2 ms/帧）。所以**解析不是瓶颈，读循环的节奏才是**。

**延迟分层（谁决定什么）**

| 环节 | 决定因素 | 量级 |
|---|---|---|
| 手指 → 命中判定 → 写 socket | 触摸屏报点率（每个 `SYN_REPORT` 一次匹配，事件与物理转发同帧内联发出，无队列/无定时器） | 4–8 ms |
| socket 传输（loopback，服务端已 `TCP_NODELAY`） | — | < 1 ms |
| 脚本收到 | **你的读循环间隔**（示例 `sleep(10)`） | 你定 |
| 业务线程启动 | `threads.start` | 亚毫秒～几毫秒 |
| `tap` 自身 | 按住时长（默认 60 ms） | 60 ms |

- 结论：端到端延迟基本由「读循环间隔 + 按住时长」决定；想更跟手就把外层 `sleep` 缩到 5–10 ms 并保证内层把队列排空。
- **无丢弃、无队列**：TCP 缓冲写满后 daemon 的 `write_full` 会阻塞在 poll 线程上 → 物理触摸转发跟着变卡（不是丢事件，是拖慢触摸）。所以消费者必须跟得上。
- **保序**：单 poll 线程顺序写，帧内顺序 = slot 升序 × 区域表索引；TCP 保序，无乱序/重复。但**事件不带时间戳**，要时间精度就用收到时刻的 `Date.now()`。
- **丢数据的三个真实场景**：① `rgList()` 内部的 `drain()` 会连带吃掉排队中的 `region_ev`（§7.3）；② `ws_send` 失败 → `drop_client()`，脚本侧表现为"从此收不到事件"，需要自己发现；③ 新客户端连上 → 踢旧并清订阅（§4.3）。
- **保真度边界**：只报物理手指；只有 `down/up/enter/exit/move` 五类；不携带压力/面积/倾斜；坐标是整数逻辑坐标；`enter/exit` 只在按住期间产生。
- 已知可优化项：客户端 socket 未设 `TCP_NODELAY`（`vtouchConnectOnce` 只设了 `SO_TIMEOUT`），突发小包（`frame` 多点、`move` 步进）在 Nagle 下可能被推迟；加一行 `sock.setTcpNoDelay(true)` 即可消除。

## 8. 面板使用说明

窗口默认 **864×1180**（`TITLE_H 88`、侧栏 `SIDE_W 256`），只吃**物理触摸**（合成触摸不算）。

| 操作 | 位置 | 说明 |
|---|---|---|
| 移动面板 | **标题栏**（唯一拖动区） | 内容区/侧栏/列表拖动是滚动，不是拖窗 |
| 收起 / 展开 | 标题栏最右自绘图标按钮 | 收起 = 224×68 半透明悬浮条（只留绿点 + `n/32`）。**收起态不持久化，每次启动完整展开** |
| 框选新区域 | 侧栏「框选工具」→ 矩形 / 圆形 | 默认 id 自动给：矩形 `r1/r2…`、圆形 `c1/c2…` |
| 改 id | **点区域卡片上的名字** | 面板内自绘触摸键盘（composer 图层收不到系统 IME）：字符集 `[A-Za-z0-9_-]`、≤15 字符、与其它区域重名被拒并红字提示，确定即写 `regions.conf` |
| 移动区域 | 先点卡片「选中」，再在框内拖动 | 四角黄块缩放 |
| 区域卡片四键 | 停用/启用、取消/选中、显示/隐藏、删除 | |
| 滚动区域列表 | 列表区上下拖（24 px 死区） | 死区内抬起 = 点击，不影响卡片按钮 |
| 区域事件日志 | 侧栏「页面」→ 事件日志 | 显示 `region_ev` 行 |
| 设置 | 侧栏「页面」→ 设置 | 叠加层显隐、操作说明 |
| 退出面板 | 侧栏操作键 → 退出 | 走 `vtouch_cleanup(); _exit(0)`，**同时释放 EVIOCGRAB** |

面板上的区域/改名会立刻生效（下一帧 daemon 事件就是新 id）；只有"按住的同时改名"会丢一个 `up`（§7.2）。

---

## 9. 脚本骨架（可直接抄）

### 9.1 最小：区域触发点击（正本，= 现在唯一推荐写法）

```js
var vt = require("/sdcard/vtouch_bundle.js");

vt.onRegion("s3", "down", function (h) {   // h = {id, ev, slot, x, y}
    vt.finger().tap(h.x, h.y);             // 回调在子线程：里面可以直接写 sleep/长按/拖拽
});

// 事件过滤：不指定 = down/up/enter/exit（不含 move）；指定 = 只传指定的
// vt.onRegion("s3", function (h) { … });              // 默认集（不含高频 move）
// vt.onRegion("s3", "up", function (h) { … });        // 只看抬起
// vt.onRegion("s3", "down,move", function (h) { … }); // 显式要 move（数组也行）
// vt.onRegion("s3", "*", function (h) { … });         // 全部（含 move）
// vt.onRegion(function (h) { … }, "up");              // 不限区域，只要抬起
// var handle = vt.onRegion(…) ;  handle.stop();       // 中途停监听（不断面板）
```

库里替你做完的六件事：`uiStart`（面板 = 后端）→ `connect` → `sub` → 常驻读线程 →
`rgParseEv` + id/ev 过滤 → 回调丢子线程（`tap` 含 sleep 不会堵读线程）；再加两条生命周期：
脚本退出自动 `vt.stop()`（释放 EVIOCGRAB）、主线程 `setInterval` 保活（脚本不会 2 秒就退）。

启动时会自动校验 `id`：面板里**没有这个区域 / 该区域被禁用 / 面板一个都没有** 三种都会立刻
`toastLog` 指出（私有探针读 `region list`，只读线程启动前调一次；探针没答复则静默，不误报）。

见 `clients/vtouch_region_min.js`（真机正本）与 §0。

### 9.2 区域 → 多指判定（id × slot）

```js
var vt = require("/sdcard/vtouch_bundle.js");
vt.uiStart();
var c = vt.connect(); vt.sub(c);            // 退出自动收尾由库注册，不必手写 exit 钩子

while (true) {
    var h = vt.rgParseEv(c.recv() || "");
    if (!h) { sleep(10); continue; }
    if (h.ev === "down") toastLog(h.id + " 被 " + h.slot + " 按下");
    if (h.ev === "up"   && h.slot === 0) toastLog(h.id + " 由 0 号手指抬起");
}
```

### 9.3 区域 → 长按（自己控制时序，不用 hold API）

```js
var f = vt.finger();                       // 自动拿空闲 slot
f.down(h.x, h.y); sleep(800); f.up();      // 按住 800 ms（sleep 在业务/子线程）
```

### 9.4 区域 → 拖拽（把区域当摇杆）

```js
if (h.id === "pad" && h.ev === "down") {
    var f = vt.finger(h.slot);
    f.down(h.x, h.y); f.move(h.x, h.y - 300); sleep(120); f.up();
}
```

### 9.5 订阅物理轨迹（路线 B：本地引擎）

```js
var rs = vt.rgList(c);                     // 只能在这里调（开读循环之前）
if (!rs.length) toastLog("面板里还没有区域");
var eng = vt.createEngine(rs, {
    onDown: function (r, f) { toastLog("down " + r.id + " slot" + f.slot); },
    onUp:   function (r, f) { }
});
while (true) {
    var line = c.recv(); if (!line) { sleep(10); continue; }
    var p = vt.parseEv(line); if (p) eng.feed(p);   // region_ev 行忽略即可
}
```

> 走路线 B 后，面板改名不会同步到 `eng`（`r.id` 是旧名）。要最新就改回 `rgParseEv`，或收到变更时 `eng.setRegions(vt.rgList(c))`（注意 §7.3）。

### 9.6 脚本下发区域（面板/脚本两侧都能改）

```js
var rs = [
    { id: "tapR", x1: 1000, y1: 1500, x2: 1300, y2: 1800, enabled: true },
    { id: "cA", type: "circle", cx: 400, cy: 2400, r: 150, enabled: true }
];
vt.rgPush(c, rs);          // region clear + 逐条 add，面板立即生效并落盘
```

---

## 10. 注意事项

### 10.1 脚本侧

1. **必须有常驻逻辑**（§4.2），否则面板起来 ~2 s 被自己收掉，表现为"窗口不弹"。
2. **`vt.sub(c)` 不能省**（§6.1）；不订阅 = 收不到任何事件，且日志会给出 `(NO-CLIENT/UNSUB)` 判据。
3. **触摸注入放子线程**：`tap/swipe/长按` 含 `sleep`，占用读线程会让事件积压（`threads.start` 或 `vt.run`）。
4. **同一时刻只有一个订阅脚本**（§4.3）。重跑前先停旧实例；两个脚本互踢时表现为事件忽有忽无。
5. `rgList` 只在开读循环前调，并 `if (!rs.length) return;` 兜底（§7.3）。
6. 想要"改名后脚本立刻看到新 id" → 走 `rgParseEv`，不要在脚本里存区域表（§7.1）。
7. **坐标**：注入用当前屏逻辑坐标（库内已 `c2p`）；事件给的是竖屏逻辑坐标，
   横屏时要在屏幕上画图/对齐请用 `vt.p2c()`。
8. `slot` 0~9；`vt.finger()` 自动分配只在"未按下"里找，未 `up` 的手指会一直占槽。
9. **回触不会自激**：虚拟触点不产生 `pev`/`region_ev`，所以"区域触发点击"不会自己再触发一次。
10. `vt.stop()` 才是收面板/释放 grab 的开关；`c.close()` 只关连接。**退出自动收尾默认开着**
    （第一次 `connect`/`uiStart` 就注册好钩子），底层写法不用再写 `events.on("exit", vt.stop)`；
    要留面板用 `vt.autoStop(false)`（§4.2）。
11. `vt.run(fn)` 结束会 `stop()`（收面板）——需要面板常驻就别用 `run()`。
12. `vt.ensure()` 在面板活着时不会再起 daemon；面板活着时别调 headless 那条路。
13. **走 `vt.onRegion` 时第 1–4 条已由库接管**：它自动 `uiStart` + `connect` + `sub` + 常驻读线程 +
    回调丢子线程 + 退出收尾 + 保活。此时**不要再自己写 `while(recv)`**——同一个 socket 两个读者会抢帧。
14. `vt.onRegion` **不指定事件 = `down/up/enter/exit`（默认不含 `move`）**，指定 = 只传指定的，
    多个用 `"down,move"` / 数组，`"*"` = 全部。实测：在 s3 里拖 15 秒收到 104 条 `move` —— 
    这就是 `move` 不进默认集的原因；要轨迹（画线、摇杆）才显式加。
    回调只吃物理手指（虚拟触摸不产生事件，回触不自激）。
15. 返回句柄 `handle.stop()` 只摘掉这一个回调；**最后一个摘掉才停监听**（`unsub` + 关连接），
    且不会收面板——面板归脚本退出时的 `exit` 钩子（`vt.stop()`）。

16. **重跑不用先停旧实例**：新实例一连接，daemon 就踢掉旧连接；旧实例（`vt.onRegion` 起的）发现
    自己被顶掉后**自动让位退出**（`exit()`，`toastLog` 会说「已被新的订阅者接管」），并且**不收面板**——
    面板归新实例。所以「双活互踢 → 面板被先退的那个收掉 → 另一个变聋」这条老坑已经消掉。

### 10.2 进程与部署侧

1. 存活判定只认 `pidof vtouch-ui`，杀进程用 `kill -9 $(pidof vtouch-ui)`（§4.1）。
2. **改 `.so` 后必须停面板再推**：面板在跑时覆盖 `.so`/`.dex` 报 `Text file busy`，push 静默失败。
3. **`adb push` 写不进 `/data/local/tmp/`**（adbd 是 shell 用户，报 `stat failed … Permission denied`）：
   先推 `/sdcard/<stage>/`，再 `su -c 'cp -f … && chmod 644 …'`；推完 `md5sum` 回读对账。
4. **不要给启动命令套 `sh -c '…'`**：后台子进程会被 su 会话收掉（日志里初始化跑完、进程随即静默消失）。
   直接把整条 `nohup app_process … & echo $!>pid` 交给 root shell（库内就是这么做的）。
5. **AutoJs6 侧 `shell(cmd, true)` 三条铁律**：命令要短、不含循环、不含管道；不要 `2>/dev/null`；
   `adb shell su -c "cmd | grep …"` 会**挂死**（su 只跑单条无管道命令，过滤放本机做）。
6. `/data/local/tmp` 是 tmpfs，`ls` 常显示 `????` —— 存在性只认内容回读（`md5sum`/size），不认 `ls`。
7. **中文路径脚本拉不起来**：`RunIntentActivity` 吃不了 `/sdcard/脚本/x.js`（实测 22 s 无反应），
   外部起脚本一律用 ASCII 路径，中文目录里的只能用户在 App 里点。
8. 面板日志只装 stderr：`ALOGI` 去 logcat，`vtouch-ui.log` 里没有不代表代码没执行。

### 10.3 面板侧（改 UI 时）

1. **一帧渲染两份内容**（overlay + 面板）：凡"可被交互改写的全局标志"参与 `Push/PopStyleColor`，
   必须帧内取快照（`const int min_bg = g_min;`），否则 `PopStyleColor() too many times` 断言 → 面板 abort。
2. 面板空转跳帧会吞掉从 `build_panel` 里设的 `g_need`：要连续帧请用 `g_force_frames`。
3. 退出必须 `_exit(0)`（看门狗循环下普通 flag 退不出，层和 grab 全留）；`kill -9` 的孤儿层由 SurfaceFlinger 回收。
4. 滚动/拖动一律"快照侧几何 + 推送式 `SetNextWindowPos`"，禁用 ImGui 原生拖拽（轮询鼠标是瞬移式的）。
5. 面板只观测不拦截是**用户铁律**：不要为了好点而打开 consume 谓词。
6. 验交互必须走真实手指：合成点按/self-test 驱动器已全删，构建只能验渲染、验不了帧内状态切换。

### 10.4 调试侧

1. **查脚本日志不要用 `logcat -s GlobalConsole:I`**：`log()`/`toastLog` 是 D 级，会被过滤掉，
   看起来像"代码没执行"。用 `adb logcat -d | grep GlobalConsole`。
2. 本机 logd 有进程配额限流，脚本日志可能被静默吞：定位启动失败改用分步落盘探针。
3. `execute_code` 有 5 分钟硬上限：`build_ui.sh` + `build_bundle.py` 这类长构建放 `terminal` 跑。
4. 位移/滚动/跟手这类断言不要目测（叠加层动画、子窗裁剪会连环误判），一律数值化 + 同态对照。
5. 改完二进制/脚本，**先对 md5 再下结论**：设备上跑的还是旧产物是最常见的一类"修了没效果"。

---

## 11. 故障排查表

| 症状 | 先查 | 常见原因 → 处理 |
|---|---|---|
| 面板不弹 / 一闪就没 | `logcat -d \| grep GlobalConsole` 里是否有 `运行结束 (用时 ≈2 秒)` | 脚本没有常驻逻辑 → 加 `while`/`setInterval`（§4.2） |
| 脚本结束后面板还在、触摸被它抓着 | 脚本里有没有 `vt.autoStop(false)`；或是不是被强杀（`force-stop`） | 强杀跳过 exit 事件 → 手动 `kill -9 $(pidof vtouch-ui)`；正常退出应自动收 |
| 面板连不上 / 起不来 | `vt.uiTail(20)`、`pidof vtouch-ui` | 端口被旧 headless 占（先 `uiStop()`）、`.so` 释放失败（看 md5 报错） |
| 脚本收不到区域事件 | 面板日志里有没有 `(NO-CLIENT/UNSUB)` | 用底层写法时没 `sub(c)`；`vt.onRegion` 写法下先查 **id 与面板卡片名是否一致**（不传 id 可收全部）；或另一个脚本把连接抢了（单客户端） |
| 用了 `vt.onRegion` 却一次都没回调 | 面板日志有没有 `vtouchd: ev <id> down` 行 | 有 ev 行 = 事件到了 daemon 侧，问题在 id 不匹配（改名/写错）；连 ev 行都没有 = 手指没按进区域（`enabled` 开关也要看） |
| 事件忽有忽无 / 脚本莫名退出 | 是否有两个订阅脚本；logcat 有没有「已被新的订阅者接管」 | 互踢 → 用 `vt.onRegion` 时旧实例会自退（正常）；底层写法要手动只留一个 |
| 启动就提示 `面板里没有区域 "x"；现有：…` | 面板卡片名 vs 脚本里的 id（B 的启动校验） | 照提示改名或改脚本；`enabled=0` 会另外提示「已被禁用」 |
| 启动提示 `面板里还没有区域` | 面板区域表 | 先在面板画一个，或脚本 `vt.rgPush` 下发 |
| 按了区域但触摸没到应用 | 注入是否在子线程、`c.recv()` 是否被阻塞 | `tap/swipe` 的 `sleep` 占了读线程；用 `threads.start` |
| 点了区域触发两次 | 是否同时用了 `createEngine` 和 `rgParseEv` | 两条路都喂会重复处理 → 只留一条 |
| 改区域 id 后脚本还用旧名 | 走的哪条路线 | `createEngine` 快照 → 换 `rgParseEv`（§7.1） |
| `regions.conf` 只剩版本头 / 区域被清空 | 脚本是否整表下发 | `rgList` 返回 `[]` 后没兜底 → `if (!rs.length) return;`（§7.3） |
| 面板里 0 条区域，但脚本"下发过" | `cat /data/local/tmp/vtouch-runtime/regions.conf` | 版本门丢弃旧格式；或落盘钩子没接 |
| 推了新的 `.so` 却没变化 | `md5sum /data/local/tmp/vtouch-ui/libtestimgui.so` | `Text file busy`/未 kill 面板；推 `/data/local/tmp` 被 SELinux 拒 → 走 `/sdcard` 中转 |
| 面板点按落到了下层应用 | 这是设计 | 面板不拦截触摸（穿透），需要吞触摸才置 consume 谓词 |
| 物理触摸"死"了 | `pidof vtouch-ui` / `pidof vtouchd` | grab 进程崩了没重启：EVIOCGRAB 随 fd 关闭释放；重起面板或用 `vt.stop()` |
| 横屏下点击位置偏 | `vt.rot()` / 是否用 `vt.p2c` | 注入已自动换算；自己画图/比较坐标要 `p2c` |

---

## 12. 构建与部署

### 12.1 构建

```sh
# 面板 + dex（PC / Git Bash）；产物 build/ui/classes.dex、build/ui/libtestimgui.so
sh scripts/build_ui.sh

# 单文件包；产物 clients/vtouch_bundle.js
python scripts/build_bundle.py
python scripts/build_bundle.py --check     # 只做 node --check

# 只要 headless 形态（可选）
NDK=C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin
"$NDK/aarch64-linux-android24-clang.cmd" -O2 -Wall -D_GNU_SOURCE src/vtouchd.c -o build/vtouchd
```

`build_ui.sh` 四步：`javac -encoding UTF-8` → `d8 --min-api 24 build/ui/classes/VTouchUI*.class` →
NDK 编 `vtouchd.o` + imgui 4 个 cpp + `vtouch_ui.cpp`（全部 `-fPIC`）→ `-shared -lEGL -lGLESv2 -landroid -llog -lm` → `llvm-strip`。
（Win 上 `javac` 必须带 `-encoding UTF-8`，否则中文注释按 GBK 报错；`.so` 对象漏 `-fPIC` 会 lld 报错。）

`build_bundle.py` 装配（第 622 行）：

```python
write_out(OUT, CORE + "\n" + blob + "\n" + uiblob + "\n" + ONE_LIB + "\n" + UI_BOOT + "\n" + DEMO)
```

| 段 | 内容 | 行 |
|---|---|---|
| `CORE` | 手写 WS 客户端 + headless 启动 + 旋转坐标 + Finger/frame/run | 27–337 |
| `blob` | `VTOUCH_BIN_SIZE/B64`（同一条 C 源码 headless 产物） | 599–603 |
| `uiblob` | `classes.dex`（b64 原样）+ `libtestimgui.so`（gz 9 → b64）+ 两个 md5 常量 | 611–621 |
| `ONE_LIB` | 区域层 `rgHit/rgPush/rgList/rgParseEv/rgCreateEngine` | 487–582 |
| `UI_BOOT` | 面板生命周期 `uiStart/uiStop/uiRestart/uiAlive/uiPid/uiDeploy/uiTail` | 376–475 |
| `DEMO` | 实为 `module.exports`（31 键），命名有误导 | 339–373 |

二进制占 86.2%，JS 本体约 100 KB。`so` 走 **gz+b64**（936 KB → 441 KB → b64 588 KB）而非裸 b64（1.25 MB）；
base64 按 76 列分行（`_b64_lines()`），因为单行 30 KB 会被中间设备/WAF 拦（403）。

### 12.2 部署

```sh
# 只推一个文件；面板二进制由 uiStart() 自己释放
adb push clients/vtouch_bundle.js /sdcard/vtouch_bundle.js
adb shell su -c "md5sum /sdcard/vtouch_bundle.js"        # 回读对账（设备=构建机）

# 起脚本（ASCII 路径！）
adb shell am start -n org.autojs.autojs6/org.autojs.autojs.external.open.RunIntentActivity \
  -d file:///sdcard/vtouch_region_min.js
```

**注意**：`/sdcard/vtouch_bundle.js` 是根正本（`require` 用的绝对路径）；AutoJs6 脚本目录里的是副本，
两份会分叉 —— 改完示例脚本要把副本一起换掉，否则用户点的是旧副本（先 `mv` 备份到 `/sdcard/.vtouch_backup_<日期>/`，不要 `rm`）。

### 12.3 状态对账（三条回读命令）

```sh
adb shell su -c "md5sum /sdcard/vtouch_bundle.js"                    # 包是不是最新
adb shell su -c "pidof vtouch-ui"                                     # 面板在不在跑
adb shell su -c "cat /data/local/tmp/vtouch-runtime/regions.conf"    # 区域表（首行版本门）
```

---

## 13. 现状对账与已知隐患

| 项 | 当前值 |
|---|---|
| `vtouch_bundle.js` | 753,715 B，md5 `61bb5fd9e2d93316249fc58273da9849`（设备 = 本机，已回读对账） |
| `libtestimgui.so` | 936,360 B，md5 `da1f09407856db4a373d88c84da8eda5`（设备 = bundle 内嵌，已回读对账） |
| `classes.dex` | md5 `271cf78e…`（设备 = bundle 内嵌） |
| 区域表 | `#vtouch-regions v2` + 用户手绘 `s3` + 脚本补的 `swipeL` / `tapR` |

已知隐患 / 待决（均未实施）：

1. **headless 缺 md5 门**：`vtouchEnsure()` 只在文件缺失时释放（`[ -x $D ] || exit 11`），
   设备上 `/data/local/tmp/vtouchd` 是 `cc23b987…` 而当前 bundle 内嵌 `7631c857…` ——
   改了 `src/vtouchd.c` 重建 bundle 后设备上仍是旧二进制，且**完全静默**。
   修法：`build_bundle.py` 多写 `VTOUCH_BIN_MD5`，`vtouchEnsure()` 加 `md5sum` 比对（约 6 行）。
   （删掉形态 A 则此隐患一并消失，见 §3.2。）
2. **改名丢一个 `up`**：按住手指的同时改区域 id，那一下 `up` 不推（§7.2）。要彻底消掉需在
   `region_changed()` 里给订阅客户端推一行变更通知。
3. **引擎路线不感知改名**：`createEngine` 的 `rs` 是快照（§7.1）。若想让引擎也自动跟上，
   可在收到变更通知后 `eng.setRegions(vt.rgList(c))`（需配合隐患 2 的推送）。
4. **R3 旋转未真机复核**：R1 已闭环（≤1 px），R2 无歧义，R3 是镜像推导。
5. **面板收起态验收未闭环**：需要真人手指点收起/展开后读日志对账（面板只吃物理触摸，合成触摸不算）。
