/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Black / White game tables, read from the player's ROM. Layouts were
 * worked out against the US ROMs (IRBO, IRAO), which agree on all of them.
 *
 *  - species: a/0/1/6, one member per species (0..649, then alternate
 *    forms), 0x3C bytes (one form record is 0x38; the last member is not a
 *    species record and is skipped by its size): base stats at 0x00..0x05,
 *    types at 0x06 / 0x07, gender ratio 0x12, base friendship 0x14, exp
 *    rate 0x15, abilities 0x18 / 0x19 and the hidden ability 0x1A (not
 *    kept). A species with one normal ability stores it either as {a, 0}
 *    or as {a, a}; both become {a, 0}.
 *  - experience: a/0/1/7 (not a/0/1/8, which holds variable-size records),
 *    eight members of 101 little-endian u32, total exp for levels 0..100
 *    (member 0, medium fast, reaches 1000000 at level 100).
 *  - moves: a/0/2/1, one 36-byte member per move: type 0x00, category 0x02
 *    (0 status, 1 physical, 2 special; remapped to nd_move.cls), power
 *    0x03, accuracy 0x04 (101: never misses, stored as 0), PP 0x05,
 *    priority (s8) 0x06, effect sequence id (u16) 0x10 -> nd_move.effect,
 *    target 0x14 (kTargetRange below).
 *  - type chart: a 17 x 17 byte matrix [attacking][defending] of x4
 *    multipliers (0, 2 = 0.5, 4 = 1, 8 = 2) in the battle overlay (overlay
 *    93 of the US ROMs, BLZ-compressed). Found by shape: every byte in
 *    {0, 2, 4, 8}, the Normal row 0.5 against Rock and Steel, 0 against
 *    Ghost and 1 otherwise, and Ghost -> Normal 0. Gen 5 type ids: Normal 0
 *    Fighting 1 Flying 2 Poison 3 Ground 4 Rock 5 Bug 6 Ghost 7 Steel 8
 *    Fire 9 Water 10 Grass 11 Electric 12 Psychic 13 Ice 14 Dragon 15
 *    Dark 16.
 *  - zone headers: a/0/1/2, one member of 48-byte records per zone (map)
 *    id; u16 at 0x1A holds the location-name index (bank 89 of a/0/0/2) in
 *    its low 10 bits. Bit 10 is a flag set on 112 of the 427 zones (mostly
 *    the outdoor maps of towns and routes) and is not part of the index.
 *    Zone 391, the player's house in Nuvema Town, reads 4 = "Nuvema Town".
 */
#include <stdlib.h>
#include <string.h>

#include "gen5.h"

#define G5_PERSONAL "a/0/1/6"
#define G5_GROWTH "a/0/1/7"
#define G5_MOVES "a/0/2/1"
#define G5_ZONES "a/0/1/2"

#define G5_SPECIES_MIN 0x1B
#define G5_SPECIES_MAX 0x3C
#define G5_MOVE_MIN 0x15
#define G5_ZONE_RECORD 48
#define G5_TYPES 17
#define OVT_ENTRY 32

#define RANGE_SINGLE_TARGET_SPECIAL (1u << 0)
#define RANGE_RANDOM_OPPONENT (1u << 1)
#define RANGE_ADJACENT_OPPONENTS (1u << 2)
#define RANGE_ALL_ADJACENT (1u << 3)
#define RANGE_USER (1u << 4)
#define RANGE_USER_SIDE (1u << 5)
#define RANGE_FIELD (1u << 6)
#define RANGE_OPPONENT_SIDE (1u << 7)
#define RANGE_ALLY (1u << 8)
#define RANGE_USER_OR_ALLY (1u << 9)
#define RANGE_SINGLE_TARGET_ME_FIRST (1u << 10)

