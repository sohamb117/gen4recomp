/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * save5: Pokémon Black / White save data (512 KiB DS flash image).
 *
 * Layout (Project Pokémon "BW Save Structure", PKHeX SaveBlockAccessor5BW;
 * checked against real Black and White saves): the image holds two copies
 * of the game data, primary at 0x00000 and backup at 0x24000. A copy is 69
 * data blocks at fixed offsets (box names, 24 PC boxes, bag, party, trainer,
 * position, ..., Mystery Gift, event work, money/badges, Pokédex, ...) and a
 * checksum block at 0x23F00. Every data block is followed by a 4-byte footer
 * {u16 write counter, u16 CRC-16-CCITT of the block}; the checksum block
 * holds a mirror of every data block's CRC (u16 at 0x23F00 + 2 * index) and
 * ends in a 0x10-byte footer {u32 save counter, u32 size 0x23F9C, u32 magic
 * 0x31053527, u16 0, u16 CRC-16-CCITT of the mirror table}.
 *
 * A copy is valid when its footer magic/size match and every block's CRC
 * matches both its footer and its mirror entry and the mirror table's own
 * CRC matches. The active copy is the valid one; when both are valid, the
 * one with the higher save counter (the game writes both copies, so they
 * normally agree; ties pick the primary). Loading fails with
 * SAVE5_ERR_CHECKSUM when neither copy is valid; the per-block state is still
 * filled in for reporting.
 *
 * Edits are applied to the active copy: the block's footer CRC, its mirror
 * entry and the mirror table CRC are recomputed. A block whose bytes (and
 * footer) were identical in both copies at load time is kept identical: the
 * edit is mirrored to the other copy too. Every other byte of the image (the
 * extra data past 0x48000, emulator trailers after 512 KiB, block padding)
 * is preserved, so an unmodified load/store round-trips byte-exact.
 *
 * Strings are UTF-16LE code units ending in 0xFFFF (the game's ♂/♀ are
 * 0x246D/0x246E); save5 converts them to and from UTF-8.
 */
#ifndef SAVE5_SAVE5_H
#define SAVE5_SAVE5_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SAVE5_IMAGE_SIZE 0x80000u
#define SAVE5_COPY_OFFSET 0x24000u    /* backup copy */
#define SAVE5_DATA_BLOCKS 69          /* data blocks per copy */
#define SAVE5_CHECKSUM_BLOCK 0x23F00u /* mirror table */
#define SAVE5_CHECKSUM_TABLE 0x8Cu    /* mirror table bytes (70 u16, the last unused) */
#define SAVE5_COPY_USED 0x23F9Cu      /* bytes of a copy covered by blocks and footers */
#define SAVE5_MAGIC 0x31053527u

#define SAVE5_PARTY_MAX 6
#define SAVE5_BOX_COUNT 24
#define SAVE5_BOX_SLOTS 30
#define SAVE5_BOX_NAME_MAX 8    /* characters */
#define SAVE5_TRAINER_NAME_MAX 7
#define SAVE5_NUM_FLAGS 2912    /* 0xB60 event flags */
#define SAVE5_NUM_VARS 318      /* 0x13E saved event vars */
#define SAVE5_VARS_START 0x4000 /* script id of the first saved var */
#define SAVE5_MONEY_MAX 9999999u
#define SAVE5_DEX_MAX 649

#define PKM5_BOX_SIZE 136
#define PKM5_PARTY_SIZE 220

typedef enum save5_status {
    SAVE5_OK = 0,
    SAVE5_ERR_ARG,
    SAVE5_ERR_SIZE,         /* image shorter than 512 KiB */
    SAVE5_ERR_EMPTY,        /* neither copy has a checksum footer (blank/erased image) */
    SAVE5_ERR_UNKNOWN_GAME, /* footer present but not a Black/White save */
    SAVE5_ERR_CHECKSUM,     /* no copy passes every checksum */
    SAVE5_ERR_RANGE,
    SAVE5_ERR_NOMEM,
    SAVE5_ERR_ENCODE,       /* text not representable */
    SAVE5_ERR_PKM_CHECKSUM, /* Pokémon data checksum mismatch */
    SAVE5_ERR_NOSPACE,      /* no free slot */
    SAVE5_ERR_FORMAT        /* malformed input file (e.g. a Wonder Card) */
} save5_status;

