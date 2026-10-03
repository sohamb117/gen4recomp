/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Offline tests: charmap, message bank crypto, NARC and FNT/FAT parsing on
 * hand-built data (the ROM-backed checks live in test_rom_text.c). */
#include <stdint.h>
#include <stdlib.h>

#include "ndsdata/ndsdata.h"
#include "testutil.h"

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

/* Message bank writer following pokeplatinum tools/msgenc
 * (MsgAlloc::encrypt, EncryptU16String; entries are 1-based in the key). */
static size_t build_bank(uint8_t *out, uint16_t seed, const uint16_t *const *strs, const size_t *lens, int count)
{
    w16(out, (uint16_t)count);
    w16(out + 2, seed);
    size_t off = 4 + (size_t)count * 8;
    for (int i = 0; i < count; i++) {
        uint32_t k = (765u * (uint32_t)(i + 1) * seed) & 0xFFFF;
        k |= k << 16;
        w32(out + 4 + i * 8, (uint32_t)off ^ k);
        w32(out + 8 + i * 8, (uint32_t)lens[i] ^ k);
        uint16_t key = (uint16_t)((i + 1) * 596947);
        for (size_t j = 0; j < lens[i]; j++) {
            w16(out + off, strs[i][j] ^ key);
            key = (uint16_t)(key + 18749);
            off += 2;
        }
    }
    return off;
}

struct membuf {
    const uint8_t *p;
    size_t n;
};
static int mem_read(void *user, uint64_t off, void *dst, size_t len)
{
    struct membuf *m = user;
    if (off > m->n || len > m->n - off)
        return -1;
    memcpy(dst, m->p + off, len);
    return 0;
}

static size_t build_narc(uint8_t *out, const char *const *files, int count)
{
    size_t btaf = 12 + (size_t)count * 8;
    size_t btnf = 16;
    size_t data = 0;
    for (int i = 0; i < count; i++)
        data += (strlen(files[i]) + 3) & ~(size_t)3;
    size_t gmif = 8 + data;
    size_t total = 16 + btaf + btnf + gmif;
    memset(out, 0, total);
    memcpy(out, "NARC", 4);
    w16(out + 4, 0xFFFE);
    w16(out + 6, 0x0100);
    w32(out + 8, (uint32_t)total);
    w16(out + 12, 0x10);
    w16(out + 14, 3);
    uint8_t *p = out + 16;
    memcpy(p, "BTAF", 4);
    w32(p + 4, (uint32_t)btaf);
    w16(p + 8, (uint16_t)count);
    uint8_t *g = out + 16 + btaf + btnf;
    size_t o = 0;
    for (int i = 0; i < count; i++) {
        size_t l = strlen(files[i]);
        w32(p + 12 + i * 8, (uint32_t)o);
        w32(p + 16 + i * 8, (uint32_t)(o + l));
        memcpy(g + 8 + o, files[i], l);
        o += (l + 3) & ~(size_t)3;
    }
    p = out + 16 + btaf;
    memcpy(p, "BTNF", 4);
    w32(p + 4, (uint32_t)btnf);
    w32(p + 8, 4);
    w16(p + 12, 0);
    w16(p + 14, 1);
    memcpy(g, "GMIF", 4);
    w32(g + 4, (uint32_t)gmif);
    return total;
}

