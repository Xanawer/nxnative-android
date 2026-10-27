#!/bin/bash
# Runs the P0.7 GPU probe for one driver and copies its results to <results>/<driver>/<exe|app>/.
# Usage: run_gpu.sh app|exe stock|842|turnip [extra probe arguments, e.g. --heavy, --tests a,d]
#   RUN_TAG=<name> puts the results in <results>/<driver>/<mode>/<name>/ instead; with --tests and no RUN_TAG, <name> is the test list
#   (e.g. --tests e, run in a process of its own, goes to <driver>/app/e/).
#   app: installs gpu.apk (org.nxnative.probe.gpu), pushes and unzips the driver package into the app's files/drivers/<driver>/,
#        clears files/res, starts the NativeActivity, and waits for files/done or for the process to die.
#   exe: /data/local/tmp/nxprobe-gpu/ (the executable, the hook libraries beside it, drivers/<driver>/).
# Driver packages (never in the repository): ZIP_842 (default /tmp/qcom_drv/v842/8Elite2-842.6.zip), ZIP_TURNIP (/tmp/qcom_drv/turnip_a8xx/turnip_a8xx.zip).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd); . "$HERE/../common/env.sh"
OUT=${PROBE_OUT:-$HOME/dm-work/android/probe-build/gpu}
RES=${PROBE_RESULTS:-$HOME/dm-work/android/probe-results/gpu}
PKG=org.nxnative.probe.gpu
TIMEOUT=${PROBE_TIMEOUT:-900}
MODE=$1 DRV=$2; shift 2
ZIP=""
case $DRV in
    stock)  DARGS=(--label stock --load system --expect-lib "${STOCK_LIB:-vulkan.adreno.so}" --expect-id 8 --expect-info "${STOCK_INFO:-E031.47}") ;;   # another phone's system driver: set STOCK_INFO / STOCK_LIB
    842)    DARGS=(--label 842 --load adrenotools --drvname vulkan.ad07xx.so --expect-lib vulkan.ad07xx.so --expect-id 8 --expect-info "${Q842_INFO:-E031.50}")
            ZIP=${ZIP_842:-/tmp/qcom_drv/v842/8Elite2-842.6.zip} ;;
    turnip) DARGS=(--label turnip --load adrenotools --drvname vulkan.ad08xx.so --expect-lib vulkan.ad08xx.so --expect-id 18)
            ZIP=${ZIP_TURNIP:-/tmp/qcom_drv/turnip_a8xx/turnip_a8xx.zip} ;;
    *) die "driver must be stock, 842 or turnip" ;;
