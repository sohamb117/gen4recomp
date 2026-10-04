# Windows x64 (x86_64-w64-mingw32, UCRT) cross toolchain built on zig.
#
#   tools/fetch_toolchains.sh windows
#   cmake -S core -B build/win-core -G Ninja \
#         -DCMAKE_TOOLCHAIN_FILE=$PWD/tools/cmake/windows-x64.cmake
#
# zig cc is clang + lld + mingw-w64 headers and CRT, built on first use into
# .cache/zig. The wrappers in zig/ fix the target and give CMake the
# single-binary compiler, archiver and resource compiler it expects; the
# resource compiler is named windres so CMake drives it windres-style and
# links the COFF object it writes.
#
# SDL3 resolves to the official mingw development package
# (.cache/toolchains/sdl3-mingw), so find_package(SDL3 CONFIG) works as on
# the native platforms; SDL3.dll is in ${NP_SDL3_MINGW}/x86_64-w64-mingw32/bin.
#
# Override with -DNP_ZIG_WRAPPERS=... or the NP_ZIG / ZIG_GLOBAL_CACHE_DIR
# environment variables the wrappers read.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

get_filename_component(_np_repo "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(NP_ZIG_WRAPPERS "${CMAKE_CURRENT_LIST_DIR}/zig" CACHE PATH "zig compiler wrappers")
set(NP_SDL3_MINGW "${_np_repo}/.cache/toolchains/sdl3-mingw" CACHE PATH "SDL3 mingw development package")

set(CMAKE_C_COMPILER "${NP_ZIG_WRAPPERS}/x86_64-windows-gnu-cc")
set(CMAKE_ASM_COMPILER "${NP_ZIG_WRAPPERS}/x86_64-windows-gnu-cc")
set(CMAKE_AR "${NP_ZIG_WRAPPERS}/zig-ar" CACHE FILEPATH "archiver")
set(CMAKE_RANLIB "${NP_ZIG_WRAPPERS}/zig-ranlib" CACHE FILEPATH "ranlib")
set(CMAKE_RC_COMPILER "${NP_ZIG_WRAPPERS}/windres")

# Search only the SDL3 package for target libraries and packages; host
# programs (wasm2c, python) come from the build machine.
set(CMAKE_FIND_ROOT_PATH "${NP_SDL3_MINGW}/x86_64-w64-mingw32" "${NP_SDL3_MINGW}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
