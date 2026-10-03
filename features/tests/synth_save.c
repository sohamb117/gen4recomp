/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Builds a synthetic Gen 4 save image directly from the decomp's layout
 * rules, independently of save4's own offset table:
 *   - SaveTableEntry_BodySize (pokeplatinum src/savedata.c) / SaveArray_sizeof
 *     (pokediamond arm9/src/save.c) and the gSaveTable order give the block
 *     offsets (see save4.c's header for the arithmetic);
 *   - SaveBlockFooter_Set writes {saveCounter, blockCounter, size, signature,
 *     blockID, CRC16-CCITT(body)} at the end of each block.
 */
#include "synth_save.h"

#include <string.h>

#include "ndsdata/ndsdata.h"

struct synth_layout {
    uint32_t general, storage, player, party, bag, vars, dex;
};

static const struct synth_layout kPt = {0xCF2C, 0x121E4, 0x64, 0x98, 0x630, 0xDAC, 0x1328};
static const struct synth_layout kDp = {0xC100, 0x121E0, 0x60, 0x90, 0x624, 0xD9C, 0x12DC};

static const struct synth_layout *lay(save4_game g) { return g == SAVE4_GAME_DP ? &kDp : &kPt; }

uint32_t synth_general_size(save4_game game) { return lay(game)->general; }
uint32_t synth_storage_size(save4_game game) { return lay(game)->storage; }

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

static void put_name(uint8_t *dst, int slots, const char *s)
{
    uint16_t buf[32];
    size_t n = 0;
    g4_text_encode(s, buf, (size_t)slots, &n);
    for (int i = 0; i < slots; i++)
        w16(dst + i * 2, (size_t)i < n ? buf[i] : 0xFFFF);
}

void synth_make_mon(pkm4 *p, uint16_t species, uint8_t level, uint32_t pid, const char *nickname,
                    uint16_t move0, int party)
{
    memset(p, 0, sizeof(*p));
    pkm4_set_pid(p, pid);
    pkm4_set_species(p, species);
    pkm4_set_ot_ids(p, SYNTH_TID, SYNTH_SID);
    pkm4_set_exp(p, (uint32_t)level * level * level);
    pkm4_set_friendship(p, 70);
    pkm4_set_ability(p, 65); /* Overgrow */
    pkm4_set_language(p, 2);  /* English */
    pkm4_set_move(p, 0, move0, 35, 0);
    for (int i = 0; i < 6; i++)
        pkm4_set_iv(p, i, (uint8_t)(i * 5 + 1));
    pkm4_set_ev(p, 0, 4);
    pkm4_set_gender_form(p, 0, 0);
    pkm4_set_nickname(p, nickname, 0);
    pkm4_set_ot_name(p, SYNTH_TRAINER_NAME);
    pkm4_set_origin_game(p, 12); /* VERSION_PLATINUM */
    pkm4_set_met(p, 1 /* Twinleaf Town */, level, 4 /* Poké Ball */, 0);
    if (party) {
        uint16_t stats[6] = {20, 11, 12, 10, 9, 10};
        pkm4_set_party_stats(p, level, 20, stats, 0);
    }
}

static uint32_t noise_state;
static uint8_t noise(void)
{
    noise_state = noise_state * 1664525u + 1013904223u;
    return (uint8_t)(noise_state >> 24);
}

static void footer(uint8_t *block, uint32_t size, uint8_t id, uint32_t counter)
{
    uint8_t *f = block + size - SAVE4_FOOTER_SIZE;
    w32(f, counter);
    w32(f + 4, counter);
    w32(f + 8, size);
    w32(f + 12, SAVE4_SIGNATURE);
    f[16] = id;
    f[17] = 0;
    w16(f + 18, save4_crc16(block, size - SAVE4_FOOTER_SIZE));
}

static void build_copy(uint8_t *copy, save4_game game, uint32_t counter, uint32_t money)
{
    const struct synth_layout *L = lay(game);
    uint8_t *gen = copy, *sto = copy + L->general;
    noise_state = 0xC0FFEEu; /* same noise in both copies */
    for (uint32_t i = 0; i < L->general + L->storage; i++)
        copy[i] = noise();

    /* PlayerSave */
    uint8_t *pl = gen + L->player;
    memset(pl, 0, 0x2C);
    put_name(pl + 0x04, 8, SYNTH_TRAINER_NAME);
    w16(pl + 0x14, SYNTH_TID);
    w16(pl + 0x16, SYNTH_SID);
    w32(pl + 0x18, money);
    pl[0x1C] = 0;          /* male */
    pl[0x1D] = 2;          /* English */
    pl[0x1E] = SYNTH_BADGES;
    pl[0x20] = 0;
    w16(pl + 0x24, 100);   /* coins */
    w16(pl + 0x26, 12);    /* 12:34:56 */
    pl[0x28] = 34;
    pl[0x29] = 56;

    /* Party */
    uint8_t *party = gen + L->party;
    w32(party, 6);
    w32(party + 4, 2);
    pkm4 mon;
    synth_make_mon(&mon, 387, 5, 0x12345678u, "TURTWIG", 33 /* Tackle */, 1);
    pkm4_encrypt(&mon, party + 8, PKM4_PARTY_SIZE);
    synth_make_mon(&mon, 1, 10, 0x9ABCDEF0u, "Bulby", 22 /* Vine Whip */, 1);
    pkm4_encrypt(&mon, party + 8 + PKM4_PARTY_SIZE, PKM4_PARTY_SIZE);
    for (int i = 2; i < 6; i++) {
        memset(&mon, 0, sizeof(mon));
        mon.party = true;
        pkm4_encrypt(&mon, party + 8 + i * PKM4_PARTY_SIZE, PKM4_PARTY_SIZE);
    }

    /* Bag: Potion x5 (items pocket), Poké Ball x10 (balls pocket) */
    uint8_t *bag = gen + L->bag;
    memset(bag, 0, 0x774);
    w16(bag + 0, 17);
    w16(bag + 2, 5);
    uint32_t balls = (165 + 50 + 100 + 12 + 40 + 64) * 4;
    w16(bag + balls, 4);
    w16(bag + balls + 2, 10);

    /* VarsFlags: zero, then a couple of known values */
    uint8_t *vars = gen + L->vars;
    memset(vars, 0, 288 * 2 + 2912 / 8);
    w16(vars + 0x10 * 2, 0x1234); /* var 0x4010 */
    vars[288 * 2 + 10 / 8] |= 1 << (10 % 8); /* flag 10 */

    /* Pokédex: magic + seen/caught bitmaps */
    uint8_t *dex = gen + L->dex;
    memset(dex, 0, 4 + 64 * 4);
    w32(dex, 0xBEEFCAFEu);
    unsigned b = 387 - 1;
    dex[4 + b / 8] |= (uint8_t)(1 << (b % 8));
    dex[0x44 + b / 8] |= (uint8_t)(1 << (b % 8));
    b = 1 - 1;
    dex[0x44 + b / 8] |= (uint8_t)(1 << (b % 8));

    /* PCBoxes */
    memset(sto, 0, 0x121C8);
    w32(sto, 0);
    for (int i = 0; i < SAVE4_BOX_COUNT * SAVE4_BOX_SLOTS; i++) {
        memset(&mon, 0, sizeof(mon));
        pkm4_encrypt(&mon, sto + 4 + i * PKM4_BOX_SIZE, PKM4_BOX_SIZE);
    }
    synth_make_mon(&mon, SYNTH_BOX_SPECIES, 7, 0x0BADF00Du, "STARLY", 33, 0);
    pkm4_encrypt(&mon, sto + 4, PKM4_BOX_SIZE);
    uint8_t *names = sto + 4 + SAVE4_BOX_COUNT * SAVE4_BOX_SLOTS * PKM4_BOX_SIZE;
    for (int i = 0; i < SAVE4_BOX_COUNT; i++) {
        char nm[16];
        int n = 0;
        const char *pre = "Box ";
        while (pre[n]) {
            nm[n] = pre[n];
            n++;
        }
        if (i + 1 >= 10)
            nm[n++] = (char)('0' + (i + 1) / 10);
        nm[n++] = (char)('0' + (i + 1) % 10);
        nm[n] = 0;
        put_name(names + i * 40, 20, nm);
    }

    footer(gen, L->general, 0, counter);
    footer(sto, L->storage, 1, counter);
}

void synth_save_build(uint8_t *img, save4_game game)
{
    memset(img, 0xFF, SAVE4_IMAGE_SIZE);
    build_copy(img, game, SYNTH_COUNTER_OLD, SYNTH_MONEY_OLD);
    build_copy(img + SAVE4_COPY_SIZE, game, SYNTH_COUNTER_NEW, SYNTH_MONEY_NEW);
}
