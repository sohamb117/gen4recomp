# SPDX-License-Identifier: GPL-3.0-or-later
# Gen 4 data libraries (ROM-side ndsdata, save-side save4), the np_save4 CLI
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
    ndsdata/src/gen4_text.c
    ndsdata/src/gen4_names.c
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

# -------------------------------------------------------------------- CLI
add_executable(np_save4 tools/np_save4.c)
target_link_libraries(np_save4 PRIVATE save4 ndsdata)
target_compile_options(np_save4 PRIVATE ${NP_WARN})

# ------------------------------------------------------------------ tests
enable_testing()

add_library(np_testsupport STATIC tests/synth_save.c)
target_include_directories(np_testsupport PUBLIC tests)
target_link_libraries(np_testsupport PUBLIC save4 ndsdata)
target_compile_options(np_testsupport PRIVATE ${NP_WARN})

foreach(t test_lz77 test_text test_rom_text test_pkm4 test_save4)
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
set_tests_properties(rom_text PROPERTIES SKIP_RETURN_CODE 77)

# Decode every message bank in the ROM and compare with the decomp's JSON.
add_executable(dump_bank tests/dump_bank.c)
target_link_libraries(dump_bank PRIVATE ndsdata)
add_test(NAME rom_text_vs_decomp
    COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_banks.py"
            $<TARGET_FILE:dump_bank> "${NP_PLATINUM_ROM}" "${NP_GAMES_DIR}/platinum")
set_tests_properties(rom_text_vs_decomp PROPERTIES SKIP_RETURN_CODE 77)

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
