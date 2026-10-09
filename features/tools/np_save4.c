/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * np_save4: inspect and edit Diamond/Pearl/Platinum/HeartGold/SoulSilver saves.
 *
 *   np_save4 dump [<rom.nds>] <save.sav>        JSON summary (names from ROM)
 *   np_save4 gamedata <rom.nds>                  JSON battle tables: species
 *                                                types, moves, type chart
 *   np_save4 verify <save.sav>                   checksum / copy report
 *   np_save4 set-money <save> <n>
 *   np_save4 set-coins <save> <n>
 *   np_save4 set-name <save> <name>
 *   np_save4 set-ids <save> <tid> <sid>
 *   np_save4 set-badges <save> <mask>
 *   np_save4 set-kanto-badges <save> <mask>      HeartGold/SoulSilver only
 *   np_save4 set-item <save> <pocket> <slot> <item> <qty>
 *   np_save4 set-flag <save> <id|FLAG_NAME> <0|1>   names: Platinum only
 *   np_save4 set-var <save> <id|VAR_NAME> <value>
 *   np_save4 set-dex <save> <species> none|seen|caught
 *   np_save4 set-national-dex <save> <0|1>
 *   np_save4 set-dex-obtained <save> <0|1>
 *   np_save4 set-mystery-gift <save> <0|1>      MYSTERY GIFT main menu option
 *   np_save4 add-gift <save> <gift.pcd|gift.pgt>
 *   np_save4 remove-gift <save> <card slot 1-3>
 *   np_save4 set-box-name <save> <box 1-18> <name>
 *   np_save4 add-mon <save> <rom.nds> <species> <level> [move...]   party Pokemon
 *   np_save4 set-move <save> <rom.nds> <slot> <index> <move>   a party Pokemon's move (full PP)
 *   np_save4 set-level <save> <rom.nds> <slot> <level>   a party Pokemon's level (stats recalculated)
 *   np_save4 heal-party <save> <rom.nds>   the party's HP, status and PP restored (a Pokemon Center's)
 *
 * Edits write back in place after copying the original to <save>.bak, or to
 * the path given with a trailing `-o <out>`.
 *
 * Exit status: 0 ok, 1 usage / I/O, 2 invalid value, 3 checksum failure or
 * unreadable save.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ndsdata/ndsdata.h"
#include "save4/save4.h"

#define EXIT_USAGE 1
#define EXIT_VALUE 2
#define EXIT_SAVE 3

static const char *prog = "np_save4";

static int usage(void)
{
    fprintf(stderr,
            "usage:\n"
            "  %s dump [<rom.nds>] <save.sav>\n"
            "  %s gamedata <rom.nds>\n"
            "  %s verify <save.sav>\n"
            "  %s set-money <save> <n>\n"
            "  %s set-coins <save> <n>\n"
            "  %s set-name <save> <name>\n"
            "  %s set-ids <save> <tid> <sid>\n"
            "  %s set-badges <save> <mask>\n"
            "  %s set-kanto-badges <save> <mask>\n"
            "  %s set-item <save> <pocket> <slot> <item> <qty>\n"
            "      pockets: items key_items tms_hms mail medicine berries balls battle_items\n"
            "  %s set-flag <save> <id|FLAG_NAME> <0|1>\n"
            "  %s set-var <save> <id|VAR_NAME> <value>\n"
            "  %s set-dex <save> <species> none|seen|caught\n"
            "  %s set-national-dex <save> <0|1>\n"
            "  %s set-dex-obtained <save> <0|1>\n"
            "  %s set-mystery-gift <save> <0|1>\n"
            "  %s add-gift <save> <gift.pcd|gift.pgt>\n"
            "  %s remove-gift <save> <card slot 1-3>\n"
            "  %s set-box-name <save> <box 1-18> <name>\n"
            "  %s add-mon <save> <rom.nds> <species> <level> [move id...]\n"
            "  %s set-move <save> <rom.nds> <party slot 0-5> <move index 0-3> <move id>\n"
            "  %s set-level <save> <rom.nds> <party slot 0-5> <level 1-100>\n"
            "  %s heal-party <save> <rom.nds>\n"
            "edits accept a trailing `-o <out.sav>`; otherwise the save is\n"
            "rewritten in place after backing it up to <save>.bak\n",
            prog, prog, prog, prog, prog, prog, prog, prog, prog, prog, prog, prog, prog, prog, prog, prog, prog,
            prog, prog, prog, prog, prog, prog);
    return EXIT_USAGE;
}

static int read_file(const char *path, uint8_t **out, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "%s: %s: %s\n", prog, path, strerror(errno));
        return -1;
    }
    size_t cap = 1 << 20, n = 0;
    uint8_t *buf = malloc(cap);
    for (;;) {
        if (!buf) {
            fclose(f);
            fprintf(stderr, "%s: out of memory\n", prog);
            return -1;
        }
        n += fread(buf + n, 1, cap - n, f);
        if (n < cap)
            break;
        cap *= 2;
        uint8_t *nb = realloc(buf, cap);
        if (!nb)
            free(buf);
        buf = nb;
    }
    int err = ferror(f);
    fclose(f);
    if (err) {
        fprintf(stderr, "%s: %s: read error\n", prog, path);
        free(buf);
        return -1;
    }
    *out = buf;
    *len = n;
    return 0;
}

static int write_file(const char *path, const uint8_t *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(data, 1, len, f) != len || fclose(f) != 0) {
        fprintf(stderr, "%s: %s: write failed: %s\n", prog, path, strerror(errno));
        return -1;
    }
    return 0;
}

static void report_blocks(FILE *o, const save4 *s)
{
    static const char *names[] = {"general", "storage"};
    for (int b = 0; b < SAVE4_BLOCK_COUNT; b++) {
        const save4_block_state *st = &s->blocks[b];
        fprintf(o, "  %s block: primary %s (save %u, block %u), backup %s (save %u, block %u)\n", names[b],
                st->valid[0] ? "valid" : "INVALID", st->save_counter[0], st->block_counter[0],
                st->valid[1] ? "valid" : "INVALID", st->save_counter[1], st->block_counter[1]);
    }
}

static int load_save(const char *path, save4 *s)
{
    uint8_t *data;
    size_t len;
    if (read_file(path, &data, &len) != 0)
        return EXIT_USAGE;
    save4_status st = save4_load(s, data, len);
    free(data);
    if (st != SAVE4_OK) {
        fprintf(stderr, "%s: error: %s: %s\n", prog, path, save4_status_str(st));
        if (st == SAVE4_ERR_CHECKSUM) {
            fprintf(stderr, "  detected game: %s\n", save4_game_name(s->game));
            report_blocks(stderr, s);
        }
        return EXIT_SAVE;
    }
    if (s->load_result == SAVE4_LOAD_RECOVERED)
        fprintf(stderr, "%s: warning: %s: one save copy is corrupt; using the other (as the game would)\n", prog, path);
    return 0;
}

/* ---------------------------------------------------------------- JSON */

static void jstr(FILE *o, const char *s)
{
    if (!s) {
        fputs("null", o);
        return;
    }
    fputc('"', o);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
        case '"': fputs("\\\"", o); break;
        case '\\': fputs("\\\\", o); break;
        case '\n': fputs("\\n", o); break;
        case '\r': fputs("\\r", o); break;
        case '\t': fputs("\\t", o); break;
        default:
            if (*p < 0x20)
                fprintf(o, "\\u%04x", *p);
            else
                fputc(*p, o);
        }
    }
    fputc('"', o);
}

static const nd_names *g_names;

static void jname(FILE *o, nd_text_kind kind, uint32_t id)
{
    jstr(o, g_names ? nd_name(g_names, kind, id) : NULL);
}

