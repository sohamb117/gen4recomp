# SPDX-License-Identifier: GPL-3.0-or-later
# Gen 4/5 data libraries (ROM-side ndsdata, save-side save4/save5), and CLIs.
# and their tests.
#
#   cmake -S features -B build/features -G Ninja
#   cmake --build build/features
#   ctest --test-dir build/features --output-on-failure
cmake_minimum_required(VERSION 3.16)
project(nativeplat_features C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

find_package(Python3 REQUIRED COMPONENTS Interpreter)

get_filename_component(NP_GAMES_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../games" ABSOLUTE)
set(NP_CHARMAP "${NP_GAMES_DIR}/platinum/tools/msgenc/charmap.txt" CACHE FILEPATH
    "Gen 4 charmap (pret/pokeplatinum tools/msgenc/charmap.txt)")
set(NP_VARS_FLAGS "${NP_GAMES_DIR}/platinum/generated/vars_flags.txt" CACHE FILEPATH
    "Platinum vars/flags list (pret/pokeplatinum generated/vars_flags.txt)")
set(NP_PLATINUM_ROM "${NP_GAMES_DIR}/platinum/build/rom/pokeplatinum.us.nds" CACHE FILEPATH
    "Platinum ROM used by ROM-backed tests (skipped when absent)")
set(NP_BLACK_ROM "" CACHE FILEPATH "Black ROM used by Gen 5 ROM-backed tests (skipped when empty/absent)")

if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    set(NP_WARN -Wall -Wextra -Wshadow -Wno-unused-parameter)
elseif(MSVC)
    set(NP_WARN /W4 /wd4100)
endif()

# ---------------------------------------------------------------- codegen
set(NP_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${NP_GEN_DIR}")

add_custom_command(
    OUTPUT "${NP_GEN_DIR}/gen4_charmap.c"
    COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_charmap.py"
            "${NP_CHARMAP}" "${NP_GEN_DIR}/gen4_charmap.c"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_charmap.py" "${NP_CHARMAP}"
    COMMENT "Generating Gen 4 charmap table"
    VERBATIM)

add_custom_command(
    OUTPUT "${NP_GEN_DIR}/save4_pt_varsflags.c"
    COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_varsflags.py"
            "${NP_VARS_FLAGS}" "${NP_GEN_DIR}/save4_pt_varsflags.c"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_varsflags.py" "${NP_VARS_FLAGS}"
    COMMENT "Generating Platinum event flag/var name tables"
    VERBATIM)

# ---------------------------------------------------------------- ndsdata
add_library(ndsdata STATIC
    ndsdata/src/nds_rom.c
    ndsdata/src/lz77.c
    ndsdata/src/blz.c
    ndsdata/src/gen4_text.c
    ndsdata/src/gen4_names.c
    ndsdata/src/gen4_data.c
    ndsdata/src/gen5_text.c
    ndsdata/src/gen5_data.c
    "${NP_GEN_DIR}/gen4_charmap.c")
target_include_directories(ndsdata
    PUBLIC ndsdata/include
    PRIVATE ndsdata/src)
target_compile_options(ndsdata PRIVATE ${NP_WARN})

# ------------------------------------------------------------------ save4
add_library(save4 STATIC
    save4/src/save4.c
    save4/src/pkm4.c
    "${NP_GEN_DIR}/save4_pt_varsflags.c")
target_include_directories(save4 PUBLIC save4/include)
target_link_libraries(save4 PUBLIC ndsdata)
target_compile_options(save4 PRIVATE ${NP_WARN})

# ------------------------------------------------------------------ save5
add_library(save5 STATIC
    save5/src/save5.c
    save5/src/pkm5.c)
target_include_directories(save5 PUBLIC save5/include)
target_compile_options(save5 PRIVATE ${NP_WARN})

add_executable(np_save5 tools/np_save5.c)
target_link_libraries(np_save5 PRIVATE save5 ndsdata)
target_compile_options(np_save5 PRIVATE ${NP_WARN})

add_executable(np_save4 tools/np_save4.c)
target_link_libraries(np_save4 PRIVATE save4 ndsdata)
target_compile_options(np_save4 PRIVATE ${NP_WARN})

# ------------------------------------------------------------------ tests
# The shell embeds this project for save editing (add_subdirectory); its
# tests run when this is the top-level project or on request.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(_np_features_tests_default ON)
else()
    set(_np_features_tests_default OFF)
endif()
option(NP_FEATURES_TESTS "Build and register the features tests" ${_np_features_tests_default})
if(NOT NP_FEATURES_TESTS)
    return()
endif()
enable_testing()

add_library(np_testsupport STATIC tests/synth_save.c)
target_include_directories(np_testsupport PUBLIC tests)
target_link_libraries(np_testsupport PUBLIC save4 ndsdata)
target_compile_options(np_testsupport PRIVATE ${NP_WARN})

foreach(t test_lz77 test_text test_rom_text test_pkm4 test_save4 test_gamedata)
    add_executable(${t} tests/${t}.c)
    target_link_libraries(${t} PRIVATE np_testsupport save4 ndsdata)
    target_compile_options(${t} PRIVATE ${NP_WARN})
endforeach()

add_executable(make_synth_save tests/make_synth_save.c)
target_link_libraries(make_synth_save PRIVATE np_testsupport)

add_test(NAME lz77 COMMAND test_lz77)
add_test(NAME text COMMAND test_text)
add_test(NAME rom_text COMMAND test_rom_text "${NP_PLATINUM_ROM}")
add_test(NAME pkm4 COMMAND test_pkm4)
add_test(NAME save4 COMMAND test_save4)
add_test(NAME gamedata COMMAND test_gamedata "${NP_PLATINUM_ROM}")
set_tests_properties(rom_text gamedata PROPERTIES SKIP_RETURN_CODE 77)

# Decode every message bank in the ROM and compare with the decomp's JSON.
add_executable(dump_bank tests/dump_bank.c)
target_link_libraries(dump_bank PRIVATE ndsdata)
add_test(NAME rom_text_vs_decomp
    COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_banks.py"
            $<TARGET_FILE:dump_bank> "${NP_PLATINUM_ROM}" "${NP_GAMES_DIR}/platinum")
set_tests_properties(rom_text_vs_decomp PROPERTIES SKIP_RETURN_CODE 77)

# ndsdata against a Black or White ROM (skipped when NP_BLACK_ROM is empty).
add_executable(test_gen5_rom tests/test_gen5_rom.c)
target_link_libraries(test_gen5_rom PRIVATE ndsdata)
target_compile_options(test_gen5_rom PRIVATE ${NP_WARN})
add_test(NAME gen5_rom COMMAND test_gen5_rom "${NP_BLACK_ROM}")
set_tests_properties(gen5_rom PROPERTIES SKIP_RETURN_CODE 77)

# End-to-end: build the synthetic Platinum save, dump it with real ROM names,
# edit it through the CLI and dump again.
set(NP_SYNTH_SAV "${CMAKE_CURRENT_BINARY_DIR}/synthetic_pt.sav")
add_test(NAME cli_make_save COMMAND make_synth_save pt "${NP_SYNTH_SAV}")
set_tests_properties(cli_make_save PROPERTIES FIXTURES_SETUP synth_save)
add_test(NAME cli_e2e
    COMMAND ${CMAKE_COMMAND}
        -DNP_SAVE4=$<TARGET_FILE:np_save4>
        -DMAKE_SAVE=$<TARGET_FILE:make_synth_save>
        -DROM=${NP_PLATINUM_ROM}
        -DSAV=${NP_SYNTH_SAV}
        -DWORK=${CMAKE_CURRENT_BINARY_DIR}/cli_e2e
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_e2e.cmake")
set_tests_properties(cli_e2e PROPERTIES FIXTURES_REQUIRED synth_save)

# ---- Black/White (save5)
add_library(np_testsupport5 STATIC tests/synth_save5.c)
target_include_directories(np_testsupport5 PUBLIC tests)
target_link_libraries(np_testsupport5 PUBLIC save5)
target_compile_options(np_testsupport5 PRIVATE ${NP_WARN})
foreach(t test_pkm5 test_save5)
    add_executable(${t} tests/${t}.c)
    target_link_libraries(${t} PRIVATE np_testsupport5 save5)
    target_compile_options(${t} PRIVATE ${NP_WARN})
endforeach()
add_executable(make_synth_save5 tests/make_synth_save5.c)
target_link_libraries(make_synth_save5 PRIVATE np_testsupport5)
target_compile_options(make_synth_save5 PRIVATE ${NP_WARN})
add_test(NAME pkm5 COMMAND test_pkm5)
add_test(NAME save5 COMMAND test_save5)

# End-to-end: a synthetic Black save through np_save5 (names from the Black
# ROM when NP_BLACK_ROM points at one).
set(NP_SYNTH_SAV5 "${CMAKE_CURRENT_BINARY_DIR}/synthetic_black.sav")
add_test(NAME cli5_make_save COMMAND make_synth_save5 black "${NP_SYNTH_SAV5}")
set_tests_properties(cli5_make_save PROPERTIES FIXTURES_SETUP synth_save5)
add_test(NAME cli5_e2e
    COMMAND ${CMAKE_COMMAND}
        -DNP_SAVE5=$<TARGET_FILE:np_save5>
        -DMAKE_SAVE=$<TARGET_FILE:make_synth_save5>
        -DROM=${NP_BLACK_ROM}
        -DSAV=${NP_SYNTH_SAV5}
        -DWORK=${CMAKE_CURRENT_BINARY_DIR}/cli5_e2e
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli5_e2e.cmake")
set_tests_properties(cli5_e2e PROPERTIES FIXTURES_REQUIRED synth_save5)
