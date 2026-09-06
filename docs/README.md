# vtouch：Android 虚拟触摸控制器

## 1. 项目定位

`vtouch` 用于在具有 root/KernelSU 权限的 Android 设备上创建一个独立的虚拟触摸屏，并为第三方程序提供本机调用接口。

当前目标设备已验证环境：

```text
设备：OnePlus PJZ110
系统：Android 16 / SDK 36
内核：6.6.144 / arm64
屏幕：1440 x 3168
SELinux：Enforcing
提权：KernelSU
```

当前实现重点是：

- 使用 Linux `uinput` 创建虚拟输入设备；
- 不直接向真实触摸屏的 event 节点写入事件；
- 使用 Unix Domain Socket 提供第三方调用接口；
- 支持单指、多指、点击、滑动、缩放和长连接；
- 支持客户端触点归属和断开后的触点清理。

> 当前实现适合 root 自动化和自有测试环境。不能保证适用于所有国产 ROM，也不能把虚拟设备伪装成物理硬件或规避应用安全检测。

---

## 2. 总体架构

```text
Auto.js / C / C++ / 其他第三方程序
                │
                │ Unix Domain Socket
                │ /data/local/tmp/vtouch.sock
                ▼
        vtouchd root 守护进程
                │
                │ Linux uinput
                ▼
        vtouch-virtual 虚拟触摸屏
                │
                ▼
        Android EventHub / InputReader
                │
                ▼
        InputDispatcher / 目标应用
```

真实触摸屏使用另一条独立路径：

```text
真实触摸面板
      │
      ▼
/dev/input/event8（当前设备）
      │
      ▼
Android InputReader
```

虚拟触摸屏当前通常会出现在另一个 event 节点，例如：

```text
/dev/input/event12
```

event 编号可能在重启或设备重建后变化，第三方程序不能依赖这个编号，必须调用 socket。

---

## 3. 每一步的实现原理

### 3.1 打开 `/dev/uinput`

`uinput` 是 Linux 输入子系统提供的用户空间虚拟输入设备接口。root 守护进程打开：

```text
/dev/uinput
```

之后可以向内核声明设备名称、总线类型、输入事件类型和绝对坐标轴。

### 3.2 声明设备能力

当前设备声明：

```text
EV_KEY
EV_ABS
EV_SYN
BTN_TOUCH
BTN_TOOL_FINGER
INPUT_PROP_DIRECT
```

多点触摸轴：

```text
ABS_MT_SLOT          0..9
ABS_MT_TRACKING_ID   0..65535
ABS_MT_POSITION_X    0..1440
ABS_MT_POSITION_Y    0..3168
ABS_MT_TOUCH_MAJOR   0..255
ABS_MT_PRESSURE      0..255
ABS_MT_TOOL_TYPE     MT_TOOL_FINGER
```

`INPUT_PROP_DIRECT` 表示直接触摸设备，Android 会将其倾向于识别为触摸屏。

`BTN_TOOL_FINGER` 和 `ABS_MT_TOOL_TYPE` 很重要。缺少它们时，当前 ColorOS/Android 16 曾经把事件识别成 `ACTION_HOVER_MOVE`，导致目标 UI 不响应。

### 3.3 创建虚拟设备

守护进程通过 `UI_DEV_SETUP` 和 `UI_DEV_CREATE` 创建设备：

```text
名称：vtouch-virtual
总线：BUS_VIRTUAL
vendor：0x1234
product：0x5678
```

Android 发现设备后，EventHub 将其交给 InputReader。InputReader 再根据设备属性、轴和配置把它分类为：

```text
TOUCH | TOUCH_MT
Sources: TOUCHSCREEN
DeviceType: TOUCH_SCREEN
```

### 3.4 发送单指事件

一次按下的底层事件顺序：

```text
ABS_MT_SLOT = 0
ABS_MT_TRACKING_ID = 新 ID
ABS_MT_POSITION_X = x
ABS_MT_POSITION_Y = y
ABS_MT_TOUCH_MAJOR = 20
ABS_MT_PRESSURE = 255
ABS_MT_TOOL_TYPE = MT_TOOL_FINGER
ABS_X = x
ABS_Y = y
BTN_TOUCH = 1
BTN_TOOL_FINGER = 1
SYN_REPORT
```

移动时只更新坐标，然后发送：

```text
SYN_REPORT
```

释放时：

