#!/bin/bash
# Builds the P0.8 shader probe through the P0.7 GPU harness (dm/android/probe/gpu/build_probe_gpu.sh, probe_glue_gpu.c and vk_common.c, used unchanged):
#   <out>/shader (adb executable, hook libraries in <out>/lib) and <out>/shader.apk (org.nxnative.probe.shader).
#   build.sh mac: <out>/shader-mac, a macOS executable for KosmicKrisp and the validation layer (Homebrew's loader, dlopen'd at run time).
# Needs glslangValidator, spirv-val (Homebrew), and for Android libadrenotools built as for P0.7 (ADRENOTOOLS_BUILD, default ~/dm-work/android/src/libadrenotools-build).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd); GPU=$HERE/../gpu; . "$HERE/../common/env.sh"
OUT=${PROBE_OUT:-$HOME/dm-work/android/probe-build/shader}
REPO=$(cd "$HERE/../../../.." && pwd)
VKH=$REPO/dm/third_party/vulkan-headers/include
GLSLANG=${GLSLANG:-/opt/homebrew/bin/glslangValidator}
SPIRV_VAL=${SPIRV_VAL:-/opt/homebrew/bin/spirv-val}
GEN=$OUT/gen; mkdir -p "$GEN"

# T3 / T4 shaders; spirv-val on each
sh_hdr() {   # <stage> <name> <source> <target env> [defines...]
    local st=$1 name=$2 src=$3 env=$4; shift 4
    "$GLSLANG" -V --target-env "$env" -S "$st" "$@" -o "$GEN/$name.spv" "$src" >"$GEN/$name.log" 2>&1 || { cat "$GEN/$name.log"; die "glslang $name"; }
    "$SPIRV_VAL" --target-env "$env" "$GEN/$name.spv"
    "$GLSLANG" -V --target-env "$env" -S "$st" "$@" --vn "${name}_spv" -o "$GEN/${name}_spv.h" "$src" >"$GEN/$name.log" 2>&1 || { cat "$GEN/$name.log"; die "glslang $name"; }
}
sh_hdr vert ydir_vert "$HERE/ydir.vert" vulkan1.1
sh_hdr frag ydir_frag "$HERE/ydir.frag" vulkan1.1
sh_hdr comp nanmm_f "$HERE/nanmm.comp" vulkan1.2
sh_hdr comp nanmm_n "$HERE/nanmm.comp" vulkan1.2 -DNAN_OPS
sh_hdr comp nanmm_fp "$HERE/nanmm.comp" vulkan1.2 -DPRESERVE
sh_hdr comp nanmm_np "$HERE/nanmm.comp" vulkan1.2 -DNAN_OPS -DPRESERVE
grep -q SignedZeroInfNanPreserve <(/opt/homebrew/bin/spirv-dis "$GEN/nanmm_np.spv") || die "nanmm_np lacks SignedZeroInfNanPreserve"

if [ "${1:-}" = mac ]; then
    clang -O1 -g -Wall -Wno-unused-function -DPROBE_EXE -I"$VKH" -I"$GEN" -I"$GPU" -o "$OUT/shader-mac" \
        "$GPU/probe_glue_gpu.c" "$HERE/shader_probe.c" "$GPU/vk_common.c" -ldl -lm
    echo "built $OUT/shader-mac"; exit 0
fi

AT=${ADRENOTOOLS_BUILD:-$HOME/dm-work/android/src/libadrenotools-build}
LIBS=("$AT/libadrenotools.so" "$AT/src/hook/libhook_impl.so" "$AT/src/hook/libmain_hook.so" "$AT/src/hook/libfile_redirect_hook.so" "$AT/src/hook/libgsl_alloc_hook.so")
EXTRA=(); for l in "${LIBS[@]}"; do [ -f "$l" ] || die "missing $l (build libadrenotools first)"; EXTRA+=(--extra-lib "$l"); done
"$GPU/build_probe_gpu.sh" shader org.nxnative.probe.shader "$OUT" "${EXTRA[@]}" -- "$HERE/shader_probe.c" "$GPU/vk_common.c" -- -I"$VKH" -I"$GEN" -I"$GPU"
mkdir -p "$OUT/lib"; cp "${LIBS[@]}" "$OUT/lib/"
echo "hook libraries for the executable in $OUT/lib"
