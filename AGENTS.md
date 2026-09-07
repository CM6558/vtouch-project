# vtouch-project

Android touch simulation/merging system for rooted devices. Merges physical touch input with virtual touches via userspace `EVIOCGRAB` + `uinput`, exposing a single unified touchscreen to Android.

## Project structure

```
src/                  C source (vtouchmerge, vtouchws, vtouchsupervise)
clients/              AutoJs6 JavaScript SDK (vtouch_onefile_example.js)
clients/plugins/      AutoJs6 项目插件版 SDK (vtouch.js, module.exports = VTouch)
clients/vtouch_plugin_example.js  项目插件加载示例 (plugins.load('vtouch'))
clients/plugin-apk/    AutoJs6 应用插件 APK 工程 (零依赖注册类 + 胶水层 + build.sh)
scripts/              Install/start/stop scripts and packaging tool
sdcard/vtouch-merge/  Files shipped in vtouch-merge-sdcard-latest.zip
build/                Compiled arm64 binaries
tests/                Python smoke tests
docs/                 Protocol and design docs
```

## APK 构建环境（本机已装并验证）

- JDK 17：`C:\Program Files\Eclipse Adoptium\jdk-17.0.20.101-hotspot`
- Android SDK：`C:\Users\21102\AppData\Local\Android\Sdk`（build-tools 34.0.0 + platforms android-24，腾讯镜像手动放置，无 cmdline-tools）
- 环境变量 `ANDROID_HOME` / `JAVA_HOME` 已持久化（setx）
- 构建：`bash clients/plugin-apk/build.sh`，产物 `out/vtouch-plugin.apk`（已本机验证：签名/包名/meta-data/assets 正斜杠条目/一致性）

## Build

Cross-compile with Android NDK r27d (Windows Git Bash):

```sh
NDK=C:/Users/21102/android-ndk-r27d
aarch64=$NDK/toolchains/llvm/prebuilt/windows-x86_64/bin/aarch64-linux-android24-clang.cmd

# vtouchmerge (core merger)
"$aarch64" -O2 -Wall -Wextra -Werror -D_GNU_SOURCE src/vtouchmerge.c -o build/vtouchmerge

# vtouchsupervise (worker supervisor)
"$aarch64" -O2 -Wall -Wextra -Werror -D_GNU_SOURCE -DVT_MERGE_LIBRARY src/vtouchsupervise.c src/vtouchmerge.c -o build/vtouchsupervise

# vtouchws (WebSocket bridge)
"$aarch64" -O2 -Wall -Wextra src/vtouchws.c -o build/vtouchws
```

Or use Android.mk:

```sh
cd src && ndk-build
```

## Test

WebSocket smoke test (requires vtouchws running on 127.0.0.1:27183):

```sh
python tests/ws_smoke.py
```

## Deploy to device

1. Copy binaries to `/sdcard/vtouch-merge/`:

```sh
adb push build/vtouchmerge sdcard/vtouch-merge/
adb push build/vtouchws sdcard/vtouch-merge/
```

2. Install and start via shell:

```sh
adb shell
su
sh /sdcard/vtouch-merge/install_from_sdcard.sh
```

Or push entire sdcard directory:

```sh
adb push sdcard/vtouch-merge/ /sdcard/vtouch-merge/
```

## Run AutoJs6 SDK

Push JS file to device:

```sh
adb push clients/vtouch_onefile_example.js /sdcard/vtouch-merge/
```

In AutoJs6, run `/sdcard/vtouch-merge/vtouch_onefile_example.js`.

## Conventions

- C code: `-O2 -Wall -Wextra -Werror` flags, POSIX APIs, Android NDK APIs.
- Shell scripts: `#!/system/bin/sh` (Android shell), no bashisms.
- JavaScript: AutoJs6 API (`WebSocket.EVENT_*`, `threads.start()`, `events.on("exit")`).
- Socket paths:
  - Unix socket: `/data/local/tmp/vtouch-runtime/merge.sock`
  - WebSocket: `ws://127.0.0.1:27183`
- Device runtime: `/data/local/tmp/vtouch-runtime/` for PID files and sockets.

## Pitfalls

- **`/sdcard` noexec**: Android mounts shared storage `noexec`. Never execute ELF directly from `/sdcard`—copy to `/data/local/tmp/` first.
- **SELinux**: AutoJs6 app context may be denied access to `/data/local/tmp/` or Unix sockets. Use loopback WebSocket (port 27183) instead.
- **`Shell.exec()` vs `shell(cmd, true)`**: `Shell` object uses terminal emulator (slow init); `shell(cmd, true)` uses `Runtime.exec` (faster for single commands). Prefer `shell(cmd, true)` for synchronous root commands.
- **`sleep()` blocks event loop**: In AutoJs6, `sleep()` on main thread blocks WebSocket event callbacks. Use `setInterval` for non-blocking keepalive, or run blocking work in `threads.start()`.
- **`EVIOCGRAB` recovery**: If merger crashes, physical touch is grabbed until fd closes. Supervisor must restart merger; otherwise physical touch is dead.
- **Dynamic touch discovery**: Never hardcode `/dev/input/eventX`. Merger scans `/dev/input/event0..event63` for Type-B multitouch devices.
- **Coordinate conversion**: AutoJs6 uses logical coordinates (device.width × height). Merger converts to raw touch axes via `-w`/`-h` flags.
- **`new Shell(true)` slow**: Shell object initialization is slow (~2s). Use `shell(cmd, true)` for one-shot root commands.
- **WebSocket retry**: `EVENT_FAILURE`/`EVENT_CLOSED` may not fire immediately. Add watchdog timeout (600ms) to break stalls.
- **Service lifecycle**: `events.on("exit")` must call `stopService()` to kill merger and release physical touch grab. Strong kills (`SIGKILL`) skip exit handlers—call `vt.stopService()` explicitly in business logic if needed.
