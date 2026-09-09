#!/usr/bin/env bash
# ============================================================================
# VTouch AutoJs6 应用插件 APK 命令行构建（双 ABI）
# 产物: out/vtouch-plugin.apk（arm64-v8a 实机 + x86_64 模拟器，按设备 ABI 自动选择）
#
# 前置要求（任一）：
#   A) 本地构建: ANDROID_HOME + NDK（$ANDROID_NDK_HOME 或 $ANDROID_HOME/ndk/*）
#      + JDK 17+（$JAVA_HOME；未设时自动用 Android Studio 自带 jbr）
#   B) 或推到 GitHub 用 .github/workflows/build-apk.yml（推荐，自动装 NDK 双 ABI 构建）
#
# native 二进制: 优先用 NDK 交叉编译 arm64 + x86_64（vtouchd.c,
#   纯 C 无 STL 依赖）；无 NDK 时回退只打包 sdcard/vtouch-merge 已有 arm64 产物
#   （x86_64 模拟器支持需 NDK 重新构建）。
# ============================================================================
set -euo pipefail
cd "$(dirname "$0")"
ROOT=../..

# ---- 定位 JDK（优先 JAVA_HOME，回退 Android Studio jbr）----
if [ -n "${JAVA_HOME:-}" ] && [ -x "$JAVA_HOME/bin/java" ]; then
    JDK="$JAVA_HOME"
else
    for cand in "/c/Program Files/Android/Android Studio/jbr" "/opt/android-studio/jbr" "$HOME/android-studio/jbr"; do
        if [ -x "$cand/bin/java" ]; then JDK="$cand"; break; fi
    done
fi
: "${JDK:?未找到 JDK 17+ (设置 JAVA_HOME 或安装 Android Studio)}"
echo "JDK: $JDK ($("$JDK/bin/java" -version 2>&1 | head -1))"

: "${ANDROID_HOME:?请设置 ANDROID_HOME 指向 Android SDK}"
BT=$(ls -d "$ANDROID_HOME"/build-tools/* | sort -V | tail -1)
PLATFORM=$(ls -d "$ANDROID_HOME"/platforms/* | sort -V | tail -1)
echo "build-tools: $BT"
echo "platform:    $PLATFORM"

rm -rf build out && mkdir -p build/gen build/obj build/dex out

# ---- 1) native 双 ABI（NDK 交叉编译；无 NDK 回退 arm64 仅 sdcard 产物）----
NDK=${ANDROID_NDK_HOME:-}
if [ -z "$NDK" ]; then
    NDK=$(ls -d "$ANDROID_HOME"/ndk/* 2>/dev/null | sort -V | tail -1 || true)
fi
build_native() {
    local abi=$1 clang=$2
    mkdir -p "build/native/$abi"
    "$clang" -O2 -Wall -Wextra -D_GNU_SOURCE "$ROOT/src/vtouchd.c" -o "build/native/$abi/vtouchd"
    echo "native $abi: OK"
}
if [ -n "$NDK" ]; then
    P="$NDK/toolchains/llvm/prebuilt"
    ARCH_HOST=$(uname -s | tr 'A-Z' 'a-z')   # linux / darwin / msys?
    case "$ARCH_HOST" in
        *msys*|*cygwin*|*mingw*) PH="windows-x86_64"; EXE=".cmd" ;;
        linux*)                    PH="linux-x86_64"; EXE="" ;;
        darwin*)                   PH="darwin-x86_64"; EXE="" ;;
        *) echo "未知宿主平台: $ARCH_HOST"; exit 1 ;;
    esac
    P="$P/$PH/bin"
    [ -f "$P/aarch64-linux-android24-clang$EXE" ] || { echo "NDK 缺少 aarch64 clang: $P"; exit 1; }
    build_native arm64-v8a "$P/aarch64-linux-android24-clang$EXE"
    [ -f "$P/x86_64-linux-android24-clang$EXE" ] || { echo "NDK 缺少 x86_64 clang: $P"; exit 1; }
    build_native x86_64 "$P/x86_64-linux-android24-clang$EXE"
else
    echo "WARN: 未检测到 NDK, 仅打包 arm64 (sdcard 已有产物); x86_64 模拟器支持需 NDK"
    mkdir -p build/native/arm64-v8a
    cp -f "$ROOT/sdcard/vtouch-merge/vtouchd" build/native/arm64-v8a/
fi

# ---- 2) 编译 manifest（不 -A assets：Windows 上 aapt2 会把条目名拼成反斜杠，
#    assets 改用 jar --update 添加，JDK jar 强制正斜杠条目名，跨平台一致）----
"$BT/aapt2" link -o build/unsigned.apk \
    --manifest AndroidManifest.xml \
    -I "$PLATFORM/android.jar"
echo "aapt2 link: OK"

# ---- 3) 编译 Java 注册类（-encoding UTF-8：Windows 默认 GBK，源文件为 UTF-8）----
"$JDK/bin/javac" -encoding UTF-8 -source 8 -target 8 -classpath "$PLATFORM/android.jar" \
    -d build/obj $(find src -name '*.java')
echo "javac: OK"

# ---- 4) class -> dex（d8 为 .bat 包装，跨平台用 java -cp 直接调 d8.jar）----
"$JDK/bin/java" -cp "$BT/lib/d8.jar" com.android.tools.r8.D8 \
    --lib "$PLATFORM/android.jar" --output build/dex $(find build/obj -name '*.class')
echo "d8: OK"

# ---- 5) assets(JS + native 双 ABI) + dex 打入 APK ----
#    jar 条目名必须是 "assets/..." 正斜杠前缀，AssetManager 依赖该前缀
mkdir -p build/native-assets/assets/native
cp -rf build/native/* build/native-assets/assets/native/
"$JDK/bin/jar" --update --file build/unsigned.apk -C . assets/vtouch/index.js
"$JDK/bin/jar" --update --file build/unsigned.apk -C build/native-assets assets/native
"$JDK/bin/jar" --update --file build/unsigned.apk -C build/dex classes.dex
echo "add assets(js+native) + classes.dex: OK"

# ---- 6) zipalign ----
"$BT/zipalign" -f 4 build/unsigned.apk build/aligned.apk
echo "zipalign: OK"

# ---- 7) 签名（keystore 固定在 HOME 下，不随 build/ 清理而变，保证签名/授权指纹稳定）----
KEYSTORE=${VT_KEYSTORE:-"$HOME/.vtouch-plugin.keystore"}
STORE_PASS=${VT_PLUGIN_STORE_PASS:-vtouch123}
if [ ! -f "$KEYSTORE" ]; then
    "$JDK/bin/keytool" -genkeypair -v \
        -keystore "$KEYSTORE" -storepass "$STORE_PASS" -alias vtouch \
        -keyalg RSA -keysize 2048 -validity 10000 \
        -dname "CN=VTouch, OU=VTouch, O=VTouch, L=City, S=State, C=CN" \
        -keypass "$STORE_PASS"
    echo "keystore generated"
fi

mkdir -p out
"$JDK/bin/java" -jar "$BT/lib/apksigner.jar" sign \
    --ks "$KEYSTORE" --ks-pass "pass:$STORE_PASS" \
    --out out/vtouch-plugin.apk build/aligned.apk
echo "sign: OK"
echo "=================================================================="
echo "产物: out/vtouch-plugin.apk (ABI: $(ls build/native | tr '\n' ' '))"
echo "安装: adb install out/vtouch-plugin.apk"
echo "脚本加载: plugins.load('org.vtouch.plugin')"
