# nativeplat shell: the SDL3 player-facing app.
#
#   cmake -S shell -B build/shell -G Ninja -DNP_CORE=stub
#   cmake --build build/shell && ctest --test-dir build/shell
#
# NP_CORE selects what implements core/include/np_core.h:
#   stub  core/stub/np_core_stub.c, a test-pattern core (default until the
#         wasm2c cores land)
#   real  np_runtime + the np_guest_<game> modules core/CMakeLists.txt
#         defines, linked with np_link_guest_modules()
cmake_minimum_required(VERSION 3.20)

# An x86_64 cmake running under Rosetta would target x86_64 and fail to link
# Homebrew's arm64-only SDL3; build for the real hardware unless told
# otherwise.
if(CMAKE_HOST_APPLE AND NOT CMAKE_OSX_ARCHITECTURES AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
  execute_process(COMMAND sysctl -n hw.optional.arm64 OUTPUT_VARIABLE np_host_arm64
                  OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  if(np_host_arm64 STREQUAL "1")
    set(CMAKE_OSX_ARCHITECTURES arm64 CACHE STRING "macOS architectures")
  endif()
endif()

project(nativeplat_shell VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(NP_CORE "stub" CACHE STRING "Core implementation linked into the app: stub or real")
set_property(CACHE NP_CORE PROPERTY STRINGS stub real)

get_filename_component(NP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

find_package(SDL3 CONFIG REQUIRED)

if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
  set(NP_WARNINGS -Wall -Wextra -Wshadow -Wno-unused-parameter)
elseif(MSVC)
  set(NP_WARNINGS /W4)
endif()

# SDL-free pieces, shared with the unit tests and the stub core.
add_library(np_shell_common STATIC
  src/card.c
  src/font8x8.c
  src/layout.c
  src/launch.c
  src/net.c
  src/png.c
  src/rewind.c
  src/romdb.c
  src/scale2x.c
  src/sha1.c
  src/slots.c
  src/undo.c
)
target_include_directories(np_shell_common PUBLIC src "${NP_ROOT}/core/include")
target_compile_options(np_shell_common PRIVATE ${NP_WARNINGS})
if(WIN32)
  target_link_libraries(np_shell_common PUBLIC ws2_32) # net.c: UDP sockets
else()
  target_link_libraries(np_shell_common PUBLIC m)
endif()

# Save editor backends: save4 (D/P/Pt save data) and ndsdata (names read from
# the player's own ROM at runtime). Built from features/ with its tests off.
if(NOT TARGET save4)
  add_subdirectory("${NP_ROOT}/features" "${CMAKE_BINARY_DIR}/features")
endif()

if(NP_CORE STREQUAL "stub")
  # The stub draws with the shell's embedded font; np_font8x8 resolves from
  # np_shell_common, which the app links after the core.
  add_library(np_core_stub STATIC "${NP_ROOT}/core/stub/np_core_stub.c")
  target_include_directories(np_core_stub PRIVATE src "${NP_ROOT}/core/include")
  target_compile_options(np_core_stub PRIVATE ${NP_WARNINGS})
  set(NP_CORE_LIBS np_core_stub)
elseif(NP_CORE STREQUAL "real")
  if(NOT TARGET np_runtime)
    add_subdirectory("${NP_ROOT}/core" "${CMAKE_BINARY_DIR}/core")
  endif()
  # np_link_guest_modules() (below) links np_runtime and generates the
  # registry of guests; games without a module report np_core_available 0.
  set(NP_CORE_LIBS)
  set(NP_GUESTS)
  foreach(game diamond pearl platinum)
    if(TARGET np_guest_${game})
      list(APPEND NP_GUESTS np_guest_${game})
    endif()
  endforeach()
else()
  message(FATAL_ERROR "NP_CORE must be 'stub' or 'real', not '${NP_CORE}'")
endif()
message(STATUS "nativeplat core: ${NP_CORE} ${NP_CORE_LIBS} ${NP_GUESTS}")

add_executable(nativeplat MACOSX_BUNDLE WIN32
  src/audio.c
  src/editor.c
  src/fx.c
  src/input.c
  src/main.c
  src/options.c
  src/session.c
  src/storage.c
  src/touchpad.c
  src/ui.c
)
target_link_libraries(nativeplat PRIVATE ${NP_CORE_LIBS} np_shell_common save4 ndsdata SDL3::SDL3)
target_compile_options(nativeplat PRIVATE ${NP_WARNINGS})
if(NP_CORE STREQUAL "real")
  np_link_guest_modules(nativeplat ${NP_GUESTS})
endif()

if(WIN32)
  # VERSIONINFO for Explorer (tools/cmake/windows-x64.cmake's RC compiler or rc.exe).
  target_sources(nativeplat PRIVATE platform/windows/nativeplat.rc)
  set_source_files_properties(platform/windows/nativeplat.rc PROPERTIES COMPILE_DEFINITIONS
    "NP_VERSION_MAJOR=${PROJECT_VERSION_MAJOR};NP_VERSION_MINOR=${PROJECT_VERSION_MINOR};NP_VERSION_PATCH=${PROJECT_VERSION_PATCH};NP_VERSION_STRING=${PROJECT_VERSION}")
endif()

if(APPLE)
  set_target_properties(nativeplat PROPERTIES
    MACOSX_BUNDLE_BUNDLE_NAME "nativeplat"
    MACOSX_BUNDLE_GUI_IDENTIFIER "org.nativeplat.nativeplat"
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
    MACOSX_BUNDLE_COPYRIGHT "nativeplat contributors, GPL-3.0-or-later"
  )
  if(IOS)
    # Device and simulator builds use the Xcode generator with
    # -DCMAKE_SYSTEM_NAME=iOS and an SDL3 built for iOS.
    set_target_properties(nativeplat PROPERTIES
      MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/platform/ios/Info.plist.in"
      XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
      XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET "15.0"
      XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "org.nativeplat.nativeplat"
      XCODE_ATTRIBUTE_CODE_SIGN_STYLE "Automatic"
    )
  else()
    set_target_properties(nativeplat PROPERTIES
      MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/platform/macos/Info.plist.in"
    )
  endif()
endif()

include(CTest)
if(BUILD_TESTING)
  add_executable(np_shell_tests tests/test_shell.c)
  target_link_libraries(np_shell_tests PRIVATE np_shell_common)
  target_compile_options(np_shell_tests PRIVATE ${NP_WARNINGS})
  add_test(NAME shell_unit COMMAND np_shell_tests)

  if(NP_CORE STREQUAL "stub")
    # Full app run on SDL's dummy video driver (software renderer, no
    # window server needed): boots the stub core on a synthetic cartridge,
    # round-trips a save, renders through the real draw path, writes a PNG.
    add_test(NAME shell_autotest COMMAND nativeplat)
    set_tests_properties(shell_autotest PROPERTIES ENVIRONMENT
      "SDL_VIDEO_DRIVER=dummy;NP_AUTOTEST=frames=60,png=${CMAKE_CURRENT_BINARY_DIR}/autotest.png,layout=hybrid,touch=100x80,keys=a+up")
  endif()
endif()

if(BUILD_TESTING AND NP_CORE STREQUAL "real" AND TARGET np_guest_platinum)
  # Progression safety: new game to the first in-game save with the real
  # core, reboot to CONTINUE, parse the save. Skips without the ROM.
  set(NP_PLATINUM_ROM "${NP_ROOT}/games/platinum/build/rom/pokeplatinum.us.nds" CACHE FILEPATH
      "Platinum (US, Rev 1) ROM for the real-core shell tests (skipped when absent)")
  add_test(NAME shell_platinum_first_save
    COMMAND ${CMAKE_COMMAND}
      -DAPP=$<TARGET_FILE:nativeplat>
      -DROM=${NP_PLATINUM_ROM}
      -DPRESS=${CMAKE_CURRENT_SOURCE_DIR}/tests/platinum_first_save.press
      -DSAVE4=$<TARGET_FILE:np_save4>
      -DOUT=${CMAKE_CURRENT_BINARY_DIR}/first_save
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/first_save.cmake)
  set_tests_properties(shell_platinum_first_save PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 900)
endif()
