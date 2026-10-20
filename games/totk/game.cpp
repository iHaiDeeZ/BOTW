// The Legend of Zelda: Tears of the Kingdom (0100F2C0115B6000, 1.0.0): what the shared runtime needs to know about
// the game. It draws through NVN like Wonder. Modules: main, subsdk0 and sdk (modules.cmake), generated with
// --base 0x4000000000 (main), 0x4018000000 (subsdk0) and 0x4010000000 (sdk) (the PS4 port: scripts/prepare-game.sh).
#include <cstdlib>

#include <nx/game.h>
#include <nx/runtime.h>

#ifdef _WIN32
// The Windows C library has no setenv: the same rule (a value already set stays unless overwrite) through _putenv_s.
static int setenv(const char* name, const char* value, int overwrite) {
    if (!overwrite && std::getenv(name)) return 0;
    return _putenv_s(name, value);
}
#endif

namespace nx {

const game::Info game::kInfo = {"totk", "The Legend of Zelda: Tears of the Kingdom", 0x0100F2C0115B6000ull,
                                "io.github.totkrecomp", "private/totk",
                                game::kSystemResourceFromNpdm /* main.npdm META +0x14 */};

// Runtime defaults for this game (config.txt still overrides them: setenv does not replace a value already set).
void game::after_init() {
    // nvnMemoryPoolInvalidateMappedRange without waiting for the submits so far (nvn_objects.cpp): TOTK calls it ~10
    // times a frame, and the wait kept its main thread behind the whole replay (30-57 ms a frame).
    setenv("NX_INVALIDATE_WAIT", "0", 0);
    // Storage that fragment shaders write (v00.17) went back to guest memory with a wait for every submit: 20 -> 9 fps,
    // and the world stayed black without it too. Off until it can apply without waiting.
    setenv("NX_GRAPHICS_WRITE_BACK", "0", 0);
    // The shadow casters draw with depth clamp; on the PS4 every clamped fragment landed at depth 0 (storyboard
    // v00.23: shadow maps cleared to 1, then 0 everywhere). Clipped instead (nvn_pipeline.cpp).
    setenv("NX_DEPTH_CLAMP", "0", 0);
}
void game::renderer_start() {}
void game::first_frame() {}

}  // namespace nx