/* enum MysteryGiftType order (save4.h SAVE4_MG_*). */
static const char *const kGiftTypes[SAVE4_MG_TYPE_MAX] = {
    NULL,          "pokemon",     "egg",         "item",        "battle_reg",  "decoration", "cosmetics",
    "manaphy_egg", "member_card", "oaks_letter", "azure_flute", "poketch_app", "secret_key", "unknown",
    "pokewalker_course", "memorial_photo"};

static const char *gift_type_name(uint16_t type)
{
    return type > 0 && type < SAVE4_MG_TYPE_MAX ? kGiftTypes[type] : NULL;
}

static void dump_mystery(FILE *o, const save4 *s)
{
    bool unlocked = false;
    int pgts = 0;
    save4_mg_get_unlocked(s, &unlocked);
    save4_mg_pgt_count(s, &pgts);
    fprintf(o, "  \"mystery_gift\": {\"unlocked\": %s, \"pgts\": %d, \"cards\": [", unlocked ? "true" : "false", pgts);
    int n = 0;
    for (int slot = 0; slot < SAVE4_WONDERCARD_SLOTS; slot++) {
        uint8_t card[SAVE4_WONDERCARD_SIZE];
        bool used = false;
        if (save4_mg_get_card(s, slot, card, &used) != SAVE4_OK || !used)
            continue;
        uint16_t type = (uint16_t)(card[0] | card[1] << 8);
        fprintf(o, "%s{\"slot\": %d, \"type\": %u, \"type_name\": ", n++ ? ", " : "", slot + 1, type);
        jstr(o, gift_type_name(type));
        fprintf(o, ", \"id\": %u}", card[0x104 + 0x4C] | card[0x104 + 0x4D] << 8);
    }
    fputs("]}\n", o);
}

static void dump_mon(FILE *o, const pkm4 *p, save4_status dst, int full)
{
    pkm4_info i;
    pkm4_info_get(p, &i);
    fprintf(o, "\"species\": %u, \"species_name\": ", i.species);
    jname(o, ND_TEXT_SPECIES, i.species);
    fputs(", \"nickname\": ", o);
    jstr(o, i.nickname);
    fprintf(o, ", \"is_egg\": %s, \"checksum_ok\": %s", i.is_egg ? "true" : "false",
            dst == SAVE4_OK ? "true" : "false");
    if (i.has_party_data)
        fprintf(o, ", \"level\": %u", i.level);
    fprintf(o, ", \"exp\": %u, \"pid\": \"0x%08X\", \"shiny\": %s", i.exp, i.pid, i.shiny ? "true" : "false");
    if (!full)
        return;
    fprintf(o, ", \"nature\": ");
    jstr(o, g_names ? nd_nature_name(g_names, i.pid) : NULL);
    fprintf(o, ", \"ability\": {\"id\": %u, \"name\": ", i.ability);
    jname(o, ND_TEXT_ABILITIES, i.ability);
    fprintf(o, "}, \"held_item\": {\"id\": %u, \"name\": ", i.held_item);
    jname(o, ND_TEXT_ITEMS, i.held_item);
    fputs("}, \"moves\": [", o);
    int first = 1;
    for (int m = 0; m < 4; m++) {
        if (!i.moves[m])
            continue;
        fprintf(o, "%s{\"id\": %u, \"name\": ", first ? "" : ", ", i.moves[m]);
        jname(o, ND_TEXT_MOVES, i.moves[m]);
        fprintf(o, ", \"pp\": %u, \"pp_ups\": %u}", i.pp[m], i.pp_ups[m]);
        first = 0;
    }
    fprintf(o, "], \"ivs\": [%u, %u, %u, %u, %u, %u]", i.ivs[0], i.ivs[1], i.ivs[2], i.ivs[3], i.ivs[4], i.ivs[5]);
    fprintf(o, ", \"evs\": [%u, %u, %u, %u, %u, %u]", i.evs[0], i.evs[1], i.evs[2], i.evs[3], i.evs[4], i.evs[5]);
    fprintf(o, ", \"friendship\": %u, \"ot_name\": ", i.friendship);
    jstr(o, i.ot_name);
    fprintf(o, ", \"tid\": %u, \"sid\": %u, \"met_location\": {\"id\": %u, \"name\": ", i.tid, i.sid, i.met_location);
    jstr(o, g_names ? nd_location_name(g_names, i.met_location) : NULL);
    fprintf(o, "}, \"met_level\": %u, \"ball\": {\"id\": %u, \"name\": ", i.met_level, i.ball);
    /* Ball ids equal their item ids up to the Cherish Ball (16); HG/SS's Apricorn balls BALL_FAST 17..BALL_SPORT 24
     * are items 492..499 (pokeheartgold src/pokemon.c MON_DATA_POKEBALL, include/constants/items.h). */
    jname(o, ND_TEXT_ITEMS, i.ball >= 17 && i.ball <= 24 ? i.ball + (492u - 17u) : i.ball);
    fputc('}', o);
    /* block C's u64 ribbonsDS2 (Platinum struct_defs/pokemon.h), canonical offset 0x08 + 2 * 0x20 + 0x18: the Super
     * Contest ribbons, bit 0 = MON_DATA_SUPER_COOL_RIBBON (Normal rank) on (pokemon.c GetRibbon) */
    uint64_t ds2 = 0;
    for (int b = 7; b >= 0; b--)
        ds2 = ds2 << 8 | p->data[0x60 + b];
    fprintf(o, ", \"super_contest_ribbons\": %llu", (unsigned long long)ds2);
    if (i.has_party_data)
        fprintf(o, ", \"hp\": %u, \"stats\": [%u, %u, %u, %u, %u, %u], \"status\": %u", i.hp, i.stats[0], i.stats[1],
                i.stats[2], i.stats[3], i.stats[4], i.stats[5], i.status);
}

static int open_rom(const char *path, FILE **fp, nd_rom *rom)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "%s: %s: %s\n", prog, path, strerror(errno));
        return -1;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return -1;
    }
    long size = ftell(f);
    nd_status st = nd_rom_open(rom, nd_read_stdio, f, size > 0 ? (uint64_t)size : 0);
    if (st != ND_OK) {
        fprintf(stderr, "%s: %s: not a readable NDS ROM (%s)\n", prog, path, nd_status_str(st));
        fclose(f);
        return -1;
    }
    *fp = f;
    return 0;
}

/* The Hall of Fame: the extra save block written by Platinum's ClearGame (src/clear_game.c
 * ClearGame_AddHallOfFameEntry, SaveDataExtra_Save in src/savedata.c) and D/P's CallTask_GameClear (SaveHallOfFame,
 * arm9/src/unk_02022504.c; chunk header UNK_020EE6E0[0] in arm9/src/save_arrays.c). It sits at sector 32 of each
 * copy in both: 0x20000 primary, 0x60000 backup, each a HallOfFame (30 entries of six 0x3C-byte HallOfFamePokemon
 * (D/P: struct HOFMon) + a u16 year, u8 month, u8 day, then u32 nextEntryIndex, u32 totalEntriesCount;
 * include/hall_of_fame_entries.h, D/P include/hall_of_fame.h) and a footer (u32 signature 0x20060623, u32
 * saveCounter, u32 size, u16 id 0, u16 CRC16 of everything before it; D/P CreateChunkFooter, arm9/src/save.c).
 * HG/SS keep the same HallOfFame and SaveArrayFooter (pokeheartgold include/hall_of_fame.h, include/save.h,
 * src/save.c CreateChunkFooter) at sector SAVE_PAGE_MAX = 35 (gExtraSaveChunkHeaders[0], src/save_arrays.c), so
 * 0x23000 and 0x63000. The valid copy with the higher counter is dumped: the entry count and the latest entry
 * (date, species and levels). null: no valid copy (no Hall of Fame entered yet). */
#define HOF_MON_SIZE 0x3C
#define HOF_ENTRY_SIZE (6 * HOF_MON_SIZE + 4)
#define HOF_ENTRIES 30
#define HOF_SIZE (HOF_ENTRIES * HOF_ENTRY_SIZE + 8)

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

