# vtouch：Android 触摸合并系统

通过用户态 `EVIOCGRAB` + `uinput` 将真实触摸与模拟触摸合并为单一统一触摸设备，供 AutoJs6 / 第三方程序通过 WebSocket 调用。

- 用户态实现，无需内核模块（.ko）
- 真实触摸与模拟触摸共存、互不干扰
- WebSocket 接口（`ws://127.0.0.1:27183`）
- 动态发现触摸设备，不硬编码 event 节点
- 自动坐标转换（逻辑屏幕 ↔ 原始触摸轴）

## 组件

| 组件 | 说明 |
|------|------|
| `vtouchmerge` | 核心触摸合并程序（EVIOCGRAB + uinput） |
| `vtouchws` | WebSocket 桥接 |
| `vtouchsupervise` | Worker 监控 / 崩溃自动重启 |
| `vtouch_onefile_example.js` | AutoJs6 单文件 SDK |
| KernelSU 模块 | 开机自启动服务 |

## 快速安装（共享存储）

```sh
adb push vtouch-merge-sdcard-latest.zip /sdcard/
adb shell "cd /sdcard && unzip -o vtouch-merge-sdcard-latest.zip -d vtouch-merge"
adb shell
su
sh /sdcard/vtouch-merge/install_from_sdcard.sh
```

输出 `[OK] VTOUCH_READY=1` 表示服务启动成功。

## AutoJs6 SDK

```javascript
var vt = new VTouch().connect(function (client) {
    threads.start(function () {
        var f = client.finger();
        f.tap(client.width / 2, client.height / 2, 60); // 点击
        f.swipe(200, 2000, 900, 2000, 1000); // 滑动
    });
});
```

完整文档见 [docs/README.md](docs/README.md)。

## 构建

Android NDK r27d 交叉编译，详见 [docs/README.md](docs/README.md) 与 [src/Android.mk](src/Android.mk)。

## 目录

```
src/                  C 源码（vtouchmerge、vtouchws、vtouchsupervise）
clients/              AutoJs6 SDK
scripts/              安装 / 启动脚本
ksu-module/           KernelSU 模块
sdcard/vtouch-merge/  手机运行目录
build/                编译产物
tests/                Python 冒烟测试
docs/                 文档
```