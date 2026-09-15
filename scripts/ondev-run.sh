#!/system/bin/sh
# ondev-run.sh —— 把 /sdcard/vtouchd 部署到设备并起 daemon（真机调试用）。
#
# 为什么要经过 /sdcard 中转：/data/local/tmp 在 SELinux Enforcing 下对 shell 不可写，
# 而 adb push 只能以 shell 身份写；所以 push 到 /sdcard，再由 root（su）拷进 /data/local/tmp。
# 另外别用 `touch` 先建文件再重定向写入 —— 本机上那样建出来的文件是坏的（stat 全是 ?），
# 用 dd/cp 直接写。
#
# 用法:
#   adb push build/vtouchd /sdcard/vtouchd
#   adb push scripts/ondev-run.sh /sdcard/
#   adb shell "su -c 'sh /sdcard/ondev-run.sh 1800'"      # 跑 1800 秒后自杀
set -u
RUN="${1:-900}"                       # daemon 存活秒数（必须有限：它抓着物理触摸）
W="${2:-1440}"; H="${3:-3168}"        # 逻辑尺寸（竖屏宽/高）
PORT="${4:-27183}"
BIN=/data/local/tmp/vtouchd
LOG=/data/local/tmp/vt.log

[ -f /sdcard/vtouchd ] || { echo "缺 /sdcard/vtouchd（先 adb push build/vtouchd /sdcard/）"; exit 1; }
pkill -x vtouchd 2>/dev/null
sleep 1
dd if=/sdcard/vtouchd of="$BIN" bs=8192 >/dev/null 2>&1 || { echo "拷进 /data/local/tmp 失败"; exit 1; }
chmod 755 "$BIN"
echo "落盘 $BIN  大小=$(wc -c < $BIN)  sha256=$(sha256sum $BIN | cut -c1-16)…"
(timeout "$RUN" "$BIN" -w "$W" -h "$H" -p "$PORT" > "$LOG" 2>&1 &)
sleep 3
echo "--- daemon 日志 ---"; cat "$LOG"
echo "--- 进程 ---";        ps -A | grep -v grep | grep vtouchd
echo "--- 端口 ---";        grep -c ":6A2F" /proc/net/tcp
