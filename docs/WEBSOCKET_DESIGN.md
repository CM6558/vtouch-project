# WebSocket 方案设计说明

## 目标

为 AutoJs6 提供比 `shell()` 更轻量的长连接接口：

```text
AutoJs6 -> ws://127.0.0.1:27183 -> vtouchws -> Unix socket -> vtouchd -> uinput
```

WebSocket bridge 只绑定回环地址，不监听局域网。它不直接操作 uinput，而是复用现有 vtouchd 的触点 owner、reset 和错误处理。

## 协议

客户端发送 WebSocket 文本帧，payload 是一条 vtouchd 命令：

```json
{"cmd":"ping"}
{"cmd":"tap","x":720,"y":1584,"duration":60}
```

当前 bridge 的协议适配层应将 JSON 命令转换为 vtouchd 的文本协议，并把响应包装为 JSON。批量多指帧：

```json
{"cmd":"frame","points":[{"slot":0,"state":"down","x":500,"y":1200},{"slot":1,"state":"down","x":900,"y":1200}]}
```

该功能只有在 vtouchd 支持真正 frame 提交后才可声称同帧；bridge 不能把多条旧命令伪装成同一帧。

## 端口

```text
127.0.0.1:27183
```

只允许本机连接。WebSocket bridge 需要作为 root 或与 vtouchd 有权限通信的服务进程启动。

## 生命周期

- WebSocket 建立时创建一个后端 vtouchd 连接；
- 所有消息按连接串行处理；
- ping/pong 用于 WebSocket 保活；
- close、EOF、协议错误、超时都关闭后端连接；
- vtouchd 负责释放该后端连接的触点；
- 单个消息最大 4096 字节；
- 不执行客户端提供的 shell 字符串。

## 性能

Auto.js 应复用单个 WebSocket 连接，不要每个 move 重新连接。二进制帧可减少 JSON 开销，但当前触摸命令频率下文本帧已足够；真正影响多指质量的是服务端单帧提交，而不是 JSON 本身。

## 安全

虽然只监听回环地址，仍应：

- 限制最大消息长度；
- 校验 WebSocket mask；
- 拒绝未完成握手的帧；
- 设置 idle timeout；
- 不接受任意 shell 命令；
- 异常关闭时 reset；
- 不监听 `0.0.0.0`。

## 重要限制

WebSocket 只解决 Auto.js 的调用和权限兼容问题，不能改变虚拟设备在系统中的来源属性，也不能绕过应用安全检测。
