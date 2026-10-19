// The Legend of Zelda: Tears of the Kingdom (0100F2C0115B6000, 1.0.0): what the shared runtime needs to know about
// the game. It draws through NVN like Wonder. Modules: main, subsdk0 and sdk (modules.cmake), generated with
// --base 0x4000000000 (main), 0x4018000000 (subsdk0) and 0x4010000000 (sdk) (the PS4 port: scripts/prepare-game.sh).
#include <cstdlib>

#include <nx/game.h>
#include <nx/runtime.h>

namespace nx {

const game::Info game::kInfo = {"totk", "The Legend of Zelda: Tears of the Kingdom", 0x0100F2C0115B6000ull,
                                "io.github.totkrecomp", "private/totk",
                                game::kSystemResourceFromNpdm /* main.npdm META +0x14 */};

// Runtime defaults for this game (config.txt still overrides them: setenv does not replace a value already set).
void game::after_init() {
    // nvnMemoryPoolInvalidateMappedRange without waiting for the submits so far (nvn_objects.cpp): TOTK calls it ~10
    // times a frame, and the wait kept its main thread behind the whole replay (30-57 ms a frame).
    setenv("NX_INVALIDATE_WAIT", "0", 0);
}
void game::renderer_start() {}
void game::first_frame() {}

}  // namespace nx
