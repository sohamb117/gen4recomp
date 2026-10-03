/* SPDX-License-Identifier: GPL-3.0-or-later */
/* ROM-backed checks against games/platinum/build/rom/pokeplatinum.us.nds.
 * Expected strings come from the decomp's text sources:
 *   build/rom/res/pokemon/species_name.json, build/rom/res/moves/move_names.json,
 *   build/rom/res/items/item_names.json, res/text/{ability,nature,location,
 *   special_met_location,mystery_gift_event}_names.json.
 * Exits 77 (CTest skip) when the ROM is absent. */
#include <stdint.h>
#include <stdlib.h>

#include "ndsdata/ndsdata.h"
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
    uint64_t size = (uint64_t)ftell(f);
    nd_rom rom;
    nd_status st = nd_rom_open(&rom, nd_read_stdio, f, size);
    CHECK(st == ND_OK);
    if (st != ND_OK)
        return TEST_RESULT();
    CHECK_EQ_STR(rom.gamecode, "CPUE");
    CHECK(rom.game == ND_GAME_PLATINUM);
    CHECK_EQ_STR(rom.title, "POKEMON PL");

    /* Path lookup vs FAT id; both reads must agree. */
    uint32_t msg_id;
    CHECK(nd_rom_find(&rom, nd_msg_narc_path(rom.game), &msg_id) == ND_OK);
    uint8_t *by_path = NULL, *by_id = NULL;
    size_t lp = 0, li = 0;
    CHECK(nd_rom_load_path(&rom, "msgdata/pl_msg.narc", &by_path, &lp) == ND_OK);
    CHECK(nd_rom_load_file(&rom, msg_id, &by_id, &li) == ND_OK);
    CHECK(lp == li && lp > 0 && by_path && by_id && memcmp(by_path, by_id, lp) == 0);

    /* Parse the whole NARC in memory; it has one member per line of
     * generated/text_banks.txt (pl_msg.naix ends at 723 + 1 entries). */
    nd_narc narc;
    CHECK(nd_narc_parse(&narc, by_path, lp) == ND_OK);
    CHECK_EQ_INT(narc.count, 724);

    /* Every string of every bank must decode with no unmapped codes. */
    size_t strings = 0, unknown = 0;
    for (uint32_t b = 0; b < narc.count; b++) {
        const uint8_t *mp;
        size_t ml;
        nd_msgbank bank;
        if (nd_narc_member(&narc, b, &mp, &ml) != ND_OK || nd_msgbank_parse(&bank, mp, ml) != ND_OK) {
            CHECK(!"bank parse");
            continue;
        }
        for (uint32_t i = 0; i < bank.count; i++) {
            char *s = NULL;
            if (nd_msgbank_get_utf8(&bank, i, &s) != ND_OK) {
                CHECK(!"string decode");
                continue;
            }
            strings++;
            if (strstr(s, "\\x"))
                unknown++;
            free(s);
        }
    }
    printf("decoded %zu strings from %u banks, %zu with unmapped codes\n", strings, narc.count, unknown);
    CHECK(strings > 10000);
    CHECK_EQ_INT(unknown, 0);

    /* Member read straight from the ROM matches the in-memory NARC. */
    uint8_t *member = NULL;
    size_t ml2 = 0;
    const uint8_t *mp;
    size_t ml;
    CHECK(nd_rom_load_narc_member(&rom, msg_id, 412, &member, &ml2) == ND_OK);
    CHECK(nd_narc_member(&narc, 412, &mp, &ml) == ND_OK && ml == ml2 && memcmp(mp, member, ml) == 0);
    free(member);
    free(by_path);
    free(by_id);

    /* Game-aware name tables. */
    nd_names names;
    CHECK(nd_names_load(&names, &rom) == ND_OK);
    CHECK_EQ_INT(names.count[ND_TEXT_SPECIES], 496);
    CHECK_EQ_INT(names.count[ND_TEXT_MOVES], 468);
    CHECK_EQ_INT(names.count[ND_TEXT_ITEMS], 468);
    CHECK_EQ_INT(names.count[ND_TEXT_ABILITIES], 124);
    CHECK_EQ_INT(names.count[ND_TEXT_NATURES], 25);
    CHECK_EQ_INT(names.count[ND_TEXT_LOCATIONS], 126);
    CHECK_EQ_INT(names.count[ND_TEXT_SPECIAL_LOCATIONS], 13);
    CHECK_EQ_INT(names.count[ND_TEXT_EVENT_LOCATIONS], 77);
    CHECK_EQ_STR(nd_name(&names, ND_TEXT_SPECIES, 0), "-----");
    CHECK_EQ_STR(nd_name(&names, ND_TEXT_SPECIES, 1), "BULBASAUR");
    CHECK_EQ_STR(nd_name(&names, ND_TEXT_SPECIES, 387), "TURTWIG");
    CHECK_EQ_STR(nd_name(&names, ND_TEXT_SPECIES, 493), "ARCEUS");
    CHECK_EQ_STR(nd_name(&names, ND_TEXT_SPECIES, 494), "Egg");
    CHECK_EQ_STR(nd_name(&names, ND_TEXT_MOVES, 33), "Tackle");
    CHECK_EQ_STR(nd_name(&names, ND_TEXT_MOVES, 467), "Shadow Force");
    CHECK_EQ_STR(nd_name(&names, ND_TEXT_ITEMS, 0), "None");
    CHECK_EQ_STR(nd_name(&names, ND_TEXT_ITEMS, 1), "Master Ball");
    CHECK_EQ_STR(nd_name(&names, ND_TEXT_ITEMS, 467), "Secret Key");
    CHECK_EQ_STR(nd_name(&names, ND_TEXT_ABILITIES, 1), "Stench");
    CHECK_EQ_STR(nd_name(&names, ND_TEXT_ABILITIES, 123), "Bad Dreams");
    CHECK_EQ_STR(nd_name(&names, ND_TEXT_NATURES, 0), "Hardy");
    CHECK_EQ_STR(nd_nature_name(&names, 24), "Quirky");
    CHECK_EQ_STR(nd_location_name(&names, 1), "Twinleaf Town");
    CHECK_EQ_STR(nd_location_name(&names, 0), "Mystery Zone");
    CHECK_EQ_STR(nd_location_name(&names, 2000), "Day-Care Couple");
    CHECK_EQ_STR(nd_location_name(&names, 3000), "Lovely place");
    CHECK_EQ_STR(nd_location_name(&names, 3001), "Pokémon Ranger");
    CHECK(nd_name(&names, ND_TEXT_SPECIES, 496) == NULL);
    nd_names_free(&names);

    nd_rom_close(&rom);
    fclose(f);
    return TEST_RESULT();
}