static void dump_hall_of_fame(FILE *o, const save4 *s)
{
    size_t len = 0;
    const uint8_t *img = save4_image(s, &len);
    const uint8_t *best = NULL;
    uint32_t best_counter = 0;
    const size_t copies[2] = {save4_game_is_hgss(s->game) ? 0x23000 : 0x20000,
                              save4_game_is_hgss(s->game) ? 0x63000 : 0x60000};
    for (int c = 0; c < 2; c++) {
        if (copies[c] + HOF_SIZE + 16 > len)
            continue;
        const uint8_t *body = img + copies[c], *ft = body + HOF_SIZE;
        if (le32(ft) != 0x20060623 || le32(ft + 8) != HOF_SIZE || le16(ft + 12) != 0 ||
            le16(ft + 14) != save4_crc16(body, HOF_SIZE + 14))
            continue;
        if (!best || le32(ft + 4) > best_counter) {
            best = body;
            best_counter = le32(ft + 4);
        }
    }
    if (!best) {
        fputs("  \"hall_of_fame\": null,\n", o);
        return;
    }
    uint32_t next = le32(best + HOF_ENTRIES * HOF_ENTRY_SIZE), total = le32(best + HOF_ENTRIES * HOF_ENTRY_SIZE + 4);
    fprintf(o, "  \"hall_of_fame\": {\"total\": %u, \"latest\": ", total);
    if (!total || next >= HOF_ENTRIES) {
        fputs("null},\n", o);
        return;
    }
    const uint8_t *e = best + ((next + HOF_ENTRIES - 1) % HOF_ENTRIES) * HOF_ENTRY_SIZE;
    const uint8_t *date = e + 6 * HOF_MON_SIZE;
    fprintf(o, "{\"date\": \"%04u-%02u-%02u\", \"party\": [", 2000u + le16(date), date[2], date[3]); /* RTCDate year 0-99 */
    for (int i = 0, n = 0; i < 6; i++) {
        const uint8_t *m = e + i * HOF_MON_SIZE;
        if (!le16(m))
            continue;
        fprintf(o, "%s{\"species\": %u, \"species_name\": ", n++ ? ", " : "", le16(m));
        jname(o, ND_TEXT_SPECIES, le16(m));
        fprintf(o, ", \"level\": %u}", m[2]);
    }
    fputs("]}},\n", o);
}

/* Platinum's Daycare (struct_defs/daycare.h) in the general block: two DaycareMon (BoxPokemon, DaycareMail,
 * u32 steps; 0xEC each), then u32 offspringPersonality (an egg waiting when non-zero) and u8 stepCounter (the
 * egg-cycle counter). Offset found by scanning lab saves for valid BoxPokemon (save_table.c sums the page sizes). */
#define PT_DAYCARE_OFF 0x1654
#define DAYCARE_MON_SIZE 0xEC

static void dump_daycare(FILE *o, const save4 *s)
{
    size_t len = 0;
    const uint8_t *img = save4_image(s, &len);
    uint32_t base = save4_block_base(s, SAVE4_BLOCK_GENERAL);
    if (s->game != SAVE4_GAME_PT || base + PT_DAYCARE_OFF + 2 * DAYCARE_MON_SIZE + 5 > len) {
        fputs("  \"daycare\": null,\n", o);
        return;
    }
    const uint8_t *d = img + base + PT_DAYCARE_OFF;
    fputs("  \"daycare\": {\"mons\": [", o);
    for (int i = 0, n = 0; i < 2; i++) {
        const uint8_t *m = d + i * DAYCARE_MON_SIZE;
        pkm4 p;
        save4_status st = pkm4_decrypt(m, PKM4_BOX_SIZE, &p);
        if (pkm4_is_empty(&p))
            continue;
        fputs(n++ ? ", {" : "{", o);
        dump_mon(o, &p, st, 0);
        fprintf(o, ", \"steps\": %u}", le32(m + DAYCARE_MON_SIZE - 4));
    }
    fprintf(o, "], \"egg_waiting\": %s, \"step_counter\": %u},\n",
            le32(d + 2 * DAYCARE_MON_SIZE) ? "true" : "false", d[2 * DAYCARE_MON_SIZE + 4]);
}

/* Platinum's SpecialEncounter roamers (struct_defs/special_encounter.h) in the general block: PlayerRecentRoutes
 * (int current, previous map), then ROAMING_SLOT_MAX (6) Roamer of 20 bytes (int map, u32 ivs, u32 personality,
 * u16 species, u16 hp, u8 level, status, active). Offset found by scanning played saves for the activated
 * Mesprit (slot 0), Cresselia (slot 1) and Moltres (slot 3). */
#define PT_ROAMERS_OFF 0x7FF4
#define ROAMER_SIZE 20
#define ROAMER_SLOTS 6

static void dump_roamers(FILE *o, const save4 *s)
{
    size_t len = 0;
    const uint8_t *img = save4_image(s, &len);
    uint32_t base = save4_block_base(s, SAVE4_BLOCK_GENERAL);
    if (s->game != SAVE4_GAME_PT || base + PT_ROAMERS_OFF + ROAMER_SLOTS * ROAMER_SIZE > len) {
        fputs("  \"roamers\": null,\n", o);
        return;
    }
    const uint8_t *r = img + base + PT_ROAMERS_OFF;
    fprintf(o, "  \"roamers\": {\"player_map\": %d, \"player_previous_map\": %d, \"slots\": [", (int32_t)le32(r - 8),
            (int32_t)le32(r - 4));
    for (int i = 0, n = 0; i < ROAMER_SLOTS; i++) {
        const uint8_t *m = r + i * ROAMER_SIZE;
        if (!le16(m + 12))
            continue;
        fprintf(o, "%s{\"slot\": %d, \"species\": %u, \"species_name\": ", n++ ? ", " : "", i, le16(m + 12));
        jname(o, ND_TEXT_SPECIES, le16(m + 12));
        fprintf(o, ", \"level\": %u, \"hp\": %u, \"map\": %d, \"active\": %s}", m[16], le16(m + 14), (int32_t)le32(m),
                m[18] ? "true" : "false");
    }
    fputs("]},\n", o);
}

/* Platinum's SpecialEncounter.trophyGarden (struct_defs/special_encounter.h: BOOL unused, u16 slot1, slot2), 8
 * bytes after the block's start (int marshDaily, swarmDaily): Mr. Backlot's daily Pokemon as indices into the
 * garden's 16-entry list (encounters_trophy_garden.json), 0xFFFF empty. Offset found by diffing a save before and
 * after Backlot's daily mon (80); the roamers above sit 0xD0 further on. */
#define PT_TROPHY_GARDEN_OFF 0x7F30

static void dump_trophy_garden(FILE *o, const save4 *s)
{
    size_t len = 0;
    const uint8_t *img = save4_image(s, &len);
    uint32_t base = save4_block_base(s, SAVE4_BLOCK_GENERAL);
    if (s->game != SAVE4_GAME_PT || base + PT_TROPHY_GARDEN_OFF + 4 > len) {
        fputs("  \"trophy_garden\": null,\n", o);
        return;
    }
    const uint8_t *g = img + base + PT_TROPHY_GARDEN_OFF;
    fputs("  \"trophy_garden\": [", o);
    for (int i = 0, n = 0; i < 2; i++)
        if (le16(g + 2 * i) != 0xFFFF)
            fprintf(o, "%s%u", n++ ? ", " : "", le16(g + 2 * i));
    fputs("],\n", o);
}

/* Platinum's PoffinCase (poffin.h) in the general block: 100 Poffin of 8 bytes (type, spiciness, dryness,
 * sweetness, bitterness, sourness, smoothness, dummy); a slot whose type is past POFFIN_TYPE_MILD (28) is empty.
 * Offset found by diffing a save before and after cooking one poffin (77). */
#define PT_POFFINS_OFF 0x52E8
#define POFFIN_SLOTS 100

