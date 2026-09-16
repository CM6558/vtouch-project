#!/bin/sh
# ui-deploy.sh —— 一条命令把「带 UI 的核心 + 面板」从源码跑到设备上跑起来。
#
# 用法（主机侧）:
#   sh scripts/ui-deploy.sh all       # build → deploy → start → status（默认）
#   sh scripts/ui-deploy.sh build     # 只编：UI 核心 + 面板（real 模式）
#   sh scripts/ui-deploy.sh deploy    # 只推：4 个产物 + md5 对账
#   sh scripts/ui-deploy.sh start     # 只起（设备侧会先停干净）
#   sh scripts/ui-deploy.sh stop      # 停（核心会把面板一起停掉、释放 EVIOCGRAB）
#   sh scripts/ui-deploy.sh status    # 只看现状（核心/面板 pid、面板 fd 卫生、日志尾）
#
# 交付物：**设备上只需要一个文件** build/vtouchd_ui —— 面板三件套（classes.dex /
# libtestimgui.so / libc++_shared.so）已内嵌进核心，核心启动时自解包到 /data/local/tmp/vtouch-ui/。
# 因此 push 的只有核心 + 设备侧起停脚本；面板文件的新鲜度由"同一个二进制"保证。
set -eu
cd "$(dirname "$0")/.."

UI_DIR=/data/local/tmp/vtouch-ui
CORE=/data/local/tmp/vtouchd_ui
STAGE=/sdcard/vtouch-ui-stage

need_adb() { command -v adb >/dev/null 2>&1 || { echo "缺 adb"; exit 1; }; }

# 构建失败要**大声报**：以前把输出吞进 /dev/null，编不过就静悄悄中止（自己踩过）
run_build() {
    log=build/_ui_build.log
    mkdir -p build
    if ! "$@" >"$log" 2>&1; then
        echo "构建失败：$*"
        grep -E 'error|Error|错误' "$log" | head -20
        echo "（完整日志：$log）"
        exit 1
    fi
}

do_build() {
    echo "[1/4] 编面板（real 模式：接核心）..."
    run_build sh -c 'VTOUCH_UI_CORE=real sh scripts/build_ui.sh'
    for f in classes.dex libtestimgui.so libc++_shared.so; do
        echo "      build/ui/$f$(printf '%*s' $((24 - ${#f})) '')$(md5sum build/ui/$f | cut -d' ' -f1)"
    done
    echo "[2/4] 编带 UI 的核心（把面板三件套内嵌进去）..."
    run_build sh scripts/build.sh ui
    echo "      build/vtouchd_ui            $(md5sum build/vtouchd_ui | cut -d' ' -f1)  ($(wc -c < build/vtouchd_ui) 字节)"
}

dev_md5() { adb shell "su -c 'md5sum $1'" 2>/dev/null | awk '{print $1}' | tr -d '\r'; }

do_deploy() {
    [ -f build/vtouchd_ui ] || { echo "缺 build/vtouchd_ui —— 先 sh scripts/ui-deploy.sh build"; exit 1; }
    echo "[3/4] 推**一个**文件（面板三件套在核心里面）"
    adb shell "mkdir -p $STAGE" >/dev/null
    adb push build/vtouchd_ui $STAGE/vtouchd_ui >/dev/null
    adb push scripts/ui_ondev.sh $STAGE/ui_ondev.sh >/dev/null
    # 正在跑的 ELF 被覆盖会 Text file busy：先停核（它会带走面板）
    adb shell "su -c 'kill -TERM \$(pidof vtouchd_ui) 2>/dev/null; sleep 1; kill -9 \$(pidof vtouchd_ui) 2>/dev/null; mv -f $STAGE/vtouchd_ui $CORE; mv -f $STAGE/ui_ondev.sh /data/local/tmp/ui_ondev.sh; chmod 755 $CORE /data/local/tmp/ui_ondev.sh; rm -rf $STAGE'" >/dev/null
    l=$(md5sum build/vtouchd_ui | cut -d' ' -f1); r=$(dev_md5 "$CORE")
    [ "$l" = "$r" ] && echo "        ✓ vtouchd_ui  $r" || { echo "        ✗ vtouchd_ui 本机=$l 设备=$r"; exit 1; }
}

# 起完之后核对"核心自解包出来的面板"就是这一版构建（新单文件交付的强断言）
do_verify_extracted() {
    echo "[5/5] 核对核心自解包的面板 = 本机构建"
    for f in classes.dex libtestimgui.so libc++_shared.so; do
        l=$(md5sum build/ui/$f | cut -d' ' -f1); r=$(dev_md5 "$UI_DIR/$f")
        [ "$l" = "$r" ] && echo "        ✓ $f  $r" || { echo "        ✗ $f 本机=$l 设备=$r"; exit 1; }
    done
}

ondev() { adb shell "su -c 'sh /data/local/tmp/ui_ondev.sh 1440 3168 $1'" 2>/dev/null; }

case "${1:-all}" in
  build)  need_adb; do_build ;;
  deploy) need_adb; do_deploy ;;
  start)  need_adb; echo "[4/4] 起（核心会自己拉起面板）"; ondev start ;;
  stop)   need_adb; ondev stop ;;
  status) need_adb; ondev status ;;
  all)    need_adb; do_build; do_deploy; echo "[4/4] 起（核心会自己拉起面板）"; ondev start; do_verify_extracted ;;
  *)      echo "用法: sh scripts/ui-deploy.sh [all|build|deploy|start|stop|status]"; exit 1 ;;
esac
