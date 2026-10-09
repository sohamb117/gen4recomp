/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "save5/save5.h"
#include "synth_save5.h"
#include "testutil.h"

#define BACKUP SAVE5_COPY_OFFSET
#define TRAINER 0x19400u
#define MISC 0x21200u

static uint16_t r16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t r32(const uint8_t *p) { return (uint32_t)r16(p) | (uint32_t)r16(p + 2) << 16; }

static size_t count_diff(const uint8_t *x, const uint8_t *y, size_t a, size_t b)
{
    size_t n = 0;
    for (size_t i = a; i < b; i++)
        n += x[i] != y[i];
    return n;
}

static void test_load(save5_game game)
{
    uint8_t *img = malloc(SAVE5_IMAGE_SIZE);
    synth5_build(img, game);

    /* CRC-16-CCITT check value (init 0xFFFF, poly 0x1021): "123456789" -> 0x29B1 */
    CHECK_EQ_INT(save5_crc16("123456789", 9), 0x29B1);

    save5 s;
    CHECK(save5_load(&s, img, SAVE5_IMAGE_SIZE) == SAVE5_OK);
    CHECK(s.game == game);
    CHECK(s.load_result == SAVE5_LOAD_OK);
    CHECK_EQ_INT(s.active, 1); /* backup copy is newer */
    CHECK(s.copies[0].valid && s.copies[1].valid);
    CHECK_EQ_INT(s.copies[0].save_counter, SYNTH5_COUNTER_OLD);
    CHECK_EQ_INT(s.copies[1].save_counter, SYNTH5_COUNTER_NEW);
    for (int b = 0; b < SAVE5_DATA_BLOCKS; b++)
        CHECK(s.blocks[b].synced == (b != SAVE5_BLK_MISC));
    CHECK_EQ_INT(s.blocks[SAVE5_BLK_TRAINER].offset, TRAINER);
    CHECK_EQ_INT(s.blocks[SAVE5_BLK_TRAINER].size, 0x68);
    CHECK_EQ_STR(save5_block_name(SAVE5_BLK_TRAINER), "trainer");
    CHECK_EQ_STR(save5_block_name(24), "box 24");
    CHECK_EQ_STR(save5_game_name(game), game == SAVE5_GAME_BLACK ? "black" : "white");

    /* unmodified: the image is the input, byte for byte */
    size_t len;
    const uint8_t *out = save5_image(&s, &len);
    CHECK_EQ_INT(len, SAVE5_IMAGE_SIZE);
    CHECK(memcmp(out, img, SAVE5_IMAGE_SIZE) == 0);

    save5_trainer t;
    CHECK(save5_get_trainer(&s, &t) == SAVE5_OK);
    CHECK_EQ_STR(t.name, SYNTH5_TRAINER_NAME);
    CHECK_EQ_INT(t.tid, SYNTH5_TID);
    CHECK_EQ_INT(t.sid, SYNTH5_SID);
    CHECK_EQ_INT(t.money, SYNTH5_MONEY_NEW);
    CHECK_EQ_INT(t.badges, SYNTH5_BADGES);
    CHECK_EQ_INT(t.language, 2);
    CHECK_EQ_INT(t.version, game == SAVE5_GAME_BLACK ? 21 : 20);
    CHECK_EQ_INT(t.play_hours, 12);
    CHECK_EQ_INT(t.play_minutes, 34);
    CHECK_EQ_INT(t.play_seconds, 56);
    save5_location loc;
    CHECK(save5_get_location(&s, &loc) == SAVE5_OK);
    CHECK_EQ_INT(loc.map, SYNTH5_MAP);
    CHECK_EQ_INT(loc.x, 7);
    CHECK_EQ_INT(loc.y, 0);
    CHECK_EQ_INT(loc.z, 9);
    save5_game_time gt;
    CHECK(save5_get_last_saved(&s, &gt) == SAVE5_OK);
    CHECK(gt.year == 2011 && gt.month == 3 && gt.day == 6 && gt.hour == 13 && gt.minute == 45);

    CHECK_EQ_INT(save5_party_count(&s), 2);
    pkm5 p;
    pkm5_info info;
    CHECK(save5_get_party(&s, 0, &p) == SAVE5_OK);
    pkm5_info_get(&p, &info);
    CHECK_EQ_INT(info.species, 495);
    CHECK_EQ_INT(info.level, 5);
    CHECK_EQ_INT(info.nature, 3);
    CHECK_EQ_STR(info.nickname, "SNIVY");
    CHECK(save5_get_party(&s, 1, &p) == SAVE5_OK);
    pkm5_info_get(&p, &info);
    CHECK_EQ_INT(info.species, 504);
    CHECK_EQ_STR(info.nickname, "Pat");
    CHECK(info.has_nickname);
    CHECK(save5_get_party(&s, 6, &p) == SAVE5_ERR_RANGE);

    CHECK(save5_get_box_mon(&s, 0, 0, &p) == SAVE5_OK);
    pkm5_info_get(&p, &info);
    CHECK_EQ_INT(info.species, SYNTH5_BOX_SPECIES);
    CHECK(!info.has_party_data);
    CHECK(save5_get_box_mon(&s, 23, 29, &p) == SAVE5_OK && pkm5_is_empty(&p));
    CHECK(save5_get_box_mon(&s, 24, 0, &p) == SAVE5_ERR_RANGE);
    char name[64];
    CHECK(save5_get_box_name(&s, 0, name, sizeof name) == SAVE5_OK);
    CHECK_EQ_STR(name, "BOX 1");
    CHECK(save5_get_box_name(&s, 23, name, sizeof name) == SAVE5_OK);
    CHECK_EQ_STR(name, "BOX 24");
    CHECK_EQ_INT(save5_current_box(&s), 1);

    uint16_t item, qty;
    CHECK(save5_get_bag_slot(&s, SAVE5_POCKET_ITEMS, 0, &item, &qty) == SAVE5_OK);
    CHECK(item == 4 && qty == 10);
    CHECK(save5_get_bag_slot(&s, SAVE5_POCKET_MEDICINE, 0, &item, &qty) == SAVE5_OK);
    CHECK(item == 17 && qty == 5);
    CHECK(save5_get_bag_slot(&s, SAVE5_POCKET_BERRIES, 64, &item, &qty) == SAVE5_ERR_RANGE);
    CHECK_EQ_INT(save5_pocket_capacity(SAVE5_POCKET_ITEMS), 310);
    CHECK_EQ_STR(save5_pocket_name(SAVE5_POCKET_TMHM), "tms_hms");

    bool seen, caught, fv;
    CHECK(save5_dex_get(&s, 495, &seen, &caught) == SAVE5_OK && seen && caught);
    CHECK(save5_dex_get(&s, 504, &seen, &caught) == SAVE5_OK && seen && !caught);
    CHECK(save5_dex_get(&s, 1, &seen, &caught) == SAVE5_OK && !seen && !caught);
    CHECK(save5_dex_get(&s, 650, &seen, &caught) == SAVE5_ERR_RANGE);
    CHECK(save5_dex_get_national(&s, &fv) == SAVE5_OK && !fv);
    CHECK(save5_flag_get(&s, 10, &fv) == SAVE5_OK && fv);
    CHECK(save5_flag_get(&s, 2000, &fv) == SAVE5_OK && fv);
    CHECK(save5_flag_get(&s, 11, &fv) == SAVE5_OK && !fv);
    CHECK(save5_flag_get(&s, SAVE5_NUM_FLAGS, &fv) == SAVE5_ERR_RANGE);
    uint16_t var;
    CHECK(save5_var_get(&s, 0x4010, &var) == SAVE5_OK && var == 0x1234);
    CHECK(save5_var_get(&s, 0x3FFF, &var) == SAVE5_ERR_RANGE);
    CHECK(save5_var_get(&s, SAVE5_VARS_START + SAVE5_NUM_VARS, &var) == SAVE5_ERR_RANGE);

    uint8_t card[SAVE5_PGF_SIZE];
    bool used = false;
    CHECK(save5_mg_get_card(&s, 0, card, &used) == SAVE5_OK && used);
    CHECK_EQ_INT(r16(card + 0xB0), SYNTH5_CARD_ID);
    CHECK_EQ_INT(card[0xB3], SAVE5_MG_POKEMON);
    CHECK(save5_mg_get_card(&s, 1, card, &used) == SAVE5_OK && !used);
    CHECK(save5_mg_get_received(&s, SYNTH5_CARD_ID, &fv) == SAVE5_OK && fv);
    CHECK(save5_mg_get_received(&s, SYNTH5_CARD_ID + 1, &fv) == SAVE5_OK && !fv);

    /* ---- edits on a snapshot (undo = keep the original) */
    save5 e;
    CHECK(save5_clone(&s, &e) == SAVE5_OK);
    CHECK(save5_set_money(&e, 123456) == SAVE5_OK);
    CHECK(save5_set_money(&e, SAVE5_MONEY_MAX + 1) == SAVE5_ERR_RANGE);
    CHECK(save5_set_trainer_name(&e, "Hilda") == SAVE5_OK);
    CHECK(save5_set_trainer_name(&e, "TooLongName") == SAVE5_ERR_RANGE);
    CHECK(save5_set_trainer_ids(&e, 1, 2) == SAVE5_OK);
    CHECK(save5_set_gender(&e, 1) == SAVE5_OK);
    CHECK(save5_set_badges(&e, 0xFF) == SAVE5_OK);
    CHECK(save5_set_play_time(&e, 99, 59, 58) == SAVE5_OK);
    CHECK(save5_set_bag_slot(&e, SAVE5_POCKET_ITEMS, 1, 50, 3) == SAVE5_OK); /* Rare Candy */
    CHECK(save5_set_bag_slot(&e, SAVE5_POCKET_ITEMS, 1, 50, 1000) == SAVE5_ERR_RANGE);
    CHECK(save5_set_bag_slot(&e, SAVE5_POCKET_KEY_ITEMS, 0, 442, 2) == SAVE5_ERR_RANGE);
    CHECK(save5_dex_set(&e, 2, false, true) == SAVE5_OK);
    CHECK(save5_dex_set(&e, 504, false, false) == SAVE5_OK);
    CHECK(save5_dex_set_national(&e, true) == SAVE5_OK);
    CHECK(save5_flag_set(&e, 10, false) == SAVE5_OK);
    CHECK(save5_flag_set(&e, 2911, true) == SAVE5_OK);
    CHECK(save5_var_set(&e, 0x4011, 7) == SAVE5_OK);
    CHECK(save5_set_box_name(&e, 2, "Favs") == SAVE5_OK);
    CHECK(save5_set_box_name(&e, 2, "NineChars") == SAVE5_ERR_RANGE);
    save5_location wl = {252, 8, 0, 9};
    CHECK(save5_set_location(&e, &wl) == SAVE5_OK);
    CHECK(save5_get_party(&e, 0, &p) == SAVE5_OK);
    pkm5_set_held_item(&p, 234);
    pkm5_set_exp(&p, 1000);
    CHECK(save5_set_party(&e, 0, &p) == SAVE5_OK);
    pkm5 nm;
    synth5_make_mon(&nm, 25, 12, 0xCAFEBABEu, 18, "Sparky", 84, 0);
    CHECK(save5_set_box_mon(&e, 4, 7, &nm) == SAVE5_OK);
    CHECK(save5_clear_box_mon(&e, 0, 0) == SAVE5_OK);
    uint8_t gift[SAVE5_PGF_SIZE];
    synth5_make_pgf(gift, SAVE5_MG_ITEM, 300);
    CHECK(save5_mg_add(&e, gift, sizeof gift) == SAVE5_OK);
    CHECK(save5_mg_remove_card(&e, 0) == SAVE5_OK);

    /* Original untouched */
    CHECK(save5_get_trainer(&s, &t) == SAVE5_OK && t.money == SYNTH5_MONEY_NEW);
    CHECK_EQ_STR(t.name, SYNTH5_TRAINER_NAME);

    /* Reload the edited bytes: everything validates and reads back. */
    size_t elen;
    const uint8_t *eimg = save5_image(&e, &elen);
    save5 r;
    CHECK(save5_load(&r, eimg, elen) == SAVE5_OK);
    CHECK(r.load_result == SAVE5_LOAD_OK && r.active == 1);
    CHECK(save5_get_trainer(&r, &t) == SAVE5_OK);
    CHECK_EQ_INT(t.money, 123456);
    CHECK_EQ_STR(t.name, "Hilda");
    CHECK(t.tid == 1 && t.sid == 2 && t.gender == 1 && t.badges == 0xFF);
    CHECK(t.play_hours == 99 && t.play_minutes == 59 && t.play_seconds == 58);
    CHECK(save5_get_bag_slot(&r, SAVE5_POCKET_ITEMS, 1, &item, &qty) == SAVE5_OK && item == 50 && qty == 3);
    CHECK(save5_dex_get(&r, 2, &seen, &caught) == SAVE5_OK && seen && caught);
    CHECK(save5_dex_get(&r, 504, &seen, &caught) == SAVE5_OK && !seen && !caught);
    CHECK(save5_dex_get_national(&r, &fv) == SAVE5_OK && fv);
    CHECK(save5_flag_get(&r, 10, &fv) == SAVE5_OK && !fv);
    CHECK(save5_flag_get(&r, 2911, &fv) == SAVE5_OK && fv);
    CHECK(save5_var_get(&r, 0x4011, &var) == SAVE5_OK && var == 7);
    CHECK(save5_get_box_name(&r, 2, name, sizeof name) == SAVE5_OK);
    CHECK_EQ_STR(name, "Favs");
    CHECK(save5_get_location(&r, &loc) == SAVE5_OK);
    CHECK(loc.map == 252 && loc.x == 8 && loc.y == 0 && loc.z == 9);
    CHECK(save5_get_party(&r, 0, &p) == SAVE5_OK);
    pkm5_info_get(&p, &info);
    CHECK(info.held_item == 234 && info.exp == 1000 && info.level == 5);
    CHECK(save5_get_box_mon(&r, 4, 7, &p) == SAVE5_OK);
    pkm5_info_get(&p, &info);
    CHECK(info.species == 25 && info.nature == 18);
    CHECK_EQ_STR(info.nickname, "Sparky");
    CHECK(save5_get_box_mon(&r, 0, 0, &p) == SAVE5_OK && pkm5_is_empty(&p));
    CHECK(save5_mg_get_card(&r, 0, card, &used) == SAVE5_OK && !used);
    CHECK(save5_mg_get_card(&r, 1, card, &used) == SAVE5_OK && used && r16(card + 0xB0) == 300);
    CHECK(save5_mg_get_received(&r, 300, &fv) == SAVE5_OK && fv);
    CHECK(save5_mg_get_received(&r, SYNTH5_CARD_ID, &fv) == SAVE5_OK && fv);
    /* the Mystery Gift block stays encrypted under its seed */
    CHECK_EQ_INT(r32(eimg + BACKUP + 0x1C800 + 0xA90), SYNTH5_MG_SEED);

    /* Synced blocks were mirrored, the money block (backup-only change) was not. */
    CHECK(memcmp(eimg + TRAINER, eimg + BACKUP + TRAINER, 0x68 + 4) == 0);
    CHECK_EQ_INT(r32(eimg + MISC), SYNTH5_MONEY_OLD);
    CHECK_EQ_INT(r32(eimg + BACKUP + MISC), 123456);
    for (int b = 0; b < SAVE5_DATA_BLOCKS; b++)
        CHECK(r.blocks[b].synced == (b != SAVE5_BLK_MISC));
    /* Bytes outside the two copies and between blocks are untouched. */
    CHECK_EQ_INT(count_diff(img, eimg, 2 * BACKUP, SAVE5_IMAGE_SIZE), 0);
    CHECK_EQ_INT(count_diff(img, eimg, TRAINER + 0x68 + 4, 0x19500), 0);
    /* An unknown block's bytes are untouched. */
    CHECK_EQ_INT(count_diff(img, eimg, BACKUP + 0x19600, BACKUP + 0x19600 + 0x1338 + 4), 0);
    /* Party edit leaves the trainer's unknown bytes alone. */
    CHECK_EQ_INT(count_diff(img, eimg, BACKUP + TRAINER + 0x30, BACKUP + TRAINER + 0x68), 0);

    /* Range errors */
    CHECK(save5_set_party_count(&e, 7) == SAVE5_ERR_RANGE);
    CHECK(save5_set_box_mon(&e, 24, 0, &nm) == SAVE5_ERR_RANGE);
    CHECK(save5_set_play_time(&e, 1000, 0, 0) == SAVE5_ERR_RANGE);
    CHECK(save5_flag_set(&e, SAVE5_NUM_FLAGS, true) == SAVE5_ERR_RANGE);
    CHECK(save5_mg_set_received(&e, SAVE5_MG_FLAGS, true) == SAVE5_ERR_RANGE);

    save5_free(&r);
    save5_free(&e);
    save5_free(&s);
    free(img);
}

