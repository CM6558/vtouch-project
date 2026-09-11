# vtouch：Android 触摸合并系统

## 项目简介

`vtouch` 是一个用于 Android root 设备的触摸合并系统，通过用户态 `EVIOCGRAB` + `uinput` 技术，将真实触摸输入与模拟触摸合并为单一统一触摸设备，使 Android 应用无法区分真实与模拟触摸。

**核心特性：**
- 真实触摸与模拟触摸同时共存，互不干扰
- 用户态实现，无需内核模块（.ko）
- WebSocket 接口，支持 AutoJs6/第三方程序调用
- 动态发现触摸设备，不硬编码 event 节点
- 自动坐标转换（逻辑屏幕坐标 ↔ 原始触摸轴）
- **原生 region 区域匹配**：≤32 区域（圆形/矩形），五事件监听（down/up/enter/exit/move），纯监听不代点

## 架构

```text
真实触摸面板
    ↓ EVIOCGRAB
vtouchd 核心（合并器 + WebSocket + region 匹配，一个 poll 循环；库化接口可嵌入）
    ├── 真实触点（1:1 透传）
    └── 模拟触点（virt 槽，WS 注入）
          ↓ /dev/uinput
Android InputReader/InputDispatcher
          ↓
应用 MotionEvent

AutoJs6（require("/sdcard/vtouch_bundle.js")：二进制自释放 + 连接 + 面板生命周期）
    ↓ WebSocket (127.0.0.1:27183)
vtouchd 核心
    ├── 物理事件流 pev（跟随订阅）
    └── 区域事件 region_ev（匹配推送）

单进程 UI 面板（vtouchd 核心同进程嵌入）
app_process（Java 80 行拿 SurfaceControl 图层）
    └── JNI → C++：EGL GLES2 + Dear ImGui + vtouchd 核心
        ├── 区域管理（表格/开关/显隐/删除/＋矩形/＋圆形框选）
        ├── 事件日志 + overlay 实时预览（触摸内绿外红 + 命中闪烁）
        └── WS 服务器对外（AutoJs6 照常连接）
```

## 部署

二进制不随仓库分发——由 GitHub Actions **Build vtouch (Android NDK)** 工作流构建，从 artifact 下载：

**只需一个文件**：面板（`classes.dex` + `libtestimgui.so`）与 vtouchd 都内嵌在 bundle 里，设备侧不需要任何 `.sh`。

```bash
adb push clients/vtouch_bundle.js /sdcard/vtouch_bundle.js
```

启动完全由脚本控制（AutoJs6 里 require 这一个文件）：

```javascript
var vt = require("/sdcard/vtouch_bundle.js");
vt.uiStart();          // 面板 = UI + daemon（grab 物理触摸 + WS 27183 + 读 regions.conf）；首次自动释放二进制
vt.uiAlive();          // true（走 pidof vtouch-ui，不信 pid 文件）
vt.uiStop();                 // 收掉面板并释放 EVIOCGRAB；vt.stop() 也会连同面板一起收
```

## 区域监听示例

区域唯一归属是**面板**（改完自动存 `regions.conf`，首行带格式版本号，旧版本文件会被整份丢弃）；脚本只做回读 + 下发，自己不存任何区域状态：

```javascript
var vt = require("/sdcard/vtouch_bundle.js");
vt.uiStart();                  // 面板就是 daemon，不要再 ensure()
var c = vt.connect();          // 连 ws://127.0.0.1:27183

var rs = vt.rgList(c);         // 回读面板当前表（必须在开收包循环之前调）
rs.push({ id: "swipeL", name: "左滑区", x1: 60, y1: 2200, x2: 660, y2: 2900, enabled: true });
vt.rgPush(c, rs);              // 整表下发：面板立刻生效并落盘（内部先 region clear）

var eng = vt.createEngine(rs, {
    onDown:  function (r) { log("down  " + r.id); },
    onUp:    function (r) { log("up    " + r.id); },
    onEnter: function (r) { log("enter " + r.id); },
    onExit:  function (r) { log("exit  " + r.id); }
});

vt.sub(c);                     // 订阅物理事件流 pev
threads.start(function () {    // 读线程必须常驻；recv() 非阻塞，空转 sleep 让一下
    for (;;) {
        var line = null;
        try { line = c.recv(); } catch (e) { break; }
        if (line) { var ev = vt.parseEv(line); if (ev) eng.feed(ev); }
        else sleep(8);
    }
});
events.on("exit", function () { try { c.close(); } catch (e) {} vt.stop(); });
```

拿不动引擎就直接读事件行也可以：`vt.parseEv(c.recv())` 返回 `{slot, action, x, y}`。
只想「按 id 分发、脚本不存区域表」就走 daemon 原生区域事件：`vt.rgParseEv(line)` → `{id, ev, slot, x, y}`（id 永远是面板里的最新名字）。
可运行示例：`clients/vtouch_region_min.js`（最小区域触发点击）、`clients/vtouch_touchback.js`（回触）、`clients/vtouch_orient_demo.js`（四角换算复核）。

## 协议

- `docs/VTOUCH_PROTOCOL.md` —— WebSocket 协议（命令/事件/region 配置，含设计说明）
- `docs/VTOUCH_BUNDLE.md` —— 单文件包完整使用手册（API 全表 / 线协议 / 区域系统 / 注意事项 / 排障）

## 常见问题

### 连接失败
- 检查服务是否运行：`su -c "ps -A | grep vtouchd"`
- 检查 WebSocket 端口：`su -c "netstat -tlnp | grep 27183"`

### 触摸无响应
- SELinux 保持 Enforcing 即可（root 域释放二进制、建 composer 图层已实测通过）；`su -c "getenforce"` 仅用于确认
- 检查虚拟设备：`su -c "dumpsys input | grep vtouch"`

### 脚本立即退出
- 检查 root：`su -c id`（AutoJs6 通过 su 调起 bundle 的自释放逻辑）
- 确认脚本末尾有常驻逻辑（读循环 / setInterval）：脚本一结束就触发 `events.on("exit")` → `vt.stop()`，把面板一起收掉，表现为「窗口一闪就没」

## 文件结构

```
vtouch-project/
├── src/vtouchd.c         # C 源码（单一二进制）
├── clients/              # AutoJs6 脚本（bundle 构建产物、示例）
├── scripts/              # 构建 / 同步脚本
├── tests/                # WS 冒烟测试
├── docs/                 # 文档
├── extension/sync-ext/   # Chrome 扩展（网页同步通道）
└── .github/workflows/    # Actions 构建
```

## 许可证

MIT License
