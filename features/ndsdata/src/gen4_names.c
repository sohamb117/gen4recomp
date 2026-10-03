/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Game-aware name lists read from the player's ROM.
 *
 * Bank indices (members of the main message NARC):
 *
 * Platinum: msgdata/pl_msg.narc (games/platinum/platinum.us/filesys.csv:
 *   "res/text/pl_msg.narc,/msgdata/pl_msg.narc"). Member index = line - 1 of
 *   games/platinum/generated/text_banks.txt, identical to the nitroarc-
 *   generated games/platinum/build/rom/res/text/pl_msg.naix:
 *     species_name 412, move_names 647, item_names 392, ability_names 610,
 *     nature_names 202, location_names 433, special_met_location_names 435,
 *     mystery_gift_event_names 434.
 *
 * Diamond / Pearl: msgdata/msg.narc (games/diamond/filesystem.mk), built from
 *   games/diamond/files/msgdata/msg/narc_NNNN.gmm where NNNN is the member
 *   index (msg.mk sorts the files by name). Identified by their English
 *   contents in pret/pokediamond:
 *     narc_0362 "-----, BULBASAUR, IVYSAUR..."     species
 *     narc_0588 "-, Pound, Karate Chop..."         moves
 *     narc_0344 "None, Master Ball, Ultra Ball..." items
 *     narc_0552 " -, Stench, Drizzle..."           abilities
 *     narc_0190 "Hardy, Lonely, Brave..."          natures
 *     narc_0382 "Mystery Zone, Twinleaf Town..."   locations
 *     narc_0384 "Day-Care Couple, Link trade..."   special met locations
 *     narc_0383 "Lovely place, Pokémon Ranger..."  event met locations
 *   Pearl ships the same msg.narc layout (pokediamond builds both from the
 *   same files/ tree).
 *
 * Met location ranges: pokeplatinum src/unk_02017038.c (bases 0, 0x7D0,
 * 0xBB8) selects the bank in StringTemplate_SetMetLocationName
 * (src/string_template.c): LOCATION_NAMES, SPECIAL_MET_LOCATION_NAMES,
 * MYSTERY_GIFT_EVENT_NAMES.
 */
#include "ndsdata/ndsdata.h"

#include <stdlib.h>
#include <string.h>

static const int16_t kBanks[2][ND_TEXT_KIND_COUNT] = {
    /* D/P */ {362, 588, 344, 552, 190, 382, 384, 383},
    /* Pt  */ {412, 647, 392, 610, 202, 433, 435, 434},
};

const char *nd_msg_narc_path(nd_game game)
{
    switch (game) {
    case ND_GAME_DIAMOND:
    case ND_GAME_PEARL:
        return "msgdata/msg.narc";
    case ND_GAME_PLATINUM:
        return "msgdata/pl_msg.narc";
    default:
        return NULL;
    }
}

int nd_text_bank(nd_game game, nd_text_kind kind)
{
    if ((unsigned)kind >= ND_TEXT_KIND_COUNT)
        return -1;
    switch (game) {
    case ND_GAME_DIAMOND:
    case ND_GAME_PEARL:
        return kBanks[0][kind];
    case ND_GAME_PLATINUM:
        return kBanks[1][kind];
    default:
        return -1;
    }
}

void nd_names_free(nd_names *names)
{
    for (int k = 0; k < ND_TEXT_KIND_COUNT; k++) {
        if (names->list[k]) {
            for (uint32_t i = 0; i < names->count[k]; i++)
                free(names->list[k][i]);
            free(names->list[k]);
        }
        names->list[k] = NULL;
        names->count[k] = 0;
    }
}

static nd_status load_bank(nd_names *names, const nd_rom *rom, uint32_t narc_id, nd_text_kind kind)
{
    uint8_t *data = NULL;
    size_t len = 0;
    nd_status st = nd_rom_load_narc_member(rom, narc_id, (uint32_t)nd_text_bank(names->game, kind), &data, &len);
    if (st != ND_OK)
        return st;
    nd_msgbank bank;
    if ((st = nd_msgbank_parse(&bank, data, len)) != ND_OK) {
        free(data);
        return st;
    }
    char **list = calloc(bank.count ? bank.count : 1, sizeof(char *));
    if (!list) {
        free(data);
        return ND_ERR_NOMEM;
    }
    names->list[kind] = list;
    names->count[kind] = bank.count;
    for (uint32_t i = 0; i < bank.count; i++) {
        if ((st = nd_msgbank_get_utf8(&bank, i, &list[i])) != ND_OK)
            break;
    }
    free(data);
    return st;
}

nd_status nd_names_load(nd_names *names, const nd_rom *rom)
{
    memset(names, 0, sizeof(*names));
    names->game = rom->game;
    const char *path = nd_msg_narc_path(rom->game);
    if (!path)
        return ND_ERR_UNSUPPORTED;
    uint32_t narc_id;
    nd_status st = nd_rom_find(rom, path, &narc_id);
    if (st != ND_OK)
        return st;
    for (int k = 0; k < ND_TEXT_KIND_COUNT; k++) {
        if ((st = load_bank(names, rom, narc_id, (nd_text_kind)k)) != ND_OK) {
            nd_names_free(names);
            return st;
        }
    }
    return ND_OK;
}

const char *nd_name(const nd_names *names, nd_text_kind kind, uint32_t id)
{
    if (!names || (unsigned)kind >= ND_TEXT_KIND_COUNT || !names->list[kind] || id >= names->count[kind])
        return NULL;
    return names->list[kind][id];
}

const char *nd_location_name(const nd_names *names, uint32_t location)
{
    if (location >= 3000)
        return nd_name(names, ND_TEXT_EVENT_LOCATIONS, location - 3000);
    if (location >= 2000)
        return nd_name(names, ND_TEXT_SPECIAL_LOCATIONS, location - 2000);
    return nd_name(names, ND_TEXT_LOCATIONS, location);
}

const char *nd_nature_name(const nd_names *names, uint32_t pid)
{
    return nd_name(names, ND_TEXT_NATURES, pid % 25);
}
