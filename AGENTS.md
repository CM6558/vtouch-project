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
src/vt_region.c      区域表 / 五事件判定 / 区域线程
src/vt_queue.c       事件队列（SPSC 无锁环）+ 出站队列
src/vt_util.c        小工具（参数解析 / 坐标换算 / 时钟 / 逻辑尺寸探测）
src/vt_shm.{h,c}     共享内存契约（单 memfd 三区：状态只读 · 双向编辑 · 事件环）
src/vt_panel.c       拉起/看护面板子进程（fork+exec app_process）+ 内嵌面板自解包
src-ui/              ImGui 面板（C++）+ JNI 胶水 + 图层/转屏 Java 壳 + 构建入口
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
- 区域表落盘：`/data/local/vtouch-runtime/regions.conf`（重启保留）。
- WebSocket：`ws://127.0.0.1:27183`（loopback，单客户端，新连接踢旧连接）。
- **没有开机自启**（按用户口径不做）；手机重启后需要重新起核心。

## 约定

- C：`-O2 -Wall -Wextra -Werror`，POSIX + NDK API，`-D_GNU_SOURCE`。
- Shell：`#!/system/bin/sh`（Android shell），不用 bashism。
- JS：AutoJs6 API（`WebSocket.EVENT_*`、`threads.start()`、`events.on("exit")`）。
- **函数文档**：每个函数定义正上方一个 Doxygen 块（含 `(vtouch-doc: 名字)` 机器标记），
  原型上方一句话；文案唯一来源 `scripts/funcdoc_data.py`，改完跑 `python scripts/apply_funcdoc.py`，
  再来一遍必须是"共调整 0 处"（幂等）。
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
