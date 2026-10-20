#!/usr/bin/env bash
# Stand-in "generated code" for building the runtime without a game dump: empty main and sdk modules (no functions,
# relocations or symbols; no subsdk, so games/botw/modules.cmake lists main and sdk), so the whole PS4 runtime
# compiles and links into an eboot. That eboot stops at
# boot ("missing .../analysis/main.bin" or "nn::init::Start not found" in nx.log); it is for checking the port, not
# for playing.
#   make-stub-gen.sh [gen dir]     (default ~/botw-build/stub-gen)
set -euo pipefail
GEN="${1:-$HOME/botw-build/stub-gen}"
rm -rf "$GEN"/subsdk*
for mod in main sdk; do
    mkdir -p "$GEN/$mod"
    case "$mod" in
        main) off=0x0 ;;
        sdk) off=0x10000000 ;;
    esac
    cat > "$GEN/$mod/table.cpp" <<EOF
// Stub module (scripts/make-stub-gen.sh): no recompiled code.
#include <nx/module.h>

extern const nx::ModuleInfo nx_module_$mod = {
    "$mod", NX_BASE + ${off}ull,
    0, 0, 0, 0, 0, 0, 0, 0,
    nullptr, 0,
    nullptr, 0,
    nullptr, 0,
    nullptr, 0, nullptr, 0,
};
EOF
done
echo "$GEN"
