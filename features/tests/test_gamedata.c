/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * nd_gamedata against the Platinum ROM: spot-check species, experience and
 * move tables with values from pokeplatinum's sources (res/pokemon/<species>/
 * data.json, res/pokemon/.shared/exp_tables.csv, res/battle/moves/<move>/
 * data.json). Skips (77) without the ROM.
 */
#include <stdio.h>
#include <stdlib.h>

#include "ndsdata/ndsdata.h"
#include "save4/save4.h"
#include "testutil.h"

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <pokeplatinum.us.nds>\n", argv[0]);
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        printf("SKIP: ROM not found at %s\n", argv[1]);
        return 77;
    }
    fseek(f, 0, SEEK_END);
    nd_rom rom;
    CHECK(nd_rom_open(&rom, nd_read_stdio, f, (uint64_t)ftell(f)) == ND_OK);
    nd_gamedata gd;
    nd_status st = nd_gamedata_load(&gd, &rom);
    CHECK(st == ND_OK);
    if (st != ND_OK)
        return TEST_RESULT();

    CHECK(gd.species_count >= 494);
    /* Bulbasaur: 45/49/49/45/65/65, Grass/Poison (12, 3), Medium Slow (3),
     * Overgrow (65), 87.5% male (31), base friendship 70. */
    const nd_species *b = nd_species_get(&gd, 1);
    CHECK(b != NULL);
    if (b) {
        CHECK_EQ_INT(b->base[0], 45);
        CHECK_EQ_INT(b->base[3], 45);
        CHECK_EQ_INT(b->base[4], 65);
        CHECK_EQ_INT(b->types[0], 12);
        CHECK_EQ_INT(b->types[1], 3);
        CHECK_EQ_INT(b->exp_rate, 3);
        CHECK_EQ_INT(b->abilities[0], 65);
        CHECK_EQ_INT(b->gender_ratio, 31);
        CHECK_EQ_INT(b->base_friendship, 70);
    }
    /* Garchomp (445): base stats of the stat-formula example. */
    const nd_species *g = nd_species_get(&gd, 445);
    CHECK(g != NULL);
    if (g) {
        const uint8_t want[6] = {108, 130, 95, 102, 80, 85};
        for (int i = 0; i < 6; i++)
            CHECK_EQ_INT(g->base[i], want[i]);
    }
    /* Exp tables: Medium Fast is n^3; Medium Slow level 100 = 1059860. */
    CHECK_EQ_INT(gd.exp[0][10], 1000);
    CHECK_EQ_INT(gd.exp[0][100], 1000000);
    CHECK_EQ_INT(gd.exp[3][100], 1059860);
    CHECK_EQ_INT(nd_exp_for_level(&gd, 1, 5), 135);  /* Medium Slow level 5 */
    CHECK_EQ_INT(nd_level_for_exp(&gd, 1, 135), 5);
    CHECK_EQ_INT(nd_level_for_exp(&gd, 1, 134), 4);
    CHECK_EQ_INT(nd_level_for_exp(&gd, 1, 0), 1);
    CHECK_EQ_INT(nd_level_for_exp(&gd, 1, 5000000), 100);
    /* Moves: Pound (1) 35 PP, Tackle (33) 35, Hyper Beam (63) 5. */
    CHECK_EQ_INT(nd_move_base_pp(&gd, 1), 35);
    CHECK_EQ_INT(nd_move_base_pp(&gd, 33), 35);
    CHECK_EQ_INT(nd_move_base_pp(&gd, 63), 5);
    CHECK_EQ_INT(nd_move_base_pp(&gd, 100000), 0);

    nd_gamedata_free(&gd);
    nd_rom_close(&rom);
    fclose(f);
    return TEST_RESULT();
}
