#!/bin/bash
# Builds the P0.7 GPU probe: <out>/gpu (adb executable, hook libraries beside it) and <out>/gpu.apk (org.nxnative.probe.gpu).
# Needs libadrenotools built for arm64-v8a (see docs/android/probes-gpu.md, "Loading the drivers"):
#   ADRENOTOOLS_BUILD (default ~/dm-work/android/src/libadrenotools-build) holding libadrenotools.so and src/hook/lib*_hook.so, libhook_impl.so
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd); COMMON=$HERE/../common; . "$COMMON/env.sh"
OUT=${PROBE_OUT:-$HOME/dm-work/android/probe-build/gpu}
AT=${ADRENOTOOLS_BUILD:-$HOME/dm-work/android/src/libadrenotools-build}
REPO=$(cd "$HERE/../../../.." && pwd)
VKH=$REPO/dm/third_party/vulkan-headers/include
GLSLANG=${GLSLANG:-/opt/homebrew/bin/glslangValidator}
GEN=$OUT/gen; mkdir -p "$GEN"

python3 -I "$HERE/gen_vkdump.py" "$VKH/vulkan/vulkan_core.h" "$GEN/vkdump_gen.h"
"$GLSLANG" -V --target-env vulkan1.1 -S comp --vn coh_desc_spv -o "$GEN/coh_desc_spv.h" "$HERE/coherence.comp" >/dev/null
"$GLSLANG" -V --target-env vulkan1.2 -S comp -DUSE_BDA --vn coh_bda_spv -o "$GEN/coh_bda_spv.h" "$HERE/coherence.comp" >/dev/null
"$GLSLANG" -V --target-env vulkan1.1 -S comp --vn diag_desc_push_spv -o "$GEN/diag_desc_push_spv.h" "$HERE/diag.comp" >/dev/null
"$GLSLANG" -V --target-env vulkan1.2 -S comp -DUSE_BDA --vn diag_bda_push_spv -o "$GEN/diag_bda_push_spv.h" "$HERE/diag.comp" >/dev/null
"$GLSLANG" -V --target-env vulkan1.1 -S comp -DPARAMS_BUF --vn diag_desc_buf_spv -o "$GEN/diag_desc_buf_spv.h" "$HERE/diag.comp" >/dev/null
"$GLSLANG" -V --target-env vulkan1.2 -S comp -DUSE_BDA -DPARAMS_BUF --vn diag_bda_buf_spv -o "$GEN/diag_bda_buf_spv.h" "$HERE/diag.comp" >/dev/null

LIBS=("$AT/libadrenotools.so" "$AT/src/hook/libhook_impl.so" "$AT/src/hook/libmain_hook.so" "$AT/src/hook/libfile_redirect_hook.so" "$AT/src/hook/libgsl_alloc_hook.so")
EXTRA=(); for l in "${LIBS[@]}"; do [ -f "$l" ] || die "missing $l (build libadrenotools first)"; EXTRA+=(--extra-lib "$l"); done
"$HERE/build_probe_gpu.sh" gpu org.nxnative.probe.gpu "$OUT" "${EXTRA[@]}" -- "$HERE/gpu_probe.c" "$HERE/vk_common.c" -- -I"$VKH" -I"$GEN" -I"$HERE" -lnativewindow -Wl,-z,max-page-size=16384   # 16 KB LOAD alignment (Android 16 page-size compat dialog otherwise)
mkdir -p "$OUT/lib"; cp "${LIBS[@]}" "$OUT/lib/"   # for the adb executable: pushed beside it
echo "hook libraries for the executable in $OUT/lib"