/* Gen 5 target byte -> the Gen 4 RANGE_* bits (pokeplatinum
 * generated/move_ranges.txt as a bit mask), from the moves that use each
 * value in the ROM:
 *   0  one other battler (Tackle, Heal Pulse, Fly, Transform) -> single target
 *   1  user or ally (Acupressure)                              -> USER_OR_ALLY
 *   2  ally (Helping Hand)                                     -> ALLY
 *   3  adjacent foe (Me First)                                 -> SINGLE_TARGET_ME_FIRST
 *   4  every adjacent battler (Surf, Earthquake, Explosion)    -> ALL_ADJACENT
 *   5  adjacent foes (Growl, Rock Slide, Hyper Voice)          -> ADJACENT_OPPONENTS
 *   6  user's party (Heal Bell, Aromatherapy)                  -> USER_SIDE
 *   7  user (Swords Dance, Recover, Protect, Bide)             -> USER
 *   8  every battler (Perish Song)                             -> FIELD
 *   9  random foe (Thrash, Petal Dance, Outrage, Uproar)       -> RANDOM_OPPONENT
 *   10 whole field (Rain Dance, Haze, Gravity, Trick Room)     -> FIELD
 *   11 foes' side (Spikes, Toxic Spikes, Stealth Rock)         -> OPPONENT_SIDE
 *   12 user's side (Light Screen, Reflect, Tailwind)           -> USER_SIDE
 *   13 decided in battle (Counter, Mirror Coat, Metal Burst,
 *      Curse, Nature Power)                                    -> SINGLE_TARGET_SPECIAL
 */
static const uint16_t kTargetRange[14] = {
    0,
    RANGE_USER_OR_ALLY,
    RANGE_ALLY,
    RANGE_SINGLE_TARGET_ME_FIRST,
    RANGE_ALL_ADJACENT,
    RANGE_ADJACENT_OPPONENTS,
    RANGE_USER_SIDE,
    RANGE_USER,
    RANGE_FIELD,
    RANGE_RANDOM_OPPONENT,
    RANGE_FIELD,
    RANGE_OPPONENT_SIDE,
    RANGE_USER_SIDE,
    RANGE_SINGLE_TARGET_SPECIAL,
};

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int parse_type_chart(nd_gamedata *gd, const uint8_t *d, size_t len)
{
    const size_t size = G5_TYPES * G5_TYPES;
    for (size_t i = 0; i + size <= len; i++) {
        const uint8_t *m = d + i;
        if (m[0] != 4 || m[5] != 2 || m[7] != 0 || m[8] != 2)
            continue;
        int ok = m[7 * G5_TYPES] == 0;
        for (int t = 0; ok && t < G5_TYPES; t++)
            ok = m[t] == (t == 7 ? 0 : t == 5 || t == 8 ? 2 : 4);
        for (size_t k = 0; ok && k < size; k++)
            ok = m[k] == 0 || m[k] == 2 || m[k] == 4 || m[k] == 8;
        if (!ok)
            continue;
        for (int a = 0; a < G5_TYPES; a++)
            for (int t = 0; t < G5_TYPES; t++)
                gd->type_chart[a][t] = (uint8_t)(m[a * G5_TYPES + t] * 10 / 4);
        return 1;
    }
    return 0;
}

static void load_type_chart(nd_gamedata *gd, const nd_rom *rom)
{
    uint8_t hdr[8];
    if (nd_rom_read(rom, 0x50, hdr, sizeof hdr))
        return;
    const uint32_t ovt = rd32(hdr), ovt_size = rd32(hdr + 4);
    for (uint32_t off = 0; off + OVT_ENTRY <= ovt_size; off += OVT_ENTRY) {
        uint8_t e[OVT_ENTRY];
        if (nd_rom_read(rom, (uint64_t)ovt + off, e, sizeof e))
            return;
        uint8_t *data, *image;
        size_t len, image_len;
        if (nd_rom_load_file(rom, rd32(e + 24), &data, &len))
            continue;
        if (e[31] & 1) {
            const nd_status st = nd_blz_decompress(data, len, &image, &image_len);
            free(data);
            if (st)
                continue;
        } else {
            image = data;
            image_len = len;
        }
        const int found = parse_type_chart(gd, image, image_len);
        free(image);
        if (found) {
            gd->type_chart_ok = 1;
            return;
        }
    }
}

