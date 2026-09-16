#!/bin/sh
# build_probe.sh —— 构建架构可行性探针（probe/）：父进程可执行 + 子进程 JNI .so + Java dex。
# 用法: sh scripts/build_probe.sh
# 依赖与面板一致：NDK、Android SDK build-tools 34.0.0、javac。
# 产物: build/probe/{probe_native, libprobe.so, classes.dex}
set -e
cd "$(dirname "$0")/.."

NDK="${NDK_ROOT:-C:/Users/21102/android-ndk-r27d}"
SDK="${ANDROID_SDK_ROOT:-${ANDROID_HOME:-C:/Users/21102/AppData/Local/Android/Sdk}}"
BT="${BUILD_TOOLS_VERSION:-34.0.0}"
API="${API_LEVEL:-24}"

case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*) HOST_TAG="windows-x86_64"; EXT=".cmd"; D8_EXT=".bat" ;;
  *)                    HOST_TAG="linux-x86_64";   EXT="";     D8_EXT="" ;;
esac
CC="$NDK/toolchains/llvm/prebuilt/$HOST_TAG/bin/aarch64-linux-android${API}-clang$EXT"
D8="$SDK/build-tools/$BT/d8$D8_EXT"

[ -f "$CC" ] || { echo "缺编译器 $CC —— 检查 NDK_ROOT"; exit 1; }
command -v javac >/dev/null 2>&1 || { echo "缺 javac"; exit 1; }

mkdir -p build/probe/classes build/probe/dex
rm -rf build/probe/classes build/probe/dex
mkdir -p build/probe/classes build/probe/dex

echo "[1/3] 父进程可执行..."
"$CC" -O2 -Wall -Wextra -D_GNU_SOURCE -Iprobe probe/probe_native.c -o build/probe/probe_native
echo "[2/3] 子进程 JNI .so..."
"$CC" -O2 -Wall -Wextra -fPIC -shared -D_GNU_SOURCE -Iprobe probe/probe_jni.c -o build/probe/libprobe.so
echo "[3/3] Java → dex..."
javac -encoding UTF-8 -d build/probe/classes probe/ProbeMain.java
if [ -f "$D8" ]; then
  "$D8" --min-api "$API" --output build/probe/dex build/probe/classes/*.class
  mv build/probe/dex/classes.dex build/probe/classes.dex
else
  echo "缺 $D8 —— 用 sdkmanager 装 build-tools;$BT"; exit 1
fi
ls -l build/probe/probe_native build/probe/libprobe.so build/probe/classes.dex
md5sum build/probe/probe_native build/probe/libprobe.so build/probe/classes.dex
