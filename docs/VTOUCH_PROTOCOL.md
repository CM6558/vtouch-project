# WebSocket 调用说明

当前 bridge 不是 JSON RPC，而是把 vtouchmerge 文本协议封装在 WebSocket 文本帧中。

```text
AutoJs6 WebSocket -> "ping" -> "pong"
AutoJs6 WebSocket -> "tap 720 1584 60" -> "ok"
```

连接地址：

```text
ws://127.0.0.1:27183
```

服务端二进制：

```text
vtouchws.production
```

安装到手机：

```sh
adb push vtouchws.production /sdcard/vtouchws.production
adb shell 'su -c "cp /sdcard/vtouchws.production /data/local/tmp/vtouchws; chmod 755 /data/local/tmp/vtouchws"'
```

启动：

```sh
adb shell 'su -c "nohup /data/local/tmp/vtouchws >/data/local/tmp/vtouchws.log 2>&1 </dev/null &"'
```

确认监听：

```sh
adb shell 'su -c "ss -ltn | grep 27183"'
```

AutoJs6 示例：

```text
autojs_vtouch_ws_example.js
```

## 原子多指帧

在同一个持续 WebSocket 连接中发送：

```text
begin_frame
point 0 down 500 1200
point 1 down 900 1200
end_frame
```

`end_frame` 会一次性提交全部点，并只发送一个 `SYN_REPORT`。每个 slot 在一帧中只能出现一次，状态支持 `down`、`move`、`up`。开启帧、提交点和结束帧必须来自同一个连接。

注意：bridge 只接受最终（FIN=1）、客户端 masked 的文本帧，最大 4096 字节；支持 ping/pong 和 close。服务端绑定 `127.0.0.1`，不接受局域网连接。

## 物理触摸订阅（pev）

监听物理手指不再走 `events.observeTouch`，直接订阅 daemon 的物理触摸流（同一条 WS 连接，又发又收）：

```text
-> "sub"          -> "ok"   （开始推送；"unsub" 停止）
<- "pev 0 down 540 1200"
<- "pev 0 move 541 1210"
<- "pev 0 up 541 1210"
```

- `pev <slot> <down|move|up> <lx> <ly>`：slot 为物理 Type-B 槽号，坐标已是逻辑坐标（与 down/move/up 入参同一坐标系）。
- daemon 只在状态变化时推送（按下/抬起/坐标变化），静止不刷屏；无订阅者时零开销。
- 新连接默认未订阅；连接被顶掉或断开后订阅清零，重连需重新 `sub`。
- 订阅后发包不再 `drain`（读线程统一消费回包，避免吃掉 pev）。
