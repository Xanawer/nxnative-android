#!/bin/bash
# Copy of ../common/build_probe.sh for the GPU probe: uses probe_glue_gpu.c (screen kept on, shown over the lock screen) and
# marks the activity showWhenLocked / turnScreenOn.
# Build one probe both ways: <out>/<name> (adb executable) and <out>/<name>.apk (NativeActivity, no Java code).
# Usage: build_probe.sh <name> <package> <out dir> [--tagging on|off] [--extra-lib <file.so>]... -- <sources...> [-- <extra clang args>]
#   --tagging: android:allowNativeHeapPointerTagging (default off, as the port will ship; P0.4 builds both)
#   --extra-lib: another .so to put in lib/arm64-v8a (libadrenotools' hook libraries, for example)
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd); . "$HERE/../common/env.sh"
NAME=$1 PKG=$2 OUT=$3; shift 3
TAG=false; EXTRA=()
while [ $# -gt 0 ] && [ "$1" != "--" ]; do
    case $1 in
        --tagging) [ "$2" = on ] && TAG=true || TAG=false; shift 2 ;;
        --extra-lib) EXTRA+=("$2"); shift 2 ;;
        *) die "unknown option $1" ;;
    esac
done
[ "${1:-}" = "--" ] || die "missing -- before the sources"; shift
SRC=(); while [ $# -gt 0 ] && [ "$1" != "--" ]; do SRC+=("$1"); shift; done
[ "${1:-}" = "--" ] && shift
LDX=("$@")
mkdir -p "$OUT"; W=$(mktemp -d); trap 'rm -rf "$W"' EXIT

# the executable (adb shell context)
"$CC" $CFLAGS_PROBE -DPROBE_EXE -o "$OUT/$NAME" "$HERE/probe_glue_gpu.c" "${SRC[@]}" -llog -ldl -lm ${LDX[@]+"${LDX[@]}"}
# the NativeActivity library (untrusted_app context)
mkdir -p "$W/apk/lib/arm64-v8a"
"$CC" $CFLAGS_PROBE -fPIC -shared -Wl,-soname,libprobe.so -o "$W/apk/lib/arm64-v8a/libprobe.so" "$HERE/probe_glue_gpu.c" "${SRC[@]}" -landroid -llog -ldl -lm ${LDX[@]+"${LDX[@]}"}
for e in ${EXTRA[@]+"${EXTRA[@]}"}; do cp "$e" "$W/apk/lib/arm64-v8a/"; done

cat > "$W/AndroidManifest.xml" <<EOF
<?xml version="1.0" encoding="utf-8"?>
<manifest xmlns:android="http://schemas.android.com/apk/res/android" package="$PKG" android:versionCode="1" android:versionName="1">
    <uses-sdk android:minSdkVersion="$API" android:targetSdkVersion="36"/>
    <application android:label="$NAME" android:hasCode="false" android:debuggable="true" android:extractNativeLibs="true"
                 android:allowNativeHeapPointerTagging="$TAG">
        <activity android:name="android.app.NativeActivity" android:exported="true" android:showWhenLocked="true" android:turnScreenOn="true" android:configChanges="orientation|screenSize|keyboardHidden">
            <meta-data android:name="android.app.lib_name" android:value="probe"/>
            <intent-filter><action android:name="android.intent.action.MAIN"/><category android:name="android.intent.category.LAUNCHER"/></intent-filter>
        </activity>
    </application>
</manifest>
EOF
"$BT/aapt2" link -o "$W/base.apk" --manifest "$W/AndroidManifest.xml" -I "$ANDROID_JAR" 2>&1 | grep -v -E '^$' || true
(cd "$W/apk" && zip -q -r "$W/base.apk" lib)
"$BT/zipalign" -f -P 16 4 "$W/base.apk" "$W/aligned.apk"
"$BT/apksigner" sign --ks "$KEYSTORE" --ks-pass pass:android --ks-key-alias androiddebugkey --key-pass pass:android --out "$OUT/$NAME.apk" "$W/aligned.apk"
rm -f "$OUT/$NAME.apk.idsig"
echo "built $OUT/$NAME and $OUT/$NAME.apk ($PKG, tagging $TAG)"
