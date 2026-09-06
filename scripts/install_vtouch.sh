#!/system/bin/sh
# vtouch 手机端一键安装/更新/启动脚本
# 手机端脚本不需要参数；尺寸由 wm size 自动读取。
# 脚本可从任意当前目录执行，所有文件路径都相对脚本自身定位。

set -u
SCRIPT_DIR="$(CDPATH= cd -- "$(dirname "$0")" 2>/dev/null && pwd)"
ROOT_DIR="$(CDPATH= cd -- "$SCRIPT_DIR/.." 2>/dev/null && pwd)"
WSDIR="$SCRIPT_DIR"
STAGE="$ROOT_DIR/build"
[ -d "$STAGE" ] || STAGE="$SCRIPT_DIR/build"

W="${1:-}"
H="${2:-}"
if [ -z "$W" ] || [ -z "$H" ]; then
    SIZE="$(wm size 2>/dev/null | sed -n 's/.*Physical size: //p' | head -n 1)"
    W="${SIZE%x*}"
    H="${SIZE#*x}"
fi
[ -n "$W" ] && [ -n "$H" ] || { echo "无法读取显示尺寸，请检查 wm size" >&2; exit 1; }

BASE=/data/local/tmp
D="$BASE/vtouchd"
C="$BASE/vtouchctl"
WS="$BASE/vtouchws"
SOCK="$BASE/vtouch.sock"
DLOG="$BASE/vtouchd.log"
WLOG="$BASE/vtouchws.log"

fail() { echo "[ERROR] $*" >&2; exit 1; }
count_proc() { ps -A -o NAME 2>/dev/null | grep -w "$1" | wc -l | tr -d ' '; }

[ "$(id -u)" = "0" ] || fail "需要 root"
case "$W" in ''|*[!0-9]*) fail "宽度非法";; esac
case "$H" in ''|*[!0-9]*) fail "高度非法";; esac
[ "$W" -ge 100 ] && [ "$W" -le 10000 ] || fail "宽度范围非法"
[ "$H" -ge 100 ] && [ "$H" -le 10000 ] || fail "高度范围非法"

# 安装包解压后，把 build 内最新可执行文件复制到运行目录。
# 不依赖旧的运行文件。
if [ ! -f "$STAGE/vtouchd" ] || [ ! -f "$STAGE/vtouchctl" ] || [ ! -f "$STAGE/vtouchws" ]; then
    fail "安装包 build 目录缺少文件: $STAGE"
fi

# 先停止旧实例，再替换正在运行的文件。
killall vtouchws >/dev/null 2>&1 || true
killall vtouchd >/dev/null 2>&1 || true
sleep 1
[ "$(count_proc vtouchd)" = "0" ] || fail "旧 vtouchd 未退出"
[ "$(count_proc vtouchws)" = "0" ] || fail "旧 vtouchws 未退出"
rm -f "$D" "$C" "$WS" "$SOCK" "$DLOG" "$WLOG"
cp -f "$STAGE/vtouchd" "$D"
cp -f "$STAGE/vtouchctl" "$C"
cp -f "$STAGE/vtouchws" "$WS"
chmod 755 "$D" "$C" "$WS"
[ -x "$D" ] || fail "找不到最新 vtouchd"
[ -x "$C" ] || fail "找不到最新 vtouchctl"
[ -x "$WS" ] || fail "找不到最新 vtouchws"

nohup "$D" -x "$W" -y "$H" >"$DLOG" 2>&1 </dev/null &
sleep 2
"$C" ping >/dev/null 2>&1 || { tail -30 "$DLOG" 2>/dev/null; fail "vtouchd 启动失败"; }
[ "$(count_proc vtouchd)" = "1" ] || fail "vtouchd 实例数量不是 1"

nohup "$WS" >"$WLOG" 2>&1 </dev/null &
sleep 1
[ "$(count_proc vtouchws)" = "1" ] || fail "vtouchws 实例数量不是 1"

settings put system show_touches 0 >/dev/null 2>&1 || true
settings put system pointer_location 0 >/dev/null 2>&1 || true

echo "[OK] 最新程序已安装并启动"
echo "vtouchd=$(count_proc vtouchd)"
echo "vtouchws=$(count_proc vtouchws)"
echo "resolution=${W}x${H}"
echo "websocket=ws://127.0.0.1:27183"
