# vtouch-project

Android 上把**物理触摸**与**注入的虚拟触摸**合成一条触摸流的用户态方案（只需 root）：
`EVIOCGRAB` 抓走真触摸屏 + `uinput` 建合并触摸屏，应用侧只看到一块普通触摸屏。
虚拟手指由 WebSocket 客户端（AutoJs6 / 任意语言）驱动；核心自带原生 ImGui 面板
（只读核心状态 + 输入面板）。**一个可执行、零依赖**：设备上只需要 `vtouchd_ui` 一个文件。

## 项目结构

```
src/                 核心 C 源码（模块化，一起链成一个可执行）
src/vt_internal.h    模块地图 + 共享类型 + struct vt_state g + 各模块原型（先看这个）
src/vtouchd.c        进程：参数 / init / poll 主循环 / 收尾 / main
src/vt_input.c       物理输入（动态认设备 / 读帧）+ 建 uinput 合并设备
src/vt_frame.c       组帧（一次 writev）+ 合帧 / 身份两段 / 转发 / 面板吞触摸
src/vt_ws.c          WebSocket（握手 / 帧解析 / 命令族）
src/vt_region.c      区域表 / 五事件判定 / 区域线程 / 触发绑定与开关（VT_UI 内）
src/vt_ops.c         操作执行器（校验 / 状态机 / 触发槽 / 门控与自动关；VT_UI 守卫内）
src/vt_queue.c       事件队列（SPSC 无锁环）+ 出站队列
src/vt_util.c        小工具（参数解析 / 坐标换算 / 时钟 / 逻辑尺寸探测）
src/vt_shm.{h,c}     共享内存契约（单 memfd 三区：状态只读 · 双向编辑 · 事件环）
src/vt_panel.c       拉起/看护面板子进程（fork+exec app_process）+ 内嵌面板自解包
src-ui/              ImGui 面板（C++）+ JNI 胶水 + 图层/转屏 Java 壳 + 构建入口（操作页 + 触发侧卡片行）
clients/vtouch.js    AutoJs6 客户端 SDK（Finger API：down/move/up/tap/swipe/frame）
clients/*_demo.js    示例：画圆 / 区域五事件 / 命中区域回触
scripts/             构建（build.sh / build_ui.sh）、部署起停（ui-deploy.sh / ui_ondev.sh / deploy.sh）、
                     文档工具（apply_funcdoc.py / funcdoc_data.py）、字库生成（gen_ui_chars.py）
docs/                VTOUCH_ARCH_PLAN.md（方案原文）/ CODE_WALKTHROUGH.md / UI_INTEGRATION.md（UI 接入定稿）
build/               编译产物（不入库）
```

## 构建

交叉编译用 Android NDK r27d（Windows Git Bash）：

```sh
sh scripts/build.sh                 # 默认核心 → build/vtouchd（无面板，零依赖）
sh scripts/build.sh ui              # 带面板核心 → build/vtouchd_ui（面板三件套内嵌进 .rodata）
sh scripts/build_ui.sh              # 只编面板 → build/ui/{classes.dex,libtestimgui.so,libc++_shared.so}
```

- `build.sh ui` 会先要 `build/ui/` 三件套存在（`ui-deploy.sh build` 已按"先面板后核心"排好序）。
- 面板构建失败必须**大声报**（脚本把输出转存日志并打印 error，不再静默中止）。

## 部署与运行

```sh
sh scripts/ui-deploy.sh all      # build → deploy(只推 1 个文件) → start → 自检
sh scripts/ui-deploy.sh stop     # 先停面板、再放 EVIOCGRAB
sh scripts/ui-deploy.sh status   # 核心/面板 pid、面板 fd 卫生、日志尾
```

设备侧等价的一行（**逻辑尺寸不用传**，核心自己问框架）：

```sh
su -c 'cd /data/local/tmp && nohup ./vtouchd_ui >/data/local/tmp/vt_ui_core.log 2>&1 </dev/null &'
```

AutoJs6：把 `clients/vtouch.js`（+ 需要的 demo）推到 `/sdcard/`，在 AutoJs6 里 `require("/sdcard/vtouch.js")`。
只认 `/sdcard` 下的文件为准（导入产生的分叉副本曾多次导致跑到旧副本）。

## 设备侧运行时（现状口径）

