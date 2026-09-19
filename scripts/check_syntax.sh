#!/bin/sh
# check_syntax.sh —— 单文件语法检查（只跑 -fsyntax-only，秒级）。
#
# 为什么单开一个：编辑器任务里不能写死 NDK 路径（评审 §3 点过）。这里与 build.sh / build_ui.sh
# 用**同一个**可覆盖变量 NDK_ROOT 与同一套告警开关，所以"编辑器里绿了、build.sh 里红"这种事不会发生。
#
# 用法（主机侧）:
#   sh scripts/check_syntax.sh src/vt_ws.c          # 单个 C 文件（核心那套开关）
#   sh scripts/check_syntax.sh src-ui/ui_glue.c     # 面板侧胶水（带 UI 的宏，见下）
#
# 可覆盖: NDK_ROOT / API_LEVEL
set -u
cd "$(dirname "$0")/.." || exit 1

FILE=${1:?用法: sh scripts/check_syntax.sh <文件>}
[ -f "$FILE" ] || { echo "没有这个文件: $FILE"; exit 1; }

NDK="${NDK_ROOT:-C:/Users/21102/android-ndk-r27d}"
API="${API_LEVEL:-24}"
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) HOST_TAG=windows-x86_64; EXT=.cmd ;;
    Darwin)               HOST_TAG=darwin-x86_64;  EXT= ;;
    *)                    HOST_TAG=linux-x86_64;   EXT= ;;
esac

# C++（面板 / imgui）走 clang++，C 走 clang；判据只看扩展名。
case "$FILE" in
    *.cpp|*.cc|*.hpp) CC="$NDK/toolchains/llvm/prebuilt/$HOST_TAG/bin/aarch64-linux-android${API}-clang++$EXT" ;;
    *)                CC="$NDK/toolchains/llvm/prebuilt/$HOST_TAG/bin/aarch64-linux-android${API}-clang$EXT" ;;
esac
[ -x "$CC" ] || [ -f "$CC" ] || { echo "缺编译器 $CC —— 检查 NDK_ROOT / API_LEVEL"; exit 1; }

# 与真实构建**逐字对齐**的开关（build.sh 用 -Wall -Wextra -Werror；build_ui.sh 的 UI 侧只开 -Wall）：
#   src/*.c          → 核心那套
#   src-ui/ui_glue.c → -DVT_UI -DVT_UI_PANEL -Isrc -Isrc-ui（build_ui.sh 的胶水那一行）
#   src-ui/ui_stubs.c / *.cpp → -Isrc-ui（vtouch_ui.cpp 还要 -Ithirdparty/imgui -Ibuild/ui：
#                               后者放生成出来的 ui_chars.h，先跑一次 sh scripts/build.sh ui）
IMG=thirdparty/imgui
case "$FILE" in
    src-ui/ui_glue.c)  FLAGS="-O2 -Wall -D_GNU_SOURCE -DVT_UI -DVT_UI_PANEL -Isrc -Isrc-ui -fsyntax-only" ;;
    src-ui/ui_stubs.c) FLAGS="-O2 -Wall -D_GNU_SOURCE -Isrc-ui -fsyntax-only" ;;
    src-ui/*.cpp)      FLAGS="-O2 -Wall -D_GNU_SOURCE -Isrc-ui -I$IMG -Ibuild/ui -fsyntax-only" ;;
    *)                 FLAGS="-O2 -Wall -Wextra -Werror -D_GNU_SOURCE -Isrc -fsyntax-only" ;;
esac

echo "$CC $FLAGS $FILE"
# shellcheck disable=SC2086
exec "$CC" $FLAGS "$FILE"
