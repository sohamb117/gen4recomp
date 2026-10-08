/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Gen 5 Pokémon data (Project Pokémon "BW PK5 Structure", PKHeX PK5):
 * 136 bytes stored, 220 in the party. A 8-byte header {u32 PID, u16 unused,
 * u16 checksum} precedes four 32-byte blocks A-D stored in a PID-dependent
 * order and XOR-encrypted with the Gen 4/5 PRNG (x = x * 0x41C64E6D +
 * 0x6073, key = x >> 16) seeded by the checksum; the party tail (status,
 * level, current stats, ...) is encrypted the same way seeded by the PID.
 * The checksum is the 16-bit sum of the 64 words of blocks A-D.
 */
#include <string.h>

#include "save5/save5.h"

/* Offsets in canonical (unshuffled) order. */
enum {
    OFS_PID = 0x00,
    OFS_CHECKSUM = 0x06,
    OFS_BLOCKS = 0x08,
    BLOCK_SIZE = 0x20,
    /* A */
    OFS_SPECIES = 0x08,
    OFS_HELD = 0x0A,
    OFS_TID = 0x0C,
    OFS_SID = 0x0E,
    OFS_EXP = 0x10,
    OFS_FRIENDSHIP = 0x14,
    OFS_ABILITY = 0x15,
    OFS_MARKINGS = 0x16,
    OFS_LANGUAGE = 0x17,
    OFS_EVS = 0x18,
    OFS_CONTEST = 0x1E,
    /* B */
    OFS_MOVES = 0x28,
    OFS_PP = 0x30,
    OFS_PP_UPS = 0x34,
    OFS_IV32 = 0x38,         /* IVs 5 bits each, bit 30 egg, bit 31 nicknamed */
    OFS_FATEFUL_GENDER = 0x40, /* bit 0 fateful, bits 1-2 gender, bits 3-7 form */
    OFS_NATURE = 0x41,
    OFS_HIDDEN_ABILITY = 0x42, /* bit 0 */
    /* C */
    OFS_NICKNAME = 0x48,
    OFS_ORIGIN_GAME = 0x5F,
    /* D */
    OFS_OT_NAME = 0x68,
    OFS_EGG_LOCATION = 0x7E,
    OFS_MET_LOCATION = 0x80,
    OFS_POKERUS = 0x82,
    OFS_BALL = 0x83,
    OFS_MET_LEVEL = 0x84,    /* bits 0-6 level, bit 7 OT gender */
    /* party tail */
    OFS_STATUS = 0x88,
    OFS_LEVEL = 0x8C,
    OFS_HP = 0x8E,
    OFS_STATS = 0x90,
};

#define NICK_UNITS 11
#define OT_UNITS 8

static uint16_t g16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t g32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void s16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}
static void s32(uint8_t *p, uint32_t v)
{
    s16(p, (uint16_t)v);
    s16(p + 2, (uint16_t)(v >> 16));
}

/* Stored position of blocks A, B, C, D for each shuffle index (Gen 5 keeps
 * the Gen 4 order; same table as save4's pkm4.c). */
static const uint8_t kShuffle[24][4] = {
    {0, 1, 2, 3}, {0, 1, 3, 2}, {0, 2, 1, 3}, {0, 3, 1, 2}, {0, 2, 3, 1}, {0, 3, 2, 1},
    {1, 0, 2, 3}, {1, 0, 3, 2}, {2, 0, 1, 3}, {3, 0, 1, 2}, {2, 0, 3, 1}, {3, 0, 2, 1},
    {1, 2, 0, 3}, {1, 3, 0, 2}, {2, 1, 0, 3}, {3, 1, 0, 2}, {2, 3, 0, 1}, {3, 2, 0, 1},
    {1, 2, 3, 0}, {1, 3, 2, 0}, {2, 1, 3, 0}, {3, 1, 2, 0}, {2, 3, 1, 0}, {3, 2, 1, 0},
};

uint32_t pkm5_shuffle_index(uint32_t pid)
{
    return ((pid >> 13) & 31) % 24;
}

