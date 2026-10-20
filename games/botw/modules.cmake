# Breath of the Wild's code modules (ExeFS) in rtld order: main, any subsdk*, sdk. Which subsdk modules 1.5.0 ships is
# not checked against a dump yet, so the build takes every subsdk folder scripts/prepare-game.sh generated.
set(NX_GAME_MODULES main)
foreach(i RANGE 0 9)
    if(EXISTS "${NX_GEN_DIR}/subsdk${i}")
        list(APPEND NX_GAME_MODULES subsdk${i})
    endif()
endforeach()
list(APPEND NX_GAME_MODULES sdk)
message(STATUS "BOTW modules: ${NX_GAME_MODULES}")
