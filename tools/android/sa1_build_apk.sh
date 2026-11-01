#!/usr/bin/env bash
# Run from Git Bash. External tools/binaries and the keystore stay outside git.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
export MSYS_NO_PATHCONV=1
SDK=${SDK:-<作業ディレクトリ>/android/sdk}
BT="$SDK/build-tools/36.0.0"
NDK=${NDK:-<作業ディレクトリ>/android/android-ndk-r27c}
LLVM="$NDK/toolchains/llvm/prebuilt/windows-x86_64/bin"
PYTHON=${PYTHON:-<home>/AppData/Roaming/uv/python/cpython-3.12-windows-x86_64-none/python.exe}
JDK=${JDK:-C:/Program Files/Eclipse Adoptium/jdk-21.0.11.10-hotspot}
KEYSTORE=${KEYSTORE:-<作業ディレクトリ>/android/debug.keystore}
BUILD=${BUILD:-build-android}
PROJECT=${PROJECT:-projects/posegraph_demo}
PLAYER_SO=${PLAYER_SO:-$BUILD/dist/RelWithDebInfo/libpelican_player.so}
VVL=${VVL:-<作業ディレクトリ>/android/deps/vvl-vulkan-sdk-1.4.309.0/arm64-v8a/libVkLayer_khronos_validation.so}
OUT=${OUT:-$BUILD/sa1b-apk}
mkdir -p "$OUT"
# Keep each staging tree, including stripped libs, for inspection.
STAGE=$(mktemp -d "$OUT/stage.XXXXXX")
ASSETS="$STAGE/assets"
LIBS="$STAGE/lib/arm64-v8a"
mkdir -p "$LIBS"
"$PYTHON" tools/android/sa1_apk.py assets "$PROJECT" "$ASSETS"
cp tools/android/AndroidManifest.xml "$STAGE/AndroidManifest.xml"
"$BT/aapt2.exe" link -I "$SDK/platforms/android-36/android.jar" \
  --min-sdk-version 33 --target-sdk-version 36 \
  --manifest "$STAGE/AndroidManifest.xml" -o "$STAGE/base.apk"
"$PYTHON" tools/android/sa1_apk.py assets-zip "$STAGE/base.apk" "$ASSETS"
# Never strip the original: it is retained for ndk-stack.
"$LLVM/llvm-strip.exe" --strip-unneeded "$PLAYER_SO" -o "$LIBS/libpelican_player.so"
if [[ -f "$VVL" ]]; then
  cp "$VVL" "$LIBS/libVkLayer_khronos_validation.so"
elif [[ ${ALLOW_MISSING_VALIDATION:-0} == 1 ]]; then
  echo 'WARNING: validation binary missing; this APK CANNOT verify validation.' >&2
else
  echo "Missing pinned validation binary: $VVL (see sa1_fetch_validation.sh)" >&2
  exit 1
fi
"$PYTHON" tools/android/sa1_apk.py libs "$STAGE/base.apk" "$LIBS"
"$BT/zipalign.exe" -P 16 -f 4 "$STAGE/base.apk" "$STAGE/aligned.apk"
if [[ ! -f "$KEYSTORE" ]]; then
  "$JDK/bin/keytool.exe" -genkeypair -keystore "$KEYSTORE" \
    -alias androiddebugkey -storepass android -keypass android -keyalg RSA \
    -keysize 2048 -validity 10000 -dname 'CN=Android Debug,O=Android,C=US'
fi
"$JDK/bin/java.exe" -jar "$BT/lib/apksigner.jar" sign \
  --ks "$KEYSTORE" --ks-key-alias androiddebugkey \
  --ks-pass pass:android --key-pass pass:android \
  --out "$OUT/pelican-sa1b-debug.apk" "$STAGE/aligned.apk"
"$JDK/bin/java.exe" -jar "$BT/lib/apksigner.jar" verify --verbose "$OUT/pelican-sa1b-debug.apk"
"$BT/zipalign.exe" -c -P 16 -v 4 "$OUT/pelican-sa1b-debug.apk" | tee "$OUT/zipalign.txt"
for library in "$LIBS/"*.so; do
  "$LLVM/llvm-readelf.exe" -lW "$library" | tee "$OUT/$(basename "$library").readelf.txt"
  # Our library must support 16 KB pages. Khronos ships the validation layer
  # 4 KB-aligned; it goes only into this debug APK, so it is recorded, not gated.
  [[ "$(basename "$library")" == libVkLayer_* ]] && continue
  "$LLVM/llvm-readelf.exe" -lW "$library" | awk '
    $1 == "LOAD" { count++; if ($NF != "0x4000") bad=1 }
    END { if (count == 0 || bad) exit 1 }'
done
printf 'APK: %s\nStaging: %s\n' "$OUT/pelican-sa1b-debug.apk" "$STAGE"
