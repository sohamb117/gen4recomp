/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * ndsdata: read game data out of a user-supplied Nintendo DS ROM.
 *
 *  - NDS ROM: header, FNT/FAT, files by path or FAT id, over a read callback
 *  - NARC archives (BTAF/BTNF/GMIF)
 *  - LZ77 type 0x10 / 0x11 and backward LZ (BLZ, compressed overlays)
 *    decompression
 *  - Gen 4 message banks (pret/pokeplatinum src/message.c scheme) -> UTF-8
 *  - Gen 4 character set (generated from the decomp's charmap.txt)
 *  - Gen 5 message banks (UTF-16 text) -> UTF-8
 *  - Game-aware name tables (species, moves, items, abilities, natures,
 *    locations) and battle tables for Diamond, Pearl, Platinum, Black and
 *    White
 *
 * No dependencies beyond libc. All returned heap memory is owned by the
 * caller unless noted and is released with free() or the matching *_free().
 */
#ifndef NDSDATA_NDSDATA_H
#define NDSDATA_NDSDATA_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum nd_status {
    ND_OK = 0,
    ND_ERR_IO,        /* read callback failed / short read */
    ND_ERR_FORMAT,    /* malformed ROM, NARC, LZ stream or message bank */
    ND_ERR_NOT_FOUND, /* path or member does not exist */
    ND_ERR_NOMEM,
    ND_ERR_RANGE,     /* index or size out of range / buffer too small */
    ND_ERR_UNSUPPORTED
} nd_status;

const char *nd_status_str(nd_status st);

/* ------------------------------------------------------------------ ROM */

/* Read exactly `len` bytes at `offset` into `dst`. Return 0 on success. */
typedef int (*nd_read_fn)(void *user, uint64_t offset, void *dst, size_t len);

/* Ready-made callback for a stdio FILE* passed as `user`. */
int nd_read_stdio(void *user, uint64_t offset, void *dst, size_t len);

typedef enum nd_game {
    ND_GAME_UNKNOWN = 0,
    ND_GAME_DIAMOND,
    ND_GAME_PEARL,
    ND_GAME_PLATINUM,
    ND_GAME_BLACK, /* gamecode IRB* */
    ND_GAME_WHITE  /* gamecode IRA* */
} nd_game;

const char *nd_game_name(nd_game g);
/* 4 for Diamond/Pearl/Platinum, 5 for Black/White, 0 when unknown. */
int nd_game_gen(nd_game g);

typedef struct nd_rom {
    nd_read_fn read;
    void *user;
    uint64_t size;           /* total ROM size in bytes (as supplied) */
    char title[13];          /* header 0x000, NUL-terminated */
    char gamecode[5];        /* header 0x00C, e.g. "CPUE" */
    uint8_t rom_version;     /* header 0x01E */
    nd_game game;            /* derived from gamecode[0..2] */
    uint32_t fnt_offset, fnt_size; /* header 0x040 / 0x044 */
    uint32_t fat_offset, fat_size; /* header 0x048 / 0x04C */
    uint32_t file_count;     /* fat_size / 8 */
    uint8_t *fnt;            /* cached file name table */
    uint8_t *fat;            /* cached file allocation table */
} nd_rom;

nd_status nd_rom_open(nd_rom *rom, nd_read_fn read, void *user, uint64_t size);
void nd_rom_close(nd_rom *rom);

/* Raw read through the callback with bounds checking. */
nd_status nd_rom_read(const nd_rom *rom, uint64_t offset, void *dst, size_t len);

/* Byte extent of a FAT entry. */
nd_status nd_rom_file_extent(const nd_rom *rom, uint32_t file_id, uint32_t *offset, uint32_t *length);

/* Resolve "dir/sub/file.ext" (leading '/' optional) to a FAT id via the FNT. */
nd_status nd_rom_find(const nd_rom *rom, const char *path, uint32_t *file_id);

/* Read a whole file into a malloc'd buffer. */
nd_status nd_rom_load_file(const nd_rom *rom, uint32_t file_id, uint8_t **out, size_t *out_len);
nd_status nd_rom_load_path(const nd_rom *rom, const char *path, uint8_t **out, size_t *out_len);

/* Read one NARC member straight from the ROM without loading the archive
 * (reads the NARC header + BTAF, then just the member). */
nd_status nd_rom_load_narc_member(const nd_rom *rom, uint32_t narc_file_id, uint32_t member,
                                  uint8_t **out, size_t *out_len);

/* ----------------------------------------------------------------- NARC */

typedef struct nd_narc {
    const uint8_t *data;   /* whole archive (borrowed) */
    size_t size;
    uint32_t count;        /* BTAF file count */
    const uint8_t *btaf;   /* first BTAF entry (start,end pairs) */
    const uint8_t *btnf;   /* BTNF section payload (unused by lookups) */
    uint32_t btnf_size;
    const uint8_t *gmif;   /* GMIF image start */
    uint32_t gmif_size;
} nd_narc;

