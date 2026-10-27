#!/bin/bash
# Build the app (docs/android/PLAN.md, dm/android/app): libdmhost.so (dm/android/cmake/build.sh) in an APK whose activity, HostActivity (java/), is a NativeActivity with the software keyboard's
# input box, made as the Phase 0 probe APKs are (javac and d8 for classes.dex, aapt2 compile and link, zip, zipalign, apksigner with the debug keystore; no Gradle). By itself it makes the development
# APK, org.nxnative.host, which carries no game files: run_app.sh pushes the modules, the RomFS and env.txt to the phone. scripts/build_apk_from_nsp.py makes a game's APK with the options below.
# Usage: build_app.sh <GAME> [--gpu null|vkn] [--manifest F] [--res DIR] [--assets DIR] [--out APK] [--version-code N] [--version-name S] [build.sh options: --tests, --clean]
#   Output: dm/android/app/build/nxhost.apk (org.nxnative.host; build/ is not committed), nxhost-vkn.apk with --gpu vkn, or --out.
#   --manifest F    the manifest to link instead of AndroidManifest.xml (a game's: its package, label and icon, made from AndroidManifest.xml by build_apk_from_nsp.py)
#   --res DIR       Android resources (DIR/mipmap-*/, drawable-*/, values/: a game's icon), compiled with aapt2 compile --dir and linked in
#   --assets DIR    DIR's files go into the APK under assets/ (deflated): a game's package, DIR/nx/ (front_android.c unpacks it at start)
#   --version-code N, --version-name S   aapt2 link's (--replace-version: over the manifest's)
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
ANDROID_JAR=${APP_ANDROID_JAR:-$ANDROID_HOME/platforms/android-36/android.jar}   # APP_ANDROID_JAR: another platform's (an SDK with android-34 only)
KEYSTORE=${APP_KEYSTORE:-$HOME/.android/debug.keystore}
die() { echo "error: $*" >&2; exit 1; }
GAME=${1:-}; [ -n "$GAME" ] && [ "${GAME#-}" = "$GAME" ] || die "usage: build_app.sh <GAME> [--gpu null|vkn] [--manifest F] [--res DIR] [--assets DIR] [--out APK] [--version-code N] [--version-name S] [build.sh options]"; shift
GPU=null BARGS=() MANIFEST=$HERE/AndroidManifest.xml RES="" ASSETS="" OUTAPK="" VARGS=()
need() { [ $# -ge 2 ] && [ -n "$2" ] || die "$1 takes a value"; }
while [ $# -gt 0 ]; do case $1 in
    --gpu) need "$@"; GPU=$2; shift 2 ;; --manifest) need "$@"; MANIFEST=$2; shift 2 ;; --res) need "$@"; RES=$2; shift 2 ;; --assets) need "$@"; ASSETS=$2; shift 2 ;;
    --out) need "$@"; OUTAPK=$2; shift 2 ;; --version-code) need "$@"; VARGS+=(--version-code "$2"); shift 2 ;; --version-name) need "$@"; VARGS+=(--version-name "$2"); shift 2 ;;
    *) BARGS+=("$1"); shift ;; esac; done   # the rest go to build.sh (it refuses what it does not know)
