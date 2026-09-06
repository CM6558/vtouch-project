# vtouch 项目结构

```text
vtouch-project/
├── src/
│   ├── vtouchd.c                 # root uinput 守护进程
│   ├── vtouchctl.c               # Unix socket 命令行客户端
│   ├── vtouchws.c                # 127.0.0.1 WebSocket bridge
│   ├── vtouchmerge.c             # dynamic physical+virtual userspace merger
│   ├── vtouchsupervise.c         # worker supervisor with heartbeat restart
│   └── Android.mk                 # NDK integration
├── clients/
│   ├── autojs_vtouch_example.js  # AutoJs6 shell 调用
│   └── autojs_vtouch_ws_example.js # AutoJs6 WebSocket 调用
├── scripts/
│   ├── install_vtouch.bat        # Windows + ADB 安装
│   ├── install_vtouch.sh         # 手机端安装/启动
│   └── service-vtouchd.sh        # KernelSU service.d 启动脚本
├── docs/
│   ├── README.md                 # 总文档
│   ├── VTOUCH_PROTOCOL.md        # WebSocket/文本协议
│   └── WEBSOCKET_DESIGN.md       # WebSocket 设计
├── tests/
│   └── ws_smoke.py               # WebSocket 握手、ping、tap 冒烟测试
└── build/                        # arm64 构建产物
    ├── vtouchd
    ├── vtouchctl
    └── vtouchws
```

## 组件关系

```text
AutoJs6 WebSocket
       │  ws://127.0.0.1:27183
       ▼
   vtouchws
       │  Unix socket
       ▼
   vtouchd
       │  /dev/uinput
       ▼
Android InputReader
```

`vtouchd` 是唯一直接写入 `/dev/uinput` 的组件。`vtouchws` 只负责协议转换和本机回环连接，不监听局域网。

## 构建

在 Windows Git Bash 中：

```sh
NDK=C:/Users/21102/android-ndk-r27d
aarch64=$NDK/toolchains/llvm/prebuilt/windows-x86_64/bin/aarch64-linux-android24-clang.cmd

"$aarch64" -O2 -Wall -Wextra -pthread src/vtouchd.c -o build/vtouchd
"$aarch64" -O2 -Wall -Wextra src/vtouchctl.c -o build/vtouchctl
"$aarch64" -O2 -Wall -Wextra -pthread src/vtouchws.c -o build/vtouchws
```

## 部署

```sh
adb push build/vtouchd /sdcard/vtouchd
adb push build/vtouchctl /sdcard/vtouchctl
adb push build/vtouchws /sdcard/vtouchws
adb shell 'su -c "cp /sdcard/vtouchd /data/local/tmp/vtouchd; cp /sdcard/vtouchctl /data/local/tmp/vtouchctl; cp /sdcard/vtouchws /data/local/tmp/vtouchws; chmod 755 /data/local/tmp/vtouchd /data/local/tmp/vtouchctl /data/local/tmp/vtouchws"'
```

先启动 `vtouchd`，再启动 `vtouchws`。
