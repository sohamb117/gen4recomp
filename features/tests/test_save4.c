/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdint.h>
#include <stdlib.h>

#include "save4/save4.h"
#include "synth_save.h"
#include "testutil.h"

static uint32_t r32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Bytes in [a, b) differ between two images? */
static size_t count_diff(const uint8_t *x, const uint8_t *y, size_t a, size_t b)
{
    size_t n = 0;
    for (size_t i = a; i < b; i++)
        n += x[i] != y[i];
    return n;
}

static void test_game(save4_game game)
{
    uint8_t *img = malloc(SAVE4_IMAGE_SIZE);
    synth_save_build(img, game);
    const uint32_t gsize = synth_general_size(game), ssize = synth_storage_size(game);
    const uint32_t soff = synth_storage_offset(game), fsz = synth_footer_size(game);
    const bool hgss = save4_game_is_hgss(game);

    /* CRC-16-CCITT check value (init 0xFFFF, poly 0x1021): "123456789" -> 0x29B1 */
    CHECK_EQ_INT(save4_crc16("123456789", 9), 0x29B1);

    save4 s;
    CHECK(save4_load(&s, img, SAVE4_IMAGE_SIZE) == SAVE4_OK);
    CHECK(s.game == game);
    CHECK(s.load_result == SAVE4_LOAD_OK);
    CHECK_EQ_INT(s.blocks[0].active, 1); /* backup copy is newer */
    CHECK_EQ_INT(s.blocks[1].active, 1);
    CHECK(s.blocks[0].valid[0] && s.blocks[0].valid[1]);
    CHECK_EQ_INT(s.blocks[0].size, gsize);
    CHECK_EQ_INT(s.blocks[1].size, ssize);
    CHECK_EQ_INT(s.blocks[1].offset, soff);

    save4_trainer t;
    CHECK(save4_get_trainer(&s, &t) == SAVE4_OK);
    CHECK_EQ_STR(t.name, SYNTH_TRAINER_NAME);
    CHECK_EQ_INT(t.tid, SYNTH_TID);
    CHECK_EQ_INT(t.sid, SYNTH_SID);
    CHECK_EQ_INT(t.money, SYNTH_MONEY_NEW);
    CHECK_EQ_INT(t.badges, SYNTH_BADGES);
    CHECK_EQ_INT(t.coins, 100);
    CHECK_EQ_INT(t.play_hours, 12);
    CHECK_EQ_INT(t.play_minutes, 34);
    CHECK_EQ_INT(t.play_seconds, 56);
    if (hgss) {
        CHECK_EQ_INT(t.kanto_badges, SYNTH_KANTO_BADGES);
        CHECK_EQ_INT(t.game_code, game == SAVE4_GAME_HG ? 7 : 8);
        CHECK_EQ_INT(save4_num_vars(&s), SAVE4_HGSS_NUM_VARS);
        CHECK_EQ_INT(save4_pocket_capacity(&s, SAVE4_POCKET_TMHM), 101);
        CHECK_EQ_INT(save4_pocket_capacity(&s, SAVE4_POCKET_BALLS), 24);
        save4_poketch pt;
        CHECK(save4_get_poketch(&s, &pt) == SAVE4_ERR_UNSUPPORTED);
    } else {
        CHECK_EQ_INT(t.kanto_badges, 0);
        CHECK_EQ_INT(save4_num_vars(&s), SAVE4_NUM_VARS);
        CHECK_EQ_INT(save4_pocket_capacity(&s, SAVE4_POCKET_TMHM), 100);
        CHECK_EQ_INT(save4_pocket_capacity(&s, SAVE4_POCKET_BALLS), 15);
    }
    CHECK_EQ_INT(save4_num_flags(&s), SAVE4_NUM_FLAGS);
    CHECK_EQ_INT(save4_pocket_capacity(NULL, SAVE4_POCKET_ITEMS), 0);

    CHECK_EQ_INT(save4_party_count(&s), 2);
    pkm4 p;
    pkm4_info info;
    CHECK(save4_get_party(&s, 0, &p) == SAVE4_OK);
    pkm4_info_get(&p, &info);
    CHECK_EQ_INT(info.species, 387);
    CHECK_EQ_INT(info.level, 5);
    CHECK_EQ_STR(info.nickname, "TURTWIG");
    CHECK(save4_get_party(&s, 1, &p) == SAVE4_OK);
    pkm4_info_get(&p, &info);
    CHECK_EQ_INT(info.species, 1);
    CHECK_EQ_STR(info.nickname, "Bulby");
    /* HG/SS: the Level Ball in HGSS_Pokeball (Poke Ball in the D/P/Pt byte). */
    CHECK_EQ_INT(info.ball, hgss ? SYNTH_HGSS_BULBY_BALL : 4);
    CHECK_EQ_INT(p.data[0x83], 4);
    CHECK(save4_get_party(&s, 6, &p) == SAVE4_ERR_RANGE);

    CHECK(save4_get_box_mon(&s, 0, 0, &p) == SAVE4_OK);
    pkm4_info_get(&p, &info);
    CHECK_EQ_INT(info.species, SYNTH_BOX_SPECIES);
    CHECK(!info.has_party_data);
    CHECK(save4_get_box_mon(&s, 17, 29, &p) == SAVE4_OK && pkm4_is_empty(&p));
    char name[64];
    CHECK(save4_get_box_name(&s, 0, name, sizeof(name)) == SAVE4_OK);
    CHECK_EQ_STR(name, "Box 1");
    CHECK(save4_get_box_name(&s, 17, name, sizeof(name)) == SAVE4_OK);
    CHECK_EQ_STR(name, "Box 18");

    uint16_t item, qty;
    CHECK(save4_get_bag_slot(&s, SAVE4_POCKET_ITEMS, 0, &item, &qty) == SAVE4_OK);
    CHECK(item == 17 && qty == 5);
    CHECK(save4_get_bag_slot(&s, SAVE4_POCKET_BALLS, 0, &item, &qty) == SAVE4_OK);
    CHECK(item == 4 && qty == 10);

    bool seen, caught, fv;
    CHECK(save4_dex_get(&s, 387, &seen, &caught) == SAVE4_OK && seen && caught);
    CHECK(save4_dex_get(&s, 1, &seen, &caught) == SAVE4_OK && seen && !caught);
    CHECK(save4_dex_get(&s, 2, &seen, &caught) == SAVE4_OK && !seen && !caught);
    CHECK(save4_flag_get(&s, 10, &fv) == SAVE4_OK && fv);
    CHECK(save4_flag_get(&s, 11, &fv) == SAVE4_OK && !fv);
    uint16_t var;
    CHECK(save4_var_get(&s, 0x4010, &var) == SAVE4_OK && var == 0x1234);
    CHECK(save4_var_get(&s, 0x3FFF, &var) == SAVE4_ERR_RANGE);
    /* The last saved var: HG/SS NUM_VARS 0x170, D/P/Pt 288. */
    CHECK(save4_var_get(&s, (uint16_t)(SAVE4_VARS_START + SAVE4_NUM_VARS), &var) ==
          (hgss ? SAVE4_OK : SAVE4_ERR_RANGE));
    CHECK(save4_var_get(&s, (uint16_t)(SAVE4_VARS_START + SAVE4_HGSS_NUM_VARS - 1), &var) ==
          (hgss ? SAVE4_OK : SAVE4_ERR_RANGE));
    CHECK(save4_var_get(&s, (uint16_t)(SAVE4_VARS_START + SAVE4_HGSS_NUM_VARS), &var) == SAVE4_ERR_RANGE);
    CHECK(save4_flag_get(&s, SAVE4_NUM_FLAGS - 1, &fv) == SAVE4_OK);
    CHECK(save4_flag_get(&s, SAVE4_NUM_FLAGS, &fv) == SAVE4_ERR_RANGE);

    /* ---- edits on a snapshot (undo = keep the original) */
    save4 e;
    CHECK(save4_clone(&s, &e) == SAVE4_OK);
    CHECK(save4_set_money(&e, 123456) == SAVE4_OK);
    CHECK(save4_set_money(&e, 1000000) == SAVE4_ERR_RANGE);
    CHECK(save4_set_trainer_name(&e, "Dawn") == SAVE4_OK);
    CHECK(save4_set_trainer_name(&e, "TooLongName") == SAVE4_ERR_RANGE);
    CHECK(save4_set_badges(&e, 0xFF) == SAVE4_OK);
    CHECK(save4_set_kanto_badges(&e, 0x81) == (hgss ? SAVE4_OK : SAVE4_ERR_UNSUPPORTED));
    CHECK(save4_set_bag_slot(&e, SAVE4_POCKET_BALLS, 20, 6, 2) == (hgss ? SAVE4_OK : SAVE4_ERR_RANGE));
    if (hgss)
        CHECK(save4_var_set(&e, (uint16_t)(SAVE4_VARS_START + 300), 99) == SAVE4_OK);
    CHECK(save4_set_bag_slot(&e, SAVE4_POCKET_ITEMS, 1, 50, 3) == SAVE4_OK); /* Rare Candy */
    CHECK(save4_set_bag_slot(&e, SAVE4_POCKET_ITEMS, 1, 50, 1000) == SAVE4_ERR_RANGE);
    CHECK(save4_dex_set(&e, 2, false, true) == SAVE4_OK);
    CHECK(save4_flag_set(&e, 10, false) == SAVE4_OK);
    CHECK(save4_var_set(&e, 0x4011, 7) == SAVE4_OK);
    CHECK(save4_set_box_name(&e, 2, "Favs") == SAVE4_OK);
    CHECK(save4_get_party(&e, 0, &p) == SAVE4_OK);
    pkm4_set_held_item(&p, 234);
    pkm4_set_exp(&p, 1000);
    CHECK(save4_set_party(&e, 0, &p) == SAVE4_OK);
    pkm4 nm;
    synth_make_mon(&nm, 25, 12, 0xCAFEBABEu, "Sparky", 84, 0);
    CHECK(save4_set_box_mon(&e, 4, 7, &nm) == SAVE4_OK);

    /* Original untouched */
    CHECK(save4_get_trainer(&s, &t) == SAVE4_OK && t.money == SYNTH_MONEY_NEW);

    /* Reload the edited bytes: everything validates and reads back. */
    size_t elen;
    const uint8_t *eimg = save4_image(&e, &elen);
    CHECK_EQ_INT(elen, SAVE4_IMAGE_SIZE);
    save4 r;
    CHECK(save4_load(&r, eimg, elen) == SAVE4_OK);
    CHECK(r.load_result == SAVE4_LOAD_OK);
    CHECK_EQ_INT(r.blocks[0].active, 1);
    CHECK(save4_get_trainer(&r, &t) == SAVE4_OK);
    CHECK_EQ_INT(t.money, 123456);
    CHECK_EQ_STR(t.name, "Dawn");
    CHECK_EQ_INT(t.badges, 0xFF);
    CHECK_EQ_INT(t.kanto_badges, hgss ? 0x81 : 0);
    if (hgss) {
        CHECK(save4_get_bag_slot(&r, SAVE4_POCKET_BALLS, 20, &item, &qty) == SAVE4_OK && item == 6 && qty == 2);
        CHECK(save4_var_get(&r, (uint16_t)(SAVE4_VARS_START + 300), &var) == SAVE4_OK && var == 99);
        /* Kanto badges sit in PlayerProfile.kantoBadges (PLAYERDATA 0x60 + 4 + 0x1F). */
        CHECK_EQ_INT(eimg[SAVE4_COPY_SIZE + 0x60 + 0x23], 0x81);
        /* The box edit marked box 4 modified (PCStorage.boxModifiedFlag 0x12004). */
        CHECK_EQ_INT(r32(eimg + SAVE4_COPY_SIZE + soff + 0x12004), 1u << 4);
        CHECK_EQ_INT(r32(img + SAVE4_COPY_SIZE + soff + 0x12004), 0);
    }
    CHECK(save4_get_bag_slot(&r, SAVE4_POCKET_ITEMS, 1, &item, &qty) == SAVE4_OK && item == 50 && qty == 3);
    CHECK(save4_dex_get(&r, 2, &seen, &caught) == SAVE4_OK && seen && caught);
    CHECK(save4_flag_get(&r, 10, &fv) == SAVE4_OK && !fv);
    CHECK(save4_var_get(&r, 0x4011, &var) == SAVE4_OK && var == 7);
    CHECK(save4_get_box_name(&r, 2, name, sizeof(name)) == SAVE4_OK);
    CHECK_EQ_STR(name, "Favs");
    CHECK(save4_get_party(&r, 0, &p) == SAVE4_OK);
    pkm4_info_get(&p, &info);
    CHECK(info.held_item == 234 && info.exp == 1000 && info.species == 387 && info.checksum_ok);
    CHECK(save4_get_box_mon(&r, 4, 7, &p) == SAVE4_OK);
    pkm4_info_get(&p, &info);
    CHECK(info.species == 25 && info.checksum_ok);
    CHECK_EQ_STR(info.nickname, "Sparky");

    /* Only the active copy changed; the stale copy and the tail are byte-identical. */
    CHECK_EQ_INT(count_diff(img, eimg, 0, SAVE4_COPY_SIZE), 0);
    CHECK_EQ_INT(count_diff(img, eimg, SAVE4_COPY_SIZE + soff + ssize, SAVE4_IMAGE_SIZE), 0);
    /* HG/SS: the bytes between the blocks too. */
    CHECK_EQ_INT(count_diff(img, eimg, SAVE4_COPY_SIZE + gsize, SAVE4_COPY_SIZE + soff), 0);
    /* Footer counters / signature untouched; only the CRC moved. */
    const uint8_t *fo = img + SAVE4_COPY_SIZE + gsize - fsz;
    const uint8_t *fe = eimg + SAVE4_COPY_SIZE + gsize - fsz;
    CHECK(memcmp(fo, fe, fsz - 2) == 0);
    /* Unknown bytes (noise) preserved: Poketch / field state between the end
     * of VarsFlags and the Pokédex (PKHeX PoketchStart Pt 0x1160, DP 0x114C;
     * HG/SS LocalFieldData 0x1234..0x12B8). */
    {
        uint32_t a = SAVE4_COPY_SIZE + (hgss ? 0x1234 : game == SAVE4_GAME_PT ? 0x1160 : 0x114C);
        uint32_t b = SAVE4_COPY_SIZE + (hgss ? 0x12B8 : game == SAVE4_GAME_PT ? 0x1328 : 0x12DC);
        CHECK_EQ_INT(count_diff(img, eimg, a, b), 0);
    }
    /* A money edit changes exactly the money bytes + CRC. */
    {
        save4 m;
        CHECK(save4_clone(&s, &m) == SAVE4_OK);
        CHECK(save4_set_money(&m, SYNTH_MONEY_NEW + 1) == SAVE4_OK);
        size_t ml;
        const uint8_t *mi = save4_image(&m, &ml);
        CHECK_EQ_INT(count_diff(img, mi, 0, SAVE4_IMAGE_SIZE), 1 + count_diff(img, mi, SAVE4_COPY_SIZE + gsize - 2, SAVE4_COPY_SIZE + gsize));
        save4_free(&m);
    }

    /* ---- copy selection & checksum failure */
    uint8_t *bad = malloc(SAVE4_IMAGE_SIZE);
    /* corrupt the newer (backup) general block -> game falls back to primary */
    memcpy(bad, img, SAVE4_IMAGE_SIZE);
    bad[SAVE4_COPY_SIZE + 0x200] ^= 0xFF;
    save4 c;
    CHECK(save4_load(&c, bad, SAVE4_IMAGE_SIZE) == SAVE4_OK);
    CHECK(c.load_result == SAVE4_LOAD_RECOVERED);
    CHECK(!c.blocks[0].valid[1] && c.blocks[0].valid[0]);
    CHECK_EQ_INT(c.blocks[0].active, 0);
    CHECK(save4_get_trainer(&c, &t) == SAVE4_OK && t.money == SYNTH_MONEY_OLD);
    save4_free(&c);

    /* corrupt both general copies -> checksum error */
    bad[0x200] ^= 0xFF;
    CHECK(save4_load(&c, bad, SAVE4_IMAGE_SIZE) == SAVE4_ERR_CHECKSUM);
    CHECK(!c.blocks[0].valid[0] && !c.blocks[0].valid[1]);
    CHECK(c.blocks[1].valid[0] && c.blocks[1].valid[1]);
    save4_free(&c);

    /* corrupt both storage copies -> checksum error */
    memcpy(bad, img, SAVE4_IMAGE_SIZE);
    bad[soff + 0x100] ^= 1;
    bad[SAVE4_COPY_SIZE + soff + 0x100] ^= 1;
    CHECK(save4_load(&c, bad, SAVE4_IMAGE_SIZE) == SAVE4_ERR_CHECKSUM);
    save4_free(&c);

    /* a stale footer CRC is caught after a raw byte edit, and revalidate agrees */
    save4 v;
    CHECK(save4_clone(&s, &v) == SAVE4_OK);
    v.img[SAVE4_COPY_SIZE + 0x300] ^= 1;
    CHECK(save4_revalidate(&v) == SAVE4_OK);
    CHECK(v.load_result == SAVE4_LOAD_RECOVERED && v.blocks[0].active == 0);
    save4_free(&v);

    /* counter wrap rule: 0xFFFFFFFF vs 0 -> 0 is newer */
    memcpy(bad, img, SAVE4_IMAGE_SIZE);
    {
        uint8_t *f0 = bad + gsize - fsz;
        uint8_t *f1 = bad + SAVE4_COPY_SIZE + gsize - fsz;
        uint8_t *g0 = bad + soff + ssize - fsz;
        uint8_t *g1 = bad + SAVE4_COPY_SIZE + soff + ssize - fsz;
        const size_t counters = hgss ? 4 : 8; /* HG/SS: count only */
        memset(f0, 0, counters); /* primary: counter 0 */
        memset(g0, 0, counters);
        memset(f1, 0xFF, counters); /* backup: counter 0xFFFFFFFF */
        memset(g1, 0xFF, counters);
        CHECK(save4_load(&c, bad, SAVE4_IMAGE_SIZE) == SAVE4_OK);
        CHECK_EQ_INT(c.blocks[0].active, 0);
        CHECK_EQ_INT(r32(f0), 0);
        save4_free(&c);
    }

    /* blank / short images */
    memset(bad, 0xFF, SAVE4_IMAGE_SIZE);
    CHECK(save4_load(&c, bad, SAVE4_IMAGE_SIZE) == SAVE4_ERR_EMPTY);
    CHECK(save4_load(&c, bad, 1000) == SAVE4_ERR_SIZE);
    bad[5] = 0x42;
    CHECK(save4_load(&c, bad, SAVE4_IMAGE_SIZE) == SAVE4_ERR_UNKNOWN_GAME);

    /* trailing emulator data beyond 512 KiB is preserved */
    uint8_t *big = malloc(SAVE4_IMAGE_SIZE + 122);
    memcpy(big, img, SAVE4_IMAGE_SIZE);
    for (int i = 0; i < 122; i++)
        big[SAVE4_IMAGE_SIZE + i] = (uint8_t)i;
    CHECK(save4_load(&c, big, SAVE4_IMAGE_SIZE + 122) == SAVE4_OK);
    CHECK(save4_set_money(&c, 1) == SAVE4_OK);
    size_t bl;
    const uint8_t *bi = save4_image(&c, &bl);
    CHECK(bl == SAVE4_IMAGE_SIZE + 122 && memcmp(bi + SAVE4_IMAGE_SIZE, big + SAVE4_IMAGE_SIZE, 122) == 0);
    save4_free(&c);
    free(big);

    free(bad);
    save4_free(&r);
    save4_free(&e);
    save4_free(&s);
    free(img);
}

