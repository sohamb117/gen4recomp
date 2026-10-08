# SPDX-License-Identifier: GPL-3.0-or-later
# The save editor on a Black save, through the real app on SDL's dummy
# video driver: every tab rendered to a PNG, a money edit, a .pgf Wonder Card
# imported into the Black slots, an event flag toggled and an Add Pokemon
# (with a Black ROM), each saved and checked with np_save5; then slot import
# from the launcher's Black card and the editor on that slot.
# Inputs: APP (nativeplat), MAKE_SAVE (make_synth_save5), NP_SAVE5, WORK,
# ROM (Black ROM, optional: names and Add Pokemon).
cmake_minimum_required(VERSION 3.19)

file(REMOVE_RECURSE "${WORK}")
# The app's data lives in app/userdata/ beside portable.txt (storage.c).
set(UD "${WORK}/app/userdata")
file(MAKE_DIRECTORY "${UD}/roms")
# A portable root: autotests may only write there (storage.c).
file(COPY_FILE "${APP}" "${WORK}/app/nativeplat")
file(WRITE "${WORK}/app/portable.txt" "")
if(EXISTS "${ROM}")
    file(CREATE_LINK "${ROM}" "${UD}/roms/black.nds" SYMBOLIC)
else()
    # Only its presence matters to the launcher; the editor shows ids.
    file(WRITE "${UD}/roms/black.nds" "placeholder")
endif()

function(make out)
    execute_process(COMMAND "${MAKE_SAVE}" ${ARGN} "${out}" RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "make_synth_save5 ${ARGN} failed")
    endif()
endfunction()

# run(<step> <frames> <script> [args...]): one app run; its log in ${step}_log.
function(run step frames script)
    set(at "boot=app,frames=${frames},png=${WORK}/${step}.png")
    if(script)
        string(APPEND at ",script=${script}")
    endif()
    execute_process(COMMAND "${CMAKE_COMMAND}" -E env SDL_VIDEO_DRIVER=dummy "NP_AUTOTEST=${at}"
                            "${WORK}/app/nativeplat" ${ARGN}
        RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    file(WRITE "${WORK}/${step}.log" "${out}${err}")
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "${step}: exit ${rc}\n${out}${err}")
    endif()
    set(${step}_log "${out}${err}" PARENT_SCOPE)
endfunction()

function(expect_log step regex)
    if(NOT ${step}_log MATCHES "${regex}")
        message(FATAL_ERROR "${step}: log lacks '${regex}':\n${${step}_log}")
    endif()
endfunction()

# keys(<var> <first frame> <gap> key...): "F:key:K;..." from frame first.
function(keys var first gap)
    set(f ${first})
    set(s "")
    foreach(k ${ARGN})
        string(APPEND s "${f}:key:${k};")
        math(EXPR f "${f} + ${gap}")
    endforeach()
    set(${var} "${s}" PARENT_SCOPE)
    set(${var}_end ${f} PARENT_SCOPE)
endfunction()

function(dump out_var file)
    if(EXISTS "${ROM}")
        execute_process(COMMAND "${NP_SAVE5}" dump "${ROM}" "${file}" RESULT_VARIABLE rc OUTPUT_VARIABLE j
                        ERROR_VARIABLE e)
    else()
        execute_process(COMMAND "${NP_SAVE5}" dump "${file}" RESULT_VARIABLE rc OUTPUT_VARIABLE j ERROR_VARIABLE e)
    endif()
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "np_save5 dump ${file}: ${e}")
    endif()
    set(${out_var} "${j}" PARENT_SCOPE)
endfunction()

function(expect_json json value)
    string(JSON got GET "${json}" ${ARGN})
    if(NOT got STREQUAL value)
        message(FATAL_ERROR "JSON ${ARGN}: got '${got}', want '${value}'")
    endif()
endfunction()

set(SAV "${WORK}/black.sav")
make("${SAV}" black)
make("${WORK}/item.pgf" pgf 2 300)

