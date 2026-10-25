#!/bin/bash
# Run a probe built by build_probe.sh on the connected phone and copy its results to <results dir>/<exe|app>/.
# Usage: run_probe.sh exe <name> <out dir> <results dir> [args...]     (adb shell: /data/local/tmp/nxprobe-<name>/)
#        run_probe.sh app <name> <package> <out dir> <results dir> [--push <local file>]... [args...]
#          The APK is installed (replacing an older build), its files directory gets args.txt and the pushed files, the activity is started,
#          and the script waits for files/done (timeout PROBE_TIMEOUT, default 600 s). It never clears logcat or touches other packages.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd); . "$HERE/env.sh"
MODE=$1; shift
TIMEOUT=${PROBE_TIMEOUT:-600}
if [ "$MODE" = exe ]; then
    NAME=$1 OUT=$2 RES=$3; shift 3
    D=/data/local/tmp/nxprobe-$NAME
    "$ADB" shell "rm -rf $D && mkdir -p $D/out"
    "$ADB" push -q "$OUT/$NAME" "$D/$NAME" >/dev/null
    set +e; "$ADB" shell "cd $D && chmod 755 ./$NAME && ./$NAME $D/out $*"; rc=$?; set -e
    mkdir -p "$RES/exe"; "$ADB" pull -q "$D/out/." "$RES/exe/" >/dev/null
    echo "exe exit $rc; results in $RES/exe"; exit 0
fi
[ "$MODE" = app ] || die "mode must be exe or app"
NAME=$1 PKG=$2 OUT=$3 RES=$4; shift 4
PUSH=(); ARGS=()
while [ $# -gt 0 ]; do case $1 in --push) PUSH+=("$2"); shift 2 ;; *) ARGS+=("$1"); shift ;; esac; done
"$ADB" install -r -t "$OUT/$NAME.apk" >/dev/null
"$ADB" shell am force-stop "$PKG"   # only this probe's own package
"$ADB" shell "run-as $PKG sh -c 'mkdir -p files && rm -f files/done files/args.txt'"
if [ ${#ARGS[@]} -gt 0 ]; then printf '%s\n' "${ARGS[@]}" | "$ADB" shell "run-as $PKG sh -c 'cat > files/args.txt'"; fi
for f in ${PUSH[@]+"${PUSH[@]}"}; do
    b=$(basename "$f"); T=/data/local/tmp/nxprobe-push-$PKG; "$ADB" shell "mkdir -p $T"; "$ADB" push -q "$f" "$T/$b" >/dev/null
    "$ADB" shell "run-as $PKG sh -c 'cp $T/$b files/$b'"; "$ADB" shell "rm -f $T/$b"
done
"$ADB" shell am start -W -n "$PKG/android.app.NativeActivity" >/dev/null
t=0; until "$ADB" shell "run-as $PKG test -f files/done" 2>/dev/null; do sleep 1; t=$((t + 1)); [ $t -lt "$TIMEOUT" ] || die "timeout waiting for $PKG files/done"; done
rc=$("$ADB" shell "run-as $PKG cat files/done" | tr -d '\r')
mkdir -p "$RES/app"
for f in $("$ADB" shell "run-as $PKG ls files" | tr -d '\r'); do
    case $f in done|args.txt) continue ;; esac
    "$ADB" shell "run-as $PKG test -f files/$f" && "$ADB" exec-out "run-as $PKG cat files/$f" > "$RES/app/$f"
done
echo "app exit $rc; results in $RES/app"