```text
ABS_MT_TRACKING_ID = -1
BTN_TOOL_FINGER = 0
BTN_TOUCH = 0
SYN_REPORT
```

`SYN_REPORT` 表示一帧输入事件结束。Android 在收到完整帧后才会向上层生成对应的 `MotionEvent`。

### 3.5 多指事件

每个触点使用不同 slot 和 tracking ID：

```text
slot 0 -> tracking ID 101
slot 1 -> tracking ID 102
slot 2 -> tracking ID 103
```

当前 `down`、`move`、`up` 命令各自产生一个 `SYN_REPORT`。因此多个触点可以同时存在，但当前命令协议还不是“真正的一帧内批量提交”。

### 3.6 守护进程和 socket

`vtouchd` 启动后：

1. 创建虚拟设备；
2. 等待约 350 ms，让 InputReader 发现设备；
3. 创建 Unix Domain Socket；
4. 监听客户端连接；
5. 为每个客户端创建处理线程；
6. 使用互斥锁串行写入 uinput；
7. 客户端断开时释放该连接创建的触点。

Unix Domain Socket 是本机进程间通信通道，不是 TCP 端口，不经过网络。

默认路径：

```text
/data/local/tmp/vtouch.sock
```

请求和响应按行处理：

```text
请求：tap 720 1584 60\n
响应：ok\n
```

长连接可以连续发送多条命令：

```text
ping\n
down 0 300 500\n
move 0 400 600\n
up 0\n
```

每条请求必须以换行结束，并读取对应的一行响应。

### 3.7 客户端 owner

每次 socket 连接都有一个 owner ID。触点创建时保存 owner：

```text
slot 0 -> owner 7
```

后续 `move` 和 `up` 只有同一个 owner 才能执行。客户端断开时，服务端遍历 slot，释放属于该 owner 的触点。

这解决了客户端异常退出后触点残留的问题。

---

## 4. 手机端文件

```text
/data/local/tmp/vtouchd       root 守护进程
/data/local/tmp/vtouchctl     命令行客户端
/data/local/tmp/vtouch.sock   Unix socket
/data/local/tmp/vtouchd.log   守护进程日志
```

本地项目文件：

```text
vtouchd.c                 守护进程源码
vtouchctl.c               命令行客户端源码
vtouchd.production        当前 arm64 守护进程构建产物
vtouchctl.production      当前 arm64 客户端构建产物
autojs_vtouch_example.js  Auto.js 示例
README.md                 本文档
```

---

## 5. 启动和停止

### 5.1 启动

```sh
su -c 'rm -f /data/local/tmp/vtouch.sock'
su -c 'nohup /data/local/tmp/vtouchd -x 1440 -y 3168 >/data/local/tmp/vtouchd.log 2>&1 </dev/null &'
```

等待约 1 秒后检查：

```sh
su -c '/data/local/tmp/vtouchctl ping'
```

期望：

```text
pong
```

### 5.2 停止

```sh
su -c '/data/local/tmp/vtouchctl reset'
su -c 'killall vtouchd'
```

停止后应确认没有残留进程：

```sh
su -c 'ps -A | grep vtouchd'
```

### 5.3 紧急复位

```sh
su -c '/data/local/tmp/vtouchctl reset'
```

异常、脚本中断、应用崩溃或手势未完成时，优先执行此命令。

---

## 6. 快速诊断

```sh
su -c 'id'
su -c 'getenforce'
su -c 'ls -l /dev/uinput'
su -c 'ps -A | grep vtouchd'
su -c '/data/local/tmp/vtouchctl ping'
su -c '/data/local/tmp/vtouchctl res'
```

检查虚拟设备：

```sh
su -c 'getevent -lp | grep -A20 -B2 vtouch-virtual'
```

应包含：

```text
BTN_TOOL_FINGER
BTN_TOUCH
ABS_MT_SLOT 0..9
ABS_MT_TOOL_TYPE
ABS_MT_TRACKING_ID
INPUT_PROP_DIRECT
```

检查 Android 分类：

```sh
su -c 'dumpsys input | grep -A60 -B5 vtouch-virtual'
```

重点确认：

```text
Sources: TOUCHSCREEN
DeviceType: TOUCH_SCREEN
Enabled: true
```

检查输入日志：

```sh
su -c 'logcat -d -v brief | grep -E "ACTION_(DOWN|MOVE|UP|HOVER)|deviceId="'
```

正常触摸应出现 `ACTION_DOWN/MOVE/UP`，不应再出现旧问题中的 `ACTION_HOVER_MOVE`。