const char *save5_status_str(save5_status st);

typedef enum save5_game {
    SAVE5_GAME_UNKNOWN = 0,
    SAVE5_GAME_BLACK, /* trainer version byte 21 */
    SAVE5_GAME_WHITE  /* trainer version byte 20 */
} save5_game;

/* "black" / "white" / "unknown" (ndsdata's nd_game_name spelling). */
const char *save5_game_name(save5_game g);

/* Block indices of the blocks save5 interprets. */
enum {
    SAVE5_BLK_BOX_NAMES = 0,
    SAVE5_BLK_BOX1 = 1, /* boxes 1-24: blocks 1-24 */
    SAVE5_BLK_BAG = 25,
    SAVE5_BLK_PARTY = 26,
    SAVE5_BLK_TRAINER = 27,
    SAVE5_BLK_POSITION = 28,
    SAVE5_BLK_MYSTERY = 34,
    SAVE5_BLK_EVENT = 45,
    SAVE5_BLK_MISC = 52, /* money, badges */
    SAVE5_BLK_DEX = 55
};

/* Short description of a data block ("trainer", "box 3", ...; "unknown"
 * for blocks whose contents are undocumented). */
const char *save5_block_name(int block);

typedef struct save5_block_state {
    uint32_t offset;           /* within a copy */
    uint32_t size;             /* data bytes, footer excluded */
    bool valid[2];             /* primary / backup: CRC == footer == mirror */
    uint16_t stored_crc[2];    /* footer */
    uint16_t mirror_crc[2];    /* checksum-table entry */
    uint16_t computed_crc[2];
    uint16_t counter[2];       /* footer write counter */
    bool synced;               /* identical in both copies (edits are mirrored) */
} save5_block_state;

typedef struct save5_copy_state {
    bool footer_ok;    /* magic and size */
    bool table_ok;     /* mirror table CRC */
    bool valid;        /* footer, table and every block */
    int bad_blocks;
    uint32_t save_counter;
    uint16_t stored_crc, computed_crc; /* mirror table */
} save5_copy_state;

typedef enum save5_load_result {
    SAVE5_LOAD_OK = 0,      /* both copies valid */
    SAVE5_LOAD_RECOVERED    /* one copy bad, the other used */
} save5_load_result;

typedef struct save5 {
    uint8_t *img; /* owned */
    size_t len;
    save5_game game;
    int active;   /* 0 primary, 1 backup */
    save5_copy_state copies[2];
    save5_block_state blocks[SAVE5_DATA_BLOCKS];
    save5_load_result load_result;
} save5;

/* CRC-16-CCITT (poly 0x1021, init 0xFFFF, no reflection, no xorout). */
uint16_t save5_crc16(const void *data, size_t len);

save5_status save5_load(save5 *s, const uint8_t *data, size_t len);
save5_status save5_clone(const save5 *src, save5 *dst);
void save5_free(save5 *s);
const uint8_t *save5_image(const save5 *s, size_t *len);
/* Re-run validation and copy selection on the current bytes. */
save5_status save5_revalidate(save5 *s);
/* Absolute image offset of a block in the active copy. */
uint32_t save5_block_base(const save5 *s, int block);
/* Recompute a block's checksums after changing its bytes in the active copy
 * (edit helpers call it). */
void save5_commit_block(save5 *s, int block);
/* Recompute every block's checksums in both copies (synthetic images). */
void save5_fix_all_checksums(uint8_t *img);