static void crypt(uint8_t *data, size_t bytes, uint32_t seed)
{
    for (size_t i = 0; i + 1 < bytes; i += 2) {
        seed = seed * 0x41C64E6Du + 0x6073u;
        const uint16_t k = (uint16_t)(seed >> 16);
        data[i] ^= (uint8_t)k;
        data[i + 1] ^= (uint8_t)(k >> 8);
    }
}

uint16_t pkm5_calc_checksum(const pkm5 *p)
{
    uint16_t sum = 0;
    for (int i = 0; i < 4 * BLOCK_SIZE; i += 2)
        sum = (uint16_t)(sum + g16(p->data + OFS_BLOCKS + i));
    return sum;
}

save5_status pkm5_decrypt(const uint8_t *enc, size_t len, pkm5 *out)
{
    if (!enc || !out || (len != PKM5_BOX_SIZE && len != PKM5_PARTY_SIZE))
        return SAVE5_ERR_ARG;
    memset(out, 0, sizeof *out);
    uint8_t stored[4 * BLOCK_SIZE];
    memcpy(out->data, enc, OFS_BLOCKS);
    memcpy(stored, enc + OFS_BLOCKS, sizeof stored);
    const uint32_t pid = g32(enc + OFS_PID);
    const uint16_t checksum = g16(enc + OFS_CHECKSUM);
    crypt(stored, sizeof stored, checksum);
    const uint8_t *order = kShuffle[pkm5_shuffle_index(pid)];
    for (int b = 0; b < 4; b++)
        memcpy(out->data + OFS_BLOCKS + b * BLOCK_SIZE, stored + order[b] * BLOCK_SIZE, BLOCK_SIZE);
    if (len == PKM5_PARTY_SIZE) {
        memcpy(out->data + PKM5_BOX_SIZE, enc + PKM5_BOX_SIZE, PKM5_PARTY_SIZE - PKM5_BOX_SIZE);
        crypt(out->data + PKM5_BOX_SIZE, PKM5_PARTY_SIZE - PKM5_BOX_SIZE, pid);
        out->party = true;
    }
    return pkm5_calc_checksum(out) == checksum ? SAVE5_OK : SAVE5_ERR_PKM_CHECKSUM;
}

save5_status pkm5_encrypt(pkm5 *p, uint8_t *out, size_t len)
{
    if (!p || !out || (len != PKM5_BOX_SIZE && len != PKM5_PARTY_SIZE))
        return SAVE5_ERR_ARG;
    const uint16_t checksum = pkm5_calc_checksum(p);
    s16(p->data + OFS_CHECKSUM, checksum);
    const uint32_t pid = g32(p->data + OFS_PID);
    uint8_t stored[4 * BLOCK_SIZE];
    const uint8_t *order = kShuffle[pkm5_shuffle_index(pid)];
    for (int b = 0; b < 4; b++)
        memcpy(stored + order[b] * BLOCK_SIZE, p->data + OFS_BLOCKS + b * BLOCK_SIZE, BLOCK_SIZE);
    crypt(stored, sizeof stored, checksum);
    memcpy(out, p->data, OFS_BLOCKS);
    memcpy(out + OFS_BLOCKS, stored, sizeof stored);
    if (len == PKM5_PARTY_SIZE) {
        uint8_t tail[PKM5_PARTY_SIZE - PKM5_BOX_SIZE];
        if (p->party)
            memcpy(tail, p->data + PKM5_BOX_SIZE, sizeof tail);
        else
            memset(tail, 0, sizeof tail);
        crypt(tail, sizeof tail, pid);
        memcpy(out + PKM5_BOX_SIZE, tail, sizeof tail);
    }
    return SAVE5_OK;
}

bool pkm5_is_empty(const pkm5 *p)
{
    return !p || g16(p->data + OFS_SPECIES) == 0;
}