---

## 7. 命令协议

### 7.1 健康检查

```text
ping
```

响应：

```text
pong
```

### 7.2 查询坐标范围

```text
res
```

响应：

```text
1440x3168
```

### 7.3 点击

```text
tap <x> <y> [duration_ms]
```

示例：

```sh
su -c '/data/local/tmp/vtouchctl tap 720 1584 60'
```

### 7.4 触点控制

```text
down <slot> <x> <y>
move <slot> <x> <y>
up <slot>
```

示例：

```text
down 0 300 500
move 0 400 600
move 0 500 700
up 0
```

当前范围：

```text
slot：0..9
x：0..1439
y：0..3167
```

### 7.5 滑动

```text
swipe <x1> <y1> <x2> <y2> [duration_ms]
```

示例：

```sh
su -c '/data/local/tmp/vtouchctl swipe 200 2000 900 2000 300'
```

### 7.6 缩放

```text
pinch <cx> <cy> <gap1> <gap2> [duration_ms]
```

示例：

```sh
su -c '/data/local/tmp/vtouchctl pinch 720 1584 200 400 300'
```

### 7.7 复位

```text
reset
```

当前 `reset` 是全局紧急复位接口，会释放所有虚拟触点。普通客户端不要在其他客户端正在执行手势时调用它。

### 7.8 标签

```text
42:tap 720 1584 60
```

响应：

```text
42:ok
```

标签只用于请求和响应配对，不改变触摸逻辑。

---

## 8. 长连接调用

### 8.1 命令行客户端

启动：

```sh
su -c '/data/local/tmp/vtouchctl --interactive'
```

随后逐行输入：

```text
ping
 down 0 300 500
move 0 400 600
up 0
```

实际输入时不要在命令前加空格。

### 8.2 为什么高频操作要用长连接

短连接每次命令都要：

```text
创建 socket -> connect -> 发送 -> 读取响应 -> close
```

高频轨迹会产生额外开销，也会和断开自动清理产生冲突。长连接只建立一次连接，适合：

- 连续轨迹；
- 多指操作；
- Auto.js；
- 高频测试。

### 8.3 客户端断开行为

如果长连接客户端执行：

```text
down 0 300 500
```

然后异常退出，服务端会自动发送释放事件。短连接不适合将 `down`、`move`、`up` 拆成三个独立进程调用，因为 `down` 完成后连接关闭，触点会被自动清理。

---

## 9. Auto.js 使用教程

### AutoJs6 推荐方式：root shell

AutoJs6 文档中的 `shell(cmd, root)` 会以 root shell 执行命令并同步返回 `{code, result, error}`。直接从 AutoJs6 进程访问 `/data/local/tmp/vtouch.sock` 可能被 Android 沙箱或 SELinux 拒绝，因此本项目推荐让 AutoJs6 通过 root shell 调用 `vtouchctl`，而不是直接 new `LocalSocket`。

最小测试：

```javascript
const r = shell("/data/local/tmp/vtouchctl ping", true);
if (r.code !== 0 || String(r.result).trim() !== "pong") {
    throw new Error(String(r.error || r.result));
}
toast("vtouchd 已连接");
```

点击：

```javascript
const r = shell("/data/local/tmp/vtouchctl tap 720 1584 60", true);
if (r.code !== 0 || String(r.result).trim() !== "ok") {
    throw new Error(String(r.error || r.result));
}
```

单次 `tap` 和 `swipe` 使用 `shell(command, true)` 很方便。多指或长轨迹不要把 `down`、`move`、`up` 分成多个独立的 `shell` 调用，因为每次 `shell` 都会创建并结束一个 shell 进程；应使用一次 root shell，把命令通过 stdin 送给 `vtouchctl --interactive`，或使用项目示例中的 `multiTouch` 函数。

完整 AutoJs6 示例已更新为：

```text
C:\Users\21102\vtouch-project\autojs_vtouch_example.js
```

该示例使用 AutoJs6 文档所述的 `shell(cmd, true)` 和 `Shell(true)`，不再直接访问 Unix socket，因此适配当前 `Permission denied` 环境。

完整示例：

```text
C:\Users\21102\vtouch-project\autojs_vtouch_example.js
```

核心调用：

