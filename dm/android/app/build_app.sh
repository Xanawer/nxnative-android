#!/bin/bash
# Build the app (docs/android/PLAN.md, dm/android/app): libdmhost.so (dm/android/cmake/build.sh) in an APK whose activity, HostActivity (java/), is a NativeActivity with the software keyboard's
# input box, made as the Phase 0 probe APKs are (javac and d8 for classes.dex, aapt2 link, zip, zipalign, apksigner with the debug keystore; no Gradle). The APK carries no game files: run_app.sh pushes the modules, the RomFS and env.txt to the phone.
# Usage: build_app.sh <GAME> [--gpu null|vkn] [build.sh options: --tests, --clean]    Output: dm/android/app/build/nxhost.apk (org.nxnative.host; build/ is not committed), nxhost-vkn.apk with --gpu vkn.
# --gpu vkn: the Vulkan backend (build.sh --gpu vkn, dm/android/cmake/build-vkn), with libadrenotools and its hook libraries beside libdmhost.so (DM_VK_DRIVER=842 / turnip: vk_dev.c opens
# them from its own directory), built as for the P0.7 probe (ADRENOTOOLS_BUILD, default ~/dm-work/android/src/libadrenotools-build), and VALIDATION_LAYER=<arm64 libVkLayer_khronos_validation.so>
# (optional, Khronos' android-binaries release) in the same directory, where the system loader finds it for DM_VK_VALIDATION=1 with every driver, as dm/android/probe/shader/build.sh packages it.
# None of these binaries is in the repository; the null APK carries none of them.
# ANDROID_HOME overrides the SDK, APP_KEYSTORE the signing keystore (the standard debug keystore by default; never in the repository), JAVA_HOME the JDK (javac, 17 or later)
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ANDROID_HOME=${ANDROID_HOME:-$HOME/Library/Android/sdk}
NDK=${ANDROID_NDK_HOME:-$ANDROID_HOME/ndk/27.1.12297006}
BT=${APP_BUILD_TOOLS:-$ANDROID_HOME/build-tools/36.1.0}
ANDROID_JAR=$ANDROID_HOME/platforms/android-36/android.jar
KEYSTORE=${APP_KEYSTORE:-$HOME/.android/debug.keystore}
die() { echo "error: $*" >&2; exit 1; }
GAME=${1:-}; [ -n "$GAME" ] && [ "${GAME#-}" = "$GAME" ] || die "usage: build_app.sh <GAME> [--gpu null|vkn] [build.sh options]"; shift
GPU=null BARGS=()
while [ $# -gt 0 ]; do case $1 in --gpu) GPU=${2:-}; shift 2 || die "--gpu takes null or vkn" ;; *) BARGS+=("$1"); shift ;; esac; done   # the rest go to build.sh (it refuses what it does not know)
case $GPU in null) LIB=$HERE/../cmake/build/libdmhost.so APKN=nxhost.apk ;; vkn) LIB=$HERE/../cmake/build-vkn/libdmhost.so APKN=nxhost-vkn.apk ;; *) die "--gpu takes null or vkn, not '$GPU'" ;; esac
for t in aapt2 zipalign apksigner; do [ -x "$BT/$t" ] || die "no $t in $BT"; done
JAVAC=${JAVA_HOME:+$JAVA_HOME/bin/}javac
"$JAVAC" -version >/dev/null 2>&1 || die "no working javac (set JAVA_HOME to a JDK 17 or later)"
[ -f "$ANDROID_JAR" ] || die "no $ANDROID_JAR"; [ -f "$KEYSTORE" ] || die "no keystore at $KEYSTORE"
EXTRA=()
if [ $GPU = vkn ]; then
    AT=${ADRENOTOOLS_BUILD:-$HOME/dm-work/android/src/libadrenotools-build}
    for l in "$AT/libadrenotools.so" "$AT/src/hook/libhook_impl.so" "$AT/src/hook/libmain_hook.so" "$AT/src/hook/libfile_redirect_hook.so" "$AT/src/hook/libgsl_alloc_hook.so"; do
        [ -f "$l" ] || die "missing $l (build libadrenotools first: docs/android/probes-gpu.md, \"Loading the drivers\")"; EXTRA+=("$l"); done
    if [ -n "${VALIDATION_LAYER:-}" ]; then [ -f "$VALIDATION_LAYER" ] || die "missing $VALIDATION_LAYER"; EXTRA+=("$VALIDATION_LAYER"); fi
fi
"$HERE/../cmake/build.sh" "$GAME" --gpu $GPU ${BARGS[@]+"${BARGS[@]}"}
OUT=$HERE/build; mkdir -p "$OUT"; W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
mkdir -p "$W/apk/lib/arm64-v8a"
for e in ${EXTRA[@]+"${EXTRA[@]}"}; do cp "$e" "$W/apk/lib/arm64-v8a/"; done
# Debug info stays in dm/android/cmake/build/libdmhost.so (symbolize crash offsets there); the APK's copy keeps its dynamic symbols, which dladdr names in crash dumps
"$NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin/llvm-strip" --strip-debug -o "$W/apk/lib/arm64-v8a/libdmhost.so" "$LIB"
# Every library 16 KB-aligned: a phone with 16 KB pages cannot load one that is not, and Android 17 shows a warning over a debuggable app with one (libadrenotools: docs/android/probes-gpu.md)
for so in "$W"/apk/lib/arm64-v8a/*.so; do
    al=$("$NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin/llvm-readelf" -lW "$so" | awk '$1 == "LOAD" { print $NF }' | while read -r h; do echo $((h)); done | sort -n | head -1)
    [ "${al:-0}" -ge 16384 ] || die "$(basename "$so") has a LOAD segment aligned to $al bytes, not 16 KB (build it with -DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON or -Wl,-z,max-page-size=16384)"
done
# HostActivity: Java 8 bytecode against the platform's android.jar, then dex (d8 --release, minimum API as the manifest's)
mkdir -p "$W/classes" "$W/apk/dex"
# (find -exec … {} +: paths with spaces stay whole, and find fails when the command does)
find "$HERE/java" -name '*.java' -exec "$JAVAC" -source 8 -target 8 -nowarn -Xlint:-options -encoding UTF-8 -bootclasspath "$ANDROID_JAR" -d "$W/classes" {} +
find "$W/classes" -name '*.class' -exec "$BT/d8" --release --min-api 35 --lib "$ANDROID_JAR" --output "$W/apk/dex" {} +
mv "$W/apk/dex/classes.dex" "$W/apk/classes.dex"; rmdir "$W/apk/dex"
"$BT/aapt2" link -o "$W/base.apk" --manifest "$HERE/AndroidManifest.xml" -I "$ANDROID_JAR"
(cd "$W/apk" && zip -q -r "$W/base.apk" classes.dex lib)
"$BT/zipalign" -f -P 16 4 "$W/base.apk" "$W/aligned.apk"   # 16 KB page alignment for uncompressed libraries, as the probes
"$BT/apksigner" sign --ks "$KEYSTORE" --ks-pass pass:android --ks-key-alias androiddebugkey --key-pass pass:android --out "$OUT/$APKN" "$W/aligned.apk"
rm -f "$OUT/$APKN.idsig"
echo "built $OUT/$APKN (org.nxnative.host, game $GAME, GPU backend $GPU, $(du -h "$OUT/$APKN" | cut -f1)); lib/arm64-v8a: $(cd "$W/apk/lib/arm64-v8a" && ls | tr '\n' ' ')"