/* Mystery Gift on the synthetic Platinum save: the entry checksum the game
 * verifies (CRC-16 over the body, stored after it), card + gift placement,
 * the received bit, removal and the unlock switches. */
static void test_mystery(void)
{
    uint8_t *img = malloc(SAVE4_IMAGE_SIZE);
    synth_save_build(img, SAVE4_GAME_PT);
    save4 s;
    CHECK(save4_load(&s, img, SAVE4_IMAGE_SIZE) == SAVE4_OK);

    save4_card_spec spec = {SAVE4_MG_MEMBER_CARD, 42, 0, {491, 0, 0}, 3500, "Member Card", "Line one\nLine two"};
    uint8_t card[SAVE4_WONDERCARD_SIZE];
    CHECK(save4_mg_build_card(&spec, card) == SAVE4_OK);
    const char *why = NULL;
    CHECK(save4_mg_validate(card, sizeof card, &why) == SAVE4_OK);
    CHECK(save4_mg_validate(card, 100, &why) == SAVE4_ERR_SIZE && why);
    uint8_t bad[SAVE4_PGT_SIZE] = {0};
    CHECK(save4_mg_validate(bad, sizeof bad, &why) == SAVE4_ERR_ARG);
    /* Title encoding: the game's charset, terminated. */
    CHECK(card[0] == SAVE4_MG_MEMBER_CARD && card[0x104 + 22] == 0xFF && card[0x104 + 23] == 0xFF);
    CHECK_EQ_INT(card[0x104 + 0x4E], 0x0C); /* hasWonderCard | savePgt */

    CHECK(save4_mg_add(&s, card, sizeof card) == SAVE4_OK);
    uint8_t back[SAVE4_WONDERCARD_SIZE];
    bool used = false;
    CHECK(save4_mg_get_card(&s, 0, back, &used) == SAVE4_OK && used);
    CHECK(!memcmp(back, card, sizeof card));
    int pgts = 0;
    CHECK(save4_mg_pgt_count(&s, &pgts) == SAVE4_OK && pgts == 1);

    size_t len;
    const uint8_t *im = save4_image(&s, &len);
    const uint8_t *mg = im + save4_block_base(&s, SAVE4_BLOCK_GENERAL) + 0xB4C0;
    CHECK_EQ_INT(mg[42 / 8] >> (42 % 8) & 1, 1);                  /* received bit */
    CHECK_EQ_INT(mg[0x100] | mg[0x101] << 8, SAVE4_MG_MEMBER_CARD); /* PGT 0 */
    CHECK_EQ_INT((mg[0x102] | mg[0x103] << 8) & 3, 0);             /* linked to card 0 */
    CHECK_EQ_INT(save4_crc16(mg, 0x132C), mg[0x132C] | mg[0x132D] << 8);
    CHECK(save4_revalidate(&s) == SAVE4_OK);

    /* A gift-only .pgt goes to a PGT slot linked to "no card" (3). */
    CHECK(save4_mg_add(&s, card, SAVE4_PGT_SIZE) == SAVE4_OK);
    CHECK(save4_mg_pgt_count(&s, &pgts) == SAVE4_OK && pgts == 2);
    CHECK_EQ_INT((mg[0x206] | mg[0x207] << 8) & 3, 3);

    /* Cards fill three slots, then refuse. */
    spec.id = 43;
    save4_mg_build_card(&spec, card);
    CHECK(save4_mg_add(&s, card, sizeof card) == SAVE4_OK);
    spec.id = 44;
    save4_mg_build_card(&spec, card);
    CHECK(save4_mg_add(&s, card, sizeof card) == SAVE4_OK);
    CHECK(save4_mg_add(&s, card, sizeof card) == SAVE4_ERR_NOSPACE);

    CHECK(save4_mg_remove_card(&s, 0) == SAVE4_OK);
    CHECK(save4_mg_get_card(&s, 0, back, &used) == SAVE4_OK && !used);
    CHECK_EQ_INT(mg[42 / 8] >> (42 % 8) & 1, 0);
    CHECK(save4_mg_pgt_count(&s, &pgts) == SAVE4_OK && pgts == 3); /* its PGT went with it */
    CHECK_EQ_INT(save4_crc16(mg, 0x132C), mg[0x132C] | mg[0x132D] << 8);

    bool on = true;
    CHECK(save4_mg_set_unlocked(&s, false) == SAVE4_OK);
    CHECK(save4_mg_get_unlocked(&s, &on) == SAVE4_OK && !on);
    CHECK(save4_mg_set_unlocked(&s, true) == SAVE4_OK);
    CHECK(save4_mg_get_unlocked(&s, &on) == SAVE4_OK && on);
    CHECK_EQ_INT(mg[255] >> 7, 1);
    CHECK_EQ_INT(im[save4_block_base(&s, SAVE4_BLOCK_GENERAL) + 0x48], 1);
    CHECK(save4_dex_set_obtained(&s, true) == SAVE4_OK);
    CHECK(save4_dex_get_obtained(&s, &on) == SAVE4_OK && on);
    CHECK(save4_revalidate(&s) == SAVE4_OK);

    /* National Dex: Pokedex.nationalDexObtained (dex + 0x31B) and
     * TrainerInfo.hasNationalDex (story flags bit 1), as the game awards it. */
    const uint8_t *gen = im + save4_block_base(&s, SAVE4_BLOCK_GENERAL);
    CHECK(save4_dex_set_national(&s, false) == SAVE4_OK);
    CHECK(save4_dex_get_national(&s, &on) == SAVE4_OK && !on);
    CHECK_EQ_INT((gen[0x64 + 0x21] >> 1) & 1, 0);
    CHECK(save4_dex_set_national(&s, true) == SAVE4_OK);
    CHECK(save4_dex_get_national(&s, &on) == SAVE4_OK && on);
    CHECK_EQ_INT(gen[0x1328 + 0x31B], 1);
    CHECK_EQ_INT(gen[0x1328 + 0x31A], 1); /* pokedexObtained untouched */
    CHECK_EQ_INT((gen[0x64 + 0x21] >> 1) & 1, 1);
    save4_trainer tr;
    CHECK(save4_get_trainer(&s, &tr) == SAVE4_OK && tr.has_national_dex);
    CHECK(save4_revalidate(&s) == SAVE4_OK);
    CHECK(save4_dex_set_national(&s, false) == SAVE4_OK);
    CHECK_EQ_INT(gen[0x1328 + 0x31B], 0);
    CHECK_EQ_INT((gen[0x64 + 0x21] >> 1) & 1, 0);
    CHECK(save4_revalidate(&s) == SAVE4_OK);
    save4_free(&s);
    free(img);
}

