/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "synth_save5.h"

#include <string.h>

static void w16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}
static void w32(uint8_t *p, uint32_t v)
{
    w16(p, (uint16_t)v);
    w16(p + 2, (uint16_t)(v >> 16));
}

/* UTF-16 + 0xFFFF terminator, zero padded to `units`. */
static void wstr(uint8_t *p, const char *ascii, int units)
{
    int n = (int)strlen(ascii);
    for (int i = 0; i < units; i++)
        w16(p + 2 * i, i < n ? (uint16_t)ascii[i] : i == n ? 0xFFFF : 0);
}

static uint32_t noise_state;
static uint8_t noise(void)
{
    noise_state = noise_state * 1103515245u + 12345u;
    return (uint8_t)(noise_state >> 16);
}

void synth5_make_mon(pkm5 *p, uint16_t species, uint8_t level, uint32_t pid, uint8_t nature, const char *nickname,
                     uint16_t move0, int party)
{
    memset(p, 0, sizeof *p);
    pkm5_set_pid(p, pid);
    pkm5_set_species(p, species);
    pkm5_set_ot_ids(p, SYNTH5_TID, SYNTH5_SID);
    pkm5_set_exp(p, (uint32_t)level * level * level);
    pkm5_set_friendship(p, 70);
    pkm5_set_ability(p, 65, false);
    pkm5_set_language(p, 2);
    pkm5_set_nature(p, nature);
    pkm5_set_move(p, 0, move0, 35, 0);
    for (int i = 0; i < 6; i++)
        pkm5_set_iv(p, i, (uint8_t)(i * 5 + 1));
    pkm5_set_ev(p, 0, 4);
    pkm5_set_gender_form(p, 0, 0);
    pkm5_set_nickname(p, nickname, 0);
    pkm5_set_ot_name(p, SYNTH5_TRAINER_NAME);
    pkm5_set_origin_game(p, 21);
    pkm5_set_met(p, 4, level, 4, 0);
    if (party) {
        const uint16_t stats[6] = {20, 11, 12, 13, 14, 15};
        pkm5_set_party_stats(p, level, 19, stats, 0);
    }
}

void synth5_make_pgf(uint8_t card[SAVE5_PGF_SIZE], uint8_t type, uint16_t id)
{
    memset(card, 0, SAVE5_PGF_SIZE);
    if (type == SAVE5_MG_POKEMON) {
        w16(card + 0x1A, 494); /* Victini */
        card[0x5B] = 15;       /* level */
        card[0x0E] = 4;        /* ball */
    } else {
        w16(card + 0x00, 1); /* item: Master Ball */
    }
    wstr(card + 0x60, "Test Card", SAVE5_PGF_TITLE_LEN);
    w16(card + 0xB0, id);
    card[0xB3] = type;
}

/* Offsets of the interpreted blocks (save5.c's table). */
enum {
    B_BOXN = 0x00000,
    B_BOX1 = 0x00400,
    B_BAG = 0x18400,
    B_PARTY = 0x18E00,
    B_TRAINER = 0x19400,
    B_POSITION = 0x19500,
    B_MYSTERY = 0x1C800,
    B_EVENT = 0x20100,
    B_MISC = 0x21200,
    B_DEX = 0x21600,
};

static const uint32_t kBlockOfs[SAVE5_DATA_BLOCKS] = {
    0x00000, 0x00400, 0x01400, 0x02400, 0x03400, 0x04400, 0x05400, 0x06400, 0x07400, 0x08400, 0x09400, 0x0A400,
    0x0B400, 0x0C400, 0x0D400, 0x0E400, 0x0F400, 0x10400, 0x11400, 0x12400, 0x13400, 0x14400, 0x15400, 0x16400,
    0x17400, 0x18400, 0x18E00, 0x19400, 0x19500, 0x19600, 0x1AA00, 0x1B200, 0x1C000, 0x1C100, 0x1C800, 0x1D300,
    0x1D500, 0x1D900, 0x1DA00, 0x1DC00, 0x1DD00, 0x1E200, 0x1F700, 0x1FA00, 0x1FD00, 0x20100, 0x20500, 0x20600,
    0x20900, 0x20A00, 0x20E00, 0x21000, 0x21200, 0x21300, 0x21500, 0x21600, 0x21B00, 0x21C00, 0x21D00, 0x21F00,
    0x22B00, 0x22C00, 0x23500, 0x23600, 0x23900, 0x23A00, 0x23B00, 0x23D00, 0x23E00};
