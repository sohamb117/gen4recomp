/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * ndsdata against a HeartGold or SoulSilver ROM: game detection, names
 * (a/0/2/7 banks 237/750/222/720/34/279/281/280), species, experience and
 * move tables, spot-checked against the pokeheartgold text and data files.
 * Skips (77) when no ROM path is given (NP_HGSS_ROM) or it is absent.
 */
#include <stdio.h>
#include <stdlib.h>

#include "ndsdata/ndsdata.h"
#include "testutil.h"

int main(int argc, char **argv)
{
    if (argc < 2 || !argv[1][0]) {
        printf("SKIP: no HeartGold/SoulSilver ROM (set NP_HGSS_ROM)\n");
        return 77;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        printf("SKIP: ROM not found at %s\n", argv[1]);
        return 77;
    }
    fseek(f, 0, SEEK_END);
    nd_rom rom;
    nd_status st = nd_rom_open(&rom, nd_read_stdio, f, (uint64_t)ftell(f));
    CHECK(st == ND_OK);
    if (st != ND_OK)
        return TEST_RESULT();
    CHECK(rom.game == ND_GAME_HEARTGOLD || rom.game == ND_GAME_SOULSILVER);
    CHECK_EQ_INT(nd_game_gen(rom.game), 4);
    CHECK_EQ_STR(nd_game_name(rom.game), rom.game == ND_GAME_HEARTGOLD ? "HeartGold" : "SoulSilver");

    /* ---- names (files/msgdata/msg/msg_NNNN.gmm) */
    nd_names names;
    st = nd_names_load(&names, &rom);
    CHECK(st == ND_OK);
    if (st == ND_OK) {
        CHECK_EQ_STR(nd_name(&names, ND_TEXT_SPECIES, 152), "CHIKORITA");
        CHECK_EQ_STR(nd_name(&names, ND_TEXT_SPECIES, 249), "LUGIA");
        CHECK_EQ_STR(nd_name(&names, ND_TEXT_SPECIES, 29), "NIDORAN\xE2\x99\x80"); /* U+2640 */
        CHECK_EQ_STR(nd_name(&names, ND_TEXT_MOVES, 33), "Tackle");
        CHECK_EQ_STR(nd_name(&names, ND_TEXT_ITEMS, 1), "Master Ball");
        CHECK_EQ_STR(nd_name(&names, ND_TEXT_ITEMS, 493), "Level Ball");
        CHECK_EQ_STR(nd_name(&names, ND_TEXT_ITEMS, 533), "Lock Capsule");
        CHECK_EQ_STR(nd_name(&names, ND_TEXT_ABILITIES, 65), "Overgrow");
        CHECK_EQ_STR(nd_name(&names, ND_TEXT_NATURES, 24), "Quirky");
        CHECK_EQ_STR(nd_location_name(&names, 126), "New Bark Town");
        CHECK_EQ_STR(nd_location_name(&names, 2000), "Day-Care Couple");
        CHECK_EQ_STR(nd_location_name(&names, 3000), "Lovely place");
        nd_names_free(&names);
    }

    /* ---- game data (a/0/0/2 personal, a/0/0/3 growtbl, a/0/1/1 waza_tbl) */
    nd_gamedata gd;
    st = nd_gamedata_load(&gd, &rom);
    CHECK(st == ND_OK);
    if (st == ND_OK) {
        CHECK(gd.species_count >= 494);
        const nd_species *chiko = nd_species_get(&gd, 152);
        CHECK(chiko != NULL);
        if (chiko) {
            CHECK_EQ_INT(chiko->types[0], 12); /* Grass */
            CHECK_EQ_INT(chiko->types[1], 12);
            CHECK_EQ_INT(chiko->abilities[0], 65);
            CHECK_EQ_INT(chiko->base[0], 45);
            CHECK_EQ_INT(chiko->base[2], 65);
        }
        CHECK_EQ_INT(nd_exp_for_level(&gd, 25, 100), 1000000);  /* medium fast */
        CHECK_EQ_INT(nd_exp_for_level(&gd, 152, 100), 1059860); /* medium slow */
        CHECK_EQ_INT(nd_move_base_pp(&gd, 33), 35);
        /* In the BLZ-compressed battle overlay. */
        CHECK(gd.type_chart_ok);
        CHECK_EQ_INT(nd_type_multiplier(&gd, 10, 12), 20); /* Fire -> Grass */
        CHECK_EQ_INT(nd_type_multiplier(&gd, 0, 7), 0);    /* Normal -> Ghost */
        CHECK_EQ_INT(nd_type_multiplier(&gd, 4, 2), 0);    /* Ground -> Flying */
        CHECK_EQ_INT(gd.type_count, 18);
        nd_gamedata_free(&gd);
    }

    nd_rom_close(&rom);
    fclose(f);
    return TEST_RESULT();
}
