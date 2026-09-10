# vtouch 项目结构

```text
vtouch-project/
├── src/
│   └── vtouchd.c                 # 单进程合并器 + WS + 区域匹配（唯一二进制）
├── clients/
│   ├── vtouch_bundle.js          # 构建产物（内嵌 vtouchd，Actions 生成，不入库）
│   ├── vtouch_region_demo.js     # 区域监听示例（五事件回调）
│   └── vtouch_bundle_example.js  # bundle 用法示例
├── scripts/
│   ├── build_bundle.py           # bundle 构建（内嵌二进制 + UI + 协议封装）
│   ├── sync_auto.py              # 一键同步（GitHub REST API）
│   ├── sync_auto_install.py      # 同步守护注册/注销
│   └── sync_web.py               # Chrome 扩展通道同步
├── docs/
│   ├── README.md                 # 总文档
│   └── VTOUCH_PROTOCOL.md        # WebSocket 协议（命令/事件/region，含设计）
├── tests/
│   ├── ws_smoke.py               # WebSocket 握手、ping、tap 冒烟测试
│   └── ws_kick.js                # 单客户端踢除测试
├── extension/sync-ext/           # Chrome 扩展（网页同步通道）
└── .github/workflows/build.yml   # Actions：编译 vtouchd 双 ABI + 生成 bundle
```

## 组件关系

```text
AutoJs6 (bundle: 自释放 + 连接 + UI)
       │  ws://127.0.0.1:27183
       ▼
vtouchd（合并器 + WS + region 匹配，共用一个 poll 循环）
       │  /dev/uinput
       ▼
Android InputReader
```

`vtouchd` 直接写入 `/dev/uinput`，只监听本机回环，不监听局域网。坏 WS 客户端只关闭该连接（虚拟触点复位），grab 不丢；只有进程整体崩溃才丢触摸，由 AutoJs6 侧重连循环自动拉起恢复。

## 构建（GitHub Actions）

`.github/workflows/build.yml`（**Build vtouch (Android NDK)**）：NDK r27d 编译 vtouchd（arm64 + x86_64）→ 跑 `scripts/build_bundle.py` 生成 `vtouch_bundle-arm64.js` / `vtouch_bundle-x86_64.js` → artifact 上传。本地手动编译：

```sh
NDK=D:/ANDROID/SDK/ndk/30.0.15729638/toolchains/llvm/prebuilt/windows-x86_64/bin
"$NDK/aarch64-linux-android24-clang" -O2 -Wall -Wextra -Werror -D_GNU_SOURCE src/vtouchd.c -o build/vtouchd
```

## 部署

```sh
# 从 Actions artifact 下载 vtouch_bundle-arm64.js，放到手机
adb push vtouch_bundle-arm64.js /sdcard/vtouch_bundle.js
# AutoJs6 运行示例（bundle 自释放 vtouchd 并连接）
adb shell am start -n org.autojs.autojs6/org.autojs.autojs.external.open.RunIntentActivity \
  -a android.intent.action.VIEW -d file:///sdcard/vtouch_region_demo.js -t application/x-javascript
```
