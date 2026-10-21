@echo off
rem Native Windows (x64) build of BOTW for debugging the renderer on a PC GPU (WINDOWS.md), as TOTK's in E:\totk-win.
rem   build-win.cmd [configure]
rem Needs VS 2026 with "C++ Clang tools for Windows" (clang-cl), CMake, Ninja. Layout (BOTW_WIN, default E:\botw-win):
rem   recomp\  clone of ~/botw-build/src/recomp (branch botw)    gen\{main,subsdk0,sdk}  the generated code
rem   third_party\Vulkan-Headers-1.4.357, third_party\opus (libopus 1.5.2 built with clang-cl /arch:AVX2, static)
rem   build\  this build      data\BOTW  the PS4 data stage
rem JOBS (default 8): each generated file takes up to ~1.5 GB of memory to compile.
setlocal
if "%BOTW_WIN%"=="" set BOTW_WIN=E:\botw-win
if "%JOBS%"=="" set JOBS=8
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if "%1"=="configure" goto configure
if exist "%BOTW_WIN%\build\build.ninja" goto build
:configure
cmake -S "%BOTW_WIN%\recomp" -B "%BOTW_WIN%\build" -G Ninja -DNX_GAME=botw -DCMAKE_BUILD_TYPE=Release ^
    -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl ^
    -DNX_GEN_DIR="%BOTW_WIN%/gen" -DNX_VULKAN_SDK="%BOTW_WIN%/third_party/Vulkan-Headers-1.4.357" ^
    -DNX_OPUS_DIR="%BOTW_WIN%/third_party/opus" || exit /b 1
:build
ninja -C "%BOTW_WIN%\build" -j%JOBS% -k 0 botw