```javascript
const LocalSocket = Packages.android.net.LocalSocket;
const LocalSocketAddress = Packages.android.net.LocalSocketAddress;
const BufferedReader = java.io.BufferedReader;
const InputStreamReader = java.io.InputStreamReader;
const BufferedWriter = java.io.BufferedWriter;
const OutputStreamWriter = java.io.OutputStreamWriter;
const UTF_8 = "UTF-8";

function VTouch() {
    this.socket = new LocalSocket();
    this.socket.connect(new LocalSocketAddress(
        "/data/local/tmp/vtouch.sock",
        LocalSocketAddress.Namespace.FILESYSTEM
    ));
    this.socket.setSoTimeout(2000);
    this.reader = new BufferedReader(new InputStreamReader(
        this.socket.getInputStream(), UTF_8));
    this.writer = new BufferedWriter(new OutputStreamWriter(
        this.socket.getOutputStream(), UTF_8));
}

VTouch.prototype.call = function (line) {
    this.writer.write(line);
    this.writer.newLine();
    this.writer.flush();

    const result = String(this.reader.readLine());
    if (result === "null" || result.indexOf("err") >= 0) {
        throw new Error(result);
    }
    return result;
};

VTouch.prototype.close = function () {
    try { this.socket.close(); } catch (e) {}
};
```

点击：

```javascript
const vt = new VTouch();
try {
    if (vt.call("ping") !== "pong") {
        throw new Error("vtouchd 未就绪");
    }
    vt.call("tap 720 1584 60");
} finally {
    vt.close();
}
```

连续滑动：

```javascript
const vt = new VTouch();
try {
    vt.call("down 0 300 500");
    for (let i = 1; i <= 40; i++) {
        vt.call("move 0 " + (300 + i * 10) + " " + (500 + i * 5));
        sleep(10);
    }
    vt.call("up 0");
} catch (e) {
    try { vt.call("reset"); } catch (ignored) {}
    throw e;
} finally {
    vt.close();
}
```

三指：

```javascript
const vt = new VTouch();
try {
    vt.call("down 0 500 1200");
    vt.call("down 1 720 1200");
    vt.call("down 2 940 1200");

    for (let i = 1; i <= 30; i++) {
        const y = 1200 + i * 5;
        vt.call("move 0 500 " + y);
        vt.call("move 1 720 " + y);
        vt.call("move 2 940 " + y);
        sleep(10);
    }

    vt.call("up 2");
    vt.call("up 1");
    vt.call("up 0");
} catch (e) {
    try { vt.call("reset"); } catch (ignored) {}
    throw e;
} finally {
    vt.close();
}
```

Auto.js 使用前提：

- 脚本运行环境能访问 `/data/local/tmp/vtouch.sock`；
- 目标环境允许 Java `LocalSocket`；
- 最好使用具有 root 能力的 Auto.js 环境；
- 普通 App 沙箱不一定能访问该路径。

---

## 10. 编译和部署

PC 端使用 Android NDK r27d，目标 ABI 为 arm64：

```sh
"C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/aarch64-linux-android24-clang.cmd" \
  -O2 -Wall -Wextra -pthread vtouchd.c -o vtouchd.production

"C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin/aarch64-linux-android24-clang.cmd" \
  -O2 -Wall -Wextra vtouchctl.c -o vtouchctl.production
```

部署：

```sh
adb push vtouchd.production /sdcard/vtouchd.production
adb push vtouchctl.production /sdcard/vtouchctl.production
adb shell "su -c 'cp /sdcard/vtouchd.production /data/local/tmp/vtouchd'"
adb shell "su -c 'cp /sdcard/vtouchctl.production /data/local/tmp/vtouchctl'"
adb shell "su -c 'chmod 755 /data/local/tmp/vtouchd /data/local/tmp/vtouchctl'"
```

启动：

```sh
adb shell "su -c 'nohup /data/local/tmp/vtouchd -x 1440 -y 3168 >/data/local/tmp/vtouchd.log 2>&1 </dev/null &'"
```

---

## 11. 已验证内容

已经在目标设备上实际验证：

- `/dev/uinput` 可用；
- root/KernelSU 权限有效；
- SELinux Enforcing 下可以创建设备；
- Android 能识别 `vtouch-virtual`；
- 虚拟设备为 `TOUCHSCREEN`；
- 事件包含 `BTN_TOOL_FINGER`；
- 单指触摸能生成正常 `ACTION_DOWN` 路径；
- 单指点击、移动、释放可用；
- 三指命令可执行；
- 客户端断开后可以捕获释放事件；
- 连续 100 次点击测试结果为 `ok=100 bad=0`；
- 压力测试后守护进程仍能 `ping -> pong`；
- 调试圆环来自 `show_touches` / `pointer_location`，已关闭。

