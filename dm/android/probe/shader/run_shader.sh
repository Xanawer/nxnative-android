#!/bin/bash
# Runs the P0.8 shader probe and copies its results to <results>/<driver>/<exe|app|mac>/[<tag>/].
# Usage: run_shader.sh app|exe stock|842|turnip [probe arguments, e.g. --set orig --tests t1,t2]
#        run_shader.sh mac kk [probe arguments]           (KosmicKrisp through Homebrew's loader, with the Khronos validation layer)
#   RUN_TAG=<name>: results in <results>/<driver>/<mode>/<name>/ (default: the --set name, "stripped" without one).
#   DATA (default ~/dm-work/android/probe-build/shader/data): manifest.txt (gen_manifest.py) and <set>/<key>.spv for the --set run (stripped, orig, or vk from
#        dmsh_build_vk.py, decompressed; only that directory is pushed). Game-derived: pushed at run time,
#        never in the repository. app: tar'd to /data/local/tmp/nxprobe-shader-push and unpacked into the app's files/shader/; exe: /data/local/tmp/nxprobe-shader/shader/.
#   EXE_PREFIX (exe only): a command put before the executable, e.g. "taskset 80" to pin it to CPU 7.
#   CACHE_IN=<file>: push this VkPipelineCache data (a previous run's res/cache.bin) and run only the t2 phase "disk".
# Driver packages (never in the repository): ZIP_842 (default /tmp/qcom_drv/v842/8Elite2-842.6.zip), ZIP_TURNIP (/tmp/qcom_drv/turnip_a8xx/turnip_a8xx.zip).
# Touches only org.nxnative.probe.shader and /data/local/tmp/nxprobe-shader*.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd); . "$HERE/../common/env.sh"
OUT=${PROBE_OUT:-$HOME/dm-work/android/probe-build/shader}
RES=${PROBE_RESULTS:-$HOME/dm-work/android/probe-results/shader}
DATA=${DATA:-$OUT/data}
PKG=org.nxnative.probe.shader
TIMEOUT=${PROBE_TIMEOUT:-600}
MODE=${1:-} DRV=${2:-}; shift 2 || die "usage: run_shader.sh app|exe stock|842|turnip [args] | run_shader.sh mac kk [args]"
case $MODE/$DRV in mac/kk|app/stock|app/842|app/turnip|exe/stock|exe/842|exe/turnip) ;; *) die "mode/driver must be app|exe with stock|842|turnip, or mac with kk" ;; esac
[ -f "$DATA/manifest.txt" ] || die "no $DATA/manifest.txt (run gen_manifest.py, see docs/android/probes-shader.md)"
tag=stripped; prev=""; for a in "$@"; do [ "$prev" = --set ] && tag=$a; prev=$a; done
SETDIR=$tag; [ -d "$DATA/$SETDIR" ] || die "no $DATA/$SETDIR (the modules for --set $SETDIR)"
[ -n "${CACHE_IN:-}" ] && tag=$tag-disk
DEST=$RES/$DRV/$MODE/${RUN_TAG:-$tag}
rm -rf "$DEST"; mkdir -p "$DEST"