static void dump_poffins(FILE *o, const save4 *s)
{
    size_t len = 0;
    const uint8_t *img = save4_image(s, &len);
    uint32_t base = save4_block_base(s, SAVE4_BLOCK_GENERAL);
    if (s->game != SAVE4_GAME_PT || base + PT_POFFINS_OFF + POFFIN_SLOTS * 8 > len) {
        fputs("  \"poffins\": null,\n", o);
        return;
    }
    const uint8_t *c = img + base + PT_POFFINS_OFF;
    fputs("  \"poffins\": [", o);
    for (int i = 0, n = 0; i < POFFIN_SLOTS; i++) {
        const uint8_t *q = c + i * 8;
        if (q[0] > 28)
            continue;
        fprintf(o, "%s{\"slot\": %d, \"type\": %u, \"flavors\": [%u, %u, %u, %u, %u], \"smoothness\": %u}",
                n++ ? ", " : "", i, q[0], q[1], q[2], q[3], q[4], q[5], q[6]);
    }
    fputs("],\n", o);
}

/* Platinum's Underground (underground.h) tail in the general block: u32 minedPlates, then u8 goodsPC[200],
 * traps[40], goodsBag[40], treasure[40], sphereTypes[40], sphereSizes[40], placedGoodSlots[15], stepCount and a
 * byte holding shouldSpawnNewBuriedObjects (low 4 bits) and hasMined (high 4). A zero is an empty slot. Offset
 * found by scanning a lab save stocked with known spheres, traps and goods (92's recipe). Values are the
 * generated sphere_types/traps/goods/treasure enums. */
#define PT_UG_PLATES_OFF 0x446C

static void dump_ug_bag(FILE *o, const char *key, const uint8_t *b, int n)
{
    fprintf(o, "\"%s\": [", key);
    for (int i = 0, k = 0; i < n; i++)
        if (b[i])
            fprintf(o, "%s%u", k++ ? ", " : "", b[i]);
    fputs("]", o);
}

static void dump_underground(FILE *o, const save4 *s)
{
    size_t len = 0;
    const uint8_t *img = save4_image(s, &len);
    uint32_t base = save4_block_base(s, SAVE4_BLOCK_GENERAL);
    if (s->game != SAVE4_GAME_PT || base + PT_UG_PLATES_OFF + 0x1A5 > len) {
        fputs("  \"underground\": null,\n", o);
        return;
    }
    const uint8_t *u = img + base + PT_UG_PLATES_OFF;
    const uint8_t *types = u + 4 + 200 + 40 * 3, *sizes = types + 40;
    fprintf(o, "  \"underground\": {\"mined_plates\": %u, ", le32(u));
    dump_ug_bag(o, "goods_pc", u + 4, 200);
    fputs(", ", o);
    dump_ug_bag(o, "traps", u + 4 + 200, 40);
    fputs(", ", o);
    dump_ug_bag(o, "goods", u + 4 + 240, 40);
    fputs(", ", o);
    dump_ug_bag(o, "treasures", u + 4 + 280, 40);
    fputs(", \"spheres\": [", o);
    for (int i = 0, k = 0; i < 40; i++)
        if (types[i])
            fprintf(o, "%s{\"type\": %u, \"size\": %u}", k++ ? ", " : "", types[i], sizes[i]);
    fprintf(o, "], \"has_mined\": %s},\n", (sizes[40 + 15 + 1] >> 4) ? "true" : "false");
}

/* Platinum's GameRecords (game_records.h: u32 recordsU32[71], u16 recordsU16[77], u16 padding, EncodingSeed
 * {u16 byteSum, u16 modifier}) in the general block. recordsU32[0] (RECORD_STEPS) is stored plain; the rest, up
 * to the seed, is EncodeData'd (math_util.c: each u16 XORed with the LCRNG stream from byteSum + (modifier << 16),
 * game_records.c EncodeGameRecords), and byteSum is the plain bytes' sum. Offset found by scanning the general
 * block for the one 0x1BC window whose decoded bytes sum to its byteSum (a save that watched TV once reads
 * RECORD_WATCHED_TV 1). Printed as a list indexed by record id (generated/game_records.txt order). */
#define PT_GAME_RECORDS_OFF 0x61B0
#define PT_GAME_RECORDS_U32 71
#define PT_GAME_RECORDS_U16 77
#define PT_GAME_RECORDS_SEED (PT_GAME_RECORDS_U32 * 4 + PT_GAME_RECORDS_U16 * 2 + 2)

static void dump_game_records(FILE *o, const save4 *s)
{
    size_t len = 0;
    const uint8_t *img = save4_image(s, &len);
    uint32_t base = save4_block_base(s, SAVE4_BLOCK_GENERAL);
    if (s->game != SAVE4_GAME_PT || base + PT_GAME_RECORDS_OFF + PT_GAME_RECORDS_SEED + 4 > len) {
        fputs("  \"game_records\": null,\n", o);
        return;
    }
    const uint8_t *r = img + base + PT_GAME_RECORDS_OFF;
    uint8_t plain[PT_GAME_RECORDS_SEED];
    uint32_t seed = (uint32_t)r[PT_GAME_RECORDS_SEED] | (uint32_t)r[PT_GAME_RECORDS_SEED + 1] << 8 |
                    (uint32_t)r[PT_GAME_RECORDS_SEED + 2] << 16 | (uint32_t)r[PT_GAME_RECORDS_SEED + 3] << 24;
    memcpy(plain, r, sizeof plain);
    for (int i = 4; i < PT_GAME_RECORDS_SEED; i += 2) {
        seed = seed * 0x41C64E6Du + 0x6073u;
        plain[i] ^= (uint8_t)(seed >> 16);
        plain[i + 1] ^= (uint8_t)(seed >> 24);
    }
    fputs("  \"game_records\": [", o);
    for (int i = 0; i < PT_GAME_RECORDS_U32; i++)
        fprintf(o, "%s%u", i ? ", " : "", le32(plain + i * 4));
    for (int i = 0; i < PT_GAME_RECORDS_U16; i++) {
        const uint8_t *h = plain + PT_GAME_RECORDS_U32 * 4 + i * 2;
        fprintf(o, ", %u", (unsigned)(h[0] | h[1] << 8));
    }
    fputs("],\n", o);
}