static void test_mystery_full(void)
{
    uint8_t *img = malloc(SAVE5_IMAGE_SIZE);
    synth5_build(img, SAVE5_GAME_WHITE);
    save5 s;
    CHECK(save5_load(&s, img, SAVE5_IMAGE_SIZE) == SAVE5_OK);
    uint8_t gift[SAVE5_PGF_SIZE];
    for (int i = 1; i < SAVE5_MG_SLOTS; i++) {
        synth5_make_pgf(gift, SAVE5_MG_POKEMON, (uint16_t)(1000 + i));
        CHECK(save5_mg_add(&s, gift, sizeof gift) == SAVE5_OK);
    }
    CHECK(save5_mg_add(&s, gift, sizeof gift) == SAVE5_ERR_NOSPACE);
    const char *why = NULL;
    CHECK(save5_mg_validate(gift, sizeof gift - 1, &why) == SAVE5_ERR_FORMAT && why);
    gift[0xB3] = 4;
    CHECK(save5_mg_validate(gift, sizeof gift, &why) == SAVE5_ERR_FORMAT);
    synth5_make_pgf(gift, SAVE5_MG_ITEM, 2048);
    CHECK(save5_mg_validate(gift, sizeof gift, &why) == SAVE5_ERR_FORMAT);
    synth5_make_pgf(gift, SAVE5_MG_POKEMON, 5);
    gift[0x1A] = gift[0x1B] = 0;
    CHECK(save5_mg_validate(gift, sizeof gift, &why) == SAVE5_ERR_FORMAT);
    save5_free(&s);
    free(img);
}

