/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * np_save5: inspect and edit Pokémon Black/White saves (np_save4's
 * commands and JSON shapes, for the Gen 5 save library save5).
 *
 *   np_save5 dump [<rom.nds>] <save.sav>        JSON summary (names from ROM)
 *   np_save5 gamedata <rom.nds>                  JSON battle tables: species
 *                                                types, moves, type chart
 *   np_save5 verify <save.sav>                   checksum / copy report
 *   np_save5 set-money <save> <n>
 *   np_save5 set-name <save> <name>
 *   np_save5 set-ids <save> <tid> <sid>
 *   np_save5 set-badges <save> <mask>
 *   np_save5 set-item <save> <pocket> <slot> <item> <qty>
 *   np_save5 set-flag <save> <id> <0|1>
 *   np_save5 set-var <save> <id> <value>
 *   np_save5 set-dex <save> <species> none|seen|caught
 *   np_save5 set-national-dex <save> <0|1>
 *   np_save5 add-gift <save> <gift.pgf>
 *   np_save5 remove-gift <save> <card slot 1-12>
 *   np_save5 set-box-name <save> <box 1-24> <name>
 *   np_save5 set-location <save> <zone> <x> <y> <z>       where CONTINUE starts
 *   np_save5 add-mon <save> <rom.nds> <species> <level> [move...]   party Pokemon
 *   np_save5 set-mon <save> <rom.nds> <slot> <species> <level> [move...]   a party slot replaced
 *   np_save5 set-level <save> <rom.nds> <slot> <level>   a party Pokemon's level (stats recalculated)
 *   np_save5 set-held <save> <slot> <item>   a party Pokemon's held item (0: none)
 *
 * np_save4's set-coins, set-dex-obtained and set-mystery-gift have no
 * Black/White counterpart (no coin case; the Pokédex and Mystery Gift need
 * no unlock bit save5 knows of).
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
#include "save5/save5.h"

#define EXIT_USAGE 1
#define EXIT_VALUE 2
#define EXIT_SAVE 3

static const char *prog = "np_save5";