static int cmd_dump(const char *rom_path, const char *save_path)
{
    save4 s;
    int rc = load_save(save_path, &s);
    if (rc)
        return rc;

    nd_names names;
    nd_rom rom;
    FILE *rf = NULL;
    int have_rom = 0;
    if (rom_path) {
        if (open_rom(rom_path, &rf, &rom) != 0) {
            save4_free(&s);
            return EXIT_USAGE;
        }
        have_rom = 1;
        int rom_pt = rom.game == ND_GAME_PLATINUM;
        int rom_dp = rom.game == ND_GAME_DIAMOND || rom.game == ND_GAME_PEARL;
        int rom_hgss = rom.game == ND_GAME_HEARTGOLD || rom.game == ND_GAME_SOULSILVER;
        if ((s.game == SAVE4_GAME_PT && !rom_pt) || (s.game == SAVE4_GAME_DP && !rom_dp) ||
            (save4_game_is_hgss(s.game) && !rom_hgss)) {
            fprintf(stderr, "%s: warning: ROM is %s (%s) but the save is %s; names omitted\n", prog,
                    nd_game_name(rom.game), rom.gamecode, save4_game_name(s.game));
        } else {
            nd_status st = nd_names_load(&names, &rom);
            if (st != ND_OK) {
                fprintf(stderr, "%s: %s: cannot read text from ROM (%s)\n", prog, rom_path, nd_status_str(st));
                nd_rom_close(&rom);
                fclose(rf);
                save4_free(&s);
                return EXIT_USAGE;
            }
            g_names = &names;
        }
    }

    FILE *o = stdout;
    save4_trainer t;
    save4_get_trainer(&s, &t);
    fprintf(o, "{\n  \"game\": \"%s\",\n  \"load_result\": \"%s\",\n", save4_game_name(s.game),
            s.load_result == SAVE4_LOAD_OK ? "ok" : "recovered");
    if (have_rom) {
        fputs("  \"rom\": {\"title\": ", o);
        jstr(o, rom.title);
        fputs(", \"gamecode\": ", o);
        jstr(o, rom.gamecode);
        fprintf(o, ", \"version\": %u},\n", rom.rom_version);
    } else {
        fputs("  \"rom\": null,\n", o);
    }
    fputs("  \"blocks\": [", o);
    for (int b = 0; b < SAVE4_BLOCK_COUNT; b++) {
        const save4_block_state *st = &s.blocks[b];
        fprintf(o,
                "%s{\"name\": \"%s\", \"active\": \"%s\", \"valid\": [%s, %s], \"save_counter\": [%u, %u], "
                "\"block_counter\": [%u, %u]}",
                b ? ", " : "", b ? "storage" : "general", st->active ? "backup" : "primary",
                st->valid[0] ? "true" : "false", st->valid[1] ? "true" : "false", st->save_counter[0],
                st->save_counter[1], st->block_counter[0], st->block_counter[1]);
    }
    fputs("],\n  \"trainer\": {\"name\": ", o);
    jstr(o, t.name);
    int nbadges = 0, nkanto = 0;
    for (int i = 0; i < 8; i++) {
        nbadges += (t.badges >> i) & 1;
        nkanto += (t.kanto_badges >> i) & 1;
    }
    fprintf(o,
            ", \"tid\": %u, \"sid\": %u, \"gender\": \"%s\", \"money\": %u, \"coins\": %u, \"badges\": %d, "
            "\"badge_mask\": %u, ",
            t.tid, t.sid, t.gender ? "female" : "male", t.money, t.coins, nbadges, t.badges);
    /* HG/SS: badges / badge_mask are Johto's, these Kanto's; D/P/Pt have none. */
    if (save4_game_is_hgss(s.game))
        fprintf(o, "\"kanto_badges\": %d, \"kanto_badge_mask\": %u, ", nkanto, t.kanto_badges);
    else
        fputs("\"kanto_badges\": null, \"kanto_badge_mask\": null, ", o);
    fprintf(o, "\"play_time\": \"%u:%02u:%02u\", \"language\": %u, \"national_dex\": %s},\n", t.play_hours,
            t.play_minutes, t.play_seconds, t.language, t.has_national_dex ? "true" : "false");
    save4_location loc;
    save4_get_location(&s, &loc);
    fprintf(o, "  \"location\": {\"map\": %u, \"x\": %u, \"z\": %u, \"dir\": %u},\n", loc.map, loc.x, loc.z, loc.dir);
    /* The game clock as last recorded (the RTC shortly before the save). */
    save4_game_time gt;
    save4_get_game_time(&s, &gt);
    fprintf(o, "  \"game_time\": {\"date\": \"%04u-%02u-%02u\", \"time\": \"%02u:%02u:%02u\"},\n", gt.year, gt.month,
            gt.day, gt.hour, gt.minute, gt.second);
    /* Event state: the ids of every set flag, and every nonzero var keyed
     * by its decimal id (tests/e2e checks story progress against these). */
    fputs("  \"flags\": [", o);
    for (unsigned id = 1, n = 0; id < (unsigned)save4_num_flags(&s); id++) {
        bool on = false;
        if (save4_flag_get(&s, (uint16_t)id, &on) == SAVE4_OK && on)
            fprintf(o, "%s%u", n++ ? ", " : "", id);
    }
    fputs("],\n  \"vars\": {", o);
    for (unsigned id = SAVE4_VARS_START, n = 0; id < SAVE4_VARS_START + (unsigned)save4_num_vars(&s); id++) {
        uint16_t v = 0;
        if (save4_var_get(&s, (uint16_t)id, &v) == SAVE4_OK && v)
            fprintf(o, "%s\"%u\": %u", n++ ? ", " : "", id, v);
    }
    fputs("},\n", o);

    fputs("  \"party\": [", o);
    uint8_t count = save4_party_count(&s);
    for (int i = 0; i < count; i++) {
        pkm4 p;
        save4_status pst = save4_get_party(&s, i, &p);
        fprintf(o, "%s\n    {\"slot\": %d, ", i ? "," : "", i + 1);
        dump_mon(o, &p, pst, 1);
        fputc('}', o);
    }
    fputs(count ? "\n  ],\n" : "],\n", o);

    fprintf(o, "  \"current_box\": %u,\n  \"boxes\": [", save4_current_box(&s) + 1);
    for (int b = 0; b < SAVE4_BOX_COUNT; b++) {
        char name[64];
        save4_get_box_name(&s, b, name, sizeof(name));
        fprintf(o, "%s\n    {\"box\": %d, \"name\": ", b ? "," : "", b + 1);
        jstr(o, name);
        fputs(", \"mons\": [", o);
        int n = 0;
        for (int slot = 0; slot < SAVE4_BOX_SLOTS; slot++) {
            pkm4 p;
            save4_status pst = save4_get_box_mon(&s, b, slot, &p);
            if (pst == SAVE4_OK && pkm4_is_empty(&p))
                continue;
            fprintf(o, "%s{\"slot\": %d, ", n ? ", " : "", slot + 1);
            dump_mon(o, &p, pst, 0);
            fputc('}', o);
            n++;
        }
        fprintf(o, "], \"count\": %d}", n);
    }
    fputs("\n  ],\n  \"bag\": {", o);
    for (int pk = 0; pk < SAVE4_POCKET_COUNT; pk++) {
        fprintf(o, "%s\n    \"%s\": [", pk ? "," : "", save4_pocket_name((save4_pocket)pk));
        int n = 0;
        for (int slot = 0; slot < save4_pocket_capacity(&s, (save4_pocket)pk); slot++) {
            uint16_t item, qty;
            save4_get_bag_slot(&s, (save4_pocket)pk, slot, &item, &qty);
            if (!item)
                continue;
            fprintf(o, "%s{\"item\": %u, \"name\": ", n++ ? ", " : "", item);
            jname(o, ND_TEXT_ITEMS, item);
            fprintf(o, ", \"qty\": %u}", qty);
        }
        fputc(']', o);
    }
    uint16_t registered = 0;
    save4_get_registered_item(&s, &registered);
    fprintf(o, ",\n    \"registered\": %u", registered);
    /* Counts, and the species ids themselves (a recipe's dex-seen and
     * dex-caught lines are checked against these). */
    int seen = 0, caught = 0;
    int dex_ok = 1;
    for (int sp = 1; sp <= SAVE4_DEX_MAX; sp++) {
        bool sv, cv;
        if (save4_dex_get(&s, (uint16_t)sp, &sv, &cv) != SAVE4_OK) {
            dex_ok = 0;
            break;
        }
        seen += sv;
        caught += cv;
    }
    if (dex_ok) {
        bool obtained = false, national = false;
        save4_dex_get_obtained(&s, &obtained);
        save4_dex_get_national(&s, &national);
        fprintf(o, "\n  },\n  \"pokedex\": {\"seen\": %d, \"caught\": %d, \"obtained\": %s, \"national\": %s",
                seen, caught, obtained ? "true" : "false", national ? "true" : "false");
        for (int list = 0; list < 2; list++) {
            fputs(list ? ", \"caught_list\": [" : ", \"seen_list\": [", o);
            for (int sp = 1, n = 0; sp <= SAVE4_DEX_MAX; sp++) {
                bool sv = false, cv = false;
                save4_dex_get(&s, (uint16_t)sp, &sv, &cv);
                if (list ? cv : sv)
                    fprintf(o, "%s%d", n++ ? ", " : "", sp);
            }
            fputc(']', o);
        }
        fputs("},\n", o);
    } else {
        fputs("\n  },\n  \"pokedex\": null,\n", o);
    }
    save4_poketch ptch;
    if (save4_get_poketch(&s, &ptch) != SAVE4_OK) {
        fputs("  \"poketch\": null,\n", o); /* HG/SS have no Poketch */
    } else {
        fprintf(o, "  \"poketch\": {\"given\": %s, \"apps\": [", ptch.given ? "true" : "false");
        for (int a = 0, n = 0; a < SAVE4_POKETCH_APPS; a++)
            if (ptch.apps[a])
                fprintf(o, "%s%d", n++ ? ", " : "", a);
        fputs("]},\n", o);
    }
    dump_hall_of_fame(o, &s);
    dump_daycare(o, &s);
    dump_roamers(o, &s);
    dump_poffins(o, &s);
    dump_trophy_garden(o, &s);
    dump_underground(o, &s);
    dump_game_records(o, &s);
    dump_mystery(o, &s);
    fputs("}\n", o);

    if (g_names)
        nd_names_free(&names);
    g_names = NULL;
    if (have_rom) {
        nd_rom_close(&rom);
        fclose(rf);
    }
    save4_free(&s);
    return 0;
}