static void test_damage(void)
{
    uint8_t *img = malloc(SAVE5_IMAGE_SIZE);
    save5 s;

    /* one copy's block corrupted: the other copy is used */
    synth5_build(img, SAVE5_GAME_BLACK);
    img[BACKUP + TRAINER + 0x30] ^= 0x5A;
    CHECK(save5_load(&s, img, SAVE5_IMAGE_SIZE) == SAVE5_OK);
    CHECK(s.load_result == SAVE5_LOAD_RECOVERED);
    CHECK_EQ_INT(s.active, 0);
    CHECK(!s.blocks[SAVE5_BLK_TRAINER].valid[1] && s.blocks[SAVE5_BLK_TRAINER].valid[0]);
    CHECK(!s.blocks[SAVE5_BLK_TRAINER].synced);
    CHECK_EQ_INT(s.copies[1].bad_blocks, 1);
    save5_trainer t;
    CHECK(save5_get_trainer(&s, &t) == SAVE5_OK && t.money == SYNTH5_MONEY_OLD);
    /* an edit to the damaged block stays in the valid copy */
    CHECK(save5_set_trainer_name(&s, "Cheren") == SAVE5_OK);
    CHECK(!s.blocks[SAVE5_BLK_TRAINER].valid[1] && s.copies[0].valid);
    save5_free(&s);

    /* both copies of a block corrupted: no valid copy */
    img[TRAINER + 0x30] ^= 0x5A;
    CHECK(save5_load(&s, img, SAVE5_IMAGE_SIZE) == SAVE5_ERR_CHECKSUM);
    CHECK(!s.copies[0].valid && !s.copies[1].valid);
    CHECK(!s.blocks[SAVE5_BLK_TRAINER].valid[0] && !s.blocks[SAVE5_BLK_TRAINER].valid[1]);
    CHECK(s.blocks[SAVE5_BLK_PARTY].valid[0] && s.blocks[SAVE5_BLK_PARTY].valid[1]);
    save5_free(&s);

    /* a mirror entry alone disagreeing invalidates the block */
    synth5_build(img, SAVE5_GAME_BLACK);
    img[0x23F00 + 2 * SAVE5_BLK_PARTY] ^= 1;
    save5_fix_all_checksums(img); /* rewrites mirrors from the blocks */
    CHECK(save5_load(&s, img, SAVE5_IMAGE_SIZE) == SAVE5_OK && s.load_result == SAVE5_LOAD_OK);
    save5_free(&s);
    img[0x23F00 + 2 * SAVE5_BLK_PARTY] ^= 1;
    CHECK(save5_load(&s, img, SAVE5_IMAGE_SIZE) == SAVE5_OK);
    CHECK(s.load_result == SAVE5_LOAD_RECOVERED && s.active == 1);
    CHECK(!s.copies[0].table_ok && !s.blocks[SAVE5_BLK_PARTY].valid[0]);
    save5_free(&s);

    /* footer magic gone in both copies: not a Black/White save */
    synth5_build(img, SAVE5_GAME_BLACK);
    img[0x23F94] ^= 0xFF;
    img[BACKUP + 0x23F94] ^= 0xFF;
    CHECK(save5_load(&s, img, SAVE5_IMAGE_SIZE) == SAVE5_ERR_UNKNOWN_GAME);
    save5_free(&s);

    /* unknown version byte */
    synth5_build(img, SAVE5_GAME_BLACK);
    img[TRAINER + 0x1F] = 22;
    img[BACKUP + TRAINER + 0x1F] = 22;
    save5_fix_all_checksums(img);
    CHECK(save5_load(&s, img, SAVE5_IMAGE_SIZE) == SAVE5_ERR_UNKNOWN_GAME);
    save5_free(&s);

    /* erased flash, short image */
    memset(img, 0xFF, SAVE5_IMAGE_SIZE);
    CHECK(save5_load(&s, img, SAVE5_IMAGE_SIZE) == SAVE5_ERR_EMPTY);
    save5_free(&s);
    CHECK(save5_load(&s, img, SAVE5_IMAGE_SIZE - 1) == SAVE5_ERR_SIZE);
    save5_free(&s);
    free(img);
}

int main(void)
{
    test_load(SAVE5_GAME_BLACK);
    test_load(SAVE5_GAME_WHITE);
    test_mystery_full();
    test_damage();
    return TEST_RESULT();
}