if [ "$MODE" = mac ]; then
    [ "$DRV" = kk ] || die "mac runs use driver kk"
    W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
    CA=(); [ -n "${CACHE_IN:-}" ] && CA=(--cache-in "$CACHE_IN")
    set +e
    VK_DRIVER_FILES=${VK_DRIVER_FILES:-/opt/homebrew/share/vulkan/icd.d/kosmickrisp_mesa_icd.aarch64.json} \
    VK_LAYER_PATH=${VK_LAYER_PATH:-/opt/homebrew/opt/vulkan-validationlayers/share/vulkan/explicit_layer.d} \
    VK_LAYER_ENABLE_MESSAGE_LIMIT=false VK_LAYER_DUPLICATE_MESSAGE_LIMIT=0 DYLD_FALLBACK_LIBRARY_PATH=${DYLD_FALLBACK_LIBRARY_PATH:-/opt/homebrew/lib} MESA_SHADER_CACHE_DIR="$W/mesa-cache" \
        "$OUT/shader-mac" "$W" --label kk --load system --expect-id 28 --dir "$DATA" --validation ${CA[@]+"${CA[@]}"} "$@" > "$DEST/stdout.txt" 2>&1
    rc=$?; set -e
    cp "$W"/res/* "$DEST/" 2>/dev/null || true
    echo "$rc" > "$DEST/exit_code.txt"
    echo "mac exit $rc; results in $DEST"; exit 0
fi

ZIP=""
case $DRV in
    stock)  DARGS=(--label stock --load system --expect-lib vulkan.adreno.so --expect-id 8 --expect-info "${STOCK_INFO:-E031.47}") ;;
    842)    DARGS=(--label 842 --load adrenotools --drvname vulkan.ad07xx.so --expect-lib vulkan.ad07xx.so --expect-id 8 --expect-info "${Q842_INFO:-E031.50}")
            ZIP=${ZIP_842:-/tmp/qcom_drv/v842/8Elite2-842.6.zip} ;;
    turnip) DARGS=(--label turnip --load adrenotools --drvname vulkan.ad08xx.so --expect-lib vulkan.ad08xx.so --expect-id 18)
            ZIP=${ZIP_TURNIP:-/tmp/qcom_drv/turnip_a8xx/turnip_a8xx.zip} ;;
    *) die "driver must be stock, 842 or turnip" ;;
esac
# CPU frequency caps: pipeline creation is CPU work, and the phone caps the clusters (e.g. while the screen is off), so every run records them
cpufreq() { "$ADB" shell "echo $1 wakefulness \$(dumpsys power | sed -n 's/.*mWakefulness=//p'); for p in /sys/devices/system/cpu/cpufreq/policy*; do echo $1 \${p##*/} cur \$(cat \$p/scaling_cur_freq) max \$(cat \$p/scaling_max_freq) hw_max \$(cat \$p/cpuinfo_max_freq); done" | tr -d '\r' >> "$DEST/cpufreq.txt" 2>/dev/null || true; }
TD=$(mktemp -d); TAR=$TD/shader.tar
# whatever happens, the game-derived tar leaves the host temp dir and the phone's push dir
cleanup() {
    rm -rf "$TD"
    "$ADB" shell "rm -rf /data/local/tmp/nxprobe-shader-push /data/local/tmp/nxprobe-shader/shader.tar /data/local/tmp/nxprobe-shader/shader /data/local/tmp/nxprobe-shader/cache_in.bin /data/local/tmp/nxprobe-shader/out/res/cache.bin" >/dev/null 2>&1 || true
    [ "$MODE" = app ] && "$ADB" shell "run-as $PKG sh -c 'rm -rf files/shader files/cache_in.bin files/res/cache.bin'" >/dev/null 2>&1 || true
}
trap cleanup EXIT
 (cd "$DATA" && COPYFILE_DISABLE=1 tar cf "$TAR" manifest.txt "$SETDIR")
T0=$("$ADB" shell "date +'%m-%d %H:%M:%S.000'" | tr -d '\r')

