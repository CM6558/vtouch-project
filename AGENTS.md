# vtouch-project

Android touch simulation/merging system for rooted devices. Merges physical touch input with virtual touches via userspace `EVIOCGRAB` + `uinput`, exposing a single unified touchscreen to Android.

## Project structure

```
src/vtouchd.c         C source (single binary: merger + WebSocket + region matching)
src-ui/               ImGui panel (vtouch_ui.cpp + VTouchUI.java); embedded INTO the bundle
clients/              AutoJs6 scripts (vtouch_bundle.js is Actions build output, NOT tracked;
                      vtouch_touchback.js is tracked)
scripts/              build_bundle.py (single source -> clients/vtouch_bundle.js), build_ui.sh
                      (panel build), sync_auto*.py, sync_web.py
tests/                WebSocket smoke tests (ws_smoke.py, ws_kick.js)
docs/                 Protocol and design docs
extension/sync-ext/   Chrome extension (web sync channel fallback)
.github/workflows/    build.yml: NDK r27d compile vtouchd (arm64+x86_64) + generate bundle
```

Binaries and bundles are **never committed** — GitHub Actions builds them, artifacts carry
`vtouch_bundle-arm64.js` (device) / `vtouch_bundle-x86_64.js` (AVD) + `vtouchd`.

## Sync (local → GitHub, no git push)

```sh
python scripts/sync_auto.py          # one-shot REST API sync (token in D:\MYP\sync-config.json)
python scripts/sync_auto_install.py install --watch 300   # background daemon
```

git is used only to align local repo state (`git fetch` + `git reset --hard origin/master`),
never to push changes.

## Build

Local manual compile (NDK r27d):

```sh
NDK=D:/ANDROID/SDK/ndk/30.0.15729638/toolchains/llvm/prebuilt/windows-x86_64/bin
"$NDK/aarch64-linux-android24-clang" -O2 -Wall -Wextra -Werror -D_GNU_SOURCE src/vtouchd.c -o build/vtouchd
```

CI (`.github/workflows/build.yml`, workflow name **Build vtouch (Android NDK)**) does the same for
arm64 + x86_64, then runs `python3 scripts/build_bundle.py` for each ABI and uploads artifacts.

## Test

```sh
python tests/ws_smoke.py        # requires vtouchd running on 127.0.0.1:27183
```

## Deploy to device

**One file. No device-side scripts.** `clients/vtouch_bundle.js` carries everything: vtouchd,
the ImGui panel (`classes.dex` + `libtestimgui.so`, stored gz+b64), region store/engine, Finger API.

```sh
adb push clients/vtouch_bundle.js /sdcard/vtouch_bundle.js
```

Everything else is script-controlled from AutoJs6:

```javascript
var vt = require("/sdcard/vtouch_bundle.js");
vt.uiStart();         // 面板起来 = UI + daemon（EVIOCGRAB + WS 27183 + regions.conf）；dex/so 首次自动释放
var c = vt.connect();  // 别另调 ensure()：面板本身就是 daemon，会抢 27183
...
vt.stop();             // 连面板一起收，释放 EVIOCGRAB（面板内「退出」按钮同效）
```

- `vt.uiStart()` 无参数、无模式开关：面板即 daemon（grab + WS 27183 + regions.conf）
- 另有 `uiAlive / uiPid / uiRestart / uiStop / uiDeploy / uiTail(n)`
- 进程真相只认 `pidof vtouch-ui`（`--nice-name` 后 `/proc/<pid>/comm` 是 `main`、`cmdline` 只剩 nice-name，pid 文件只是书签）：`uiAlive()` 走 pidof，收尾 `kill -9` 后回读确认
- 区域唯一归属是面板：面板读写 `/data/local/tmp/vtouch-runtime/regions.conf`（首行带版本号 `#vtouch-regions v2`，版本不符整份丢弃），脚本侧不存任何区域状态；`vt.rgList(c)` 回读面板当前表、`vt.rgPush(c, rs)` 整表下发（内部先 `region clear`）。先 `rgList` 后开收包循环

