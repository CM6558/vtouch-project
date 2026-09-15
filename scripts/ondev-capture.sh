#!/system/bin/sh
# ondev-capture.sh —— 在设备上抓「合并设备」的原始事件流（只读，不抓屏、不影响物理触摸）。
#
# 合并设备名 = 物理设备名 + "_vtouch"（setup_uinput 里定的），节点号每次起 daemon 可能变，
# 所以这里按名字找，不写死 eventN。
#
# 用法:
#   adb push scripts/ondev-capture.sh /sdcard/
#   adb shell "su -c 'sh /sdcard/ondev-capture.sh 300'"     # 抓 300 秒
#   adb shell "su -c 'cat /data/local/tmp/merged.log'" > merged.log
#   python tests/id_split_check.py merged.log
set -u
SEC="${1:-300}"
SUFFIX="${2:-_vtouch}"
NODE=""
for f in /sys/class/input/event*/device/name; do
  if [ "$(cat "$f" 2>/dev/null)" = "touchpanel${SUFFIX}" ]; then
    NODE=$(basename "$(dirname "$(dirname "$f")")")
    break
  fi
done
if [ -z "$NODE" ]; then echo "没找到合并设备（daemon 没在跑？）"; exit 1; fi
echo "合并设备节点: /dev/input/$NODE（抓 $SEC 秒）"
rm -f /data/local/tmp/merged.log
(timeout "$SEC" getevent -lt "/dev/input/$NODE" > /data/local/tmp/merged.log 2>&1 &)
sleep 1
echo "抓包已开始: $(wc -l < /data/local/tmp/merged.log) 行 → /data/local/tmp/merged.log"