if [ "$MODE" = exe ]; then
    D=/data/local/tmp/nxprobe-shader
    "$ADB" shell am force-stop "$PKG"
    "$ADB" shell "rm -rf $D/out $D/shader $D/home && mkdir -p $D/out $D/drivers $D/home"
    "$ADB" push -q "$OUT/shader" "$D/shader-probe" >/dev/null
    for l in "$OUT"/lib/*.so; do "$ADB" push -q "$l" "$D/" >/dev/null; done
    "$ADB" push -q "$TAR" "$D/shader.tar" >/dev/null
    "$ADB" shell "cd $D && mkdir shader && tar xf shader.tar -C shader && rm shader.tar"
    if [ -n "$ZIP" ]; then
        "$ADB" push -q "$ZIP" "$D/drivers/$DRV.zip" >/dev/null
        "$ADB" shell "cd $D/drivers && rm -rf $DRV && mkdir $DRV && cd $DRV && unzip -o -q ../$DRV.zip && rm ../$DRV.zip"
        DARGS+=(--drvdir "$D/drivers/$DRV/")
    fi
    if [ -n "${CACHE_IN:-}" ]; then "$ADB" push -q "$CACHE_IN" "$D/cache_in.bin" >/dev/null; DARGS+=(--cache-in "$D/cache_in.bin"); fi
    cpufreq before
    set +e; "$ADB" shell "cd $D && chmod 755 ./shader-probe && HOME=$D/home TMPDIR=$D/home ${EXE_PREFIX:-} ./shader-probe $D/out ${DARGS[*]} --dir $D/shader $*"; rc=$?; set -e
    cpufreq after
    "$ADB" pull -q "$D/out/res/." "$DEST/" >/dev/null 2>&1 || true
    echo "$rc" > "$DEST/exit_code.txt"
    "$ADB" logcat -d -T "$T0" 2>/dev/null | grep -E "nxprobe|hook_impl|linker|DEBUG|libc|vulkan|adreno|Adreno|turnip|TU:|MESA" > "$DEST/logcat.txt" || true
    "$ADB" shell "ls -laR $D/home" > "$DEST/home_after.txt" 2>&1 || true
    "$ADB" shell "rm -rf $D/shader $D/out/res/cache.bin $D/cache_in.bin"
    echo "exe exit $rc; results in $DEST"; exit 0
fi
[ "$MODE" = app ] || die "mode must be exe, app or mac"
others=$("$ADB" shell "ps -A -o NAME" | tr -d '\r' | grep '^org.nxnative.probe' | grep -v "^$PKG\$" || true)
[ -z "$others" ] || echo "note: other probe processes running: $others"
# install only when the APK changed: every install can raise a Play Protect scan dialog that blocks "adb install" until someone answers it on the phone
have=$("$ADB" shell 'p=$(pm path '"$PKG"' 2>/dev/null | sed -n "s/^package://p" | head -1); [ -n "$p" ] && sha256sum "$p"' | tr -d '\r' | cut -d' ' -f1 || true)
want=$(shasum -a 256 "$OUT/shader.apk" | cut -d' ' -f1)
if [ "$have" != "$want" ]; then
    "$ADB" install -r -t "$OUT/shader.apk" >/dev/null &
    ip=$!; for _ in $(seq 120); do kill -0 $ip 2>/dev/null || break; sleep 1; done
    if kill -0 $ip 2>/dev/null; then kill $ip; die "adb install still waiting after 120 s (a Play Protect dialog on the phone?)"; fi
    wait $ip || die "adb install failed"
fi
"$ADB" shell am force-stop "$PKG"
# cold start: no results, no shader data, no caches left by an earlier run (the drivers' own caches would live in cache/ or code_cache/)
"$ADB" shell "run-as $PKG sh -c 'mkdir -p files && rm -rf files/res files/done files/args.txt files/shader files/cache_in.bin files/mesa cache/* code_cache/*'"
T=/data/local/tmp/nxprobe-shader-push; "$ADB" shell "rm -rf $T && mkdir -p $T"
"$ADB" push -q "$TAR" "$T/shader.tar" >/dev/null
"$ADB" shell "run-as $PKG sh -c 'mkdir -p files/shader && tar xf $T/shader.tar -C files/shader'"
if [ -n "$ZIP" ]; then
    "$ADB" push -q "$ZIP" "$T/$DRV.zip" >/dev/null
    "$ADB" shell "run-as $PKG sh -c 'rm -rf files/drivers/$DRV && mkdir -p files/drivers/$DRV && cp $T/$DRV.zip files/drivers/$DRV.zip && cd files/drivers/$DRV && unzip -o -q ../$DRV.zip && rm ../$DRV.zip'"
    DARGS+=(--drvdir "drivers/$DRV/")
fi
if [ -n "${CACHE_IN:-}" ]; then
    "$ADB" push -q "$CACHE_IN" "$T/cache_in.bin" >/dev/null
    "$ADB" shell "run-as $PKG sh -c 'cp $T/cache_in.bin files/cache_in.bin'"
    DARGS+=(--cache-in cache_in.bin)
fi
"$ADB" shell "rm -rf $T"
printf '%s\n' "${DARGS[@]}" --dir shader "$@" | "$ADB" shell "run-as $PKG sh -c 'cat > files/args.txt'"
cpufreq before
"$ADB" shell am start -n "$PKG/android.app.NativeActivity" >/dev/null
t=0; pid=""; state=timeout
while [ $t -lt "$TIMEOUT" ]; do
    if "$ADB" shell "run-as $PKG test -f files/done" 2>/dev/null; then state=done; break; fi
    p=$("$ADB" shell pidof "$PKG" | tr -d '\r' || true)
    if [ -n "$p" ]; then pid=$p; elif [ $t -gt 8 ]; then state=died; break; fi
    sleep 1; t=$((t + 1))
done
cpufreq after
rc=$("$ADB" shell "run-as $PKG cat files/done 2>/dev/null" | tr -d '\r' || true)
for f in $("$ADB" shell "run-as $PKG ls files/res 2>/dev/null" | tr -d '\r'); do
    "$ADB" exec-out "run-as $PKG cat files/res/$f" > "$DEST/$f"
done
echo "state=$state rc=${rc:-none} pid=${pid:-?}" > "$DEST/exit_code.txt"
"$ADB" shell am force-stop "$PKG"
"$ADB" shell "run-as $PKG sh -c 'rm -rf files/shader files/cache_in.bin files/res/cache.bin'"   # the game-derived modules and pipeline caches do not stay on the phone
"$ADB" shell "run-as $PKG sh -c 'ls -laR cache code_cache files 2>/dev/null'" > "$DEST/appdirs_after.txt" 2>&1 || true   # driver-written caches would show here
if [ -n "$pid" ]; then "$ADB" logcat -d -T "$T0" --pid="$pid" > "$DEST/logcat.txt" 2>/dev/null || true; fi
"$ADB" logcat -d -T "$T0" 2>/dev/null | grep -E "DEBUG|libc|$PKG|hook_impl" >> "$DEST/logcat.txt" || true
echo "app $state rc=${rc:-none}; results in $DEST"
