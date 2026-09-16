#!/system/bin/sh
# ondev-ui-smoke.sh —— P1 真机冒烟：VT_UI 核心拉起面板，验「fd 卫生」这条硬门 + 生命周期。
#
# 判据（都要在这里打出来，别靠人记）：
#   1) 核心日志出现「共享内存就绪」与「面板已启动 pid=…」；
#   2) 面板子进程 fd/3 指向 /memfd:vtouch-shm（共享内存传到了）；
#   3) 面板子进程**没有任何 fd 指向 /dev/input/event* 或 /dev/uinput**
#      —— 硬性检查项：否则面板继承了带 EVIOCGRAB 的 input_fd，核心退出后 grab 不释放、物理触摸回不来。
#      注意别用 'grep event' 这种松判据：ART 自己的 anon_inode:[eventfd]/[eventpoll] 会被误报。
#   4) 面板子进程的父进程 = 核心 pid（说明是核心拉起的）。
#
# 用法（设备侧，su）：
#   sh ondev-ui-smoke.sh            起核心并检查
#   sh ondev-ui-smoke.sh stop       停核心，检查面板是否被一起停掉、event8 是否回到系统手里
#   sh ondev-ui-smoke.sh noui       面板目录不可用时，核心应按「无 UI 模式」继续跑
W=1440; H=3168
BIN=/data/local/tmp/vtouchd_ui
LOG=/data/local/tmp/vt_ui_core_smoke.log
MODE="$1"

hold_event8() { ls -l /proc/*/fd 2>/dev/null | grep -c '/dev/input/event8'; }

if [ "$MODE" = "stop" ]; then
    CP=$(pidof vtouchd_ui)
    echo "停核心 pid=$CP（SIGTERM）"
    kill -TERM $CP 2>/dev/null
    sleep 2
    echo "核心还在否: [$(pidof vtouchd_ui)]   （应为空）"
    echo "面板还在否: [$(ps -A 2>/dev/null | grep VTouchUI | grep -v grep)]   （应为空）"
    echo "持有 /dev/input/event8 的 fd 数: $(hold_event8)   （应回到系统自身的数量，本机为 3）"
    echo "==== 收尾日志 ===="
    tail -6 $LOG 2>/dev/null
    exit 0
fi

if [ "$MODE" = "noui" ]; then
    killall vtouchd_ui 2>/dev/null; sleep 1
    export VTOUCH_UI_DIR=/data/local/tmp/definitely-missing
    cd /data/local/tmp || exit 1
    nohup $BIN -w $W -h $H >$LOG 2>&1 </dev/null &
    sleep 2
    echo "==== 日志（应出现「面板未就绪 → 以无 UI 模式继续」，且引擎正常起来）===="
    cat $LOG
    echo "核心在否: [$(pidof vtouchd_ui)]  （应在）"
    exit 0
fi

killall vtouchd_ui vtouchd 2>/dev/null
sleep 1
cd /data/local/tmp || exit 1
nohup $BIN -w $W -h $H >$LOG 2>&1 </dev/null &
sleep 2

echo "==== 核心日志 ===="
cat $LOG 2>/dev/null
CPID=$(grep -o 'pid=[0-9]*' $LOG 2>/dev/null | tail -1 | cut -d= -f2)
echo "==== 核心 pid=$(pidof vtouchd_ui) / 面板子进程 pid=$CPID ===="
if [ -z "$CPID" ]; then echo "结论：面板未启动（看上面日志原因）"; exit 0; fi

echo "面板父进程 pid=$(cat /proc/$CPID/stat 2>/dev/null | awk '{print $4}')（应等于核心 pid）"
echo "fd 3 -> $(readlink /proc/$CPID/fd/3 2>/dev/null)"

echo "--- 指向 /dev/input/event* 或 /dev/uinput 的 fd（应无输出）---"
ls -l /proc/$CPID/fd 2>/dev/null | grep -E '/dev/input/event|/dev/uinput'
N_INPUT=$(ls -l /proc/$CPID/fd 2>/dev/null | grep -cE '/dev/input/event|/dev/uinput')
N_MEMFD=$(ls -l /proc/$CPID/fd 2>/dev/null | grep -c 'memfd:vtouch-shm')
echo "==== 判据 ===="
echo "  继承/持有 触摸设备 fd 条数 = $N_INPUT   （必须 0）"
echo "  共享内存 fd 条数           = $N_MEMFD   （必须 ≥1）"
if [ "$N_INPUT" = "0" ] && [ "$N_MEMFD" != "0" ]; then echo "  ✓ fd 卫生合格"; else echo "  ✗ fd 卫生不合格"; fi