nd_status nd_narc_parse(nd_narc *narc, const uint8_t *data, size_t size);
nd_status nd_narc_member(const nd_narc *narc, uint32_t index, const uint8_t **ptr, size_t *len);

/* ----------------------------------------------------------------- LZ77 */

/* Decompressed size from the header of an LZ 0x10 / 0x11 stream. */
nd_status nd_lz_size(const uint8_t *src, size_t src_len, size_t *out_size);
/* Decompress into dst (must hold nd_lz_size() bytes). */
nd_status nd_lz_decompress(const uint8_t *src, size_t src_len, uint8_t *dst, size_t dst_len);
/* Convenience: allocate and decompress. */
nd_status nd_lz_decompress_alloc(const uint8_t *src, size_t src_len, uint8_t **out, size_t *out_len);

/* Backward LZ ("BLZ", the ARM9 / overlay compression of the DS SDK): the
 * stream decompresses from its end, described by an 8-byte footer (u32
 * header length << 24 | compressed length, u32 size increase). Allocates
 * the decompressed image; a footer with a zero size increase means the data
 * is stored uncompressed and is copied as is. */
nd_status nd_blz_decompress(const uint8_t *src, size_t src_len, uint8_t **out, size_t *out_len);

/* ------------------------------------------------------------ Gen 4 text */

/* Decode in-game charcodes to UTF-8. Stops at 0xFFFF (EOS) or after `n`
 * codes. Control sequences (0xFFFE ...) render as "{CMD args}" like the
 * decomp's msgenc; unknown codes render as "\\x%04X". Always NUL-terminates
 * when cap > 0. Returns the full length needed (excluding NUL), snprintf-like. */
size_t g4_text_decode(const uint16_t *codes, size_t n, char *out, size_t cap);
/* Same, returning a malloc'd string (NULL on OOM). */
char *g4_text_decode_alloc(const uint16_t *codes, size_t n);

/* Encode UTF-8 into charcodes (greedy longest match against the charmap),
 * appending 0xFFFF. `cap` counts u16 slots including the terminator.
 * ND_ERR_RANGE if it does not fit, ND_ERR_FORMAT on an unmappable char. */
nd_status g4_text_encode(const char *utf8, uint16_t *out, size_t cap, size_t *out_len);

/* ------------------------------------------------------------ Gen 5 text */

/* Decode Gen 5 text (UTF-16 code units) to UTF-8. Stops at 0xFFFF or after
 * `n` units. 0xFFFE renders as "\n", a 0xF000 control (0xF000, command,
 * argc, args...) as "{CMD_XXXX a, b}", the game's gender glyphs 0x246D /
 * 0x246E as U+2642 / U+2640; unpaired surrogates and 0xF100 render as
 * "\\x%04X". snprintf-like, like g4_text_decode. */
size_t g5_text_decode(const uint16_t *codes, size_t n, char *out, size_t cap);
char *g5_text_decode_alloc(const uint16_t *codes, size_t n);

/* -------------------------------------------------------- message banks */

typedef struct nd_msgbank {
    const uint8_t *data;  /* borrowed */
    size_t size;
    uint16_t count;
    uint16_t seed;
} nd_msgbank;

nd_status nd_msgbank_parse(nd_msgbank *bank, const uint8_t *data, size_t size);
/* Decrypted charcodes of entry `index`. *len receives the code count; if
 * `dst` is NULL or too small, only the length is reported (ND_ERR_RANGE). */
nd_status nd_msgbank_get_codes(const nd_msgbank *bank, uint32_t index, uint16_t *dst, size_t cap, size_t *len);
/* malloc'd UTF-8 string for entry `index`. */
nd_status nd_msgbank_get_utf8(const nd_msgbank *bank, uint32_t index, char **out);

/* ------------------------------------------------------------ name lists */

typedef enum nd_text_kind {
    ND_TEXT_SPECIES = 0,
    ND_TEXT_MOVES,
    ND_TEXT_ITEMS,
    ND_TEXT_ABILITIES,
    ND_TEXT_NATURES,
    ND_TEXT_LOCATIONS,         /* met locations 0..1999 (Gen 4), 0..29999 (Gen 5) */
    ND_TEXT_SPECIAL_LOCATIONS, /* met locations 2000..2999 (Gen 4), 30001.. (Gen 5) */
    ND_TEXT_EVENT_LOCATIONS,   /* met locations 3000.. (Gen 4), 40001.. (Gen 5) */
    ND_TEXT_PERSON_LOCATIONS,  /* Gen 5 met locations 60001.. (Day-Care Couple...); empty in Gen 4 */
    ND_TEXT_KIND_COUNT
} nd_text_kind;

