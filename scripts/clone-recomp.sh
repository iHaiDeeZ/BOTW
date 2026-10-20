#!/usr/bin/env bash
# BelmanteGu's Switch recomp (github.com/BelmanteGu/SuperMarioWonderRecomp: the recompiler, the shared runtime with NVN
# on Vulkan, and SMW's PS4 port) at the commit in patches/BASE.txt into ~/botw-build/src/recomp, with this port
# (patches/botw-ps4.patch: SMW's PS4 work + games/botw) applied on a branch "botw".
#   clone-recomp.sh
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="${RECOMP_SRC:-$HOME/botw-build/src/recomp}"
BASE="$(cat "$HERE/../patches/BASE.txt")"
if [ ! -d "$SRC/.git" ]; then
    git clone https://github.com/BelmanteGu/SuperMarioWonderRecomp "$SRC"
fi
git -C "$SRC" fetch -q origin
if ! git -C "$SRC" rev-parse -q --verify botw >/dev/null; then
    git -C "$SRC" checkout -q "$BASE"
    git -C "$SRC" checkout -q -b botw
    git -C "$SRC" apply --index "$HERE/../patches/botw-ps4.patch"
    git -C "$SRC" -c user.name=BOTW -c user.email=botw@localhost commit -q -m "BOTW PS4 port (patches/botw-ps4.patch)"
fi
git -C "$SRC" log -2 --oneline
