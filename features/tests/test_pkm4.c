/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdint.h>
#include <stdlib.h>

#include "save4/save4.h"
#include "synth_save.h"
#include "testutil.h"

static uint16_t r16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

int main(void)
{
    /* Known answer: pid 0 (block order ABCD), species 1, everything else 0.
     * checksum = 1; stream words = data ^ (LCG(seed=1) >> 16), LCG from
     * pokeplatinum include/math_util.h (1103515245, 24691). */
    pkm4 k;
    memset(&k, 0, sizeof(k));
    pkm4_set_species(&k, 1);
    uint8_t enc[PKM4_PARTY_SIZE];
    CHECK(pkm4_encrypt(&k, enc, PKM4_BOX_SIZE) == SAVE4_OK);
    CHECK_EQ_INT(r16(enc + 6), 1);
    CHECK_EQ_INT(r16(enc + 8), 0x41C7);
    CHECK_EQ_INT(r16(enc + 10), 0xAC21);
    CHECK_EQ_INT(r16(enc + 12), 0xD2EE);
    CHECK_EQ_INT(r16(enc + 14), 0x1FB7);

    /* Hand-built party Pokémon: encrypt, decrypt, compare, checksum. */
    pkm4 mon, back;
    synth_make_mon(&mon, 387, 5, 0x12345678u, "TURTWIG", 33, 1);
    pkm4_set_held_item(&mon, 234); /* Leftovers */
    pkm4_set_move(&mon, 1, 45, 40, 2);
    CHECK(pkm4_encrypt(&mon, enc, PKM4_PARTY_SIZE) == SAVE4_OK);
    CHECK(memcmp(enc + 8, mon.data + 8, PKM4_PARTY_SIZE - 8) != 0); /* actually encrypted */
    CHECK_EQ_INT(r16(enc + 6), pkm4_calc_checksum(&mon));
    CHECK(pkm4_decrypt(enc, PKM4_PARTY_SIZE, &back) == SAVE4_OK);
    CHECK(back.party);
    CHECK(memcmp(back.data, mon.data, PKM4_PARTY_SIZE) == 0);

    pkm4_info info;
    pkm4_info_get(&back, &info);
    CHECK_EQ_INT(info.pid, 0x12345678u);
    CHECK_EQ_INT(info.species, 387);
    CHECK_EQ_INT(info.held_item, 234);
    CHECK_EQ_INT(info.tid, SYNTH_TID);
    CHECK_EQ_INT(info.sid, SYNTH_SID);
    CHECK_EQ_INT(info.exp, 125);
    CHECK_EQ_INT(info.moves[0], 33);
    CHECK_EQ_INT(info.moves[1], 45);
    CHECK_EQ_INT(info.pp[1], 40);
    CHECK_EQ_INT(info.pp_ups[1], 2);
    for (int i = 0; i < 6; i++)
        CHECK_EQ_INT(info.ivs[i], i * 5 + 1);
    CHECK_EQ_INT(info.evs[0], 4);
    CHECK_EQ_STR(info.nickname, "TURTWIG");
    CHECK_EQ_STR(info.ot_name, SYNTH_TRAINER_NAME);
    CHECK_EQ_INT(info.met_location, 1);
    CHECK_EQ_INT(info.met_level, 5);
    CHECK_EQ_INT(info.ball, 4);
    CHECK_EQ_INT(info.nature, 0x12345678u % 25);
    CHECK(info.checksum_ok);
    CHECK(info.has_party_data);
    CHECK_EQ_INT(info.level, 5);
    CHECK_EQ_INT(info.stats[0], 20);
    CHECK(!info.is_egg);

    /* Shiny rule: (tid ^ sid ^ pid_hi ^ pid_lo) < 8 */
    pkm4 sh = mon;
    pkm4_set_pid(&sh, ((uint32_t)(SYNTH_TID ^ SYNTH_SID ^ 0x0003) << 16) | 0x0000);
    pkm4_info_get(&sh, &info);
    CHECK(info.shiny);

    /* Party tail is keyed by PID: changing the tail changes only bytes >= 136. */
    uint8_t enc2[PKM4_PARTY_SIZE];
    pkm4 mon2 = mon;
    uint16_t stats[6] = {99, 1, 2, 3, 4, 5};
    pkm4_set_party_stats(&mon2, 50, 99, stats, 0);
    pkm4_encrypt(&mon2, enc2, PKM4_PARTY_SIZE);
    CHECK(memcmp(enc, enc2, PKM4_BOX_SIZE) == 0);
    CHECK(memcmp(enc + PKM4_BOX_SIZE, enc2 + PKM4_BOX_SIZE, PKM4_PARTY_SIZE - PKM4_BOX_SIZE) != 0);

    /* Every shuffle order round-trips; check placement for orders 0 and 23. */
    for (uint32_t sidx = 0; sidx < 32; sidx++) {
        pkm4 m = mon;
        uint32_t pid = (sidx << 13) | 0x1001;
        pkm4_set_pid(&m, pid);
        CHECK_EQ_INT(pkm4_shuffle_index(pid), sidx % 24);
        uint8_t e[PKM4_BOX_SIZE];
        pkm4_encrypt(&m, e, PKM4_BOX_SIZE);
        pkm4 d;
        CHECK(pkm4_decrypt(e, PKM4_BOX_SIZE, &d) == SAVE4_OK);
        CHECK(memcmp(d.data, m.data, PKM4_BOX_SIZE) == 0);
        CHECK(!d.party);
        if (sidx == 0 || sidx == 23) {
            /* undo the XOR stream by hand and find block A (species) */
            uint32_t seed = r16(e + 6);
            uint8_t plain[128];
            for (int i = 0; i < 128; i += 2) {
                seed = seed * 1103515245u + 24691u;
                uint16_t w = (uint16_t)(r16(e + 8 + i) ^ (seed >> 16));
                plain[i] = (uint8_t)w;
                plain[i + 1] = (uint8_t)(w >> 8);
            }
            int slot_a = sidx == 0 ? 0 : 3; /* case 0: ABCD; case 23: DCBA */
            CHECK_EQ_INT(r16(plain + slot_a * 32), 387);
        }
    }

    /* Corruption is detected. */
    enc[20] ^= 0x40;
    CHECK(pkm4_decrypt(enc, PKM4_PARTY_SIZE, &back) == SAVE4_ERR_PKM_CHECKSUM);

    /* Empty slot as BoxPokemon_Init writes it. */
    pkm4 empty;
    memset(&empty, 0, sizeof(empty));
    pkm4_encrypt(&empty, enc, PKM4_BOX_SIZE);
    CHECK(pkm4_decrypt(enc, PKM4_BOX_SIZE, &back) == SAVE4_OK);
    CHECK(pkm4_is_empty(&back));

    /* Nickname limits: 10 characters max (MON_NAME_LEN). */
    CHECK(pkm4_set_nickname(&mon, "ABCDEFGHIJ", 1) == SAVE4_OK);
    CHECK(pkm4_set_nickname(&mon, "ABCDEFGHIJK", 1) == SAVE4_ERR_RANGE);
    pkm4_info_get(&mon, &info);
    CHECK_EQ_STR(info.nickname, "ABCDEFGHIJ");
    CHECK(info.has_nickname);

    /* HG/SS bytes D/P/Pt leave unused (pokeheartgold
     * include/pokemon_types_def.h): block B 0x19 shiny leaves (canonical
     * 0x41), block D 0x1E HGSS_Pokeball (0x86) and 0x1F mood (0x87). Every
     * setter except pkm4_set_ball_hgss keeps them, through encrypt/decrypt. */
    {
        pkm4 h;
        synth_make_mon(&h, 152, 5, 0x0BEEF123u, "CHIKORITA", 33, 1);
        pkm4_set_origin_game(&h, 7); /* VERSION_HEARTGOLD */
        CHECK(pkm4_set_ball_hgss(&h, 494) == SAVE4_OK); /* Lure Ball */
        CHECK_EQ_INT(h.data[0x86], 19);                 /* BALL_LURE */
        CHECK_EQ_INT(h.data[0x83], 4);                  /* BALL_POKE */
        h.data[0x41] = 0x25;                            /* shiny leaves */
        h.data[0x87] = 0xF6;                            /* mood -10 */
        pkm4_info_get(&h, &info);
        CHECK_EQ_INT(info.ball, 19);
        pkm4_set_pid(&h, 0x0BEEF124u);
        pkm4_set_species(&h, 153);
        pkm4_set_held_item(&h, 234);
        pkm4_set_ot_ids(&h, 1, 2);
        pkm4_set_exp(&h, 999);
        pkm4_set_friendship(&h, 1);
        pkm4_set_ability(&h, 65);
        pkm4_set_language(&h, 2);
        for (int i = 0; i < 6; i++) {
            pkm4_set_ev(&h, i, 3);
            pkm4_set_iv(&h, i, 31);
        }
        pkm4_set_move(&h, 3, 75, 25, 1);
        pkm4_set_is_egg(&h, false);
        pkm4_set_gender_form(&h, 1, 0);
        CHECK(pkm4_set_nickname(&h, "Leafy", 1) == SAVE4_OK);
        CHECK(pkm4_set_ot_name(&h, "Ethan") == SAVE4_OK);
        pkm4_set_met(&h, 126, 9, 4, 0);
        uint16_t hs[6] = {30, 20, 20, 20, 20, 20};
        pkm4_set_party_stats(&h, 9, 30, hs, 0);
        uint8_t he[PKM4_PARTY_SIZE];
        CHECK(pkm4_encrypt(&h, he, PKM4_PARTY_SIZE) == SAVE4_OK);
        pkm4 hb;
        CHECK(pkm4_decrypt(he, PKM4_PARTY_SIZE, &hb) == SAVE4_OK);
        CHECK_EQ_INT(hb.data[0x41], 0x25);
        CHECK_EQ_INT(hb.data[0x86], 19);
        CHECK_EQ_INT(hb.data[0x87], 0xF6);
        pkm4_info_get(&hb, &info);
        CHECK_EQ_INT(info.ball, 19); /* HG/SS origin: the HGSS ball */
        pkm4_set_origin_game(&hb, 12);
        pkm4_info_get(&hb, &info);
        CHECK_EQ_INT(info.ball, 4); /* any other origin: the D/P/Pt ball */
        CHECK(pkm4_set_ball_hgss(&hb, 16) == SAVE4_OK); /* Cherish Ball: both bytes */
        CHECK(hb.data[0x83] == 16 && hb.data[0x86] == 16);
        CHECK(pkm4_set_ball_hgss(&hb, 0) == SAVE4_ERR_RANGE);
        CHECK(pkm4_set_ball_hgss(&hb, 500) == SAVE4_ERR_RANGE);
    }
    CHECK(pkm4_decrypt(enc, 100, &back) == SAVE4_ERR_ARG);
    /* Stat formula: Bulbapedia's worked example, a level 78 Adamant Garchomp
     * (base 108/130/95/102/80/85 as HP Atk Def Spe SpA SpD, IVs 24/12/30/5/16/23,
     * EVs 74/190/91/23/48/84) has 289/278/193/171/135/171. */
    {
        const uint8_t base[6] = {108, 130, 95, 102, 80, 85};
        const uint8_t ivs[6] = {24, 12, 30, 5, 16, 23};
        const uint8_t evs[6] = {74, 190, 91, 23, 48, 84};
        uint16_t st[6];
        pkm4_calc_stats(base, ivs, evs, 78, 3 /* Adamant */, false, st);
        CHECK_EQ_INT(st[0], 289);
        CHECK_EQ_INT(st[1], 278);
        CHECK_EQ_INT(st[2], 193);
        CHECK_EQ_INT(st[3], 171);
        CHECK_EQ_INT(st[4], 135);
        CHECK_EQ_INT(st[5], 171);
        pkm4_calc_stats(base, ivs, evs, 78, 0 /* Hardy: neutral */, true, st);
        CHECK_EQ_INT(st[0], 1); /* Shedinja */
        CHECK_EQ_INT(st[1], 253);
        CHECK_EQ_INT(st[4], 151);
    }
    return TEST_RESULT();
}