- 必需文件只有 `/data/local/tmp/vtouchd_ui`（引擎 + 内嵌面板三件套，启动时自解包）。
- 面板目录：`/data/local/tmp/vtouch-ui/`（核心每次启动**无条件**覆盖解包，日志 `面板自解包 <名> <字节> fnv=`）。
- 区域表落盘：`/data/local/vtouch-runtime/regions.conf`（重启保留；含触发绑定 `bind` / 开关型 `kind` 增量行）。
- 操作表落盘：`/data/local/vtouch-runtime/ops.conf`（`#vtouch-ops v2`：step 行固定 7 字段、ref 空写 `-`；读端兼容 v1 6 字段；面板编辑后存，启动只补缺）。
- WebSocket：`ws://127.0.0.1:27183`（loopback，单客户端，新连接踢旧连接）。
- **没有开机自启**（按用户口径不做）；手机重启后需要重新起核心。

## 操作编辑器 / 执行器 / 触发侧（现状口径）

- **操作**：「操作」页把「点按 / 滑动 / 等待 / 按下 / 弹起 / 区域判断 / 开关判断」7 种步骤编成操作
  （≤16 条、每条 ≤32 步），编辑与运行都在面板里完成；执行器在**核心主线程**（`src/vt_ops.c`）——
  面板崩了已起跑的操作照跑，一次只跑一条、忙时丢弃。
- **变量（触发数据）**：`tdx/tdy/tux/tuy/tms` 运行内只读；字段**负数编码** `-1..-5`（字面值恒 ≥0）；
  完整按压触发全有值 / 按下触发只有 `tdx/tdy` / 手动运行无值；起跑快照不回填；引用无值 → 中止
  `原因=变量无值`；TRACE 级起跑一行 `op 变量 tdx=… tdy=… tux=… tuy=… tms=…`（未设打 `-`）。
- **按下 / 弹起**：按下 = down 并保持、弹起 = 松开（同一虚拟槽）；按住期间只允许 等待 / 区域判断 /
  开关判断 / 弹起，出现 点按 / 滑动 / 按下 → 中止 `槽占用`；没按住先弹起 → `未按下`；
  正常完成 / 中止若还按着 → 自动松开 + `op 收尾 松开`。
- **条件步**：区域判断 = 点 ∈ `ref` 区域（区域不存在 → 中止 `区域不存在`；停用恒不命中）；开关判断 =
  `ref` 须开关型（否则 `非开关型`）、成立 = 开着；不成立两档：中止（默认，`条件不成立`）/ 跳过下一步
  （步序 +1；末步跳过 = 正常完成）—— 日志 `op 条件 <区域判断|开关判断> <ref> 不成立 → <中止|跳过下一步>`。
- **触发侧**（区域 → 操作）：区域卡片三行 `触发 / 时机 / 开关型`（面板编辑，走区 B 编辑邮箱）。
  时机 = 按下（down 命中即触发）/ 完整按压（down 命中锁存、抬起结算一次）；开关型 = 完整按压翻转开/关
  （核心推环行 `toggle_ev <id> <0|1>`）。触发与开关只由**物理手指**产生（虚拟触点不进队列，防自激不破）。
- **门控与自动关**：操作可绑一个开关型区域作门控 —— 起跑前要求「存在 + 开关型 + 开着」，否则
  `op 丢弃 门控拦截`；`跑完自动关` 在**正常完成**时把门控开关翻回关（中止不翻）。
- **落盘**：操作表 = `/data/local/vtouch-runtime/ops.conf`（`#vtouch-ops v2`：step 行固定 7 字段
  `step <type> <a1> <a2> <a3> <a4> <ms> <ref>`、ref 空写 `-`、变量照写负数；**读端兼容 v1** 6 字段行；
  一条 = op 行 + N 条 step 行）；
  触发绑定 / 开关型 = `regions.conf` **增量行**（`bind <区域id> <操作名|-> <down|press>`、`kind <区域id> <0|1>`；
  文件版本不升、旧读方静默忽略）。两表同口径：面板编辑后存、启动加载**区域几何/操作定义只补缺；触发绑定与开关型是面板单源、加载时照灌（不覆盖几何）**、坏记录只跳过单条。
- **契约 v5**：`VT_SHM_VERSION` 4→5（`step` 增 `ref`、触发槽增 6 字段：mask + 按下/抬起坐标与时长）；
  核心/面板版本不符照旧拒绝启动面板。
- **名字规则**：区域 id / 操作名同一把尺子 `[A-Za-z0-9_-]`、1..15，**裸 `-` 除外**（`-` 是邮箱/落盘的
  「解除 / 无门控」哨兵，核心 `vt_id_ok` 拒收）。
