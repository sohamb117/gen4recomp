/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Gen 4 Pokémon data (pokeplatinum include/struct_defs/pokemon.h,
 * src/pokemon.c, src/math_util.c; HG/SS: pokeheartgold
 * include/pokemon_types_def.h, the same 136/236-byte format).
 *
 * BoxPokemon (136 bytes): u32 personality, u16 flags, u16 checksum, then four
 * 32-byte blocks A-D stored in a PID-dependent order
 * (BoxPokemon_GetDataBlock: index ((pid & 0x3E000) >> 13), 24..31 alias
 * 0..7) and encrypted as one stream by EncodeData(seed = checksum): each u16
 * is XORed with the high half of the LCG seed = seed * 1103515245 + 24691.
 * The checksum is the u16 sum of the 64 decrypted block words.
 * Pokemon (236 bytes) appends PartyPokemon (100 bytes) encrypted with
 * seed = personality.
 *
 * HG/SS-only bytes that D/P/Pt leave unused (block B 0x19 shiny leaves,
 * block D 0x1E HGSS_Pokeball, 0x1F mood) are only written by
 * pkm4_set_ball_hgss; every other setter keeps them.
 */
#include "save4/save4.h"

#include <string.h>

#include "ndsdata/ndsdata.h"

/* Offsets in canonical (unshuffled) order. */
enum {
    OFS_PID = 0x00,
    OFS_FLAGS = 0x04,
    OFS_CHECKSUM = 0x06,
    OFS_BLOCKS = 0x08,
    BLOCK_SIZE = 0x20,
    /* block A */
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
    /* block B */
    OFS_MOVES = 0x28,
    OFS_PP = 0x30,
    OFS_PPUPS = 0x34,
    OFS_IVS = 0x38,
    OFS_FATEFUL_GENDER_FORM = 0x40,
    OFS_EGG_LOC_PT = 0x44,
    OFS_MET_LOC_PT = 0x46,
    /* block C */
    OFS_NICKNAME = 0x48, /* 11 charcodes */
    OFS_ORIGIN_GAME = 0x5F,
    /* block D */
    OFS_OT_NAME = 0x68, /* 8 charcodes */
    OFS_EGG_LOC_DP = 0x7E,
    OFS_MET_LOC_DP = 0x80,
    OFS_POKERUS = 0x82,
    OFS_BALL = 0x83,
    OFS_METLEVEL_OTGENDER = 0x84,
    OFS_BALL_HGSS = 0x86, /* PokemonDataBlockD.HGSS_Pokeball (block D 0x1E) */
    /* party tail */
    OFS_STATUS = 0x88,
    OFS_LEVEL = 0x8C,
    OFS_HP = 0x8E,
    OFS_STATS = 0x90
};

#define NICK_CODES 11
#define OT_CODES 8

/* pokeheartgold include/config.h, constants/items.h, constants/balls.h */
#define VERSION_HEARTGOLD 7
#define VERSION_SOULSILVER 8
#define ITEM_MASTER_BALL 1
#define ITEM_CHERISH_BALL 16
#define ITEM_FAST_BALL 492
#define ITEM_SPORT_BALL 499
#define BALL_POKE 4
#define BALL_FAST 17

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

/* Position (stored slot) of blocks A, B, C, D for each shuffle index,
 * transcribed from the DATA_BLOCK_SHUFFLE_CASE table in src/pokemon.c. */
static const uint8_t kShuffle[24][4] = {
    {0, 1, 2, 3}, {0, 1, 3, 2}, {0, 2, 1, 3}, {0, 3, 1, 2}, {0, 2, 3, 1}, {0, 3, 2, 1},
    {1, 0, 2, 3}, {1, 0, 3, 2}, {2, 0, 1, 3}, {3, 0, 1, 2}, {2, 0, 3, 1}, {3, 0, 2, 1},
    {1, 2, 0, 3}, {1, 3, 0, 2}, {2, 1, 0, 3}, {3, 1, 0, 2}, {2, 3, 0, 1}, {3, 2, 0, 1},
    {1, 2, 3, 0}, {1, 3, 2, 0}, {2, 1, 3, 0}, {3, 1, 2, 0}, {2, 3, 1, 0}, {3, 2, 1, 0},
};

uint32_t pkm4_shuffle_index(uint32_t pid)
{
    return ((pid & 0x3E000) >> 13) % 24;
}

static void crypt(uint8_t *data, size_t bytes, uint32_t seed)
{
    for (size_t i = 0; i + 1 < bytes; i += 2) {
        seed = seed * 1103515245u + 24691u;
        uint16_t k = (uint16_t)(seed >> 16);
        data[i] ^= (uint8_t)k;
        data[i + 1] ^= (uint8_t)(k >> 8);
    }
}

