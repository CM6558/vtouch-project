#!/bin/sh
# build_rotprobe.sh —— 构建旋转策略探针（src-ui/rotprobe.c + src-ui/RotProbeMain.java）。
# 产物: build/rotprobe/{librotprobe.so, classes.dex}
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
AJAR="$SDK/platforms/android-$API/android.jar"
D8="$SDK/build-tools/$BT/d8$D8_EXT"
[ -f "$AJAR" ] || { echo "缺 $AJAR"; exit 1; }
[ -f "$CC" ]   || { echo "缺编译器 $CC"; exit 1; }

rm -rf build/rotprobe; mkdir -p build/rotprobe/classes build/rotprobe/dex

echo "[1/3] javac..."
javac -encoding UTF-8 -cp "$AJAR" -d build/rotprobe/classes src-ui/RotProbeMain.java
echo "[2/3] d8..."
"$D8" --lib "$AJAR" --min-api "$API" --output build/rotprobe/dex build/rotprobe/classes/*.class
mv build/rotprobe/dex/classes.dex build/rotprobe/classes.dex
echo "[3/3] native .so..."
"$CC" -O2 -Wall -Wextra -fPIC -shared -D_GNU_SOURCE src-ui/rotprobe.c -o build/rotprobe/librotprobe.so -lEGL -lGLESv2 -landroid -llog
ls -l build/rotprobe/classes.dex build/rotprobe/librotprobe.so
md5sum build/rotprobe/classes.dex build/rotprobe/librotprobe.so
