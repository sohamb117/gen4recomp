/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * save4: Diamond / Pearl / Platinum save data (512 KiB DS flash image).
 *
 * The image holds two copies (primary at 0x00000, backup at 0x40000), each
 * with a "general" block and a "storage" (PC boxes) block, each ending in a
 * 0x14-byte footer {u32 saveCounter, u32 blockCounter, u32 size,
 * u32 signature 0x20060623, u8 blockID, u16 CRC-16-CCITT}
 * (pokeplatinum include/savedata.h, src/savedata.c; pokediamond
 * arm9/src/save.c). The newest valid copy of each block is selected with the
 * game's own rules (SaveData_LoadCheck). Edits are applied to that active
 * copy and its footer checksum is recomputed; every other byte of the image
 * (including the stale copy and anything after 512 KiB, e.g. emulator
 * trailers) is preserved.
 *
 * Undo: a save4 owns a private copy of the image. Snapshot with
 * save4_clone() (or keep the bytes from save4_image()) before editing and
 * restore by reloading the snapshot.
 */
#ifndef SAVE4_SAVE4_H
#define SAVE4_SAVE4_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SAVE4_IMAGE_SIZE 0x80000u
#define SAVE4_COPY_SIZE 0x40000u
#define SAVE4_FOOTER_SIZE 0x14u
#define SAVE4_SIGNATURE 0x20060623u

#define SAVE4_PARTY_MAX 6
#define SAVE4_BOX_COUNT 18
#define SAVE4_BOX_SLOTS 30
#define SAVE4_NUM_FLAGS 2912   /* NUM_FLAGS, pokeplatinum include/vars_flags.h */
#define SAVE4_NUM_VARS 288     /* VARS_END - VARS_START, generated/vars_flags.txt */
#define SAVE4_VARS_START 0x4000 /* generated/vars_flags.txt VARS_START */
#define SAVE4_MONEY_MAX 999999u /* MONEY_MAX, src/trainer_info.c */
#define SAVE4_COINS_MAX 50000u  /* MAX_COINS, include/coins.h */
#define SAVE4_DEX_MAX 493

#define PKM4_BOX_SIZE 136   /* sizeof(BoxPokemon) */
#define PKM4_PARTY_SIZE 236 /* sizeof(Pokemon) */

typedef enum save4_status {
    SAVE4_OK = 0,
    SAVE4_ERR_ARG,
    SAVE4_ERR_SIZE,         /* image shorter than 512 KiB */
    SAVE4_ERR_EMPTY,        /* no save footers at all (blank/erased image) */
    SAVE4_ERR_UNKNOWN_GAME, /* footers present but not D/P/Pt sized */
    SAVE4_ERR_CHECKSUM,     /* no copy of a block passes its checksum */
    SAVE4_ERR_RANGE,
    SAVE4_ERR_NOMEM,
    SAVE4_ERR_ENCODE,       /* text not representable in the game charset */
    SAVE4_ERR_PKM_CHECKSUM, /* Pokémon data checksum mismatch */
    SAVE4_ERR_LAYOUT,       /* structure magic mismatch, or not supported for this game */
    SAVE4_ERR_NOSPACE       /* no free slot */
} save4_status;

const char *save4_status_str(save4_status st);

typedef enum save4_game {
    SAVE4_GAME_UNKNOWN = 0,
    SAVE4_GAME_DP,
    SAVE4_GAME_PT
} save4_game;

const char *save4_game_name(save4_game g);

typedef enum save4_block_id {
    SAVE4_BLOCK_GENERAL = 0,
    SAVE4_BLOCK_STORAGE = 1,
    SAVE4_BLOCK_COUNT
} save4_block_id;

typedef struct save4_block_state {
    uint32_t offset;            /* within a copy */
    uint32_t size;              /* including footer */
    bool valid[2];              /* footer + checksum OK in primary/backup */
    uint32_t save_counter[2];
    uint32_t block_counter[2];
    uint16_t stored_crc[2];
    int active;                 /* 0 = primary, 1 = backup */
} save4_block_state;

typedef enum save4_load_result {
    SAVE4_LOAD_OK = 0,      /* LOAD_RESULT_OK */
    SAVE4_LOAD_RECOVERED    /* LOAD_RESULT_CORRUPT: one copy bad, other used */
} save4_load_result;

typedef struct save4_layout save4_layout;

typedef struct save4 {
    uint8_t *img;  /* owned */
    size_t len;
    save4_game game;
    const save4_layout *layout;
    save4_block_state blocks[SAVE4_BLOCK_COUNT];
    save4_load_result load_result;
} save4;

