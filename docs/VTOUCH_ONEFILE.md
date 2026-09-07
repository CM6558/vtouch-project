# vtouch_onefile_example.js 使用说明

该文件是 AutoJs6 单文件客户端：SDK 与业务示例在同一个 JS 文件中，SDK 已压缩为一行，业务调用保持简洁。

## 运行前提

手机端必须安装并启动服务（SDK 的 `startService()` 会在首次连接失败时自动拉起，服务进程异常退出后也会周期性重启）：

```text
/data/local/tmp/vtouchmerge
/data/local/tmp/vtouchws
```

WebSocket 地址默认是：

```text
ws://127.0.0.1:27183
```

屏幕尺寸默认读取：

```javascript
device.width
device.height
```

## 连接

```javascript
var vt = new VTouch().connect();
```

- `new VTouch(options)`：`options.url` / `options.width` / `options.height` / `options.connectTimeout`（总连接超时，默认 15000ms）/ `options.onError`
- `connect(onReady)`：连接成功后回调；失败自动检查并启动服务后重试（每 10 次尝试重启一次服务），60 次或超时后抛异常
- 连接是幂等的：重复调用不会叠加监听器
- 脚本退出时自动关闭连接并停止服务（`events.on("exit")`）

## Finger API

```javascript
var f = vt.finger();     // 自动分配空闲 slot 0~9
var f2 = vt.finger(2);   // 指定 slot 2
```

- `f.down(x, y)` / `f.move(x, y)` / `f.up()`：异步排队的触点生命周期操作
- `f.tap(x, y, durationMs)`：点击，durationMs 默认 60ms
- `f.swipe(x1, y1, x2, y2, durationMs)`：滑动，durationMs 默认 300ms；内部线程按帧插值，不阻塞主线程
- `f.hold(durationMs, continuation)`：按住指定时间后调用回调；回调运行在 Auto.js 定时器中，必须自行继续 `move()` 或 `up()`
- `f.press(x, y, durationMs, continuation)`：`down` 后由定时器 `up`，适合点击/自动释放按压
- `f.frame(state, x, y)`：用 `begin_frame/point/end_frame` 协议提交该触点的一帧
- `f.cancel()`：取消尚未触发的本地定时器，不会抬指；请随后调用 `up()` 或 `reset()`
- `f.state()`：返回本地生命周期状态 `"down"` 或 `"up"`

## VTouch API

### frame —— 原子多指帧

```javascript
vt.frame([
    {slot: 0, state: "down", x: 500, y: 1200},
    {slot: 1, state: "down", x: 900, y: 1200}
]);
```

- `state`：`down`、`move` 或 `up`；所有点先校验后统一提交
- 一个 `frame` 内的多个触点**一次原子提交**，服务端只发送一个 `SYN_REPORT`
- 每个 slot 在一帧中只能出现一次
- 帧操作会同步对应 Finger 的本地状态

### gesture —— 帧序列

```javascript
vt.gesture(frames);            // 立即逐帧提交
vt.gesture(frames, 800);       // 可选总时长，按帧均分间隔
```

`gesture(frames[, durationMs])` 按顺序排队多个 `frame(points)`。第二参为可选总时长（毫秒），缺省或 0 表示无时序立即提交。带时序时使用定时器逐帧提交，`reset()`/`close()` 会取消未触发的帧。

### pinch —— 双指缩放

```javascript
vt.pinch(cx, cy, startGap, endGap, durationMs);
```

- `cx/cy`：缩放中心；`startGap`：开始间距；`endGap`：结束间距；`durationMs`：持续时间（默认 300ms，生效）
- 双指 down/up 使用原子帧提交，中间按 `durationMs` 在线程中步进插值，不阻塞主线程
- 间距会被屏幕边界自动裁剪

### reset / close

```javascript
vt.reset();   // 释放全部虚拟触点（服务端 reset 命令）
vt.close();   // 关闭 WebSocket 连接
```

两者都会取消本地定时器与动画线程；`close()` 额外清空发送队列并关闭连接。

### onError

```javascript
vt.onError = function (msg) { log("[vtouch] server error: " + msg); };
```

服务端返回错误响应（如 "err point"）时回调。连续 5 次错误会自动 `reset()` 全部触点。

## 示例

### 点击

```javascript
vt.finger().tap(540, 1200, 60);
```

### 单指滑动

```javascript
vt.finger().swipe(300, 500, 500, 700, 1000);
```

### 双指原子操作

```javascript
vt.frame([
    {slot: 0, state: "down", x: 500, y: 1200},
    {slot: 1, state: "down", x: 900, y: 1200}
]);

vt.frame([
    {slot: 0, state: "move", x: 450, y: 1250},
    {slot: 1, state: "move", x: 950, y: 1250}
]);

vt.frame([
    {slot: 0, state: "up", x: 450, y: 1250},
    {slot: 1, state: "up", x: 950, y: 1250}
]);
```

### hold / press

```javascript
var finger = vt.finger(0);
finger.down(300, 500)
    .hold(1000, function (f) {
        f.move(420, 620).up();
    });

// 定时按下并自动释放；可选回调在释放后执行
vt.finger(1).press(700, 900, 250, function (f) {
    log(f.state()); // "up"
});
```

## 时序说明

- `swipe`/`pinch` 的动画由内部动画线程（`threads.start` + `sleep`）驱动，不阻塞 AutoJs6 主线程
- `tap`/`hold`/`press` 的计时由 AutoJs6 主线程 `setTimeout` 完成；主线程被业务 `sleep()` 阻塞时定时器会延迟触发
- 所有网络调用只进入异步发送队列，不使用主线程 `sleep()`
- 脚本退出、连接断开或超时可能使定时回调未执行；强杀（SIGKILL）会跳过 exit 处理器，业务逻辑应主动调用 `vt.stopService()`