nd_status g5_gamedata_load(nd_gamedata *gd, const nd_rom *rom)
{
    memset(gd, 0, sizeof *gd);
    gd->game = rom->game;
    gd->type_count = G5_TYPES;
    for (int a = 0; a < ND_TYPES; a++)
        for (int t = 0; t < ND_TYPES; t++)
            gd->type_chart[a][t] = 10;

    uint8_t *data;
    size_t len;
    nd_narc narc;
    nd_status st = nd_load_narc(rom, G5_PERSONAL, &data, &len, &narc);
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
        if (nd_narc_member(&narc, i, &m, &ml) || ml < G5_SPECIES_MIN || ml > G5_SPECIES_MAX)
            continue;
        nd_species *s = &gd->species[i];
        memcpy(s->base, m, 6);
        s->types[0] = m[0x06];
        s->types[1] = m[0x07];
        s->gender_ratio = m[0x12];
        s->base_friendship = m[0x14];
        s->exp_rate = m[0x15];
        s->abilities[0] = m[0x18];
        s->abilities[1] = m[0x19] == m[0x18] ? 0 : m[0x19];
        s->valid = 1;
    }
    gd->species_count = narc.count;
    free(data);

    st = nd_load_narc(rom, G5_GROWTH, &data, &len, &narc);
    if (st)
        goto fail;
    for (uint32_t r = 0; r < ND_EXP_RATES; r++) {
        const uint8_t *m;
        size_t ml;
        if (r >= narc.count || nd_narc_member(&narc, r, &m, &ml) || ml < 101 * 4) {
            free(data);
            st = ND_ERR_FORMAT;
            goto fail;
        }
        for (int l = 0; l <= 100; l++)
            gd->exp[r][l] = rd32(m + 4 * l);
    }
    free(data);

    st = nd_load_narc(rom, G5_MOVES, &data, &len, &narc);
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
        if (nd_narc_member(&narc, i, &m, &ml) || ml < G5_MOVE_MIN)
            continue;
        nd_move *mv = &gd->moves[i];
        mv->type = m[0x00];
        mv->cls = m[0x02] == 0 ? 2 : (uint8_t)(m[0x02] - 1);
        mv->power = m[0x03];
        mv->accuracy = m[0x04] > 100 ? 0 : m[0x04];
        mv->pp = m[0x05];
        mv->priority = (int8_t)m[0x06];
        mv->effect = rd16(m + 0x10);
        mv->range = m[0x14] < sizeof kTargetRange / sizeof kTargetRange[0] ? kTargetRange[m[0x14]] : 0;
    }
    gd->move_count = narc.count;
    free(data);
    load_type_chart(gd, rom);
    return ND_OK;

fail:
    nd_gamedata_free(gd);
    return st;
}

nd_status g5_zone_locations_load(const nd_rom *rom, uint16_t **zone_location, uint32_t *zone_count)
{
    uint32_t id;
    nd_status st = nd_rom_find(rom, G5_ZONES, &id);
    if (st)
        return st;
    uint8_t *data;
    size_t len;
    if ((st = nd_rom_load_narc_member(rom, id, 0, &data, &len)))
        return st;
    const uint32_t n = (uint32_t)(len / G5_ZONE_RECORD);
    if (n == 0 || len % G5_ZONE_RECORD) {
        free(data);
        return ND_ERR_FORMAT;
    }
    uint16_t *table = malloc(n * sizeof *table);
    if (!table) {
        free(data);
        return ND_ERR_NOMEM;
    }
    for (uint32_t z = 0; z < n; z++)
        table[z] = rd16(data + z * G5_ZONE_RECORD + 0x1A) & 0x3FF;
    free(data);
    *zone_location = table;
    *zone_count = n;
    return ND_OK;
}