uint16_t pkm4_calc_checksum(const pkm4 *p)
{
    uint16_t sum = 0;
    for (int i = 0; i < 4 * BLOCK_SIZE; i += 2)
        sum = (uint16_t)(sum + g16(p->data + OFS_BLOCKS + i));
    return sum;
}

save4_status pkm4_decrypt(const uint8_t *enc, size_t len, pkm4 *out)
{
    if (len != PKM4_BOX_SIZE && len != PKM4_PARTY_SIZE)
        return SAVE4_ERR_ARG;
    memset(out, 0, sizeof(*out));
    uint8_t stored[4 * BLOCK_SIZE];
    memcpy(out->data, enc, OFS_BLOCKS);
    memcpy(stored, enc + OFS_BLOCKS, sizeof(stored));
    uint32_t pid = g32(enc + OFS_PID);
    uint16_t checksum = g16(enc + OFS_CHECKSUM);
    crypt(stored, sizeof(stored), checksum);
    const uint8_t *order = kShuffle[pkm4_shuffle_index(pid)];
    for (int b = 0; b < 4; b++)
        memcpy(out->data + OFS_BLOCKS + b * BLOCK_SIZE, stored + order[b] * BLOCK_SIZE, BLOCK_SIZE);
    if (len == PKM4_PARTY_SIZE) {
        memcpy(out->data + PKM4_BOX_SIZE, enc + PKM4_BOX_SIZE, PKM4_PARTY_SIZE - PKM4_BOX_SIZE);
        crypt(out->data + PKM4_BOX_SIZE, PKM4_PARTY_SIZE - PKM4_BOX_SIZE, pid);
        out->party = true;
    }
    return pkm4_calc_checksum(out) == checksum ? SAVE4_OK : SAVE4_ERR_PKM_CHECKSUM;
}

save4_status pkm4_encrypt(pkm4 *p, uint8_t *out, size_t len)
{
    if (len != PKM4_BOX_SIZE && len != PKM4_PARTY_SIZE)
        return SAVE4_ERR_ARG;
    uint16_t checksum = pkm4_calc_checksum(p);
    s16(p->data + OFS_CHECKSUM, checksum);
    uint32_t pid = g32(p->data + OFS_PID);
    uint8_t stored[4 * BLOCK_SIZE];
    const uint8_t *order = kShuffle[pkm4_shuffle_index(pid)];
    for (int b = 0; b < 4; b++)
        memcpy(stored + order[b] * BLOCK_SIZE, p->data + OFS_BLOCKS + b * BLOCK_SIZE, BLOCK_SIZE);
    crypt(stored, sizeof(stored), checksum);
    memcpy(out, p->data, OFS_BLOCKS);
    memcpy(out + OFS_BLOCKS, stored, sizeof(stored));
    if (len == PKM4_PARTY_SIZE) {
        uint8_t tail[PKM4_PARTY_SIZE - PKM4_BOX_SIZE];
        if (p->party)
            memcpy(tail, p->data + PKM4_BOX_SIZE, sizeof(tail));
        else
            memset(tail, 0, sizeof(tail));
        crypt(tail, sizeof(tail), pid);
        memcpy(out + PKM4_BOX_SIZE, tail, sizeof(tail));
    }
    return SAVE4_OK;
}

bool pkm4_is_empty(const pkm4 *p)
{
    return g16(p->data + OFS_SPECIES) == 0;
}

static void decode_name(const uint8_t *src, int codes, char *out, size_t cap)
{
    uint16_t buf[16];
    for (int i = 0; i < codes; i++)
        buf[i] = g16(src + i * 2);
    g4_text_decode(buf, (size_t)codes, out, cap);
}

