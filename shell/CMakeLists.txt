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
  src/json.c
  src/layout.c
  src/launch.c
  src/lowpass.c
  src/modpkg.c
  src/net.c
  src/png.c
  src/release.c
  src/rewind.c
  src/romdb.c
  src/scale2x.c
  src/sha1.c
  src/sha256.c
  src/skinfmt.c
  src/slots.c
  src/sync_plan.c
  src/touchlayout.c
  src/undo.c
  src/zip.c
)
target_include_directories(np_shell_common PUBLIC src "${NP_ROOT}/core/include")
target_compile_options(np_shell_common PRIVATE ${NP_WARNINGS})
if(WIN32)
  target_link_libraries(np_shell_common PUBLIC ws2_32) # net.c: UDP sockets
else()
  target_link_libraries(np_shell_common PUBLIC m)
endif()

# Save editor backends: save4 (D/P/Pt save data), save5 (B/W) and ndsdata
# (names read from the player's own ROM at runtime), built from features/
# with its tests off, behind one interface (src/edsave.c, SDL-free).
if(NOT TARGET save4)
  add_subdirectory("${NP_ROOT}/features" "${CMAKE_BINARY_DIR}/features")
endif()
add_library(np_shell_save STATIC src/edsave.c)
target_include_directories(np_shell_save PUBLIC src "${NP_ROOT}/core/include")
target_link_libraries(np_shell_save PUBLIC save4 save5 ndsdata)
target_compile_options(np_shell_save PRIVATE ${NP_WARNINGS})

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
  foreach(game diamond pearl platinum black white heartgold soulsilver ruby sapphire emerald)
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
  src/carts.c
  src/editor.c
  src/fx.c
  src/input.c
  src/main.c
  src/mods.c
  src/options.c
  src/session.c
  src/skin.c
  src/storage.c
  src/sync.c
  src/touchpad.c
  src/ui.c
  src/update.c
)
target_link_libraries(nativeplat PRIVATE ${NP_CORE_LIBS} np_shell_common np_shell_save SDL3::SDL3)
target_compile_options(nativeplat PRIVATE ${NP_WARNINGS})
if(NP_CORE STREQUAL "real")
  np_link_guest_modules(nativeplat ${NP_GUESTS})
endif()

# Updater (src/update.c): the GitHub repository whose releases it offers,
# "owner/name"; empty hides the feature. The HTTP backend is the OS's own:
# libcurl from the macOS SDK, WinHTTP on Windows; iOS gets none (App Store).
set(NP_UPDATE_REPO "" CACHE STRING "GitHub owner/name the updater checks; empty = no updater")
target_compile_definitions(nativeplat PRIVATE "NP_UPDATE_REPO=\"${NP_UPDATE_REPO}\""
  "NP_VERSION_STRING=\"${PROJECT_VERSION}\"")
if(APPLE AND NOT IOS)
  target_sources(nativeplat PRIVATE src/http_curl.c)
  target_link_libraries(nativeplat PRIVATE curl)
  target_compile_definitions(nativeplat PRIVATE NP_HAVE_HTTP)
elseif(WIN32)
  target_sources(nativeplat PRIVATE src/http_winhttp.c)
  target_link_libraries(nativeplat PRIVATE winhttp)
  target_compile_definitions(nativeplat PRIVATE NP_HAVE_HTTP)
endif()

# Delta skins with PDF art: CoreGraphics rasterizes them on Apple platforms
# (src/pdfraster_apple.c); elsewhere only PNG art is accepted.
if(APPLE)
  target_sources(nativeplat PRIVATE src/pdfraster_apple.c)
  target_link_libraries(nativeplat PRIVATE "-framework CoreGraphics")
  target_compile_definitions(nativeplat PRIVATE NP_HAVE_PDF)
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
  # The save-interface tests build synthetic saves with features/tests' builders.
  add_executable(np_shell_tests tests/test_shell.c "${NP_ROOT}/features/tests/synth_save.c"
    "${NP_ROOT}/features/tests/synth_save5.c")
  target_include_directories(np_shell_tests PRIVATE "${NP_ROOT}/features/tests")
  target_link_libraries(np_shell_tests PRIVATE np_shell_common np_shell_save)
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

  # The save editor on synthetic Black and HeartGold/SoulSilver saves (stub
  # core: the editor needs no core): tabs, edits, gift imports, slot import;
  # checked with np_save5 / np_save4.
  if(NP_CORE STREQUAL "stub")
    add_executable(np_make_synth_save5 "${NP_ROOT}/features/tests/make_synth_save5.c"
      "${NP_ROOT}/features/tests/synth_save5.c")
    target_include_directories(np_make_synth_save5 PRIVATE "${NP_ROOT}/features/tests")
    target_link_libraries(np_make_synth_save5 PRIVATE save5)
    set(NP_BLACK_ROM "" CACHE FILEPATH "Black ROM for the editor test's names and Add Pokemon (optional)")
    add_test(NAME shell_editor_bw
      COMMAND ${CMAKE_COMMAND}
        -DAPP=$<TARGET_FILE:nativeplat>
        -DMAKE_SAVE=$<TARGET_FILE:np_make_synth_save5>
        -DNP_SAVE5=$<TARGET_FILE:np_save5>
        -DROM=${NP_BLACK_ROM}
        -DWORK=${CMAKE_CURRENT_BINARY_DIR}/editor_bw
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/editor_bw.cmake)
    set_tests_properties(shell_editor_bw PROPERTIES TIMEOUT 300)

    add_executable(np_make_synth_save "${NP_ROOT}/features/tests/make_synth_save.c"
      "${NP_ROOT}/features/tests/synth_save.c")
    target_include_directories(np_make_synth_save PRIVATE "${NP_ROOT}/features/tests")
    target_link_libraries(np_make_synth_save PRIVATE save4 ndsdata)
    add_test(NAME shell_editor_hgss
      COMMAND ${CMAKE_COMMAND}
        -DAPP=$<TARGET_FILE:nativeplat>
        -DMAKE_SAVE=$<TARGET_FILE:np_make_synth_save>
        -DNP_SAVE4=$<TARGET_FILE:np_save4>
        -DROM=${NP_HGSS_ROM}
        -DWORK=${CMAKE_CURRENT_BINARY_DIR}/editor_hgss
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/editor_hgss.cmake)
    set_tests_properties(shell_editor_hgss PROPERTIES TIMEOUT 300)
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
