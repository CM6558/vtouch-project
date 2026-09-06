# Userspace merged touchscreen backend

This version does not require KernelSU. Start and stop it with `su -c sh` scripts.

## Files on the phone

```text
/data/local/tmp/vtouchmerge
/data/local/tmp/vtouchws
/data/local/tmp/vtouch-start.sh
/data/local/tmp/vtouch-stop.sh
/data/local/tmp/vtouch-runtime/merge.sock
```

## Start

```sh
su -c 'sh /data/local/tmp/vtouch-start.sh'
```

The script stops stale `vtouchmerge`/`vtouchws` processes, starts the merger, waits for its Unix socket, starts the WebSocket bridge, and prints:

```text
[OK] VTOUCH_READY=1
```

## Stop completely

```sh
su -c 'sh /data/local/tmp/vtouch-stop.sh'
```

It sends TERM, then KILL if required, removes the socket and pid files, and prints:

```text
[OK] VTOUCH_STOPPED=1
```

The worker owns the physical input fd, so stopping it closes the fd and releases `EVIOCGRAB`.

## WebSocket

```text
ws://127.0.0.1:27183
```

The bridge connects to:

```text
/data/local/tmp/vtouch-runtime/merge.sock
```

No old `vtouchd`, old `vtouch.sock`, or KernelSU module is required.

`vtouchmerge` requires logical display dimensions via `-w WIDTH -h HEIGHT`; start scripts read them dynamically from `wm size` and fail if unavailable. It dynamically selects the first `/dev/input/event*` device described by `/proc/bus/input/devices` whose ioctl capabilities include EV_ABS, ABS_MT_SLOT, ABS_MT_TRACKING_ID, ABS_MT_POSITION_X/Y, and INPUT_PROP_DIRECT. It opens the source nonblocking, creates `vtouch-merged` on `/dev/uinput` using discovered raw ranges and `physical slots + virtual slots` (virtual slots default 10, bounded to 32), verifies UI_DEV_CREATE/UI_GET_SYSNAME, then grabs the physical source. Physical events remain raw; only virtual logical coordinates are converted with rounded, clamped integer mapping.

`vtouchsupervise` forks the worker and receives heartbeat bytes over a pipe. The worker alone owns physical/uinput/socket descriptors; a crash therefore releases EVIOCGRAB during kernel fd close. The supervisor restarts with bounded exponential backoff.

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
"$CC" -O2 -Wall -Wextra -Werror -D_GNU_SOURCE src/vtouchmerge.c -o build/vtouchmerge
"$CC" -O2 -Wall -Wextra -Werror -D_GNU_SOURCE -DVT_MERGE_LIBRARY -c src/vtouchmerge.c -o build/vtouchmerge_lib.o
"$CC" -O2 -Wall -Wextra -Werror -D_GNU_SOURCE -c src/vtouchsupervise.c -o build/vtouchsupervise_main.o
"$CC" build/vtouchsupervise_main.o build/vtouchmerge_lib.o -o build/vtouchsupervise
"$CC" -O2 -Wall -Wextra -Werror -D_GNU_SOURCE -DVT_MERGE_TEST -fsyntax-only src/vtouchmerge.c
```

Or use `src/Android.mk` with `ndk-build` (the existing `vtouchd` is not changed). This backend is build-only here; it has not been deployed to a phone and EVIOCGRAB has not been called on a real device.