关闭调试显示：

```sh
adb shell 'settings put system show_touches 0'
adb shell 'settings put system pointer_location 0'
```

---

## 12. 当前遗留问题

### 12.1 动态旋转映射

当前虚拟设备使用固定轴范围 `1440x3168`，InputReader 当前负责 `OrientationAware` 映射。守护进程没有完整实现横竖屏切换时的设备重建和坐标策略。

不要同时在调用方和守护进程中旋转坐标，否则会产生双重旋转。

### 12.2 分辨率变化自动检测

当前启动参数决定 uinput 轴范围。修改 `wm size`、外接显示器、折叠状态或显示配置后，需要重新创建虚拟设备。守护进程目前没有完整的显示变化监控、设备重建和调用暂停流程。

### 12.3 真正批量同帧多指

当前每一条 `down/move/up` 命令都会提交一个 `SYN_REPORT`。多指可以同时保持，但多个触点不是在单个协议帧中一次提交。

未来可加入：

```text
begin_frame 3
0 down 500 1200
1 down 720 1200
2 down 940 1200
end_frame
```

服务端在 `end_frame` 时一次性发送一个 `SYN_REPORT`。

### 12.4 Watchdog 和开机自启

当前没有独立 watchdog，也没有 KernelSU 开机自启模块。系统重启或守护进程崩溃后需要手动启动。

### 12.5 其他限制

- 服务端部分旧命令仍使用宽松整数解析；
- 写入错误反馈还可以进一步完善；
- `/data/local/tmp` 适合调试，不是稳定的系统服务目录；
- 普通 App 可能没有 socket 访问权限；
- 长时间数小时/数天运行尚未完成耐久性测试；
- 不同国产 ROM 可能对虚拟输入、边缘手势和触摸策略有不同处理；
- 当前没有完整的 system_server 重启、InputReader 重载和显示切换恢复测试。

---

## 13. 未来优化方向

按优先级建议：

1. **批量同帧协议**：支持一次提交多个 slot，减少多指时序误差；
2. **显示状态监控**：读取当前逻辑尺寸和旋转，变化时暂停输入并重建 uinput；
3. **坐标模式明确化**：区分逻辑坐标、物理坐标和原始坐标，避免双重旋转；
4. **服务端严格解析**：使用 `strtol` 全面校验参数、命令长度、时长和尾随参数；
5. **写入事务处理**：任意 input event 写入失败时立即返回错误并 reset；
6. **触点租约**：客户端超时或心跳丢失时自动释放触点；
7. **watchdog**：监测守护进程、socket 和虚拟设备，异常时重新启动；
8. **KernelSU 模块**：实现开机自启、日志目录和版本管理；
9. **正式 SDK**：提供 Auto.js 模块、C API、Java/Kotlin JNI 封装；
10. **耐久性测试**：执行长时间高频轨迹、多客户端、旋转、锁屏和 system_server 重启测试。

---

## 14. 结论

当前项目的核心链路已经跑通：

```text
第三方程序
  -> Unix socket
  -> vtouchd
  -> /dev/uinput
  -> Android InputReader
  -> 应用 MotionEvent
```

当前版本已经适合 root 自动化和自有测试程序调用。动态显示适配、批量同帧和自动恢复仍属于后续增强功能，不能把它们描述为当前已完成能力。

## 15. 一键安装

## 15. 项目结构与构建

项目已整理为：

```text
vtouch-project/
├── src/       C 源码
├── clients/   AutoJs6 客户端示例
├── scripts/   安装和开机启动脚本
├── docs/      使用和协议文档
├── tests/     冒烟测试
└── build/     arm64 构建产物
```

主要文件：

```text
src/vtouchd.c
src/vtouchctl.c
src/vtouchws.c
clients/autojs_vtouch_example.js
clients/autojs_vtouch_ws_example.js
scripts/install_vtouch.bat
scripts/install_vtouch.sh
scripts/service-vtouchd.sh
```

## 16. WebSocket 调用

WebSocket bridge 只监听本机回环地址：

```text
ws://127.0.0.1:27183
```

安装并启动：

```sh
adb push vtouchws.production /sdcard/vtouchws.production
adb shell 'su -c "cp /sdcard/vtouchws.production /data/local/tmp/vtouchws; chmod 755 /data/local/tmp/vtouchws; nohup /data/local/tmp/vtouchws >/data/local/tmp/vtouchws.log 2>&1 </dev/null &"'
```