void pkm4_info_get(const pkm4 *p, pkm4_info *info)
{
    const uint8_t *d = p->data;
    memset(info, 0, sizeof(*info));
    info->pid = g32(d + OFS_PID);
    info->checksum = g16(d + OFS_CHECKSUM);
    info->checksum_ok = pkm4_calc_checksum(p) == info->checksum;
    info->species = g16(d + OFS_SPECIES);
    info->held_item = g16(d + OFS_HELD);
    info->tid = g16(d + OFS_TID);
    info->sid = g16(d + OFS_SID);
    info->exp = g32(d + OFS_EXP);
    info->friendship = d[OFS_FRIENDSHIP];
    info->ability = d[OFS_ABILITY];
    info->markings = d[OFS_MARKINGS];
    info->language = d[OFS_LANGUAGE];
    memcpy(info->evs, d + OFS_EVS, 6);
    memcpy(info->contest, d + OFS_CONTEST, 6);
    for (int i = 0; i < 4; i++) {
        info->moves[i] = g16(d + OFS_MOVES + i * 2);
        info->pp[i] = d[OFS_PP + i];
        info->pp_ups[i] = d[OFS_PPUPS + i];
    }
    uint32_t iv = g32(d + OFS_IVS);
    for (int i = 0; i < 6; i++)
        info->ivs[i] = (uint8_t)((iv >> (5 * i)) & 31);
    info->is_egg = (iv >> 30) & 1;
    info->has_nickname = (iv >> 31) & 1;
    uint8_t fgf = d[OFS_FATEFUL_GENDER_FORM];
    info->fateful = fgf & 1;
    info->gender = (fgf >> 1) & 3;
    info->form = fgf >> 3;
    decode_name(d + OFS_NICKNAME, NICK_CODES, info->nickname, sizeof(info->nickname));
    info->origin_game = d[OFS_ORIGIN_GAME];
    decode_name(d + OFS_OT_NAME, OT_CODES, info->ot_name, sizeof(info->ot_name));
    /* Platinum stores locations in block B and mirrors D/P-representable ones
     * in block D; D/P only fill block D (PKHeX PK4 uses the same rule). */
    uint16_t egg_pt = g16(d + OFS_EGG_LOC_PT), met_pt = g16(d + OFS_MET_LOC_PT);
    info->egg_location = egg_pt ? egg_pt : g16(d + OFS_EGG_LOC_DP);
    info->met_location = met_pt ? met_pt : g16(d + OFS_MET_LOC_DP);
    info->pokerus = d[OFS_POKERUS];
    /* GetBoxMonDataInternal MON_DATA_POKEBALL (pokeheartgold src/pokemon.c):
     * an HG/SS-origin mon with a nonzero HGSS ball shows that one. */
    info->ball = d[OFS_BALL];
    if ((info->origin_game == VERSION_HEARTGOLD || info->origin_game == VERSION_SOULSILVER) && d[OFS_BALL_HGSS])
        info->ball = d[OFS_BALL_HGSS];
    info->met_level = d[OFS_METLEVEL_OTGENDER] & 0x7F;
    info->ot_gender = d[OFS_METLEVEL_OTGENDER] >> 7;
    info->nature = (uint8_t)(info->pid % 25);
    info->shiny = (uint16_t)(info->tid ^ info->sid ^ (info->pid >> 16) ^ (info->pid & 0xFFFF)) < 8;
    if (p->party) {
        info->has_party_data = true;
        info->status = g32(d + OFS_STATUS);
        info->level = d[OFS_LEVEL];
        info->hp = g16(d + OFS_HP);
        for (int i = 0; i < 6; i++)
            info->stats[i] = g16(d + OFS_STATS + i * 2);
    }
}

void pkm4_set_pid(pkm4 *p, uint32_t pid) { s32(p->data + OFS_PID, pid); }
void pkm4_set_species(pkm4 *p, uint16_t species) { s16(p->data + OFS_SPECIES, species); }
void pkm4_set_held_item(pkm4 *p, uint16_t item) { s16(p->data + OFS_HELD, item); }
void pkm4_set_ot_ids(pkm4 *p, uint16_t tid, uint16_t sid)
{
    s16(p->data + OFS_TID, tid);
    s16(p->data + OFS_SID, sid);
}
void pkm4_set_exp(pkm4 *p, uint32_t exp) { s32(p->data + OFS_EXP, exp); }
void pkm4_set_friendship(pkm4 *p, uint8_t v) { p->data[OFS_FRIENDSHIP] = v; }
void pkm4_set_ability(pkm4 *p, uint8_t ability) { p->data[OFS_ABILITY] = ability; }
void pkm4_set_language(pkm4 *p, uint8_t lang) { p->data[OFS_LANGUAGE] = lang; }

void pkm4_set_ev(pkm4 *p, int stat, uint8_t v)
{
    if (stat >= 0 && stat < 6)
        p->data[OFS_EVS + stat] = v;
}

void pkm4_set_move(pkm4 *p, int slot, uint16_t move, uint8_t pp, uint8_t pp_ups)
{
    if (slot < 0 || slot >= 4)
        return;
    s16(p->data + OFS_MOVES + slot * 2, move);
    p->data[OFS_PP + slot] = pp;
    p->data[OFS_PPUPS + slot] = pp_ups;
}

