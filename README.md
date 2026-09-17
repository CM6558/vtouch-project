# vtouch-project —— 物理触摸 × 虚拟触摸合成（用户态，只需 root）

`EVIOCGRAB` 抓走真触摸屏，再用 `uinput` 建一个**合并触摸屏**：物理手指与注入的虚拟手指
合成**同一条**触摸流交给系统，应用侧只看到一块普通触摸屏。虚拟手指由 WebSocket 客户端
（AutoJs6 / 任意语言）驱动；核心自带一块原生 ImGui 面板（只读状态 + 输入面板）。

一个可执行、零依赖（除 Android 系统库）：**设备上只需要 `vtouchd_ui` 一个文件**。

## 目录

```
src/                    核心：进程 / 物理输入 / 组帧 / WS / 区域 / 队列 / 工具 / 共享内存 / 面板拉起
src/vt_shm.{h,c}        共享内存契约（单 memfd 三区：状态只读 · 双向编辑 · 事件环）
src/vt_panel.c          拉起/看护面板子进程（fork+exec app_process），内嵌面板三件套的自解包
src-ui/                 ImGui 面板（C++）+ JNI 胶水 + 图层/转屏 Java 壳 + 构建入口
clients/vtouch.js       AutoJs6 客户端（Finger API：down/move/up/tap/swipe/frame）
clients/example.js      调用示例（注入 / 多指同帧 / 区域订阅 / 生命周期开关）
scripts/build.sh        交叉编译核心（`ui` 目标出带面板的 vtouchd_ui）
scripts/build_ui.sh     编译面板（classes.dex + libtestimgui.so + libc++_shared.so）
scripts/ui-deploy.sh    主机侧一键：build / deploy / start / stop / status（含 md5 对账）
scripts/ui_ondev.sh     设备侧起停与自检（核心/面板 pid、面板 fd 卫生、日志尾）
scripts/deploy.sh       不带面板的最小核心：推二进制 + 起停
docs/VTOUCH_ARCH_PLAN.md  方案原文；docs/UI_INTEGRATION.md  UI 接入定稿
```

## 前提

- 设备已 root（需要 `/dev/uinput` 与 `EVIOCGRAB`）。
- 本机构建：Android NDK（默认 `C:/Users/21102/android-ndk-r27d`，可用 `NDK_ROOT` 覆盖）+ SDK build-tools。
- git-bash / Linux 均可跑脚本（按 `uname` 选工具链后缀）。

## 跑起来

```sh
sh scripts/ui-deploy.sh all      # 编面板 + 编核心（面板内嵌）→ 推 1 个文件 → 起 → 自检
```

设备侧等价的一行（**逻辑尺寸不用传**，核心自己问框架）：

```sh
su -c 'cd /data/local/tmp && nohup ./vtouchd_ui >/data/local/tmp/vt_ui_core.log 2>&1 </dev/null &'
```

停：`sh scripts/ui-deploy.sh stop`（先停面板、再放 `EVIOCGRAB`，物理触摸立刻回系统）。

AutoJs6 侧（`clients/vtouch.js`：**引用即用** —— require 时自动确保 daemon 在跑，
脚本退出（`events.on("exit")`）自动停掉并释放 EVIOCGRAB；daemon 本来就在跑则复用、退出不动它）：

```js
var vt = require("/sdcard/vtouch.js");
var c = vt.connect();                              // 连上就能用
vt.finger().tap(540, 1200);                        // 自动挑空闲 slot
vt.finger(1).down(100, 200).move(140, 240).up();   // 显式 slot（finger()/finger(3)/finger(conn,3) 都吃）
vt.frame([{slot:0,state:"down",x:100,y:200},       // 多指合并进同一帧
          {slot:1,state:"down",x:300,y:200}]);
// 走到结尾 / 按停止 → 自动收尾，不需要你调 vt.stop()
// 需要保留时：vt.keepRunning(true)；不想自动起：require 前 global.VTOUCH_NO_AUTOSTART = true

vt.onRegion("c1", "down", function (h) {           // 区域事件（只由物理手指产生；回调跑在子线程）
    toastLog(h.id + " 被 slot" + h.slot + " 按下 @" + h.x + "," + h.y);
});                                                // 省略事件 = down/up/enter/exit；"*" = 含 move
```

