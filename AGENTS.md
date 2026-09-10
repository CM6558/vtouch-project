# vtouch-project

Android touch simulation/merging system for rooted devices. Merges physical touch input with virtual touches via userspace `EVIOCGRAB` + `uinput`, exposing a single unified touchscreen to Android.

## Project structure

```
src/vtouchd.c         C source (single binary: merger + WebSocket + region matching)
clients/              AutoJs6 scripts (vtouch_bundle.js is Actions build output, NOT tracked;
                      vtouch_region_demo.js / vtouch_bundle_example.js are tracked)
scripts/              build_bundle.py + sync_auto*.py (GitHub API sync) + sync_web.py
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

```sh
adb push vtouch_bundle-arm64.js /sdcard/vtouch_bundle.js
# in AutoJs6 run an example; bundle self-extracts vtouchd to /data/local/tmp/vtouchd and connects
```

## Conventions

- C code: `-O2 -Wall -Wextra -Werror` flags, POSIX APIs, Android NDK APIs.
- JavaScript: AutoJs6 API (`WebSocket.EVENT_*`, `threads.start()`, `events.on("exit")`).
- WebSocket: `ws://127.0.0.1:27183` only (loopback, single client, new kicks old).
- Device runtime: `/data/local/tmp/vtouch-runtime/` for PID files and logs.

## Pitfalls

- **`/sdcard` noexec**: Android mounts shared storage `noexec`. Never execute ELF directly from `/sdcard` — copy to `/data/local/tmp/` first (bundle does this).
- **SELinux**: AutoJs6 app context may be denied access to `/data/local/tmp/` — `setenforce 0` (Permissive) for the su/self-extract path.
- **`sleep()` blocks event loop**: In AutoJs6, `sleep()` on main thread blocks WebSocket event callbacks. Use `threads.start()` for blocking work.
- **`EVIOCGRAB` recovery**: If vtouchd crashes, physical touch is grabbed until fd closes. AutoJs6-side bootWatch reconnects and re-extracts (seconds-level blind window).
- **Dynamic touch discovery**: Never hardcode `/dev/input/eventX`. vtouchd scans `/dev/input/event0..63` for the first Type-B multitouch device.
- **Coordinate conversion**: AutoJs6 uses logical coordinates; vtouchd converts via `-w`/`-h` flags (raw 0..32767 → logical).
- **su for self-extract**: AutoJs6 `Runtime.exec("su")` is interactive stdin; a v8 wrapper (/data/local/tmp/su_wrap.sh + sud.sh daemon, sudq file queue) implements it on stock systems.
- **Region matching is listen-only**: physical touch is always 1:1 forwarded; region_match never injects (no tap-backfill by design).
- **Console injection on AVD**: emulator console `event mouse` down events carry no coordinates (ABS_X/Y type-A fallback added); move-under-down does not update position — physical `move` events must be validated on real hardware.

## Debugging

```sh
# vtouchd log (unbuffered)
adb shell tail -f /data/local/tmp/vtouch-runtime/vtouchd.log
# region config + hit events are logged there (region add / ev <id> <type>)
```