/* gamedata: what a battle bot needs from the ROM (tests/e2e auto_battle).
 * species[i] = [type1, type2, ability1, ability2 (0: one ability)]; moves[i] = [effect, class, power, type,
 * accuracy, pp, priority, range]; type_chart[attack][defend] = multiplier x10. */
static int cmd_gamedata(const char *rom_path)
{
    FILE *rf;
    nd_rom rom;
    if (open_rom(rom_path, &rf, &rom) != 0)
        return EXIT_USAGE;
    nd_gamedata gd;
    nd_status st = nd_gamedata_load(&gd, &rom);
    if (st != ND_OK || !gd.type_chart_ok) {
        fprintf(stderr, "%s: %s: no game data (%s)\n", prog, rom_path,
                st != ND_OK ? nd_status_str(st) : "type chart not found");
        if (st == ND_OK)
            nd_gamedata_free(&gd);
        nd_rom_close(&rom);
        fclose(rf);
        return EXIT_USAGE;
    }
    printf("{\"game\": \"%s\",\n\"species\": [", nd_game_name(gd.game));
    for (uint32_t i = 0; i < gd.species_count; i++)
        printf("%s[%u,%u,%u,%u]", i ? "," : "", gd.species[i].types[0], gd.species[i].types[1],
               gd.species[i].abilities[0], gd.species[i].abilities[1]);
    printf("],\n\"moves\": [");
    for (uint32_t i = 0; i < gd.move_count; i++) {
        const nd_move *m = &gd.moves[i];
        printf("%s[%u,%u,%u,%u,%u,%u,%d,%u]", i ? "," : "", m->effect, m->cls, m->power, m->type, m->accuracy, m->pp,
               m->priority, m->range);
    }
    printf("],\n\"type_chart\": [");
    for (int a = 0; a < ND_TYPES; a++) {
        printf("%s[", a ? "," : "");
        for (int d = 0; d < ND_TYPES; d++)
            printf("%s%u", d ? "," : "", gd.type_chart[a][d]);
        printf("]");
    }
    printf("]}\n");
    nd_gamedata_free(&gd);
    nd_rom_close(&rom);
    fclose(rf);
    return 0;
}

static int cmd_verify(const char *path)
{
    save4 s;
    int rc = load_save(path, &s);
    if (rc)
        return rc;
    printf("%s: %s save, %s\n", path, save4_game_name(s.game),
           s.load_result == SAVE4_LOAD_OK ? "all checksums valid" : "recovered from backup copy");
    report_blocks(stdout, &s);
    printf("  active: general=%s storage=%s\n", s.blocks[0].active ? "backup" : "primary",
           s.blocks[1].active ? "backup" : "primary");
    save4_free(&s);
    return 0;
}

static int parse_ul(const char *s, unsigned long max, unsigned long *out)
{
    char *end;
    errno = 0;
    unsigned long v = strtoul(s, &end, 0);
    if (errno || end == s || *end || v > max || s[0] == '-') {
        fprintf(stderr, "%s: invalid number '%s' (max %lu)\n", prog, s, max);
        return -1;
    }
    *out = v;
    return 0;
}

static int lookup_id(const save4 *s, const char *name, unsigned long *out)
{
    uint16_t id;
    if (name[0] >= '0' && name[0] <= '9')
        return parse_ul(name, 0xFFFF, out);
    if (save4_game_is_hgss(s->game)) {
        fprintf(stderr, "%s: flag/var names are Platinum's; use a numeric id for %s\n", prog,
                save4_game_name(s->game));
        return -1;
    }
    if (save4_pt_lookup_name(name, &id) == 0) {
        *out = id;
        return 0;
    }
    fprintf(stderr, "%s: unknown flag/var name '%s' (Platinum names from pokeplatinum vars_flags.txt)\n", prog, name);
    return -1;
}

static int edit_status(save4_status st)
{
    if (st == SAVE4_OK)
        return 0;
    fprintf(stderr, "%s: error: %s\n", prog, save4_status_str(st));
    return EXIT_VALUE;
}

/* add-mon: a party Pokemon as the game's own gift would make it (the
 * species' base friendship and first ability, its growth rate's EXP for the
 * level, stats from base stats, IVs 20 and no EVs, met here in a Poke Ball
 * with the trainer as OT; on an HG/SS save the ball also goes to the HGSS
 * ball byte, as HG/SS's SetMonData does), appended to the party. Moves are
 * given by id. The origin game is the ROM's (D 10, P 11, Pt 12, HG 7, SS 8). */
static save4_status add_mon(save4 *s, const char *rom_path, unsigned long species, unsigned long level,
                            char **moves, int nmoves)
{
    FILE *rf;
    nd_rom rom;
    if (open_rom(rom_path, &rf, &rom) != 0)
        return SAVE4_ERR_ARG;
    nd_gamedata gd;
    nd_names names;
    save4_status st = SAVE4_ERR_ARG;
    int have_gd = nd_gamedata_load(&gd, &rom) == ND_OK;
    int have_names = have_gd && nd_names_load(&names, &rom) == ND_OK;
    const nd_species *sp = have_gd ? nd_species_get(&gd, (uint32_t)species) : NULL;
    uint8_t count = save4_party_count(s);
    save4_trainer t;
    if (!sp || !sp->valid || !have_names || count >= SAVE4_PARTY_MAX || save4_get_trainer(s, &t) != SAVE4_OK) {
        fprintf(stderr, "%s: cannot add species %lu (unknown species, unreadable ROM or full party)\n", prog,
                species);
        goto out;
    }
    pkm4 p;
    memset(&p, 0, sizeof p);
    p.party = true;
    /* A fixed personality per species, level and trainer: runs repeat. */
    uint32_t pid = (uint32_t)species * 2654435761u ^ (uint32_t)level * 40503u ^ ((uint32_t)t.sid << 16 | t.tid);
    pkm4_set_pid(&p, pid);
    pkm4_set_species(&p, (uint16_t)species);
    pkm4_set_ot_ids(&p, t.tid, t.sid);
    pkm4_set_exp(&p, nd_exp_for_level(&gd, (uint32_t)species, (uint32_t)level));
    pkm4_set_friendship(&p, sp->base_friendship);
    pkm4_set_ability(&p, sp->abilities[0]);
    pkm4_set_language(&p, t.language);
    for (int i = 0; i < nmoves && i < 4; i++) {
        unsigned long mv;
        if (parse_ul(moves[i], 0xFFFF, &mv) != 0)
            goto out;
        pkm4_set_move(&p, i, (uint16_t)mv, nd_move_base_pp(&gd, (uint32_t)mv), 0);
    }
    uint8_t ivs[6], evs[6] = {0};
    for (int i = 0; i < 6; i++) {
        ivs[i] = 20;
        pkm4_set_iv(&p, i, 20);
    }
    uint8_t gender = sp->gender_ratio == 255 ? 2 : sp->gender_ratio == 254 ? 1 : sp->gender_ratio == 0 ? 0
                     : (pid & 0xFF) < sp->gender_ratio ? 1 : 0;
    pkm4_set_gender_form(&p, gender, 0);
    const char *name = nd_name(&names, ND_TEXT_SPECIES, (uint32_t)species);
    if (!name || pkm4_set_nickname(&p, name, false) != SAVE4_OK || pkm4_set_ot_name(&p, t.name) != SAVE4_OK) {
        st = SAVE4_ERR_ENCODE;
        goto out;
    }
    pkm4_set_origin_game(&p, rom.game == ND_GAME_DIAMOND      ? 10
                             : rom.game == ND_GAME_PEARL      ? 11
                             : rom.game == ND_GAME_HEARTGOLD  ? 7
                             : rom.game == ND_GAME_SOULSILVER ? 8
                                                              : 12);
    pkm4_set_met(&p, 0, (uint8_t)level, 4 /* Poke Ball */, t.gender);
    if (save4_game_is_hgss(s->game))
        pkm4_set_ball_hgss(&p, 4);
    uint16_t stats[6];
    pkm4_calc_stats(sp->base, ivs, evs, (uint8_t)level, (uint8_t)(pid % 25), species == 292, stats);
    pkm4_set_party_stats(&p, (uint8_t)level, stats[0], stats, 0);
    st = save4_set_party(s, count, &p);
    if (st == SAVE4_OK)
        st = save4_set_party_count(s, (uint8_t)(count + 1));
out:
    if (have_names)
        nd_names_free(&names);
    if (have_gd)
        nd_gamedata_free(&gd);
    nd_rom_close(&rom);
    fclose(rf);
    return st;
}

