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
W=$1; H=$2; ACT=${3:-status}     # 给 "-" 或省略 = 不传尺寸，核心自己探测
BIN=/data/local/tmp/vtouchd_ui
LOG=/data/local/tmp/vt_ui_core.log

panel_pid() { ps -A 2>/dev/null | grep 'vtouch-ui' | grep -v grep | awk '{print $2}' | head -1; }
core_pid()  { pidof vtouchd_ui; }

# 动态认设备：**绝不写死 /dev/input/eventN**（换机器/换口就变，AGENTS 里明令禁止）。
# 节点名从「核心自己抓的那个 fd」现读；grab 释放判定用「停前 vs 停后」的持有者条数对比。
core_touch_nodes() {
    P=$(core_pid); [ -n "$P" ] || return 0
    ls -l /proc/$P/fd 2>/dev/null \
        | sed -n 's/.*-> \(\/dev\/input\/event[0-9]*\|\/dev\/uinput\)$/\1/p' | sort -u
}
count_holders() {   # $1 = 节点列表（空格分隔）；数全系统持有这些节点的 fd 条数
    [ -z "$1" ] && { echo 0; return; }
    T=0
    for n in $1; do
        C=$(ls -l /proc/*/fd 2>/dev/null | grep -c "$n")
        T=$((T + C))
    done
    echo $T
}

do_stop() {
    NODES=$(core_touch_nodes)                 # 停之前先记下（核心一死它的 fd 表就没了）
    N0=$(count_holders "$NODES")
    P=$(core_pid)
    [ -n "$P" ] && kill -TERM $P
    i=0
    while [ $i -lt 15 ] && [ -n "$(core_pid)" ]; do sleep 0.2; i=$((i+1)); done
    P=$(core_pid)
    [ -n "$P" ] && { echo "核心不响应 SIGTERM → SIGKILL"; kill -9 $P; sleep 1; }
    [ -n "$NODES" ] && echo "停前：核心抓着 [$NODES]，全系统持有 fd 数 = $N0"
    echo "停后：核心=[$(core_pid)] 面板=[$(panel_pid)]   （都应为空）"
    if [ -z "$NODES" ]; then
        echo "grab 判定：停之前核心没在跑（现读不到它抓过哪个节点）⇒ 无需释放"
    else
        echo "grab 判定：[$NODES] 现在持有 fd 数 = $(count_holders "$NODES")   （比停前少掉核心自己的那几条 ⇒ grab 已释放；system_server 自己那份会留着）"
    fi
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
    echo "--- 面板目录（B 方案：核心启动时自解包出来的）---"
    ls -l /data/local/tmp/vtouch-ui/ 2>/dev/null | grep -E 'classes.dex|lib'
}

do_start() {
    do_stop >/dev/null 2>&1
    cd /data/local/tmp || exit 1
    case "$W" in ""|-|auto) ARGS=""; echo "（不传逻辑尺寸 → 核心启动时自己探测）";;
                    *) ARGS="-w $W -h $H"; echo "（显式指定逻辑尺寸 $W x $H）";; esac
    nohup $BIN $ARGS >$LOG 2>&1 </dev/null &
    sleep 3
    echo "--- 核心日志 ---"; cat $LOG
    echo
    do_status
}

case "$ACT" in
  start)  do_start ;;
  stop)   do_stop ;;
  status) do_status ;;
  *)      # 兜底**绝不启动**：动作拼错时启动核心会悄没声抓走触摸屏（自己踩过）
          echo "用法: sh ui_ondev.sh <W> <H> start|stop|status   （W/H 给 - 或省略 = 核心自己探测）"
          exit 2 ;;
esac
