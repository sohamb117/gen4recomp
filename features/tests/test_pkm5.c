/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdint.h>
#include <string.h>

#include "save5/save5.h"
#include "synth_save5.h"
#include "testutil.h"

static uint16_t r16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

static void test_roundtrip(void)
{
    /* every shuffle order: PID bits 13-17 select it */
    for (uint32_t k = 0; k < 32; k++) {
        const uint32_t pid = k << 13 | 0x80000123u;
        pkm5 p, q;
        synth5_make_mon(&p, 495, 17, pid, 12, "SNIVY", 33, 1);
        uint8_t enc[PKM5_PARTY_SIZE];
        CHECK(pkm5_encrypt(&p, enc, sizeof enc) == SAVE5_OK);
        CHECK_EQ_INT(r16(enc + 6), pkm5_calc_checksum(&p));
        /* the stored blocks are encrypted: the species word is not in clear */
        CHECK(memcmp(enc + 8, p.data + 8, 128) != 0);
        /* the party tail is encrypted with the PID */
        CHECK(memcmp(enc + PKM5_BOX_SIZE, p.data + PKM5_BOX_SIZE, PKM5_PARTY_SIZE - PKM5_BOX_SIZE) != 0);
        CHECK(pkm5_decrypt(enc, sizeof enc, &q) == SAVE5_OK);
        CHECK(memcmp(p.data, q.data, PKM5_PARTY_SIZE) == 0);
        CHECK(q.party);

        /* the box form carries the same first 136 bytes */
        uint8_t box[PKM5_BOX_SIZE];
        CHECK(pkm5_encrypt(&p, box, sizeof box) == SAVE5_OK);
        CHECK(memcmp(box, enc, PKM5_BOX_SIZE) == 0);
        CHECK(pkm5_decrypt(box, sizeof box, &q) == SAVE5_OK);
        CHECK(!q.party);
        CHECK(memcmp(p.data, q.data, PKM5_BOX_SIZE) == 0);
    }
    CHECK_EQ_INT(pkm5_shuffle_index(0), 0);
    CHECK_EQ_INT(pkm5_shuffle_index(23u << 13), 23);
    CHECK_EQ_INT(pkm5_shuffle_index(24u << 13), 0);
    CHECK_EQ_INT(pkm5_shuffle_index(31u << 13), 7);
}

static void test_fields(void)
{
    pkm5 p;
    synth5_make_mon(&p, 495, 5, 0x12345678u, 3, "SNIVY", 33, 1);
    pkm5_set_held_item(&p, 234);
    pkm5_set_ability(&p, 126, true);
    pkm5_set_gender_form(&p, 1, 2);
    pkm5_set_is_egg(&p, true);
    pkm5_set_move(&p, 3, 15, 30, 3);
    pkm5_info i;
    pkm5_info_get(&p, &i);
    CHECK_EQ_INT(i.species, 495);
    CHECK_EQ_INT(i.pid, 0x12345678u);
    CHECK_EQ_INT(i.nature, 3);
    CHECK_EQ_INT(i.held_item, 234);
    CHECK_EQ_INT(i.ability, 126);
    CHECK(i.hidden_ability);
    CHECK_EQ_INT(i.gender, 1);
    CHECK_EQ_INT(i.form, 2);
    CHECK(i.is_egg);
    CHECK(!i.has_nickname);
    CHECK_EQ_INT(i.moves[0], 33);
    CHECK_EQ_INT(i.moves[3], 15);
    CHECK_EQ_INT(i.pp[3], 30);
    CHECK_EQ_INT(i.pp_ups[3], 3);
    for (int s = 0; s < 6; s++)
        CHECK_EQ_INT(i.ivs[s], s * 5 + 1);
    CHECK_EQ_INT(i.evs[0], 4);
    CHECK_EQ_STR(i.nickname, "SNIVY");
    CHECK_EQ_STR(i.ot_name, SYNTH5_TRAINER_NAME);
    CHECK_EQ_INT(i.tid, SYNTH5_TID);
    CHECK_EQ_INT(i.sid, SYNTH5_SID);
    CHECK_EQ_INT(i.met_level, 5);
    CHECK_EQ_INT(i.ball, 4);
    CHECK_EQ_INT(i.origin_game, 21);
    CHECK_EQ_INT(i.level, 5);
    CHECK_EQ_INT(i.hp, 19);
    CHECK_EQ_INT(i.stats[0], 20);
    CHECK(i.checksum_ok == (pkm5_calc_checksum(&p) == i.checksum));

    /* shiny: (tid ^ sid ^ pid_hi ^ pid_lo) < 8 */
    pkm5_set_ot_ids(&p, 0x1234, 0x0000);
    pkm5_set_pid(&p, 0x00001234u ^ 0x5u);
    pkm5_info_get(&p, &i);
    CHECK(i.shiny);
    pkm5_set_pid(&p, 0x00001234u ^ 0x8u);
    pkm5_info_get(&p, &i);
    CHECK(!i.shiny);

    /* a flipped ciphertext byte is reported, the data still decrypts */
    uint8_t enc[PKM5_BOX_SIZE];
    pkm5_encrypt(&p, enc, sizeof enc);
    enc[40] ^= 1;
    pkm5 q;
    CHECK(pkm5_decrypt(enc, sizeof enc, &q) == SAVE5_ERR_PKM_CHECKSUM);
    CHECK(pkm5_decrypt(enc, 100, &q) == SAVE5_ERR_ARG);

    /* nickname flag, length limit (10 characters) */
    CHECK(pkm5_set_nickname(&p, "Leafy", true) == SAVE5_OK);
    pkm5_info_get(&p, &i);
    CHECK(i.has_nickname);
    CHECK_EQ_STR(i.nickname, "Leafy");
    CHECK(pkm5_set_nickname(&p, "ElevenChars", true) == SAVE5_ERR_RANGE);
    CHECK(pkm5_set_nickname(&p, "TenCharsOK", true) == SAVE5_OK);
}

