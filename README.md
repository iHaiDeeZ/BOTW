# BOTW: Breath of the Wild recomp for PS4

BOTW runs **The Legend of Zelda: Breath of the Wild** (Switch, `01007EF00011E000`; tested with the 1.6.0 update)
natively on a jailbroken PS4 with the same approach as [SMW](https://github.com/iHaiDeeZ/SMW) and TOTK:

- BelmanteGu's static recompiler turns the game's ARM code into C++, which is compiled for the PS4's CPU.
- The game's NVN graphics are replayed on Vulkan through Mesa's RADV driver, ported to the PS4's GPU.
- There's no emulator.

[zeldaret/botw](https://github.com/zeldaret/botw) decompiles the same 1.5.0 executable. About half of its 113,491
functions are still undecompiled, so it can't build the game by itself. The recompiler uses its function list
(`data/uking_functions.csv`) as exact function starts and names: `--symbols`, from `scripts/decomp-symbols.py`.

You bring your own game: nothing copyrighted is in this repository. No game code, assets or keys. A package built
from your dump contains your game's code, so it is for your console only, and the releases carry none.

## Status

**It plays, slowly.** Tested on a PS4 with a base + 1.6.0 dump, 2026-10-21 to 23. Each line below is a package
version; [docs/GAPS.md](docs/GAPS.md) has the details and the logs are in `logs/`.

| What | State |
|---|---|
| Boot, title screen, menus and loading screens with their text | works (v00.13) |
| The opening, the Shrine of Resurrection and the Great Plateau, lit | works (v00.10) |
| Sound | works (v00.11) |
| Saves, with their pictures in the load list | works (v00.16) |
| Frame rate | ~9 fps outdoors: the PS4's CPU replays ~3,000 draws a frame at ~33 us each |
| Big render passes recorded on three worker threads ("Parallel draws" in the L3+R3 menu) | v00.20, untested |
| The loading screen's picture, movies, 8 effect vertex shaders | open |

The recompiler gets the decomp's function list only for a 1.5.0 executable; a 1.6.0 one is recompiled without it.

### Compared with TOTK

- **Memory.** BOTW is a smaller game than TOTK: the Switch version was made from a Wii U game, so it has a
  smaller RomFS and a smaller executable.
- **SDK.** BOTW 1.5.0 is built with a much older NVN SDK than Wonder or TOTK. Every fixed SDK offset in the
  runtime is a risk.
- **CPU.** Hyrule's open world draws far more than Wonder's courses. TOTK reached 18-20 fps on the same runtime.

## Building

See [docs/BUILDING.md](docs/BUILDING.md). In short (WSL Ubuntu, the SMW build tree already set up):

```sh
bash scripts/seed-build-tree.sh                        # ~/botw-build from ~/smw-build, ~/botw -> this repo
bash scripts/clone-recomp.sh                           # the recompiler at patches/BASE.txt + patches/botw-ps4.patch
bash scripts/prepare-game.sh <folder with keys + dump> # import, recompile, shaders -> private/botw
bash scripts/rebuild-botw.sh                           # eboot
BOTW_VERSION=00.01 bash scripts/package-botw.sh        # out/BOTW-v00.01.pkg
bash scripts/stage-data.sh                             # data for /data/BOTW
```

## Credits

See [CREDITS.md](CREDITS.md).