case $GPU in null) LIB=$HERE/../cmake/build/libdmhost.so APKN=nxhost.apk ;; vkn) LIB=$HERE/../cmake/build-vkn/libdmhost.so APKN=nxhost-vkn.apk ;; *) die "--gpu takes null or vkn, not '$GPU'" ;; esac
[ -f "$MANIFEST" ] || die "no manifest $MANIFEST"
MIN_API=$(sed -n 's/.*minSdkVersion="\([0-9]*\)".*/\1/p' "$MANIFEST" | head -1); [ -n "$MIN_API" ] || die "no minSdkVersion in $MANIFEST"
export DM_ANDROID_API=${DM_ANDROID_API:-$MIN_API}   # the native build's API level follows the manifest (build.sh), as d8's --min-api below
[ -z "$RES" ] || [ -d "$RES" ] || die "no resource directory $RES"
[ -z "$ASSETS" ] || [ -d "$ASSETS" ] || die "no assets directory $ASSETS"
for t in aapt2 zipalign apksigner; do [ -x "$BT/$t" ] || die "no $t in $BT"; done
JAVAC=${JAVA_HOME:+$JAVA_HOME/bin/}javac
"$JAVAC" -version >/dev/null 2>&1 || die "no working javac (set JAVA_HOME to a JDK 17 or later)"
[ -f "$ANDROID_JAR" ] || die "no $ANDROID_JAR"; [ -f "$KEYSTORE" ] || die "no keystore at $KEYSTORE"
EXTRA=()
if [ $GPU = vkn ]; then
    AT=${ADRENOTOOLS_BUILD:-$HOME/dm-work/android/src/libadrenotools-build}
    # ADRENOTOOLS_BUILD=none: no driver-package loading (Mali and other non-Adreno targets: libadrenotools loads Qualcomm drivers only); DM_VK_DRIVER=842 / turnip then say in red that it is missing
    [ "$AT" = none ] || for l in "$AT/libadrenotools.so" "$AT/src/hook/libhook_impl.so" "$AT/src/hook/libmain_hook.so" "$AT/src/hook/libfile_redirect_hook.so" "$AT/src/hook/libgsl_alloc_hook.so"; do
        [ -f "$l" ] || die "missing $l (build libadrenotools first: docs/android/probes-gpu.md, \"Loading the drivers\")"; EXTRA+=("$l"); done
    if [ -n "${VALIDATION_LAYER:-}" ]; then [ -f "$VALIDATION_LAYER" ] || die "missing $VALIDATION_LAYER"; EXTRA+=("$VALIDATION_LAYER"); fi
fi
"$HERE/../cmake/build.sh" "$GAME" --gpu $GPU ${BARGS[@]+"${BARGS[@]}"}
if [ -n "$OUTAPK" ]; then OUT=$(dirname "$OUTAPK"); APKN=$(basename "$OUTAPK"); else OUT=$HERE/build; fi
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd); W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
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
find "$W/classes" -name '*.class' -exec "$BT/d8" --release --min-api "$MIN_API" --lib "$ANDROID_JAR" --output "$W/apk/dex" {} +
mv "$W/apk/dex/classes.dex" "$W/apk/classes.dex"; rmdir "$W/apk/dex"
RZ=(); if [ -n "$RES" ]; then "$BT/aapt2" compile --dir "$RES" -o "$W/res.zip"; RZ=("$W/res.zip"); fi
"$BT/aapt2" link -o "$W/base.apk" --manifest "$MANIFEST" -I "$ANDROID_JAR" ${VARGS[@]+--replace-version "${VARGS[@]}"} ${RZ[@]+"${RZ[@]}"}
ZE=(classes.dex lib); if [ -n "$ASSETS" ]; then ln -s "$(cd "$ASSETS" && pwd)" "$W/apk/assets"; ZE+=(assets); fi   # zip follows the link: the files go in as assets/..., read where they are
(cd "$W/apk" && zip -q -r "$W/base.apk" "${ZE[@]}")
ZA=(-P 16); "$BT/zipalign" 2>&1 | grep -q -- '-P ' || { ZA=(-p); echo "note: $BT/zipalign has no -P (build-tools before 35): libraries aligned to 4 KB pages, enough on 4 KB-page devices only"; }
"$BT/zipalign" -f "${ZA[@]}" 4 "$W/base.apk" "$W/aligned.apk"   # 16 KB page alignment for uncompressed libraries, as the probes
"$BT/apksigner" sign --ks "$KEYSTORE" --ks-pass pass:android --ks-key-alias androiddebugkey --key-pass pass:android --out "$OUT/$APKN" "$W/aligned.apk"
rm -f "$OUT/$APKN.idsig"
echo "built $OUT/$APKN ($("$BT/aapt2" dump packagename "$OUT/$APKN" 2>/dev/null), game $GAME, GPU backend $GPU, $(du -h "$OUT/$APKN" | cut -f1)); lib/arm64-v8a: $(cd "$W/apk/lib/arm64-v8a" && ls | tr '\n' ' ')"
