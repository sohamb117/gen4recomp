/*
 * Pokemon Emerald (pret/pokeemerald) on the GBA guest: what the machine
 * needs to know about this cartridge.
 */
#include "global.h"
#include "main.h"
#include "gba_port.h"

const gba_game_info gba_game = {
    .name = "Pokemon Emerald",
    .game_code = "BPEE",
    .save_size = 0x20000,
    .intr_table = (uint32_t)(uintptr_t)gIntrTable,
    .agb_main = AgbMain,
};