void pkm5_info_get(const pkm5 *p, pkm5_info *info)
{
    if (!info)
        return;
    memset(info, 0, sizeof *info);
    if (!p)
        return;
    const uint8_t *d = p->data;
    info->pid = g32(d + OFS_PID);
    info->checksum = g16(d + OFS_CHECKSUM);
    info->checksum_ok = pkm5_calc_checksum(p) == info->checksum;
    info->species = g16(d + OFS_SPECIES);
    info->held_item = g16(d + OFS_HELD);
    info->tid = g16(d + OFS_TID);
    info->sid = g16(d + OFS_SID);
    info->exp = g32(d + OFS_EXP);
    info->friendship = d[OFS_FRIENDSHIP];
    info->ability = d[OFS_ABILITY];
    info->markings = d[OFS_MARKINGS];
    info->language = d[OFS_LANGUAGE];
    for (int i = 0; i < 6; i++) {
        info->evs[i] = d[OFS_EVS + i];
        info->contest[i] = d[OFS_CONTEST + i];
    }
    for (int i = 0; i < 4; i++) {
        info->moves[i] = g16(d + OFS_MOVES + 2 * i);
        info->pp[i] = d[OFS_PP + i];
        info->pp_ups[i] = d[OFS_PP_UPS + i];
    }
    const uint32_t iv = g32(d + OFS_IV32);
    for (int i = 0; i < 6; i++)
        info->ivs[i] = (uint8_t)((iv >> (5 * i)) & 31);
    info->is_egg = (iv >> 30) & 1;
    info->has_nickname = (iv >> 31) & 1;
    info->fateful = d[OFS_FATEFUL_GENDER] & 1;
    info->gender = (uint8_t)((d[OFS_FATEFUL_GENDER] >> 1) & 3);
    info->form = (uint8_t)(d[OFS_FATEFUL_GENDER] >> 3);
    info->nature = d[OFS_NATURE];
    info->hidden_ability = d[OFS_HIDDEN_ABILITY] & 1;
    save5_text_decode(d + OFS_NICKNAME, NICK_UNITS, info->nickname, sizeof info->nickname);
    info->origin_game = d[OFS_ORIGIN_GAME];
    save5_text_decode(d + OFS_OT_NAME, OT_UNITS, info->ot_name, sizeof info->ot_name);
    info->egg_location = g16(d + OFS_EGG_LOCATION);
    info->met_location = g16(d + OFS_MET_LOCATION);
    info->pokerus = d[OFS_POKERUS];
    info->ball = d[OFS_BALL];
    info->met_level = d[OFS_MET_LEVEL] & 0x7F;
    info->ot_gender = d[OFS_MET_LEVEL] >> 7;
    info->shiny = ((uint32_t)info->tid ^ info->sid ^ (info->pid >> 16) ^ (info->pid & 0xFFFF)) < 8;
    if (p->party) {
        info->has_party_data = true;
        info->status = g32(d + OFS_STATUS);
        info->level = d[OFS_LEVEL];
        info->hp = g16(d + OFS_HP);
        for (int i = 0; i < 6; i++)
            info->stats[i] = g16(d + OFS_STATS + 2 * i);
    }
}

void pkm5_set_pid(pkm5 *p, uint32_t pid) { s32(p->data + OFS_PID, pid); }
void pkm5_set_species(pkm5 *p, uint16_t species) { s16(p->data + OFS_SPECIES, species); }
void pkm5_set_held_item(pkm5 *p, uint16_t item) { s16(p->data + OFS_HELD, item); }
void pkm5_set_ot_ids(pkm5 *p, uint16_t tid, uint16_t sid)
{
    s16(p->data + OFS_TID, tid);
    s16(p->data + OFS_SID, sid);
}
void pkm5_set_exp(pkm5 *p, uint32_t exp) { s32(p->data + OFS_EXP, exp); }
void pkm5_set_friendship(pkm5 *p, uint8_t v) { p->data[OFS_FRIENDSHIP] = v; }
void pkm5_set_ability(pkm5 *p, uint8_t ability, bool hidden)
{
    p->data[OFS_ABILITY] = ability;
    p->data[OFS_HIDDEN_ABILITY] = (uint8_t)((p->data[OFS_HIDDEN_ABILITY] & ~1u) | (hidden ? 1u : 0u));
}
void pkm5_set_language(pkm5 *p, uint8_t lang) { p->data[OFS_LANGUAGE] = lang; }