esac
# a subset run (--tests) goes to its own subdirectory unless RUN_TAG says otherwise, so it never replaces a full run's results
if [ -z "${RUN_TAG:-}" ]; then prev=""; for a in "$@"; do [ "$prev" = --tests ] && RUN_TAG=${a//,/-}; prev=$a; done; fi
DEST=$RES/$DRV/$MODE${RUN_TAG:+/$RUN_TAG}
if [ -n "${RUN_TAG:-}" ]; then rm -rf "$DEST"; else rm -f "$DEST"/*.txt "$DEST"/*.tsv 2>/dev/null || true; fi; mkdir -p "$DEST"
T0=$("$ADB" shell "date +'%m-%d %H:%M:%S.000'" | tr -d '\r')

if [ "$MODE" = exe ]; then
    D=/data/local/tmp/nxprobe-gpu
    "$ADB" shell am force-stop "$PKG"   # our own app, if a previous app run left it alive
    "$ADB" shell "rm -rf $D/out && mkdir -p $D/out $D/drivers"
    "$ADB" push -q "$OUT/gpu" "$D/gpu" >/dev/null
    for l in "$OUT"/lib/*.so; do "$ADB" push -q "$l" "$D/" >/dev/null; done
    if [ -n "$ZIP" ]; then
        "$ADB" push -q "$ZIP" "$D/drivers/$DRV.zip" >/dev/null
        "$ADB" shell "cd $D/drivers && rm -rf $DRV && mkdir $DRV && cd $DRV && unzip -o -q ../$DRV.zip"
        DARGS+=(--drvdir "$D/drivers/$DRV/")
    fi
    set +e; "$ADB" shell "cd $D && chmod 755 ./gpu && ./gpu $D/out ${DARGS[*]} $*"; rc=$?; set -e
    "$ADB" pull -q "$D/out/res/." "$DEST/" >/dev/null 2>&1 || true
    echo "$rc" > "$DEST/exit_code.txt"
    "$ADB" logcat -d -T "$T0" 2>/dev/null | grep -E "nxprobe|hook_impl|linker|DEBUG|libc|vulkan|adreno|Adreno|turnip|TU:|MESA" > "$DEST/logcat.txt" || true
    echo "exe exit $rc; results in $DEST"; exit 0
fi
[ "$MODE" = app ] || die "mode must be exe or app"
others=$("$ADB" shell "ps -A -o NAME" | tr -d '\r' | grep '^org.nxnative.probe' | grep -v "^$PKG\$" || true)
[ -z "$others" ] || echo "note: other probe processes running: $others"
"$ADB" install -r -t "$OUT/gpu.apk" >/dev/null
"$ADB" shell am force-stop "$PKG"
"$ADB" shell "run-as $PKG sh -c 'mkdir -p files && rm -rf files/res files/done files/args.txt'"
if [ -n "$ZIP" ]; then
    T=/data/local/tmp/nxprobe-gpu-push; "$ADB" shell "mkdir -p $T"; "$ADB" push -q "$ZIP" "$T/$DRV.zip" >/dev/null
    "$ADB" shell "run-as $PKG sh -c 'rm -rf files/drivers/$DRV && mkdir -p files/drivers/$DRV && cp $T/$DRV.zip files/drivers/$DRV.zip && cd files/drivers/$DRV && unzip -o -q ../$DRV.zip && rm ../$DRV.zip'"
    "$ADB" shell "rm -rf $T"
    DARGS+=(--drvdir "drivers/$DRV/")   # relative: the probe resolves it against its files directory
fi
printf '%s\n' "${DARGS[@]}" "$@" | "$ADB" shell "run-as $PKG sh -c 'cat > files/args.txt'"
"$ADB" shell am start -n "$PKG/android.app.NativeActivity" >/dev/null
t=0; pid=""; state=timeout
while [ $t -lt "$TIMEOUT" ]; do
    if "$ADB" shell "run-as $PKG test -f files/done" 2>/dev/null; then state=done; break; fi
    p=$("$ADB" shell pidof "$PKG" | tr -d '\r' || true)
    if [ -n "$p" ]; then pid=$p; elif [ $t -gt 8 ]; then state=died; break; fi
    sleep 1; t=$((t + 1))
done
rc=$("$ADB" shell "run-as $PKG cat files/done 2>/dev/null" | tr -d '\r' || true)
for f in $("$ADB" shell "run-as $PKG ls files/res 2>/dev/null" | tr -d '\r'); do
    "$ADB" exec-out "run-as $PKG cat files/res/$f" > "$DEST/$f"
done
echo "state=$state rc=${rc:-none} pid=${pid:-?}" > "$DEST/exit_code.txt"
"$ADB" shell am force-stop "$PKG"   # the NativeActivity stays alive after the probe; the drivers keep freed memory until the process exits
if [ -n "$pid" ]; then "$ADB" logcat -d -T "$T0" --pid="$pid" > "$DEST/logcat.txt" 2>/dev/null || true; fi
"$ADB" logcat -d -T "$T0" 2>/dev/null | grep -E "DEBUG|libc|$PKG|hook_impl" >> "$DEST/logcat.txt" || true
echo "app $state rc=${rc:-none}; results in $DEST"
