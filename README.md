# vtouch：Android 触摸合并系统

通过用户态 `EVIOCGRAB` + `uinput` 把真实触摸与模拟触摸合并成**同一条**触摸流，Android 应用侧无法区分；供 AutoJs6 通过本地 WebSocket 调用。**区域匹配、管理面板、触摸注入全部单进程运行**。

- 用户态实现，无需内核模块；真实触摸与合成触摸共存、互不干扰
- 动态发现触摸设备（Type-B MT），不硬编码 event 节点；自动做逻辑坐标 ↔ 原始触摸轴换算
- 面板侧原生 region 匹配（≤32 区域，监听 down/up/enter/exit/move，**纯监听不代点**）
- 单进程整合：核心 `src/vtouchd.c` 编译为 C 库（`vtouch_init/poll_step/region_*/set_callbacks`），嵌入 `app_process` 渲染进程（SurfaceControl 图层 → EGL GLES2 → Dear ImGui），UI / 触摸 / 匹配 / WS 同进程直连
- 脚本侧只有一个入口：`vt.onRegion(id, [ev], fn)`，其余仪式（起面板、连接、订阅、校验、读线程、收尾）全在库里

## 30 秒上手

```javascript
var vt = require("/sdcard/vtouch_bundle.js");

// 区域 s3 被按下/抬起/进入/移出时回调；回调已在子线程，可直接 sleep/长按/拖拽
vt.onRegion("s3", function (h) {
    log(h.id + " " + h.ev + " @" + h.x + "," + h.y);
    vt.finger().tap(h.x, h.y);        // 虚拟回注（与物理触摸合并后一起回系统）
});
```

`ev` 省略 = `down/up/enter/exit`（**不含 move**）；要 move 就显式写 `"move"` / `"down,move"` / `["down","move"]` / `"*"`。
`id` 省略或 `"*"` = 所有区域。返回值带 `.stop()`。

库内自动完成：`uiStart()`（起面板 = 起 daemon）→ `connect()` → `sub region` → `region list` 校验 id（写错/被禁用会 toast 报错，不会静默）→ 常驻读线程（`ping/pong` 探活）→ 事件过滤 → 回调丢子线程 → 主线程保活；
**脚本结束自动收尾**：收面板 + 释放 EVIOCGRAB。想留面板用 `vt.autoStop(false)`；
被新实例连上时旧实例自退让位（不动面板）。

## 组件

| 路径 | 说明 |
|---|---|
| `src/vtouchd.c` | 核心：EVIOCGRAB 采集 + uinput 注入 + region 匹配 + WS 服务器（`127.0.0.1:27183`）。库化接口 `vtouch_init/poll_step/cleanup`、`vtouch_region_clear/count/add/get_region`、`vtouch_set_callbacks`；命令行入口保留（`-w -h [-v] [-p] [-ui]`） |
| `src-ui/vtouch_ui.cpp` + `VTouchUI.java` | 单进程面板：Java 只反射拿 SurfaceControl 图层，其余全 C++（EGL GLES2 + Dear ImGui）。区域表格（名称/形状/坐标/开关/显隐/删除）、＋矩形/＋圆形框选、事件日志、全屏透明 overlay（区域描边着色，命中闪烁） |
| `scripts/build_ui.sh` | 编面板 → `build/ui/libtestimgui.so` + `build/ui/classes.dex` |
| `scripts/build_bundle.py` | **唯一来源**：把 `build/vtouchd` + 面板 dex/so + JS 库装配成 `clients/vtouch_bundle.js` |
| `clients/vtouch_bundle.js` | 生成物（设备侧唯一交付物）：自释放二进制 + WS 协议封装 + `onRegion` 库。文件较大，不入库 |
| `clients/vtouch_region_min.js` | 最小可跑示例（一行 `onRegion` + 业务回调） |
| `clients/vtouch_touchback.js`、`vtouch_orient_demo.js` | 示例：区域触发回触（滑/点）、转屏演示 |
| `tests/onregion_harness.js` | 主机侧 Node 桩测（假 AutoJs6 + 假 daemon，46 项断言） |