/* Gen 5 string <-> UTF-8. `units` counts u16 slots including the 0xFFFF
 * terminator; slots after it are zeroed. SAVE5_ERR_RANGE if too long,
 * SAVE5_ERR_ENCODE for characters outside the BMP or control characters. */
size_t save5_text_decode(const uint8_t *src, size_t units, char *out, size_t cap);
save5_status save5_text_encode(const char *utf8, uint8_t *dst, size_t units);

/* --------------------------------------------------------- trainer */

typedef struct save5_trainer {
    char name[64];       /* UTF-8 */
    uint16_t name_raw[8];
    uint16_t tid, sid;
    uint32_t money;
    uint8_t gender;      /* 0 male, 1 female */
    uint8_t language;
    uint8_t version;     /* 20 White, 21 Black */
    uint8_t country, region;
    uint8_t badges;      /* bitmask, 8 Unova badges */
    uint16_t play_hours;
    uint8_t play_minutes, play_seconds;
} save5_trainer;

save5_status save5_get_trainer(const save5 *s, save5_trainer *t);
save5_status save5_set_money(save5 *s, uint32_t money);
save5_status save5_set_trainer_name(save5 *s, const char *utf8);
save5_status save5_set_trainer_ids(save5 *s, uint16_t tid, uint16_t sid);
save5_status save5_set_gender(save5 *s, uint8_t gender);
save5_status save5_set_badges(save5 *s, uint8_t mask);
save5_status save5_set_play_time(save5 *s, uint16_t hours, uint8_t minutes, uint8_t seconds);

/* Where the player was saved (trainer position block): zone id and the
 * tile under the player (fx32 positions, integer part). */
typedef struct save5_location {
    uint32_t map;
    uint16_t x, y, z;
} save5_location;
save5_status save5_get_location(const save5 *s, save5_location *loc);
/* Moves the saved player to zone `map`, standing at the centre of tile (x, z)
 * at height y (tiles): the game's CONTINUE places the player there. */
save5_status save5_set_location(save5 *s, const save5_location *loc);

/* The date and time of the last save (trainer block +0x28, packed: year
 * bits 0-6 (since 2000), month 7-10, day 11-15, hour 16-20, minute 21-26). */
typedef struct save5_game_time {
    uint16_t year;
    uint8_t month, day, hour, minute;
} save5_game_time;
save5_status save5_get_last_saved(const save5 *s, save5_game_time *t);

/* ----------------------------------------------------------- Pokémon */

/* Decrypted Pokémon: header, blocks A B C D in canonical order, then for
 * party mons the 84-byte battle-stats tail. */
typedef struct pkm5 {
    uint8_t data[PKM5_PARTY_SIZE];
    bool party; /* tail present */
} pkm5;

typedef struct pkm5_info {
    uint32_t pid;
    uint16_t checksum;
    bool checksum_ok;
    uint16_t species;
    uint16_t held_item;
    uint16_t tid, sid;
    uint32_t exp;
    uint8_t friendship;
    uint8_t ability;
    uint8_t markings;
    uint8_t language;
    uint8_t evs[6];   /* HP Atk Def Spe SpA SpD */
    uint8_t contest[6];
    uint16_t moves[4];
    uint8_t pp[4];
    uint8_t pp_ups[4];
    uint8_t ivs[6];   /* HP Atk Def Spe SpA SpD */
    bool is_egg;
    bool has_nickname;
    bool fateful;
    uint8_t gender;   /* 0 male, 1 female, 2 genderless */
    uint8_t form;
    uint8_t nature;   /* stored (Gen 5 does not derive it from the PID) */
    bool hidden_ability;
    char nickname[64];
    uint8_t origin_game;
    char ot_name[64];
    uint16_t egg_location;
    uint16_t met_location;
    uint8_t met_level;
    uint8_t ot_gender;
    uint8_t ball;
    uint8_t pokerus;
    bool shiny;
    /* party tail */
    bool has_party_data;
    uint32_t status;
    uint8_t level;
    uint16_t hp;
    uint16_t stats[6]; /* MaxHP Atk Def Spe SpA SpD */
} pkm5_info;