void pkm5_set_ev(pkm5 *p, int stat, uint8_t v)
{
    if (stat >= 0 && stat < 6)
        p->data[OFS_EVS + stat] = v;
}

void pkm5_set_move(pkm5 *p, int slot, uint16_t move, uint8_t pp, uint8_t pp_ups)
{
    if (slot < 0 || slot >= 4)
        return;
    s16(p->data + OFS_MOVES + 2 * slot, move);
    p->data[OFS_PP + slot] = pp;
    p->data[OFS_PP_UPS + slot] = pp_ups;
}

void pkm5_set_iv(pkm5 *p, int stat, uint8_t v)
{
    if (stat < 0 || stat >= 6 || v > 31)
        return;
    uint32_t iv = g32(p->data + OFS_IV32);
    iv = (iv & ~(31u << (5 * stat))) | ((uint32_t)v << (5 * stat));
    s32(p->data + OFS_IV32, iv);
}

void pkm5_set_is_egg(pkm5 *p, bool egg)
{
    uint32_t iv = g32(p->data + OFS_IV32);
    iv = (iv & ~(1u << 30)) | (egg ? 1u << 30 : 0);
    s32(p->data + OFS_IV32, iv);
}

void pkm5_set_gender_form(pkm5 *p, uint8_t gender, uint8_t form)
{
    p->data[OFS_FATEFUL_GENDER] = (uint8_t)((p->data[OFS_FATEFUL_GENDER] & 1) | ((gender & 3) << 1) | (form << 3));
}

void pkm5_set_nature(pkm5 *p, uint8_t nature) { p->data[OFS_NATURE] = nature; }

save5_status pkm5_set_nickname(pkm5 *p, const char *utf8, bool is_nickname)
{
    save5_status st = save5_text_encode(utf8, p->data + OFS_NICKNAME, NICK_UNITS);
    if (st != SAVE5_OK)
        return st;
    uint32_t iv = g32(p->data + OFS_IV32);
    iv = (iv & ~(1u << 31)) | (is_nickname ? 1u << 31 : 0);
    s32(p->data + OFS_IV32, iv);
    return SAVE5_OK;
}

save5_status pkm5_set_ot_name(pkm5 *p, const char *utf8)
{
    return save5_text_encode(utf8, p->data + OFS_OT_NAME, OT_UNITS);
}

void pkm5_set_origin_game(pkm5 *p, uint8_t game) { p->data[OFS_ORIGIN_GAME] = game; }

void pkm5_set_met(pkm5 *p, uint16_t location, uint8_t level, uint8_t ball, uint8_t ot_gender)
{
    s16(p->data + OFS_MET_LOCATION, location);
    p->data[OFS_BALL] = ball;
    p->data[OFS_MET_LEVEL] = (uint8_t)((level & 0x7F) | (ot_gender ? 0x80 : 0));
}

void pkm5_set_party_stats(pkm5 *p, uint8_t level, uint16_t hp, const uint16_t stats[6], uint32_t status)
{
    p->party = true;
    s32(p->data + OFS_STATUS, status);
    p->data[OFS_LEVEL] = level;
    s16(p->data + OFS_HP, hp);
    for (int i = 0; i < 6; i++)
        s16(p->data + OFS_STATS + 2 * i, stats[i]);
}

void pkm5_calc_stats(const uint8_t base[6], const uint8_t ivs[6], const uint8_t evs[6], uint8_t level,
                     uint8_t nature, bool shedinja, uint16_t out[6])
{
    if (shedinja)
        out[0] = 1;
    else
        out[0] = (uint16_t)((2 * base[0] + ivs[0] + evs[0] / 4) * level / 100 + level + 10);
    const int up = nature % 25 / 5, down = nature % 25 % 5;
    for (int i = 1; i < 6; i++) {
        int v = (2 * base[i] + ivs[i] + evs[i] / 4) * level / 100 + 5;
        if (up != down) {
            if (i - 1 == up)
                v = v * 110 / 100;
            else if (i - 1 == down)
                v = v * 90 / 100;
        }
        out[i] = (uint16_t)v;
    }
}
