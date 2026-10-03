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

    /* ---- edits on a snapshot (undo = keep the original) */
    save4 e;
    CHECK(save4_clone(&s, &e) == SAVE4_OK);
    CHECK(save4_set_money(&e, 123456) == SAVE4_OK);
    CHECK(save4_set_money(&e, 1000000) == SAVE4_ERR_RANGE);
    CHECK(save4_set_trainer_name(&e, "Dawn") == SAVE4_OK);
    CHECK(save4_set_trainer_name(&e, "TooLongName") == SAVE4_ERR_RANGE);
    CHECK(save4_set_badges(&e, 0xFF) == SAVE4_OK);
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
    CHECK_EQ_INT(count_diff(img, eimg, SAVE4_COPY_SIZE + gsize + ssize, SAVE4_IMAGE_SIZE), 0);
    /* Footer counters / signature untouched; only the CRC moved. */
    const uint8_t *fo = img + SAVE4_COPY_SIZE + gsize - SAVE4_FOOTER_SIZE;
    const uint8_t *fe = eimg + SAVE4_COPY_SIZE + gsize - SAVE4_FOOTER_SIZE;
    CHECK(memcmp(fo, fe, 18) == 0);
    /* Unknown bytes (noise) preserved: Poketch / field state between the end
     * of VarsFlags and the Pokédex (PKHeX PoketchStart Pt 0x1160, DP 0x114C). */
    {
        uint32_t a = SAVE4_COPY_SIZE + (game == SAVE4_GAME_PT ? 0x1160 : 0x114C);
        uint32_t b = SAVE4_COPY_SIZE + (game == SAVE4_GAME_PT ? 0x1328 : 0x12DC);
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
    bad[gsize + 0x100] ^= 1;
    bad[SAVE4_COPY_SIZE + gsize + 0x100] ^= 1;
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
        uint8_t *f0 = bad + gsize - SAVE4_FOOTER_SIZE;
        uint8_t *f1 = bad + SAVE4_COPY_SIZE + gsize - SAVE4_FOOTER_SIZE;
        uint8_t *g0 = bad + gsize + ssize - SAVE4_FOOTER_SIZE;
        uint8_t *g1 = bad + SAVE4_COPY_SIZE + gsize + ssize - SAVE4_FOOTER_SIZE;
        memset(f0, 0, 8);   /* primary: counter 0 */
        memset(g0, 0, 8);
        memset(f1, 0xFF, 8); /* backup: counter 0xFFFFFFFF */
        memset(g1, 0xFF, 8);
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

int main(void)
{
    test_game(SAVE4_GAME_PT);
    test_game(SAVE4_GAME_DP);

    /* Flag/var names generated from the decomp. */
    uint16_t id = 0;
    CHECK(save4_pt_lookup_name("VAR_RESULT", &id) == 0);
    CHECK_EQ_INT(id, 0x800C);
    CHECK(save4_pt_flag_names_count > 2000);
    CHECK(save4_pt_lookup_name("NOT_A_FLAG", &id) != 0);
    return TEST_RESULT();
}
