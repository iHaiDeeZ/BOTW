#!/usr/bin/env bash
# Configure the recomp (branch botw: patches/botw-ps4.patch, scripts/clone-recomp.sh) for the PS4 as BOTW.
#   configure-botw.sh [extra cmake args...]
# The generated code (python -m recomp, scripts/prepare-game.sh) comes from BOTW_GEN, default
# ~/botw-build/src/recomp/private/botw/gen; with BOTW_GEN pointing at scripts/make-stub-gen.sh's output only the
# runtime is real. Run through ~/botw (no space in the path).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
source "$HERE/ps4-env.sh"
SRC="${RECOMP_SRC:-$PS4_BUILD_ROOT/src/recomp}"
BUILD="${BOTW_BUILD:-$PS4_BUILD_ROOT/build/botw}"
GEN="${BOTW_GEN:-$SRC/private/botw/gen}"
[ -d "$GEN/main" ] || { echo "no generated code in $GEN (scripts/prepare-game.sh, or BOTW_GEN=\$(scripts/make-stub-gen.sh))" >&2; exit 1; }
ps4_cmake -S "$SRC" -B "$BUILD" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$HERE/../toolchain/ps4-botw.cmake" \
    "-DCMAKE_PROJECT_SwitchGamesRecomp_INCLUDE=$HERE/../toolchain/ps4-love-style.cmake" \
    -DPS4_PAID="${PS4_PAID:-0x3800000000000035}" \
    -DPS4_MESA_BUILD="$PS4_BUILD_ROOT/build/mesa" \
    -DNX_PS4=ON -DNX_GAME=botw -DNX_PS4_APP=BOTW -DNX_GEN_DIR="$GEN" \
    -DNX_VULKAN_INCLUDE="$PS4_BUILD_ROOT/third_party/Vulkan-Headers-1.4.357/include" \
    "$@"