/* Block order index ((pid >> 13) & 31) % 24, as in Gen 4. */
uint32_t pkm5_shuffle_index(uint32_t pid);
/* Sum of the 64 u16 words of blocks A-D. */
uint16_t pkm5_calc_checksum(const pkm5 *p);
/* Decrypt/unshuffle 136 (box) or 220 (party) bytes: blocks with the PRNG
 * seeded by the checksum, the party tail seeded by the PID. Returns
 * SAVE5_ERR_PKM_CHECKSUM (with `out` still filled) on mismatch. */
save5_status pkm5_decrypt(const uint8_t *enc, size_t len, pkm5 *out);
/* Recompute checksum, shuffle and encrypt into 136 or 220 bytes. */
save5_status pkm5_encrypt(pkm5 *p, uint8_t *out, size_t len);
bool pkm5_is_empty(const pkm5 *p); /* species == 0 */
void pkm5_info_get(const pkm5 *p, pkm5_info *info);

void pkm5_set_pid(pkm5 *p, uint32_t pid);
void pkm5_set_species(pkm5 *p, uint16_t species);
void pkm5_set_held_item(pkm5 *p, uint16_t item);
void pkm5_set_ot_ids(pkm5 *p, uint16_t tid, uint16_t sid);
void pkm5_set_exp(pkm5 *p, uint32_t exp);
void pkm5_set_friendship(pkm5 *p, uint8_t v);
void pkm5_set_ability(pkm5 *p, uint8_t ability, bool hidden);
void pkm5_set_language(pkm5 *p, uint8_t lang);
void pkm5_set_ev(pkm5 *p, int stat, uint8_t v);
void pkm5_set_move(pkm5 *p, int slot, uint16_t move, uint8_t pp, uint8_t pp_ups);
void pkm5_set_iv(pkm5 *p, int stat, uint8_t v);
void pkm5_set_is_egg(pkm5 *p, bool egg);
void pkm5_set_gender_form(pkm5 *p, uint8_t gender, uint8_t form);
void pkm5_set_nature(pkm5 *p, uint8_t nature);
save5_status pkm5_set_nickname(pkm5 *p, const char *utf8, bool is_nickname);
save5_status pkm5_set_ot_name(pkm5 *p, const char *utf8);
void pkm5_set_origin_game(pkm5 *p, uint8_t game);
void pkm5_set_met(pkm5 *p, uint16_t location, uint8_t level, uint8_t ball, uint8_t ot_gender);
void pkm5_set_party_stats(pkm5 *p, uint8_t level, uint16_t hp, const uint16_t stats[6], uint32_t status);
/* Stats (MaxHP Atk Def Spe SpA SpD) from base stats (same order), IVs,
 * EVs, level and nature (+10% stat nature/5, -10% nature%5 over Atk Def Spe
 * SpA SpD). */
void pkm5_calc_stats(const uint8_t base[6], const uint8_t ivs[6], const uint8_t evs[6], uint8_t level,
                     uint8_t nature, bool shedinja, uint16_t out[6]);

uint8_t save5_party_count(const save5 *s);
save5_status save5_get_party(const save5 *s, int slot, pkm5 *out);
/* Writes slot 0..5; the count is separate (save5_set_party_count). */
save5_status save5_set_party(save5 *s, int slot, pkm5 *p);
save5_status save5_set_party_count(save5 *s, uint8_t count);

save5_status save5_get_box_mon(const save5 *s, int box, int slot, pkm5 *out);
save5_status save5_set_box_mon(save5 *s, int box, int slot, pkm5 *p);
save5_status save5_clear_box_mon(save5 *s, int box, int slot);
save5_status save5_get_box_name(const save5 *s, int box, char *utf8, size_t cap);
save5_status save5_set_box_name(save5 *s, int box, const char *utf8);
uint32_t save5_current_box(const save5 *s);