/* CRC-16-CCITT (poly 0x1021, init 0xFFFF, no reflection, no xorout) as in
 * NitroSDK MATH_CalcCRC16CCITT used by CalcCRC16Checksum. */
uint16_t save4_crc16(const void *data, size_t len);

save4_status save4_load(save4 *s, const uint8_t *data, size_t len);
save4_status save4_clone(const save4 *src, save4 *dst);
void save4_free(save4 *s);
const uint8_t *save4_image(const save4 *s, size_t *len);
/* Re-run detection and checksum validation on the current bytes. */
save4_status save4_revalidate(save4 *s);
/* Absolute image offset of a block in its active copy. */
uint32_t save4_block_base(const save4 *s, save4_block_id b);
/* Recompute the active copy footer checksum of a block (edit helpers call it). */
void save4_commit_block(save4 *s, save4_block_id b);

/* --------------------------------------------------------- trainer */

typedef struct save4_trainer {
    char name[64];       /* UTF-8 */
    uint16_t name_raw[8];
    uint16_t tid, sid;
    uint32_t money;
    uint8_t gender;      /* 0 male, 1 female */
    uint8_t language;
    uint8_t badges;      /* bitmask, 8 Sinnoh badges */
    uint8_t game_code;
    bool main_story_cleared;
    bool has_national_dex;
    uint16_t coins;
    uint16_t play_hours;
    uint8_t play_minutes, play_seconds;
} save4_trainer;

save4_status save4_get_trainer(const save4 *s, save4_trainer *t);
save4_status save4_set_money(save4 *s, uint32_t money);
save4_status save4_set_coins(save4 *s, uint16_t coins);
save4_status save4_set_trainer_name(save4 *s, const char *utf8);
save4_status save4_set_trainer_ids(save4 *s, uint16_t tid, uint16_t sid);
save4_status save4_set_gender(save4 *s, uint8_t gender);
save4_status save4_set_badges(save4 *s, uint8_t mask);
save4_status save4_set_play_time(save4 *s, uint16_t hours, uint8_t minutes, uint8_t seconds);

/* ----------------------------------------------------------- Pokémon */

/* Decrypted Pokémon: bytes in canonical order (header, blocks A B C D,
 * then for party mons the 100-byte battle-stats tail). */
typedef struct pkm4 {
    uint8_t data[PKM4_PARTY_SIZE];
    bool party;        /* tail present */
} pkm4;

typedef struct pkm4_info {
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
    uint8_t evs[6];      /* HP Atk Def Spe SpA SpD */
    uint8_t contest[6];
    uint16_t moves[4];
    uint8_t pp[4];
    uint8_t pp_ups[4];
    uint8_t ivs[6];      /* HP Atk Def Spe SpA SpD */
    bool is_egg;
    bool has_nickname;
    bool fateful;
    uint8_t gender;      /* 0 male, 1 female, 2 genderless */
    uint8_t form;
    char nickname[64];   /* UTF-8 */
    uint8_t origin_game;
    char ot_name[64];
    uint16_t egg_location; /* Pt field if set, else D/P field */
    uint16_t met_location;
    uint8_t met_level;
    uint8_t ot_gender;
    uint8_t ball;
    uint8_t pokerus;
    uint8_t nature;      /* pid % 25 */
    bool shiny;
    /* party tail */
    bool has_party_data;
    uint32_t status;
    uint8_t level;
    uint16_t hp;
    uint16_t stats[6];   /* MaxHP Atk Def Spe SpA SpD */
} pkm4_info;

/* Block order index: ((pid & 0x3E000) >> 13) % 24 (BoxPokemon_GetDataBlock). */
uint32_t pkm4_shuffle_index(uint32_t pid);
/* Sum of the 64 u16 words of blocks A-D. */
uint16_t pkm4_calc_checksum(const pkm4 *p);
/* Decrypt/unshuffle 136 (box) or 236 (party) bytes. Returns
 * SAVE4_ERR_PKM_CHECKSUM (with `out` still filled) on checksum mismatch. */
save4_status pkm4_decrypt(const uint8_t *enc, size_t len, pkm4 *out);
/* Recompute checksum, shuffle and encrypt into `len` bytes (136 or 236).
 * A 236-byte encode from a box-only pkm4 writes a zeroed (encrypted) tail. */
