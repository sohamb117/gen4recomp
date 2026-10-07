/*
 * Pokemon Ruby and Sapphire (pret/pokeruby, US 1.0) on the GBA guest: what
 * the machine needs to know about the cartridge. One source, built twice
 * (-DRUBY / -DSAPPHIRE, as pokeruby's Makefile does).
 */
#include "global.h"
#include "main.h"
#include "palette.h"
#include "save.h"
#include "script.h"
#include "gba_port.h"
#include "np_guest_abi.h"

GBA_LOCAL_DECL(overworld, CB1_Overworld);
GBA_LOCAL_DECL(overworld, CB2_Overworld);
extern IntrFunc gIntrTable[]; /* main.c; pokeruby's main.h does not declare it */

static void status(uint32_t *st) {
    st[NP_STAT_FIELD_READY] = gMain.callback1 == (MainCallback)GBA_LOCAL(overworld, CB1_Overworld) &&
                              gMain.callback2 == (MainCallback)GBA_LOCAL(overworld, CB2_Overworld) &&
                              !ArePlayerFieldControlsLocked() && !gPaletteFade.active && !gMain.inBattle;
    st[NP_STAT_MAP_ID] = (uint32_t)gSaveBlock1.location.mapGroup << 8 | (uint8_t)gSaveBlock1.location.mapNum;
    st[NP_STAT_IN_BATTLE] = gMain.inBattle;
}

static uint32_t quicksave(void) {
    return Save_WriteData(SAVE_NORMAL) == SAVE_STATUS_OK ? NP_QS_SAVED : NP_QS_FAILED;
}

const gba_game_info gba_game = {
#ifdef SAPPHIRE
    .name = "Pokemon Sapphire",
    .game_code = "AXPE",
#else
    .name = "Pokemon Ruby",
    .game_code = "AXVE",
#endif
    .save_size = 0x20000,
    .intr_table = (uint32_t)(uintptr_t)gIntrTable,
    .crt0 = &gba_crt0_ruby,
    .agb_main = AgbMain,
    .callback2 = (uint32_t *)&gMain.callback2,
    .status = status,
    .quicksave = quicksave,
};