static int usage(void)
{
    fprintf(stderr,
            "usage:\n"
            "  %s dump [<rom.nds>] <save.sav>\n"
            "  %s gamedata <rom.nds>\n"
            "  %s verify <save.sav>\n"
            "  %s set-money <save> <n>\n"
            "  %s set-name <save> <name>\n"
            "  %s set-ids <save> <tid> <sid>\n"
            "  %s set-badges <save> <mask>\n"
            "  %s set-item <save> <pocket> <slot> <item> <qty>\n"
            "      pockets: items key_items tms_hms medicine berries\n"
            "  %s set-flag <save> <id> <0|1>\n"
            "  %s set-var <save> <id> <value>\n"
            "  %s set-dex <save> <species> none|seen|caught\n"
            "  %s set-national-dex <save> <0|1>\n"
            "  %s add-gift <save> <gift.pgf>\n"
            "  %s remove-gift <save> <card slot 1-12>\n"
            "  %s set-box-name <save> <box 1-24> <name>\n"
            "  %s set-location <save> <zone> <x> <y> <z>\n"
            "  %s add-mon <save> <rom.nds> <species> <level> [move id...]\n"
            "  %s set-mon <save> <rom.nds> <party slot 0-5> <species> <level> [move id...]\n"
            "  %s set-level <save> <rom.nds> <party slot 0-5> <level 1-100>\n"
            "  %s set-held <save> <party slot 0-5> <item id, 0 for none>\n"
            "edits accept a trailing `-o <out.sav>`; otherwise the save is\n"
            "rewritten in place after backing it up to <save>.bak\n",
            prog, prog, prog, prog, prog, prog, prog, prog, prog, prog, prog, prog, prog, prog, prog, prog, prog, prog,
            prog, prog);
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

/* Copy summary plus one line per block that fails in either copy. */
static void report_blocks(FILE *o, const save5 *s)
{
    static const char *copy[] = {"primary", "backup"};
    for (int c = 0; c < 2; c++) {
        const save5_copy_state *cs = &s->copies[c];
        fprintf(o, "  %s copy (0x%05X): %s (save %u; footer %s, checksum table %s, %d bad block%s)\n", copy[c],
                c ? SAVE5_COPY_OFFSET : 0, cs->valid ? "valid" : "INVALID", cs->save_counter,
                cs->footer_ok ? "ok" : "BAD", cs->table_ok ? "ok" : "BAD", cs->bad_blocks,
                cs->bad_blocks == 1 ? "" : "s");
        if (!cs->table_ok && cs->footer_ok)
            fprintf(o, "    checksum table: stored 0x%04X, computed 0x%04X\n", cs->stored_crc, cs->computed_crc);
        if (!cs->footer_ok)
            continue;
        for (int b = 0; b < SAVE5_DATA_BLOCKS; b++) {
            const save5_block_state *bs = &s->blocks[b];
            if (bs->valid[c])
                continue;
            fprintf(o, "    block %d (%s, 0x%05X+0x%X): stored 0x%04X, mirror 0x%04X, computed 0x%04X\n", b,
                    save5_block_name(b), bs->offset, bs->size, bs->stored_crc[c], bs->mirror_crc[c],
                    bs->computed_crc[c]);
        }
    }
}

static int load_save(const char *path, save5 *s)
{
    uint8_t *data;
    size_t len;
    if (read_file(path, &data, &len) != 0)
        return EXIT_USAGE;
    save5_status st = save5_load(s, data, len);
    free(data);
    if (st != SAVE5_OK) {
        fprintf(stderr, "%s: error: %s: %s\n", prog, path, save5_status_str(st));
        if (st == SAVE5_ERR_CHECKSUM)
            report_blocks(stderr, s);
        save5_free(s);
        return EXIT_SAVE;
    }
    if (s->load_result == SAVE5_LOAD_RECOVERED)
        fprintf(stderr, "%s: warning: %s: one save copy is corrupt; using the other\n", prog, path);
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

static const char *jbool(bool v) { return v ? "true" : "false"; }

static const nd_names *g_names;

static void jname(FILE *o, nd_text_kind kind, uint32_t id)
{
    jstr(o, g_names ? nd_name(g_names, kind, id) : NULL);
}

/* Ball id -> item id: balls 1-16 share their item ids, the HGSS balls 17-24
 * are items 492-499 and the Dream Ball (25) is item 576. */
static uint32_t ball_item(uint8_t ball)
{
    return ball <= 16 ? ball : ball <= 24 ? 492u + (ball - 17u) : ball == 25 ? 576u : 0;
}

static const char *gift_type_name(uint8_t type)
{
    return type == SAVE5_MG_POKEMON ? "pokemon" : type == SAVE5_MG_ITEM ? "item" : type == SAVE5_MG_POWER ? "power" : NULL;
}

static void dump_mystery(FILE *o, const save5 *s)
{
    fputs("  \"mystery_gift\": {\"cards\": [", o);
    int n = 0;
    for (int slot = 0; slot < SAVE5_MG_SLOTS; slot++) {
        uint8_t card[SAVE5_PGF_SIZE];
        bool used = false;
        if (save5_mg_get_card(s, slot, card, &used) != SAVE5_OK || !used)
            continue;
        char title[128];
        save5_text_decode(card + 0x60, SAVE5_PGF_TITLE_LEN, title, sizeof title);
        fprintf(o, "%s{\"slot\": %d, \"type\": %u, \"type_name\": ", n++ ? ", " : "", slot + 1, card[0xB3]);
        jstr(o, gift_type_name(card[0xB3]));
        fprintf(o, ", \"id\": %u, \"title\": ", card[0xB0] | card[0xB1] << 8);
        jstr(o, title);
        fprintf(o, ", \"used\": %s}", jbool(card[0xB4] & 2));
    }
    fputs("], \"received\": [", o);
    for (unsigned id = 0, k = 0; id < SAVE5_MG_FLAGS; id++) {
        bool r = false;
        if (save5_mg_get_received(s, (uint16_t)id, &r) == SAVE5_OK && r)
            fprintf(o, "%s%u", k++ ? ", " : "", id);
    }
    fputs("]}\n", o);
}

static void dump_mon(FILE *o, const pkm5 *p, save5_status dst, int full)
{
    pkm5_info i;
    pkm5_info_get(p, &i);
    fprintf(o, "\"species\": %u, \"species_name\": ", i.species);
    jname(o, ND_TEXT_SPECIES, i.species);
    fputs(", \"nickname\": ", o);
    jstr(o, i.nickname);
    fprintf(o, ", \"is_egg\": %s, \"checksum_ok\": %s", jbool(i.is_egg), jbool(dst == SAVE5_OK));
    if (i.has_party_data)
        fprintf(o, ", \"level\": %u", i.level);
    fprintf(o, ", \"exp\": %u, \"pid\": \"0x%08X\", \"shiny\": %s", i.exp, i.pid, jbool(i.shiny));
    if (!full)
        return;
    fprintf(o, ", \"nature\": ");
    jname(o, ND_TEXT_NATURES, i.nature);
    fprintf(o, ", \"ability\": {\"id\": %u, \"name\": ", i.ability);
    jname(o, ND_TEXT_ABILITIES, i.ability);
    fprintf(o, ", \"hidden\": %s}, \"held_item\": {\"id\": %u, \"name\": ", jbool(i.hidden_ability), i.held_item);
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
    jname(o, ND_TEXT_ITEMS, ball_item(i.ball));
    fputc('}', o);
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

static int cmd_dump(const char *rom_path, const char *save_path)
{
    save5 s;
    int rc = load_save(save_path, &s);
    if (rc)
        return rc;

    nd_names names;
    nd_rom rom;
    FILE *rf = NULL;
    int have_rom = 0;
    if (rom_path) {
        if (open_rom(rom_path, &rf, &rom) != 0) {
            save5_free(&s);
            return EXIT_USAGE;
        }
        have_rom = 1;
        /* Black and White share their text, so either ROM names either save. */
        if (nd_game_gen(rom.game) != 5) {
            fprintf(stderr, "%s: warning: ROM is %s (%s) but the save is %s; names omitted\n", prog,
                    nd_game_name(rom.game), rom.gamecode, save5_game_name(s.game));
        } else {
            nd_status st = nd_names_load(&names, &rom);
            if (st != ND_OK) {
                fprintf(stderr, "%s: %s: cannot read text from ROM (%s)\n", prog, rom_path, nd_status_str(st));
                nd_rom_close(&rom);
                fclose(rf);
                save5_free(&s);
                return EXIT_USAGE;
            }
            g_names = &names;
        }
    }

    FILE *o = stdout;
    save5_trainer t;
    save5_get_trainer(&s, &t);
    fprintf(o, "{\n  \"game\": \"%s\",\n  \"load_result\": \"%s\",\n", save5_game_name(s.game),
            s.load_result == SAVE5_LOAD_OK ? "ok" : "recovered");
    if (have_rom) {
        fputs("  \"rom\": {\"title\": ", o);
        jstr(o, rom.title);
        fputs(", \"gamecode\": ", o);
        jstr(o, rom.gamecode);
        fprintf(o, ", \"version\": %u},\n", rom.rom_version);
    } else {
        fputs("  \"rom\": null,\n", o);
    }
    fprintf(o, "  \"active\": \"%s\",\n  \"copies\": [", s.active ? "backup" : "primary");
    for (int c = 0; c < 2; c++) {
        const save5_copy_state *cs = &s.copies[c];
        fprintf(o, "%s{\"name\": \"%s\", \"valid\": %s, \"save_counter\": %u, \"footer_ok\": %s, \"table_ok\": %s, "
                   "\"bad_blocks\": [",
                c ? ", " : "", c ? "backup" : "primary", jbool(cs->valid), cs->save_counter, jbool(cs->footer_ok),
                jbool(cs->table_ok));
        for (int b = 0, n = 0; b < SAVE5_DATA_BLOCKS; b++)
            if (!s.blocks[b].valid[c])
                fprintf(o, "%s%d", n++ ? ", " : "", b);
        fputs("]}", o);
    }
    fputs("],\n  \"trainer\": {\"name\": ", o);
    jstr(o, t.name);
    int nbadges = 0;
    for (int i = 0; i < 8; i++)
        nbadges += (t.badges >> i) & 1;
    fprintf(o,
            ", \"tid\": %u, \"sid\": %u, \"gender\": \"%s\", \"money\": %u, \"badges\": %d, \"badge_mask\": %u, "
            "\"play_time\": \"%u:%02u:%02u\", \"language\": %u, \"version\": %u},\n",
            t.tid, t.sid, t.gender ? "female" : "male", t.money, nbadges, t.badges, t.play_hours, t.play_minutes,
            t.play_seconds, t.language, t.version);
    save5_location loc;
    save5_get_location(&s, &loc);
    fprintf(o, "  \"location\": {\"map\": %u, \"name\": ", loc.map);
    jstr(o, g_names ? nd_zone_name(g_names, loc.map) : NULL);
    fprintf(o, ", \"x\": %u, \"y\": %u, \"z\": %u},\n", loc.x, loc.y, loc.z);
    save5_game_time gt;
    save5_get_last_saved(&s, &gt);
    fprintf(o, "  \"last_saved\": {\"date\": \"%04u-%02u-%02u\", \"time\": \"%02u:%02u\"},\n", gt.year, gt.month,
            gt.day, gt.hour, gt.minute);
    /* Event state: every set flag, every nonzero var keyed by its id. */
    fputs("  \"flags\": [", o);
    for (unsigned id = 0, n = 0; id < SAVE5_NUM_FLAGS; id++) {
        bool on = false;
        if (save5_flag_get(&s, (uint16_t)id, &on) == SAVE5_OK && on)
            fprintf(o, "%s%u", n++ ? ", " : "", id);
    }
    fputs("],\n  \"vars\": {", o);
    for (unsigned id = SAVE5_VARS_START, n = 0; id < SAVE5_VARS_START + SAVE5_NUM_VARS; id++) {
        uint16_t v = 0;
        if (save5_var_get(&s, (uint16_t)id, &v) == SAVE5_OK && v)
            fprintf(o, "%s\"%u\": %u", n++ ? ", " : "", id, v);
    }
    fputs("},\n", o);

    fputs("  \"party\": [", o);
    uint8_t count = save5_party_count(&s);
    for (int i = 0; i < count; i++) {
        pkm5 p;
        save5_status pst = save5_get_party(&s, i, &p);
        fprintf(o, "%s\n    {\"slot\": %d, ", i ? "," : "", i + 1);
        dump_mon(o, &p, pst, 1);
        fputc('}', o);
    }
    fputs(count ? "\n  ],\n" : "],\n", o);

    fprintf(o, "  \"current_box\": %u,\n  \"boxes\": [", save5_current_box(&s) + 1);
    for (int b = 0; b < SAVE5_BOX_COUNT; b++) {
        char name[64];
        save5_get_box_name(&s, b, name, sizeof(name));
        fprintf(o, "%s\n    {\"box\": %d, \"name\": ", b ? "," : "", b + 1);
        jstr(o, name);
        fputs(", \"mons\": [", o);
        int n = 0;
        for (int slot = 0; slot < SAVE5_BOX_SLOTS; slot++) {
            pkm5 p;
            save5_status pst = save5_get_box_mon(&s, b, slot, &p);
            if (pst == SAVE5_OK && pkm5_is_empty(&p))
                continue;
            fprintf(o, "%s{\"slot\": %d, ", n ? ", " : "", slot + 1);
            dump_mon(o, &p, pst, 0);
            fputc('}', o);
            n++;
        }
        fprintf(o, "], \"count\": %d}", n);
    }
    fputs("\n  ],\n  \"bag\": {", o);
    for (int pk = 0; pk < SAVE5_POCKET_COUNT; pk++) {
        fprintf(o, "%s\n    \"%s\": [", pk ? "," : "", save5_pocket_name((save5_pocket)pk));
        int n = 0;
        for (int slot = 0; slot < save5_pocket_capacity((save5_pocket)pk); slot++) {
            uint16_t item, qty;
            save5_get_bag_slot(&s, (save5_pocket)pk, slot, &item, &qty);
            if (!item)
                continue;
            fprintf(o, "%s{\"item\": %u, \"name\": ", n++ ? ", " : "", item);
            jname(o, ND_TEXT_ITEMS, item);
            fprintf(o, ", \"qty\": %u}", qty);
        }
        fputc(']', o);
    }
    int seen = 0, caught = 0;
    for (int sp = 1; sp <= SAVE5_DEX_MAX; sp++) {
        bool sv = false, cv = false;
        save5_dex_get(&s, (uint16_t)sp, &sv, &cv);
        seen += sv;
        caught += cv;
    }
    bool national = false;
    save5_dex_get_national(&s, &national);
    fprintf(o, "\n  },\n  \"pokedex\": {\"seen\": %d, \"caught\": %d, \"national\": %s", seen, caught, jbool(national));
    for (int list = 0; list < 2; list++) {
        fputs(list ? ", \"caught_list\": [" : ", \"seen_list\": [", o);
        for (int sp = 1, n = 0; sp <= SAVE5_DEX_MAX; sp++) {
            bool sv = false, cv = false;
            save5_dex_get(&s, (uint16_t)sp, &sv, &cv);
            if (list ? cv : sv)
                fprintf(o, "%s%d", n++ ? ", " : "", sp);
        }
        fputc(']', o);
    }
    fputs("},\n", o);
    dump_mystery(o, &s);
    fputs("}\n", o);

    if (g_names)
        nd_names_free(&names);
    g_names = NULL;
    if (have_rom) {
        nd_rom_close(&rom);
        fclose(rf);
    }
    save5_free(&s);
    return 0;
}

/* gamedata: np_save4 gamedata's shape for a Black/White ROM.
 * species[i] = [type1, type2, ability1, ability2 (0: one ability)]; moves[i] = [effect, class (0 physical,
 * 1 special, 2 status), power, type, accuracy, pp, priority, range (Gen 4 RANGE_* bits)];
 * type_chart[attack][defend] = multiplier x10 over the game's type_count types (17: no ??? type). */
static int cmd_gamedata(const char *rom_path)
{
    FILE *rf;
    nd_rom rom;
    if (open_rom(rom_path, &rf, &rom) != 0)
        return EXIT_USAGE;
    if (nd_game_gen(rom.game) != 5)
        fprintf(stderr, "%s: warning: %s is %s, not Black/White\n", prog, rom_path, nd_game_name(rom.game));
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
    for (int a = 0; a < gd.type_count; a++) {
        printf("%s[", a ? "," : "");
        for (int d = 0; d < gd.type_count; d++)
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
    save5 s;
    int rc = load_save(path, &s);
    if (rc)
        return rc;
    printf("%s: %s save, %s\n", path, save5_game_name(s.game),
           s.load_result == SAVE5_LOAD_OK ? "all checksums valid" : "recovered from the other copy");
    report_blocks(stdout, &s);
    int synced = 0;
    for (int b = 0; b < SAVE5_DATA_BLOCKS; b++)
        synced += s.blocks[b].synced;
    printf("  active: %s; %d of %d blocks identical in both copies\n", s.active ? "backup" : "primary", synced,
           SAVE5_DATA_BLOCKS);
    save5_free(&s);
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

static int edit_status(save5_status st)
{
    if (st == SAVE5_OK)
        return 0;
    fprintf(stderr, "%s: error: %s\n", prog, save5_status_str(st));
    return EXIT_VALUE;
}

/* add-mon / set-mon: a party Pokemon as np_save4 add-mon makes one (base
 * friendship, first ability, the growth rate's EXP for the level, stats from
 * base stats with IVs 20 and no EVs, nature pid % 25, full HP, a Poke Ball,
 * the trainer as OT, this game as origin). Moves are given by id. `slot` < 0
 * appends it to the party (add-mon); otherwise it replaces the Pokemon in
 * that slot (set-mon: the e2e boosts' way to strengthen a full party). */
static save5_status put_mon(save5 *s, const char *rom_path, long slot, unsigned long species, unsigned long level,
                            char **moves, int nmoves)
{
    FILE *rf;
    nd_rom rom;
    if (open_rom(rom_path, &rf, &rom) != 0)
        return SAVE5_ERR_ARG;
    nd_gamedata gd;
    nd_names names;
    save5_status st = SAVE5_ERR_ARG;
    int gen5 = nd_game_gen(rom.game) == 5;
    int have_gd = gen5 && nd_gamedata_load(&gd, &rom) == ND_OK;
    int have_names = have_gd && nd_names_load(&names, &rom) == ND_OK;
    const nd_species *sp = have_gd ? nd_species_get(&gd, (uint32_t)species) : NULL;
    uint8_t count = save5_party_count(s);
    save5_trainer t;
    if (!sp || !sp->valid || !have_names || save5_get_trainer(s, &t) != SAVE5_OK
        || (slot < 0 ? count >= SAVE5_PARTY_MAX : slot >= count)) {
        if (slot < 0)
            fprintf(stderr, "%s: cannot add species %lu (not a Black/White ROM, unknown species or full party)\n",
                    prog, species);
        else
            fprintf(stderr, "%s: cannot put species %lu in slot %ld (not a Black/White ROM, unknown species or no "
                    "Pokemon in that slot)\n", prog, species, slot);
        goto out;
    }
    pkm5 p;
    memset(&p, 0, sizeof p);
    p.party = true;
    /* A fixed personality per species, level and trainer: runs repeat. */
    const uint32_t pid = (uint32_t)species * 2654435761u ^ (uint32_t)level * 40503u ^ ((uint32_t)t.sid << 16 | t.tid);
    const uint8_t nature = (uint8_t)(pid % 25);
    pkm5_set_pid(&p, pid);
    pkm5_set_species(&p, (uint16_t)species);
    pkm5_set_ot_ids(&p, t.tid, t.sid);
    pkm5_set_exp(&p, nd_exp_for_level(&gd, (uint32_t)species, (uint32_t)level));
    pkm5_set_friendship(&p, sp->base_friendship);
    pkm5_set_ability(&p, sp->abilities[0], false);
    pkm5_set_language(&p, t.language);
    pkm5_set_nature(&p, nature);
    for (int i = 0; i < nmoves && i < 4; i++) {
        unsigned long mv;
        if (parse_ul(moves[i], 0xFFFF, &mv) != 0)
            goto out;
        pkm5_set_move(&p, i, (uint16_t)mv, nd_move_base_pp(&gd, (uint32_t)mv), 0);
    }
    uint8_t ivs[6], evs[6] = {0};
    for (int i = 0; i < 6; i++) {
        ivs[i] = 20;
        pkm5_set_iv(&p, i, 20);
    }
    uint8_t gender = sp->gender_ratio == 255 ? 2 : sp->gender_ratio == 254 ? 1 : sp->gender_ratio == 0 ? 0
                     : (pid & 0xFF) < sp->gender_ratio ? 1 : 0;
    pkm5_set_gender_form(&p, gender, 0);
    const char *name = nd_name(&names, ND_TEXT_SPECIES, (uint32_t)species);
    if (!name || pkm5_set_nickname(&p, name, false) != SAVE5_OK || pkm5_set_ot_name(&p, t.name) != SAVE5_OK) {
        st = SAVE5_ERR_ENCODE;
        goto out;
    }
    pkm5_set_origin_game(&p, t.version);
    pkm5_set_met(&p, 0, (uint8_t)level, 4 /* Poke Ball */, t.gender);
    uint16_t stats[6];
    pkm5_calc_stats(sp->base, ivs, evs, (uint8_t)level, nature, species == 292, stats);
    pkm5_set_party_stats(&p, (uint8_t)level, stats[0], stats, 0);
    st = save5_set_party(s, slot < 0 ? count : (int)slot, &p);
    if (st == SAVE5_OK && slot < 0)
        st = save5_set_party_count(s, (uint8_t)(count + 1));
out:
    if (have_names)
        nd_names_free(&names);
    if (have_gd)
        nd_gamedata_free(&gd);
    nd_rom_close(&rom);
    fclose(rf);
    return st;
}

/* set-level: party slot `slot` at `level`: its growth rate's EXP for the
 * level, its stats recalculated from its own IVs, EVs and nature, its HP the
 * new maximum (the e2e boosts' party-level; np_save4 set-level's rule).
 * Moves, friendship and the rest stay as they are: no level-up moves are
 * learned and no evolution runs. */
static save5_status set_level(save5 *s, const char *rom_path, unsigned long slot, unsigned long level)
{
    FILE *rf;
    nd_rom rom;
    if (open_rom(rom_path, &rf, &rom) != 0)
        return SAVE5_ERR_ARG;
    nd_gamedata gd;
    save5_status st = SAVE5_ERR_ARG;
    int have_gd = nd_game_gen(rom.game) == 5 && nd_gamedata_load(&gd, &rom) == ND_OK;
    pkm5 p;
    pkm5_info info;
    const nd_species *sp = NULL;
    if (have_gd && slot < save5_party_count(s) && save5_get_party(s, (int)slot, &p) == SAVE5_OK
        && !pkm5_is_empty(&p)) {
        pkm5_info_get(&p, &info);
        sp = nd_species_get(&gd, info.species);
    }
    if (!sp || !sp->valid || info.is_egg) {
        fprintf(stderr, "%s: no party Pokemon in slot %lu (an egg, or not a Black/White ROM)\n", prog, slot);
        goto out;
    }
    pkm5_set_exp(&p, nd_exp_for_level(&gd, info.species, (uint32_t)level));
    uint16_t stats[6];
    pkm5_calc_stats(sp->base, info.ivs, info.evs, (uint8_t)level, info.nature, info.species == 292, stats);
    pkm5_set_party_stats(&p, (uint8_t)level, stats[0], stats, info.status);
    st = save5_set_party(s, (int)slot, &p);
out:
    if (have_gd)
        nd_gamedata_free(&gd);
    nd_rom_close(&rom);
    fclose(rf);
    return st;
}

/* set-held: party slot `slot` holds `item` (0: nothing). The item id is not
 * checked against the ROM's item table; the rest of the Pokemon is kept (the
 * e2e recipes' party-item: an Exp. Share on a member that does not fight). */
static save5_status set_held(save5 *s, unsigned long slot, unsigned long item)
{
    pkm5 p;
    if (slot >= save5_party_count(s) || save5_get_party(s, (int)slot, &p) != SAVE5_OK || pkm5_is_empty(&p)) {
        fprintf(stderr, "%s: no party Pokemon in slot %lu\n", prog, slot);
        return SAVE5_ERR_ARG;
    }
    pkm5_set_held_item(&p, (uint16_t)item);
    return save5_set_party(s, (int)slot, &p);
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

    save5 s;
    int rc = load_save(path, &s);
    if (rc)
        return rc;
    unsigned long v1 = 0, v2 = 0, v3 = 0;
    save5_status st = SAVE5_ERR_ARG;
    int bad = 0;

    if (!strcmp(cmd, "set-money") && na == 1) {
        bad = parse_ul(a[0], SAVE5_MONEY_MAX, &v1);
        if (!bad)
            st = save5_set_money(&s, (uint32_t)v1);
    } else if (!strcmp(cmd, "set-name") && na == 1) {
        st = save5_set_trainer_name(&s, a[0]);
    } else if (!strcmp(cmd, "set-ids") && na == 2) {
        bad = parse_ul(a[0], 0xFFFF, &v1) || parse_ul(a[1], 0xFFFF, &v2);
        if (!bad)
            st = save5_set_trainer_ids(&s, (uint16_t)v1, (uint16_t)v2);
    } else if (!strcmp(cmd, "set-badges") && na == 1) {
        bad = parse_ul(a[0], 0xFF, &v1);
        if (!bad)
            st = save5_set_badges(&s, (uint8_t)v1);
    } else if (!strcmp(cmd, "set-item") && na == 4) {
        int pocket = -1;
        for (int i = 0; i < SAVE5_POCKET_COUNT; i++)
            if (!strcmp(a[0], save5_pocket_name((save5_pocket)i)))
                pocket = i;
        if (pocket < 0) {
            fprintf(stderr, "%s: unknown pocket '%s'\n", prog, a[0]);
            bad = 1;
        } else {
            bad = parse_ul(a[1], (unsigned long)save5_pocket_capacity((save5_pocket)pocket), &v1) || v1 == 0 ||
                  parse_ul(a[2], 0xFFFF, &v2) || parse_ul(a[3], save5_pocket_max_qty((save5_pocket)pocket), &v3);
            if (!bad)
                st = save5_set_bag_slot(&s, (save5_pocket)pocket, (int)v1 - 1, (uint16_t)v2, (uint16_t)v3);
        }
    } else if (!strcmp(cmd, "set-flag") && na == 2) {
        bad = parse_ul(a[0], SAVE5_NUM_FLAGS - 1, &v1) || parse_ul(a[1], 1, &v2);
        if (!bad)
            st = save5_flag_set(&s, (uint16_t)v1, v2 != 0);
    } else if (!strcmp(cmd, "set-var") && na == 2) {
        bad = parse_ul(a[0], 0xFFFF, &v1) || parse_ul(a[1], 0xFFFF, &v2);
        if (!bad)
            st = save5_var_set(&s, (uint16_t)v1, (uint16_t)v2);
    } else if (!strcmp(cmd, "set-dex") && na == 2) {
        bad = parse_ul(a[0], SAVE5_DEX_MAX, &v1);
        if (!bad) {
            if (!strcmp(a[1], "none"))
                st = save5_dex_set(&s, (uint16_t)v1, false, false);
            else if (!strcmp(a[1], "seen"))
                st = save5_dex_set(&s, (uint16_t)v1, true, false);
            else if (!strcmp(a[1], "caught"))
                st = save5_dex_set(&s, (uint16_t)v1, true, true);
            else
                bad = 1;
        }
    } else if (!strcmp(cmd, "set-national-dex") && na == 1) {
        bad = parse_ul(a[0], 1, &v1);
        if (!bad)
            st = save5_dex_set_national(&s, v1 != 0);
    } else if (!strcmp(cmd, "add-gift") && na == 1) {
        uint8_t *gift;
        size_t glen;
        if (read_file(a[0], &gift, &glen) != 0) {
            save5_free(&s);
            return EXIT_USAGE;
        }
        const char *why;
        if (save5_mg_validate(gift, glen, &why) != SAVE5_OK) {
            fprintf(stderr, "%s: %s: %s\n", prog, a[0], why);
            bad = 1;
        } else {
            st = save5_mg_add(&s, gift, glen);
        }
        free(gift);
    } else if (!strcmp(cmd, "remove-gift") && na == 1) {
        bad = parse_ul(a[0], SAVE5_MG_SLOTS, &v1) || v1 == 0;
        if (!bad)
            st = save5_mg_remove_card(&s, (int)v1 - 1);
    } else if (!strcmp(cmd, "set-box-name") && na == 2) {
        bad = parse_ul(a[0], SAVE5_BOX_COUNT, &v1) || v1 == 0;
        if (!bad)
            st = save5_set_box_name(&s, (int)v1 - 1, a[1]);
    } else if (!strcmp(cmd, "set-location") && na == 4) {
        unsigned long z;
        bad = parse_ul(a[0], 0xFFFF, &v1) || parse_ul(a[1], 0xFFFF, &v2) || parse_ul(a[2], 0xFFFF, &v3) ||
              parse_ul(a[3], 0xFFFF, &z);
        if (!bad) {
            save5_location loc = {(uint32_t)v1, (uint16_t)v2, (uint16_t)v3, (uint16_t)z};
            st = save5_set_location(&s, &loc);
        }
    } else if (!strcmp(cmd, "add-mon") && na >= 3 && na <= 7) {
        bad = parse_ul(a[1], SAVE5_DEX_MAX, &v1) || v1 == 0 || parse_ul(a[2], 100, &v2) || v2 == 0;
        if (!bad)
            st = put_mon(&s, a[0], -1, v1, v2, a + 3, na - 3);
    } else if (!strcmp(cmd, "set-mon") && na >= 4 && na <= 8) {
        bad = parse_ul(a[1], SAVE5_PARTY_MAX - 1, &v3) || parse_ul(a[2], SAVE5_DEX_MAX, &v1) || v1 == 0 ||
              parse_ul(a[3], 100, &v2) || v2 == 0;
        if (!bad)
            st = put_mon(&s, a[0], (long)v3, v1, v2, a + 4, na - 4);
    } else if (!strcmp(cmd, "set-level") && na == 3) {
        bad = parse_ul(a[1], SAVE5_PARTY_MAX - 1, &v1) || parse_ul(a[2], 100, &v2) || v2 == 0;
        if (!bad)
            st = set_level(&s, a[0], v1, v2);
    } else if (!strcmp(cmd, "set-held") && na == 2) {
        bad = parse_ul(a[0], SAVE5_PARTY_MAX - 1, &v1) || parse_ul(a[1], 0xFFFF, &v2);
        if (!bad)
            st = set_held(&s, v1, v2);
    } else {
        save5_free(&s);
        return usage();
    }
    if (bad) {
        save5_free(&s);
        return EXIT_VALUE;
    }
    if ((rc = edit_status(st)) != 0) {
        save5_free(&s);
        return rc;
    }

    size_t len;
    const uint8_t *img = save5_image(&s, &len);
    if (!out_path) {
        /* back up the original bytes before overwriting */
        uint8_t *orig;
        size_t olen;
        char bak[4096];
        if (snprintf(bak, sizeof(bak), "%s.bak", path) >= (int)sizeof(bak) || read_file(path, &orig, &olen) != 0) {
            save5_free(&s);
            return EXIT_USAGE;
        }
        int w = write_file(bak, orig, olen);
        free(orig);
        if (w != 0) {
            save5_free(&s);
            return EXIT_USAGE;
        }
        out_path = path;
    }
    rc = write_file(out_path, img, len) == 0 ? 0 : EXIT_USAGE;
    save5_free(&s);
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
        !strcmp(cmd, "add-mon"))
        return cmd_edit(argc, argv);
    return usage();
}
