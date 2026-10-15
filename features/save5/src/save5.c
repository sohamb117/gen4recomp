/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Black / White save data. Block table: Project Pokémon "BW Save Structure"
 * (https://projectpokemon.org/home/docs/gen-5/bw-save-structure-r73/), the
 * same as PKHeX SaveBlockAccessor5BW; footers and the checksum block were
 * checked against real Black and White saves. Field offsets inside blocks
 * follow PKHeX (PlayerData5, Misc5, PlayerPosition5, BoxLayout5,
 * EventWork5BW, Zukan5, MysteryBlock5, PlayerBag5BW).
 */
#include "save5/save5.h"

#include <stdlib.h>
#include <string.h>

static const struct {
    uint32_t offset, size;
} kBlocks[SAVE5_DATA_BLOCKS] = {
    {0x00000, 0x03E0}, /* 0 box names, wallpapers, current box */
    {0x00400, 0x0FF0}, {0x01400, 0x0FF0}, {0x02400, 0x0FF0}, {0x03400, 0x0FF0}, {0x04400, 0x0FF0},
    {0x05400, 0x0FF0}, {0x06400, 0x0FF0}, {0x07400, 0x0FF0}, {0x08400, 0x0FF0}, {0x09400, 0x0FF0},
    {0x0A400, 0x0FF0}, {0x0B400, 0x0FF0}, {0x0C400, 0x0FF0}, {0x0D400, 0x0FF0}, {0x0E400, 0x0FF0},
    {0x0F400, 0x0FF0}, {0x10400, 0x0FF0}, {0x11400, 0x0FF0}, {0x12400, 0x0FF0}, {0x13400, 0x0FF0},
    {0x14400, 0x0FF0}, {0x15400, 0x0FF0}, {0x16400, 0x0FF0}, {0x17400, 0x0FF0}, /* 1-24 boxes */
    {0x18400, 0x09C0}, /* 25 bag */
    {0x18E00, 0x0534}, /* 26 party */
    {0x19400, 0x0068}, /* 27 trainer */
    {0x19500, 0x009C}, /* 28 position */
    {0x19600, 0x1338}, {0x1AA00, 0x07C4}, {0x1B200, 0x0D54}, {0x1C000, 0x002C}, {0x1C100, 0x0658},
    {0x1C800, 0x0A94}, /* 34 Mystery Gift */
    {0x1D300, 0x01AC}, {0x1D500, 0x03EC}, {0x1D900, 0x005C}, {0x1DA00, 0x01E0}, {0x1DC00, 0x00A8},
    {0x1DD00, 0x0460}, {0x1E200, 0x1400}, {0x1F700, 0x02A4}, {0x1FA00, 0x02DC}, {0x1FD00, 0x034C},
    {0x20100, 0x03EC}, /* 45 event vars and flags */
    {0x20500, 0x00F8}, {0x20600, 0x02FC}, {0x20900, 0x0094}, {0x20A00, 0x035C}, {0x20E00, 0x01CC},
    {0x21000, 0x0168},
    {0x21200, 0x00EC}, /* 52 money, badges */
    {0x21300, 0x01B0}, {0x21500, 0x001C},
    {0x21600, 0x04D4}, /* 55 Pokédex */
    {0x21B00, 0x0034}, {0x21C00, 0x003C}, {0x21D00, 0x01AC}, {0x21F00, 0x0B90}, {0x22B00, 0x009C},
    {0x22C00, 0x0850}, {0x23500, 0x0028}, {0x23600, 0x0284}, {0x23900, 0x0010}, {0x23A00, 0x005C},
    {0x23B00, 0x016C}, {0x23D00, 0x0040}, {0x23E00, 0x00FC},
};

/* Block contents as PKHeX's SaveBlockAccessor5BW comments name them. */
const char *save5_block_name(int block)
{
    static const char *const boxes[24] = {"box 1",  "box 2",  "box 3",  "box 4",  "box 5",  "box 6",
                                          "box 7",  "box 8",  "box 9",  "box 10", "box 11", "box 12",
                                          "box 13", "box 14", "box 15", "box 16", "box 17", "box 18",
                                          "box 19", "box 20", "box 21", "box 22", "box 23", "box 24"};
    switch (block) {
    case 0: return "box names";
    case 25: return "bag";
    case 26: return "party";
    case 27: return "trainer";
    case 28: return "position";
    case 29: return "unity tower and survey";
    case 30: return "pal pad player";
    case 31: return "pal pad friends";
    case 32: return "skin info";
    case 33: return "gym badge data";
    case 34: return "mystery gift";
    case 35: return "dream world";
    case 36: return "chatter";
    case 37: return "adventure info";
    case 38: return "trainer card records";
    case 40: return "mail";
    case 41: return "overworld state";
    case 42: return "musical";
    case 43: return "white forest / black city";
    case 44: return "ir";
    case 45: return "event work";
    case 46: return "gts";
    case 47: return "regulation tournament";
    case 48: return "gimmick";
    case 49: return "battle box";
    case 50: return "daycare";
    case 51: return "strength boulders";
    case 52: return "money and badges";
    case 53: return "entralink";
    case 55: return "pokedex";
    case 56: return "encounter (swarm, repel)";
    case 57: return "battle subway play";
    case 58: return "battle subway score";
    case 59: return "battle subway wi-fi";
    case 60: return "online records";
    case 61: return "entralink forest";
    case 63: return "answered questions";
    case 64: return "unity tower";
    case 65: return "battle institute";
    default: return block >= 1 && block <= 24 ? boxes[block - 1] : block >= 0 && block < SAVE5_DATA_BLOCKS ? "unknown" : NULL;
    }
}

/* Checksum block footer, after the mirror table. */
#define FOOTER_OFS (SAVE5_CHECKSUM_BLOCK + SAVE5_CHECKSUM_TABLE) /* 0x23F8C */
#define FOOTER_COUNTER (FOOTER_OFS + 0x0)
#define FOOTER_SIZE (FOOTER_OFS + 0x4)
#define FOOTER_MAGIC (FOOTER_OFS + 0x8)
#define FOOTER_CRC (FOOTER_OFS + 0xE)

/* Trainer block (PlayerData5). */
#define TR_NAME 0x04
#define TR_TID 0x14
#define TR_SID 0x16
#define TR_COUNTRY 0x1C
#define TR_REGION 0x1D
#define TR_LANGUAGE 0x1E
#define TR_VERSION 0x1F
#define TR_GENDER 0x21
#define TR_HOURS 0x24
#define TR_MINUTES 0x26
#define TR_SECONDS 0x27
#define TR_LAST_SAVED 0x28
#define TR_NAME_UNITS 8
/* Misc block. */
#define MISC_MONEY 0x00
#define MISC_BADGES 0x04
/* Position block (fx32 x/y/z, 16 fraction bits). */
#define POS_MAP 0x80
#define POS_X 0x84
#define POS_Y 0x88
#define POS_Z 0x8C
/* Box names block. */
#define BOXN_CURRENT 0x00
#define BOXN_NAME(b) (0x04 + 0x28 * (b))
#define BOXN_NAME_UNITS 10
/* Party block. */
#define PARTY_COUNT 0x04
#define PARTY_MON(i) (0x08 + PKM5_PARTY_SIZE * (i))
/* Event block. */
#define EV_FLAGS (SAVE5_NUM_VARS * 2)
/* Pokédex block. */
#define DEX_PACKED 0x04
#define DEX_CAUGHT 0x08
#define DEX_REGION 0x54
#define DEX_SEEN 0x5C
#define DEX_DISPLAYED 0x1AC
/* Mystery Gift block. */
#define MG_DATA 0xA90
#define MG_SEED 0xA90
#define MG_CARDS 0x100
/* Wonder Card (.pgf). */
#define PGF_SPECIES 0x1A
#define PGF_TITLE 0x60
#define PGF_ID 0xB0
#define PGF_TYPE 0xB3

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

uint16_t save5_crc16(const void *data, size_t len)
{
    const uint8_t *p = data;
    uint16_t crc = 0xFFFF;
    while (len--) {
        crc ^= (uint16_t)(*p++ << 8);
        for (int i = 0; i < 8; i++)
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}

const char *save5_status_str(save5_status st)
{
    switch (st) {
    case SAVE5_OK: return "ok";
    case SAVE5_ERR_ARG: return "invalid argument";
    case SAVE5_ERR_SIZE: return "save image smaller than 512 KiB";
    case SAVE5_ERR_EMPTY: return "no save data (blank image)";
    case SAVE5_ERR_UNKNOWN_GAME: return "not a Pokemon Black/White save";
    case SAVE5_ERR_CHECKSUM: return "save checksum failure (no valid copy)";
    case SAVE5_ERR_RANGE: return "value out of range";
    case SAVE5_ERR_NOMEM: return "out of memory";
    case SAVE5_ERR_ENCODE: return "text not representable in the game's character set";
    case SAVE5_ERR_PKM_CHECKSUM: return "Pokemon data checksum mismatch";
    case SAVE5_ERR_NOSPACE: return "no free slot";
    case SAVE5_ERR_FORMAT: return "malformed data";
    }
    return "unknown error";
}

const char *save5_game_name(save5_game g)
{
    return g == SAVE5_GAME_BLACK ? "black" : g == SAVE5_GAME_WHITE ? "white" : "unknown";
}

/* ------------------------------------------------------------ blocks */

static uint32_t copy_base(int copy) { return copy ? SAVE5_COPY_OFFSET : 0; }

static uint16_t table_crc(const uint8_t *img, int copy)
{
    return save5_crc16(img + copy_base(copy) + SAVE5_CHECKSUM_BLOCK, SAVE5_CHECKSUM_TABLE);
}

/* Fill copy and block validity from the current bytes. */
static void scan(save5 *s)
{
    const uint8_t *img = s->img;
    for (int c = 0; c < 2; c++) {
        const uint32_t base = copy_base(c);
        save5_copy_state *cs = &s->copies[c];
        cs->footer_ok = g32(img + base + FOOTER_MAGIC) == SAVE5_MAGIC && g32(img + base + FOOTER_SIZE) == SAVE5_COPY_USED;
        cs->save_counter = g32(img + base + FOOTER_COUNTER);
        cs->stored_crc = g16(img + base + FOOTER_CRC);
        cs->computed_crc = table_crc(img, c);
        cs->table_ok = cs->stored_crc == cs->computed_crc;
        cs->bad_blocks = 0;
        for (int b = 0; b < SAVE5_DATA_BLOCKS; b++) {
            save5_block_state *bs = &s->blocks[b];
            const uint8_t *blk = img + base + kBlocks[b].offset;
            bs->offset = kBlocks[b].offset;
            bs->size = kBlocks[b].size;
            bs->computed_crc[c] = save5_crc16(blk, bs->size);
            bs->counter[c] = g16(blk + bs->size);
            bs->stored_crc[c] = g16(blk + bs->size + 2);
            bs->mirror_crc[c] = g16(img + base + SAVE5_CHECKSUM_BLOCK + 2 * b);
            bs->valid[c] = bs->computed_crc[c] == bs->stored_crc[c] && bs->computed_crc[c] == bs->mirror_crc[c];
            cs->bad_blocks += !bs->valid[c];
        }
        cs->valid = cs->footer_ok && cs->table_ok && cs->bad_blocks == 0;
    }
}

static bool uniform(const uint8_t *p, size_t n)
{
    for (size_t i = 1; i < n; i++)
        if (p[i] != p[0])
            return false;
    return p[0] == 0x00 || p[0] == 0xFF;
}

static save5_status select_copy(save5 *s)
{
    scan(s);
    const save5_copy_state *c0 = &s->copies[0], *c1 = &s->copies[1];
    if (!c0->footer_ok && !c1->footer_ok) {
        s->active = 0;
        return uniform(s->img, 2 * SAVE5_COPY_OFFSET) ? SAVE5_ERR_EMPTY : SAVE5_ERR_UNKNOWN_GAME;
    }
    save5_status st = SAVE5_OK;
    if (c0->valid && c1->valid) {
        s->active = c1->save_counter > c0->save_counter ? 1 : 0;
        s->load_result = SAVE5_LOAD_OK;
    } else if (c0->valid || c1->valid) {
        s->active = c0->valid ? 0 : 1;
        s->load_result = SAVE5_LOAD_RECOVERED;
    } else {
        /* report against the copy that is closer to valid */
        s->active = (!c0->footer_ok || (c1->footer_ok && c1->bad_blocks < c0->bad_blocks)) ? 1 : 0;
        st = SAVE5_ERR_CHECKSUM;
    }
    for (int b = 0; b < SAVE5_DATA_BLOCKS; b++) {
        save5_block_state *bs = &s->blocks[b];
        bs->synced = bs->valid[0] && bs->valid[1] &&
                     memcmp(s->img + bs->offset, s->img + SAVE5_COPY_OFFSET + bs->offset, bs->size + 4) == 0;
    }
    if (st != SAVE5_OK)
        return st;
    const uint8_t ver = s->img[copy_base(s->active) + kBlocks[SAVE5_BLK_TRAINER].offset + TR_VERSION];
    s->game = ver == 21 ? SAVE5_GAME_BLACK : ver == 20 ? SAVE5_GAME_WHITE : SAVE5_GAME_UNKNOWN;
    return s->game == SAVE5_GAME_UNKNOWN ? SAVE5_ERR_UNKNOWN_GAME : SAVE5_OK;
}

save5_status save5_load(save5 *s, const uint8_t *data, size_t len)
{
    if (!s || !data)
        return SAVE5_ERR_ARG;
    memset(s, 0, sizeof *s);
    if (len < SAVE5_IMAGE_SIZE)
        return SAVE5_ERR_SIZE;
    s->img = malloc(len);
    if (!s->img)
        return SAVE5_ERR_NOMEM;
    memcpy(s->img, data, len);
    s->len = len;
    return select_copy(s);
}

save5_status save5_clone(const save5 *src, save5 *dst)
{
    if (!src || !dst || !src->img)
        return SAVE5_ERR_ARG;
    *dst = *src;
    dst->img = malloc(src->len);
    if (!dst->img)
        return SAVE5_ERR_NOMEM;
    memcpy(dst->img, src->img, src->len);
    return SAVE5_OK;
}

void save5_free(save5 *s)
{
    if (!s)
        return;
    free(s->img);
    memset(s, 0, sizeof *s);
}

const uint8_t *save5_image(const save5 *s, size_t *len)
{
    if (len)
        *len = s ? s->len : 0;
    return s ? s->img : NULL;
}

save5_status save5_revalidate(save5 *s)
{
    if (!s || !s->img)
        return SAVE5_ERR_ARG;
    return select_copy(s);
}

uint32_t save5_block_base(const save5 *s, int block)
{
    return copy_base(s->active) + kBlocks[block].offset;
}

static void commit_copy(uint8_t *img, int copy, int block)
{
    const uint32_t base = copy_base(copy);
    uint8_t *blk = img + base + kBlocks[block].offset;
    const uint16_t crc = save5_crc16(blk, kBlocks[block].size);
    s16(blk + kBlocks[block].size + 2, crc);
    s16(img + base + SAVE5_CHECKSUM_BLOCK + 2 * block, crc);
    s16(img + base + FOOTER_CRC, table_crc(img, copy));
}

void save5_commit_block(save5 *s, int block)
{
    if (!s || !s->img || block < 0 || block >= SAVE5_DATA_BLOCKS)
        return;
    commit_copy(s->img, s->active, block);
    if (s->blocks[block].synced) {
        const uint32_t ofs = kBlocks[block].offset, n = kBlocks[block].size + 4;
        memcpy(s->img + copy_base(!s->active) + ofs, s->img + copy_base(s->active) + ofs, n);
        commit_copy(s->img, !s->active, block);
    }
    scan(s);
}

void save5_fix_all_checksums(uint8_t *img)
{
    for (int c = 0; c < 2; c++)
        for (int b = 0; b < SAVE5_DATA_BLOCKS; b++)
            commit_copy(img, c, b);
}

static uint8_t *blk(const save5 *s, int block) { return s->img + save5_block_base(s, block); }

/* -------------------------------------------------------------- text */

static size_t put_utf8(char *out, size_t cap, size_t n, uint32_t cp)
{
    char buf[4];
    size_t k;
    if (cp < 0x80) {
        buf[0] = (char)cp;
        k = 1;
    } else if (cp < 0x800) {
        buf[0] = (char)(0xC0 | (cp >> 6));
        buf[1] = (char)(0x80 | (cp & 0x3F));
        k = 2;
    } else {
        buf[0] = (char)(0xE0 | (cp >> 12));
        buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[2] = (char)(0x80 | (cp & 0x3F));
        k = 3;
    }
    for (size_t i = 0; i < k; i++)
        if (n + i + 1 < cap)
            out[n + i] = buf[i];
    return n + k;
}

size_t save5_text_decode(const uint8_t *src, size_t units, char *out, size_t cap)
{
    size_t n = 0;
    for (size_t i = 0; i < units; i++) {
        uint32_t c = g16(src + 2 * i);
        if (c == 0xFFFF || c == 0)
            break;
        if (c == 0x246D)
            c = 0x2642; /* ♂ */
        else if (c == 0x246E)
            c = 0x2640; /* ♀ */
        else if (c >= 0xD800 && c < 0xE000)
            c = 0xFFFD;
        n = put_utf8(out, cap, n, c);
    }
    if (cap)
        out[n < cap ? n : cap - 1] = '\0';
    return n;
}

save5_status save5_text_encode(const char *utf8, uint8_t *dst, size_t units)
{
    if (!utf8 || !dst || units == 0)
        return SAVE5_ERR_ARG;
    uint16_t tmp[64];
    size_t n = 0;
    const unsigned char *p = (const unsigned char *)utf8;
    while (*p) {
        uint32_t cp;
        int extra;
        if (*p < 0x80) {
            cp = *p;
            extra = 0;
        } else if ((*p & 0xE0) == 0xC0) {
            cp = *p & 0x1F;
            extra = 1;
        } else if ((*p & 0xF0) == 0xE0) {
            cp = *p & 0x0F;
            extra = 2;
        } else {
            return SAVE5_ERR_ENCODE; /* invalid lead byte, or outside the BMP */
        }
        p++;
        for (int i = 0; i < extra; i++, p++) {
            if ((*p & 0xC0) != 0x80)
                return SAVE5_ERR_ENCODE;
            cp = cp << 6 | (*p & 0x3F);
        }
        if (cp < 0x20 || (cp >= 0xD800 && cp < 0xE000) || cp >= 0xFFFE)
            return SAVE5_ERR_ENCODE;
        if (cp == 0x2642)
            cp = 0x246D;
        else if (cp == 0x2640)
            cp = 0x246E;
        if (n + 1 >= units || n >= sizeof tmp / sizeof tmp[0] - 1)
            return SAVE5_ERR_RANGE;
        tmp[n++] = (uint16_t)cp;
    }
    for (size_t i = 0; i < units; i++)
        s16(dst + 2 * i, i < n ? tmp[i] : i == n ? 0xFFFF : 0);
    return SAVE5_OK;
}

/* ----------------------------------------------------------- trainer */

save5_status save5_get_trainer(const save5 *s, save5_trainer *t)
{
    if (!s || !s->img || !t)
        return SAVE5_ERR_ARG;
    memset(t, 0, sizeof *t);
    const uint8_t *tr = blk(s, SAVE5_BLK_TRAINER), *misc = blk(s, SAVE5_BLK_MISC);
    save5_text_decode(tr + TR_NAME, TR_NAME_UNITS, t->name, sizeof t->name);
    for (int i = 0; i < TR_NAME_UNITS; i++)
        t->name_raw[i] = g16(tr + TR_NAME + 2 * i);
    t->tid = g16(tr + TR_TID);
    t->sid = g16(tr + TR_SID);
    t->country = tr[TR_COUNTRY];
    t->region = tr[TR_REGION];
    t->language = tr[TR_LANGUAGE];
    t->version = tr[TR_VERSION];
    t->gender = tr[TR_GENDER];
    t->play_hours = g16(tr + TR_HOURS);
    t->play_minutes = tr[TR_MINUTES];
    t->play_seconds = tr[TR_SECONDS];
    t->money = g32(misc + MISC_MONEY);
    t->badges = misc[MISC_BADGES];
    return SAVE5_OK;
}

save5_status save5_set_money(save5 *s, uint32_t money)
{
    if (!s || !s->img)
        return SAVE5_ERR_ARG;
    if (money > SAVE5_MONEY_MAX)
        return SAVE5_ERR_RANGE;
    s32(blk(s, SAVE5_BLK_MISC) + MISC_MONEY, money);
    save5_commit_block(s, SAVE5_BLK_MISC);
    return SAVE5_OK;
}

save5_status save5_set_trainer_name(save5 *s, const char *utf8)
{
    if (!s || !s->img || !utf8 || !*utf8)
        return SAVE5_ERR_ARG;
    uint8_t buf[TR_NAME_UNITS * 2];
    save5_status st = save5_text_encode(utf8, buf, TR_NAME_UNITS);
    if (st != SAVE5_OK)
        return st;
    memcpy(blk(s, SAVE5_BLK_TRAINER) + TR_NAME, buf, sizeof buf);
    save5_commit_block(s, SAVE5_BLK_TRAINER);
    return SAVE5_OK;
}

save5_status save5_set_trainer_ids(save5 *s, uint16_t tid, uint16_t sid)
{
    if (!s || !s->img)
        return SAVE5_ERR_ARG;
    s16(blk(s, SAVE5_BLK_TRAINER) + TR_TID, tid);
    s16(blk(s, SAVE5_BLK_TRAINER) + TR_SID, sid);
    save5_commit_block(s, SAVE5_BLK_TRAINER);
    return SAVE5_OK;
}

save5_status save5_set_gender(save5 *s, uint8_t gender)
{
    if (!s || !s->img)
        return SAVE5_ERR_ARG;
    if (gender > 1)
        return SAVE5_ERR_RANGE;
    blk(s, SAVE5_BLK_TRAINER)[TR_GENDER] = gender;
    save5_commit_block(s, SAVE5_BLK_TRAINER);
    return SAVE5_OK;
}

save5_status save5_set_badges(save5 *s, uint8_t mask)
{
    if (!s || !s->img)
        return SAVE5_ERR_ARG;
    blk(s, SAVE5_BLK_MISC)[MISC_BADGES] = mask;
    save5_commit_block(s, SAVE5_BLK_MISC);
    return SAVE5_OK;
}

save5_status save5_set_play_time(save5 *s, uint16_t hours, uint8_t minutes, uint8_t seconds)
{
    if (!s || !s->img)
        return SAVE5_ERR_ARG;
    if (hours > 999 || minutes > 59 || seconds > 59)
        return SAVE5_ERR_RANGE;
    uint8_t *tr = blk(s, SAVE5_BLK_TRAINER);
    s16(tr + TR_HOURS, hours);
    tr[TR_MINUTES] = minutes;
    tr[TR_SECONDS] = seconds;
    save5_commit_block(s, SAVE5_BLK_TRAINER);
    return SAVE5_OK;
}

save5_status save5_get_location(const save5 *s, save5_location *loc)
{
    if (!s || !s->img || !loc)
        return SAVE5_ERR_ARG;
    const uint8_t *p = blk(s, SAVE5_BLK_POSITION);
    loc->map = g32(p + POS_MAP);
    loc->x = g16(p + POS_X + 2);
    loc->y = g16(p + POS_Y + 2);
    loc->z = g16(p + POS_Z + 2);
    return SAVE5_OK;
}

save5_status save5_set_location(save5 *s, const save5_location *loc)
{
    if (!s || !s->img || !loc)
        return SAVE5_ERR_ARG;
    uint8_t *p = blk(s, SAVE5_BLK_POSITION);
    /* fx32: the tile in the integer half, the tile's centre (8 of its 16
     * units) in the fraction, as an object's position is kept */
    s32(p + POS_MAP, loc->map);
    s32(p + POS_X, (uint32_t)loc->x << 16 | 0x8000u);
    s32(p + POS_Y, (uint32_t)loc->y << 16);
    s32(p + POS_Z, (uint32_t)loc->z << 16 | 0x8000u);
    save5_commit_block(s, SAVE5_BLK_POSITION);
    return SAVE5_OK;
}

save5_status save5_get_last_saved(const save5 *s, save5_game_time *t)
{
    if (!s || !s->img || !t)
        return SAVE5_ERR_ARG;
    const uint32_t v = g32(blk(s, SAVE5_BLK_TRAINER) + TR_LAST_SAVED);
    t->year = (uint16_t)(2000 + (v & 0x7F));
    t->month = (uint8_t)((v >> 7) & 0x0F);
    t->day = (uint8_t)((v >> 11) & 0x1F);
    t->hour = (uint8_t)((v >> 16) & 0x1F);
    t->minute = (uint8_t)((v >> 21) & 0x3F);
    return SAVE5_OK;
}

/* ----------------------------------------------------------- Pokémon */

uint8_t save5_party_count(const save5 *s)
{
    if (!s || !s->img)
        return 0;
    const uint8_t n = blk(s, SAVE5_BLK_PARTY)[PARTY_COUNT];
    return n > SAVE5_PARTY_MAX ? SAVE5_PARTY_MAX : n;
}

save5_status save5_get_party(const save5 *s, int slot, pkm5 *out)
{
    if (!s || !s->img || !out)
        return SAVE5_ERR_ARG;
    if (slot < 0 || slot >= SAVE5_PARTY_MAX)
        return SAVE5_ERR_RANGE;
    return pkm5_decrypt(blk(s, SAVE5_BLK_PARTY) + PARTY_MON(slot), PKM5_PARTY_SIZE, out);
}

save5_status save5_set_party(save5 *s, int slot, pkm5 *p)
{
    if (!s || !s->img || !p)
        return SAVE5_ERR_ARG;
    if (slot < 0 || slot >= SAVE5_PARTY_MAX)
        return SAVE5_ERR_RANGE;
    save5_status st = pkm5_encrypt(p, blk(s, SAVE5_BLK_PARTY) + PARTY_MON(slot), PKM5_PARTY_SIZE);
    if (st == SAVE5_OK)
        save5_commit_block(s, SAVE5_BLK_PARTY);
    return st;
}

save5_status save5_set_party_count(save5 *s, uint8_t count)
{
    if (!s || !s->img)
        return SAVE5_ERR_ARG;
    if (count > SAVE5_PARTY_MAX)
        return SAVE5_ERR_RANGE;
    blk(s, SAVE5_BLK_PARTY)[PARTY_COUNT] = count;
    save5_commit_block(s, SAVE5_BLK_PARTY);
    return SAVE5_OK;
}

static bool box_ok(int box, int slot)
{
    return box >= 0 && box < SAVE5_BOX_COUNT && slot >= 0 && slot < SAVE5_BOX_SLOTS;
}

save5_status save5_get_box_mon(const save5 *s, int box, int slot, pkm5 *out)
{
    if (!s || !s->img || !out)
        return SAVE5_ERR_ARG;
    if (!box_ok(box, slot))
        return SAVE5_ERR_RANGE;
    return pkm5_decrypt(blk(s, SAVE5_BLK_BOX1 + box) + PKM5_BOX_SIZE * slot, PKM5_BOX_SIZE, out);
}

save5_status save5_set_box_mon(save5 *s, int box, int slot, pkm5 *p)
{
    if (!s || !s->img || !p)
        return SAVE5_ERR_ARG;
    if (!box_ok(box, slot))
        return SAVE5_ERR_RANGE;
    save5_status st = pkm5_encrypt(p, blk(s, SAVE5_BLK_BOX1 + box) + PKM5_BOX_SIZE * slot, PKM5_BOX_SIZE);
    if (st == SAVE5_OK)
        save5_commit_block(s, SAVE5_BLK_BOX1 + box);
    return st;
}

save5_status save5_clear_box_mon(save5 *s, int box, int slot)
{
    if (!s || !s->img)
        return SAVE5_ERR_ARG;
    if (!box_ok(box, slot))
        return SAVE5_ERR_RANGE;
    /* an empty slot is an encrypted all-zero Pokémon, as the game stores it */
    pkm5 empty;
    memset(&empty, 0, sizeof empty);
    pkm5_encrypt(&empty, blk(s, SAVE5_BLK_BOX1 + box) + PKM5_BOX_SIZE * slot, PKM5_BOX_SIZE);
    save5_commit_block(s, SAVE5_BLK_BOX1 + box);
    return SAVE5_OK;
}

save5_status save5_get_box_name(const save5 *s, int box, char *utf8, size_t cap)
{
    if (!s || !s->img || !utf8 || !cap)
        return SAVE5_ERR_ARG;
    if (box < 0 || box >= SAVE5_BOX_COUNT)
        return SAVE5_ERR_RANGE;
    save5_text_decode(blk(s, SAVE5_BLK_BOX_NAMES) + BOXN_NAME(box), BOXN_NAME_UNITS, utf8, cap);
    return SAVE5_OK;
}

save5_status save5_set_box_name(save5 *s, int box, const char *utf8)
{
    if (!s || !s->img || !utf8 || !*utf8)
        return SAVE5_ERR_ARG;
    if (box < 0 || box >= SAVE5_BOX_COUNT)
        return SAVE5_ERR_RANGE;
    uint8_t buf[(SAVE5_BOX_NAME_MAX + 1) * 2];
    save5_status st = save5_text_encode(utf8, buf, SAVE5_BOX_NAME_MAX + 1);
    if (st != SAVE5_OK)
        return st;
    memcpy(blk(s, SAVE5_BLK_BOX_NAMES) + BOXN_NAME(box), buf, sizeof buf);
    save5_commit_block(s, SAVE5_BLK_BOX_NAMES);
    return SAVE5_OK;
}

uint32_t save5_current_box(const save5 *s)
{
    return s && s->img ? blk(s, SAVE5_BLK_BOX_NAMES)[BOXN_CURRENT] : 0;
}

/* --------------------------------------------------------------- bag */

static const struct {
    const char *name;
    uint16_t offset, capacity, max_qty;
} kPockets[SAVE5_POCKET_COUNT] = {
    {"items", 0x000, 310, 999},
    {"key_items", 0x4D8, 83, 1},
    {"tms_hms", 0x624, 109, 1},
    {"medicine", 0x7D8, 48, 999},
    {"berries", 0x898, 64, 999},
};

const char *save5_pocket_name(save5_pocket p)
{
    return (unsigned)p < SAVE5_POCKET_COUNT ? kPockets[p].name : NULL;
}

int save5_pocket_capacity(save5_pocket p)
{
    return (unsigned)p < SAVE5_POCKET_COUNT ? kPockets[p].capacity : 0;
}

uint16_t save5_pocket_max_qty(save5_pocket p)
{
    return (unsigned)p < SAVE5_POCKET_COUNT ? kPockets[p].max_qty : 0;
}

save5_status save5_get_bag_slot(const save5 *s, save5_pocket p, int slot, uint16_t *item, uint16_t *qty)
{
    if (!s || !s->img || !item || !qty)
        return SAVE5_ERR_ARG;
    if ((unsigned)p >= SAVE5_POCKET_COUNT || slot < 0 || slot >= kPockets[p].capacity)
        return SAVE5_ERR_RANGE;
    const uint8_t *e = blk(s, SAVE5_BLK_BAG) + kPockets[p].offset + 4 * slot;
    *item = g16(e);
    *qty = g16(e + 2);
    return SAVE5_OK;
}

save5_status save5_set_bag_slot(save5 *s, save5_pocket p, int slot, uint16_t item, uint16_t qty)
{
    if (!s || !s->img)
        return SAVE5_ERR_ARG;
    if ((unsigned)p >= SAVE5_POCKET_COUNT || slot < 0 || slot >= kPockets[p].capacity ||
        qty > kPockets[p].max_qty || (item && !qty))
        return SAVE5_ERR_RANGE;
    uint8_t *e = blk(s, SAVE5_BLK_BAG) + kPockets[p].offset + 4 * slot;
    s16(e, item);
    s16(e + 2, item ? qty : 0);
    save5_commit_block(s, SAVE5_BLK_BAG);
    return SAVE5_OK;
}

/* ----------------------------------------------------------- Pokédex */

static bool bit_get(const uint8_t *base, unsigned bit) { return (base[bit >> 3] >> (bit & 7)) & 1; }
static void bit_set(uint8_t *base, unsigned bit, bool v)
{
    if (v)
        base[bit >> 3] |= (uint8_t)(1u << (bit & 7));
    else
        base[bit >> 3] &= (uint8_t)~(1u << (bit & 7));
}

save5_status save5_dex_get(const save5 *s, uint16_t species, bool *seen, bool *caught)
{
    if (!s || !s->img || !seen || !caught)
        return SAVE5_ERR_ARG;
    if (species == 0 || species > SAVE5_DEX_MAX)
        return SAVE5_ERR_RANGE;
    const uint8_t *d = blk(s, SAVE5_BLK_DEX);
    const unsigned bit = species - 1u;
    *caught = bit_get(d + DEX_CAUGHT, bit);
    *seen = false;
    for (int r = 0; r < 4; r++)
        *seen = *seen || bit_get(d + DEX_SEEN + r * DEX_REGION, bit);
    return SAVE5_OK;
}

save5_status save5_dex_set(save5 *s, uint16_t species, bool seen, bool caught)
{
    if (!s || !s->img)
        return SAVE5_ERR_ARG;
    if (species == 0 || species > SAVE5_DEX_MAX)
        return SAVE5_ERR_RANGE;
    uint8_t *d = blk(s, SAVE5_BLK_DEX);
    const unsigned bit = species - 1u;
    seen = seen || caught;
    bit_set(d + DEX_CAUGHT, bit, caught);
    if (seen) {
        bool any_seen = false, any_disp = false;
        for (int r = 0; r < 4; r++) {
            any_seen = any_seen || bit_get(d + DEX_SEEN + r * DEX_REGION, bit);
            any_disp = any_disp || bit_get(d + DEX_DISPLAYED + r * DEX_REGION, bit);
        }
        if (!any_seen)
            bit_set(d + DEX_SEEN, bit, true);
        if (!any_disp)
            bit_set(d + DEX_DISPLAYED, bit, true);
    } else {
        for (int r = 0; r < 4; r++) {
            bit_set(d + DEX_SEEN + r * DEX_REGION, bit, false);
            bit_set(d + DEX_DISPLAYED + r * DEX_REGION, bit, false);
        }
    }
    save5_commit_block(s, SAVE5_BLK_DEX);
    return SAVE5_OK;
}

save5_status save5_dex_get_national(const save5 *s, bool *obtained)
{
    if (!s || !s->img || !obtained)
        return SAVE5_ERR_ARG;
    *obtained = blk(s, SAVE5_BLK_DEX)[DEX_PACKED] & 1;
    return SAVE5_OK;
}

save5_status save5_dex_set_national(save5 *s, bool obtained)
{
    if (!s || !s->img)
        return SAVE5_ERR_ARG;
    bit_set(blk(s, SAVE5_BLK_DEX) + DEX_PACKED, 0, obtained);
    save5_commit_block(s, SAVE5_BLK_DEX);
    return SAVE5_OK;
}

/* -------------------------------------------------------- event data */

save5_status save5_flag_get(const save5 *s, uint16_t id, bool *value)
{
    if (!s || !s->img || !value)
        return SAVE5_ERR_ARG;
    if (id >= SAVE5_NUM_FLAGS)
        return SAVE5_ERR_RANGE;
    *value = bit_get(blk(s, SAVE5_BLK_EVENT) + EV_FLAGS, id);
    return SAVE5_OK;
}

save5_status save5_flag_set(save5 *s, uint16_t id, bool value)
{
    if (!s || !s->img)
        return SAVE5_ERR_ARG;
    if (id >= SAVE5_NUM_FLAGS)
        return SAVE5_ERR_RANGE;
    bit_set(blk(s, SAVE5_BLK_EVENT) + EV_FLAGS, id, value);
    save5_commit_block(s, SAVE5_BLK_EVENT);
    return SAVE5_OK;
}

save5_status save5_var_get(const save5 *s, uint16_t id, uint16_t *value)
{
    if (!s || !s->img || !value)
        return SAVE5_ERR_ARG;
    if (id < SAVE5_VARS_START || id >= SAVE5_VARS_START + SAVE5_NUM_VARS)
        return SAVE5_ERR_RANGE;
    *value = g16(blk(s, SAVE5_BLK_EVENT) + 2 * (id - SAVE5_VARS_START));
    return SAVE5_OK;
}

save5_status save5_var_set(save5 *s, uint16_t id, uint16_t value)
{
    if (!s || !s->img)
        return SAVE5_ERR_ARG;
    if (id < SAVE5_VARS_START || id >= SAVE5_VARS_START + SAVE5_NUM_VARS)
        return SAVE5_ERR_RANGE;
    s16(blk(s, SAVE5_BLK_EVENT) + 2 * (id - SAVE5_VARS_START), value);
    save5_commit_block(s, SAVE5_BLK_EVENT);
    return SAVE5_OK;
}

/* ------------------------------------------------------ Mystery Gift */

/* Gen 4/5 PRNG stream XOR over u16 words (its own inverse). */
static void mg_crypt(uint8_t *data, size_t bytes, uint32_t seed)
{
    for (size_t i = 0; i + 1 < bytes; i += 2) {
        seed = seed * 0x41C64E6Du + 0x6073u;
        s16(data + i, (uint16_t)(g16(data + i) ^ (seed >> 16)));
    }
}

/* Decrypted copy of the gift data (MG_DATA bytes). */
static void mg_read(const save5 *s, uint8_t *out)
{
    const uint8_t *m = blk(s, SAVE5_BLK_MYSTERY);
    memcpy(out, m, MG_DATA);
    mg_crypt(out, MG_DATA, g32(m + MG_SEED));
}

static void mg_write(save5 *s, const uint8_t *plain)
{
    uint8_t *m = blk(s, SAVE5_BLK_MYSTERY);
    memcpy(m, plain, MG_DATA);
    mg_crypt(m, MG_DATA, g32(m + MG_SEED));
    save5_commit_block(s, SAVE5_BLK_MYSTERY);
}

static bool all_zero(const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (p[i])
            return false;
    return true;
}

save5_status save5_mg_validate(const uint8_t *data, size_t len, const char **why)
{
    const char *dummy;
    if (!why)
        why = &dummy;
    *why = NULL;
    if (!data || len != SAVE5_PGF_SIZE) {
        *why = "not a Gen 5 Wonder Card (.pgf is 204 bytes)";
        return SAVE5_ERR_FORMAT;
    }
    const uint8_t type = data[PGF_TYPE];
    if (type < SAVE5_MG_POKEMON || type > SAVE5_MG_POWER) {
        *why = "unknown gift type (want 1 Pokemon, 2 item, 3 Pass Power)";
        return SAVE5_ERR_FORMAT;
    }
    if (g16(data + PGF_ID) >= SAVE5_MG_FLAGS) {
        *why = "card id out of range (0..2047)";
        return SAVE5_ERR_FORMAT;
    }
    if (type == SAVE5_MG_POKEMON && (g16(data + PGF_SPECIES) == 0 || g16(data + PGF_SPECIES) > SAVE5_DEX_MAX)) {
        *why = "Pokemon gift with an invalid species";
        return SAVE5_ERR_FORMAT;
    }
    return SAVE5_OK;
}

save5_status save5_mg_add(save5 *s, const uint8_t *pgf, size_t len)
{
    if (!s || !s->img)
        return SAVE5_ERR_ARG;
    save5_status st = save5_mg_validate(pgf, len, NULL);
    if (st != SAVE5_OK)
        return st;
    uint8_t plain[MG_DATA];
    mg_read(s, plain);
    for (int slot = 0; slot < SAVE5_MG_SLOTS; slot++) {
        uint8_t *card = plain + MG_CARDS + slot * SAVE5_PGF_SIZE;
        if (!all_zero(card, SAVE5_PGF_SIZE))
            continue;
        memcpy(card, pgf, SAVE5_PGF_SIZE);
        bit_set(plain, g16(pgf + PGF_ID), true);
        mg_write(s, plain);
        return SAVE5_OK;
    }
    return SAVE5_ERR_NOSPACE;
}

save5_status save5_mg_get_card(const save5 *s, int slot, uint8_t card[SAVE5_PGF_SIZE], bool *used)
{
    if (!s || !s->img || !card)
        return SAVE5_ERR_ARG;
    if (slot < 0 || slot >= SAVE5_MG_SLOTS)
        return SAVE5_ERR_RANGE;
    uint8_t plain[MG_DATA];
    mg_read(s, plain);
    memcpy(card, plain + MG_CARDS + slot * SAVE5_PGF_SIZE, SAVE5_PGF_SIZE);
    if (used)
        *used = !all_zero(card, SAVE5_PGF_SIZE);
    return SAVE5_OK;
}

save5_status save5_mg_remove_card(save5 *s, int slot)
{
    if (!s || !s->img)
        return SAVE5_ERR_ARG;
    if (slot < 0 || slot >= SAVE5_MG_SLOTS)
        return SAVE5_ERR_RANGE;
    uint8_t plain[MG_DATA];
    mg_read(s, plain);
    memset(plain + MG_CARDS + slot * SAVE5_PGF_SIZE, 0, SAVE5_PGF_SIZE);
    mg_write(s, plain);
    return SAVE5_OK;
}

save5_status save5_mg_get_received(const save5 *s, uint16_t id, bool *received)
{
    if (!s || !s->img || !received)
        return SAVE5_ERR_ARG;
    if (id >= SAVE5_MG_FLAGS)
        return SAVE5_ERR_RANGE;
    uint8_t plain[MG_DATA];
    mg_read(s, plain);
    *received = bit_get(plain, id);
    return SAVE5_OK;
}

save5_status save5_mg_set_received(save5 *s, uint16_t id, bool received)
{
    if (!s || !s->img)
        return SAVE5_ERR_ARG;
    if (id >= SAVE5_MG_FLAGS)
        return SAVE5_ERR_RANGE;
    uint8_t plain[MG_DATA];
    mg_read(s, plain);
    bit_set(plain, id, received);
    mg_write(s, plain);
    return SAVE5_OK;
}
