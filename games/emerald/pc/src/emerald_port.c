/*
 * Pokemon Emerald (pret/pokeemerald) on the GBA guest: what the machine
 * needs to know about this cartridge.
 */
#include "global.h"
#include "main.h"
#include "overworld.h"
#include "palette.h"
#include "save.h"
#include "script.h"
#include "gba_port.h"
#include "np_guest_abi.h"

static void status(uint32_t *st) {
    st[NP_STAT_FIELD_READY] = gMain.callback1 == CB1_Overworld &&
                              gMain.callback2 == CB2_Overworld && !ArePlayerFieldControlsLocked() &&
                              !gPaletteFade.active && !gMain.inBattle;
    st[NP_STAT_MAP_ID] = gSaveBlock1Ptr ? (uint32_t)gSaveBlock1Ptr->location.mapGroup << 8 |
                                              (uint8_t)gSaveBlock1Ptr->location.mapNum
                                        : 0;
    st[NP_STAT_IN_BATTLE] = gMain.inBattle;
}

static uint32_t quicksave(void) {
    return TrySavingData(SAVE_NORMAL) == SAVE_STATUS_OK ? NP_QS_SAVED : NP_QS_FAILED;
}

const gba_game_info gba_game = {
    .name = "Pokemon Emerald",
    .game_code = "BPEE",
    .save_size = 0x20000,
    .intr_table = (uint32_t)(uintptr_t)gIntrTable,
    .crt0 = &gba_crt0_emerald,
    .agb_main = AgbMain,
    .callback2 = (uint32_t *)&gMain.callback2,
    .status = status,
    .quicksave = quicksave,
};
