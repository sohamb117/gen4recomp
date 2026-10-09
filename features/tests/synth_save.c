/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Builds a synthetic Gen 4 save image directly from the decomp's layout
 * rules, independently of save4's own offset table:
 *   - SaveTableEntry_BodySize (pokeplatinum src/savedata.c) / SaveArray_sizeof
 *     (pokediamond arm9/src/save.c) and the gSaveTable order give the block
 *     offsets (see save4.c's header for the arithmetic);
 *   - SaveBlockFooter_Set writes {saveCounter, blockCounter, size, signature,
 *     blockID, CRC16-CCITT(body)} at the end of each block.
 *   - HG/SS (pokeheartgold src/save.c): GetSaveChunkSizePlusCRC entries in
 *     gSaveChunkHeaders order, the storage block at the next 0x100 boundary
 *     (SaveData_InitSlotSpecs), and SaveSlot_BuildFooter's
 *     {count, size, magic, slot u16, CRC16-CCITT(body)}; PCStorage
 *     (include/pokemon_storage_system.h) with 0x1000-byte boxes.
 */
#include "synth_save.h"

#include <string.h>

#include "ndsdata/ndsdata.h"

struct synth_layout {
    int hgss;
    uint32_t general, storage_off, storage, footer;
    uint32_t player, party, bag, bag_size, vars, nvars, dex;
    uint32_t mystery, mystery_body, mystery_crc; /* MysteryGift entry; CRC offset, 0 = none */
    const uint8_t *pockets;
    uint32_t pc_size, box_mons, box_stride, box_names;
};

static const uint8_t kSinnohPockets[8] = {165, 50, 100, 12, 40, 64, 15, 30};
/* pokeheartgold include/constants/items.h NUM_BAG_* */
static const uint8_t kJohtoPockets[8] = {165, 50, 101, 12, 40, 64, 24, 30};

static const struct synth_layout kPt = {0,     0xCF2C, 0xCF2C, 0x121E4, 0x14,   0x64,   0x98,
                                        0x630, 0x774,  0xDAC,  288,     0x1328, 0xB4C0, 0x1330,
                                        0x132C, kSinnohPockets, 0x121C8, 4, 30 * 136, 0x11EE4};
static const struct synth_layout kDp = {0,     0xC100, 0xC100, 0x121E0, 0x14,   0x60,   0x90,
                                        0x624, 0x774,  0xD9C,  288,     0x12DC, 0xA6D0, 0x1358,
                                        0,     kSinnohPockets, 0x121C8, 4, 30 * 136, 0x11EE4};
/* HG/SS: SysInfo 0x5C -> PLAYERDATA 0x60 -> Party 0x90 -> Bag 0x644 (0x79C)
 * -> SaveVarsFlags 0xDE4 (u16 vars[0x170], flags) -> LocalFieldData 0x1234
 * -> Pokedex 0x12B8; MysteryGiftSave 0x9D3C (0x1680 + CRC); general block
 * 0xF628; PCStorage at 0xF700 (sizeof 0x122FC, block 0x12310). */
static const struct synth_layout kHgss = {1,     0xF628, 0xF700, 0x12310, 0x10,   0x60,   0x90,
                                          0x644, 0x79C,  0xDE4,  368,     0x12B8, 0x9D3C, 0x1684,
                                          0x1680, kJohtoPockets, 0x122FC, 0, 0x1000, 0x12008};

static int is_hgss(save4_game g) { return g == SAVE4_GAME_HG || g == SAVE4_GAME_SS; }

static const struct synth_layout *lay(save4_game g)
{
    return is_hgss(g) ? &kHgss : g == SAVE4_GAME_DP ? &kDp : &kPt;
}

uint32_t synth_general_size(save4_game game) { return lay(game)->general; }
uint32_t synth_storage_offset(save4_game game) { return lay(game)->storage_off; }
uint32_t synth_storage_size(save4_game game) { return lay(game)->storage; }
uint32_t synth_footer_size(save4_game game) { return lay(game)->footer; }

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

/* An HG/SS-caught mon: its version, and the ball through HG/SS's
 * SetMonData(MON_DATA_POKEBALL). */
static void make_johto(pkm4 *p, save4_game game, uint16_t ball_item)
{
    pkm4_set_origin_game(p, game == SAVE4_GAME_HG ? 7 : 8);
    pkm4_set_ball_hgss(p, ball_item);
}

static uint32_t noise_state;
static uint8_t noise(void)
{
    noise_state = noise_state * 1664525u + 1013904223u;
    return (uint8_t)(noise_state >> 24);
}

static void footer(const struct synth_layout *L, uint8_t *block, uint32_t size, uint8_t id, uint32_t counter)
{
    uint8_t *f = block + size - L->footer;
    if (L->hgss) {
        w32(f, counter); /* SaveChunkFooter: count, size, magic, slot, crc */
        w32(f + 4, size);
        w32(f + 8, SAVE4_SIGNATURE);
        w16(f + 12, id);
        w16(f + 14, save4_crc16(block, size - L->footer));
        return;
    }
    w32(f, counter);
    w32(f + 4, counter);
    w32(f + 8, size);
    w32(f + 12, SAVE4_SIGNATURE);
    f[16] = id;
    f[17] = 0;
    w16(f + 18, save4_crc16(block, size - L->footer));
}

static void build_copy(uint8_t *copy, save4_game game, uint32_t counter, uint32_t money)
{
    const struct synth_layout *L = lay(game);
    uint8_t *gen = copy, *sto = copy + L->storage_off;
    noise_state = 0xC0FFEEu; /* same noise in both copies */
    for (uint32_t i = 0; i < L->storage_off + L->storage; i++)
        copy[i] = noise();

    /* PlayerSave / PLAYERDATA */
    uint8_t *pl = gen + L->player;
    memset(pl, 0, 0x2C);
    put_name(pl + 0x04, 8, SYNTH_TRAINER_NAME);
    w16(pl + 0x14, SYNTH_TID);
    w16(pl + 0x16, SYNTH_SID);
    w32(pl + 0x18, money);
    pl[0x1C] = 0;          /* male */
    pl[0x1D] = 2;          /* English */
    pl[0x1E] = SYNTH_BADGES;
    pl[0x20] = game == SAVE4_GAME_HG ? 7 : game == SAVE4_GAME_SS ? 8 : 0; /* PlayerProfile.version */
    if (L->hgss)
        pl[0x23] = SYNTH_KANTO_BADGES; /* PlayerProfile.kantoBadges */
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
    if (L->hgss)
        make_johto(&mon, game, 4 /* Poké Ball */);
    pkm4_encrypt(&mon, party + 8, PKM4_PARTY_SIZE);
    synth_make_mon(&mon, 1, 10, 0x9ABCDEF0u, "Bulby", 22 /* Vine Whip */, 1);
    if (L->hgss)
        make_johto(&mon, game, 493 /* Level Ball */);
    pkm4_encrypt(&mon, party + 8 + PKM4_PARTY_SIZE, PKM4_PARTY_SIZE);
    for (int i = 2; i < 6; i++) {
        memset(&mon, 0, sizeof(mon));
        mon.party = true;
        pkm4_encrypt(&mon, party + 8 + i * PKM4_PARTY_SIZE, PKM4_PARTY_SIZE);
    }

    /* Bag: Potion x5 (items pocket), Poké Ball x10 (balls pocket) */
    uint8_t *bag = gen + L->bag;
    memset(bag, 0, L->bag_size);
    w16(bag + 0, 17);
    w16(bag + 2, 5);
    uint32_t balls = 0;
    for (int i = 0; i < 6; i++)
        balls += L->pockets[i] * 4u;
    w16(bag + balls, 4);
    w16(bag + balls + 2, 10);

    /* VarsFlags: zero, then a couple of known values */
    uint8_t *vars = gen + L->vars;
    memset(vars, 0, L->nvars * 2 + 2912 / 8);
    w16(vars + 0x10 * 2, 0x1234); /* var 0x4010 */
    vars[L->nvars * 2 + 10 / 8] |= 1 << (10 % 8); /* flag 10 */

    /* Pokédex: magic + seen/caught bitmaps (HG/SS: the whole 0x340-byte
     * struct, dexEnabled / nationalDex clear as before the Pokédex is given) */
    uint8_t *dex = gen + L->dex;
    memset(dex, 0, L->hgss ? 0x340 : 4 + 64 * 4);
    w32(dex, 0xBEEFCAFEu);
    unsigned b = 387 - 1;
    dex[4 + b / 8] |= (uint8_t)(1 << (b % 8));
    dex[0x44 + b / 8] |= (uint8_t)(1 << (b % 8));
    b = 1 - 1;
    dex[0x44 + b / 8] |= (uint8_t)(1 << (b % 8));

    /* MysteryGift: empty, as a new game leaves it (Pt and HG/SS keep its
     * CRC). HG/SS SysInfo.mysteryGiftActive (0x48) off. */
    uint8_t *mg = gen + L->mystery;
    memset(mg, 0, L->mystery_body);
    if (L->mystery_crc)
        w16(mg + L->mystery_crc, save4_crc16(mg, L->mystery_crc));
    if (L->hgss)
        gen[0x48] = 0;

    /* HG/SS: LocalFieldData.currentPosition (Location: map, warp, x, z,
     * direction) in New Bark (map 60), and the saved map objects CONTINUE
     * restores (SavedMapObjectList at 0x2348, measured in the game's own
     * saves; SavedMapObject 0x50 bytes, pokeheartgold include/map_object.h):
     * the player (objId 0xFF), the walking Pokemon (0xFD) and one person of
     * the map (objId 3), each active (flags bit 0). */
    if (L->hgss) {
        uint8_t *loc = gen + 0x1234;
        w32(loc, SYNTH_HGSS_MAP);
        w32(loc + 4, 0xFFFFFFFFu);
        w32(loc + 8, SYNTH_HGSS_X);
        w32(loc + 12, SYNTH_HGSS_Z);
        w32(loc + 16, 1);
        uint8_t *mo = gen + 0x2348;
        memset(mo, 0, 64 * 0x50);
        static const uint8_t ids[3] = {0xFF, 0xFD, 3};
        for (int i = 0; i < 3; i++) {
            uint8_t *o = mo + i * 0x50;
            w32(o, 0x2000E431u);
            o[8] = ids[i];
            w16(o + 0x10, i == 2 ? SYNTH_HGSS_MAP : 1);
            w16(o + 0x20, (uint16_t)(SYNTH_HGSS_X + i));
            w16(o + 0x26, (uint16_t)(SYNTH_HGSS_X + i));
            w16(o + 0x24, SYNTH_HGSS_Z);
            w16(o + 0x2A, SYNTH_HGSS_Z);
            w16(o + 0x28, 2);
            w32(o + 0x2C, 2u << 15);
        }
        /* MigratedPokemon (Pal Park, pokeheartgold include/palPark_migration.h)
         * at 0xB3C0, measured after a migration from Emerald: Pokemon[6],
         * MigratedPokemon_Init's zeroed ones but slot 3. */
        uint8_t *mig = gen + 0xB3C0;
        for (int i = 0; i < 6; i++) {
            if (i == 2)
                synth_make_mon(&mon, SYNTH_HGSS_MIGRATED, 10, 0x2468ACE0u, "TREECKO", 1 /* Pound */, 1);
            else {
                memset(&mon, 0, sizeof(mon));
                mon.party = true;
            }
            pkm4_encrypt(&mon, mig + i * PKM4_PARTY_SIZE, PKM4_PARTY_SIZE);
        }
    }

    /* PCBoxes / PCStorage: current box 0, every slot BoxPokemon_Init'd */
    memset(sto, 0, L->pc_size);
    for (int box = 0; box < SAVE4_BOX_COUNT; box++)
        for (int i = 0; i < SAVE4_BOX_SLOTS; i++) {
            memset(&mon, 0, sizeof(mon));
            pkm4_encrypt(&mon, sto + L->box_mons + box * L->box_stride + i * PKM4_BOX_SIZE, PKM4_BOX_SIZE);
        }
    synth_make_mon(&mon, SYNTH_BOX_SPECIES, 7, 0x0BADF00Du, "STARLY", 33, 0);
    pkm4_encrypt(&mon, sto + L->box_mons, PKM4_BOX_SIZE);
    uint8_t *names = sto + L->box_names;
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

    footer(L, gen, L->general, 0, counter);
    footer(L, sto, L->storage, 1, counter);
}

void synth_save_build(uint8_t *img, save4_game game)
{
    memset(img, 0xFF, SAVE4_IMAGE_SIZE);
    build_copy(img, game, SYNTH_COUNTER_OLD, SYNTH_MONEY_OLD);
    build_copy(img + SAVE4_COPY_SIZE, game, SYNTH_COUNTER_NEW, SYNTH_MONEY_NEW);
}