- **VT_UI 守卫纪律**：ops / 触发侧代码全部在 `#ifdef VT_UI` 内 —— 默认（无面板）构建**零泄漏**
  （`vt_ops.c` 在默认构建是空 TU）。触碰共享路径（如 `vt_id_ok`）的改动会改变默认核心 md5，
  属**有意**变更 —— 须在报告/提交里记录。
- **日志**：`op ` 前缀族（启动/步/完成/中止/丢弃/编辑/被拒/槽冲突/触发）+ `区域 <id> 开关 → 开|关`；
  v2 增量：`op 条件 <区域判断|开关判断> <ref> 不成立 → <中止|跳过下一步>`、`op 收尾 松开`、中止原因词
  （变量无值 / 槽占用 / 未按下 / 区域不存在 / 非开关型 / 条件不成立）、TRACE 级起跑一行 `op 变量 …`。
  高频明细（滑动每采样点等）默认**不打**，`VTOUCH_OPS_TRACE=1` 才开（首次用到时多一行 `op trace 开`）。
  完整清单见 `docs/OPS_PLAN.md` §8 + `docs/OPS_PLAN_V2.md` §8。

## 约定

- C：`-O2 -Wall -Wextra -Werror`，POSIX + NDK API，`-D_GNU_SOURCE`。
- Shell：`#!/system/bin/sh`（Android shell），不用 bashism。
- JS：AutoJs6 API（`WebSocket.EVENT_*`、`threads.start()`、`events.on("exit")`）。
- **函数文档**：每个函数定义正上方一个 Doxygen 块（含 `(vtouch-doc: 名字)` 机器标记），
  原型上方一句话；文案唯一来源 `scripts/funcdoc_data.py`，改完跑 `python scripts/apply_funcdoc.py`，
  再来一遍必须是"共调整 0 处"（幂等）。**`--check` 有退出码**（0 = 0 处调整且不变式全满足）⇒ 可以当门跑，CI 里就是一道。
- **逻辑尺寸是坐标契约**：区域表 / 区域事件 / 注入命令 / 脚本看到的 `device.width,height`
  全在同一套**竖屏逻辑坐标**里，固定、不随旋转变。取值优先 `-w/-h`，否则核心启动时自己
  问框架（`wm size` → 归一化竖屏）；`wm` 拿不到就**报错退出**，不用内核 sysfs 兜底
  （实测 `/sys/class/drm/card0-DP-1` 会报 2560x5120，拿它当坐标空间会全线错位）。
- **以核心为准**：引擎（设备/grab/uinput/监听/区域线程）全部就绪后才拉面板；面板崩了不影响注入。
- **事件驱动**（2026-09-19 起）：核心空闲睡在 `poll` 上（默认 1s 兜底），三类事件立刻唤醒 —— 物理输入 /
  客户端 / **面板唤醒 pipe**（核心开 pipe、面板持写端 `VTOUCH_WAKE_FD=4`：投编辑·请求停引擎时写 1 字节，
  面板一死读端立刻 EOF ⇒ 核心 ~50ms 收尸并按策略重启）；区域线程同理睡在自己的 `eventfd` 上
  （入队方 `region_q_wake()` 写一次；1s 只是兜底）。空闲全线程上下文切换实测 **~2 次/s**（改前 ~966 次/s）。
  面板侧判「核心死了没有」也是**单调钟 3s**，不是拍数（`vt_shm.c` 的 `vt_shm_ui_tick`）。
- **面板看门狗**（`src/vt_panel.c`）：面板不在时**每 ~3 秒重试拉起一次**（单调钟判据，不是循环拍数）；
  心跳停滞 ~3 秒则杀掉并按策略重启；`waitpid` 按单调钟限频 200ms（唤醒 fd 报 EOF 时每轮都试，最多 2s）；
  **1 分钟窗口内最多 3 次**（`VT_PANEL_MAX_RESTART` / `VT_PANEL_RESTART_WIN`）；
  共享内存没建成（`S_shm_ok` 为假）时**不重试** —— 重试只会沿用同一个坏 fd。
  面板**永远起不来**时不会放弃：稳定态是每 ~60s 再来 3 次（代价只是每轮两条日志，注入不受影响）。

## 坑

- **`/sdcard` noexec**：共享存储不可执行，绝不能直接从 `/sdcard` 跑 ELF —— 先复制到 `/data/local/tmp/`。
- **`/data/local/tmp` 写入**：SELinux Enforcing 下应用侧写不进去（且应用挂载命名空间看不到它），
  必须走 root（`su cp` 或 adb push 到 `/sdcard` 再 `su cp`）。设备侧存在性判断要用 root 回读。
