#!/bin/sh
# build_ui.sh — 编译 ImGui 单进程面板（本机 Git Bash / CI Linux 通用）。
# 产物: build/ui/classes.dex + build/ui/libtestimgui.so + build/ui/libc++_shared.so
# 用法: sh scripts/build_ui.sh
#   默认先清掉 classes/obj（陈旧 .class/.o 被打进 dex/so 是踩过的坑）；
#   想跳过大头（imgui 的 .o，重编约 40s）设 VTOUCH_UI_KEEP=1 —— 只在你确定源没动时用。
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
# 存在性检查而不是 -x：Windows 上编译器是 clang.cmd（MSYS 不认它为可执行），-x 会误报
[ -f "$CC" ] || { echo "缺编译器 $CC — 检查 NDK_ROOT / API_LEVEL / HOST_TAG"; exit 1; }

mkdir -p build/ui/classes build/ui/dex build/ui/obj
# 默认清干净再编：陈旧的 .class/.o（改过源码没重编、或删掉的类还留着）会被打进 dex/so ——
# 踩过的坑（历史上 SCProbe.class 就是这么混进 dex 的）。VTOUCH_UI_KEEP=1 只跳过 imgui 的 .o。
if [ "${VTOUCH_UI_KEEP:-0}" != "1" ]; then
  rm -rf build/ui/classes build/ui/obj
  mkdir -p build/ui/classes build/ui/obj
else
  rm -rf build/ui/classes
  mkdir -p build/ui/classes
fi
echo "[0/6] 面板字形表..."
# 字形表按源码实际用到的字符生成（别用 ChineseFull：2 万+ 汉字 × 44/30 两档全烘，真机 ~860ms）；
# 每次编译都重生成，改文案不可能漏（源码头 src-ui/ui_chars.h 由 gen_ui_chars.py 生成到 build/）。
PY=""
for c in python3 python; do            # Windows 上 python3 可能只是商店占位符，得真跑一下才算数
  if command -v "$c" >/dev/null 2>&1 && "$c" -c "print(1)" >/dev/null 2>&1; then PY="$c"; break; fi
done
[ -n "$PY" ] || { echo "缺可用的 python3/python —— 生成面板字形表要用"; exit 1; }
"$PY" scripts/gen_ui_chars.py --out build/ui/ui_chars.h
echo "[1/6] javac..."
javac -encoding UTF-8 -cp "$AJAR" -d build/ui/classes src-ui/VTouchUI.java
echo "[2/6] d8..."
# 喂全部 class（别写 VTouchUI*：类名一变/多一个顶层类就静默漏编，历史上 SCProbe.class 就这么混过）
"$D8" --lib "$AJAR" --min-api "$API" --output build/ui/dex build/ui/classes/*.class
mv build/ui/dex/classes.dex build/ui/classes.dex
echo "[3/6] ndk cc..."
# 核心来源（新核心是模块化多文件，UI 也要一个不依赖核心的单跑模式）:
#   VTOUCH_UI_CORE=stub（默认）面板单跑：11 个 vtouch_* 由 src-ui/ui_stubs.c 提供
#   VTOUCH_UI_CORE=real        接新核心：编译 src/*.c 全部 + 胶水层 src-ui/ui_glue.c
if [ "${VTOUCH_UI_CORE:-stub}" = "real" ]; then
  # 面板是**独立进程**：它只读区 A（状态）、读写区 B（邮箱/矩形）、只读区 C（事件环），
  # 所以只编 vt_util.c（raw_to_logical 等只读辅助）+ vt_shm.c 的面板半边（-DVT_UI_PANEL）。
  # 引擎在核心进程里跑（scripts/build.sh ui → build/vtouchd_ui），面板不再托管它。
  "$CC" -O2 -Wall -fPIC -D_GNU_SOURCE -DVT_UI -DVT_UI_PANEL -Isrc -c src/vt_util.c -o build/ui/obj/vt_util.o
  "$CC" -O2 -Wall -fPIC -D_GNU_SOURCE -DVT_UI -DVT_UI_PANEL -Isrc -c src/vt_shm.c  -o build/ui/obj/vt_shm.o
  "$CC" -O2 -Wall -fPIC -D_GNU_SOURCE -DVT_UI -DVT_UI_PANEL -Isrc -Isrc-ui -c src-ui/ui_glue.c -o build/ui/obj/ui_glue.o
else
  "$CC" -O2 -Wall -fPIC -D_GNU_SOURCE -Isrc-ui -c src-ui/ui_stubs.c -o build/ui/obj/ui_stubs.o
fi
for f in imgui imgui_draw imgui_tables imgui_widgets; do
  "$CXX" -O2 -Wall -fPIC -I$IMG -c $IMG/$f.cpp -o build/ui/obj/$f.o
done
"$CXX" -O2 -Wall -fPIC -DIMGUI_IMPL_OPENGL_ES2 -I$IMG -c $IMG/backends/imgui_impl_opengl3.cpp \
  -o build/ui/obj/imgui_impl_opengl3.o
"$CXX" -O2 -Wall -fPIC -I$IMG -Isrc-ui -Ibuild/ui -c src-ui/vtouch_ui.cpp -o build/ui/obj/vtouch_ui.o
echo "[4/6] link..."
"$CXX" -shared -o build/ui/libtestimgui.so build/ui/obj/*.o \
  -lEGL -lGLESv2 -landroid -llog -lm
echo "[5/6] strip..."
"$STRIP" --strip-unneeded -o build/ui/libtestimgui.so.stripped build/ui/libtestimgui.so
mv -f build/ui/libtestimgui.so.stripped build/ui/libtestimgui.so
echo "[6/6] libc++_shared.so..."
# 面板是 C++，NDK 默认**动态**链 libc++ —— 少了这个 .so，真机上 System.load 直接
# UnsatisfiedLinkError（被 VTouchUI 的 catch 吞成一条日志后 return，表现为"进程退 0 什么都不干"）。
# 踩过的坑，所以由脚本固定交付，不靠人记得。
LIBCXX="$NDK/toolchains/llvm/prebuilt/$HOST_TAG/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so"
[ -f "$LIBCXX" ] || { echo "缺 $LIBCXX —— NDK 安装不完整（sysroot 里应该有）"; exit 1; }
cp -f "$LIBCXX" build/ui/libc++_shared.so
ls -l build/ui/classes.dex build/ui/libtestimgui.so build/ui/libc++_shared.so
md5sum build/ui/classes.dex build/ui/libtestimgui.so build/ui/libc++_shared.so | tee build/ui/md5.txt
