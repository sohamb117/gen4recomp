/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Game tables a save editor needs to keep edits consistent, read from the
 * player's ROM:
 *
 *  - species base data, one NARC member per species (pokeplatinum
 *    include/struct_defs/species.h SpeciesData, 44 bytes): base stats at 0,
 *    types at 6, gender ratio at 16, base friendship at 18, exp rate at 19,
 *    abilities at 22.
 *  - experience tables, one member per exp rate: 101 little-endian u32,
 *    the total experience needed for levels 0..100 (src/pokemon.c
 *    Pokemon_LoadExperienceTableOf).
 *  - move data, one member per move (include/move_table.h MoveTable):
 *    effect u16 at 0, class at 2 (0 physical, 1 special, 2 status), power
 *    at 3, type at 4, accuracy at 5, base PP at 6, range (u16) at 8,
 *    priority (s8) at 10.
 *  - the type chart, which is code data, not a file: the battle overlay's
 *    rows of {attacking type, defending type, multiplier x10} (pokeplatinum
 *    src/battle/battle_lib.c sTypeMatchupMultipliers; D/P's overlay 11 has
 *    the same 111 rows), ended by {0xFF, 0xFF}. A {0xFE, 0xFE} row separates
 *    the two ghost immunities Foresight lifts; they count here. Found by its
 *    first five rows in the ARM9 overlays (stored uncompressed in the US
 *    ROMs; a BLZ-compressed overlay is not searched).
 *
 * Archive paths (checked against the US ROMs):
 *   Platinum poketool/personal/pl_personal.narc, pl_growtbl.narc,
 *            poketool/waza/pl_waza_tbl.narc
 *   Diamond  poketool/personal/personal.narc, growtbl.narc,
 *            poketool/waza/waza_tbl.narc
 *   Pearl    poketool/personal_pearl/personal.narc, otherwise as Diamond
 */
#include "ndsdata/ndsdata.h"

#include <stdlib.h>
#include <string.h>

#define SPECIES_RECORD 44
#define MOVE_RECORD_MIN 11
#define OVT_ENTRY 32

/* The type chart's first rows: Normal->Rock/Steel 0.5, Fire->Fire/Water 0.5,
 * Fire->Grass 2. */
static const uint8_t kTypeChartHead[15] = {0, 5, 5, 0, 8, 5, 10, 10, 5, 10, 11, 5, 10, 12, 20};

static int parse_type_chart(nd_gamedata *gd, const uint8_t *d, size_t len)
{
    for (size_t i = 0; i + sizeof kTypeChartHead <= len; i++) {
        if (memcmp(d + i, kTypeChartHead, sizeof kTypeChartHead))
            continue;
        for (size_t j = i; j + 3 <= len; j += 3) {
            const uint8_t atk = d[j], def = d[j + 1], mult = d[j + 2];
            if (atk == 0xFF && def == 0xFF)
                return 1;
            if (atk == 0xFE && def == 0xFE)
                continue;
            if (atk >= ND_TYPES || def >= ND_TYPES)
                break;
            gd->type_chart[atk][def] = mult;
        }
        return 0;
    }
    return 0;
}

static void load_type_chart(nd_gamedata *gd, const nd_rom *rom)
{
    for (int a = 0; a < ND_TYPES; a++)
        for (int d = 0; d < ND_TYPES; d++)
            gd->type_chart[a][d] = 10;
    uint8_t hdr[8];
    if (nd_rom_read(rom, 0x50, hdr, sizeof hdr))
        return;
    const uint32_t ovt = (uint32_t)hdr[0] | (uint32_t)hdr[1] << 8 | (uint32_t)hdr[2] << 16 | (uint32_t)hdr[3] << 24;
    const uint32_t ovt_size =
        (uint32_t)hdr[4] | (uint32_t)hdr[5] << 8 | (uint32_t)hdr[6] << 16 | (uint32_t)hdr[7] << 24;
    for (uint32_t off = 0; off + OVT_ENTRY <= ovt_size; off += OVT_ENTRY) {
        uint8_t e[OVT_ENTRY];
        if (nd_rom_read(rom, (uint64_t)ovt + off, e, sizeof e))
            return;
        const uint32_t file_id = (uint32_t)e[24] | (uint32_t)e[25] << 8 | (uint32_t)e[26] << 16 | (uint32_t)e[27] << 24;
        if (e[31] & 1)
            continue; /* compressed */
        uint8_t *data;
        size_t len;
        if (nd_rom_load_file(rom, file_id, &data, &len))
            continue;
        const int found = parse_type_chart(gd, data, len);
        free(data);
        if (found) {
            gd->type_chart_ok = 1;
            return;
        }
    }
}

static nd_status load_narc(const nd_rom *rom, const char *path, uint8_t **data, size_t *len, nd_narc *narc)
{
    nd_status st = nd_rom_load_path(rom, path, data, len);
    if (st)
        return st;
    st = nd_narc_parse(narc, *data, *len);
    if (st) {
        free(*data);
        *data = NULL;
    }
    return st;
}

