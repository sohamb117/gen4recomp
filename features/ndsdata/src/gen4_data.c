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
 *  - move data, one member per move (include/move_table.h MoveTable): base
 *    PP at offset 6.
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
#define MOVE_RECORD_MIN 7

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
    gd->move_pp = calloc(narc.count ? narc.count : 1, 1);
    if (!gd->move_pp) {
        free(data);
        st = ND_ERR_NOMEM;
        goto fail;
    }
    for (uint32_t i = 0; i < narc.count; i++) {
        const uint8_t *m;
        size_t ml;
        if (!nd_narc_member(&narc, i, &m, &ml) && ml >= MOVE_RECORD_MIN)
            gd->move_pp[i] = m[6];
    }
    gd->move_count = narc.count;
    free(data);
    return ND_OK;

fail:
    nd_gamedata_free(gd);
    return st;
}

void nd_gamedata_free(nd_gamedata *gd)
{
    free(gd->species);
    free(gd->move_pp);
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

uint8_t nd_move_base_pp(const nd_gamedata *gd, uint32_t move)
{
    return move < gd->move_count ? gd->move_pp[move] : 0;
}
