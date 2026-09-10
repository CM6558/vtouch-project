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
vtouchd（单进程：合并器 + WebSocket + region 匹配，一个 poll 循环）
    ├── 真实触点（1:1 透传）
    └── 模拟触点（virt 槽，WS 注入）
          ↓ /dev/uinput
Android InputReader/InputDispatcher
          ↓
应用 MotionEvent

AutoJs6（clients/vtouch_bundle.js：自释放 + 连接 + 管理 UI）
    ↓ WebSocket (127.0.0.1:27183)
vtouchd
    ├── 物理事件流 pev（跟随订阅）
    └── 区域事件 region_ev（匹配推送）
```

## 部署

二进制不随仓库分发——由 GitHub Actions **Build vtouch (Android NDK)** 工作流构建，从 artifact 下载：

```bash
adb push vtouch_bundle-arm64.js /sdcard/vtouch_bundle.js
```

在 AutoJs6 里运行示例脚本即可：bundle 首次运行自释放 vtouchd 到 `/data/local/tmp/vtouchd`、启动并连接（依赖 root / su）。

## 区域监听示例

```javascript
var vt = require("/sdcard/vtouch_bundle.js");
eval(vt.uiSource);   // 可选：管理 UI（框选添加/删除区域，配置自动下发）

vt.connect({
    onDown:  function (region, f) { log("down  " + region.id + " s" + f.slot + " " + f.x + "," + f.y); },
    onMove:  function (region, f) { log("move  " + region.id + " s" + f.slot + " " + f.x + "," + f.y); },
    onUp:    function (region, f) { log("up    " + region.id + " s" + f.slot); },
    onEnter: function (region, f) { log("enter " + region.id + " s" + f.slot); },
    onExit:  function (region, f) { log("exit  " + region.id + " s" + f.slot); }
});
```

## 协议

- `docs/VTOUCH_PROTOCOL.md` —— WebSocket 协议（命令/事件/region 配置，含设计说明）

## 常见问题

### 连接失败
- 检查服务是否运行：`su -c "ps -A | grep vtouchd"`
- 检查 WebSocket 端口：`su -c "netstat -tlnp | grep 27183"`

### 触摸无响应
- 检查 SELinux：`su -c "getenforce"`（需 Permissive）
- 检查虚拟设备：`su -c "dumpsys input | grep vtouch"`

### 脚本立即退出
- 检查 root：`su -c id`（AutoJs6 通过 su 调起 bundle 的自释放逻辑）
- 检查 WebSocket 重连逻辑（bundle 内置 bootWatch 自动重连 + 重新下发配置）

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
