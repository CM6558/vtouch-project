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
# 交付物共 4 个文件（这就是"单文件启动"方案 A 的全部输入）：
#   build/vtouchd_ui                带 UI 的核心（-DVT_UI：状态进共享内存 + 核心拉起面板）
#   build/ui/classes.dex            面板的 Java 壳
#   build/ui/libtestimgui.so        面板本体（ImGui）
#   build/ui/libc++_shared.so       面板动态依赖（缺它 System.load 直接失败）
set -eu
cd "$(dirname "$0")/.."

UI_DIR=/data/local/tmp/vtouch-ui
CORE=/data/local/tmp/vtouchd_ui
STAGE=/sdcard/vtouch-ui-stage

need_adb() { command -v adb >/dev/null 2>&1 || { echo "缺 adb"; exit 1; }; }

do_build() {
    echo "[1/4] 编带 UI 的核心 ..."
    sh scripts/build.sh ui >/dev/null
    echo "      build/vtouchd_ui            $(md5sum build/vtouchd_ui | cut -d' ' -f1)"
    echo "[2/4] 编面板（real 模式：接核心）..."
    VTOUCH_UI_CORE=real sh scripts/build_ui.sh >/dev/null 2>&1
    for f in classes.dex libtestimgui.so libc++_shared.so; do
        echo "      build/ui/$f$(printf '%*s' $((24 - ${#f})) '')$(md5sum build/ui/$f | cut -d' ' -f1)"
    done
}

dev_md5() { adb shell "su -c 'md5sum $1'" 2>/dev/null | awk '{print $1}' | tr -d '\r'; }

do_deploy() {
    [ -f build/vtouchd_ui ] && [ -f build/ui/classes.dex ] || { echo "缺产物 —— 先 sh scripts/ui-deploy.sh build"; exit 1; }
    echo "[3/4] 推产物（先经 /sdcard 中转：/data/local/tmp 对 shell 不可写）"
    adb shell "mkdir -p $STAGE" >/dev/null
    adb push build/vtouchd_ui $STAGE/vtouchd_ui >/dev/null
    adb push build/ui/classes.dex $STAGE/classes.dex >/dev/null
    adb push build/ui/libtestimgui.so $STAGE/libtestimgui.so >/dev/null
    adb push build/ui/libc++_shared.so $STAGE/libc++_shared.so >/dev/null
    adb push scripts/ui_ondev.sh $STAGE/ui_ondev.sh >/dev/null
    # 正在跑的 ELF 被覆盖会 Text file busy：先停核（它会带走面板）
    adb shell "su -c 'kill -TERM \$(pidof vtouchd_ui) 2>/dev/null; sleep 1; kill -9 \$(pidof vtouchd_ui) 2>/dev/null; mkdir -p $UI_DIR; cp -f $STAGE/* $UI_DIR/; mv -f $UI_DIR/vtouchd_ui $CORE; mv -f $UI_DIR/ui_ondev.sh /data/local/tmp/ui_ondev.sh; chmod 755 $CORE /data/local/tmp/ui_ondev.sh; chmod 644 $UI_DIR/classes.dex $UI_DIR/*.so; rm -rf $STAGE'" >/dev/null
    echo "      设备侧对账："
    for f in classes.dex libtestimgui.so libc++_shared.so; do
        l=$(md5sum build/ui/$f | cut -d' ' -f1); r=$(dev_md5 "$UI_DIR/$f")
        [ "$l" = "$r" ] && echo "        ✓ $f  $r" || { echo "        ✗ $f 本机=$l 设备=$r"; exit 1; }
    done
    l=$(md5sum build/vtouchd_ui | cut -d' ' -f1); r=$(dev_md5 "$CORE")
    [ "$l" = "$r" ] && echo "        ✓ vtouchd_ui  $r" || { echo "        ✗ vtouchd_ui 本机=$l 设备=$r"; exit 1; }
}

ondev() { adb shell "su -c 'sh /data/local/tmp/ui_ondev.sh 1440 3168 $1'" 2>/dev/null; }

case "${1:-all}" in
  build)  need_adb; do_build ;;
  deploy) need_adb; do_deploy ;;
  start)  need_adb; echo "[4/4] 起（核心会自己拉起面板）"; ondev start ;;
  stop)   need_adb; ondev stop ;;
  status) need_adb; ondev status ;;
  all)    need_adb; do_build; do_deploy; echo "[4/4] 起（核心会自己拉起面板）"; ondev start ;;
  *)      echo "用法: sh scripts/ui-deploy.sh [all|build|deploy|start|stop|status]"; exit 1 ;;
esac