/* Mystery Gift / Pokédex on the synthetic D/P save: the D/P MysteryGift
 * shape (slot-used markers, PGT link = card slot + 1, no entry CRC) at
 * 0xA6D0, SaveSysInfo 0x48, Pokedex 0x12DC + 0x138 / 0x139, and the gift
 * types D/P cannot deliver. */
static void test_mystery_dp(void)
{
    CHECK(save4_mg_type_supported(SAVE4_GAME_DP, SAVE4_MG_POKEMON));
    CHECK(save4_mg_type_supported(SAVE4_GAME_DP, SAVE4_MG_POKETCH_APP));
    CHECK(!save4_mg_type_supported(SAVE4_GAME_DP, SAVE4_MG_SECRET_KEY));
    CHECK(!save4_mg_type_supported(SAVE4_GAME_DP, SAVE4_MG_UNKNOWN));
    CHECK(save4_mg_type_supported(SAVE4_GAME_PT, SAVE4_MG_SECRET_KEY));
    CHECK(save4_mg_type_supported(SAVE4_GAME_PT, SAVE4_MG_UNKNOWN));
    CHECK(!save4_mg_type_supported(SAVE4_GAME_PT, 0));
    CHECK(!save4_mg_type_supported(SAVE4_GAME_PT, SAVE4_MG_TYPE_MAX));
    CHECK(!save4_mg_type_supported(SAVE4_GAME_UNKNOWN, SAVE4_MG_ITEM));

    uint8_t *img = malloc(SAVE4_IMAGE_SIZE);
    synth_save_build(img, SAVE4_GAME_DP);
    save4 s;
    CHECK(save4_load(&s, img, SAVE4_IMAGE_SIZE) == SAVE4_OK);
    CHECK(s.game == SAVE4_GAME_DP);
    size_t len;
    const uint8_t *im = save4_image(&s, &len);
    const uint8_t *gen = im + save4_block_base(&s, SAVE4_BLOCK_GENERAL);
    const uint8_t *mg = gen + 0xA6D0;
    const uint32_t mg_end = 0xA6D0 + 0x1358; /* entry 32 starts here */
    uint8_t *ref = malloc(SAVE4_IMAGE_SIZE);
    memcpy(ref, im, SAVE4_IMAGE_SIZE);
    size_t gbase = save4_block_base(&s, SAVE4_BLOCK_GENERAL);

    /* Main menu switches and the Pokédex flags. */
    bool on = true;
    CHECK(save4_mg_get_unlocked(&s, &on) == SAVE4_OK && !on);
    CHECK(save4_mg_set_unlocked(&s, true) == SAVE4_OK);
    CHECK(save4_mg_get_unlocked(&s, &on) == SAVE4_OK && on);
    CHECK_EQ_INT(gen[0x48], 1);    /* SaveSysInfo.mysteryGiftActive */
    CHECK_EQ_INT(mg[255] >> 7, 1); /* received bit 2047 */
    CHECK(save4_dex_set_obtained(&s, true) == SAVE4_OK);
    CHECK(save4_dex_get_obtained(&s, &on) == SAVE4_OK && on);
    CHECK_EQ_INT(gen[0x12DC + 0x138], 1);
    CHECK(save4_dex_set_national(&s, true) == SAVE4_OK);
    CHECK(save4_dex_get_national(&s, &on) == SAVE4_OK && on);
    CHECK_EQ_INT(gen[0x12DC + 0x139], 1);          /* Pokedex.unlockedNationalDex */
    CHECK_EQ_INT((gen[0x60 + 0x21] >> 1) & 1, 1); /* PlayerProfile.nationalDex */
    save4_trainer tr;
    CHECK(save4_get_trainer(&s, &tr) == SAVE4_OK && tr.has_national_dex);

    /* A card with its PGT. */
    save4_card_spec spec = {SAVE4_MG_MEMBER_CARD, 42, 0, {491, 0, 0}, 3500, "Member Card", "Darkrai"};
    uint8_t card[SAVE4_WONDERCARD_SIZE], back[SAVE4_WONDERCARD_SIZE];
    CHECK(save4_mg_build_card(&spec, card) == SAVE4_OK);
    CHECK(save4_mg_add(&s, card, sizeof card) == SAVE4_OK);
    bool used = false;
    CHECK(save4_mg_get_card(&s, 0, back, &used) == SAVE4_OK && used);
    CHECK(!memcmp(back, card, sizeof card));
    CHECK(!memcmp(mg + 0x94C, card, sizeof card));         /* wonderCards[0] */
    CHECK_EQ_INT(r32(mg + 0x120), 0xEDB88320u);           /* cardUsed[0] */
    CHECK_EQ_INT(r32(mg + 0x100), 0xEDB88320u);           /* pgtUsed[0] */
    CHECK_EQ_INT(mg[0x12C] | mg[0x12D] << 8, SAVE4_MG_MEMBER_CARD); /* pgts[0].type */
    CHECK_EQ_INT(mg[0x12E] & 3, 1);                        /* linked to card slot 0 + 1 */
    CHECK_EQ_INT(mg[42 / 8] >> (42 % 8) & 1, 1);
    int pgts = 0;
    CHECK(save4_mg_pgt_count(&s, &pgts) == SAVE4_OK && pgts == 1);

    /* A gift-only .pgt: PGT slot 1, linked to no card (0). */
    CHECK(save4_mg_add(&s, card, SAVE4_PGT_SIZE) == SAVE4_OK);
    CHECK_EQ_INT(r32(mg + 0x104), 0xEDB88320u);
    CHECK_EQ_INT(mg[0x12C + 0x104 + 2] & 3, 0);
    CHECK(save4_mg_pgt_count(&s, &pgts) == SAVE4_OK && pgts == 2);

    /* Gift types D/P cannot deliver are refused and change nothing. */
    uint8_t *snap = malloc(SAVE4_IMAGE_SIZE);
    memcpy(snap, im, SAVE4_IMAGE_SIZE);
    save4_card_spec rotom = {SAVE4_MG_SECRET_KEY, 50, 0, {479, 0, 0}, 3500, "Secret Key", "Rotom"};
    CHECK(save4_mg_build_card(&rotom, card) == SAVE4_OK);
    CHECK(save4_mg_add(&s, card, sizeof card) == SAVE4_ERR_UNSUPPORTED);
    CHECK(save4_mg_add(&s, card, SAVE4_PGT_SIZE) == SAVE4_ERR_UNSUPPORTED);
    CHECK(!memcmp(snap, im, SAVE4_IMAGE_SIZE));
    free(snap);

    /* Cards fill three slots, then refuse. */
    spec.id = 43;
    save4_mg_build_card(&spec, card);
    CHECK(save4_mg_add(&s, card, sizeof card) == SAVE4_OK);
    spec.id = 44;
    save4_mg_build_card(&spec, card);
    CHECK(save4_mg_add(&s, card, sizeof card) == SAVE4_OK);
    CHECK(save4_mg_add(&s, card, sizeof card) == SAVE4_ERR_NOSPACE);
    CHECK_EQ_INT(mg[0x12C + 2 * 0x104 + 2] & 3, 2); /* card 1's PGT */
    CHECK_EQ_INT(mg[0x12C + 3 * 0x104 + 2] & 3, 3); /* card 2's PGT */

    /* sub_0202ADC8: the slot marker and the linked PGT go; the received
     * flag and the stored bytes stay. */
    CHECK(save4_mg_remove_card(&s, 0) == SAVE4_OK);
    CHECK(save4_mg_get_card(&s, 0, back, &used) == SAVE4_OK && !used);
    CHECK_EQ_INT(r32(mg + 0x120), 0);
    CHECK_EQ_INT(r32(mg + 0x100), 0);
    CHECK_EQ_INT(mg[0x12E] & 3, 0);
    CHECK_EQ_INT(mg[42 / 8] >> (42 % 8) & 1, 1);
    CHECK(save4_mg_pgt_count(&s, &pgts) == SAVE4_OK && pgts == 3);

    /* Only the MysteryGift entry, SaveSysInfo, PlayerData's story flags and
     * the Pokédex flags changed in the general block (no CRC after the entry). */
    CHECK_EQ_INT(count_diff(ref, im, gbase + mg_end, gbase + 0xC100 - 0x14), 0);
    CHECK_EQ_INT(count_diff(ref, im, gbase + 0x49, gbase + 0x60 + 0x21), 0);
    CHECK_EQ_INT(count_diff(ref, im, gbase + 0x60 + 0x22, gbase + 0x12DC + 0x138), 0);
    CHECK_EQ_INT(count_diff(ref, im, gbase + 0x12DC + 0x13A, gbase + 0xA6D0), 0);
    CHECK(save4_revalidate(&s) == SAVE4_OK);

    /* The edited image loads back with the same state. */
    save4 s2;
    CHECK(save4_load(&s2, im, len) == SAVE4_OK && s2.load_result == SAVE4_LOAD_OK);
    CHECK(save4_mg_get_card(&s2, 1, back, &used) == SAVE4_OK && used);
    CHECK_EQ_INT(back[0x104 + 0x4C], 43);
    CHECK(save4_mg_get_unlocked(&s2, &on) == SAVE4_OK && on);
    CHECK(save4_dex_get_national(&s2, &on) == SAVE4_OK && on);
    save4_free(&s2);

    CHECK(save4_dex_set_national(&s, false) == SAVE4_OK);
    CHECK_EQ_INT(gen[0x12DC + 0x139], 0);
    CHECK_EQ_INT((gen[0x60 + 0x21] >> 1) & 1, 0);
    save4_free(&s);
    free(ref);
    free(img);
}

