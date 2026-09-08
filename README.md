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
| `clients/plugins/vtouch.js` | AutoJs6 SDK v2（项目插件） |
| `clients/plugin-apk` | APK 应用插件（org.vtouch.plugin，SDK 唯一载体） |

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
scripts/              安装 / 启动 / 打包脚本
sdcard/vtouch-merge/  手机运行目录（随安装包分发）
build/                编译产物
tests/                Python 冒烟测试
docs/                 文档
extension/sync-ext/   Chrome 扩展（一键网页同步通道）
```

## 一键同步（本地 → GitHub，零 git push）

本地改动无需 `git push`，两种通道自动选择：

```sh
# 一步同步 (有 token 走 GitHub REST API, 秒级静默; 无 token 自动回退 Chrome 扩展通道)
python scripts/sync_auto.py

# 完全静默后台守护 (开机自启, 每 N 秒检测变化自动同步)
python scripts/sync_auto_install.py install --watch 300   # 注册开机自启
python scripts/sync_auto_install.py uninstall             # 注销
```

**API 通道（推荐）**：GitHub fine-grained PAT 存 `D:\MYP\sync-config.json`（`{"token": "github_pat_..."}`）
或环境变量 `VT_SYNC_TOKEN`。需 `Contents: Read and write` 权限（仅 vtouch-project 仓库）。
秒级、免 Chrome、天然支持删除，`409` 时跳过（以远程为主）。

**扩展通道（回退）**：`chrome://extensions` → 开发者模式 → 加载已解压的扩展 → `extension/sync-ext`。
Chrome 需已登录 GitHub。扩展经代理隧道连本机局域网 IP（默认 `10.164.120.30`，
见 `extension/sync-ext/offscreen.js` 与 `manifest.json`，换机器需改）；`127.0.0.1` 会被系统代理 CONNECT 拦截。

状态缓存：`D:\MYP\sync-state.json`（sha256 增量，内容未变不提交）；日志：`D:\MYP\sync-auto.log`。
