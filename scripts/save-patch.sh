#!/usr/bin/env bash
# Commit the work in ~/botw-build/src/recomp (branch botw) and export it as patches/botw-ps4.patch against the
# upstream commit in patches/BASE.txt. private/ (the game) is gitignored upstream and never part of it.
#   save-patch.sh "commit message"
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="${RECOMP_SRC:-$HOME/botw-build/src/recomp}"
BASE="$(cat "$HERE/../patches/BASE.txt")"
git -C "$SRC" add -A
git -C "$SRC" diff --cached --quiet || git -C "$SRC" -c user.name=BOTW -c user.email=botw@localhost commit -q -m "${1:-BOTW port work}"
git -C "$SRC" diff "$BASE" HEAD > "$HERE/../patches/botw-ps4.patch"
echo "patches/botw-ps4.patch: $(grep -c '^diff --git' "$HERE/../patches/botw-ps4.patch") files"
