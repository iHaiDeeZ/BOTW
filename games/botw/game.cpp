// The Legend of Zelda: Breath of the Wild (01007EF00011E000, 1.5.0, the version zeldaret/botw decompiles): what the
// shared runtime needs to know about the game. It draws through NVN (agl/gsys, an older SDK than Wonder's). Modules:
// main, sdk and any subsdk (modules.cmake), generated with --base 0x4000000000 (main), 0x4018000000 (+0x8000000 per
// subsdk) and 0x4010000000 (sdk) (the PS4 port: scripts/prepare-game.sh). main 1.5.0 is generated with the decomp's
// function list (--symbols: every function start of main, and names).
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

const game::Info game::kInfo = {"botw", "The Legend of Zelda: Breath of the Wild", 0x01007EF00011E000ull,
                                "io.github.botwrecomp", "private/botw",
                                game::kSystemResourceFromNpdm /* main.npdm META +0x14 */};

// Runtime defaults for this game (config.txt still overrides them: setenv does not replace a value already set). The
// same as TOTK's where the reason is the PS4 or the engine family, not TOTK's own renderer.
void game::after_init() {
    // NVN API version: nnMain requires major 53 and minor >= 311 (main+e55304, 1.6.0) and otherwise skips its whole
    // graphics setup (then sead::Graphics::sInstance is null at the first draw-context lock).
    setenv("NX_NVN_API", "53.311", 0);
    // nvnMemoryPoolInvalidateMappedRange without waiting for the submits so far (nvn_objects.cpp): TOTK's waits kept
    // its main thread behind the whole replay.
    setenv("NX_INVALIDATE_WAIT", "0", 0);
    // Fragment-shader storage write-backs made every submit wait (TOTK: 20 -> 9 fps).
    setenv("NX_GRAPHICS_WRITE_BACK", "0", 0);
    // Depth clamp on the PS4's RADV put every clamped fragment at depth 0 (TOTK's shadow maps): clipped instead.
    setenv("NX_DEPTH_CLAMP", "0", 0);
    // Depth EQUAL kept exact. The deferred lighting classifies materials by depth: fs 4f2551d4 writes
    // FragDepth = (G-buffer material bits & mask) / 255 into a D16 target (k * 257 exactly), and each light's
    // fullscreen quad sits at z = (k / 255) * 2 - 1 and tests EQUAL. Wonder's EQUAL -> LEQUAL with a bias toward the
    // camera lets a light through on every material at or behind its own. TOTK's lighting works this way too.
    setenv("NX_EQUAL_EXACT", "1", 0);
}
void game::renderer_start() {}
void game::first_frame() {}

}  // namespace nx