/* -------------------------------------------------------------- bag */

typedef enum save5_pocket {
    SAVE5_POCKET_ITEMS = 0,
    SAVE5_POCKET_KEY_ITEMS,
    SAVE5_POCKET_TMHM,
    SAVE5_POCKET_MEDICINE,
    SAVE5_POCKET_BERRIES,
    SAVE5_POCKET_COUNT
} save5_pocket;

const char *save5_pocket_name(save5_pocket p); /* items key_items tms_hms medicine berries */
int save5_pocket_capacity(save5_pocket p);
/* Largest quantity a slot of the pocket holds (999, or 1 for key items and TMs/HMs). */
uint16_t save5_pocket_max_qty(save5_pocket p);
save5_status save5_get_bag_slot(const save5 *s, save5_pocket p, int slot, uint16_t *item, uint16_t *qty);
save5_status save5_set_bag_slot(save5 *s, save5_pocket p, int slot, uint16_t item, uint16_t qty);

/* ----------------------------------------------------------- Pokédex */

/* caught = caught bit; seen = any of the four seen (gender/shiny) bits. */
save5_status save5_dex_get(const save5 *s, uint16_t species, bool *seen, bool *caught);
/* seen sets the male/genderless seen bit (and its displayed bit when the
 * species shows no form yet); !seen clears every seen and displayed bit. */
save5_status save5_dex_set(save5 *s, uint16_t species, bool seen, bool caught);
/* The National Pokédex (Pokédex block +4 bit 0). */
save5_status save5_dex_get_national(const save5 *s, bool *obtained);
save5_status save5_dex_set_national(save5 *s, bool obtained);

/* ------------------------------------------------------ event data */

/* Flag ids 0..SAVE5_NUM_FLAGS-1 (bit id%8 of byte id/8 after the vars). */
save5_status save5_flag_get(const save5 *s, uint16_t id, bool *value);
save5_status save5_flag_set(save5 *s, uint16_t id, bool value);
/* Saved var ids SAVE5_VARS_START .. +SAVE5_NUM_VARS-1. */
save5_status save5_var_get(const save5 *s, uint16_t id, uint16_t *value);
save5_status save5_var_set(save5 *s, uint16_t id, uint16_t value);

/* ------------------------------------------------------ Mystery Gift */
/* The Mystery Gift block: 0xA90 bytes encrypted with the Gen 4/5 PRNG
 * (seed = the u32 after them): 2048 received-gift flags (0x100 bytes), then
 * 12 Wonder Cards (.pgf, 0xCC bytes each). */

#define SAVE5_PGF_SIZE 0xCC
#define SAVE5_MG_SLOTS 12
#define SAVE5_MG_FLAGS 2048
#define SAVE5_PGF_TITLE_LEN 37 /* u16 slots */

enum { SAVE5_MG_POKEMON = 1, SAVE5_MG_ITEM = 2, SAVE5_MG_POWER = 3 }; /* card type, pgf +0xB3 */

/* Checks a .pgf image (size, gift type, card id). */
save5_status save5_mg_validate(const uint8_t *data, size_t len, const char **why);
/* Stores a card in the first free slot and sets its received flag.
 * SAVE5_ERR_NOSPACE when all 12 slots are used. */
save5_status save5_mg_add(save5 *s, const uint8_t *pgf, size_t len);
/* `used` = the slot holds a card. */
save5_status save5_mg_get_card(const save5 *s, int slot, uint8_t card[SAVE5_PGF_SIZE], bool *used);
/* Empties the slot; its received flag stays set. */
save5_status save5_mg_remove_card(save5 *s, int slot);
save5_status save5_mg_get_received(const save5 *s, uint16_t id, bool *received);
save5_status save5_mg_set_received(save5 *s, uint16_t id, bool received);

#ifdef __cplusplus
}
#endif

#endif
