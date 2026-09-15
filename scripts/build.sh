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
  MINGW*|MSYS*|CYGWIN*) HOST_TAG="windows-x86_64"; EXT=".cmd" ;;
  *)                    HOST_TAG="linux-x86_64";   EXT="" ;;
esac

CC="$NDK/toolchains/llvm/prebuilt/$HOST_TAG/bin/aarch64-linux-android${API}-clang$EXT"
# 存在性检查用 -f 而不是 -x：Windows 上编译器是 clang.cmd，MSYS 不认它为可执行
[ -f "$CC" ] || { echo "缺编译器 $CC —— 检查 NDK_ROOT / API_LEVEL / 平台"; exit 1; }

mkdir -p build
"$CC" -O2 -Wall -Wextra -Werror -D_GNU_SOURCE src/vtouchd.c -o build/vtouchd

ls -l build/vtouchd
md5sum build/vtouchd
echo "构建完成: build/vtouchd（arm64）"
