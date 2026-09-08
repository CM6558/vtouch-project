# vtouch 项目结构

```text
vtouch-project/
├── src/
│   ├── vtouchmerge.c             # 核心触摸合并器（EVIOCGRAB + uinput）
│   ├── vtouchws.c                # 127.0.0.1 WebSocket bridge
│   ├── vtouchsupervise.c         # worker 监控器（心跳检测 + 崩溃自动重启）
│   └── Android.mk                # NDK integration
├── clients/
│   ├── plugin-apk/               # APK 应用插件（org.vtouch.plugin，SDK v2 唯一载体）
│   ├── plugins/vtouch.js         # AutoJs6 项目插件（SDK v2 分发）
│   ├── vtouch_plugin_example.js  # 插件用法示例
│   └── vtouch_plugin_apk_test.js # 插件真机测试脚本
├── scripts/
│   ├── install_from_sdcard.sh    # 手机端安装/启动（部署入口）
│   ├── vtouch-start.sh           # 启动服务
│   ├── vtouch-stop.sh            # 停止服务
│   ├── verify_vtouch_merge.sh    # 服务健康检查
│   └── make_install_archives.py  # 生成 sdcard 安装包 zip
├── sdcard/vtouch-merge/          # 手机运行目录（随安装包分发）
├── docs/
│   ├── README.md                 # 总文档
│   ├── VTOUCH_MERGE.md           # 合并器架构文档
│   ├── VTOUCH_PROTOCOL.md        # WebSocket/文本协议
│   └── WEBSOCKET_DESIGN.md       # WebSocket 设计
├── tests/
│   └── ws_smoke.py               # WebSocket 握手、ping、tap 冒烟测试
└── build/                        # arm64 构建产物
    ├── vtouchmerge
    ├── vtouchsupervise
    └── vtouchws
```

## 组件关系

```text
AutoJs6 WebSocket
       │  ws://127.0.0.1:27183
       ▼
   vtouchws
       │  Unix socket (/data/local/tmp/vtouch-runtime/merge.sock)
       ▼
   vtouchmerge
       │  /dev/uinput
       ▼
Android InputReader
```

`vtouchmerge` 是唯一直接写入 `/dev/uinput` 的组件。`vtouchws` 只负责协议转换和本机回环连接，不监听局域网。`vtouchsupervise` 通过心跳管道监控 `vtouchmerge`，崩溃后指数退避自动重启。

## 构建

在 Windows Git Bash 中（NDK r27d）：

```sh
NDK=C:/Users/<user>/android-ndk-r27d
aarch64=$NDK/toolchains/llvm/prebuilt/windows-x86_64/bin/aarch64-linux-android24-clang.cmd

"$aarch64" -O2 -Wall -Wextra -Werror -D_GNU_SOURCE src/vtouchmerge.c -o build/vtouchmerge
"$aarch64" -O2 -Wall -Wextra -Werror -D_GNU_SOURCE -DVT_MERGE_LIBRARY src/vtouchsupervise.c src/vtouchmerge.c -o build/vtouchsupervise
"$aarch64" -O2 -Wall -Wextra -pthread src/vtouchws.c -o build/vtouchws
```

或使用 `src/Android.mk`：`cd src && ndk-build`。

## 部署

```sh
# 方式一：安装包（推荐）
adb push vtouch-merge-sdcard-latest.zip /sdcard/
adb shell
su
sh /sdcard/vtouch-merge/install_from_sdcard.sh

# 方式二：手动部署
adb push build/vtouchmerge /sdcard/vtouchmerge
adb push build/vtouchws /sdcard/vtouchws
adb shell 'su -c "cp /sdcard/vtouchmerge /data/local/tmp/vtouchmerge; cp /sdcard/vtouchws /data/local/tmp/vtouchws; chmod 755 /data/local/tmp/vtouchmerge /data/local/tmp/vtouchws"'
```

先启动 `vtouchmerge`，再启动 `vtouchws`。AutoJs6 SDK 的 `startService()` 也会自动完成这一过程。
