#!/bin/bash
# Build the Android host (docs/android/PLAN.md 1.1): build.sh <GAME> [--gpu null|vkn] [--tests] [--clean]: dmhost (the adb loop) and libdmhost.so (the app, dm/android/app/build_app.sh)
#   GAME: base game title ID (dm/games/<id>/), as make GAME= for the Mac host
#   --tests: also the unit tests that run on the phone (t_hmap t_cover t_pace t_fstep t_dirty_slots t_dirty_race t_dirty_fail t_sig_guard t_tex t_sha256)
#   --gpu: the GPU backend (CMakeLists.txt GPU_BACKEND): null (the default) or vkn, the Vulkan backend, built in build-vkn/ so neither build reconfigures the other
#   --clean: start from an empty build directory
# Output: dm/android/cmake/build/dmhost and libdmhost.so (build-vkn/ with --gpu vkn) (arm64-v8a, android-35), the tests beside them. ANDROID_HOME / ANDROID_NDK_HOME override the SDK / NDK locations;
# cmake comes from PATH (Homebrew) or the SDK, ninja from PATH or the SDK's cmake; python3 from PATH runs the generators (game.py, gen_protos.py)
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ANDROID_HOME=${ANDROID_HOME:-$HOME/Library/Android/sdk}
NDK=${ANDROID_NDK_HOME:-$ANDROID_HOME/ndk/27.1.12297006}
API=35
die() { echo "error: $*" >&2; exit 1; }
GAME=${1:-}; [ -n "$GAME" ] && [ "${GAME#-}" = "$GAME" ] || die "usage: build.sh <GAME> [--gpu null|vkn] [--tests] [--clean]"; shift
TESTS=0 CLEAN=0 GPU=null
while [ $# -gt 0 ]; do case $1 in --tests) TESTS=1; shift ;; --clean) CLEAN=1; shift ;; --gpu) GPU=${2:-}; shift 2 || die "--gpu takes null or vkn" ;; *) die "unknown option $1" ;; esac; done
case $GPU in null|vkn) ;; *) die "--gpu takes null or vkn, not '$GPU'" ;; esac
[ -f "$NDK/build/cmake/android.toolchain.cmake" ] || die "no NDK at $NDK"
SDKCM=$(ls -d "$ANDROID_HOME"/cmake/*/bin 2>/dev/null | sort -V | tail -1 || true)
CMAKE=$(command -v cmake || echo "$SDKCM/cmake"); [ -x "$CMAKE" ] || die "no cmake (brew install cmake, or the SDK's cmake package)"
NINJA=$(command -v ninja || echo "$SDKCM/ninja"); [ -x "$NINJA" ] || die "no ninja (brew install ninja, or the SDK's cmake package)"
PY=$(command -v python3) || die "no python3"
B="$HERE/build"; [ $GPU = null ] || B="$HERE/build-$GPU"
[ $CLEAN = 1 ] && rm -rf "$B"
# A build directory made for another game is reconfigured (the cache keeps GAME)
"$CMAKE" -Wno-deprecated -S "$HERE" -B "$B" -G Ninja -DCMAKE_MAKE_PROGRAM="$NINJA" -DCMAKE_BUILD_TYPE= \
    -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-$API \
    -DPython3_EXECUTABLE="$PY" -DGAME="$GAME" -DGPU_BACKEND=$GPU >/dev/null
"$CMAKE" --build "$B" --target dmhost dmhost_lib
[ $TESTS = 1 ] && "$CMAKE" --build "$B" --target tests
file "$B/dmhost" "$B/libdmhost.so"
