#!/bin/sh
# build_ui.sh — 编译 ImGui 单进程 UI（PC，Git Bash）。
# 产物: build/ui/classes.dex + build/ui/libtestimgui.so
# 用法: sh scripts/build_ui.sh
set -e
cd "$(dirname "$0")/.."
NDK="C:/Users/21102/android-ndk-r27d/toolchains/llvm/prebuilt/windows-x86_64/bin"
SDK="C:/Users/21102/AppData/Local/Android/Sdk"
CC="$NDK/aarch64-linux-android24-clang.cmd"
STRIP="$NDK/llvm-strip.exe"
CXX="$NDK/aarch64-linux-android24-clang++.cmd"
AJAR="$SDK/platforms/android-24/android.jar"
D8="$SDK/build-tools/34.0.0/d8.bat"
IMG=thirdparty/imgui

mkdir -p build/ui/classes build/ui/dex build/ui/obj
echo "[1/4] javac..."
javac -encoding UTF-8 -cp "$AJAR" -d build/ui/classes src-ui/VTouchUI.java
echo "[2/4] d8..."
"$D8" --lib "$AJAR" --min-api 24 --output build/ui/dex build/ui/classes/VTouchUI*.class
mv build/ui/dex/classes.dex build/ui/classes.dex
echo "[3/4] ndk cc..."
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
