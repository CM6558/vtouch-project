#!/system/bin/sh
# ui_ondev.sh —— 设备侧起停「带 UI 的核心」（由 scripts/ui-deploy.sh 推上来调用）。
#
# 为什么只管核心：**核心自己会 fork/exec 面板子进程**（以核心为准），所以设备侧不需要
# 单独起面板；停核心时核心会把面板一起停掉并释放 EVIOCGRAB。
#
# 用法（设备侧，su）：
#   sh ui_ondev.sh <W> <H> start    起（已在跑会先停干净）
#   sh ui_ondev.sh <W> <H> stop     停并验证 grab 已释放
#   sh ui_ondev.sh <W> <H> status   只看现状
W=${1:-1440}; H=${2:-3168}; ACT=${3:-status}
BIN=/data/local/tmp/vtouchd_ui
LOG=/data/local/tmp/vt_ui_core.log

panel_pid() { ps -A 2>/dev/null | grep 'vtouch-ui' | grep -v grep | awk '{print $2}' | head -1; }
core_pid()  { pidof vtouchd_ui; }
event8_holders() { ls -l /proc/*/fd 2>/dev/null | grep -c '/dev/input/event8'; }

do_stop() {
    P=$(core_pid)
    [ -n "$P" ] && kill -TERM $P
    i=0
    while [ $i -lt 15 ] && [ -n "$(core_pid)" ]; do sleep 0.2; i=$((i+1)); done
    P=$(core_pid)
    [ -n "$P" ] && { echo "核心不响应 SIGTERM → SIGKILL"; kill -9 $P; sleep 1; }
    echo "停后：核心=[$(core_pid)] 面板=[$(panel_pid)]   （都应为空）"
    echo "持有 /dev/input/event8 的 fd 数 = $(event8_holders)   （回到系统自身数量 = grab 已释放）"
}

do_status() {
    CP=$(core_pid); UP=$(panel_pid)
    echo "核心 pid=[$CP]  面板 pid=[$UP]"
    echo "--- 核心日志尾部 ---"; tail -4 $LOG 2>/dev/null
    if [ -n "$UP" ]; then
        echo "--- 面板 fd 卫生（只看关键三类）---"
        ls -l /proc/$UP/fd 2>/dev/null | grep -E 'memfd|/dev/input/event|/dev/uinput'
        echo "  指向触摸设备的 fd 数 = $(ls -l /proc/$UP/fd 2>/dev/null | grep -cE '/dev/input/event|/dev/uinput')   （必须 0）"
        echo "  共享内存 fd 数       = $(ls -l /proc/$UP/fd 2>/dev/null | grep -c 'memfd:vtouch-shm')   （必须 ≥1）"
    fi
}

do_start() {
    do_stop >/dev/null 2>&1
    cd /data/local/tmp || exit 1
    nohup $BIN -w $W -h $H >$LOG 2>&1 </dev/null &
    sleep 3
    echo "--- 核心日志 ---"; cat $LOG
    echo
    do_status
}

case "$ACT" in
  stop)   do_stop ;;
  status) do_status ;;
  *)      do_start ;;
esac