/* set-move: party slot `slot`'s move `index` becomes `move` with the ROM's base PP and no PP Ups (what a
 * move tutor or TM leaves; the e2e boosts' party-move). */
static save4_status set_move(save4 *s, const char *rom_path, unsigned long slot, unsigned long index,
                             unsigned long move)
{
    FILE *rf;
    nd_rom rom;
    if (open_rom(rom_path, &rf, &rom) != 0)
        return SAVE4_ERR_ARG;
    nd_gamedata gd;
    save4_status st = SAVE4_ERR_ARG;
    int have_gd = nd_gamedata_load(&gd, &rom) == ND_OK;
    pkm4 p;
    if (!have_gd || slot >= save4_party_count(s) || save4_get_party(s, (int)slot, &p) != SAVE4_OK
        || pkm4_is_empty(&p)) {
        fprintf(stderr, "%s: no party Pokemon in slot %lu (or an unreadable ROM)\n", prog, slot);
        goto out;
    }
    pkm4_set_move(&p, (int)index, (uint16_t)move, move ? nd_move_base_pp(&gd, (uint32_t)move) : 0, 0);
    st = save4_set_party(s, (int)slot, &p);
out:
    if (have_gd)
        nd_gamedata_free(&gd);
    nd_rom_close(&rom);
    fclose(rf);
    return st;
}

/* set-level: party slot `slot` at `level`: its growth rate's EXP for the level, its stats recalculated from its
 * own IVs, EVs and nature, its HP the new maximum (the e2e boosts' party-level). Moves, friendship and the rest
 * stay as they are: no level-up moves are learned and no evolution runs. */
static save4_status set_level(save4 *s, const char *rom_path, unsigned long slot, unsigned long level)
{
    FILE *rf;
    nd_rom rom;
    if (open_rom(rom_path, &rf, &rom) != 0)
        return SAVE4_ERR_ARG;
    nd_gamedata gd;
    save4_status st = SAVE4_ERR_ARG;
    int have_gd = nd_gamedata_load(&gd, &rom) == ND_OK;
    pkm4 p;
    pkm4_info info;
    const nd_species *sp = NULL;
    if (have_gd && slot < save4_party_count(s) && save4_get_party(s, (int)slot, &p) == SAVE4_OK
        && !pkm4_is_empty(&p)) {
        pkm4_info_get(&p, &info);
        sp = nd_species_get(&gd, info.species);
    }
    if (!sp || !sp->valid || info.is_egg) {
        fprintf(stderr, "%s: no party Pokemon in slot %lu (an egg, or an unreadable ROM)\n", prog, slot);
        goto out;
    }
    pkm4_set_exp(&p, nd_exp_for_level(&gd, info.species, (uint32_t)level));
    uint16_t stats[6];
    pkm4_calc_stats(sp->base, info.ivs, info.evs, (uint8_t)level, info.nature, info.species == 292, stats);
    pkm4_set_party_stats(&p, (uint8_t)level, stats[0], stats, info.status);
    st = save4_set_party(s, (int)slot, &p);
out:
    if (have_gd)
        nd_gamedata_free(&gd);
    nd_rom_close(&rom);
    fclose(rf);
    return st;
}

/* heal-party: every party Pokemon (eggs left alone) as a Pokemon Center leaves it: HP at its maximum, no status,
 * every move's PP at its maximum (the ROM's base PP raised by its PP Ups, base + base * ups / 5). The e2e boosts'
 * party-heal: what a player's Full Restores and Revives do between the Elite Four's rooms, which no bot uses. */
static save4_status heal_party(save4 *s, const char *rom_path)
{
    FILE *rf;
    nd_rom rom;
    if (open_rom(rom_path, &rf, &rom) != 0)
        return SAVE4_ERR_ARG;
    nd_gamedata gd;
    save4_status st = SAVE4_ERR_ARG;
    if (nd_gamedata_load(&gd, &rom) != ND_OK) {
        fprintf(stderr, "%s: unreadable ROM\n", prog);
        goto out_rom;
    }
    st = SAVE4_OK;
    for (int slot = 0; slot < save4_party_count(s) && st == SAVE4_OK; slot++) {
        pkm4 p;
        pkm4_info info;
        if ((st = save4_get_party(s, slot, &p)) != SAVE4_OK)
            break;
        if (pkm4_is_empty(&p))
            continue;
        pkm4_info_get(&p, &info);
        if (info.is_egg)
            continue;
        for (int i = 0; i < 4; i++) {
            if (!info.moves[i])
                continue;
            uint8_t base = nd_move_base_pp(&gd, info.moves[i]);
            pkm4_set_move(&p, i, info.moves[i], (uint8_t)(base + base * info.pp_ups[i] / 5), info.pp_ups[i]);
        }
        pkm4_set_party_stats(&p, info.level, info.stats[0], info.stats, 0);
        st = save4_set_party(s, slot, &p);
    }
    nd_gamedata_free(&gd);
out_rom:
    nd_rom_close(&rom);
    fclose(rf);
    return st;
}

