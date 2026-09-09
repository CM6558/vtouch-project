# Userspace merged touchscreen backend (vtouchd, single process)

No KernelSU required. No shell scripts: the APK is self-contained —
install it, run any script, and `new VTouch()` auto-extracts the binary,
starts the backend and connects. Lifecycle and health live in
`VTouchPlugin` (startBackend/stopBackend/isServiceReady/getBackendStatus).

## Files on the phone

```text
/data/local/tmp/vtouchd                  # single binary (APK assets → root cp)
/data/local/tmp/vtouch-runtime/vtouchd.pid
/data/local/tmp/vtouch-runtime/vtouchd.log
```

## Start

```js
var vt = new VTouch();   // auto-extract (first run) + start + connect
vt.ready();
```

## Stop completely

```js
vt.close(); vt.stopService();
```

It kills the daemon, removes pid files; the dead process closes fds and
releases `EVIOCGRAB`. Status check: `vt.status()` (APK channel).

The worker owns the physical input fd, so stopping it closes the fd and releases `EVIOCGRAB`.

## WebSocket

```text
ws://127.0.0.1:27183
```

The bridge connects to:

```text
/data/local/tmp/vtouch-runtime/merge.sock
```

No KernelSU module or legacy `vtouchd`/`vtouchctl` is required.

`vtouchmerge` requires logical display dimensions via `-w WIDTH -h HEIGHT`; start scripts read them dynamically from `wm size` and fail if unavailable. It dynamically selects the first `/dev/input/event*` device described by `/proc/bus/input/devices` whose ioctl capabilities include EV_ABS, ABS_MT_SLOT, ABS_MT_TRACKING_ID, ABS_MT_POSITION_X/Y, and INPUT_PROP_DIRECT. It opens the source nonblocking, creates `vtouch-merged` on `/dev/uinput` using discovered raw ranges and `physical slots + virtual slots` (virtual slots default 10, bounded to 32), verifies UI_DEV_CREATE/UI_GET_SYSNAME, then grabs the physical source. Physical events remain raw; only virtual logical coordinates are converted with rounded, clamped integer mapping.

`vtouchd` 单进程直接对外提供 `ws://127.0.0.1:27183`，内部直调合并状态，无 UDS 跳转。坏客户端只关闭该连接并复位虚拟触点，不丢 grab；进程整体崩溃才丢触摸，由启动脚本重拉恢复。

Socket default: `/data/local/tmp/vtouch-merge.sock`, mode 0660. Text protocol is line-oriented and intentionally non-JSON:

```text
ping                 -> pong
res                  -> res LOGICAL_WIDTH LOGICAL_HEIGHT raw XMIN XMAX YMIN YMAX
begin_frame          -> ok
point SLOT down|move|up X Y -> ok (X/Y are logical display coordinates)
end_frame            -> ok
reset                -> ok
```

Virtual slots are numbered 0..V-1 and map after the physical slots. Every completed frame emits exactly one SYN_REPORT. A disconnected client resets its virtual contacts.

## Build (Android NDK r27d)

```sh
NDK=C:/Users/21102/android-ndk-r27d
CC=$NDK/toolchains/llvm/prebuilt/windows-x86_64/bin/aarch64-linux-android24-clang.cmd
"$CC" -O2 -Wall -Wextra -Werror -D_GNU_SOURCE src/vtouchd.c -o build/vtouchd
"$CC" -O2 -Wall -Wextra -Werror -D_GNU_SOURCE -DVT_MERGE_TEST -fsyntax-only src/vtouchmerge.c
```

Or use `src/Android.mk` with `ndk-build`.
