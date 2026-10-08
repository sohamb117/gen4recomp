# nativeplat core: the native runtime that hosts wasm2c'd game modules.
#
# Targets:
#   np_runtime      static library: np_core API, np_host/WASI imports, fibers,
#                   guest memory, and wabt's wasm2c runtime (minus its memory
#                   allocator, which np_memory.c replaces).
#   np_guest_<game> per game, via np_add_guest_module() (cmake/NpGuestModule.cmake).
#
# Host-neutral on purpose: the shell configures this for macOS/Windows with
# Ninja or Visual Studio and for iOS with the Xcode generator and an iOS
# toolchain; only the fiber sources depend on the target system.
#
# Options:
#   NP_BOUNDS_CHECK   compile guests with explicit bounds checks (OOB becomes
#                     a reported wasm trap; slow; for debugging).
#   NP_BUILD_TESTS    build the mock guest and test_core (needs wasi-sdk).
cmake_minimum_required(VERSION 3.21)

# An x86_64 cmake (e.g. /usr/local Homebrew) runs under Rosetta on Apple
# Silicon and would make x86_64 the default target. Build for the machine
# instead, unless the caller chose (iOS toolchain, CMAKE_OSX_ARCHITECTURES,
# or CMAKE_APPLE_SILICON_PROCESSOR).
if(CMAKE_HOST_APPLE AND CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR AND NOT CMAKE_TOOLCHAIN_FILE
   AND NOT DEFINED CMAKE_SYSTEM_NAME AND NOT DEFINED CMAKE_APPLE_SILICON_PROCESSOR
   AND NOT DEFINED ENV{CMAKE_APPLE_SILICON_PROCESSOR})
  execute_process(COMMAND sysctl -n hw.optional.arm64 OUTPUT_VARIABLE _np_arm64
                  ERROR_QUIET OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(_np_arm64 STREQUAL "1")
    set(CMAKE_APPLE_SILICON_PROCESSOR arm64 CACHE STRING "Host processor on Apple Silicon")
  endif()
endif()

project(nativeplat_core LANGUAGES C)

# GNU-syntax fiber switches: every target but MSVC (which keeps Win32 Fibers
# and has no snapshots).
if(NOT MSVC)
  enable_language(ASM)
endif()

if(PROJECT_IS_TOP_LEVEL AND NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

set(NP_CORE_DIR "${CMAKE_CURRENT_SOURCE_DIR}" CACHE INTERNAL "")
get_filename_component(_np_repo "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

set(NP_WABT_ROOT "${_np_repo}/.cache/toolchains/wabt" CACHE PATH "wabt install (tools/fetch_toolchains.sh)")
set(NP_WASI_SDK "${_np_repo}/.cache/toolchains/wasi-sdk" CACHE PATH "wasi-sdk install (tools/fetch_toolchains.sh)")
# wasm2c runs on the build machine, so search the host-side toolchain even
# when cross-compiling the runtime for iOS.
find_program(NP_WASM2C wasm2c HINTS "${NP_WABT_ROOT}/bin" NO_CMAKE_FIND_ROOT_PATH)

option(NP_BOUNDS_CHECK "Bounds-check every guest memory access (debug; slow)" OFF)
option(NP_BUILD_TESTS "Build the core runtime tests (needs wasi-sdk)" ${PROJECT_IS_TOP_LEVEL})

set(_wabt_rt "${NP_WABT_ROOT}/share/wabt/wasm2c")
if(NOT EXISTS "${_wabt_rt}/wasm-rt-impl.c" OR NOT EXISTS "${NP_WABT_ROOT}/include/wasm-rt.h")
  message(FATAL_ERROR "wabt runtime not found under NP_WABT_ROOT=${NP_WABT_ROOT}; run tools/fetch_toolchains.sh")
endif()

include(cmake/NpGuestModule.cmake)

set(_np_fiber_sources)
if(CMAKE_SYSTEM_NAME STREQUAL "Windows" AND MSVC)
  list(APPEND _np_fiber_sources runtime/np_fiber_win32.c)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
  # Own stacks with an x64 switch, so snapshots can capture parked fibers.
  list(APPEND _np_fiber_sources runtime/np_fiber_win64.c runtime/np_fiber_win64.S)
else()
  # Each switch file assembles to nothing for other architectures, so
  # universal builds (CMAKE_OSX_ARCHITECTURES "arm64;x86_64") list both.
  list(APPEND _np_fiber_sources runtime/np_fiber_posix.c)
  set(_np_archs "${CMAKE_OSX_ARCHITECTURES}")
  if(NOT _np_archs)
    set(_np_archs "${CMAKE_SYSTEM_PROCESSOR}")
  endif()
  set(_np_any_arch ON)
  if(_np_archs MATCHES "arm64|aarch64|ARM64")
    list(APPEND _np_fiber_sources runtime/np_fiber_arm64.S)
    set(_np_any_arch OFF)
  endif()
  if(_np_archs MATCHES "x86_64|AMD64|amd64")
    list(APPEND _np_fiber_sources runtime/np_fiber_x86_64.S)
    set(_np_any_arch OFF)
  endif()
  if(_np_any_arch)
    list(APPEND _np_fiber_sources runtime/np_fiber_arm64.S runtime/np_fiber_x86_64.S)
  endif()
endif()

add_library(np_runtime STATIC
  runtime/np_core.c
  runtime/np_imports.c
  runtime/np_wasi.c
  runtime/np_memory.c
  runtime/np_snapshot.c
  ${_np_fiber_sources}
  "${_wabt_rt}/wasm-rt-impl.c"
  "${_wabt_rt}/wasm-rt-exceptions-impl.c")
set_target_properties(np_runtime PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_include_directories(np_runtime
  PUBLIC include runtime "${NP_WABT_ROOT}/include"
  PRIVATE "${_wabt_rt}")

# The wasm-rt configuration must be identical for the runtime and every
# generated module, so it is PUBLIC. No signal handler (it would fight the
# shell's crash reporting and Windows SEH), unchecked native stack depth
# (fiber stacks have guard pages), and either guard-page mode without forced
# reads (no bounds checks at all) or explicit bounds checks.
set(_np_wasm_rt_defs
  WASM_RT_SKIP_SIGNAL_RECOVERY=1
  WASM_RT_NONCONFORMING_UNCHECKED_STACK_EXHAUSTION=1)
if(NP_BOUNDS_CHECK)
  list(APPEND _np_wasm_rt_defs WASM_RT_MEMCHECK_BOUNDS_CHECK=1 NP_BOUNDS_CHECK=1)
else()
  list(APPEND _np_wasm_rt_defs WASM_RT_MEMCHECK_GUARD_PAGES=1 WASM_RT_NONCONFORMING_ALLOW_OOB_READ_ELIMINATION=1)
endif()
target_compile_definitions(np_runtime
  PUBLIC ${_np_wasm_rt_defs}
  # Traps go to np_core (switch to the driver fiber), never to longjmp.
  PRIVATE WASM_RT_TRAP_HANDLER=np_rt_wasm_trap)
if(MSVC)
  target_compile_options(np_runtime PRIVATE /W3)
else()
  target_compile_options(np_runtime PRIVATE $<$<COMPILE_LANGUAGE:C>:-Wall -Wextra -fno-strict-aliasing>)
  set_source_files_properties("${_wabt_rt}/wasm-rt-impl.c" "${_wabt_rt}/wasm-rt-exceptions-impl.c"
    PROPERTIES COMPILE_OPTIONS "-w")
endif()

if(NP_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# Real game modules. Each NP_GUEST_WASM_<game> names a .wasm built by the
# game's own build (e.g. games/platinum/build/pc-wasm/pokeplatinum.wasm);
# when set, np_guest_<game> is defined here, which the shell picks up, and
# np_headless is built to run it without SDL. Post-processing of the
# wasm2c output comes from NP_GUEST_POSTPROCESS[_<game>] (default none).
set(NP_GUEST_NUM_OUTPUTS 8 CACHE STRING "wasm2c --num-outputs for game modules (parallel compile)")
set(_np_real_guests)
foreach(_game diamond pearl platinum black white heartgold soulsilver ruby sapphire emerald)
  set(NP_GUEST_WASM_${_game} "" CACHE FILEPATH "wasm2c input for the ${_game} core (empty: not built)")
  if(NP_GUEST_WASM_${_game})
    get_filename_component(_wasm "${NP_GUEST_WASM_${_game}}" ABSOLUTE)
    np_add_guest_module(${_game} "${_wasm}" NUM_OUTPUTS ${NP_GUEST_NUM_OUTPUTS})
    list(APPEND _np_real_guests np_guest_${_game})
  endif()
endforeach()
if(_np_real_guests AND PROJECT_IS_TOP_LEVEL)
  # --net borrows the shell's UDP local-wireless transport (SDL-free).
  add_executable(np_headless tools/np_headless.c "${CMAKE_CURRENT_SOURCE_DIR}/../shell/src/net.c")
  target_include_directories(np_headless PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../shell/src")
  set_target_properties(np_headless PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
  np_link_guest_modules(np_headless ${_np_real_guests})
  if(NOT MSVC)
    target_link_libraries(np_headless PRIVATE m)
  endif()
  if(WIN32)
    target_link_libraries(np_headless PRIVATE ws2_32)
  endif()
endif()
