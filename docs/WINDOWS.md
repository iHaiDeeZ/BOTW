# BOTW on Windows (debug build)

A native x64 build of the same recompiled game and NVN→Vulkan runtime as the PS4 port, set up like TOTK's
(`E:\totk-win`, `E:\PS4 Projects\TOTK\docs\WINDOWS.md`). It runs on the PC's NVIDIA GPU, so the renderer can be
inspected locally with validation, frame dumps, storyboards and probe shaders, without a package per test. It is a
debugging tool, not a release target.

## Status (2026-10-21)

- **It boots and plays the opening unattended** on the RTX 5070 Ti at 30 fps (the game's cap; replay 13-16 ms per
  frame in the shrine): "Nintendo Presents" (frame ~300), the title text and flare, "Open your eyes", "Wake up, Link",
  the camera pan, then gameplay with the hearts and the HUD (frame ~3600, about 2.5 minutes after start). It autosaves
  there (`data\BOTW\user`): later runs need `-NewGame` to see the opening again without input.
- **The black shrine reproduced here, so the bug was in the shared NVN layer, and it is FIXED on this build**
  (`recomp` branch `botw-win`, commit `7d3afa7`; "Root cause" below). Link, the pool and the carvings are lit with
  stock settings (exact EQUAL, nothing skipped): `shots\run14-fix`. Before the fix (`ac17c79` = PS4 v00.09):
  - `NX_EQUAL_EXACT=0` (v00.08's EQUAL handling): exactly the console's v00.08: emissive lights and fog, every lit
    surface black (`shots\run2`).
  - default `NX_EQUAL_EXACT=1` (v00.09): darker still, the emissives gone too, only the ambient (`shots\run1`).
- **Still wrong after the fix**: a strong blue-white haze over the camera-pan frames (the last ones nearly white), and
  wavy orange bands over Link's skin. Separate passes (glare/exposure, the character-mask or subsurface blur); not
  looked into yet.
- **Menu text renders on the PC** (title menu: Continue, New Game, Options, amiibo, Downloadable Content), with the
  console's save in `data\BOTW\user\save` (from `E:\PS4 Projects\BOTW\logs\v00.09\save`): the console's missing menu
  text is PS4-side. A on Continue opened "Select save data to load." with six empty slots (no picture, place or date)
  that stayed for 30 s: `shots\menu`.
- Storyboard of the shrine with the fix (frame 2401, "Wake up, Link", default settings with `NX_DIAG_FLOAT/CMDS`):
  `data\BOTW\diag`, stats in `data\nx.log`. Before the fix: `shots\diag-run6` (+ `shots\passes-run6.txt`, with layers
  and image ids) and `shots\diag-run1` (sheets `shots\diag-run1-{gbuf,light,post}.png`). Pictures left over from other
  runs: `shots\diag-stale` (a storyboard overwrites `passes.txt`, not pictures of other names).

## Layout

Everything is in `E:\botw-win` (`BOTW_WIN`), outside the BOTW repo and WSL, for fast builds:

| Where | What |
|---|---|
| `recomp\` | a clone of `~/botw-build/src/recomp` (`core.autocrlf=true`): branch `botw` (`ac17c79`), and `botw-win` (checked out) = `botw` + the storyboard instrumentation `818166f` + the fix `7d3afa7` |
| `gen\{main,subsdk0,sdk}` | copied from `~/botw-build/src/recomp/private/botw/gen` (bases main `0x4000000000`, sdk `0x4010000000`, subsdk0 `0x4018000000`, as on the PS4) |
| `third_party\Vulkan-Headers-1.4.357` | the headers `vk_gen` was generated from (copied from `E:\totk-win`) |
| `third_party\opus` | libopus 1.5.2, static, built with clang-cl `/arch:AVX2` (copied from `E:\totk-win`) |
| `data\BOTW` | copied from `~/botw-build/data-stage/BOTW` (romfs.pak/idx, spirv, analysis, config.txt), 15 GB; the 826 shaders the stage gained at 08:42 (Event effects) copied in after |
| `data\nx.log` | the log (the previous run's in `nx.log.prev`); `data\BOTW\frames\*.ppm` frame dumps, `data\BOTW\diag` the storyboard |
| `data\BOTW\user` | the save (`-NewGame` moves it to `user.prev`) |
| `build\botw.exe` | the build (182 MB) |
| `shaders\` | decompiled lighting shaders (`<hash>.glsl`), `make_probe.py` (constant-colour probes), `orig\` the SPIR-V a probe run replaced, `restore.sh` |
| `shots\` | frame dumps and storyboard pictures converted to PNG (the game's own images: keep them out of git) |
| `build-win.cmd`, `run-win.ps1` | build and run (copies in `E:\PS4 Projects\BOTW\scripts`) |

## Toolchain

The same as TOTK's Windows build, nothing new installed:

- Visual Studio Community 2026 (18.x) with **C++ Clang tools for Windows** (clang-cl 22). MSVC alone cannot build the
  generated code, which uses `__int128`. vcvars prints "'vswhere.exe' is not recognized": harmless.
- CMake 4.2, Ninja 1.13, Python 3.12 with Pillow (frame dumps to PNG: `recomp\tools\frames.py`).
- The **LunarG Vulkan SDK 1.4.363** (`C:\VulkanSDK\1.4.363.0`) for the validation layer, spirv-cross and
  glslangValidator. The headers stay at 1.4.357. The NVIDIA driver provides `vulkan-1.dll`.

No source change was needed for Windows: the `botw` branch already has TOTK's Windows commit (`c19717d`: clang-cl
flags, `-DNX_OPUS_DIR`, `/clang:-ffp-contract=off` for the game modules) and `games/botw/game.cpp` its own `setenv`
shim for `_WIN32`.

## Build and run

```bat
build-win.cmd configure   & rem first time, or after CMakeLists changes (then builds)
build-win.cmd             & rem incremental: ninja -j8 (JOBS=n to change; up to ~1.5 GB per generated file)
```

A full build took **9 minutes** at `-j8` on this PC (934 steps: main 664 files, sdk 109, subsdk0 73, the runtime;
4.9 s per file on average, the largest, `code_327.cpp` at 13.8 MB, 227 s). A runtime-only change rebuilds in about
a minute.

```powershell
.\run-win.ps1 -Seconds 240 -DumpEvery 150          # windowed; every 150th presented frame -> data\BOTW\frames\NNNNN.ppm
.\run-win.ps1 -Seconds 240 -DiagEvery 2400         # a storyboard every 2400 presents -> data\BOTW\diag (each replaces the last)
.\run-win.ps1 -NewGame                             # save moved aside first: the opening plays from the start
.\run-win.ps1 -Ps4Like                             # PS4 defaults: 1280x720 + FSR, pipelined submits, dynamic UBOs
.\run-win.ps1 -Validation                          # VK_LAYER_KHRONOS_validation -> data\validation.txt
```

`$env:NX_MUTE='1'` before the script silences the game. A storyboard can also be asked for at any moment: create an
empty file `data\BOTW\diag_now` (checked every 60 presents). Frames to PNG:
`python recomp\tools\frames.py png data\BOTW\frames shots\<run>`.

To get source changes from the PS4 side: `git pull` in `E:\botw-win\recomp` (origin is
`//wsl.localhost/Ubuntu/home/shiro/botw-build/src/recomp`), then `build-win.cmd`. If the recompiler or the dump
changes, copy `gen\` again. Fixes made here: commit on a branch `botw-win` in `E:\botw-win\recomp`, then from WSL
`git fetch /mnt/e/botw-win/recomp botw-win:botw-win`. Keep `core.autocrlf=true` in that clone: its files were
checked out with CRLF, and changing the setting marks every file as modified.

## The shrine's lighting on the PC (2026-10-21)

How the frame is built (storyboard of frame 2401, 1600x900; BOTW's dynamic resolution drops to 1440x810 when the
diagnostics slow it down):

1. **G-buffer** (fs `bdee4a747c8e00a3`, ~200 draws): albedo (sRGB), normals, material in c0 (R = material ID 4..14 in
   the shrine), and c5 = the full-res lighting target (`508cad1000`, RGBA16F) pre-filled with an ambient term (the
   dark blue left in the black frames).
2. Decals (fs `19d434c47b5df6ae`), linear depth (fs `21dcc7b8b5b778bc`, R32F half-res), shadow maps (448x448 R16),
   a 12x1 RGBA16F of light parameters (fs `b9e731dcdda973e6`).
3. **Half-res shadow/fog pass** (800x450): classifier fs `4f2551d45d6b662d` (`FragDepth = (ID & mask) / 255` into
   a D16), then quads `1112d2666e11ebac`, `2e837e442ca135c1` (EQUAL) and `9876b7b00a10742e` (LEQUAL). `9876b7b0`
   writes c0 = **(sun shadow, cloud shadow, transmittance, 0)** (RGBA8 at `508f5ed000`), c1 = fog colour (RGBA16F),
   c2 = 0 by design (layer 0 of the local-light array), then `ClearColor(3)` (layer 1) to (0, 0, 0, 1).
4. Light volumes (fs `23b4ee59f59cd7b0`, back faces, GREATER) into layers 0/1 of the local-light array (2-layer
   RGBA16F at `508f027000`): layer 0 holds the shrine's blue lamps (mean RGB 7.7 / 13.7 / 24.1).
5. Character mask (Link: material bit 3): `47c1cfbcd4c98937` (alpha only, writes 16/255 into a D16),
   `f46430253e7a56ad` (EQUAL), `40882560a0a325dd` (NOTEQUAL, writes 0), `4c77419c93b6a292` (alpha only).
6. **Full-res lighting**: classifier again, then 9 EQUAL quads (fs `bda4fbae490fe310`, `100a63cc99bf95ca`,
   `4864a1f998c11913`, `bff465da65814297`, `c03315f4470aa71b`, `b6c2365c74ca2b8f`, `10a0099551ee1d4c`,
   `4b6a0d3d02030bf2`, `16f21240ca591663`, partly through called command lists). Each computes
   `light * t12.z + fog`: everything lit (sun, local lights from the array, environment cube, specular) is multiplied
   by the **transmittance in B of the half-res pass's c0** (`fp_t_tcb_12`).

### Root cause (fixed in `7d3afa7`)

agl renders through holder structs: it copies one texture's `NVNtexture` after another into the same object and
renders, clears and copies through it (`509c0b14e8` held ~10 textures per frame: quarter-res R8 blur targets at
400x225, the shadow/transmittance target, others). v00.08's `adopt_texture` recognises each copy by its stamp, but
recorded commands kept the holder's address, and a replay resolves addresses at one mark per submit (the end of its
recording, `nvn_incarnation.cpp`). So **every pass through the holder rendered into its last incarnation of the
recording**, the shadow/transmittance target: the blur passes (viewport 400x225 into an 800x450 image, the
"top-left quadrant" pictures) and the character mask's `40882560` (writes 0 to every pixel but Link's) zeroed its B
channel, and the full-res lighting multiplied every light by 0. NVN takes these states when a command is recorded.

Fix: each adopted incarnation is also published under a host key nothing re-initializes (`0xD000...`, a ring of
8192), and `SetRenderTargets`, `ClearTexture` and the texture copies record that key (`stable_texture_key`).
`NX_ADOPT_KEYS=0` gives the old behaviour. Shrine frame 2401: the full-res lighting result went from the ambient only
(mean RGB 0 / 0.012 / 0.031) to lit (0.19 / 0.34 / 0.44, peaks 21.8).

### How it was found (runs of ~3 minutes each, `-NewGame`)

| Run | Settings | Result |
|---|---|---|
| 1 | defaults (`NX_EQUAL_EXACT=1`) | fog/ambient only, no emissives |
| 2 | `NX_EQUAL_EXACT=0` | emissives + ambient, lit surfaces black = console v00.08 |
| 3 | `NX_EQUAL_AS=7` (EQUAL quads ALWAYS) | still no light |
| 4 | `NX_ALIAS_SYNC=1` | **device lost** (`vkQueueSubmit` -4) at frame ~1540 |
| 6 | instrumented storyboard (`818166f`) | the light array's layers and images are right; layer 0 holds the lamps; the full-res result is still ambient only |
| 7 | constant-colour probes for the 9 light quads (`shaders\make_probe.py`) | one class (`b6c2365c`, cyan) clearly passes EQUAL; the rest too dark to read (auto-exposure follows the bright probe) |
| 8, 9 | `NX_SKIP_FS=47c1cfbc...,f4643025...,40882560...,4c77419c...` | **the shrine is lit**; full-res result mean 0.17 / 0.52 / 0.80 |
| 11 | `NX_SKIP_FS=40882560a0a325dd` | environment lit, Link black (`f4643025` zeroes his B) |
| 10, 12 | `NX_TRACE_TEXTURES=2380`; `NX_TRACE_DRAWS=1 NX_TRACE_FS_HASH=...` | both holders adopt init #77112 (`508f5ed000`); the mask draws: blend off, logic COPY, channel mask f; every pass through `509c0b14e8` resolves to `508f5ed000` |
| 14 | the fix, stock settings | Link, pool, carvings lit; haze and skin bands remain |

Ruled out on the way: depth bias on the light quads (set but not enabled), the light-array layers and image sharing
(right), per-component output masks (the shaders' OMAP is `0xf`), conditional rendering (none around these passes).

## Next steps

- **PS4**: the fix is in the shared NVN layer. From WSL: `git fetch /mnt/e/botw-win/recomp botw-win:botw-win`, then
  cherry-pick `7d3afa7` (and `818166f` for the storyboard) onto `botw`, rebuild, package.
- The haze over the camera pan and the orange bands on Link's skin: storyboard a pan frame (`-DiagEvery 3000`) and
  look at the glare/exposure passes and the character-mask blur (`f4643025`, `4c77419c`) now that they have their own
  targets.
- Other record-time texture references may hold agl holders too (`BindImage`; pool registrations take snapshots
  already): check them if more corruption shows.
- Exact EQUAL: run 7 could not tell whether every class passes; with the fix, compare `NX_EQUAL_EXACT=0` and `=1`
  on the shrine before changing the PS4 default back.
- Alias sync crashing the device on NVIDIA is worth a look on its own: the PS4 has it off too.