# ---- every tab (standalone editor: the save says it is Black)
run(1-trainer 20 "" --editor --save "${SAV}")
expect_log(1-trainer "editor: opened black file .*\\(black\\)")
set(i 2)
foreach(tab party boxes bag pokedex events)
    math(EXPR n "${i} - 1")
    set(pd "")
    foreach(k RANGE 1 ${n})
        list(APPEND pd PageDown)
    endforeach()
    keys(s 4 2 ${pd})
    run(${i}-${tab} 24 "${s}" --editor --save "${SAV}")
    math(EXPR i "${i} + 1")
endforeach()

# ---- money: Trainer row 5 (Name, Gender, TID, SID, Money), +3, saved
keys(s 4 2 Down Down Down Down Return Up Up Up Return Escape Return)
run(8-money 40 "${s}" --editor --save "${SAV}")
expect_log(8-money "editor: saved")
dump(j "${SAV}")
expect_json("${j}" "3003" trainer money)

# ---- Events: import a .pgf (row 13 after 12 Wonder Card slots), set flag 0
keys(s 4 2 PageDown PageDown PageDown PageDown PageDown Down Down Down Down Down Down Down Down Down Down Down Down
     Return)
math(EXPR d "${s_end} + 1")
math(EXPR k "${d} + 4")
keys(s2 ${k} 2 Down Down Down Return Escape Return)
run(9-gift 60 "${s}${d}:dialog:${WORK}/item.pgf;${s2}" --editor --save "${SAV}")
expect_log(9-gift "gift import .*: Added")
expect_log(9-gift "editor: saved")
dump(j "${SAV}")
expect_json("${j}" "300" mystery_gift cards 1 id)
expect_json("${j}" "item" mystery_gift cards 1 type_name)
expect_json("${j}" "0" flags 0)
execute_process(COMMAND "${NP_SAVE5}" verify "${SAV}" RESULT_VARIABLE rc OUTPUT_VARIABLE v)
if(NOT rc EQUAL 0 OR NOT v MATCHES "all checksums valid")
    message(FATAL_ERROR "verify after editor saves: ${v}")
endif()

# ---- Add Pokemon (needs the ROM's tables): Party row 3, Tepig, level 5
if(EXISTS "${ROM}")
    keys(s 4 2 PageDown Down Down Return)
    math(EXPR t "${s_end} + 1")
    math(EXPR k "${t} + 3")
    keys(s2 ${k} 3 Return Return Escape Return)
    run(10-add-mon 60 "${s}${t}:text:Tepig;${s2}" --editor --save "${SAV}")
    expect_log(10-add-mon "Added to the party")
    dump(j "${SAV}")
    expect_json("${j}" "Tepig" party 2 species_name)
    expect_json("${j}" "5" party 2 level)
    expect_json("${j}" "ON" party 2 checksum_ok)
    expect_json("${j}" "Nuvema Town" location name)
    keys(s 4 2 PageDown)
    run(10-party-added 20 "${s}" --editor --save "${SAV}")
    message(STATUS "Add Pokemon with the Black ROM: OK")
else()
    message(STATUS "Black ROM not given (NP_BLACK_ROM): Add Pokemon not exercised")
endif()

# ---- slot import from the launcher's Black card (Diamond, Pearl, Platinum,
# Black): Slots (New, Import .sav...) -> Import -> the file dialog; then the
# new slot -> Edit save...
keys(s 4 2 Right Right Right Return Down Return)
math(EXPR d "${s_end} + 1")
run(11-slot-import 30 "${s}${d}:dialog:${WORK}/black.sav")
expect_log(11-slot-import "save import .*Imported")
if(NOT EXISTS "${UD}/saves/black/black.sav")
    file(GLOB got "${UD}/saves/black/*")
    message(FATAL_ERROR "imported slot missing: ${got}")
endif()
# Rows New, black, Import: the slot -> its menu (Play, Edit save..., ...).
keys(s 4 2 Right Right Right Return Down Return Down Return)
run(12-slot-edit 30 "${s}")
expect_log(12-slot-edit "editor: opened black slot")
message(STATUS "Black editor autotest OK; screenshots in ${WORK}")
