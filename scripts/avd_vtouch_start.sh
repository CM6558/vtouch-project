#!/bin/bash
# 模拟器免 su 直启 vtouchd（AVD google_apis 镜像内置 adb root，无需 Magisk/su 守护）。
# 用法: bash scripts/avd_vtouch_start.sh [vtouchd二进制] [分辨率W H]
#   二进制默认 build/vtouchd-x86_64（本地编译）或从 Actions artifact 取 vtouchd
#   分辨率默认 1344 2992（Pixel30Root 竖屏）
# 说明: 起好后 AutoJs6 跑 bundle 时 vtouchEnsure 的 TCP 探测会直接命中,
#       完全不经过 su 通道（bundle 免 su 直启分支）。
set -e

export ANDROID_ADB_SERVER_PORT=5039
ADB="${ANDROID_SDK:-D:/ANDROID/SDK}/platform-tools/adb.exe"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${1:-$ROOT/build/vtouchd-x86_64}"
W="${2:-1344}"
H="${3:-2992}"

echo "[1/4] adb root ..."
"$ADB" start-server >/dev/null 2>&1 || true
sleep 3
"$ADB" -s emulator-5554 root >/dev/null 2>&1 || true
sleep 4

echo "[2/4] push vtouchd -> /data/local/tmp/vtouchd"
"$ADB" -s emulator-5554 push "$BIN" /data/local/tmp/vtouchd
"$ADB" -s emulator-5554 shell 'chmod 755 /data/local/tmp/vtouchd'

echo "[3/4] 启动 (pid -> /data/local/tmp/vtouch-runtime/vtouchd.pid)"
"$ADB" -s emulator-5554 shell 'mkdir -p /data/local/tmp/vtouch-runtime; killall vtouchd 2>/dev/null || true; rm -f /data/local/tmp/vtouch-runtime/vtouchd.pid /data/local/tmp/vtouch-runtime/vtouchd.log; nohup /data/local/tmp/vtouchd -w '"$W"' -h '"$H"' -p 27183 >/data/local/tmp/vtouch-runtime/vtouchd.log 2>&1 </dev/null & echo $! > /data/local/tmp/vtouch-runtime/vtouchd.pid'
sleep 2

echo "[4/4] 验证"
"$ADB" -s emulator-5554 shell 'head -1 /data/local/tmp/vtouch-runtime/vtouchd.log; cat /data/local/tmp/vtouch-runtime/vtouchd.pid'
echo "vtouchd ready (免 su, 模拟器专用)"
