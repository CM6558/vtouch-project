#!/system/bin/sh
# vtouch.sh —— 设备侧入口（自包含：内嵌核心二进制；面板三件套由核心启动时自解包）。
#
# 用法（都要 root）：
#   su -c 'sh /sdcard/vtouch.sh start'     # 装 + 起（幂等；已在跑则只报状态）
#   su -c 'sh /sdcard/vtouch.sh stop'      # 停：SIGTERM → 核心自己收尾（先停面板、再放 EVIOCGRAB）
#   su -c 'sh /sdcard/vtouch.sh status'    # 核心/面板 pid + 端口 + 二进制 md5 + 日志尾
#   su -c 'sh /sdcard/vtouch.sh install'   # 只装不启
#
# 生成：python scripts/pack_su.py（把 build/vtouchd_ui 内嵌进本模板 → build/vtouch.sh）
set -u

TARGET=/data/local/tmp/vtouchd_ui
RUNDIR=/data/local/tmp
LOG=$RUNDIR/vt_ui_core.log

PAYLOAD_MD5="<<PAYLOAD_MD5>>"
PAYLOAD_SIZE=<<PAYLOAD_SIZE>>
MARK=__VTOUCH_PAYLOAD_BELOW__

say() { echo "[vtouch] $*"; }
die() { echo "[vtouch] $*" >&2; exit 1; }

[ "$(id -u)" = "0" ] || die "需要 root：su -c 'sh $0 ${1:-start}'"

md5of() { md5sum "$1" 2>/dev/null | awk '{print $1}'; }

decode_payload() {   # $1 = 输出路径
    awk -v m="$MARK" 'x { print } $0 == m { x = 1 }' "$0" | base64 -d > "$1" 2>/dev/null \
        || die "载荷解码失败（toybox base64 可用？）"
}

do_install() {
    want="$PAYLOAD_MD5"
    have=""
    [ -f "$TARGET" ] && have=$(md5of "$TARGET")
    if [ "$have" = "$want" ]; then
        say "已是最新（md5 $want）"
        return 0
    fi
    say "安装核心（$PAYLOAD_SIZE 字节）→ $TARGET"
    tmp="$TARGET.tmp.$$"
    decode_payload "$tmp"
    sz=$(wc -c < "$tmp" | tr -d ' ')
    [ "$sz" = "$PAYLOAD_SIZE" ] || { rm -f "$tmp"; die "长度不符：$sz != $PAYLOAD_SIZE"; }
    got=$(md5of "$tmp")
    [ "$got" = "$want" ] || { rm -f "$tmp"; die "md5 不符：$got != $want"; }
    chmod 755 "$tmp" || { rm -f "$tmp"; die "chmod 失败"; }
    mv -f "$tmp" "$TARGET" || die "替换失败"
    got=$(md5of "$TARGET")
    [ "$got" = "$want" ] || die "回读 md5 不符：$got"
    say "安装完成（回读 md5 $got）"
}

do_start() {
    do_install || exit 1
    cur=$(pidof vtouchd_ui 2>/dev/null) && {
        say "核心已在跑 pid=$cur；面板 pid=$(pidof vtouch-ui 2>/dev/null || echo '(看门狗拉起中)')"
        return 0
    }
    : > "$LOG"
    cd "$RUNDIR" || die "进不去 $RUNDIR"
    nohup ./vtouchd_ui >>"$LOG" 2>&1 </dev/null &
    i=0
    while [ $i -lt 8 ]; do
        sleep 1
        if pidof vtouchd_ui >/dev/null 2>&1 && grep -q "engine=on" "$LOG" 2>/dev/null; then break; fi
        pidof vtouchd_ui >/dev/null 2>&1 || die "进程退出了，看 $LOG"
        i=$((i + 1))
    done
    cur=$(pidof vtouchd_ui 2>/dev/null) || die "启动超时（看 $LOG）"
    sleep 1
    say "核心 pid=$cur  面板 pid=$(pidof vtouch-ui 2>/dev/null || echo '(看门狗拉起中)')"
    tail -n 3 "$LOG"
}

do_stop() {
    cur=$(pidof vtouchd_ui 2>/dev/null) || { say "没在跑"; return 0; }
    say "停核心 pid=$cur（SIGTERM：先停面板、再放 EVIOCGRAB）"
    kill -TERM $cur 2>/dev/null
    i=0
    while [ $i -lt 10 ] && pidof vtouchd_ui >/dev/null 2>&1; do sleep 1; i=$((i + 1)); done
    if pidof vtouchd_ui >/dev/null 2>&1; then
        say "SIGTERM 10s 未退 → SIGKILL（grab 随进程退出释放）"
        kill -9 $(pidof vtouchd_ui) 2>/dev/null
        sleep 1
    fi
    pidof vtouchd_ui >/dev/null 2>&1 && die "没停掉？pid=$(pidof vtouchd_ui)"
    say "已停；面板 pid=$(pidof vtouch-ui 2>/dev/null || echo '-')  触摸已回系统"
}

do_status() {
    say "版本：$PAYLOAD_SIZE 字节 md5 $PAYLOAD_MD5"
    if [ -f "$TARGET" ]; then say "已安装 md5 $(md5of "$TARGET")"; else say "未安装"; fi
    say "核心 pid=$(pidof vtouchd_ui 2>/dev/null || echo '-')  面板 pid=$(pidof vtouch-ui 2>/dev/null || echo '-')"
    if grep -qi "6a2f" /proc/net/tcp 2>/dev/null; then say "端口 27183：在听/有连接"; else say "端口 27183：无"; fi
    say "日志尾："; tail -n 15 "$LOG" 2>/dev/null || say "(无日志)"
}

case "${1:-}" in
    install) do_install ;;
    start)   do_start ;;
    stop)    do_stop ;;
    status)  do_status ;;
    *) echo "用法: sh $0 install|start|stop|status"; exit 2 ;;
esac

exit 0

# ---- 以下为内嵌载荷（pack_su.py 填充；勿手改）----
__VTOUCH_PAYLOAD_BELOW__
<<PAYLOAD>>
