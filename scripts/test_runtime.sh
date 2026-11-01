#!/bin/sh
# Pure runtime checks; never opens /dev/input, starts the core, or changes live configuration.
set -eu
cd "$(dirname "$0")/.."
CC=${CC:-clang}
TMPBASE=${TMPDIR:-/data/data/com.termux/files/usr/tmp/opencode}
if [ ! -d "$TMPBASE" ]; then TMPBASE=/tmp; fi
B=$(mktemp -d "$TMPBASE/vtouch-check.XXXXXX")
echo "Runtime test binaries: $B"
CFLAGS="-O2 -Wall -Wextra -Werror -D_GNU_SOURCE -pthread -Isrc"
# shellcheck disable=SC2086
"$CC" $CFLAGS -DVT_UI tests/runtime_check.c src/vt_vm.c src/vt_vision.c -lm -o "$B/runtime"
"$B/runtime"
# shellcheck disable=SC2086
"$CC" $CFLAGS -DVT_UI -DVT_VIS_FORCE_SCALAR tests/runtime_check.c src/vt_vm.c src/vt_vision.c -lm -o "$B/runtime-scalar"
"$B/runtime-scalar"
# shellcheck disable=SC2086
"$CC" $CFLAGS -ffunction-sections -fdata-sections -Wl,--gc-sections tests/queue_ws_check.c src/vt_ws.c src/vt_queue.c src/vt_util.c -o "$B/queue-ws"
"$B/queue-ws"
# shellcheck disable=SC2086
"$CC" $CFLAGS -ffunction-sections -fdata-sections -Wl,--gc-sections tests/poll_check.c src/vt_ws.c src/vt_queue.c src/vt_util.c src/vt_region.c src/vt_frame.c -lm -o "$B/poll"
"$B/poll"
# shellcheck disable=SC2086
"$CC" $CFLAGS -DVT_UI -ffunction-sections -fdata-sections -Wl,--gc-sections tests/vision_async_check.c src/vt_vision.c -lm -o "$B/vision-async"
"$B/vision-async"