save4_status pkm4_encrypt(pkm4 *p, uint8_t *out, size_t len);
bool pkm4_is_empty(const pkm4 *p); /* pid == 0 && species == 0 */
void pkm4_info_get(const pkm4 *p, pkm4_info *info);

/* Field setters on decrypted data (checksum is refreshed by pkm4_encrypt). */
void pkm4_set_pid(pkm4 *p, uint32_t pid);
void pkm4_set_species(pkm4 *p, uint16_t species);
void pkm4_set_held_item(pkm4 *p, uint16_t item);
void pkm4_set_ot_ids(pkm4 *p, uint16_t tid, uint16_t sid);
void pkm4_set_exp(pkm4 *p, uint32_t exp);
void pkm4_set_friendship(pkm4 *p, uint8_t v);
void pkm4_set_ability(pkm4 *p, uint8_t ability);
void pkm4_set_language(pkm4 *p, uint8_t lang);
void pkm4_set_ev(pkm4 *p, int stat, uint8_t v);
void pkm4_set_move(pkm4 *p, int slot, uint16_t move, uint8_t pp, uint8_t pp_ups);
void pkm4_set_iv(pkm4 *p, int stat, uint8_t v);
void pkm4_set_is_egg(pkm4 *p, bool egg);
void pkm4_set_gender_form(pkm4 *p, uint8_t gender, uint8_t form);
save4_status pkm4_set_nickname(pkm4 *p, const char *utf8, bool is_nickname);
save4_status pkm4_set_ot_name(pkm4 *p, const char *utf8);
void pkm4_set_origin_game(pkm4 *p, uint8_t game);
/* Writes both the Pt/HGSS (block B) and D/P (block D) location fields. */
void pkm4_set_met(pkm4 *p, uint16_t location, uint8_t level, uint8_t ball, uint8_t ot_gender);
/* Party tail. */
void pkm4_set_party_stats(pkm4 *p, uint8_t level, uint16_t hp, const uint16_t stats[6], uint32_t status);
/* Party stats (MaxHP Atk Def Spe SpA SpD) from species base stats (same
 * order), IVs, EVs, level and nature, as the game computes them. */
void pkm4_calc_stats(const uint8_t base[6], const uint8_t ivs[6], const uint8_t evs[6], uint8_t level,
                     uint8_t nature, bool shedinja, uint16_t out[6]);

uint8_t save4_party_count(const save4 *s);
save4_status save4_get_party(const save4 *s, int slot, pkm4 *out);
/* Writes slot (0..5). Does not change the party count; see save4_set_party_count. */
save4_status save4_set_party(save4 *s, int slot, pkm4 *p);
save4_status save4_set_party_count(save4 *s, uint8_t count);

save4_status save4_get_box_mon(const save4 *s, int box, int slot, pkm4 *out);
save4_status save4_set_box_mon(save4 *s, int box, int slot, pkm4 *p);
save4_status save4_clear_box_mon(save4 *s, int box, int slot);
save4_status save4_get_box_name(const save4 *s, int box, char *utf8, size_t cap);
save4_status save4_set_box_name(save4 *s, int box, const char *utf8);
uint32_t save4_current_box(const save4 *s);

/* -------------------------------------------------------------- bag */

typedef enum save4_pocket {
    SAVE4_POCKET_ITEMS = 0,
    SAVE4_POCKET_KEY_ITEMS,
    SAVE4_POCKET_TMHM,
    SAVE4_POCKET_MAIL,
    SAVE4_POCKET_MEDICINE,
    SAVE4_POCKET_BERRIES,
    SAVE4_POCKET_BALLS,
    SAVE4_POCKET_BATTLE_ITEMS,
    SAVE4_POCKET_COUNT
} save4_pocket;

const char *save4_pocket_name(save4_pocket p);
int save4_pocket_capacity(save4_pocket p);
save4_status save4_get_bag_slot(const save4 *s, save4_pocket p, int slot, uint16_t *item, uint16_t *qty);
save4_status save4_set_bag_slot(save4 *s, save4_pocket p, int slot, uint16_t item, uint16_t qty);

/* ----------------------------------------------------------- Pokédex */

save4_status save4_dex_get(const save4 *s, uint16_t species, bool *seen, bool *caught);
/* Sets only the seen/caught bits (Pokedex.seenPokemon / caughtPokemon). */
save4_status save4_dex_set(save4 *s, uint16_t species, bool seen, bool caught);

/* ------------------------------------------------------ event data */

