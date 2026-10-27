#!/bin/bash
# Build and run the Mac-side fake-MediaCodec test of dm/android/media (the sources unchanged, compiled where they are) with AddressSanitizer and UBSan: build.sh [--no-run]
# The decoders include dm.h, which needs the game profile header the Mac build generates: run `make GAME=<id>` in dm/host first (GAME= picks it, default 0100E65002BB8000).
# The binary goes to OUT (default ${TMPDIR:-/tmp}/t_mock, outside the work tree) and runs in OUT's directory, where it writes its stderr.log
set -e
cd "$(dirname "$0")"
W=$(cd ../../.. && pwd) GAME=${GAME:-0100E65002BB8000} OUT=${OUT:-${TMPDIR:-/tmp}/t_mock}
case "$OUT" in /*) ;; *) OUT=$PWD/$OUT ;; esac
[ -f "$W/host/gen/$GAME/game_profile.h" ] || { echo "build.sh: $W/host/gen/$GAME/game_profile.h is missing: run make GAME=$GAME in dm/host first" >&2; exit 1; }
clang -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer -Wall -Wno-unused-function -include compat.h -Iinc -I$W/android/media -I$W/host -I$W/host/gen/$GAME \
  $W/android/media/movie_mc.c $W/android/media/movie_avc_mc.c $W/android/media/movie_aac_mc.c mock_ndk.c t_mock.c -o "$OUT"
[ "$1" = --no-run ] || (cd "$(dirname "$OUT")" && "$OUT")
