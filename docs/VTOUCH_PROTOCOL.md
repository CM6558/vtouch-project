# vtouch 协议说明（vtouchd WebSocket）

`vtouchd` 单进程直接暴露 WebSocket 接口（`ws://127.0.0.1:27183`），文本帧协议，非 JSON RPC。
协议是 vtouchmerge/vtouchws 时代的演进：去掉中间 Unix socket 跳转，合并器 + WS + region 匹配共用一个 poll 循环。

```text
AutoJs6 (clients/vtouch_bundle.js)  -> ws://127.0.0.1:27183 -> vtouchd -> /dev/uinput
```

## 连接与生命周期

- 只绑定回环 `127.0.0.1:27183`，不接受局域网连接
- **单客户端**：新连接顶掉旧连接（旧连接触点复位）
- 所有消息按连接串行处理；客户端消息最大 4096 字节
- 只接受客户端 masked 的文本帧（FIN=1）；支持 ping/pong、close
- 连接断开/EOF/协议错误 → 关闭连接、复位该连接的虚拟触点
- 不执行客户端提供的 shell 字符串
- 服务端需 root 运行（`EVIOCGRAB` + `uinput`）

## 命令（客户端 → vtouchd）

```text
ping                          -> pong
res                           -> 逻辑分辨率与 raw 坐标范围
sub | unsub                   -> 订阅/退订物理触摸流（pev）；默认未订阅，断连后清零
region clear                  -> 清空全部区域
region add <id> <type> <a1> <a2> <a3> <a4> <en>
                              -> 添加/更新区域（type 0=矩形, 1=圆形；≤32 个；非法参数/超限拒绝）
region list                   -> 查询当前全部区域配置（每行一条 region ...，末尾 end <n>）
down | move | up <slot> <x> <y>
                              -> 虚拟触点注入（virt 槽，逻辑坐标）
begin_frame                   -> 开启原子多指帧
point <slot> <state> <x> <y>  -> 帧内点（state: down/move/up；每槽每帧一次）
end_frame                     -> 提交帧（单次 SYN_REPORT）
```

响应：`ok 0`（成功）/ `ok 1`（已处理）/ `err region`（区域配置非法）。

### region add 参数

```text
矩形: region add <id> type0 <x1> <y1> <x2> <y2> <en>
圆形: region add <id> type1 <cx> <cy> <r> <en>
```

- 坐标逻辑坐标系；`en` 0/1 启用开关
- **同 id 查重更新**：`region add` 已存在同 id 时原地更新属性（type/坐标/en），不新增（UI 开关/改配置安全）
- 越界/非法（区域数 >32、参数非数字、type 非 0/1）→ `err region`，不影响已有配置

### region list 响应

```text
region <id> <type> <a1> <a2> <a3> <a4> <en>
region ...
end <n>
```

面板/客户端启动时可拉取当前配置（`region list` 后逐行读取直到 `end <n>`），与 `region add` 共用同一张表（≤32 行）。

### 原子多指帧

```text
begin_frame
point 0 down 500 1200
point 1 down 900 1200
end_frame
```

`end_frame` 一次性提交全部点，单次 `SYN_REPORT`。开启、提交、结束必须在同一连接。

## 事件（vtouchd → 客户端）

### 物理触摸流（pev，跟随 sub）

```text
pev <slot> <down|move|up> <lx> <ly>
```

- slot 为物理 Type-B 槽号，坐标已换算为逻辑坐标
- 只在状态变化时推送（按下/抬起/坐标变化），静止不刷屏；无订阅者零开销
- 新连接默认未订阅；被顶掉/断开后订阅清零，重连需重新 `sub`

### 区域事件（region_ev）

```text
region_ev <id> <ev> <slot> <lx> <ly>
```

五事件（纯监听，不做任何补充点击）：

| ev | 触发 |
|---|---|
| `down` | 手指在区域内按下 |
| `up` | 手指抬起且按下期间命中过该区域 |
| `enter` | 手指从区域外移入 |
| `exit` | 手指从区域内移出 |
| `move` | 手指在区域内位置变化（坐标变化才推） |

- 区域匹配在物理帧透传注入之后执行（SYN 帧后），**绝不阻塞/修改触摸路径**
- 物理触摸始终 1:1 透传；区域命中只推事件，不拦截、不代点
- **单进程面板直通**：`region_ev` 在 WS 推送之前先调进程内事件回调（`vtouch_set_callbacks`）——
  UI 面板即使无 WS 客户端也能收到事件（闪烁/日志）；触摸事件经 `vtouch_ui_sync` 回调面板（喂 ImGui io + overlay 实时着色）

## 性能

- 单连接复用，不要每个 move 重连
- 注入路径整帧 `writev` 一次提交（多槽同帧零碎片）
- 区域匹配纯 int 比较、静态数组、无分配，匹配开销微秒级

## 安全

- 仅回环监听；限制最大消息长度；校验 mask；拒绝未完成握手的帧
- 异常关闭时复位虚拟触点；不监听 `0.0.0.0`

## 限制

WebSocket 只解决调用与权限兼容，不能改变虚拟设备在系统中的来源属性，也不能绕过应用安全检测。