/* Flag ids 1..SAVE4_NUM_FLAGS-1 (VarsFlags.flags, bit id%8 of byte id/8). */
save4_status save4_flag_get(const save4 *s, uint16_t id, bool *value);
save4_status save4_flag_set(save4 *s, uint16_t id, bool value);
/* Saved var ids SAVE4_VARS_START .. +SAVE4_NUM_VARS-1. */
save4_status save4_var_get(const save4 *s, uint16_t id, uint16_t *value);
save4_status save4_var_set(save4 *s, uint16_t id, uint16_t value);

/* Platinum flag/var names from pokeplatinum generated/vars_flags.txt
 * (generated at build time). */
struct save4_named_id {
    uint16_t id;
    const char *name;
};
extern const struct save4_named_id save4_pt_flag_names[];
extern const size_t save4_pt_flag_names_count;
extern const struct save4_named_id save4_pt_var_names[];
extern const size_t save4_pt_var_names_count;
/* Returns 0 and fills *id if found. */
int save4_pt_lookup_name(const char *name, uint16_t *id);
const char *save4_pt_flag_name(uint16_t id);

/* ------------------------------------------------------ Mystery Gift */
/* Platinum only for now (D/P offsets are not verified: SAVE4_ERR_LAYOUT). */

#define SAVE4_PGT_SIZE 0x104        /* sizeof(PGT): a .pgt file */
#define SAVE4_WONDERCARD_SIZE 0x358 /* sizeof(WonderCard): a .pcd file */
#define SAVE4_PGT_SLOTS 8
#define SAVE4_WONDERCARD_SLOTS 3
#define SAVE4_MG_ID_MAX 2047        /* id 2047 is the "unlocked" flag */
#define SAVE4_WC_TITLE_LEN 36
#define SAVE4_WC_DESC_LEN 250

/* enum MysteryGiftType, pokeplatinum include/mystery_gift.h */
enum {
    SAVE4_MG_POKEMON = 1,
    SAVE4_MG_EGG,
    SAVE4_MG_ITEM,
    SAVE4_MG_BATTLE_REG,
    SAVE4_MG_DECORATION,
    SAVE4_MG_COSMETICS,
    SAVE4_MG_MANAPHY_EGG,
    SAVE4_MG_MEMBER_CARD, /* Darkrai event */
    SAVE4_MG_OAKS_LETTER, /* Shaymin event */
    SAVE4_MG_AZURE_FLUTE, /* Arceus event */
    SAVE4_MG_POKETCH_APP,
    SAVE4_MG_SECRET_KEY,  /* Rotom event */
    SAVE4_MG_UNKNOWN,
    SAVE4_MG_TYPE_MAX
};

typedef struct save4_card_spec {
    uint16_t type;
    uint16_t id;             /* event id, < SAVE4_MG_ID_MAX */
    uint16_t item;           /* SAVE4_MG_ITEM */
    uint16_t sprites[3];     /* species shown on the card, 0 = none */
    int32_t received_day;    /* days since 2000-01-01 */
    const char *title;       /* UTF-8, up to 35 characters */
    const char *description; /* UTF-8, up to 249 characters; "\n" breaks lines */
} save4_card_spec;

/* Builds a Wonder Card (with its gift, delivered by the Poke Mart
 * deliveryman) the way the game builds its own. */
save4_status save4_mg_build_card(const save4_card_spec *spec, uint8_t card[SAVE4_WONDERCARD_SIZE]);
/* Checks a .pgt (SAVE4_PGT_SIZE) or .pcd (SAVE4_WONDERCARD_SIZE) image. */
save4_status save4_mg_validate(const uint8_t *data, size_t len, const char **why);
/* Stores a .pcd (card + gift) or .pgt (gift only) as the game does on
 * reception. SAVE4_ERR_NOSPACE when the slots are full. */
save4_status save4_mg_add(save4 *s, const uint8_t *data, size_t len);
save4_status save4_mg_get_card(const save4 *s, int slot, uint8_t card[SAVE4_WONDERCARD_SIZE], bool *used);
save4_status save4_mg_remove_card(save4 *s, int slot);
save4_status save4_mg_pgt_count(const save4 *s, int *count);
/* The MYSTERY GIFT main menu option (shown once the Pokédex is obtained). */
save4_status save4_mg_get_unlocked(const save4 *s, bool *unlocked);
save4_status save4_mg_set_unlocked(save4 *s, bool unlocked);
save4_status save4_dex_get_obtained(const save4 *s, bool *obtained);
save4_status save4_dex_set_obtained(save4 *s, bool obtained);

#ifdef __cplusplus
}
#endif

#endif