**单文件自包含版**（设备上什么都不用先放）：`python scripts/pack_client.py` 把核心二进制
base64 内嵌进 `build/vtouch_onefile.js`（~3.6MB），推到 `/sdcard/vtouch.js` 后 require 即可 ——
设备上没有该二进制或版本不对时，它会自己写进去并用 md5 校验（当前手机里放的就是这一版）。

## 逻辑尺寸：启动时自动获取

尺寸不是"屏幕多大"，是这套系统的**坐标契约**（区域表 / 区域事件 / 注入命令 / 脚本看到的
`device.width,height` 全都在同一个空间里），必须**固定、不随屏幕旋转变**。

| 情况 | 行为 |
|---|---|
| 传了 `-w 宽 -h 高` | 直接用，跳过探测（最高优先） |
| 没传，`wm size` 可用 | 取最后一个 `WxH`（有 `Override size:` 时它才生效）→ 归一化竖屏（短边当宽） |
| 没传，`wm` 不可用 | 打印原因并**退出**，要求显式 `-w/-h`（不静默用可疑来源） |

`wm size`（= AOSP 公开命令 `/system/bin/wm` → `cmd window size`）与脚本看到的尺寸**同源**。
启动时另有一条交叉校验：触摸屏 raw 量程比与逻辑尺寸比不一致会告警。

## 设备上有什么

```
/data/local/tmp/vtouchd_ui     3.0MB  必需的唯一文件（引擎 + 内嵌面板三件套）
/data/local/tmp/ui_ondev.sh           可选：起停/自检便利脚本
/data/local/tmp/vtouch-ui/*           核心每次启动自己解包，不用手推
/data/local/vtouch-runtime/regions.conf  区域表落盘，重启保留
```

## 生命周期（谁拉起谁）

```
vtouchd_ui(root)
 └─ ① 取逻辑尺寸 → ② 建共享内存（单 memfd 三区）→ ③ 认触摸屏 → EVIOCGRAB → 建 uinput 合并设备
    → 监听 127.0.0.1:27183 → 起区域线程
    → ④ 自解包面板三件套 → fork/exec 面板（app_process … VTouchUI，共享内存 fd 传给子进程）
    → ⑤ 主循环 8ms：读触摸 → 合帧注入 → 吃面板编辑邮箱 → 心跳/看门狗
```

- **以核心为准**：引擎全部就绪后才拉面板；面板崩了不影响注入（看门狗按 3 次/分钟上限重启）。
- **无命令通道**：核心与面板之间只有共享内存（状态只读段 / 双向编辑段 / 事件环），
  唯一的"请求"是面板停引擎（写 `stop_req` + 给校验过的 `core_pid` 发 `SIGTERM`）。
- **fd 卫生是硬性项**：面板绝不继承带 `EVIOCGRAB` 的 fd（`FD_CLOEXEC`；子进程里显式清共享内存
  fd 的 CLOEXEC），否则核心退出后物理触摸回不来。

## 转屏

核心恒为竖屏坐标系、不感知旋转；面板按当前朝向绘制与命中。转屏走**双图层原子翻转**：
转屏时给备用图层按新尺寸准备、画满两帧，然后**一个事务**里旧层 `alpha→0`、新层 `alpha→1`，
SurfaceFlinger 原子提交 → 屏幕无空白。备用方案 `VTOUCH_UI_ROT_MODE=hide`（单图层遮挡换绑）。

## 线协议（loopback WS，一行一条命令，单客户端，新连接踢旧连接）