AutoJs6 示例：

```text
autojs_vtouch_ws_example.js
```

### 推荐：使用 SDK，业务脚本只保留操作逻辑

如果希望只使用一个 JS 文件，直接使用：

```text
clients/vtouch_onefile_example.js
```

该文件把 SDK 压缩在一行，后面是业务区和简短的参数说明。SDK 默认从 AutoJs6 的 `device.width` 和 `device.height` 自动读取屏幕尺寸，不再写死 `1440x3168`；只有特殊场景才需要在配置中显式覆盖 `width/height`。

```javascript
var vt=new VTouch().connect();
vt.tap(720,1584,60);
vt.swipe(300,1000,900,1000,500);
vt.frame([
  {slot:0,state:"down",x:500,y:1200},
  {slot:1,state:"down",x:900,y:1200}
]);
```

参数：

- `tap(x, y, durationMs)`：点击；时长默认 60 ms；
- `swipe(x1, y1, x2, y2, durationMs)`：滑动；时长默认 300 ms；
- `frame(points)`：批量同帧多指；每项为 `{slot,state,x,y}`；
- `slot`：`0~9`；
- `state`：`down`、`move` 或 `up`；
- `reset()`：释放触点；
- `close()`：关闭连接。

单文件完整示例已经包含所有当前支持的屏幕操作：

```text
clients/vtouch_onefile_example.js
```

支持的 API：

| API | 作用 | 参数 |
|---|---|---|
| `tap(x, y, durationMs)` | 单击 | 坐标、按下时长；默认 60 ms |
| `swipe(x1, y1, x2, y2, durationMs)` | 单指滑动 | 起点、终点、时长；默认 300 ms |
| `down(slot, x, y)` | 按下一个触点 | slot 0~9、坐标 |
| `move(slot, x, y)` | 移动一个触点 | 已按下的 slot、坐标 |
| `up(slot)` | 抬起一个触点 | slot 0~9 |
| `frame(points)` | 批量同帧多指 | `[{slot,state,x,y}]` |
| `pinch(cx, cy, startGap, endGap, durationMs)` | 双指缩放 | 中心、起始间距、结束间距、时长 |
| `reset()` | 紧急释放所有触点 | 无 |
- `close()` | 关闭 WebSocket | 无 |

SDK 的触摸调用只入队并立即返回，不使用 `sleep()`、`delay()` 或 `burst()`；实际发送由 WebSocket 文本回调推进，不阻塞 AutoJs6 主线程。连续轨迹应使用 `down/move/up` 或 `frame`，需要时间间隔时由业务层使用 AutoJs6 定时器调度，而不是阻塞脚本线程。

`frame` 中的 `state` 只能是：

```text
down
move
up
```

示例：

```javascript
vt.frame([
    {slot: 0, state: "down", x: 500, y: 1200},
    {slot: 1, state: "down", x: 900, y: 1200}
]);
```

一个 `frame` 会在服务端一次性提交并生成一个 `SYN_REPORT`，适合多指按下、移动和抬起。

SDK 文件：

```text
clients/vtouch_sdk.js
```

业务脚本只需要：

```javascript
var VTouch = require("./vtouch_sdk.js");
var vt = new VTouch();
vt.connect();
sleep(500);

vt.tap(720, 1584);
vt.swipe(300, 1000, 900, 1000, 500);
vt.frame([
    {slot: 0, state: "down", x: 500, y: 1200},
    {slot: 1, state: "down", x: 720, y: 1200}
]);
vt.frame([
    {slot: 0, state: "up", x: 500, y: 1200},
    {slot: 1, state: "up", x: 720, y: 1200}
]);
vt.close();
```

SDK 已封装连接保活、WebSocket 事件、命令队列、超时、坐标校验、批量帧和关闭清理。完整业务示例：

```text
clients/autojs_vtouch_business_example.js
```

AutoJs6 的 WebSocket 应使用文档中的 `web.newWebSocket()`，不是浏览器的 `new WebSocket()`，也不是直接使用 `onopen/onmessage` 属性：

```javascript
var ws = web.newWebSocket("ws://127.0.0.1:27183");
ws.on("open", function () { ws.send("ping"); });
ws.on("text", function (text) { log(text); });
ws.on("failure", function (err) { log(err); });
```