## 构建（本机 Git Bash）

```sh
NDK=C:/Users/21102/android-ndk-r27d
A64=$NDK/toolchains/llvm/prebuilt/windows-x86_64/bin/aarch64-linux-android24-clang.cmd

"$A64" -O2 -Wall -Wextra -Werror -D_GNU_SOURCE src/vtouchd.c -o build/vtouchd   # ① 核心
sh scripts/build_ui.sh                                                          # ② 面板 dex + so
python scripts/build_bundle.py                                                  # ③ 出 bundle
```

`build_ui.sh` 需要 JDK（`javac`/`d8`）与 Android SDK `platforms/android-24`、`build-tools/34.0.0`，以及 `thirdparty/imgui`（本仓库不入库，需自备）。
路径都可用环境变量覆盖（`NDK_ROOT` / `ANDROID_SDK_ROOT` / `BUILD_TOOLS_VERSION` / `API_LEVEL`）。
各产物的作用、**哪个才是手机可运行的最小包**、设备侧落盘位置见 [`docs/ARTIFACTS.md`](docs/ARTIFACTS.md)。
只要纯 daemon、不需要面板时用 headless 构建：`python scripts/build_bundle.py --headless`（此时 `uiStart()` 会明确报错，不会静默起不来）；默认不带这个开关**必须**有面板产物，避免误发一个没有 UI 的包。

CI（`.github/workflows/build.yml`）跑的就是上面三步：装 JDK 17 + Android SDK build-tools + 拉 `thirdparty/imgui` v1.91.8（都有 cache），**arm64 出的是带面板的完整 bundle**（产物 `vtouch_bundle-arm64.js`，并断言 >400KB 且内嵌面板）；x86_64 因面板 `.so` 是 arm64 的，只出 `vtouch_bundle-x86_64-headless.js`（AVD 测试用）。

## 部署与验证

```sh
adb push clients/vtouch_bundle.js /sdcard/
adb shell md5sum /sdcard/vtouch_bundle.js          # 回读对账，别信 push 回执
adb shell su -c "pidof vtouch-ui"                  # 面板是否在跑（= 是否抓着 EVIOCGRAB）
adb shell su -c "tail -20 /data/local/tmp/vtouch-runtime/vtouch-ui.log"
```

在 AutoJs6 里运行 `clients/vtouch_region_min.js`（或你自己的脚本）即可。
**强杀 App（强行停止）会跳过 exit 事件 → 收尾钩子不执行，面板会留在后台抓着触摸**，手动清理：`adb shell su -c "kill -9 $(pidof vtouch-ui)"`。

## 测试

```sh
node tests/onregion_harness.js     # 期望 ALL PASS 46/46
```

## 文档

- [`docs/ARTIFACTS.md`](docs/ARTIFACTS.md) — **编译产物说明**：哪个产物才是手机可运行的最小包（一个 `vtouch_bundle.js`）、各中间产物作用、设备侧落盘位置、CI 产物、完整性判据
- [`docs/VTOUCH_BUNDLE.md`](docs/VTOUCH_BUNDLE.md) — **使用手册**（客户端 API / 区域监听 / 生命周期 / 常见问题）
- [`docs/VTOUCH_PROTOCOL.md`](docs/VTOUCH_PROTOCOL.md) — WS 线协议（`sub` / `region` / `pev` / `region_ev` / `set_virtual` …）
- [`docs/diagrams/`](docs/diagrams/README.md) — 工程图（全流程总览、优化前后对照；JSON 源 + SVG/PNG，可重渲）

## 目录

```
src/          C 核心（vtouchd.c）
src-ui/       面板（vtouch_ui.cpp + VTouchUI.java）
scripts/      构建：build_bundle.py（唯一来源）、build_ui.sh
clients/      AutoJs6 脚本与生成物（vtouch_bundle.js）
tests/        主机侧桩测（onregion_harness.js）
docs/         手册 / 协议 / 流程图
```