void pkm4_set_iv(pkm4 *p, int stat, uint8_t v)
{
    if (stat < 0 || stat >= 6)
        return;
    uint32_t iv = g32(p->data + OFS_IVS);
    iv &= ~(31u << (5 * stat));
    iv |= (uint32_t)(v & 31) << (5 * stat);
    s32(p->data + OFS_IVS, iv);
}

void pkm4_set_is_egg(pkm4 *p, bool egg)
{
    uint32_t iv = g32(p->data + OFS_IVS);
    iv = egg ? (iv | (1u << 30)) : (iv & ~(1u << 30));
    s32(p->data + OFS_IVS, iv);
}

void pkm4_set_gender_form(pkm4 *p, uint8_t gender, uint8_t form)
{
    uint8_t *b = p->data + OFS_FATEFUL_GENDER_FORM;
    *b = (uint8_t)((*b & 1) | ((gender & 3) << 1) | ((form & 31) << 3));
}

static save4_status put_name(uint8_t *dst, int codes, const char *utf8)
{
    uint16_t buf[16];
    size_t n;
    nd_status st = g4_text_encode(utf8, buf, (size_t)codes, &n);
    if (st == ND_ERR_RANGE)
        return SAVE4_ERR_RANGE;
    if (st != ND_OK)
        return SAVE4_ERR_ENCODE;
    /* Terminate and pad with EOS like the game's string copy does. */
    for (int i = 0; i < codes; i++)
        s16(dst + i * 2, (size_t)i < n ? buf[i] : 0xFFFF);
    return SAVE4_OK;
}

save4_status pkm4_set_nickname(pkm4 *p, const char *utf8, bool is_nickname)
{
    save4_status st = put_name(p->data + OFS_NICKNAME, NICK_CODES, utf8);
    if (st != SAVE4_OK)
        return st;
    uint32_t iv = g32(p->data + OFS_IVS);
    iv = is_nickname ? (iv | (1u << 31)) : (iv & ~(1u << 31));
    s32(p->data + OFS_IVS, iv);
    return SAVE4_OK;
}

save4_status pkm4_set_ot_name(pkm4 *p, const char *utf8)
{
    return put_name(p->data + OFS_OT_NAME, OT_CODES, utf8);
}

void pkm4_set_origin_game(pkm4 *p, uint8_t game) { p->data[OFS_ORIGIN_GAME] = game; }

void pkm4_set_met(pkm4 *p, uint16_t location, uint8_t level, uint8_t ball, uint8_t ot_gender)
{
    s16(p->data + OFS_MET_LOC_PT, location);
    s16(p->data + OFS_MET_LOC_DP, location);
    p->data[OFS_BALL] = ball;
    p->data[OFS_METLEVEL_OTGENDER] = (uint8_t)((level & 0x7F) | ((ot_gender & 1) << 7));
}

save4_status pkm4_set_ball_hgss(pkm4 *p, uint16_t ball_item)
{
    if (ball_item >= ITEM_MASTER_BALL && ball_item <= ITEM_CHERISH_BALL) {
        p->data[OFS_BALL_HGSS] = (uint8_t)ball_item;
        p->data[OFS_BALL] = (uint8_t)ball_item;
    } else if (ball_item >= ITEM_FAST_BALL && ball_item <= ITEM_SPORT_BALL) {
        p->data[OFS_BALL_HGSS] = (uint8_t)(ball_item - (ITEM_FAST_BALL - BALL_FAST));
        p->data[OFS_BALL] = BALL_POKE;
    } else {
        return SAVE4_ERR_RANGE;
    }
    return SAVE4_OK;
}

void pkm4_set_party_stats(pkm4 *p, uint8_t level, uint16_t hp, const uint16_t stats[6], uint32_t status)
{
    p->party = true;
    s32(p->data + OFS_STATUS, status);
    p->data[OFS_LEVEL] = level;
    s16(p->data + OFS_HP, hp);
    for (int i = 0; i < 6; i++)
        s16(p->data + OFS_STATS + i * 2, stats[i]);
}

/* Gen 4 stat formula (pokeplatinum src/pokemon.c Pokemon_CalcStats) with
 * the nature modifier of Pokemon_GetNatureStatValue: +10% on stat
 * nature/5 and -10% on nature%5, over Atk Def Spe SpA SpD. */
void pkm4_calc_stats(const uint8_t base[6], const uint8_t ivs[6], const uint8_t evs[6], uint8_t level,
                     uint8_t nature, bool shedinja, uint16_t out[6])
{
    if (shedinja)
        out[0] = 1;
    else
        out[0] = (uint16_t)((2 * base[0] + ivs[0] + evs[0] / 4) * level / 100 + level + 10);
    int up = nature % 25 / 5, down = nature % 25 % 5;
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