完整的串行队列、心跳、错误处理和退出清理已合并到 `clients/vtouch_onefile_example.js`。该文件现在是唯一的 AutoJs6 JS 客户端示例。

AutoJs6 的 WebSocket 不是浏览器式 `onopen/onmessage` 属性接口。应使用文档中的事件常量：

```javascript
var ws = new WebSocket("ws://127.0.0.1:27183");
ws.on(WebSocket.EVENT_OPEN, function (response, socket) {
    socket.send("ping");
});
ws.on(WebSocket.EVENT_TEXT, function (text, socket) {
    log(String(text));
});
ws.on(WebSocket.EVENT_FAILURE, function (error) {
    log(error);
});
```

不要使用浏览器写法 `ws.onopen = ...`、`ws.onmessage = ...`。完整的事件队列、连接失败、关闭处理和同帧多指示例见 `clients/autojs_vtouch_ws_example.js`。

当前 WebSocket 文本帧中直接发送原有命令字符串，而不是 JSON：

```text
ping
tap 720 1584 60
down 0 500 1200
move 0 520 1220
up 0
```

当前 bridge 已完成 RFC6455 握手、客户端掩码校验、文本帧、ping/pong、close、半包读写和 4096 字节限制。它复用 vtouchd 的 Unix socket，因此现有 vtouchd 的 owner 和断开清理逻辑仍然适用。详细说明见 `VTOUCH_PROTOCOL.md` 和 `WEBSOCKET_DESIGN.md`。

> 当前 bridge 是可用的最小 WebSocket 代理，但尚未实现真正的 JSON RPC、WebSocket 分片帧和 vtouchd 的 `begin_frame/end_frame` 批量同帧协议。不要把多条普通命令描述为同一个输入帧。

### Windows + ADB

将以下文件放在同一目录：

```text
install_vtouch.bat
vtouchd.production
vtouchctl.production
```

手机已连接并授权 USB 调试、KernelSU 已允许 adb shell root 后，双击：

```text
install_vtouch.bat
```

或指定尺寸：

```text
install_vtouch.bat 1440 3168
```

脚本会检查 adb 和 root，上传二进制，停止旧进程，清理旧 socket，启动服务，执行 `ping`，并关闭系统触摸可视化选项。

### 安装时的环境清理

一键安装和手机端安装脚本都会先执行清理：

- 尝试发送 `reset` 释放旧触点；
- 停止旧的 `vtouchws` 和 `vtouchd` 进程；
- 删除旧 Unix socket；
- 删除旧日志和临时上传文件；
- 再复制新二进制并启动服务。
- 安装前删除旧二进制，确保运行的是本次上传的最新文件；
- 启动后检查 `vtouchd` 和 `vtouchws` 各只有一个进程。

如果脚本异常中断，也可以手动清理：

```sh
adb shell 'su -c "if [ -x /data/local/tmp/vtouchctl ]; then /data/local/tmp/vtouchctl reset >/dev/null 2>&1 || true; fi; killall vtouchws >/dev/null 2>&1 || true; killall vtouchd >/dev/null 2>&1 || true; rm -f /data/local/tmp/vtouch.sock /data/local/tmp/vtouchd.log /data/local/tmp/vtouchws.log /sdcard/vtouchd.install /sdcard/vtouchctl.install /sdcard/vtouchws.install"'
```

注意：安装脚本会删除并重新复制三个运行二进制，以确保不会继续使用旧版本；不会删除 KernelSU 的开机自启脚本。手动清理命令只清理运行状态、socket、日志和临时上传文件，不删除已安装二进制。

### 手动安装（逐步执行）

适用于不使用一键脚本、需要逐步检查每个环节的情况。以下命令在 Windows Git Bash 中执行；如果电脑上的 `adb` 不在 `PATH`，将 `adb` 替换为 `platform-tools/adb.exe` 的完整路径。

#### 1. 检查设备连接

```sh
adb devices
```

设备状态应为：

```text
<serial>    device
```

如果显示 `unauthorized`，需要在手机上确认 USB 调试授权。

#### 2. 检查 root 和 uinput

```sh
adb shell 'su -c "id; getenforce; ls -l /dev/uinput"'
```

应看到：

```text
uid=0(root)
```

并且 `/dev/uinput` 存在。SELinux 为 `Enforcing` 不一定是问题；只有实际出现 `Permission denied` 时才需要检查 KernelSU 策略。

#### 3. 获取屏幕尺寸

```sh
adb shell wm size
```