static void test_text(void)
{
    uint8_t buf[2 * 11];
    char out[64];
    CHECK(save5_text_encode("Nidoran\xE2\x99\x80", buf, 11) == SAVE5_OK); /* ♀ */
    CHECK_EQ_INT(r16(buf + 14), 0x246E);
    CHECK_EQ_INT(r16(buf + 16), 0xFFFF);
    CHECK_EQ_INT(r16(buf + 18), 0);
    save5_text_decode(buf, 11, out, sizeof out);
    CHECK_EQ_STR(out, "Nidoran\xE2\x99\x80");
    CHECK(save5_text_encode("Pok\xC3\xA9mon", buf, 11) == SAVE5_OK); /* é */
    CHECK_EQ_INT(r16(buf + 6), 0xE9);
    save5_text_decode(buf, 11, out, sizeof out);
    CHECK_EQ_STR(out, "Pok\xC3\xA9mon");
    CHECK(save5_text_encode("\xF0\x9F\x98\x80", buf, 11) == SAVE5_ERR_ENCODE); /* outside the BMP */
    CHECK(save5_text_encode("a\nb", buf, 11) == SAVE5_ERR_ENCODE);
    CHECK(save5_text_encode("0123456789", buf, 11) == SAVE5_OK);
    CHECK(save5_text_encode("01234567890", buf, 11) == SAVE5_ERR_RANGE);
    /* decode stops at 0xFFFF or 0 and truncates to the buffer */
    CHECK(save5_text_encode("ABCDEFG", buf, 8) == SAVE5_OK);
    CHECK_EQ_INT(save5_text_decode(buf, 8, out, 4), 7);
    CHECK_EQ_STR(out, "ABC");
}

static void test_stats(void)
{
    /* Bulbapedia's stat example: Garchomp Lv78 Adamant (HP Atk Def Spe SpA SpD order) */
    const uint8_t base[6] = {108, 130, 95, 102, 80, 85}, ivs[6] = {24, 12, 30, 5, 16, 23},
                  evs[6] = {74, 190, 91, 23, 48, 84};
    uint16_t st[6];
    pkm5_calc_stats(base, ivs, evs, 78, 3, false, st);
    CHECK_EQ_INT(st[0], 289);
    CHECK_EQ_INT(st[1], 278);
    CHECK_EQ_INT(st[2], 193);
    CHECK_EQ_INT(st[3], 171);
    CHECK_EQ_INT(st[4], 135);
    CHECK_EQ_INT(st[5], 171);
}

int main(void)
{
    test_roundtrip();
    test_fields();
    test_text();
    test_stats();
    return TEST_RESULT();
}
