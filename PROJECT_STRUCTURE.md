# vtouch 项目结构

```text
vtouch-project/
├── src/
│   └── vtouchd.c                 # 核心：合并器 + WS + 区域匹配（命令行二进制 / 库化接口 vtouch_init/poll_step/region_*/set_callbacks）
├── src-ui/                       # 面板：app_process + composer 图层 + ImGui（面板自身即 daemon）
│   ├── vtouch_ui.cpp             # 面板实现（EGL/GLES2 + Dear ImGui + 触摸路由 + regions.conf 读写）
│   └── VTouchUI.java             # Java 宿主（只建 SurfaceControl 图层送 Surface + JNI 入口）
├── clients/
│   ├── vtouch_bundle.js          # 唯一交付物（构建产物）：内嵌 vtouchd + 面板 classes.dex + gz(libtestimgui.so)
│   ├── vtouch_touchback.js       # 监听+回触示例（uiStart + rgList/rgPush + 区域触发→虚拟上滑/点按）
│   └── vtouch_orient_demo.js     # 旋转/四角区域示例（复核逻辑坐标换算）
├── scripts/
│   ├── build_ui.sh               # 面板构建（javac/d8 → NDK 编 so → llvm-strip + md5）
│   ├── build_bundle.py           # bundle 构建（唯一来源：内嵌二进制 + 协议封装 + 面板启动/释放）
│   ├── sync_auto.py              # 一键同步（GitHub REST API）
│   ├── sync_auto_install.py      # 同步守护注册/注销
│   └── sync_web.py               # Chrome 扩展通道同步
├── docs/
│   ├── README.md                 # 总文档
│   ├── VTOUCH_PROTOCOL.md        # WebSocket 协议（命令/事件/region，含设计）
│   └── ui_approaches_research.md # UI 方案调研（历史存档：floaty/ImgUI/单进程渲染选型，结论已落地）
├── tests/
│   ├── ws_smoke.py               # WebSocket 握手、ping、tap 冒烟测试
│   └── ws_kick.js                # 单客户端踢除测试
├── extension/sync-ext/           # Chrome 扩展（网页同步通道）
└── .github/workflows/build.yml   # Actions：编译 vtouchd 双 ABI + 生成 bundle
```

## 组件关系

```text
AutoJs6 (bundle: 起面板 + 连接 + 业务；不做 UI)
       │  ws://127.0.0.1:27183
       ▼
面板 = UI + daemon（或纯 headless vtouchd）
       ▼
vtouchd 核心（EVIOCGRAB 采集 + uinput 注入 + region 匹配 + WS，一个 poll 循环）
       │  /dev/uinput
       ▼
Android InputReader

单进程 UI（正式路径）：vtouchd 核心编译为 C 库嵌入 app_process 渲染进程
app_process（Java ~70 行: SurfaceControl 图层）→ JNI → libtestimgui.so
    ├── vtouchd 核心（同进程：触摸/匹配/WS）
    ├── 触摸回调 → ImGui io / 框选 / 区域绘制（内存直连）
    ├── 事件回调 → 面板日志 / 命中闪烁（无 WS 客户端也通知）
    └── EGL GLES2 + Dear ImGui（区域管理面板；只有这一个启动模式）

设备侧只有两个目录：
    /data/local/tmp/vtouch-ui/       dex + so（uiStart 从 bundle 内嵌释放，md5 幂等）
    /data/local/tmp/vtouch-runtime/  regions.conf / vtouch-ui.log / vtouch-ui.pid
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
  -a android.intent.action.VIEW -d file:///sdcard/vtouch_touchback.js -t application/x-javascript
```
