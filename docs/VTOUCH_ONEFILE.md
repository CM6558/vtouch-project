# vtouch_onefile_example.js 使用说明

该文件是 AutoJs6 单文件客户端：SDK 与业务示例在同一个 JS 文件中，SDK 已压缩为一行，业务调用保持简洁。

## 运行前提

手机端必须运行：

```text
/data/local/tmp/vtouchd
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

## API

```javascript
var vt = new VTouch().connect();
```

### tap

```javascript
vt.tap(x, y, durationMs);
```

- `x/y`：屏幕坐标；
- `durationMs`：按下时间，默认 60 毫秒。

### swipe

```javascript
vt.swipe(x1, y1, x2, y2, durationMs);
```

- `(x1,y1)`：起点；
- `(x2,y2)`：终点；
- `durationMs`：持续时间，默认 300 毫秒。

### down / move / up

```javascript
vt.down(slot, x, y);
vt.move(slot, x, y);
vt.up(slot);
```

- `slot`：触点编号，范围 `0~9`；
- 同一个手指生命周期必须使用同一个 slot；
- 顺序通常是 `down -> move* -> up`。

### frame

```javascript
vt.frame([
    {slot: 0, state: "down", x: 500, y: 1200},
    {slot: 1, state: "down", x: 900, y: 1200}
]);
```

- `state`：`down`、`move` 或 `up`；
- 一个 `frame` 内的多个触点一次提交；
- 服务端只发送一个 `SYN_REPORT`。

### pinch

```javascript
vt.pinch(cx, cy, startGap, endGap, durationMs);
```

- `cx/cy`：缩放中心；
- `startGap`：开始间距；
- `endGap`：结束间距；
- `durationMs`：持续时间，默认 300 毫秒。

### reset / close

```javascript
vt.reset();
vt.close();
```

`reset()` 释放全部虚拟触点；`close()` 关闭 WebSocket。

## 示例

### 点击

```javascript
vt.tap(vt.width / 2, vt.height / 2, 60);
```

### 单指滑动

```javascript
vt.down(0, 300, 500);
vt.move(0, 500, 700);
vt.up(0);
```

### 双指操作

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

### Finger 对象

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

- `vt.finger(slot)`：取得 slot 对应的 `Finger` 对象；同一 slot 始终复用同一对象。
- `finger.down(x, y)` / `move(x, y)` / `up()`：异步排队的触点生命周期操作。
- `finger.hold(durationMs, continuation)`：按住指定时间后调用回调；回调运行在 Auto.js 定时器中，必须自行继续 `move()` 或 `up()`。
- `finger.press(x, y, durationMs, continuation)`：`down` 后由回调定时 `up`，适合点击/自动释放按压。
- `finger.frame(state, x, y)`：用现有 `begin_frame/point/end_frame` 协议提交该触点的一帧。
- `finger.cancel()`：取消尚未触发的本地定时器，不会抬指；请随后调用 `up()` 或 `reset()`。
- `finger.state()`：返回本地生命周期状态 `"down"` 或 `"up"`。
- `vt.gesture(frames)`：按顺序排队多个 `frame(points)`；不新增协议。

`hold`/`press` 的计时由 Auto.js 的 `setTimeout` 完成，服务端不会等待，也不会伪造服务端时序；因此脚本退出、连接断开或超时可能使回调未执行。所有网络调用只进入异步发送队列，不使用 `sleep()`、`delay()` 或 `burst()`，不会阻塞 Auto.js 主线程。