例如：

```text
Physical size: 1440x3168
```

记录宽度和高度，下面以 `1440 3168` 为例。

#### 4. 上传文件

```sh
adb push vtouchd.production /sdcard/vtouchd.install
adb push vtouchctl.production /sdcard/vtouchctl.install
```

如果只有旧文件名，则使用对应文件：

```sh
adb push vtouchd /sdcard/vtouchd.install
adb push vtouchctl /sdcard/vtouchctl.install
```

#### 5. 安装到可执行目录

```sh
adb shell 'su -c "cp /sdcard/vtouchd.install /data/local/tmp/vtouchd; cp /sdcard/vtouchctl.install /data/local/tmp/vtouchctl; chmod 755 /data/local/tmp/vtouchd /data/local/tmp/vtouchctl"'
```

确认文件：

```sh
adb shell 'su -c "ls -l /data/local/tmp/vtouchd /data/local/tmp/vtouchctl"'
```

#### 6. 停止旧服务并清理状态

```sh
adb shell 'su -c "if [ -x /data/local/tmp/vtouchctl ]; then /data/local/tmp/vtouchctl reset >/dev/null 2>&1 || true; fi; killall vtouchd >/dev/null 2>&1 || true; rm -f /data/local/tmp/vtouch.sock"'
```

#### 7. 启动守护进程

```sh
adb shell 'su -c "nohup /data/local/tmp/vtouchd -x 1440 -y 3168 >/data/local/tmp/vtouchd.log 2>&1 </dev/null &"'
```

启动其他分辨率时替换参数：

```sh
adb shell 'su -c "nohup /data/local/tmp/vtouchd -x <宽> -y <高> >/data/local/tmp/vtouchd.log 2>&1 </dev/null &"'
```

#### 8. 等待并验证 socket

```sh
sleep 2
adb shell 'su -c "/data/local/tmp/vtouchctl ping"'
adb shell 'su -c "/data/local/tmp/vtouchctl res"'
```

期望：

```text
pong
1440x3168
```

#### 9. 验证 Android 输入设备

```sh
adb shell 'su -c "dumpsys input | grep -A40 -B5 vtouch-virtual"'
```

应包含：

```text
Classes: TOUCH | TOUCH_MT
Sources: TOUCHSCREEN
DeviceType: TOUCH_SCREEN
Enabled: true
```

#### 10. 发送测试点击

```sh
adb shell 'su -c "/data/local/tmp/vtouchctl tap 720 1584 60"'
```

返回：

```text
ok
```

#### 11. 关闭触摸可视化

如果屏幕出现触摸圆环或坐标轨迹：

```sh
adb shell 'settings put system show_touches 0'
adb shell 'settings put system pointer_location 0'
```

#### 12. 查看日志

```sh
adb shell 'su -c "cat /data/local/tmp/vtouchd.log"'
```

正常启动日志应包含：

```text
虚拟触摸设备已创建
监听 /data/local/tmp/vtouch.sock
vtouchd 就绪
```

#### 13. 手动停止

```sh
adb shell 'su -c "/data/local/tmp/vtouchctl reset; killall vtouchd"'
```

停止后确认：

```sh
adb shell 'su -c "ps -A | grep vtouchd || true; test ! -e /data/local/tmp/vtouch.sock && echo socket_removed"'
```

### 手机端安装脚本

如果文件已经在手机的 `/data/local/tmp/vtouch-install`：

```sh
su -c 'sh /data/local/tmp/vtouch-install/scripts/install_vtouch.sh'
```

不需要传宽高，脚本会通过 `wm size` 自动读取当前物理屏幕尺寸。
脚本从自身路径定位 `../build`，因此从任意当前工作目录执行都可以。

### KernelSU 开机自启

将 `service-vtouchd.sh` 安装为：

```text
/data/adb/service.d/90-vtouchd
```

并设置可执行权限：

```sh
su -c 'cp /data/local/tmp/service-vtouchd.sh /data/adb/service.d/90-vtouchd; chmod 755 /data/adb/service.d/90-vtouchd'
```

当前自启动脚本和 `vtouch-start.sh` 都通过 `wm size` 动态读取逻辑显示尺寸并传给 `vtouchmerge -w/-h`；读取失败会拒绝启动。系统存在旋转、分辨率切换或多显示器场景时，需重启服务以重新创建映射。

新增安装文件：

```text
install_vtouch.bat
install_vtouch.sh
service-vtouchd.sh
```