/* Archive path of the main message NARC for a game ("msgdata/pl_msg.narc"). */
const char *nd_msg_narc_path(nd_game game);
/* Bank (NARC member) index of a name list for a game, or -1. */
int nd_text_bank(nd_game game, nd_text_kind kind);

typedef struct nd_names {
    nd_game game;
    char **list[ND_TEXT_KIND_COUNT];
    uint32_t count[ND_TEXT_KIND_COUNT];
    uint16_t *zone_location; /* Gen 5: ND_TEXT_LOCATIONS index per zone id */
    uint32_t zone_count;
} nd_names;

/* Load every name list for the ROM's game. */
nd_status nd_names_load(nd_names *names, const nd_rom *rom);
void nd_names_free(nd_names *names);
/* Borrowed string or NULL if out of range / not loaded. */
const char *nd_name(const nd_names *names, nd_text_kind kind, uint32_t id);
/* Met/egg location id -> name. Gen 4: routes 2000/3000 ranges like
 * StringTemplate_SetMetLocationName in pokeplatinum. Gen 5: 0.., 30001..,
 * 40001.., 60001.. (see gen4_names.c). NULL when unknown. */
const char *nd_location_name(const nd_names *names, uint32_t location);
/* Location name of an overworld zone (map) id, as a Gen 5 save stores the
 * player's position. NULL for Gen 4 or an unknown zone. */
const char *nd_zone_name(const nd_names *names, uint32_t zone);
/* Nature name for a PID (nature = pid % 25). */
const char *nd_nature_name(const nd_names *names, uint32_t pid);

/* ------------------------------------------------------------ game data */

#define ND_EXP_RATES 8

/* The species fields a save editor needs (pokeplatinum SpeciesData). */
typedef struct nd_species {
    uint8_t valid;
    uint8_t base[6];        /* HP Atk Def Spe SpA SpD */
    uint8_t types[2];
    uint8_t gender_ratio;   /* 0 male only .. 254 female only, 255 genderless */
    uint8_t base_friendship;
    uint8_t exp_rate;       /* index into exp[] */
    uint8_t abilities[2];   /* ability ids; [1] is 0 when there is one (Gen 5: the two non-hidden abilities) */
} nd_species;

/* A move's battle data (pokeplatinum MoveTable). */
typedef struct nd_move {
    uint16_t effect;        /* battle effect id (Gen 5: the move's effect sequence id) */
    uint8_t cls;            /* 0 physical, 1 special, 2 status */
    uint8_t power;
    uint8_t type;
    uint8_t accuracy;       /* 0: never misses */
    uint8_t pp;             /* base PP */
    int8_t priority;
    uint16_t range;         /* targets: 0 one, RANGE_* bits (8: every adjacent battler, the ally too);
                             * Gen 5 targets are mapped onto the Gen 4 bits */
} nd_move;

#define ND_TYPES 18 /* type ids 0..17 (Gen 4: 9 is the ??? type; Gen 5 uses 0..16) */

typedef struct nd_gamedata {
    nd_game game;
    uint32_t species_count; /* personal NARC members (forms after 493 / 649) */
    nd_species *species;
    uint32_t exp[ND_EXP_RATES][101]; /* total exp for levels 0..100 */
    uint32_t move_count;
    nd_move *moves;         /* per move id */
    /* damage multiplier x10 (0, 5, 10, 20), [attacking type][defending
     * type]; all 10 unless type_chart_ok, and 10 in rows/columns at or past
     * type_count */
    uint8_t type_chart[ND_TYPES][ND_TYPES];
    uint8_t type_chart_ok;
    uint8_t type_count;     /* types in use: 18 in Gen 4, 17 in Gen 5 */
} nd_gamedata;

/* Species, experience, move tables and the type chart for the ROM's game. */
nd_status nd_gamedata_load(nd_gamedata *gd, const nd_rom *rom);
void nd_gamedata_free(nd_gamedata *gd);
/* NULL when out of range. */
const nd_species *nd_species_get(const nd_gamedata *gd, uint32_t species);
/* Total experience a species needs for `level` (0 if unknown species). */
uint32_t nd_exp_for_level(const nd_gamedata *gd, uint32_t species, uint32_t level);
/* The level `exp` reaches, 1..100, as Pokemon_GetLevel computes it. */
uint32_t nd_level_for_exp(const nd_gamedata *gd, uint32_t species, uint32_t exp);
/* NULL when out of range. */
const nd_move *nd_move_get(const nd_gamedata *gd, uint32_t move);
uint8_t nd_move_base_pp(const nd_gamedata *gd, uint32_t move);
/* The type chart's multiplier x10 for one attacking and one defending type. */
uint8_t nd_type_multiplier(const nd_gamedata *gd, uint32_t attack, uint32_t defend);

#ifdef __cplusplus
}
#endif

#endif
