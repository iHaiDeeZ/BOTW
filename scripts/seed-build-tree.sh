#!/usr/bin/env bash
# One-time: create ~/botw-build from copies of SMW's build tree (~/smw-build: the libc++ 21 sysroot with the libc
# overlay, the RADV/GNM Mesa build from Crema, Vulkan-Headers 1.4.357, the fixed PkgTool and the .NET SDK + Ryujinx
# sources the shader step uses), so this port never writes into ~/smw-build. Then ~/botw -> this repo.
set -euo pipefail
SMW="${SMW:-$HOME/smw-build}"
PSB="${PSB:-$HOME/botw-build}"
mkdir -p "$PSB/src" "$PSB/build" "$PSB/out" "$PSB/logs" "$PSB/oelf" "$PSB/third_party" "$PSB/tools"
[ -d "$PSB/sysroot" ] || cp -a "$SMW/sysroot" "$PSB/sysroot"
[ -d "$PSB/host-compat" ] || cp -a "$SMW/host-compat" "$PSB/host-compat"
# Mesa: the whole build directory (its static libraries are thin archives). Crema's, the newest PS4 RADV (it has the
# Liverpool vertex fetch fix; without it Wonder's lit models drew black).
[ -d "$PSB/build/mesa" ] || cp -a "$SMW/build/mesa" "$PSB/build/mesa"
PSB="$PSB" bash "$(dirname "$0")/../toolchain/botw-overlay.sh"
VKH="$PSB/third_party/Vulkan-Headers-1.4.357"
[ -d "$VKH" ] || cp -a "$SMW/third_party/Vulkan-Headers-1.4.357" "$VKH"
[ -d "$PSB/tools/pkgtool-fixed" ] || cp -a "$SMW/tools/pkgtool-fixed" "$PSB/tools/pkgtool-fixed"
# .NET 10 SDK and the Ryujinx (Ryubing) sources: the shader translator (scripts/prepare-game.sh) and PkgTool.
T="$PSB/src/recomp/private/third_party"
if [ -d "$PSB/src/recomp" ] && [ ! -d "$T/dotnet" ]; then
    mkdir -p "$T"
    cp -a "$SMW/src/wonder/private/third_party/." "$T/"
fi
# Space-free path to this repo: toolchain flags (linker script paths) break on "PS4 Projects".
REPO="$(cd "$(dirname "$0")/.." && pwd)"
[ -e "$HOME/botw" ] || ln -s "$REPO" "$HOME/botw"
du -sh "$PSB"/* 2>/dev/null
find "$PSB/build/mesa" -name '*.a' | wc -l