| 命令 | 应答 | 说明 |
|---|---|---|
| `ping` | `pong` | 探活 |
| `res` | `res <宽> <高> raw <xmin> <xmax> <ymin> <ymax>` | 逻辑尺寸与内核轴量程 |
| `reset` | `ok` / `err frame` | 抬掉全部虚拟触点（帧中途拒绝） |
| `down <slot> <x> <y>` | `ok` / `err point` | 按下（各自成一帧） |
| `move <slot> <x> <y>` | `ok` / `err point` | 移动（各自成一帧） |
| `up <slot>` | `ok` / `err point` | 抬起（各自成一帧） |
| `begin_frame` / `point <slot> <down\|move\|up> <x> <y>` / `end_frame` | `ok` / `err frame` `err point` | 一帧多指 |
| `region add <id> <0矩形\|1圆形> <a1..a4> <0\|1>` | `ok <总数>` / `err region` | rect: `x1 y1 x2 y2`；circle: `cx cy r 0` |
| `region list` | 每行 `region <id> <type> <a1..a4> <en>` + `end <n>` | 表很小（≤32），一次回全量 |
| `region clear` | `ok 0` | 清空 |
| `sub [region]` / `unsub` | `ok` / `err sub` | 订阅区域通道（只有这一条推送） |

推送（单向，混在同一条 WS 里）：`region_ev <id> <ev> <slot> <x> <y>`，`ev` ∈ `down/enter/move/exit/up`。
它**只由物理手指产生** —— 虚拟触点不进转发队列，这就是"回触不会自己触发自己"的保证。

## 坐标与身份

- 坐标是**竖屏逻辑坐标**（上面的尺寸那套）；raw 轴量程由内核给出，转换在 `src/vt_util.c`。
- 身份两段、静态分配：物理触点 `slot = tracking_id = 物理槽号(0..phys_slots-1)`，
  虚拟触点 `= phys_slots + 客户端槽号`（10 槽机器 → `10..19`）。两段不可能撞号，所以没有避让表，
  物理手指也永远不会被虚拟顶掉。
- 天花板：Android pointer id 是 32 位 BitSet（上限 31）；`phys_slots + vslots - 1 > 31` 时
  超出的部分由框架自分配。

## 退出码

| 退出码 | 含义 |
|---|---|
| `2` | 逻辑尺寸拿不到（`wm` 不可用且没给 `-w/-h`）/ 扫不到 Type-B 触摸屏 |
| `3` | uinput 建不起设备 |
| `4` | 打不开触摸设备 |
| `5` | `EVIOCGRAB` 失败 |
| `6` | 端口被占（127.0.0.1:27183） |
| `7` | 区域线程创建失败 |

启动顺序是 **先起监听 → 最后 grab → 区域线程再最后 → 面板最后**：任何失败路径都不会留下
"抓着触摸却没人能控制"的状态。

## 函数文档（Doxygen 风格，机器可维护）

每个函数定义正上方就是它唯一的文档块（`(vtouch-doc: 名字)` 机器标记），`src/vt_internal.h`
里每个原型上方有一句话说明 —— VSCode 悬停与读源码看到的是同一份文案。文案的唯一来源是
`scripts/funcdoc_data.py`：

```sh
python scripts/apply_funcdoc.py --check   # 先看要改哪些（不改文件）
python scripts/apply_funcdoc.py           # 幂等写入；再来一遍必须"共调整 0 处"
```

## 怎么自己验一轮（都在命令行里，不需要额外脚本）

```sh
sh scripts/ui-deploy.sh status                 # 核心/面板 pid、面板 fd 卫生（触摸设备 fd 必须 0）、日志尾
adb forward tcp:27183 tcp:27183
printf 'res\n' | nc -q1 127.0.0.1 27183        # 应回：res 1440 3168 raw 0 23040 0 50688
su -c 'ls -l /proc/$(pidof vtouch-ui)/fd'      # 面板：memfd:vtouch-shm 有、/dev/input/event* 没有
```

## 已知边界

- **单客户端**：新连接踢掉旧连接；被踢的一方要自己发现。
- **区域判定只认物理手指**：虚拟触点不产生 `region_ev`（防自激的设计，不是缺失）。
- **`move` 只在位置变化时报**：同位置重复推不算事件。
- 注入与物理同一坐标时会两根触点共存（按坐标判定的应用表现为"等注入结束才弹起"）。
- **不开机自启**：核心是手工/脚本起进程，重启后需要重新起。
- 面板的层不参与触摸判定：面板矩形由核心按当前朝向换算后在区域判定里吞掉对应触摸。
