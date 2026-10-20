# What BOTW needs beyond TOTK

The runtime is TOTK's (branch `totk` of the recomp, 2026-10-20) with `games/botw` on top. Everything below is
expected or inferred, **not yet checked on a dump**. Check each item at the first import and boot, and move it to
"Checked" with the numbers.

## Done

- `games/botw`: title `01007EF00011E000`, `SystemResourceSize` from `main.npdm`, and TOTK's PS4-side defaults:
  - `NX_DEPTH_CLAMP=0`
  - `NX_INVALIDATE_WAIT=0`
  - `NX_GRAPHICS_WRITE_BACK=0`
- Modules: main, sdk, and whichever subsdk the dump has (`modules.cmake` takes the generated folders). There is
  room for one subsdk.
- Recompiler `--symbols` (from the SMO port):
  - the decomp's 113,491 function starts are added as seeds and names, for main 1.5.0 only;
  - it refuses if fewer than 90% land on instructions.
- Yaz0 in `tools/maxwell/containers.py`. BOTW's packed files (`.s*`) are Yaz0, not zstd.

## Checked on the user's dump (base + 1.6.0 update, 2026-10-20)

- **Version.** The update is 1.6.0 (`v786432`): its main has the decomp's 1.6.0 hash, so it is recompiled
  without symbols. DLC Pack 1 and 2 are in the dump folder but not imported (the base game runs without them).
- **Modules.**
  - main: 44.5 MB, 83,256 functions, 1,499 jump tables, 0 unhandled.
  - sdk: NintendoSDK 7.3.2, 34,480 functions, 0 unhandled.
  - subsdk0: the movie library, 6 MB, 21 unhandled NEON instructions (bfc, saddlv, sqabs, sqrshl, sqshl), as TOTK's.
  - The sdk has the SDK 9 form of `nn::init::Start` (thread, base, init, fini), which the runtime handles, and the
    in-process `nn::audio::server::AudioRenderSystem`.
- **RomFS.** 41,052 files, 15.13 GB.
- **Shaders.** All in `Pack/Bootup_Graphics.pack`, except for the effects:
  - the nine `Shader/uking_*.product.sbfsha` (FSHA 3.0.1, Yaz0 inside the SARC);
  - agl, gsys and KSys `*.ssarc` (BAHS v9);
  - `Effect/*.sesetlist` (VFXB, compute).

  In `containers.py`: Yaz0 members inside a SARC are opened, the FSHA 3.0.1 shading-model layout falls back to
  BNSH by magic, and BNSH 2.1.2 programs start at 0 with no NVN header. Results: 868 containers, 7,878 distinct
  blocks, **7,870 translated** and passed spirv-val.

  **8 vertex shaders fail**, all with "Invalid instruction at 0x10":
  - 6 effects: `DgnObj_Hrl_Warpin_2x2_01`, `Enemy_Chuchu_Electric_Junior`, `Enemy_SiteBoss_Spear`, `FlyLocater`,
    `Octarock`, `RemainsElectricCannon`;
  - 2 in `Bootup_Graphics`.

  Probably another program start (VertexA/B pairs?). Check `spirv/results.tsv`.
- **Not extracted.** `System/font/nvn_font/nvn_font_shader*.bin` is a raw NVN layout (offset table, then a control
  section). It is probably a debug font.
- **Eboot.** 220 MB.

## To check at the first import

- **Which update.** The decomp targets 1.5.0, but most dumps carry 1.6.0. A 1.6.0 main is recompiled without
  symbols. The decomp's setup downloads xdelta patches from 1.6.0 to 1.5.0 for main only, but a 1.5.0 main with
  a 1.6.0 RomFS or sdk is untested: prefer a real 1.5.0 update.
- **Modules.** Does 1.5.0 ship a subsdk? Check the sizes against the 256/128/128 MiB rooms, look for `DT_RELR`,
  and confirm DT_INIT.
- **Shaders.** Which RomFS folders hold them:
  - shader archives (`Shader/*.bfsha`, maybe Yaz0);
  - effect `ptcl` inside `Effect/*.sesetlist` (the old VFXB layout: `parse_vfxb` may need another version);
  - UI shaders in the ExeFS (`exefs_shaders.py`).
- **SDK offsets.** The runtime still has offsets for Wonder's SDK 16.2:
  - the NVN format table is found by content now (TOTK v00.05);
  - `hle_audio.cpp` looks things up by symbol;
  - grep the runtime for other fixed `sdk+0x` offsets.

  BOTW 1.5.0 uses a far older SDK, so its NVN driver structures and nn:: symbols may differ.

## To check at the first boot

- **Audio renderer.** An older audren revision. The in-process `AudioRenderSystem` path may not match the old SDK.
- **NVN.** An older NVN API version. Check the bootstrap loader names, the texture/sampler descriptor layouts, and
  the `nvnDeviceGetProcAddress` names the game asks for (the `nvn_trace.cpp` misses).
- **Movies.** BOTW plays its cutscene and memory videos as files in the RomFS. TOTK's runtime has VP9 work for its
  own movies; check which codec BOTW 1.5.0 uses.
- **Memory.** Watch the "direct memory free" lines. SMW and TOTK both ran short. BOTW streams the open world.
- **Saves.** `nn::fs` save mount, and the game's save slots.
- **amiibo (nfp).** TOTK's `hle_nfp.cpp` (no reader) should cover it.
- **swkbd.** Used to name a horse.

## Performance

TOTK reaches 18-20 fps on this runtime, with the NVN replay on one core. BOTW's draw count in the open world is
the main unknown: measure the replay time per frame ("draw replay per frame" in nx.log) at the Great Plateau.
