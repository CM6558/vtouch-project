# vtouch：Android 触摸合并系统

## 项目简介

`vtouch` 是一个用于 Android root 设备的触摸合并系统，通过用户态 `EVIOCGRAB` + `uinput` 技术，将真实触摸输入与模拟触摸合并为单一统一触摸设备，使 Android 应用无法区分真实与模拟触摸。

**核心特性：**
- 真实触摸与模拟触摸同时共存，互不干扰
- 用户态实现，无需内核模块（.ko）
- WebSocket 接口，支持 AutoJs6/第三方程序调用
- 动态发现触摸设备，不硬编码 event 节点
- 自动坐标转换（逻辑屏幕坐标 ↔ 原始触摸轴）

## 架构

```text
真实触摸面板
    ↓ EVIOCGRAB
vtouchmerge
    ├── 真实触点
    └── 模拟触点
          ↓
单一 merged uinput 设备
          ↓
Android InputReader/InputDispatcher
          ↓
应用 MotionEvent

AutoJs6/第三方程序
    ↓ WebSocket (127.0.0.1:27183)
vtouchws
    ↓ Unix socket
vtouchmerge
```

## 组件

| 组件 | 说明 |
|------|------|
| `vtouchmerge` | 核心触摸合并程序，接管真实触摸并合并模拟触点 |
| `vtouchws` | WebSocket 桥接，将 AutoJs6 调用转发到 vtouchmerge |
| `vtouchsupervise` | Worker 监控器，崩溃时自动重启 |
| `vtouch_onefile_example.js` | AutoJs6 单文件 SDK |
| KernelSU 模块 | 开机自启动服务 |

## 构建

使用 Android NDK r27d 交叉编译：

```bash
NDK=C:/Users/21102/android-ndk-r27d
aarch64=$NDK/toolchains/llvm/prebuilt/windows-x86_64/bin/aarch64-linux-android24-clang.cmd

# vtouchmerge
"$aarch64" -O2 -Wall -Wextra -Werror -D_GNU_SOURCE src/vtouchmerge.c -o build/vtouchmerge

# vtouchsupervise
"$aarch64" -O2 -Wall -Wextra -Werror -D_GNU_SOURCE -DVT_MERGE_LIBRARY src/vtouchsupervise.c src/vtouchmerge.c -o build/vtouchsupervise

# vtouchws
"$aarch64" -O2 -Wall -Wextra src/vtouchws.c -o build/vtouchws
```

## 安装与运行

### 1. 共享存储安装（推荐测试）

```bash
# 推送安装包到手机
adb push vtouch-merge-sdcard-latest.zip /sdcard/
adb shell "cd /sdcard && unzip -o vtouch-merge-sdcard-latest.zip -d vtouch-merge"

# 执行安装脚本
adb shell
su
sh /sdcard/vtouch-merge/install_from_sdcard.sh
```

输出 `[OK] VTOUCH_READY=1` 表示服务启动成功。

### 2. KernelSU 模块（生产环境）

```bash
# 刷入模块
adb push vtouch-merge-ksu-latest.zip /sdcard/
# 在 KernelSU 管理器中安装该 zip
# 重启设备
```

### 3. 运行 AutoJs6 SDK

```bash
# 推送 SDK 到手机
adb push clients/vtouch_onefile_example.js /sdcard/vtouch-merge/
```

在 AutoJs6 中运行 `/sdcard/vtouch-merge/vtouch_onefile_example.js`。

## AutoJs6 SDK 使用

```javascript
var vt = new VTouch().connect(function (client) {
    threads.start(function () {
        var f = client.finger();
        
        // 点击
        f.tap(client.width / 2, client.height / 2, 60);
        
        // 滑动
        f.swipe(200, 2000, 900, 2000, 1000);
        
        // 按下、等待、移动、抬起
        f.down(720, 1584);
        sleep(1000);
        f.move(760, 1584);
        f.up();
    });
});
```

**Finger 对象方法：**
- `f.down(x, y)` - 按下
- `f.move(x, y)` - 移动
- `f.up()` - 抬起
- `f.tap(x, y, ms)` - 点击（默认 60ms）
- `f.swipe(x1, y1, x2, y2, durationMs)` - 滑动（默认 300ms）
- `f.state()` - 返回 "down" 或 "up"
- `f.cancel()` - 清理本地状态

## 停止服务

```javascript
vt.stopService();
```

或手动停止：

```bash
su -c "sh /data/local/tmp/vtouch-stop.sh"
```

## 常见问题

### 连接失败
- 检查服务是否运行：`su -c "ps -A | grep vtouch"`
- 检查 WebSocket 端口：`su -c "netstat -tlnp | grep 27183"`

### 触摸无响应
- 检查 SELinux：`su -c "getenforce"`
- 检查虚拟设备：`su -c "dumpsys input | grep vtouch-merged"`

### 脚本立即退出
- 确保 `events.on("exit")` 中调用 `stopService()`
- 检查 WebSocket 重连逻辑

## 文件结构

```
vtouch-project/
├── src/                    # C 源码
├── clients/                # AutoJs6 SDK
├── scripts/                # 安装/启动脚本
├── ksu-module/             # KernelSU 模块
├── sdcard/vtouch-merge/    # 手机运行目录
├── build/                  # 编译产物
├── tests/                  # 测试脚本
└── docs/                   # 文档
```

## 许可证

MIT License