#!/bin/sh
# deploy.sh —— 部署/起停最小版 vtouchd（需 adb + 设备 root）。
#   sh scripts/deploy.sh deploy   # 推二进制到 /data/local/tmp/vtouchd 并回读 md5 对账
#   sh scripts/deploy.sh start    # 起（已在跑会先停掉旧的）
#   sh scripts/deploy.sh stop     # 停（进程退出即释放 EVIOCGRAB，物理触摸回系统）
#   sh scripts/deploy.sh status   # pid + 日志尾部
# 设备侧起停走一个 /sdcard 上的 sh 脚本（adb-su 里直接 nohup & 容易被 su 会话收走子进程）。
set -eu
cd "$(dirname "$0")/.."
BIN=build/vtouchd
DST=/data/local/tmp/vtouchd
LOG=/data/local/tmp/vtouchd.log
STAGE=/sdcard/vtouchd.new

need_adb() { command -v adb >/dev/null 2>&1 || { echo "缺 adb"; exit 1; }; }
dev_pid() { adb shell "su -c 'pidof vtouchd'" 2>/dev/null | tr -d '\r\n'; }

case "${1:-deploy}" in
  deploy)
    need_adb
    [ -f "$BIN" ] || { echo "缺 $BIN —— 先 sh scripts/build.sh"; exit 1; }
    local_md5=$(md5sum "$BIN" | awk '{print $1}')
    echo "推 $BIN → $STAGE"
    adb push "$BIN" "$STAGE"
    # 先停进程再覆盖：运行中的 ELF 被覆盖会 Text file busy
    PID=$(dev_pid); [ -n "$PID" ] && adb shell "su -c 'kill -9 $PID'"
    adb shell "su -c 'rm -f $DST'"
    adb shell "su -c 'cp $STAGE $DST'"
    adb shell "su -c 'chmod 755 $DST'"
    remote_md5=$(adb shell "su -c 'md5sum $DST'" | awk '{print $1}' | tr -d '\r')
    echo "本机 md5 = $local_md5"
    echo "设备 md5 = $remote_md5"
    [ "$local_md5" = "$remote_md5" ] || { echo "对账不一致：设备上不是这一版"; exit 1; }
    echo "部署完成且已回读对账"
    ;;
  start)
    need_adb
    # 设备侧起停脚本：归一化成竖屏尺寸（daemon 只认竖屏逻辑坐标）
    cat > build/vt_start.sh <<'EOF'
#!/system/bin/sh
D=/data/local/tmp/vtouchd
S=$(wm size 2>/dev/null); S=${S##*Physical size: }
W=${S%%x*}; H=${S##*x}
if [ "$W" -gt "$H" ]; then T=$W; W=$H; H=$T; fi
kill -9 $(pidof vtouchd) 2>/dev/null
cp /dev/null /data/local/tmp/vtouchd.log
nohup $D -w $W -h $H -p 27183 >>/data/local/tmp/vtouchd.log 2>&1 </dev/null &
echo $! > /data/local/tmp/vtouchd.pid
sleep 0.3
pidof vtouchd
EOF
    adb push build/vt_start.sh /sdcard/vt_start.sh >/dev/null
    echo "启动中（尺寸由设备自报）…"
    adb shell "su -c 'sh /sdcard/vt_start.sh'" | tr -d '\r'
    sleep 0.5
    PID=$(dev_pid)
    if [ -n "$PID" ]; then
      echo "vtouchd 已运行 pid=$PID"
    else
      echo "没起来，日志尾部："
      adb shell "su -c 'tail -n 20 $LOG'" | tr -d '\r'
      exit 1
    fi
    ;;
  stop)
    need_adb
    PID=$(dev_pid)
    if [ -z "$PID" ]; then echo "vtouchd 本来就没在跑"; exit 0; fi
    adb shell "su -c 'kill -9 $PID'"
    sleep 0.3
    PID2=$(dev_pid)
    if [ -n "$PID2" ]; then echo "还在跑 pid=$PID2"; exit 1; fi
    echo "已停止（EVIOCGRAB 随进程退出释放，物理触摸回系统）"
    ;;
  status)
    need_adb
    PID=$(dev_pid)
    echo "pid: ${PID:-（未运行）}"
    adb shell "su -c 'tail -n 12 $LOG'" | tr -d '\r'
    ;;
  *)
    echo "用法: sh scripts/deploy.sh {deploy|start|stop|status}"
    exit 1
    ;;
esac