- **`EVIOCGRAB` 必须释放**：核心崩/被杀而 fd 未关，物理触摸就一直是死的。所以收尾顺序是
  **先停面板 → 再放 grab**；面板绝不继承带 grab 的 fd（`FD_CLOEXEC` 硬性项）。
- **反向坑（CLOEXEC）**：父进程给 memfd 设了 CLOEXEC 后，若它恰好就是传给子进程的那个 fd，
  子进程不会走 `dup2`（唯一会清 CLOEXEC 的操作）→ fd 在 exec 时被关掉、随后被 ART 复用
  （实测被复用成 socket，面板拿不到共享内存）→ 子进程里必须 `fcntl(fd, F_SETFD, 0)`。
- **面板依赖 `libc++_shared.so`**：缺它 `System.load` 抛 `UnsatisfiedLinkError`，被 catch 吞掉后
  进程退 0、什么都不干只留一行日志 —— 构建脚本固定交付它，自解包与构建产物做 md5 对账。
- **动态认设备**：绝不写死 `/dev/input/eventX`（扫 event0..63 找 Type-B 多指设备）；
  驱动报的 tracking id 不一定等于槽号（本机 PJZ110 是槽号 +16），别假设。
- **两个坐标空间别混**：核心/区域表/脚本用**竖屏逻辑**（固定）；面板绘制与命中用**当前屏**
  （反射查 `getRotation`/`getRealSize`，转屏时刷新）。绘制尺寸取 `eglQuerySurface`，
  不要用 `ANativeWindow_getWidth/Height`（图层 default 几何，换绑后是陈旧的）。
- **转屏**：核心不感知旋转；面板走**双图层原子翻转**（备用图层按新尺寸准备好、画满两帧后，
  一个事务里旧层 alpha→0 / 新层 alpha→1）。单图层遮挡模式保留为 `VTOUCH_UI_ROT_MODE=hide`。
- **AutoJs6**：`sleep()` 在主线程会堵住 WebSocket 回调（用 `setInterval` 或 `threads.start()`）；
  `new Shell(true)` 初始化慢（~2s），一次性 root 命令用 `shell(cmd, true)`；
  `events.on("exit")` 里要 `stopService()`，强杀不会走退出回调。
- **AutoJs6 脚本不会「跑到结尾」就结束**：只要**还有子线程**或**建过 `setInterval`**，引擎就继续跑
  （实测：30s 定时器被子线程 `clearInterval` 后 40s 仍不结束；子线程全结束才结束）⇒ 收尾要真正结束脚本
  必须显式 `exit()`（它会**照常触发** `events.on("exit")` 钩子，钩子里的写盘/收尾不会丢）。
- **客户端收包不能用 `available()` 判「没数据」**：对端 `close()` 时 Java 的 `available()` 返回 0 而不抛异常
  ⇒ 永远察觉不到掉线（脚本照旧「活着」但事件永不来）。必须**阻塞读 + 读超时**：超时 = 本轮无数据、
  `read()` 返回 -1 = 对端已关（EOF 判据）。
- **单文件新鲜度**：设备上只推 `vtouchd_ui`，面板三件套由核心自解包 —— 版本一致性由
  "同一个二进制"保证，`ui-deploy.sh` 会回读 md5 对账，别手工替换设备上的面板文件。

## 验收自检（不依赖额外脚本）

```sh
sh scripts/ui-deploy.sh status                 # 核心/面板 pid、面板 fd 卫生、日志尾
su -c 'grep -i 6a2f /proc/net/tcp'             # 27183 在听：0100007F:6A2F，状态 0A=LISTEN / 01=有客户端连着
su -c 'ls -l /proc/$(pidof vtouch-ui)/fd'      # 面板：有 memfd:vtouch-shm，无 /dev/input/event*
```

**协议回包别用裸 nc 验**：核心只认 WebSocket 握手，`printf 'res\n' | nc …` 会被判握手失败并关连接
（症状 = 收 EOF、核心日志多一条 `ws 握手失败`）—— 这条自检以前写在文档里，是错的。
要验 `res` 回包就**用客户端连**：推 `build/vtouch_onefile.js` 到 `/sdcard/vtouch.js`，
AutoJs6 里 `require` 后看它日志里的 `res` 行；核心自己的日志也能看出主循环是活的（`engine=on`、
`ws client connected`、手指按压时的 `phys down/up`）。
