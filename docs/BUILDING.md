# Building BOTW

BOTW is BelmanteGu's Switch static recompiler and runtime ([SuperMarioWonderRecomp](https://github.com/BelmanteGu/SuperMarioWonderRecomp)),
with SMW's PS4 port and a `games/botw` target. The package is built from **your own dump**: its `eboot.bin` is
mostly the game's code, recompiled to x86-64, so it can't be downloaded and must never be shared.

## Layout

| Where | What |
|---|---|
| this repo (`~/botw` in WSL) | scripts, toolchain, PS4 runtime (`port/`), `patches/botw-ps4.patch` |
| `~/botw-build/src/recomp` | the recompiler checkout, branch `botw` (`scripts/clone-recomp.sh`) |
| `~/botw-build/src/recomp/private/botw` | your game: `analysis/`, `exefs/`, `extract/romfs`, `gen/`, `spirv/` |
| `~/botw-build/build/{mesa,botw}` | RADV (Crema's build, copied from SMW) and the eboot |
| `~/botw-build/out`, `out/` | packages; `~/botw-build/oelf/vXX.YY.oelf` keeps every build's symbols |

## Steps

1. **Build tree**: `scripts/seed-build-tree.sh` copies the sysroot, Mesa, Vulkan-Headers 1.4.357, the fixed PkgTool
   and the .NET SDK from `~/smw-build` (SMW must have been set up once).
2. **Recompiler**: `scripts/clone-recomp.sh`. After changes there: `scripts/save-patch.sh "message"`.
3. **Your game**: put `prod.keys` (and `title.keys`) in one folder with the BOTW base game (XCI or NSP) and the
   **1.5.0 update** NSP, then `scripts/prepare-game.sh <folder>` (steps `import recompile shaders`, or any of
   them).
   - The importer takes the base program and one update. The update's ExeFS replaces the base's, and its RomFS is
     patched over the base's.
   - main 1.5.0 is recognised by its SHA-256 (the hashes in the decomp's `tools/setup.py`). It is then recompiled
     with `--symbols`: the decomp's `data/uking_functions.csv` is downloaded at a pinned commit and turned into
     `private/botw/symbols-main.txt` by `scripts/decomp-symbols.py`. The names end up as comments in the
     generated code and in `gen/main/names.txt`. Any other version (1.6.0 is the last one) is recompiled
     without symbols.
   - The modules are generated for main `0x4000000000`, sdk `0x4010000000`, and one subsdk at `0x4018000000`, if
     the game has one (`games/botw/modules.cmake` builds whichever ones were generated). The script stops if a
     module doesn't fit below the next one.
   - Shaders: `Shader Effect Layout System Terrain Lib UI` under the RomFS by default (not checked against a dump
     yet); `BOTW_SHADER_DIRS="..."` changes the list, and `BOTW_SHADER_DIRS=all` scans everything.
   - `SystemResourceSize` comes from `analysis/main.npdm` at run time.
4. **Eboot**: `scripts/rebuild-botw.sh` (logs in `~/botw-build/logs`). Without a dump:
   `BOTW_GEN=$(scripts/make-stub-gen.sh) scripts/rebuild-botw.sh` builds the runtime alone (it stops at boot).
5. **Package**: `BOTW_VERSION=00.01 scripts/package-botw.sh`, then from Windows (Git Bash)
   `scripts/upload-to-console.sh out/BOTW-v00.01.pkg`.
6. **Data**: `scripts/stage-data.sh`, then copy `~/botw-build/data-stage/BOTW` to `/data/BOTW` on the console
   (`scripts/upload-data.py`).