int main(void)
{
    /* ---- charmap */
    uint16_t codes[16];
    size_t n = 0;
    CHECK(g4_text_encode("Aa0é ", codes, 16, &n) == ND_OK);
    CHECK_EQ_INT(n, 5);
    CHECK_EQ_INT(codes[0], 0x012B); /* charmap.txt: 012B=A */
    CHECK_EQ_INT(codes[1], 0x0145); /* 0145=a */
    CHECK_EQ_INT(codes[2], 0x0121); /* 0121=0 */
    CHECK_EQ_INT(codes[3], 0x0188); /* 0188=é */
    CHECK_EQ_INT(codes[4], 0x01DE); /* 01DE=' ' */
    CHECK_EQ_INT(codes[5], 0xFFFF);
    char buf[128];
    CHECK_EQ_INT(g4_text_decode(codes, 16, buf, sizeof(buf)), strlen("Aa0é "));
    CHECK_EQ_STR(buf, "Aa0é ");
    CHECK(g4_text_encode("TOOLONGNAME", codes, 8, &n) == ND_ERR_RANGE);
    CHECK(g4_text_encode("\x01", codes, 8, &n) == ND_ERR_FORMAT);
    /* truncation reports full length */
    CHECK_EQ_INT(g4_text_decode(codes, 0, buf, sizeof(buf)), 0);
    uint16_t lucas[] = {0x0136, 0x0159, 0x0147, 0x0145, 0x0157, 0xFFFF};
    CHECK_EQ_INT(g4_text_decode(lucas, 6, buf, 3), 5);
    CHECK_EQ_STR(buf, "Lu");
    /* commands render like msgenc */
    uint16_t cmd[] = {0x012B, 0xFFFE, 0x0100, 0x0001, 0x0000, 0xE000, 0xFFFE, 0x0200, 0x0000, 0xFFFF};
    g4_text_decode(cmd, sizeof(cmd) / 2, buf, sizeof(buf));
    CHECK_EQ_STR(buf, "A{STRVAR_1 0, 0}\n{YESNO}");

    /* ---- message bank */
    uint16_t s0[] = {0x012B, 0x0145, 0xFFFF};
    uint16_t s1[] = {0x0121, 0x0122, 0x0123, 0xFFFF};
    uint16_t s2[] = {0xFFFF};
    const uint16_t *strs[] = {s0, s1, s2};
    size_t lens[] = {3, 4, 1};
    uint8_t bankbuf[256];
    size_t bl = build_bank(bankbuf, 0x58E6, strs, lens, 3);
    nd_msgbank bank;
    CHECK(nd_msgbank_parse(&bank, bankbuf, bl) == ND_OK);
    CHECK_EQ_INT(bank.count, 3);
    char *t = NULL;
    CHECK(nd_msgbank_get_utf8(&bank, 0, &t) == ND_OK);
    CHECK_EQ_STR(t, "Aa");
    free(t);
    CHECK(nd_msgbank_get_utf8(&bank, 1, &t) == ND_OK);
    CHECK_EQ_STR(t, "012");
    free(t);
    CHECK(nd_msgbank_get_utf8(&bank, 2, &t) == ND_OK);
    CHECK_EQ_STR(t, "");
    free(t);
    CHECK(nd_msgbank_get_utf8(&bank, 3, &t) == ND_ERR_NOT_FOUND);
    CHECK(nd_msgbank_parse(&bank, bankbuf, 10) == ND_ERR_FORMAT);

    /* ---- NARC */
    static uint8_t narcbuf[512];
    const char *files[] = {"hello", "narc!", "x"};
    size_t nl = build_narc(narcbuf, files, 3);
    nd_narc narc;
    CHECK(nd_narc_parse(&narc, narcbuf, nl) == ND_OK);
    CHECK_EQ_INT(narc.count, 3);
    const uint8_t *mp;
    size_t ml;
    CHECK(nd_narc_member(&narc, 1, &mp, &ml) == ND_OK && ml == 5 && !memcmp(mp, "narc!", 5));
    CHECK(nd_narc_member(&narc, 3, &mp, &ml) == ND_ERR_NOT_FOUND);
    CHECK(nd_narc_parse(&narc, narcbuf, 20) == ND_ERR_FORMAT);

    /* ---- ROM FNT/FAT on a hand-built image */
    static uint8_t rom[0x1000];
    memset(rom, 0, sizeof(rom));
    memcpy(rom, "POKEMON PL\0\0", 12);
    memcpy(rom + 0x0C, "CPUE", 4);
    uint8_t *fnt = rom + 0x200;
    /* main table: root (sub 0x10, first file 0, 2 dirs), msgdata (sub 0x28, first 1, parent F000) */
    w32(fnt, 0x10);
    w16(fnt + 4, 0);
    w16(fnt + 6, 2);
    w32(fnt + 8, 0x28);
    w16(fnt + 12, 1);
    w16(fnt + 14, 0xF000);
    uint8_t *sub = fnt + 0x10;
    *sub++ = 7;
    memcpy(sub, "top.txt", 7);
    sub += 7;
    *sub++ = 0x80 | 7;
    memcpy(sub, "msgdata", 7);
    sub += 7;
    w16(sub, 0xF001);
    sub += 2;
    *sub++ = 0;
    sub = fnt + 0x28;
    *sub++ = 5;
    memcpy(sub, "a.bin", 5);
    sub += 5;
    *sub++ = 6;
    memcpy(sub, "b.narc", 6);
    sub += 6;
    *sub++ = 0;
    uint32_t fnt_size = (uint32_t)(sub - fnt);
    uint8_t *fat = rom + 0x300;
    memcpy(rom + 0x400, "top", 3);
    memcpy(rom + 0x410, "abin", 4);
    memcpy(rom + 0x500, narcbuf, nl);
    w32(fat + 0, 0x400);
    w32(fat + 4, 0x403);
    w32(fat + 8, 0x410);
    w32(fat + 12, 0x414);
    w32(fat + 16, 0x500);
    w32(fat + 20, (uint32_t)(0x500 + nl));
    w32(rom + 0x40, 0x200);
    w32(rom + 0x44, fnt_size);
    w32(rom + 0x48, 0x300);
    w32(rom + 0x4C, 24);

    struct membuf mb = {rom, sizeof(rom)};
    nd_rom r;
    CHECK(nd_rom_open(&r, mem_read, &mb, sizeof(rom)) == ND_OK);
    CHECK_EQ_STR(r.gamecode, "CPUE");
    CHECK(r.game == ND_GAME_PLATINUM);
    CHECK_EQ_INT(r.file_count, 3);
    uint32_t id = 99;
    CHECK(nd_rom_find(&r, "top.txt", &id) == ND_OK && id == 0);
    CHECK(nd_rom_find(&r, "/msgdata/a.bin", &id) == ND_OK && id == 1);
    CHECK(nd_rom_find(&r, "msgdata/b.narc", &id) == ND_OK && id == 2);
    CHECK(nd_rom_find(&r, "msgdata/c.bin", &id) == ND_ERR_NOT_FOUND);
    CHECK(nd_rom_find(&r, "msgdata", &id) == ND_ERR_NOT_FOUND);
    CHECK(nd_rom_find(&r, "nope/a.bin", &id) == ND_ERR_NOT_FOUND);
    uint8_t *fb = NULL;
    size_t fl = 0;
    CHECK(nd_rom_load_path(&r, "msgdata/a.bin", &fb, &fl) == ND_OK && fl == 4 && !memcmp(fb, "abin", 4));
    free(fb);
    CHECK(nd_rom_load_narc_member(&r, 2, 0, &fb, &fl) == ND_OK && fl == 5 && !memcmp(fb, "hello", 5));
    free(fb);
    CHECK(nd_rom_load_narc_member(&r, 2, 2, &fb, &fl) == ND_OK && fl == 1 && fb[0] == 'x');
    free(fb);
    CHECK(nd_rom_load_narc_member(&r, 2, 3, &fb, &fl) == ND_ERR_NOT_FOUND);
    CHECK(nd_rom_load_narc_member(&r, 1, 0, &fb, &fl) == ND_ERR_FORMAT);
    nd_rom_close(&r);

    return TEST_RESULT();
}