static int cmd_edit(int argc, char **argv)
{
    const char *cmd = argv[1];
    const char *out_path = NULL;
    if (argc >= 5 && !strcmp(argv[argc - 2], "-o")) {
        out_path = argv[argc - 1];
        argc -= 2;
    }
    if (argc < 4)
        return usage();
    const char *path = argv[2];
    char **a = argv + 3;
    int na = argc - 3;

    save4 s;
    int rc = load_save(path, &s);
    if (rc)
        return rc;
    unsigned long v1 = 0, v2 = 0, v3 = 0;
    save4_status st = SAVE4_ERR_ARG;
    int bad = 0;

    if (!strcmp(cmd, "set-money") && na == 1) {
        bad = parse_ul(a[0], SAVE4_MONEY_MAX, &v1);
        if (!bad)
            st = save4_set_money(&s, (uint32_t)v1);
    } else if (!strcmp(cmd, "set-coins") && na == 1) {
        bad = parse_ul(a[0], SAVE4_COINS_MAX, &v1);
        if (!bad)
            st = save4_set_coins(&s, (uint16_t)v1);
    } else if (!strcmp(cmd, "set-name") && na == 1) {
        st = save4_set_trainer_name(&s, a[0]);
    } else if (!strcmp(cmd, "set-ids") && na == 2) {
        bad = parse_ul(a[0], 0xFFFF, &v1) || parse_ul(a[1], 0xFFFF, &v2);
        if (!bad)
            st = save4_set_trainer_ids(&s, (uint16_t)v1, (uint16_t)v2);
    } else if (!strcmp(cmd, "set-badges") && na == 1) {
        bad = parse_ul(a[0], 0xFF, &v1);
        if (!bad)
            st = save4_set_badges(&s, (uint8_t)v1);
    } else if (!strcmp(cmd, "set-kanto-badges") && na == 1) {
        bad = parse_ul(a[0], 0xFF, &v1);
        if (!bad)
            st = save4_set_kanto_badges(&s, (uint8_t)v1);
    } else if (!strcmp(cmd, "set-item") && na == 4) {
        int pocket = -1;
        for (int i = 0; i < SAVE4_POCKET_COUNT; i++)
            if (!strcmp(a[0], save4_pocket_name((save4_pocket)i)))
                pocket = i;
        if (pocket < 0) {
            fprintf(stderr, "%s: unknown pocket '%s'\n", prog, a[0]);
            bad = 1;
        } else {
            bad = parse_ul(a[1], (unsigned long)save4_pocket_capacity(&s, (save4_pocket)pocket), &v1) || v1 == 0 ||
                  parse_ul(a[2], 0xFFFF, &v2) || parse_ul(a[3], 999, &v3);
            if (!bad)
                st = save4_set_bag_slot(&s, (save4_pocket)pocket, (int)v1 - 1, (uint16_t)v2, (uint16_t)v3);
        }
    } else if (!strcmp(cmd, "set-flag") && na == 2) {
        bad = lookup_id(&s, a[0], &v1) || parse_ul(a[1], 1, &v2);
        if (!bad)
            st = save4_flag_set(&s, (uint16_t)v1, v2 != 0);
    } else if (!strcmp(cmd, "set-var") && na == 2) {
        bad = lookup_id(&s, a[0], &v1) || parse_ul(a[1], 0xFFFF, &v2);
        if (!bad)
            st = save4_var_set(&s, (uint16_t)v1, (uint16_t)v2);
    } else if (!strcmp(cmd, "set-dex") && na == 2) {
        bad = parse_ul(a[0], SAVE4_DEX_MAX, &v1);
        if (!bad) {
            if (!strcmp(a[1], "none"))
                st = save4_dex_set(&s, (uint16_t)v1, false, false);
            else if (!strcmp(a[1], "seen"))
                st = save4_dex_set(&s, (uint16_t)v1, true, false);
            else if (!strcmp(a[1], "caught"))
                st = save4_dex_set(&s, (uint16_t)v1, true, true);
            else
                bad = 1;
        }
    } else if (!strcmp(cmd, "set-national-dex") && na == 1) {
        bad = parse_ul(a[0], 1, &v1);
        if (!bad)
            st = save4_dex_set_national(&s, v1 != 0);
    } else if (!strcmp(cmd, "set-dex-obtained") && na == 1) {
        bad = parse_ul(a[0], 1, &v1);
        if (!bad)
            st = save4_dex_set_obtained(&s, v1 != 0);
    } else if (!strcmp(cmd, "set-mystery-gift") && na == 1) {
        bad = parse_ul(a[0], 1, &v1);
        if (!bad)
            st = save4_mg_set_unlocked(&s, v1 != 0);
    } else if (!strcmp(cmd, "add-gift") && na == 1) {
        uint8_t *gift;
        size_t glen;
        if (read_file(a[0], &gift, &glen) != 0) {
            save4_free(&s);
            return EXIT_USAGE;
        }
        const char *why;
        uint16_t type = glen >= 2 ? (uint16_t)(gift[0] | gift[1] << 8) : 0;
        if (save4_mg_validate(gift, glen, &why) != SAVE4_OK) {
            fprintf(stderr, "%s: %s: %s\n", prog, a[0], why);
            bad = 1;
        } else if (!save4_mg_type_supported(s.game, type)) {
            fprintf(stderr, "%s: %s: %s gifts (type %u) cannot be delivered in %s\n", prog, a[0],
                    gift_type_name(type), type, save4_game_name(s.game));
            bad = 1;
        } else {
            st = save4_mg_add(&s, gift, glen);
        }
        free(gift);
    } else if (!strcmp(cmd, "remove-gift") && na == 1) {
        bad = parse_ul(a[0], SAVE4_WONDERCARD_SLOTS, &v1) || v1 == 0;
        if (!bad)
            st = save4_mg_remove_card(&s, (int)v1 - 1);
    } else if (!strcmp(cmd, "set-box-name") && na == 2) {
        bad = parse_ul(a[0], SAVE4_BOX_COUNT, &v1) || v1 == 0;
        if (!bad)
            st = save4_set_box_name(&s, (int)v1 - 1, a[1]);
    } else if (!strcmp(cmd, "set-move") && na == 4) {
        unsigned long v4 = 0;
        bad = parse_ul(a[1], 5, &v1) || parse_ul(a[2], 3, &v2) || parse_ul(a[3], 0xFFFF, &v4);
        if (!bad)
            st = set_move(&s, a[0], v1, v2, v4);
    } else if (!strcmp(cmd, "set-level") && na == 3) {
        bad = parse_ul(a[1], 5, &v1) || parse_ul(a[2], 100, &v2) || v2 == 0;
        if (!bad)
            st = set_level(&s, a[0], v1, v2);
    } else if (!strcmp(cmd, "heal-party") && na == 1) {
        st = heal_party(&s, a[0]);
    } else if (!strcmp(cmd, "add-mon") && na >= 3 && na <= 7) {
        bad = parse_ul(a[1], 493, &v1) || v1 == 0 || parse_ul(a[2], 100, &v2) || v2 == 0;
        if (!bad)
            st = add_mon(&s, a[0], v1, v2, a + 3, na - 3);
    } else {
        save4_free(&s);
        return usage();
    }
    if (bad) {
        save4_free(&s);
        return EXIT_VALUE;
    }
    if ((rc = edit_status(st)) != 0) {
        save4_free(&s);
        return rc;
    }

    size_t len;
    const uint8_t *img = save4_image(&s, &len);
    if (!out_path) {
        /* back up the original bytes before overwriting */
        uint8_t *orig;
        size_t olen;
        char bak[4096];
        if (snprintf(bak, sizeof(bak), "%s.bak", path) >= (int)sizeof(bak) || read_file(path, &orig, &olen) != 0) {
            save4_free(&s);
            return EXIT_USAGE;
        }
        int w = write_file(bak, orig, olen);
        free(orig);
        if (w != 0) {
            save4_free(&s);
            return EXIT_USAGE;
        }
        out_path = path;
    }
    rc = write_file(out_path, img, len) == 0 ? 0 : EXIT_USAGE;
    save4_free(&s);
    return rc;
}

int main(int argc, char **argv)
{
    if (argc < 2)
        return usage();
    const char *cmd = argv[1];
    if (!strcmp(cmd, "dump")) {
        if (argc == 3)
            return cmd_dump(NULL, argv[2]);
        if (argc == 4)
            return cmd_dump(argv[2], argv[3]);
        return usage();
    }
    if (!strcmp(cmd, "verify"))
        return argc == 3 ? cmd_verify(argv[2]) : usage();
    if (!strcmp(cmd, "gamedata"))
        return argc == 3 ? cmd_gamedata(argv[2]) : usage();
    if (!strncmp(cmd, "set-", 4) || !strcmp(cmd, "add-gift") || !strcmp(cmd, "remove-gift") ||
        !strcmp(cmd, "add-mon") || !strcmp(cmd, "heal-party"))
        return cmd_edit(argc, argv);
    return usage();
}
