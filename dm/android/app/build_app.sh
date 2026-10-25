#!/bin/bash
# Build the Phase 1 app (docs/android/PLAN.md, dm/android/app): libdmhost.so (dm/android/cmake/build.sh) in a NativeActivity APK with no Java code, made as the Phase 0 probe APKs are
# (aapt2 link, zip, zipalign, apksigner with the debug keystore; no Gradle). The APK carries no game files: run_app.sh pushes the modules, the RomFS and env.txt to the phone.
# Usage: build_app.sh <GAME> [--clean]    Output: dm/android/app/build/nxhost.apk (org.nxnative.host; build/ is not committed).
# ANDROID_HOME overrides the SDK, APP_KEYSTORE the signing keystore (the standard debug keystore by default; never in the repository)
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ANDROID_HOME=${ANDROID_HOME:-$HOME/Library/Android/sdk}
NDK=${ANDROID_NDK_HOME:-$ANDROID_HOME/ndk/27.1.12297006}
BT=${APP_BUILD_TOOLS:-$ANDROID_HOME/build-tools/36.1.0}
ANDROID_JAR=$ANDROID_HOME/platforms/android-36/android.jar
KEYSTORE=${APP_KEYSTORE:-$HOME/.android/debug.keystore}
die() { echo "error: $*" >&2; exit 1; }
GAME=${1:-}; [ -n "$GAME" ] && [ "${GAME#-}" = "$GAME" ] || die "usage: build_app.sh <GAME> [--clean]"; shift
for t in aapt2 zipalign apksigner; do [ -x "$BT/$t" ] || die "no $t in $BT"; done
[ -f "$ANDROID_JAR" ] || die "no $ANDROID_JAR"; [ -f "$KEYSTORE" ] || die "no keystore at $KEYSTORE"
"$HERE/../cmake/build.sh" "$GAME" "$@"
OUT=$HERE/build; mkdir -p "$OUT"; W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
mkdir -p "$W/apk/lib/arm64-v8a"
# Debug info stays in dm/android/cmake/build/libdmhost.so (symbolize crash offsets there); the APK's copy keeps its dynamic symbols, which dladdr names in crash dumps
"$NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin/llvm-strip" --strip-debug -o "$W/apk/lib/arm64-v8a/libdmhost.so" "$HERE/../cmake/build/libdmhost.so"
"$BT/aapt2" link -o "$W/base.apk" --manifest "$HERE/AndroidManifest.xml" -I "$ANDROID_JAR"
(cd "$W/apk" && zip -q -r "$W/base.apk" lib)
"$BT/zipalign" -f -P 16 4 "$W/base.apk" "$W/aligned.apk"   # 16 KB page alignment for uncompressed libraries, as the probes
"$BT/apksigner" sign --ks "$KEYSTORE" --ks-pass pass:android --ks-key-alias androiddebugkey --key-pass pass:android --out "$OUT/nxhost.apk" "$W/aligned.apk"
rm -f "$OUT/nxhost.apk.idsig"
echo "built $OUT/nxhost.apk (org.nxnative.host, game $GAME, $(du -h "$OUT/nxhost.apk" | cut -f1))"
