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

## Checked on the console (2026-10-21)

Each item was one package version. Logs are in `logs/vXX.YY`.

- **NVN API.** nnMain checks for API 53.311 and skips its graphics setup otherwise (v00.03: `NX_NVN_API`).
- **Old SDK names.** Seven-six-axis sensor and other hid calls (v00.04), the SDK 7 audio device calls (v00.05),
  and `nn::oe` launch parameter and notification messages (v00.06).
- **Jump tables** whose anchor is the table's own base register (v00.07: `analysis.py`).
- **Black screen (v00.07).** The game ran (first frame at 40 s, the Great Plateau drawn at 12 fps with ~2,400
  draws per frame), but every presented frame was black. agl builds each texture in a stack temporary, registers
  it, then `memcpy`s the 192-byte `NVNtexture` into its own object (main+1289ad0) and renders through the copy.
  The runtime knew textures only by the address they were initialized at, so no render target, clear or copy
  reached the GPU (storyboard: every target `0x0 vk0`). agl also clears with `nvnCommandBufferClearTexture`,
  which was a stub.

  v00.08: textures and samplers write their description into their struct, and a copy becomes the same texture
  (`adopt_texture`, `NX_TEXTURE_STAMPS=0` turns it off). Also new: `ClearTexture`, `nvnTextureCompare` /
  `nvnSamplerCompare`, and `nvnQueueAcquireTexture`.
- **v00.08: the opening plays.** "Nintendo Presents", the title text, "Open your eyes", the Shrine of Resurrection
  and the camera pan after "Wake up, Link" (a cutscene: Link cannot be moved until it ends). Open problems:
  - **Surfaces black.** Only emissive lights and fog show. The lighting classifies materials by depth (fs
    `4f2551d4` writes `(material bits & mask) / 255` into a D16; light quads at `z = k/255` test EQUAL), which
    needs `NX_EQUAL_EXACT=1` as TOTK has (v00.09). Not yet confirmed as the cause.
  - **Frame rate.** 9-14 fps in the shrine: the replay costs ~25 us per draw over 3,000-4,400 draws (80-105 ms per
    frame: descriptor sets 24-31 ms, binds 20-26 ms, textures 13-18 ms). The GPU is not the limit.
  - **One pass per decal.** fs `19d434c4` samples two of its own 8 targets (agl turns the Switch's tiled cache off
    around them instead of using barriers), so the replay ends the pass before each of those draws.
  - **20 s freezes** came from the diagnostic storyboard itself (400 pictures); it now pictures each image at most
    4 times in passes of one or two draws (`NX_DIAG_PER_IMAGE`).
- **v00.09: surfaces still black** with `NX_EQUAL_EXACT=1`. Found on the Windows PC build (`docs/WINDOWS.md`): agl
  renders, clears and copies through "holder" structs, copying one texture after another into the same object
  (one held ~10 textures a frame). Commands kept the holder's address and the replay resolved it once per submit,
  so every pass through it drew into its last texture of the frame; the character-mask pass then zeroed the
  channel the lighting multiplies by. v00.10 (recomp `a6ebcb1`): render targets, clears and copies record a key of
  the texture the holder held when recorded (`NX_ADOPT_KEYS=0`: the address). On PC the shrine is lit with exact
  EQUAL; a haze and orange bands on Link's skin remain. On PC the title menu text shows with the console's save.
- **v00.10 on the console: lit.** Link and the shrine render with lighting (capture of frame 6135). Still missing on
  the PS4 only: the title menu's option text and the loading screens' text (the PC shows them; being reproduced on
  PC with the console's settings, `NX_RENDER_RES=1280x720` and FSR). 10-11 fps in the shrine: the replay costs
  35-40 us per draw (2,100-2,800 draws: descriptor sets 26-28 ms, textures 19-20 ms, binds 17-21 ms).
- **No sound (all versions until v00.11).** `ExecuteAudioRendererRendering` renders only in the manual mode on the
  CPU device; the runtime switched the render system at SDK 16.2's offsets (+0x2d0/+0x2d4), but BotW's SDK 7.3.2
  keeps them at +0x2d8/+0x2dc: every render failed with 0x40299 ("last result 40299" in nx.log). v00.11 reads the
  offsets from `GetExecutionMode` / `GetRenderingDevice`.
- **Diagnosis on the console only** (the user's call, 2026-10-21: no PC diagnosis). v00.12: `NX_DIAG_KEEP=1` keeps
  each storyboard in `diag/<frame>/`, so `NX_DIAG_EVERY=450` collects the title menu, a loading screen and
  gameplay in one run; storyboards list skipped draws with the reason, and each draw's scissor, viewport origin,
  blend, logic op, alpha test and vertex count.
- **v00.09: cutscene effects missing.** The opening's effects (`Event/Demo102_0.sbeventpack` →
  `Effect/Event_Demo102_0.sesetlist`, 10 shaders) were never extracted: the event packs carry their own effect lists.
  `prepare-game.sh` now scans `Event` too (`BOTW_SHADER_DIRS` default).
- **v00.09: menu text missing.** The main menu's options and the overwrite dialog's Yes/No have no text, while the
  dialog's message shows. Both use the R8 font `NormalS_00` (the other five fonts are BC4 sheets, which also work:
  the subtitles use `Normal_00`), so the font is not the difference; the labels sit in button parts
  (`PaCommonBtnThin*_00`). Next data: a diagnostics frame (L3 + R3 menu) on the main menu, to see whether the label
  draws exist.

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