static const uint16_t kBlockSize[SAVE5_DATA_BLOCKS] = {
    0x03E0, 0x0FF0, 0x0FF0, 0x0FF0, 0x0FF0, 0x0FF0, 0x0FF0, 0x0FF0, 0x0FF0, 0x0FF0, 0x0FF0, 0x0FF0, 0x0FF0, 0x0FF0,
    0x0FF0, 0x0FF0, 0x0FF0, 0x0FF0, 0x0FF0, 0x0FF0, 0x0FF0, 0x0FF0, 0x0FF0, 0x0FF0, 0x0FF0, 0x09C0, 0x0534, 0x0068,
    0x009C, 0x1338, 0x07C4, 0x0D54, 0x002C, 0x0658, 0x0A94, 0x01AC, 0x03EC, 0x005C, 0x01E0, 0x00A8, 0x0460, 0x1400,
    0x02A4, 0x02DC, 0x034C, 0x03EC, 0x00F8, 0x02FC, 0x0094, 0x035C, 0x01CC, 0x0168, 0x00EC, 0x01B0, 0x001C, 0x04D4,
    0x0034, 0x003C, 0x01AC, 0x0B90, 0x009C, 0x0850, 0x0028, 0x0284, 0x0010, 0x005C, 0x016C, 0x0040, 0x00FC};

static int interpreted(uint32_t ofs)
{
    return ofs < 0x19600 || ofs == B_MYSTERY || ofs == B_EVENT || ofs == B_MISC || ofs == B_DEX;
}

static void mg_crypt(uint8_t *d, size_t n, uint32_t seed)
{
    for (size_t i = 0; i + 1 < n; i += 2) {
        seed = seed * 0x41C64E6Du + 0x6073u;
        d[i] ^= (uint8_t)(seed >> 16);
        d[i + 1] ^= (uint8_t)(seed >> 24);
    }
}

