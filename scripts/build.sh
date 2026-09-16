#!/bin/sh
# build.sh —— 编译最小版 vtouchd（arm64 / Android，NDK 交叉编译）。
# 用法: sh scripts/build.sh
# 可覆盖: NDK_ROOT / API_LEVEL
set -e
cd "$(dirname "$0")/.."

NDK="${NDK_ROOT:-C:/Users/21102/android-ndk-r27d}"
API="${API_LEVEL:-24}"

# Windows(Git Bash) 的编译器带 .cmd 包装，Linux 是裸可执行
case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*) HOST_TAG="windows-x86_64"; EXT=".cmd"; EXE=".exe" ;;
  *)                    HOST_TAG="linux-x86_64";   EXT="";     EXE="" ;;
esac

CC="$NDK/toolchains/llvm/prebuilt/$HOST_TAG/bin/aarch64-linux-android${API}-clang$EXT"
# 存在性检查用 -f 而不是 -x：Windows 上编译器是 clang.cmd，MSYS 不认它为可执行
[ -f "$CC" ] || { echo "缺编译器 $CC —— 检查 NDK_ROOT / API_LEVEL / 平台"; exit 1; }

mkdir -p build

# 可选参数: ui —— 编「带 UI 的核心」（-DVT_UI：状态进共享内存 + 核心拉起面板子进程），产物 build/vtouchd_ui。
# 不带参数 = 默认核心（行为不变：VT_UI 关，src/vt_shm.c / vt_panel.c 编成空 TU）。
if [ "${1:-}" = "ui" ]; then
  # B 方案（单可执行启动）：把面板三件套当作**二进制对象**链进核心，运行时自解包到
  # /data/local/tmp/vtouch-ui/ —— 设备上只需要一个文件，也就不存在"面板是旧的那一版"。
  # 面板产物必须先存在：VTOUCH_UI_CORE=real sh scripts/build_ui.sh
  # 想退回"设备上单独放面板文件"：VTOUCH_UI_EMBED=0 sh scripts/build.sh ui
  EMB=build/embed
  LINK_EXTRA=""
  DEFS="-DVT_UI"
  if [ "${VTOUCH_UI_EMBED:-1}" = "1" ]; then
    for f in classes.dex libtestimgui.so libc++_shared.so; do
      [ -f "build/ui/$f" ] || { echo "缺 build/ui/$f —— 先跑: VTOUCH_UI_CORE=real sh scripts/build_ui.sh"; exit 1; }
    done
    OBJCOPY="$(dirname "$CC")/llvm-objcopy$EXE"
    [ -f "$OBJCOPY" ] || { echo "缺 $OBJCOPY"; exit 1; }
    mkdir -p "$EMB"
    cp -f build/ui/classes.dex "$EMB/classes.dex"
    cp -f build/ui/libtestimgui.so "$EMB/libtestimgui.so"
    cp -f build/ui/libc++_shared.so "$EMB/libcxx_shared.so"   # 去掉 + 号，符号名可预测
    ( cd "$EMB" && for f in classes.dex libtestimgui.so libcxx_shared.so; do
        "$OBJCOPY" -I binary -O elf64-littleaarch64 -B aarch64 \
          --rename-section .data=.rodata,alloc,load,readonly,data,contents "$f" "$f.o"
      done )
    LINK_EXTRA="$EMB/classes.dex.o $EMB/libtestimgui.so.o $EMB/libcxx_shared.so.o"
  else
    DEFS="-DVT_UI -DVT_UI_NO_EMBED"
  fi
  "$CC" -O2 -Wall -Wextra -Werror -D_GNU_SOURCE $DEFS src/*.c $LINK_EXTRA -o build/vtouchd_ui
  ls -l build/vtouchd_ui
  md5sum build/vtouchd_ui
  if [ -n "$LINK_EXTRA" ]; then echo "构建完成: build/vtouchd_ui（arm64，带 UI 的核心，**已内嵌面板三件套**）"; else echo "构建完成: build/vtouchd_ui（arm64，带 UI 的核心，未内嵌面板）"; fi
  exit 0
fi

"$CC" -O2 -Wall -Wextra -Werror -D_GNU_SOURCE src/*.c -o build/vtouchd   # 模块化后是多个 .c，一起链成一个可执行

ls -l build/vtouchd
md5sum build/vtouchd
echo "构建完成: build/vtouchd（arm64）"
