# vtouch-project（最小版）

Android 上把**真实手指**和**注入的虚拟手指**合成**一条**触摸流的用户态方案，只需要 root。
一个 C 文件、无 UI、无区域、无插件。

```
src/vtouchd.c        daemon：EVIOCGRAB 抓物理触摸屏 + uinput 合并转发（也接收注入命令）
clients/vtouch.js    AutoJs6 客户端（Finger API：down/move/up/tap/swipe/frame）
scripts/build.sh     NDK 交叉编译（arm64）
scripts/deploy.sh    adb 推二进制到 /data/local/tmp + 起/停/状态（含回读 md5 对账）
tests/ws_smoke.py    主机侧 smoke（adb forward 后跑；握手 + 注入命令链路）
docs/                完整版（面板/区域那一代）的走读与工程图，与最小版无关，可删
                     （最小版的流程图在 docs/diagrams/vtouch-min-*.png）
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

坐标是**竖屏逻辑坐标**（`-w/-h` 那一套）。

## 合并是怎么做的（四点，都在 `src/vtouchd.c`）

1. **抓**：扫 `/dev/input/event0..63` 找 Type-B 触摸屏（认槽/tracking id/XY 四轴），`EVIOCGRAB` 抓走它。
2. **镜像声明**：把物理屏的 EV/KEY/ABS(+absinfo)/props 整份照抄到 uinput 设备，只有 4 处真冲突取相似值
   （tool 量程、槽数、id 池、名字/bus）；`INPUT_PROP_DIRECT` 无条件声明，否则系统会把它当触控板画鼠标指针。
3. **同帧合并**：每个 `SYN_REPORT` 触发一帧：先发待抬触点（`TRACKING_ID=-1`）→ 物理触点 → 虚拟触点 →
   `BTN_TOUCH/BTN_TOOL_FINGER` → `SYN_REPORT`，**整个帧一次 `writev`**。物理与虚拟共用一套下游身份池
   （槽与 tracking id 都重新分配，不透传客户端编号 → 不会撞号）。
4. **失败兜底**：写帧失败绝不丢「抬手」那一帧，置重发标志 5ms 后再发（连续 200 次才认 uinput 真死并退出）。
   客户端挂断/被踢时，抬掉它的虚拟触点并归还帧内身份（否则池会被泄漏的身份占死）。

启动顺序是 **先起监听、最后 grab**：任何失败路径都不会留下「抓着触摸却没人能控制」的状态。
退出码：`2`=尺寸缺失/扫不到触摸屏 · `3`=uinput 建不起来 · `4`=打不开设备 · `5`=grab 失败 · `6`=端口被占。

## 有意删掉的功能（在完整版里）

区域匹配与事件推送、订阅通道、ImGui 面板与 overlay、`regions.conf` 持久化、面板区吞触摸、
旋转坐标换算、出站队列线程、host 侧桩测与产物对账脚本、CI。

完整版连同它的文档、工程图都在 `build/_backup_full_<时间戳>/`
（`files/` 是逐份拷贝，`from_head/` 是 HEAD 里被删前的文件，`RESTORE.md` 写了回滚步骤）；
`docs/CODE_WALKTHROUGH.md` 与 `docs/diagrams/` 讲的也是完整版。

## 已知边界（最小版没处理的）

- **不做旋转换算**：daemon 恒为竖屏坐标系，横屏时由调用方自行换算（完整版有 c2p/p2c）。
- 单客户端：新连接会踢掉旧连接；被踢的一方要自己发现（本客户端不做保活/重连）。
- 注入与物理同一坐标时会出现两根触点共存（按坐标判定的应用会表现为「等注入结束才弹起」）。
