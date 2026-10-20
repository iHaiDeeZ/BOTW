# BOTW: Breath of the Wild recomp for PS4

BOTW is an attempt to run **The Legend of Zelda: Breath of the Wild** (Switch, 1.5.0, `01007EF00011E000`) natively
on a jailbroken PS4 with the same approach as [SMW](https://github.com/iHaiDeeZ/SMW) and TOTK:

- BelmanteGu's static recompiler turns the game's ARM code into C++, which is compiled for the PS4's CPU.
- The game's NVN graphics are replayed on Vulkan through Mesa's RADV driver, ported to the PS4's GPU.
- There's no emulator.

[zeldaret/botw](https://github.com/zeldaret/botw) decompiles the same 1.5.0 executable. About half of its 113,491
functions are still undecompiled, so it can't build the game by itself. The recompiler uses its function list
(`data/uking_functions.csv`) as exact function starts and names: `--symbols`, from `scripts/decomp-symbols.py`.

You bring your own game: nothing copyrighted is in this repository. No game code, assets or keys. A package built
from your dump contains your game's code, so it is for your console only.

## Status

**Early work. The game doesn't run yet.**

| Step | State |
|---|---|
| Runtime from TOTK's branch (NVN on RADV, PS4 memory, boot screen, quick menu), `games/botw` | done |
| Recompiler `--symbols` with the decomp's function list (main 1.5.0 only) | done, untested on BOTW |
| Separate PS4 app (`BOTW00001`, data in `/data/BOTW`), own art | done |
| Runtime builds and packages against stub modules (`BOTW-v00.00`) | done, a smoke test only |
| Import base + 1.5.0 update, recompile, translate the shaders | waiting for a dump |
| Switch services, NVN features and patches BOTW needs | to do ([docs/GAPS.md](docs/GAPS.md)) |
| Boot to the title screen on a PS4 | to do |

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
