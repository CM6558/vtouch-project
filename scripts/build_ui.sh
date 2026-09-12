#!/bin/sh
# build_ui.sh — 编译 ImGui 单进程面板（本机 Git Bash / CI Linux 通用）。
# 产物: build/ui/classes.dex + build/ui/libtestimgui.so
# 用法: sh scripts/build_ui.sh
#
# 路径可用环境变量覆盖（CI 依赖这些）:
#   NDK_ROOT              Android NDK（默认本机 C:/Users/21102/android-ndk-r27d）
#   ANDROID_SDK_ROOT      Android SDK（默认本机 %LOCALAPPDATA%/Android/Sdk；也认 ANDROID_HOME）
#   BUILD_TOOLS_VERSION   build-tools 版本（默认 34.0.0）
#   API_LEVEL             API 等级（默认 24）
#
# 依赖 thirdparty/imgui（不入库，自拉同版本）:
#   git clone --depth 1 -b v1.91.8 https://github.com/ocornut/imgui thirdparty/imgui
set -e
cd "$(dirname "$0")/.."

NDK="${NDK_ROOT:-C:/Users/21102/android-ndk-r27d}"
SDK="${ANDROID_SDK_ROOT:-${ANDROID_HOME:-C:/Users/21102/AppData/Local/Android/Sdk}}"
BT="${BUILD_TOOLS_VERSION:-34.0.0}"
API="${API_LEVEL:-24}"
IMG=thirdparty/imgui

# Windows(MSYS/Git Bash) 用 .cmd/.bat 包装，Linux(CI) 是裸可执行；NDK 预编译目录名也不同
case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*) HOST_TAG="windows-x86_64"; EXT=".cmd"; STRIP_EXT=".exe"; D8_EXT=".bat" ;;
  *)                    HOST_TAG="linux-x86_64";   EXT="";     STRIP_EXT="";     D8_EXT="" ;;
esac

NDKBIN="$NDK/toolchains/llvm/prebuilt/$HOST_TAG/bin"
CC="$NDKBIN/aarch64-linux-android${API}-clang$EXT"
CXX="$NDKBIN/aarch64-linux-android${API}-clang++$EXT"
STRIP="$NDKBIN/llvm-strip$STRIP_EXT"
AJAR="$SDK/platforms/android-$API/android.jar"
D8="$SDK/build-tools/$BT/d8$D8_EXT"

# 依赖先查清楚再动手：缺什么直接说怎么装，别编到一半报一堆找不到
[ -f "$IMG/imgui.h" ] || { echo "缺 $IMG — git clone --depth 1 -b v1.91.8 https://github.com/ocornut/imgui $IMG"; exit 1; }
[ -f "$AJAR" ]        || { echo "缺 $AJAR — 用 sdkmanager 装: platforms;android-$API"; exit 1; }
[ -f "$D8" ]          || { echo "缺 $D8 — 用 sdkmanager 装: build-tools;$BT"; exit 1; }
command -v javac >/dev/null 2>&1 || { echo "缺 javac — 装 JDK（CI 用 actions/setup-java）"; exit 1; }

mkdir -p build/ui/classes build/ui/dex build/ui/obj
echo "[1/5] javac..."
javac -encoding UTF-8 -cp "$AJAR" -d build/ui/classes src-ui/VTouchUI.java
echo "[2/5] d8..."
"$D8" --lib "$AJAR" --min-api "$API" --output build/ui/dex build/ui/classes/VTouchUI*.class
mv build/ui/dex/classes.dex build/ui/classes.dex
echo "[3/5] ndk cc..."
"$CC" -O2 -Wall -fPIC -D_GNU_SOURCE -c src/vtouchd.c -o build/ui/obj/vtouchd.o
for f in imgui imgui_draw imgui_tables imgui_widgets; do
  "$CXX" -O2 -Wall -fPIC -I$IMG -c $IMG/$f.cpp -o build/ui/obj/$f.o
done
"$CXX" -O2 -Wall -fPIC -DIMGUI_IMPL_OPENGL_ES2 -I$IMG -c $IMG/backends/imgui_impl_opengl3.cpp \
  -o build/ui/obj/imgui_impl_opengl3.o
"$CXX" -O2 -Wall -fPIC -I$IMG -Isrc-ui -c src-ui/vtouch_ui.cpp -o build/ui/obj/vtouch_ui.o
echo "[4/5] link..."
"$CXX" -shared -o build/ui/libtestimgui.so build/ui/obj/*.o \
  -lEGL -lGLESv2 -landroid -llog -lm
echo "[5/5] strip..."
"$STRIP" --strip-unneeded -o build/ui/libtestimgui.so.stripped build/ui/libtestimgui.so
mv -f build/ui/libtestimgui.so.stripped build/ui/libtestimgui.so
ls -l build/ui/classes.dex build/ui/libtestimgui.so
md5sum build/ui/classes.dex build/ui/libtestimgui.so | tee build/ui/md5.txt
