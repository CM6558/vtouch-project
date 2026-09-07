#!/usr/bin/env bash
# ============================================================================
# VTouch AutoJs6 应用插件 APK 命令行构建脚本
#
# 前置要求（任一即可）：
#   A) Android SDK build-tools + JDK 17+（本机或 CI）：
#        export ANDROID_HOME=/path/to/android-sdk     # 含 build-tools/ 和 platforms/
#        export JAVA_HOME=/path/to/jdk17
#   B) 或直接推到 GitHub 用 .github/workflows/build-apk.yml（见 README）
#
# 产物：out/vtouch-plugin.apk（首次运行自动生成签名 keystore）
# ============================================================================
set -euo pipefail
cd "$(dirname "$0")"

: "${ANDROID_HOME:?请设置 ANDROID_HOME 指向 Android SDK}"
: "${JAVA_HOME:?请设置 JAVA_HOME 指向 JDK 17+}"

BT=$(ls -d "$ANDROID_HOME"/build-tools/* | sort -V | tail -1)
PLATFORM=$(ls -d "$ANDROID_HOME"/platforms/* | sort -V | tail -1)
echo "build-tools: $BT"
echo "platform:    $PLATFORM"

rm -rf build out && mkdir -p build/gen build/obj build/dex out

# 1) 编译 manifest（不 -A assets：Windows 上 aapt2 会把条目名拼成反斜杠，Android AssetManager 无法读取。
#    assets 改用 jar --update 添加，JDK jar 强制正斜杠条目名，跨平台一致）
"$BT/aapt2" link -o build/unsigned.apk \
    --manifest AndroidManifest.xml \
    -I "$PLATFORM/android.jar"
echo "aapt2 link: OK"

# 2) 编译 Java 注册类（-encoding UTF-8：Windows 默认 GBK，源文件为 UTF-8）
"$JAVA_HOME/bin/javac" -encoding UTF-8 -source 8 -target 8 -classpath "$PLATFORM/android.jar" \
    -d build/obj $(find src -name '*.java')
echo "javac: OK"

# 3) class -> dex（d8 为 .bat 包装，跨平台用 java -cp 直接调 d8.jar）
"$JAVA_HOME/bin/java" -cp "$BT/lib/d8.jar" com.android.tools.r8.D8 \
    --lib "$PLATFORM/android.jar" --output build/dex $(find build/obj -name '*.class')
echo "d8: OK"

# 4) assets + dex 打入 APK（jar 条目名必须是 "assets/..." 正斜杠前缀，AssetManager 依赖该前缀）
"$JAVA_HOME/bin/jar" --update --file build/unsigned.apk -C . assets/vtouch/index.js
"$JAVA_HOME/bin/jar" --update --file build/unsigned.apk -C build/dex classes.dex
echo "add assets + classes.dex: OK"

# 5) zipalign
"$BT/zipalign" -f 4 build/unsigned.apk build/aligned.apk
echo "zipalign: OK"

# 6) 签名（keystore 固定在 HOME 下，不随 build/ 清理而变，保证签名/授权指纹稳定）
KEYSTORE=${VT_KEYSTORE:-"$HOME/.vtouch-plugin.keystore"}
STORE_PASS=${VT_PLUGIN_STORE_PASS:-vtouch123}
if [ ! -f "$KEYSTORE" ]; then
    "$JAVA_HOME/bin/keytool" -genkeypair -v \
        -keystore "$KEYSTORE" -storepass "$STORE_PASS" -alias vtouch \
        -keyalg RSA -keysize 2048 -validity 10000 \
        -dname "CN=VTouch, OU=VTouch, O=VTouch, L=City, S=State, C=CN" \
        -keypass "$STORE_PASS"
    echo "keystore generated"
fi

mkdir -p out
"$JAVA_HOME/bin/java" -jar "$BT/lib/apksigner.jar" sign \
    --ks "$KEYSTORE" --ks-pass "pass:$STORE_PASS" \
    --out out/vtouch-plugin.apk build/aligned.apk
echo "sign: OK"
echo "=================================================================="
echo "产物: out/vtouch-plugin.apk"
echo "安装: adb install out/vtouch-plugin.apk"
echo "脚本加载: plugins.load('org.vtouch.plugin')"
