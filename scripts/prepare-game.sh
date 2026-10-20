#!/usr/bin/env bash
# Turns your own The Legend of Zelda: Breath of the Wild dump (01007EF00011E000: the base game XCI/NSP plus the 1.5.0
# update NSP, the version zeldaret/botw decompiles) into what the PS4 build needs, with the recomp's tools (docs in
# ~/botw-build/src/recomp: docs/nvn/TRANSLATOR.md, SHADERS.md):
#
#   1. import    base + update with your keys -> analysis/{main,sdk,subsdk*}.bin + main.npdm, exefs/, extract/romfs
#                (the update's ExeFS and its patched RomFS)
#   2. recompile main, sdk and any subsdk to C++                          -> gen/<module>
#                main 1.5.0 also gets the decomp's function list (scripts/decomp-symbols.py, --symbols): exact
#                function starts and names. Another version (1.6.0 is the last) is recompiled without it.
#   3. shaders   extract the Maxwell blocks and translate them to SPIR-V (.NET 10 + Ryujinx's translator)
#                                                                            -> spirv/<hash>.spv, results.tsv
#
#   prepare-game.sh <folder with prod.keys, title.keys, the base XCI/NSP and the update NSP> [steps...]
#   (steps: import recompile shaders; default all three)
#
# Everything lands in ~/botw-build/src/recomp/private/botw (the tools only write under private/). Nothing here is
# shared or committed: it is your game. Needs: clang, cmake, ninja, python3-venv, curl; spirv-val (apt install
# spirv-tools) validates the shaders if present.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
IN="$(readlink -f "${1:?folder with prod.keys, title.keys, the base XCI/NSP and the update NSP}")"; shift
STEPS="${*:-import recompile shaders}"
SRC="${RECOMP_SRC:-$HOME/botw-build/src/recomp}"
G="$SRC/private/botw"
T="$SRC/private/third_party"
VENV="$SRC/private/venv"
TITLE=01007EF00011E000
# zeldaret/botw at the commit this port was set up with (2026-10-19): only data/uking_functions.csv is used.
DECOMP_CSV="https://raw.githubusercontent.com/zeldaret/botw/8f0334acf0d348bcf48787038d0a0ba63f740ee4/data/uking_functions.csv"
# main 1.5.0 as the decomp's tools/setup.py knows it (the NSO as stored, and decompressed)
MAIN_150=(898dc199301f7c419be5144bb5cb27e2fc346e22b27345ba3fb40c0060c2baf8
          d9fa308d0ee7c0ab081c66d987523385e1afe06f66731bbfa32628438521c106)
mkdir -p "$G" "$T"
cd "$SRC"

has() { [[ " $STEPS " == *" $1 "* ]]; }
venv() {
    [ -x "$VENV/bin/python" ] || python3 -m venv "$VENV"
    "$VENV/bin/python" -m pip -q install capstone lz4 zstandard fonttools
}

