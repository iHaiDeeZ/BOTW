# Credits

BOTW is a port. Nearly everything that will make the game run is other people's work.

## The game's recompilation

- **[Super Mario Bros. Wonder Recomp](https://github.com/BelmanteGu/SuperMarioWonderRecomp)** by **BelmanteGu**
  (MIT): the static recompiler, the runtime (Horizon services, NVN on Vulkan, audio), the importer and the shader
  tools. BOTW applies `patches/botw-ps4.patch` (SMW's PS4 port plus the BOTW changes) to it and builds it at the
  commit in `patches/BASE.txt`. Upstream's
  own third-party components (listed in its README) keep their licenses.
- **[Ryujinx](https://ryujinx.app)** and its continuation **[Ryubing](https://codeberg.org/Software_arch/Ryubing)**
  (MIT): the Maxwell shader translator upstream's tools use to make SPIR-V. Downloaded by
  `scripts/prepare-game.sh`, not included here.
- **[zeldaret/botw](https://github.com/zeldaret/botw)**: the decompilation of Breath of the Wild 1.5.0. Its
  function list (`data/uking_functions.csv`) gives the recompiler exact function starts and names. It is
  downloaded by `scripts/prepare-game.sh`, not included here.
- **[MacNativeGames](https://github.com/ly05010419/MacNativeGames)** (MIT): Tears of the Kingdom on Apple silicon,
  the reference for TOTK's runtime work that BOTW inherits.

## The PS4 side

- **[Mesa](https://mesa3d.org)** (MIT): the RADV Vulkan driver, ported to the PS4's GPU in
  [Crema](https://github.com/iHaiDeeZ/Crema). `port/mesa/ac_ps4_drm.c` is that port's winsys.
- **[OpenOrbis](https://github.com/OpenOrbis)** (GPL-3.0 toolchain; its libraries under their own terms) and
  **[PacBrew](https://github.com/PacBrew)**: the PS4 toolchains. `toolchain/` comes from
  [love-ps4](https://github.com/Mari0/love-ps4)'s setup by way of Resonance, DolphinPS4 and Crema.
- **[Resonance](https://github.com/iHaiDeeZ/Resonance)**, **[DolphinPS4](https://github.com/iHaiDeeZ/DolphinPS4)**,
  **[M33](https://github.com/iHaiDeeZ/M33)**, **[Crema](https://github.com/iHaiDeeZ/Crema)** and
  **[SMW](https://github.com/iHaiDeeZ/SMW)** by ShiroKlein:
  the PS4 runtime in `port/` (heap, crash and hang logs, thread placement) and the VideoOut presentation.
- **[libopus](https://opus-codec.org)** (BSD-3-Clause), PacBrew's PS4 build: the game's Opus audio
  (`runtime/src/hle_opus.cpp` stands in for the Switch's hardware Opus decoder, as MacNativeGames does).
- **stb_dxt** by Fabian Giesen and Sean Barrett (public domain / MIT): the BC3 encoder the game's ASTC textures
  are re-packed with (`runtime/src/nvn/stb_dxt.h` in `patches/botw-ps4.patch`).
- **dlmalloc** by Doug Lea (its MIT-style licence, at the top of `port/third_party/dlmalloc/malloc.c`).
- **libjpeg-turbo** (BSD-3-Clause): the `turbojpeg.h` header in `port/third_party/turbojpeg`, with its notice.

## In the package

- **[AMD FidelityFX Super Resolution 1](https://github.com/GPUOpen-Effects/FidelityFX-FSR)** (MIT): EASU and
  RCAS in upstream's present pass.
- **Fredoka** by Milena Brandão (SIL Open Font License 1.1, licence text in the package's `fonts/`): the boot
  screen, the quick menu and the shared font.
- Upstream's button glyph font (MIT, as upstream).
- **DejaVu Serif** (Bitstream Vera licence): the "BOTW" lettering on the home screen and boot screen art.
- The home screen and boot screen art (`scripts/make-art.py`) is drawn from shapes and lettering: no game art.

## Thanks

To BelmanteGu for making the recompilation open, to the Ryujinx and Mesa developers, and to everyone keeping the
PS4 homebrew scene and its toolchains alive.
