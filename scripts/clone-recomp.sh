#!/usr/bin/env bash
# BelmanteGu's Switch recomp (the recompiler, the shared runtime with NVN on Vulkan, and SMW's PS4 port; MIT) with this
# port on top, into ~/botw-build/src/recomp on a branch "botw". BelmanteGu's repository is gone from GitHub (404 since
# 2026-10-24), so its history with this port's commits is this repository's `recomp` branch.
# patches/botw-ps4.patch is the same port as one diff against the commit in patches/BASE.txt (BelmanteGu's).
#   clone-recomp.sh
set -euo pipefail
SRC="${RECOMP_SRC:-$HOME/botw-build/src/recomp}"
if [ ! -d "$SRC/.git" ]; then
    git clone --branch recomp https://github.com/iHaiDeeZ/BOTW "$SRC"
    git -C "$SRC" checkout -q -b botw
fi
git -C "$SRC" log -2 --oneline