## Conventions

- C code: `-O2 -Wall -Wextra -Werror` flags, POSIX APIs, Android NDK APIs.
- JavaScript: AutoJs6 API (`WebSocket.EVENT_*`, `threads.start()`, `events.on("exit")`).
- WebSocket: `ws://127.0.0.1:27183` only (loopback, single client, new kicks old).
- Device runtime: `/data/local/tmp/vtouch-runtime/` for PID files and logs.
- Panel binaries: `/data/local/tmp/vtouch-ui/{classes.dex,libtestimgui.so}` — written by the bundle only.
- Panel runtime files: `vtouch-runtime/vtouch-ui.{pid,mode,log}` — `pid` says alive, `mode` says which mode
  (native writes it at startup; `/proc/<pid>/cmdline` is unusable, see Pitfalls).
- WebSocket is **single client, new kicks old** — never run two subscriber scripts at once.

## Pitfalls

- **`/sdcard` noexec**: Android mounts shared storage `noexec`. Never execute ELF directly from `/sdcard` — copy to `/data/local/tmp/` first (bundle does this).
- **SELinux**: AutoJs6 app context may be denied access to `/data/local/tmp/` — `setenforce 0` (Permissive) for the su/self-extract path.
- **`sleep()` blocks event loop**: In AutoJs6, `sleep()` on main thread blocks WebSocket event callbacks. Use `threads.start()` for blocking work.
- **`EVIOCGRAB` recovery**: If vtouchd crashes, physical touch is grabbed until fd closes. AutoJs6-side bootWatch reconnects and re-extracts (seconds-level blind window).
- **Dynamic touch discovery**: Never hardcode `/dev/input/eventX`. vtouchd scans `/dev/input/event0..63` for the first Type-B multitouch device.
- **Coordinate conversion**: AutoJs6 uses logical coordinates; vtouchd converts via `-w`/`-h` flags (raw 0..32767 → logical).
- **su for self-extract**: AutoJs6 `Runtime.exec("su")` is interactive stdin; a v8 wrapper (/data/local/tmp/su_wrap.sh + sud.sh daemon, sudq file queue) implements it on stock systems.
- **`--nice-name` eats `/proc/<pid>/cmdline`**: `app_process --nice-name=vtouch-ui` rewrites argv, so
  `/proc/<pid>/cmdline` only holds `vtouch-ui` and the mode is unrecoverable there. Native writes
  `vtouch-runtime/vtouch-ui.mode` instead; scripts do alive=(pid file + `kill -0`), mode=(that file).
- **CRLF kills on-device scripts**: writing `.sh` from Windows tooling injects `\r`, and device `sh`
  fails with `bad number`. Keep device-bound scripts LF-only (`newline="\n"`).
- **Embed gz, not raw**: `libtestimgui.so` is 937 KB raw / 441 KB gz (~588 KB as b64). The bundle embeds
  gz+b64 and gunzips in Java (`GZIPInputStream`), keeping the single file at ~769 KB instead of ~1.5 MB.
- **Deploy is md5-idempotent**: `uiDeploy()` compares the embedded md5 with on-device `md5sum` and only
  rewrites on mismatch — never hand-copy dex/so over the deployed pair.
- **Region matching is listen-only**: physical touch is always 1:1 forwarded; region_match never injects (no tap-backfill by design).
- **Console injection on AVD**: emulator console `event mouse` down events carry no coordinates (ABS_X/Y type-A fallback added); move-under-down does not update position — physical `move` events must be validated on real hardware.

## Debugging

```sh
# vtouchd log (unbuffered)
adb shell tail -f /data/local/tmp/vtouch-runtime/vtouchd.log
# region config + hit events are logged there (region add / ev <id> <type>)
```