static void build_copy(uint8_t *c, save5_game game, uint32_t counter, uint32_t money)
{
    for (int b = 0; b < SAVE5_DATA_BLOCKS; b++) {
        uint8_t *blk = c + kBlockOfs[b];
        if (interpreted(kBlockOfs[b]))
            memset(blk, 0, kBlockSize[b]);
        else
            for (int i = 0; i < kBlockSize[b]; i++)
                blk[i] = noise();
        w16(blk + kBlockSize[b], (uint16_t)(1 + b % 3)); /* footer write counter */
    }

    /* box names, wallpapers, current box 2, 8 boxes unlocked */
    char name[16];
    for (int b = 0; b < SAVE5_BOX_COUNT; b++) {
        int n = 0;
        memcpy(name, "BOX ", 4);
        n = 4;
        if (b + 1 >= 10)
            name[n++] = (char)('0' + (b + 1) / 10);
        name[n++] = (char)('0' + (b + 1) % 10);
        name[n] = 0;
        wstr(c + B_BOXN + 4 + 0x28 * b, name, 10);
        c[B_BOXN + 0x3C4 + b] = (uint8_t)(b % 16);
    }
    c[B_BOXN] = 1;
    c[B_BOXN + 0x3DD] = 8;

    /* trainer */
    uint8_t *t = c + B_TRAINER;
    for (int i = 0; i < 0x68; i++)
        t[i] = noise(); /* unknown fields keep their bytes */
    wstr(t + 0x04, SYNTH5_TRAINER_NAME, 8);
    w16(t + 0x14, SYNTH5_TID);
    w16(t + 0x16, SYNTH5_SID);
    t[0x1E] = 2;
    t[0x1F] = game == SAVE5_GAME_WHITE ? 20 : 21;
    t[0x21] = 0;
    w16(t + 0x24, 12);
    t[0x26] = 34;
    t[0x27] = 56;
    /* 2011-03-06 13:45 */
    w32(t + 0x28, 11u | 3u << 7 | 6u << 11 | 13u << 16 | 45u << 21);

    /* position: zone, fx32 x/y/z */
    uint8_t *pos = c + B_POSITION;
    w32(pos + 0x80, SYNTH5_MAP);
    w32(pos + 0x84, 7u << 16 | 0x8000);
    w32(pos + 0x88, 0);
    w32(pos + 0x8C, 9u << 16 | 0x8000);

    /* money, badges */
    w32(c + B_MISC, money);
    c[B_MISC + 4] = SYNTH5_BADGES;
    wstr(c + B_MISC + 0x20, "Hello!", 8);

    /* party */
    pkm5 p;
    w32(c + B_PARTY, 6);
    c[B_PARTY + 4] = 2;
    synth5_make_mon(&p, 495, 5, 0x12345678u, 3, "SNIVY", 33, 1);
    pkm5_encrypt(&p, c + B_PARTY + 8, PKM5_PARTY_SIZE);
    synth5_make_mon(&p, 504, 3, 0x9ABCDEF0u, 10, "Pat", 33, 1);
    pkm5_set_nickname(&p, "Pat", 1);
    pkm5_encrypt(&p, c + B_PARTY + 8 + PKM5_PARTY_SIZE, PKM5_PARTY_SIZE);

    /* boxes: empty slots hold an encrypted all-zero Pokémon (as the game
     * stores them); box 1 slot 1 a Lillipup */
    memset(&p, 0, sizeof p);
    for (int b = 0; b < SAVE5_BOX_COUNT; b++)
        for (int i = 0; i < SAVE5_BOX_SLOTS; i++)
            pkm5_encrypt(&p, c + B_BOX1 + 0x1000 * b + PKM5_BOX_SIZE * i, PKM5_BOX_SIZE);
    synth5_make_mon(&p, SYNTH5_BOX_SPECIES, 4, 0x0BADF00Du, 7, "LILLIPUP", 33, 0);
    pkm5_encrypt(&p, c + B_BOX1, PKM5_BOX_SIZE);

    /* bag: 10 Poke Balls in items slot 1, 5 Potions in medicine slot 1 */
    w16(c + B_BAG + 0x000, 4);
    w16(c + B_BAG + 0x002, 10);
    w16(c + B_BAG + 0x7D8, 17);
    w16(c + B_BAG + 0x7DA, 5);

    /* event work */
    w16(c + B_EVENT + 2 * 0x10, 0x1234);
    c[B_EVENT + 0x27C + 10 / 8] |= 1 << (10 % 8);
    c[B_EVENT + 0x27C + 2000 / 8] |= 1 << (2000 % 8);

    /* Pokédex: magic, Snivy caught+seen+displayed, Patrat seen+displayed */
    uint8_t *dx = c + B_DEX;
    w32(dx, 0xBEEFCAFEu);
    dx[0x08 + (494 >> 3)] |= 1 << (494 & 7);
    dx[0x5C + (494 >> 3)] |= 1 << (494 & 7);
    dx[0x1AC + (494 >> 3)] |= 1 << (494 & 7);
    dx[0x5C + 0x54 + (503 >> 3)] |= 1 << (503 & 7); /* female seen */
    dx[0x1AC + 0x54 + (503 >> 3)] |= 1 << (503 & 7);

    /* Mystery Gift: card SYNTH5_CARD_ID in slot 1, its received flag */
    uint8_t *mg = c + B_MYSTERY;
    mg[SYNTH5_CARD_ID >> 3] |= 1 << (SYNTH5_CARD_ID & 7);
    synth5_make_pgf(mg + 0x100, SAVE5_MG_POKEMON, SYNTH5_CARD_ID);
    mg_crypt(mg, 0xA90, SYNTH5_MG_SEED);
    w32(mg + 0xA90, SYNTH5_MG_SEED);

    /* checksum block footer */
    w32(c + 0x23F8C, counter);
    w32(c + 0x23F90, SAVE5_COPY_USED);
    w32(c + 0x23F94, SAVE5_MAGIC);
    w16(c + 0x23F98, 0);
}

void synth5_build(uint8_t *img, save5_game game)
{
    noise_state = 0x5EED5u;
    memset(img, 0xFF, SAVE5_IMAGE_SIZE);
    build_copy(img, game, SYNTH5_COUNTER_OLD, SYNTH5_MONEY_OLD);
    noise_state = 0x5EED5u; /* identical unknown blocks in both copies */
    build_copy(img + SAVE5_COPY_OFFSET, game, SYNTH5_COUNTER_NEW, SYNTH5_MONEY_NEW);
    for (uint32_t i = 2 * SAVE5_COPY_OFFSET; i < SAVE5_IMAGE_SIZE; i++)
        img[i] = noise();
    save5_fix_all_checksums(img);
}
