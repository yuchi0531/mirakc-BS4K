#!/usr/bin/env bash
# Build a minimal debuggable helper APK that calls the exported recordext AIDL service.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
SDK="$HOME/android-sdk"
ANDROID_JAR="$SDK/platforms/android-34/android.jar"
BT="$SDK/build-tools/36.0.0"
KS="$HOME/.android/debug.keystore"

WORK="$HERE/build"
rm -rf "$WORK"
mkdir -p "$WORK/classes" "$WORK/dex" "$WORK/gen"

echo "[1/6] javac"
javac -source 11 -target 11 -Xlint:none \
    -cp "$ANDROID_JAR" \
    -d "$WORK/classes" \
    "$HERE/src/com/probe/rext/MainActivity.java"

echo "[2/6] d8"
"$BT/d8" --lib "$ANDROID_JAR" --min-api 23 \
    --output "$WORK/dex" \
    $(find "$WORK/classes" -name '*.class')

echo "[3/6] aapt2 link (manifest only)"
"$BT/aapt2" link -o "$WORK/base.apk" \
    -I "$ANDROID_JAR" \
    --manifest "$HERE/AndroidManifest.xml" \
    --java "$WORK/gen" \
    --min-sdk-version 23 \
    --target-sdk-version 34

echo "[4/6] add classes.dex"
( cd "$WORK/dex" && zip -q "$WORK/base.apk" classes.dex )

echo "[5/6] zipalign"
"$BT/zipalign" -f -p 4 "$WORK/base.apk" "$WORK/aligned.apk"

echo "[6/6] apksigner"
"$BT/apksigner" sign \
    --ks "$KS" --ks-pass pass:android --key-pass pass:android \
    --v1-signing-enabled true --v2-signing-enabled true \
    --out "$HERE/rext-helper.apk" "$WORK/aligned.apk"

echo "OK: $HERE/rext-helper.apk"
ls -la "$HERE/rext-helper.apk"
"$BT/apksigner" verify --print-certs "$HERE/rext-helper.apk" | head -20