nd_status nd_gamedata_load(nd_gamedata *gd, const nd_rom *rom)
{
    memset(gd, 0, sizeof *gd);
    const char *personal, *growth, *moves;
    switch (rom->game) {
    case ND_GAME_PLATINUM:
        personal = "poketool/personal/pl_personal.narc";
        growth = "poketool/personal/pl_growtbl.narc";
        moves = "poketool/waza/pl_waza_tbl.narc";
        break;
    case ND_GAME_DIAMOND:
    case ND_GAME_PEARL:
        personal = rom->game == ND_GAME_PEARL ? "poketool/personal_pearl/personal.narc"
                                              : "poketool/personal/personal.narc";
        growth = "poketool/personal/growtbl.narc";
        moves = "poketool/waza/waza_tbl.narc";
        break;
    default:
        return ND_ERR_UNSUPPORTED;
    }
    gd->game = rom->game;

    uint8_t *data;
    size_t len;
    nd_narc narc;
    nd_status st = load_narc(rom, personal, &data, &len, &narc);
    if (st)
        return st;
    gd->species = calloc(narc.count ? narc.count : 1, sizeof *gd->species);
    if (!gd->species) {
        free(data);
        return ND_ERR_NOMEM;
    }
    for (uint32_t i = 0; i < narc.count; i++) {
        const uint8_t *m;
        size_t ml;
        if (nd_narc_member(&narc, i, &m, &ml) || ml < SPECIES_RECORD)
            continue;
        nd_species *s = &gd->species[i];
        memcpy(s->base, m, 6);
        s->types[0] = m[6];
        s->types[1] = m[7];
        s->gender_ratio = m[16];
        s->base_friendship = m[18];
        s->exp_rate = m[19];
        s->abilities[0] = m[22];
        s->abilities[1] = m[23];
        s->valid = 1;
    }
    gd->species_count = narc.count;
    free(data);

    st = load_narc(rom, growth, &data, &len, &narc);
    if (st)
        goto fail;
    for (uint32_t r = 0; r < ND_EXP_RATES && r < narc.count; r++) {
        const uint8_t *m;
        size_t ml;
        if (nd_narc_member(&narc, r, &m, &ml) || ml < 101 * 4) {
            free(data);
            st = ND_ERR_FORMAT;
            goto fail;
        }
        for (int l = 0; l <= 100; l++)
            gd->exp[r][l] = (uint32_t)m[4 * l] | (uint32_t)m[4 * l + 1] << 8 | (uint32_t)m[4 * l + 2] << 16 |
                            (uint32_t)m[4 * l + 3] << 24;
    }
    free(data);

    st = load_narc(rom, moves, &data, &len, &narc);
    if (st)
        goto fail;
    gd->moves = calloc(narc.count ? narc.count : 1, sizeof *gd->moves);
    if (!gd->moves) {
        free(data);
        st = ND_ERR_NOMEM;
        goto fail;
    }
    for (uint32_t i = 0; i < narc.count; i++) {
        const uint8_t *m;
        size_t ml;
        if (nd_narc_member(&narc, i, &m, &ml) || ml < MOVE_RECORD_MIN)
            continue;
        nd_move *mv = &gd->moves[i];
        mv->effect = (uint16_t)(m[0] | m[1] << 8);
        mv->cls = m[2];
        mv->power = m[3];
        mv->type = m[4];
        mv->accuracy = m[5];
        mv->pp = m[6];
        mv->priority = (int8_t)m[10];
        mv->range = (uint16_t)(m[8] | m[9] << 8);
    }
    gd->move_count = narc.count;
    free(data);
    load_type_chart(gd, rom);
    return ND_OK;

fail:
    nd_gamedata_free(gd);
    return st;
}

void nd_gamedata_free(nd_gamedata *gd)
{
    free(gd->species);
    free(gd->moves);
    memset(gd, 0, sizeof *gd);
}

const nd_species *nd_species_get(const nd_gamedata *gd, uint32_t species)
{
    return species < gd->species_count && gd->species[species].valid ? &gd->species[species] : NULL;
}

uint32_t nd_exp_for_level(const nd_gamedata *gd, uint32_t species, uint32_t level)
{
    const nd_species *s = nd_species_get(gd, species);
    if (!s || s->exp_rate >= ND_EXP_RATES)
        return 0;
    return gd->exp[s->exp_rate][level > 100 ? 100 : level];
}

uint32_t nd_level_for_exp(const nd_gamedata *gd, uint32_t species, uint32_t exp)
{
    const nd_species *s = nd_species_get(gd, species);
    if (!s || s->exp_rate >= ND_EXP_RATES)
        return 1;
    uint32_t level = 1;
    while (level < 100 && gd->exp[s->exp_rate][level + 1] <= exp)
        level++;
    return level;
}

const nd_move *nd_move_get(const nd_gamedata *gd, uint32_t move)
{
    return move < gd->move_count ? &gd->moves[move] : NULL;
}

uint8_t nd_move_base_pp(const nd_gamedata *gd, uint32_t move)
{
    return move < gd->move_count ? gd->moves[move].pp : 0;
}

uint8_t nd_type_multiplier(const nd_gamedata *gd, uint32_t attack, uint32_t defend)
{
    return attack < ND_TYPES && defend < ND_TYPES ? gd->type_chart[attack][defend] : 10;
}
