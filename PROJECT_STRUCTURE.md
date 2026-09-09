# vtouch 项目结构

```text
vtouch-project/
├── src/
│   ├── vtouchd.c                 # 单进程合并器+WS（推荐；旧双进程见下）
│   ├── vtouchmerge.c             # 旧双进程合并器（回滚备用）
│   ├── vtouchws.c                # 旧双进程 WS bridge（回滚备用）
│   └── Android.mk                # NDK integration
├── clients/
│   ├── plugin-apk/               # APK 应用插件（org.vtouch.plugin，SDK v2 唯一载体）
│   ├── plugins/vtouch.js         # AutoJs6 项目插件（SDK v2 分发）
│   ├── vtouch_plugin_example.js  # 插件用法示例
│   └── vtouch_plugin_apk_test.js # 插件真机测试脚本
├── scripts/
│   ├── build_sdk.py              # SDK 单源构建（分发 clients + APK 胶水）
│   ├── vtouch-sdk.src.js         # SDK 唯一可读主源
│   └── verify_vtouch_merge.sh    # 开发者诊断（adb 手动跑，不随包分发）
├── sdcard/vtouch-merge/          # 手机运行目录（随安装包分发）
├── docs/
│   ├── README.md                 # 总文档
│   ├── VTOUCH_MERGE.md           # 合并器架构文档
│   ├── VTOUCH_PROTOCOL.md        # WebSocket/文本协议
│   └── WEBSOCKET_DESIGN.md       # WebSocket 设计
├── tests/
│   └── ws_smoke.py               # WebSocket 握手、ping、tap 冒烟测试
└── build/                        # arm64 构建产物
    ├── vtouchd
    ├── vtouchmerge                # 旧双进程（回滚备用）
    └── vtouchws                   # 旧双进程（回滚备用）
```

## 组件关系

```text
AutoJs6 WebSocket
       │  ws://127.0.0.1:27183
       ▼
   vtouchd（单进程：合并器 + WS，共用一个 poll 循环，无 UDS 跳转）
       │  /dev/uinput
       ▼
Android InputReader
```

`vtouchd` 直接写入 `/dev/uinput`，只监听本机回环，不监听局域网。坏 WS 客户端只关闭该连接（虚拟触点复位），grab 不丢；只有进程整体崩溃才丢触摸，由启动脚本重拉恢复。旧 `vtouchmerge` + `vtouchws` 双进程仅作回滚备用。

## 构建

在 Windows Git Bash 中（NDK r27d）：

```sh
NDK=C:/Users/<user>/android-ndk-r27d
aarch64=$NDK/toolchains/llvm/prebuilt/windows-x86_64/bin/aarch64-linux-android24-clang.cmd

"$aarch64" -O2 -Wall -Wextra -Werror -D_GNU_SOURCE src/vtouchd.c -o build/vtouchd
```

或使用 `src/Android.mk`：`cd src && ndk-build`。

## 部署

```sh
# 安装 APK（自包含：启停/健康检查全在插件 Java 侧）
adb install clients/plugin-apk/out/vtouch-plugin.apk
```

先装 APK，`new VTouch()` 自动完成释放、启动与连接。
