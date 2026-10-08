# SPDX-License-Identifier: GPL-3.0-or-later
# The save editor on synthetic HeartGold/SoulSilver saves, through the real
# app on SDL's dummy video driver: every tab rendered to a PNG, a Kanto badge
# toggled, a Wonder Card (.pcd) imported, each saved and checked with
# np_save4; a SoulSilver save identifies itself; slot import from the
# launcher's HeartGold card and the editor on that slot.
# Inputs: APP (nativeplat), MAKE_SAVE (make_synth_save), NP_SAVE4, WORK.
cmake_minimum_required(VERSION 3.21)

set(GAME_ID heartgold)
set(ROM "") # no HG/SS ROM: the editor shows ids
include("${CMAKE_CURRENT_LIST_DIR}/editor_common.cmake")

set(SAV "${WORK}/hg.sav")
make("${SAV}" hg)
make("${WORK}/ss.sav" ss)
make("${WORK}/gift.pcd" card 1)

# ---- every tab (standalone editor: the save says it is HeartGold)
tabs("" "${SAV}")
expect_log(1-trainer "editor: opened heartgold file .*\\(heartgold\\)")
run(ss 12 "" --editor --save "${WORK}/ss.sav")
expect_log(ss "editor: opened soulsilver file .*\\(soulsilver\\)")

# ---- Trainer: Name, Gender, TID, SID, Money, Coins, 8 Johto badges, then
# the Kanto badges: row 21 is the Earth Badge (bit 7)
set(down "")
foreach(k RANGE 1 21)
    list(APPEND down Down)
endforeach()
keys(s 4 2 ${down} Return Escape Return)
run(7-kanto 70 "${s}" --editor --save "${SAV}")
expect_log(7-kanto "editor: saved")
dump(j "${NP_SAVE4}" "${SAV}")
expect_json("${j}" "heartgold" game)
expect_json("${j}" "131" trainer kanto_badge_mask) # synthetic 0x03 + Earth

# ---- Events: MYSTERY GIFT, Pokedex obtained, 3 Wonder Cards, gifts waiting,
# then Import .pgt / .pcd... (row 6; HG/SS take none of the D/P/Pt event cards)
keys(s 4 2 PageDown PageDown PageDown PageDown PageDown Down Down Down Down Down Down Return)
math(EXPR d "${s_end} + 1")
math(EXPR k "${d} + 4")
keys(s2 ${k} 2 Escape Return)
run(8-gift 50 "${s}${d}:dialog:${WORK}/gift.pcd;${s2}" --editor --save "${SAV}")
expect_log(8-gift "gift import .*: Added")
expect_log(8-gift "editor: saved")
dump(j "${NP_SAVE4}" "${SAV}")
expect_json("${j}" "pokemon" mystery_gift cards 0 type_name)
execute_process(COMMAND "${NP_SAVE4}" verify "${SAV}" RESULT_VARIABLE rc OUTPUT_VARIABLE v)
if(NOT rc EQUAL 0 OR NOT v MATCHES "all checksums valid")
    message(FATAL_ERROR "verify after editor saves: ${v}")
endif()

# ---- slot import from the launcher's HeartGold card (Diamond, Pearl,
# Platinum, Black, White, HeartGold), then that slot -> Edit save...
keys(s 4 2 Right Right Right Right Right Return Down Return)
math(EXPR d "${s_end} + 1")
run(9-slot-import 30 "${s}${d}:dialog:${SAV}")
expect_log(9-slot-import "save import .*Imported")
if(NOT EXISTS "${UD}/saves/heartgold/hg.sav")
    file(GLOB got "${UD}/saves/heartgold/*")
    message(FATAL_ERROR "imported slot missing: ${got}")
endif()
keys(s 4 2 Right Right Right Right Right Return Down Return Down Return)
run(10-slot-edit 30 "${s}")
expect_log(10-slot-edit "editor: opened heartgold slot")
message(STATUS "HeartGold/SoulSilver editor autotest OK; screenshots in ${WORK}")
