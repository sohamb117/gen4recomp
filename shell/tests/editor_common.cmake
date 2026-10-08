# SPDX-License-Identifier: GPL-3.0-or-later
# Shared by the save editor autotests (editor_bw.cmake, editor_hgss.cmake):
# a portable copy of the app in ${WORK}/app and helpers to run it.
# Inputs: APP (nativeplat), MAKE_SAVE, WORK, GAME_ID (roms/<id>.nds), ROM
# (optional: linked as the imported ROM, else a placeholder file).

file(REMOVE_RECURSE "${WORK}")
# The app's data lives in app/userdata/ beside portable.txt; autotests may
# only write to such a portable root (storage.c).
set(UD "${WORK}/app/userdata")
file(MAKE_DIRECTORY "${UD}/roms")
file(COPY_FILE "${APP}" "${WORK}/app/nativeplat")
file(WRITE "${WORK}/app/portable.txt" "")
if(EXISTS "${ROM}")
    file(CREATE_LINK "${ROM}" "${UD}/roms/${GAME_ID}.nds" SYMBOLIC)
else()
    # Only its presence matters to the launcher; the editor shows ids.
    file(WRITE "${UD}/roms/${GAME_ID}.nds" "placeholder")
endif()

function(make out)
    execute_process(COMMAND "${MAKE_SAVE}" ${ARGN} "${out}" RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "${MAKE_SAVE} ${ARGN} failed")
    endif()
endfunction()

# run(<step> <frames> <script> [args...]): one app run; its log in ${WORK}/${step}.log.
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
endfunction()

function(expect_log step regex)
    file(READ "${WORK}/${step}.log" log)
    if(NOT log MATCHES "${regex}")
        message(FATAL_ERROR "${step}: log lacks '${regex}':\n${log}")
    endif()
endfunction()

# keys(<var> <first frame> <gap> key...): "F:key:K;..." from frame first;
# ${var}_end is the frame after the last key.
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

# dump(<var> <tool> <file>): the tool's JSON dump (names when ROM is given).
function(dump out_var tool file)
    if(EXISTS "${ROM}")
        execute_process(COMMAND "${tool}" dump "${ROM}" "${file}" RESULT_VARIABLE rc OUTPUT_VARIABLE j
                        ERROR_VARIABLE e)
    else()
        execute_process(COMMAND "${tool}" dump "${file}" RESULT_VARIABLE rc OUTPUT_VARIABLE j ERROR_VARIABLE e)
    endif()
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "${tool} dump ${file}: ${e}")
    endif()
    set(${out_var} "${j}" PARENT_SCOPE)
endfunction()

function(expect_json json value)
    string(JSON got GET "${json}" ${ARGN})
    if(NOT got STREQUAL value)
        message(FATAL_ERROR "JSON ${ARGN}: got '${got}', want '${value}'")
    endif()
endfunction()

# tabs(<prefix> <save>): every editor tab of the standalone editor to a PNG,
# ${prefix}1-trainer.png .. ${prefix}6-events.png.
function(tabs prefix save)
    set(i 1)
    foreach(tab trainer party boxes bag pokedex events)
        math(EXPR n "${i} - 1")
        set(pd "")
        if(n GREATER 0)
            foreach(k RANGE 1 ${n})
                list(APPEND pd PageDown)
            endforeach()
        endif()
        keys(s 4 2 ${pd})
        run(${prefix}${i}-${tab} 24 "${s}" --editor --save "${save}")
        math(EXPR i "${i} + 1")
    endforeach()
endfunction()

# export_cards(<save> <card row>): the Trainer tab's "Export Trainer Card
# PNG..." (row <card row>) and "Export Pokedex diploma PNG..." (the next
# row) through the file dialog; both PNGs must be written.
function(export_cards save row)
    foreach(kind card diploma)
        set(down "")
        foreach(k RANGE 1 ${row})
            list(APPEND down Down)
        endforeach()
        keys(s 4 2 ${down} Return)
        math(EXPR d "${s_end} + 2")
        math(EXPR f "${d} + 8")
        run(export-${kind} ${f} "${s}${d}:dialog:${WORK}/trainer-${kind}.png" --editor --save "${save}")
        if(NOT EXISTS "${WORK}/trainer-${kind}.png")
            message(FATAL_ERROR "no ${kind} PNG written")
        endif()
        file(SIZE "${WORK}/trainer-${kind}.png" n)
        if(n LESS 1000)
            message(FATAL_ERROR "${kind} PNG is ${n} bytes")
        endif()
        math(EXPR row "${row} + 1")
    endforeach()
endfunction()