static void w32le(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

/* HG/SS copy selection (Save_GetSaveFilesStatus, pokeheartgold src/save.c):
 * both blocks always come from the copy whose general and storage counts
 * agree; footers {count, size, magic, slot, crc} (0x10 bytes) with the
 * storage block at 0xF700; the version byte picks HG or SS. */
static void test_hgss_selection(void)
{
    CHECK_EQ_STR(save4_game_name(SAVE4_GAME_HG), "heartgold");
    CHECK_EQ_STR(save4_game_name(SAVE4_GAME_SS), "soulsilver");
    CHECK_EQ_STR(save4_game_name(SAVE4_GAME_PT), "Pt");
    CHECK(save4_game_is_hgss(SAVE4_GAME_SS) && !save4_game_is_hgss(SAVE4_GAME_PT));

    uint8_t *img = malloc(SAVE4_IMAGE_SIZE), *bad = malloc(SAVE4_IMAGE_SIZE);
    synth_save_build(img, SAVE4_GAME_HG);
    const uint32_t gs = 0xF628, so = 0xF700, ss = 0x12310;
    /* The footer as the decomp lays it out. */
    const uint8_t *f = img + SAVE4_COPY_SIZE + gs - 0x10;
    CHECK_EQ_INT(r32(f), SYNTH_COUNTER_NEW);
    CHECK_EQ_INT(r32(f + 4), gs);
    CHECK_EQ_INT(r32(f + 8), SAVE4_SIGNATURE);
    CHECK_EQ_INT(f[12] | f[13] << 8, 0);
    CHECK_EQ_INT(f[14] | f[15] << 8, save4_crc16(img + SAVE4_COPY_SIZE, gs - 0x10));
    save4 c;

    /* Newest counts disagree (storage 6, general 5), the older copy's agree:
     * LOAD_STATUS_SLOT_FAIL with the older copy for both blocks. */
    memcpy(bad, img, SAVE4_IMAGE_SIZE);
    w32le(bad + SAVE4_COPY_SIZE + so + ss - 0x10, 6);
    CHECK(save4_load(&c, bad, SAVE4_IMAGE_SIZE) == SAVE4_OK);
    CHECK(c.load_result == SAVE4_LOAD_RECOVERED);
    CHECK(c.blocks[0].active == 0 && c.blocks[1].active == 0);
    CHECK(c.blocks[0].block_counter[1] == 0 && c.blocks[1].save_counter[1] == 6);
    save4_free(&c);
    /* ... and neither copy agrees: LOAD_STATUS_TOTAL_FAIL. */
    w32le(bad + so + ss - 0x10, 9);
    CHECK(save4_load(&c, bad, SAVE4_IMAGE_SIZE) == SAVE4_ERR_CHECKSUM);
    save4_free(&c);

    /* Newer general and older storage each lost a copy: one valid copy of
     * each, but not the same one -> TOTAL_FAIL (D/P/Pt would mix copies). */
    memcpy(bad, img, SAVE4_IMAGE_SIZE);
    bad[SAVE4_COPY_SIZE + 0x200] ^= 1;
    bad[so + 0x200] ^= 1;
    CHECK(save4_load(&c, bad, SAVE4_IMAGE_SIZE) == SAVE4_ERR_CHECKSUM);
    CHECK(c.blocks[0].valid[0] && !c.blocks[0].valid[1] && !c.blocks[1].valid[0] && c.blocks[1].valid[1]);
    save4_free(&c);

    /* Older storage copy bad, both general copies fine: the newest copy
     * still agrees -> LOAD_STATUS_IS_GOOD. */
    memcpy(bad, img, SAVE4_IMAGE_SIZE);
    bad[so + 0x200] ^= 1;
    CHECK(save4_load(&c, bad, SAVE4_IMAGE_SIZE) == SAVE4_OK);
    CHECK(c.load_result == SAVE4_LOAD_OK && c.blocks[0].active == 1 && c.blocks[1].active == 1);
    save4_free(&c);

    /* Newer storage copy bad: the newer general copy has no partner, the older
     * pair agrees -> SLOT_FAIL on copy 0. */
    memcpy(bad, img, SAVE4_IMAGE_SIZE);
    bad[SAVE4_COPY_SIZE + so + 0x200] ^= 1;
    CHECK(save4_load(&c, bad, SAVE4_IMAGE_SIZE) == SAVE4_OK);
    CHECK(c.load_result == SAVE4_LOAD_RECOVERED && c.blocks[0].active == 0 && c.blocks[1].active == 0);
    save4_trainer t;
    CHECK(save4_get_trainer(&c, &t) == SAVE4_OK && t.money == SYNTH_MONEY_OLD);
    save4_free(&c);

    /* A version byte other than 7/8 (re-checksummed) is not an HG/SS save. */
    memcpy(bad, img, SAVE4_IMAGE_SIZE);
    for (int copy = 0; copy < 2; copy++) {
        uint8_t *g = bad + copy * SAVE4_COPY_SIZE;
        g[0x60 + 0x20] = 12;
        uint16_t crc = save4_crc16(g, gs - 0x10);
        g[gs - 2] = (uint8_t)crc;
        g[gs - 1] = (uint8_t)(crc >> 8);
    }
    CHECK(save4_load(&c, bad, SAVE4_IMAGE_SIZE) == SAVE4_ERR_UNKNOWN_GAME);
    save4_free(&c);

    /* SoulSilver: same layout, version 8. */
    synth_save_build(img, SAVE4_GAME_SS);
    CHECK(save4_load(&c, img, SAVE4_IMAGE_SIZE) == SAVE4_OK && c.game == SAVE4_GAME_SS);
    save4_free(&c);
    free(bad);
    free(img);
}

/* Mystery Gift / Pokédex on the synthetic HeartGold save: MysteryGiftSave at
 * 0x9D3C (gifts 0x100, cards 0x920, specialWonderCard 0x1328, CRC-16 at
 * 0x1680), SysInfo.mysteryGiftActive 0x48, Pokedex 0x12B8 + 0x336 / 0x337,
 * and the gift types HG/SS deliver. */
static void test_mystery_hgss(void)
{
    static const uint16_t yes[] = {SAVE4_MG_POKEMON,  SAVE4_MG_EGG,     SAVE4_MG_ITEM,
                                   SAVE4_MG_BATTLE_REG, SAVE4_MG_COSMETICS, SAVE4_MG_MANAPHY_EGG,
                                   SAVE4_MG_UNKNOWN,  SAVE4_MG_POKEWALKER_COURSE, SAVE4_MG_MEMORIAL_PHOTO};
    static const uint16_t no[] = {0, SAVE4_MG_DECORATION, SAVE4_MG_MEMBER_CARD, SAVE4_MG_OAKS_LETTER,
                                  SAVE4_MG_AZURE_FLUTE, SAVE4_MG_POKETCH_APP, SAVE4_MG_SECRET_KEY,
                                  SAVE4_MG_TYPE_MAX};
    for (size_t i = 0; i < sizeof yes / sizeof yes[0]; i++)
        CHECK(save4_mg_type_supported(SAVE4_GAME_HG, yes[i]) && save4_mg_type_supported(SAVE4_GAME_SS, yes[i]));
    for (size_t i = 0; i < sizeof no / sizeof no[0]; i++)
        CHECK(!save4_mg_type_supported(SAVE4_GAME_HG, no[i]) && !save4_mg_type_supported(SAVE4_GAME_SS, no[i]));
    CHECK(!save4_mg_type_supported(SAVE4_GAME_PT, SAVE4_MG_POKEWALKER_COURSE));
    CHECK(!save4_mg_type_supported(SAVE4_GAME_PT, SAVE4_MG_MEMORIAL_PHOTO));
    CHECK(!save4_mg_type_supported(SAVE4_GAME_DP, SAVE4_MG_UNKNOWN));

    uint8_t *img = malloc(SAVE4_IMAGE_SIZE);
    synth_save_build(img, SAVE4_GAME_HG);
    save4 s;
    CHECK(save4_load(&s, img, SAVE4_IMAGE_SIZE) == SAVE4_OK && s.game == SAVE4_GAME_HG);
    size_t len;
    const uint8_t *im = save4_image(&s, &len);
    const uint8_t *gen = im + save4_block_base(&s, SAVE4_BLOCK_GENERAL);
    const uint8_t *mg = gen + 0x9D3C;
    CHECK_EQ_INT(save4_crc16(mg, 0x1680), mg[0x1680] | mg[0x1681] << 8);

    /* Main menu switches and the Pokédex flags. */
    bool on = true;
    CHECK(save4_mg_get_unlocked(&s, &on) == SAVE4_OK && !on);
    CHECK(save4_mg_set_unlocked(&s, true) == SAVE4_OK);
    CHECK(save4_mg_get_unlocked(&s, &on) == SAVE4_OK && on);
    CHECK_EQ_INT(gen[0x48], 1);    /* SysInfo.mysteryGiftActive */
    CHECK_EQ_INT(mg[255] >> 7, 1); /* received bit 2047 */
    CHECK(save4_dex_get_obtained(&s, &on) == SAVE4_OK && !on);
    CHECK(save4_dex_set_obtained(&s, true) == SAVE4_OK);
    CHECK(save4_dex_get_obtained(&s, &on) == SAVE4_OK && on);
    CHECK_EQ_INT(gen[0x12B8 + 0x336], 1); /* Pokedex.dexEnabled */
    CHECK(save4_dex_set_national(&s, true) == SAVE4_OK);
    CHECK_EQ_INT(gen[0x12B8 + 0x337], 1);          /* Pokedex.nationalDex */
    CHECK_EQ_INT((gen[0x60 + 0x21] >> 1) & 1, 1); /* PlayerProfile.natDex */

    /* A card with its gift: card slot 0, gift 0 linked to slot 0. */
    save4_card_spec spec = {SAVE4_MG_POKEMON, 42, 0, {249, 0, 0}, 3500, "Lugia", "Gift"};
    uint8_t card[SAVE4_WONDERCARD_SIZE], back[SAVE4_WONDERCARD_SIZE];
    CHECK(save4_mg_build_card(&spec, card) == SAVE4_OK);
    CHECK(save4_mg_add(&s, card, sizeof card) == SAVE4_OK);
    bool used = false;
    CHECK(save4_mg_get_card(&s, 0, back, &used) == SAVE4_OK && used && !memcmp(back, card, sizeof card));
    CHECK(!memcmp(mg + 0x920, card, sizeof card));
    CHECK_EQ_INT(mg[0x100] | mg[0x101] << 8, SAVE4_MG_POKEMON);
    CHECK_EQ_INT(mg[0x102] & 3, 0);
    CHECK_EQ_INT(mg[42 / 8] >> (42 % 8) & 1, 1);
    CHECK_EQ_INT(save4_crc16(mg, 0x1680), mg[0x1680] | mg[0x1681] << 8);
    CHECK(save4_revalidate(&s) == SAVE4_OK);

    /* A gift-only .pgt: linked to no card (3). */
    CHECK(save4_mg_add(&s, card, SAVE4_PGT_SIZE) == SAVE4_OK);
    CHECK_EQ_INT(mg[0x100 + 0x104 + 2] & 3, 3);
    int pgts = 0;
    CHECK(save4_mg_pgt_count(&s, &pgts) == SAVE4_OK && pgts == 2);

    /* Types HG/SS cannot deliver are refused and change nothing. */
    uint8_t *snap = malloc(SAVE4_IMAGE_SIZE);
    memcpy(snap, im, SAVE4_IMAGE_SIZE);
    save4_card_spec darkrai = {SAVE4_MG_MEMBER_CARD, 50, 0, {491, 0, 0}, 3500, "Member Card", "Darkrai"};
    CHECK(save4_mg_build_card(&darkrai, card) == SAVE4_OK);
    CHECK(save4_mg_add(&s, card, sizeof card) == SAVE4_ERR_UNSUPPORTED);
    CHECK(!memcmp(snap, im, SAVE4_IMAGE_SIZE));

    /* The Lock Capsule card goes to the special slot, once. */
    save4_card_spec lock = {SAVE4_MG_ITEM, 60, 533, {0, 0, 0}, 3500, "Lock Capsule", "Capsule"};
    CHECK(save4_mg_build_card(&lock, card) == SAVE4_OK);
    CHECK(save4_mg_add(&s, card, sizeof card) == SAVE4_OK);
    CHECK(!memcmp(mg + 0x1328, card, sizeof card));
    CHECK(save4_mg_get_card(&s, 1, back, &used) == SAVE4_OK && !used);
    CHECK(save4_mg_pgt_count(&s, &pgts) == SAVE4_OK && pgts == 2);
    CHECK_EQ_INT(mg[60 / 8] >> (60 % 8) & 1, 1);
    CHECK(save4_mg_add(&s, card, sizeof card) == SAVE4_ERR_NOSPACE);

    /* A card without a gift (savePgt clear) in slot 1. */
    spec.id = 43;
    CHECK(save4_mg_build_card(&spec, card) == SAVE4_OK);
    card[0x104 + 0x4E] &= (uint8_t)~(1u << 3);
    CHECK(save4_mg_add(&s, card, sizeof card) == SAVE4_OK);
    CHECK(save4_mg_pgt_count(&s, &pgts) == SAVE4_OK && pgts == 2);

    /* Tossing card 0 (gift linked): type, received flag and its gift go. */
    CHECK(save4_mg_remove_card(&s, 0) == SAVE4_OK);
    CHECK(save4_mg_get_card(&s, 0, back, &used) == SAVE4_OK && !used);
    CHECK_EQ_INT(mg[42 / 8] >> (42 % 8) & 1, 0);
    CHECK_EQ_INT(mg[0x100] | mg[0x101] << 8, 0);
    CHECK(save4_mg_pgt_count(&s, &pgts) == SAVE4_OK && pgts == 1);
    /* Tossing card 1 (no gift): only the type (DeleteWonderCardByIndex). */
    CHECK(save4_mg_remove_card(&s, 1) == SAVE4_OK);
    CHECK(save4_mg_get_card(&s, 1, back, &used) == SAVE4_OK && !used);
    CHECK_EQ_INT(mg[43 / 8] >> (43 % 8) & 1, 1);
    CHECK_EQ_INT(save4_crc16(mg, 0x1680), mg[0x1680] | mg[0x1681] << 8);

    /* Reloads with the same state. */
    save4 s2;
    CHECK(save4_load(&s2, im, len) == SAVE4_OK && s2.load_result == SAVE4_LOAD_OK && s2.game == SAVE4_GAME_HG);
    CHECK(save4_mg_get_unlocked(&s2, &on) == SAVE4_OK && on);
    CHECK(save4_dex_get_national(&s2, &on) == SAVE4_OK && on);
    save4_trainer tr;
    CHECK(save4_get_trainer(&s2, &tr) == SAVE4_OK && tr.has_national_dex);
    save4_free(&s2);
    /* Nothing outside the MysteryGift entry (+ CRC), SysInfo's flag and the
     * Pokédex / profile flags moved in the general block. */
    size_t gb = save4_block_base(&s, SAVE4_BLOCK_GENERAL);
    CHECK_EQ_INT(count_diff(snap, im, gb + 0x49, gb + 0x9D3C), 0);
    CHECK_EQ_INT(count_diff(snap, im, gb + 0x9D3C + 0x1684, gb + 0xF628 - 2), 0);
    free(snap);
    save4_free(&s);
    free(img);
}

/* HG/SS set-location: Location and the saved map objects move together; the
 * old map's people go; D/P/Pt say unsupported. */
static void test_location(save4_game game)
{
    static uint8_t img[SAVE4_IMAGE_SIZE];
    synth_save_build(img, game);
    save4 s;
    CHECK(save4_load(&s, img, sizeof(img)) == SAVE4_OK);
    save4_location loc = {.map = 76, .warp = 5, .x = 371, .z = 333, .dir = 2};
    if (!save4_game_is_hgss(game)) {
        CHECK(save4_set_location(&s, &loc, -1) == SAVE4_ERR_UNSUPPORTED);
        return;
    }
    save4_location got;
    CHECK(save4_get_location(&s, &got) == SAVE4_OK);
    CHECK_EQ_INT(got.map, SYNTH_HGSS_MAP);
    CHECK_EQ_INT(got.x, SYNTH_HGSS_X);
    loc.dir = 4;
    CHECK(save4_set_location(&s, &loc, -1) == SAVE4_ERR_ARG);
    loc.dir = 2;
    CHECK(save4_set_location(&s, &loc, -1) == SAVE4_OK);
    size_t len;
    const uint8_t *e = save4_image(&s, &len);
    save4 r;
    CHECK(save4_load(&r, e, len) == SAVE4_OK && r.load_result == SAVE4_LOAD_OK);
    CHECK(save4_get_location(&r, &got) == SAVE4_OK);
    CHECK_EQ_INT(got.map, 76);
    CHECK_EQ_INT(got.warp, -1);
    CHECK_EQ_INT(got.x, 371);
    CHECK_EQ_INT(got.z, 333);
    CHECK_EQ_INT(got.dir, 2);
    const uint8_t *mo = e + SAVE4_COPY_SIZE + 0x2348; /* the newer copy */
    for (int i = 0; i < 2; i++) {
        const uint8_t *o = mo + i * 0x50;
        CHECK_EQ_INT(r32(o) & 1u, 1);
        CHECK_EQ_INT(o[0x26] | o[0x27] << 8, 371);
        CHECK_EQ_INT(o[0x2A] | o[0x2B] << 8, 333);
        CHECK_EQ_INT(o[0x28] | o[0x29] << 8, 2); /* the saved height kept */
        CHECK_EQ_INT(o[0x0D], 2);
    }
    CHECK_EQ_INT(r32(mo + 2 * 0x50), 0); /* the old map's person dropped */
    CHECK(save4_set_location(&s, &loc, 4) == SAVE4_OK);
    e = save4_image(&s, &len);
    CHECK_EQ_INT(e[SAVE4_COPY_SIZE + 0x2348 + 0x28], 4);
    CHECK_EQ_INT(r32(e + SAVE4_COPY_SIZE + 0x2348 + 0x2C), 4u << 15);
}

int main(void)
{
    test_game(SAVE4_GAME_PT);
    test_game(SAVE4_GAME_DP);
    test_game(SAVE4_GAME_HG);
    test_game(SAVE4_GAME_SS);
    test_mystery();
    test_mystery_dp();
    test_hgss_selection();
    test_mystery_hgss();
    test_location(SAVE4_GAME_PT);
    test_location(SAVE4_GAME_DP);
    test_location(SAVE4_GAME_HG);
    test_location(SAVE4_GAME_SS);

    /* Flag/var names generated from the decomp. */
    uint16_t id = 0;
    CHECK(save4_pt_lookup_name("VAR_RESULT", &id) == 0);
    CHECK_EQ_INT(id, 0x800C);
    CHECK(save4_pt_flag_names_count > 2000);
    CHECK(save4_pt_lookup_name("NOT_A_FLAG", &id) != 0);
    return TEST_RESULT();
}
