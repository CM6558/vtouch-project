#!/data/data/com.termux/files/usr/bin/bash
# vtouch 编辑台 —— 设备端构建管线（javac -> d8 -> 手写 AXML/ARSC -> zip -> apksigner）。
# 说明：aapt2/zipalign 是 x86_64 glibc 二进制，Termux(aarch64) 跑不了；
#       d8/apksigner 是纯 Java，AXML/ARSC 由 tools/*.py 手写编码。
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
SDK="$HERE/sdk"
APP="$HERE/app"
B="$HERE/build"
OUT="$HERE/out"
KEYS="$HERE/keys"
PY=/data/data/com.termux/files/usr/bin/python3

D8="$SDK/d8.jar"
APKSIGNER="$SDK/apksigner.jar"
ANDROID_JAR="$SDK/android.jar"
PKG=com/vtouch/editor

for f in "$D8" "$APKSIGNER" "$ANDROID_JAR" "$APP/icon/ic_launcher.png" "$APP/fonts/geist-medium.ttf" "$APP/fonts/jetbrains-mono.ttf"; do
  [ -f "$f" ] || { echo "缺少 $f"; exit 1; }
done

rm -rf "$B"
mkdir -p "$B/classes" "$B/dex" "$OUT" "$KEYS"

echo "[1/7] javac"
mapfile -t SRCS < <(ls "$APP"/src/$PKG/*.java)
javac --release 8 -classpath "$ANDROID_JAR" -d "$B/classes" "${SRCS[@]}"

echo "[2/7] d8 -> classes.dex"
mapfile -t CLS < <(ls "$B"/classes/$PKG/*.class)
java -cp "$D8" com.android.tools.r8.D8 --release --min-api 24 \
  --lib "$ANDROID_JAR" --output "$B/dex" "${CLS[@]}"
ls -l "$B/dex/classes.dex"

echo "[3/7] 手写 AXML 编码 AndroidManifest.xml"
"$PY" "$HERE/tools/axml_encode.py" "$APP/AndroidManifest.xml" "$B/AndroidManifest.xml"

echo "[4/7] 手写 ARSC 编码 resources.arsc（应用图标）"
"$PY" "$HERE/tools/arsc_encode.py" "$B/resources.arsc"

echo "[5/7] 打包(未签名)"
"$PY" "$HERE/tools/pack_apk.py" "$B/AndroidManifest.xml" "$B/dex/classes.dex" \
  "$OUT/vtouch-editor-unsigned.apk" \
  "resources.arsc=$B/resources.arsc" "res/mipmap/ic_launcher.png=$APP/icon/ic_launcher.png" \
  "assets/fonts/geist-medium.ttf=$APP/fonts/geist-medium.ttf" \
  "assets/fonts/jetbrains-mono.ttf=$APP/fonts/jetbrains-mono.ttf"

echo "[6/7] 签名"
if [ ! -f "$KEYS/vtouch.jks" ]; then
  keytool -genkeypair -keystore "$KEYS/vtouch.jks" -alias vtouch \
    -keyalg RSA -keysize 2048 -validity 10000 -storetype PKCS12 \
    -storepass vtouch -keypass vtouch -dname "CN=vtouch-editor,O=dev"
fi
java -cp "$APKSIGNER" com.android.apksigner.ApkSignerTool sign \
  --ks "$KEYS/vtouch.jks" --ks-pass pass:vtouch \
  --ks-key-alias vtouch --key-pass pass:vtouch \
  --out "$OUT/vtouch-editor.apk" "$OUT/vtouch-editor-unsigned.apk"

echo "[7/7] 校验签名"
java -cp "$APKSIGNER" com.android.apksigner.ApkSignerTool verify --print-certs \
  "$OUT/vtouch-editor.apk" | head -5

echo "OK -> $OUT/vtouch-editor.apk"
ls -l "$OUT/vtouch-editor.apk"
