#!/usr/bin/env bash
# Lays out the game data the PS4 runtime reads from /data/BOTW (scripts/prepare-game.sh's output, your own dump):
#
#   analysis/{main,sdk,subsdk*}.bin, main.npdm   the modules as loaded, the program metadata
#   romfs.pak, romfs.idx                  the RomFS (base + update, scripts/make-romfs-pack.py; or extract/romfs/...)
#   spirv/<hash>.spv, spirv/results.tsv   the translated shaders and the translator's records
#   config.txt                            NX_* switches (written if missing; see README.md)
#
#   stage-data.sh [out dir]   (default ~/botw-build/data-stage/BOTW; files are hard links where possible, no copies)
#
# Then copy the folder's contents to /data/BOTW on the console over FTP (FileZilla, or scripts/upload-data.sh from
# Windows), once; a new build of the app needs only the new package. Never share it: it is the game.
set -euo pipefail
SRC="${RECOMP_SRC:-$HOME/botw-build/src/recomp}"
G="$SRC/private/botw"
OUT="${1:-$HOME/botw-build/data-stage/BOTW}"
for f in analysis/main.bin analysis/sdk.bin analysis/main.npdm spirv/results.tsv; do
    [ -f "$G/$f" ] || { echo "missing $G/$f (scripts/prepare-game.sh)" >&2; exit 1; }
done
[ -d "$G/extract/romfs" ] || { echo "missing $G/extract/romfs" >&2; exit 1; }
mkdir -p "$OUT/analysis" "$OUT/spirv"
link() { ln -f "$1" "$2" 2>/dev/null || cp -f "$1" "$2"; }
# main, sdk and whichever subsdk modules the game ships (games/botw/modules.cmake)
for f in "$G"/analysis/*.bin "$G/analysis/main.npdm"; do link "$f" "$OUT/analysis/$(basename "$f")"; done
link "$G/spirv/results.tsv" "$OUT/spirv/results.tsv"
find "$G/spirv" -maxdepth 1 -name '*.spv' -exec ln -f -t "$OUT/spirv" {} + 2>/dev/null ||
    find "$G/spirv" -maxdepth 1 -name '*.spv' -exec cp -f -t "$OUT/spirv" {} +
# The RomFS as romfs.pak + romfs.idx (scripts/make-romfs-pack.py: two files to upload instead of tens of thousands), made once
# (BOTW_REPACK=1 makes it again). BOTW_ROMFS_FOLDER=1 stages the extract/romfs folder instead.
if [ "${BOTW_ROMFS_FOLDER:-0}" = 1 ]; then
    rm -f "$OUT/romfs.pak" "$OUT/romfs.idx"
    rm -rf "$OUT/extract/romfs"; mkdir -p "$OUT/extract"
    cp -al "$G/extract/romfs" "$OUT/extract/romfs" 2>/dev/null || cp -a "$G/extract/romfs" "$OUT/extract/romfs"
else
    rm -rf "$OUT/extract"
    if [ "${BOTW_REPACK:-0}" = 1 ] || [ ! -f "$OUT/romfs.idx" ] || [ ! -f "$OUT/romfs.pak" ]; then
        python3 "$(dirname "$0")/make-romfs-pack.py" "$G/extract/romfs" "$OUT"
    fi
fi
if [ ! -f "$OUT/config.txt" ]; then
    cat > "$OUT/config.txt" <<'EOF'
# BOTW settings: one NX_<KEY>=<value> per line, read at startup ('#' comments). See README.md.
# NX_PS4_CORES=pinned        guest threads on their Switch core's PS4 core (default: balanced; or spread)
# NX_BUTTONS=nintendo        Circle = A, Cross = B (by position); default Cross = A
# NX_HANDHELD=1              the game's handheld mode (720p) instead of docked (1080p)
# NX_RENDER_RES=1600x900     the size the game renders at while docked (lighter on the GPU)
# NX_FSR=1                   upscale to 1080p with FidelityFX Super Resolution 1 instead of a plain stretch
# NX_MUTE=1                  no sound
EOF
fi
echo "$(find "$OUT" -type f | wc -l) files, $(du -sh "$OUT" | cut -f1) in $OUT"
echo "copy its contents to /data/BOTW on the console"
