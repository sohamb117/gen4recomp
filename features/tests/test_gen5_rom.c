/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * ndsdata against a Black or White ROM: names, zone names, species,
 * experience, move tables and the type chart, spot-checked with known
 * answers. Skips (77) when no ROM path is given or it is absent.
 */
#include <stdio.h>
#include <stdlib.h>

#include "ndsdata/ndsdata.h"
#include "testutil.h"

int main(int argc, char **argv)
{
    if (argc < 2 || !argv[1][0]) {
        printf("SKIP: no Black/White ROM (set NP_BLACK_ROM)\n");
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
    CHECK(rom.game == ND_GAME_BLACK || rom.game == ND_GAME_WHITE);
    CHECK_EQ_INT(nd_game_gen(rom.game), 5);
    CHECK_EQ_STR(nd_game_name(rom.game), rom.game == ND_GAME_BLACK ? "black" : "white");
    CHECK_EQ_INT(nd_game_gen(ND_GAME_PLATINUM), 4);
    CHECK_EQ_INT(nd_game_gen(ND_GAME_UNKNOWN), 0);

    /* ---- names */
    nd_names names;
    st = nd_names_load(&names, &rom);
    CHECK(st == ND_OK);
    if (st == ND_OK) {
        CHECK_EQ_STR(nd_name(&names, ND_TEXT_SPECIES, 1), "Bulbasaur");
        CHECK_EQ_STR(nd_name(&names, ND_TEXT_SPECIES, 495), "Snivy");
        CHECK_EQ_STR(nd_name(&names, ND_TEXT_SPECIES, 649), "Genesect");
        CHECK_EQ_STR(nd_name(&names, ND_TEXT_SPECIES, 29), "Nidoran\xE2\x99\x80"); /* U+2640 */
        CHECK_EQ_STR(nd_name(&names, ND_TEXT_SPECIES, 32), "Nidoran\xE2\x99\x82"); /* U+2642 */
        CHECK_EQ_STR(nd_name(&names, ND_TEXT_MOVES, 33), "Tackle");
        CHECK_EQ_STR(nd_name(&names, ND_TEXT_MOVES, 57), "Surf");
        CHECK_EQ_STR(nd_name(&names, ND_TEXT_MOVES, 14), "Swords Dance");
        CHECK_EQ_STR(nd_name(&names, ND_TEXT_NATURES, 0), "Hardy");
        CHECK_EQ_STR(nd_name(&names, ND_TEXT_NATURES, 24), "Quirky");
        CHECK_EQ_STR(nd_nature_name(&names, 25), "Hardy");
        CHECK_EQ_STR(nd_name(&names, ND_TEXT_ITEMS, 1), "Master Ball");
        CHECK_EQ_STR(nd_name(&names, ND_TEXT_ABILITIES, 65), "Overgrow");
        /* Met locations: the plain range and the 30001 / 40001 / 60001 ones. */
        CHECK_EQ_STR(nd_location_name(&names, 4), "Nuvema Town");
        CHECK_EQ_STR(nd_location_name(&names, 30002), "Link Trade");
        CHECK_EQ_STR(nd_location_name(&names, 30003), "Link Trade");
        CHECK_EQ_STR(nd_location_name(&names, 30004), "Kanto");
        CHECK_EQ_STR(nd_location_name(&names, 40001), "Lovely place");
        CHECK_EQ_STR(nd_location_name(&names, 60002), "Day-Care Couple");
        CHECK(nd_location_name(&names, 30000) == NULL);
        CHECK(nd_location_name(&names, 65535) == NULL);
        /* Zone 391: the player's house in Nuvema Town. */
        CHECK_EQ_STR(nd_zone_name(&names, 391), "Nuvema Town");
        CHECK(nd_zone_name(&names, 100000) == NULL);
        nd_names_free(&names);
    }

    /* ---- game data */
    nd_gamedata gd;
    st = nd_gamedata_load(&gd, &rom);
    CHECK(st == ND_OK);
    if (st == ND_OK) {
        CHECK_EQ_INT(gd.type_count, 17);
        CHECK(gd.species_count >= 650);
        /* Snivy: Grass/Grass, Overgrow (hidden Contrary is not kept). */
        const nd_species *snivy = nd_species_get(&gd, 495);
        CHECK(snivy != NULL);
        if (snivy) {
            CHECK_EQ_INT(snivy->types[0], 11);
            CHECK_EQ_INT(snivy->types[1], 11);
            CHECK_EQ_INT(snivy->abilities[0], 65);
            CHECK_EQ_INT(snivy->abilities[1], 0);
            CHECK_EQ_INT(snivy->base[0], 45);
            CHECK_EQ_INT(snivy->base[3], 63);
        }
        /* Experience: medium fast (Pikachu) is n^3; medium slow (Snivy). */
        CHECK_EQ_INT(gd.exp[0][100], 1000000);
        CHECK_EQ_INT(nd_exp_for_level(&gd, 25, 100), 1000000);
        CHECK_EQ_INT(nd_exp_for_level(&gd, 495, 100), 1059860);
        CHECK_EQ_INT(nd_level_for_exp(&gd, 25, 1000), 10);
        /* Moves. */
        const nd_move *tackle = nd_move_get(&gd, 33), *surf = nd_move_get(&gd, 57), *sd = nd_move_get(&gd, 14),
                      *eq = nd_move_get(&gd, 89), *growl = nd_move_get(&gd, 45);
        CHECK(tackle && surf && sd && eq && growl);
        if (tackle && surf && sd && eq && growl) {
            CHECK_EQ_INT(tackle->type, 0);
            CHECK_EQ_INT(tackle->cls, 0);
            CHECK_EQ_INT(tackle->power, 50);
            CHECK_EQ_INT(tackle->accuracy, 100);
            CHECK_EQ_INT(tackle->pp, 35);
            CHECK_EQ_INT(tackle->range, 0);
            CHECK(surf->range & 8); /* RANGE_ALL_ADJACENT */
            CHECK_EQ_INT(surf->cls, 1);
            CHECK(eq->range & 8);
            CHECK_EQ_INT(growl->range, 4); /* RANGE_ADJACENT_OPPONENTS */
            CHECK_EQ_INT(sd->cls, 2);
            CHECK_EQ_INT(sd->accuracy, 0);
            CHECK_EQ_INT(sd->range, 0x10); /* RANGE_USER */
        }
        CHECK_EQ_INT(nd_move_base_pp(&gd, 33), 35);
        /* Type chart (Gen 5 ids). */
        CHECK(gd.type_chart_ok);
        CHECK_EQ_INT(nd_type_multiplier(&gd, 9, 11), 20);  /* Fire -> Grass */
        CHECK_EQ_INT(nd_type_multiplier(&gd, 10, 9), 20);  /* Water -> Fire */
        CHECK_EQ_INT(nd_type_multiplier(&gd, 0, 7), 0);    /* Normal -> Ghost */
        CHECK_EQ_INT(nd_type_multiplier(&gd, 4, 2), 0);    /* Ground -> Flying */
        CHECK_EQ_INT(nd_type_multiplier(&gd, 15, 15), 20); /* Dragon -> Dragon */
        CHECK_EQ_INT(nd_type_multiplier(&gd, 1, 7), 0);    /* Fighting -> Ghost */
        CHECK_EQ_INT(nd_type_multiplier(&gd, 10, 10), 5);  /* Water -> Water */
        CHECK_EQ_INT(nd_type_multiplier(&gd, 17, 0), 10);  /* unused row */
        nd_gamedata_free(&gd);
    }

    nd_rom_close(&rom);
    fclose(f);
    return TEST_RESULT();
}
