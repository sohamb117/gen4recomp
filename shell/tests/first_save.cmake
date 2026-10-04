# End-to-end progression-safety check with the real Platinum core:
#
#  1. Play a new game from power-on to the first in-game save
#     (platinum_first_save.press) with the save slot on portable storage.
#  2. Check the game stored its backup chip through the shell (save_stores)
#     into saves/platinum/<slot>.sav, written by temp file + rename.
#  3. Boot the slot again and check the title menu loaded it (save_loads)
#     and shows CONTINUE (screenshot kept for inspection).
#  4. Parse the save with np_save4 and check the trainer name.
#
# Skips (exit code 77) when the ROM is absent. Invoked by CTest with
# -DAPP= -DROM= -DPRESS= -DSAVE4= -DOUT= (see shell/CMakeLists.txt).
cmake_minimum_required(VERSION 3.20)

if(NOT EXISTS "${ROM}")
  message("SKIP: no Platinum ROM at ${ROM}")
  cmake_language(EXIT 77)
endif()

# The app's portable root for a bundle is the folder holding the .app; for a
# plain executable, the folder holding it.
string(REGEX REPLACE "/[^/]+\\.app/Contents/MacOS/[^/]+$" "" root "${APP}")
if(root STREQUAL APP)
  get_filename_component(root "${APP}" DIRECTORY)
endif()
set(slot "First Save Regression")
set(sav "${root}/userdata/saves/platinum/${slot}.sav")
if(NOT EXISTS "${root}/portable.txt")
  file(WRITE "${root}/portable.txt" "Created by shell/tests/first_save.cmake: keep test data in userdata/.\n")
endif()
file(REMOVE "${sav}" "${sav}.bak" "${sav}.tmp")
file(MAKE_DIRECTORY "${OUT}")

function(run_app label spec out_var)
  execute_process(
    COMMAND ${CMAKE_COMMAND} -E env SDL_VIDEO_DRIVER=dummy "NP_AUTOTEST=${spec}" "${APP}"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 600)
  set(log "${out}${err}")
  string(REGEX MATCH "autotest: boot=[^\n]*" summary "${log}")
  message("${label}: ${summary}")
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${label} failed (exit ${rc}):\n${log}")
  endif()
  set(${out_var} "${summary}" PARENT_SCOPE)
endfunction()

set(common "size=512x192,layout=horizontal,rom=${ROM},storage=1,slot=${slot}")
run_app("new game" "frames=12700,png=${OUT}/first_save_bedroom.png,${common},press=@${PRESS}" s1)
string(REGEX MATCH "save_stores=([0-9]+)" _ "${s1}")
if(NOT CMAKE_MATCH_1 OR CMAKE_MATCH_1 LESS 1)
  message(FATAL_ERROR "the game never stored its save")
endif()
file(SIZE "${sav}" size)
if(NOT size EQUAL 524288)
  message(FATAL_ERROR "${sav} is ${size} bytes, expected 524288")
endif()
if(EXISTS "${sav}.tmp")
  message(FATAL_ERROR "a temporary file was left behind: ${sav}.tmp")
endif()

run_app("continue" "frames=1760,png=${OUT}/first_save_continue.png,${common},press=1450:start:4:40:4" s2)
if(NOT s2 MATCHES "save_loads=1 ")
  message(FATAL_ERROR "the second boot did not load the slot")
endif()

execute_process(COMMAND "${SAVE4}" dump "${ROM}" "${sav}" RESULT_VARIABLE rc OUTPUT_VARIABLE dump ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "np_save4 could not parse the save:\n${err}")
endif()
string(REGEX MATCH "\"trainer\": [^\n]*" trainer "${dump}")
message("np_save4: ${trainer}")
if(NOT trainer MATCHES "\"name\": \"NATIVE\"")
  message(FATAL_ERROR "unexpected trainer: ${trainer}")
endif()
message("first save OK: ${sav}")
