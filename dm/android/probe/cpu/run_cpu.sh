#!/bin/bash
# Build and run the CPU Phase 0 probes (docs/android/PLAN.md P0.1-P0.6).
# Build outputs: ~/dm-work/android/probe-build/cpu/. Raw results: ~/dm-work/android/probe-results/cpu/<probe>/{exe,app}/.
# Packages: org.nxnative.probe.cpu (tagging off) and org.nxnative.probe.cputag (tagging on).
#
# Usage:
#   run_cpu.sh build            # build exe + both APKs + libblob.so + libie.so
#   run_cpu.sh <probe> [args]   # run one probe in BOTH contexts (probe = p01..p06; p03 takes a variant)
#   run_cpu.sh all              # build then run every probe in both contexts
# Individual run examples (also in docs/android/probes-cpu.md):
#   run_cpu.sh p02
#   run_cpu.sh p03 alt 2 ; run_cpu.sh p03 noalt 2
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
COMMON=$(cd "$HERE/../common" && pwd)
. "$COMMON/env.sh"

BUILD=${CPU_BUILD:-$HOME/dm-work/android/probe-build/cpu}
RES=${CPU_RESULTS:-$HOME/dm-work/android/probe-results/cpu}
PKG=org.nxnative.probe.cpu
PKGTAG=org.nxnative.probe.cputag
SRC=("$HERE/main.c" "$HERE/p01_execmem.c" "$HERE/p02_slots.c" "$HERE/p03_scratch.c" \
     "$HERE/p04_tagging.c" "$HERE/p05_x18tls.c" "$HERE/p06_sysreg.c" \
     "$HERE/blob_fn.S" "$HERE/scratch_loop.S")

mkdir -p "$BUILD" "$RES"

do_build() {
    # libblob.so: a raw aarch64 code blob (not an ELF), the two functions of blob_fn.S concatenated.
    "$CC" -c "$HERE/blob_fn.S" -o "$BUILD/blob_fn.o"
    "$TC/llvm-objcopy" -O binary --only-section=.text "$BUILD/blob_fn.o" "$BUILD/libblob.so"
    echo "libblob.so: $(wc -c < "$BUILD/libblob.so") bytes (raw code)"
    # libieonly.so (initial-exec only) and libgd.so (global-dynamic only): the P0.2(b) dlopen tests.
    "$CC" $CFLAGS_PROBE -fPIC -shared -Wl,-soname,libieonly.so -o "$BUILD/libieonly.so" "$HERE/libieonly.c"
    "$CC" $CFLAGS_PROBE -fPIC -shared -Wl,-soname,libgd.so -o "$BUILD/libgd.so" "$HERE/libgd.c"
    # the probe: executable + APK (tagging off), and a second APK (tagging on). The exe is shared.
    "$COMMON/build_probe.sh" cpuprobe "$PKG" "$BUILD" --tagging off \
        --extra-lib "$BUILD/libblob.so" --extra-lib "$BUILD/libieonly.so" --extra-lib "$BUILD/libgd.so" -- "${SRC[@]}"
    "$COMMON/build_probe.sh" cpuprobe_tag "$PKGTAG" "$BUILD" --tagging on \
        --extra-lib "$BUILD/libblob.so" --extra-lib "$BUILD/libieonly.so" --extra-lib "$BUILD/libgd.so" -- "${SRC[@]}"
    echo "build done in $BUILD"
}

# run the executable with libblob.so + libie.so beside it so dladdr/proc-self-exe finds them
run_exe() {
    local probe=$1; shift
    local D=/data/local/tmp/nxprobe-cpu-exe
    "$ADB" shell "rm -rf $D && mkdir -p $D/out" >/dev/null
    "$ADB" push -q "$BUILD/cpuprobe" "$D/cpuprobe" >/dev/null
    "$ADB" push -q "$BUILD/libblob.so" "$D/libblob.so" >/dev/null
    "$ADB" push -q "$BUILD/libieonly.so" "$D/libieonly.so" >/dev/null
    "$ADB" push -q "$BUILD/libgd.so" "$D/libgd.so" >/dev/null
    set +e
    "$ADB" shell "cd $D && chmod 755 ./cpuprobe && ./cpuprobe $D/out $probe $*"
    local rc=$?
    set -e
    mkdir -p "$RES/$probe/exe"
    "$ADB" pull -q "$D/out/." "$RES/$probe/exe/" >/dev/null || true
    echo "  exe $probe exit=$rc -> $RES/$probe/exe"
}

run_app() {
    local probe=$1; shift
    "$COMMON/run_probe.sh" app cpuprobe "$PKG" "$BUILD" "$RES/$probe" "$probe" "$@"
}
run_app_tag() {
    local probe=$1; shift
    "$COMMON/run_probe.sh" app cpuprobe_tag "$PKGTAG" "$BUILD" "$RES/${probe}_tag" "$probe" "$@"
}

run_probe() {
    local probe=$1; shift
    echo "== $probe $* =="
    run_exe "$probe" "$@"
    run_app "$probe" "$@"
}

case "${1:-all}" in
    build) do_build ;;
    p01|p02|p05|p06) run_probe "$1" ;;
    p03) shift; run_probe p03 "${1:-alt}" "${2:-2}" ;;
    p04)
        run_exe p04
        run_app p04              # tagging off (.cpu)
        run_app_tag p04          # tagging on  (.cputag)
        ;;
    all)
        do_build
        run_probe p01
        run_probe p02
        run_probe p03 alt 2
        run_probe p03 noalt 2
        run_exe p04; run_app p04; run_app_tag p04
        run_probe p05
        run_probe p06
        ;;
    *) die "unknown target $1" ;;
esac