if has import; then
    echo "== import"
    cmake -S android/wonder/importer -B private/build/importer -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
    ninja -C private/build/importer nximport
    shopt -s nullglob nocaseglob
    games=("$IN"/*.xci "$IN"/*.nsp)
    shopt -u nullglob nocaseglob
    [ ${#games[@]} -gt 0 ] || { echo "no .xci / .nsp in $IN" >&2; exit 1; }
    keys=("$IN/prod.keys"); [ -f "$IN/title.keys" ] && keys+=("$IN/title.keys")
    # The importer takes the base program and at most one update of the title from these files (DLC is ignored).
    NX_IMPORT_NOSYNC=1 private/build/importer/nximport --title "$TITLE" --exefs --out "$G" "${keys[@]}" "${games[@]}"
    sync
    ls -la "$G/analysis"
fi

if has recompile; then
    echo "== recompile"
    venv
    mods=(main)
    for f in "$G"/exefs/subsdk[0-9]; do [ -f "$f" ] && mods+=("$(basename "$f")"); done
    mods+=(sdk)
    # The runtime reserves 512 MiB for the modules (memory.cpp): main at the base, sdk 256 MiB above it, one subsdk
    # at +384 MiB (scripts/make-stub-gen.sh, games/botw/game.cpp).
    [ ${#mods[@]} -le 3 ] || { echo "more than one subsdk (${mods[*]}): the module layout has room for one" >&2; exit 1; }
    declare -A BASE=([main]=0x4000000000 [sdk]=0x4010000000 [subsdk0]=0x4018000000 [subsdk1]=0x4018000000)
    declare -A ROOM=([main]=0x10000000 [sdk]=0x8000000 [subsdk0]=0x8000000 [subsdk1]=0x8000000)
    # The decomp's function list, for main 1.5.0 only.
    sym=()
    sum="$(sha256sum "$G/exefs/main" | cut -d' ' -f1)"
    if [[ " ${MAIN_150[*]} " == *" $sum "* ]]; then
        [ -f "$G/uking_functions.csv" ] || curl -fsSL "$DECOMP_CSV" -o "$G/uking_functions.csv"
        "$VENV/bin/python" "$HERE/decomp-symbols.py" "$G/uking_functions.csv" "$G/symbols-main.txt"
        sym=(--symbols "$G/symbols-main.txt")
    else
        echo "main is not 1.5.0 (sha256 $sum): recompiled without the decomp's function list"
    fi
    for mod in "${mods[@]}"; do
        [ -f "$G/exefs/$mod" ] || { echo "missing $G/exefs/$mod (import step)" >&2; exit 1; }
        # The module's size in memory (text, ro, data and bss) must fit below the next module.
        size=$("$VENV/bin/python" - "$G/exefs/$mod" <<'EOF'
import struct, sys
raw = open(sys.argv[1], "rb").read()
end = max(struct.unpack_from("<III", raw, 0x10 + i * 0x10)[1] + struct.unpack_from("<III", raw, 0x10 + i * 0x10)[2]
          for i in range(3))
print(end + struct.unpack_from("<I", raw, 0x3C)[0])
EOF
)
        printf '%-8s %d MiB at %s\n' "$mod" $((size >> 20)) "${BASE[$mod]}"
        [ "$size" -le $((ROOM[$mod])) ] || { echo "$mod is $size bytes: more than the $((ROOM[$mod] >> 20)) MiB it has" >&2; exit 1; }
        # private/botw/trace-<module>.txt (hex function addresses): those functions log entry and return (diagnostics)
        trace=(); [ -f "$G/trace-$mod.txt" ] && trace=(--trace "$G/trace-$mod.txt")
        extra=(); [ "$mod" = main ] && extra=("${sym[@]}")
        "$VENV/bin/python" -m recomp "$G/exefs/$mod" "$G/gen/$mod" --name "$mod" --base "${BASE[$mod]}" \
            "${trace[@]}" "${extra[@]}"
    done
    # A gen folder of a module this dump does not have would be built in (games/botw/modules.cmake).
    for d in "$G"/gen/subsdk*; do
        [ -d "$d" ] && [ ! -f "$G/exefs/$(basename "$d")" ] && rm -rf "$d"
    done
    du -sh "$G/gen"/*
fi

if has shaders; then
    echo "== shaders"
    venv
    if [ ! -x "$T/dotnet/dotnet" ]; then
        mkdir -p "$T/dotnet"
        curl -L https://builds.dotnet.microsoft.com/dotnet/Sdk/10.0.401/dotnet-sdk-10.0.401-linux-x64.tar.gz | tar -xz -C "$T/dotnet"
    fi
    if [ ! -d "$T/ryubing/src" ]; then
        mkdir -p "$T/ryubing"
        curl -L https://codeberg.org/Software_arch/Ryubing/archive/a82350bb77.tar.gz | tar -xz -C "$T/ryubing" --strip-components=1
    fi
    # Invariant globalization: WSL's Ubuntu has no libicu, which .NET would otherwise require.
    export DOTNET_ROOT="$T/dotnet" DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1
    "$DOTNET_ROOT/dotnet" build tools/maxwell/spirv_harness/SpirvHarness.csproj -c Release -m:2
    H="private/build/spirv_harness/bin/Release/net10.0/spirv_harness.dll"
    P="$VENV/bin/python"; X=tools/maxwell/containers.py; O="$G/shaders_extracted"; R="$G/extract/romfs"
    mkdir -p "$O"
    # Breath of the Wild's shaders (1.6.0, checked): Pack/Bootup_Graphics.pack has the nine
    # Shader/uking_*.product.sbfsha (FSHA 3.0.1, Yaz0 inside the SARC; containers.py takes their BNSH 2.1.2 files by
    # magic, programs start at 0 with no NVN header) and the agl, gsys and KSys archives (BAHS) in System/*.ssarc.
    # The effects (Effect/*.sesetlist: VFXB with BNSH) and layouts (*.sblarc: bgsh/__ArchiveShader.bnsh) are both
    # loose and inside other packs: v00.05 drew from Bootup.pack's Effect/GameResident.sesetlist and
    # Layout/Common.sblarc (1,593 misses). containers.py only decompresses the Yaz0 members that can hold shaders.
    # BOTW_SHADER_DIRS names other files or folders of the RomFS; BOTW_SHADER_DIRS=all scans all of it (slow: one
    # process per file). The PS4 has no translator at run time, so this set must be complete: a shader the game asks
    # for and spirv/ lacks is logged by the runtime ("no .../spirv/<hash>.spv"; NX_DUMP_SHADER_MISSES=1 saves its
    # code to shader_misses/). Files without shaders extract nothing and their folder is removed.
    dirs=()
    if [ "${BOTW_SHADER_DIRS:-}" = all ]; then
        dirs=("$R")
    else
        for d in ${BOTW_SHADER_DIRS:-Pack Effect Layout System}; do
            [ -e "$R/$d" ] && dirs+=("$R/$d")
        done
    fi
    [ ${#dirs[@]} -gt 0 ] || { echo "none of the shader folders exist under $R (BOTW_SHADER_DIRS=all scans everything)" >&2; exit 1; }
    while IFS= read -r -d '' f; do
        rel="${f#"$R"/}"
        n="$(printf '%s' "$rel" | sed 's#[/ ]#_#g; s#\.zs$##')"
        "$P" $X "$f" --extract "$O/$n" --unique >/dev/null 2>&1 || true
        [ -n "$(ls -A "$O/$n" 2>/dev/null | grep -v manifest.tsv)" ] || rm -rf "$O/$n"
    done < <(find "${dirs[@]}" -type f -print0 2>/dev/null)
    "$P" tools/maxwell/exefs_shaders.py "$G/analysis/main.bin" --extract "$O/exefs"
    echo "$(ls "$O" | wc -l) shader containers extracted"
    if command -v spirv-val >/dev/null; then val=(--spirv-val "$(command -v spirv-val)"); else val=(--no-val); fi
    "$DOTNET_ROOT/dotnet" "$H" --batch "$O" --out "$G/spirv" --jobs "${SHADER_JOBS:-6}" "${val[@]}"
    echo "$(ls "$G/spirv" | grep -c '\.spv$') shaders"
fi
echo "done: $G (next: scripts/rebuild-botw.sh, scripts/package-botw.sh, scripts/stage-data.sh)"
