/*
 * The save editor's view of a DS save, whichever library reads it:
 * features/save4 (Diamond, Pearl, Platinum, HeartGold, SoulSilver) or
 * features/save5 (Black, White). The editor, the slot summaries and the
 * Trainer Card work on this one interface; things only one generation has
 * (Gen 4 event gifts and the MYSTERY GIFT unlock, HG/SS's Kanto badges,
 * Gen 5's twelve Wonder Card slots) are reported as absent or reached
 * through np_save_s4().
 *
 * SDL-free so the unit tests exercise it on synthetic saves.
 */
#ifndef NP_EDSAVE_H
#define NP_EDSAVE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "np_core.h"
#include "save4/save4.h"
#include "save5/save5.h"

typedef enum np_save_status {
    NP_SAVE_OK = 0,
    NP_SAVE_ERR_ARG,
    NP_SAVE_ERR_SIZE,
    NP_SAVE_ERR_EMPTY,
    NP_SAVE_ERR_WRONG_GAME, /* a save, but not of the game asked for */
    NP_SAVE_ERR_CHECKSUM,
    NP_SAVE_ERR_RANGE,
    NP_SAVE_ERR_NOMEM,
    NP_SAVE_ERR_ENCODE,
    NP_SAVE_ERR_PKM_CHECKSUM,
    NP_SAVE_ERR_NOSPACE,
    NP_SAVE_ERR_UNSUPPORTED,
    NP_SAVE_ERR_FORMAT
} np_save_status;

const char *np_save_status_str(np_save_status st);

typedef struct np_save {
    int gen;      /* 4 or 5 */
    np_game game; /* the game the save belongs to */
    save4 s4;     /* gen 4 */
    save5 s5;     /* gen 5 */
} np_save;

/* Whether the editor reads `game`'s saves, and which generation they are. */
int np_save_gen_of(np_game game);

/* Loads `data`. `game` is the slot's game; -1 detects it from the save
 * (Diamond/Pearl saves share a format and load as `dp_default`). A save of
 * another format (D/P, Pt, HG/SS, B/W) than the slot's game fails with
 * NP_SAVE_ERR_WRONG_GAME; the two games of one format are interchangeable. */
np_save_status np_save_load(np_save *s, int game, np_game dp_default, const uint8_t *data, size_t len);
void np_save_free(np_save *s);
uint8_t *np_save_img(np_save *s);
size_t np_save_len(const np_save *s);
np_save_status np_save_revalidate(np_save *s);
/* One copy was damaged and the other is used. */
bool np_save_recovered(const np_save *s);
/* save4 / save5 game name of the save ("Pt", "black", ...). */
const char *np_save_game_name(const np_save *s);
/* The save4 handle of a Gen 4 save, else NULL. */
save4 *np_save_s4(np_save *s);
const save4 *np_save_s4c(const np_save *s);

/* ---------------------------------------------------------- trainer */

typedef struct np_trainer {
    char name[64];
    uint16_t tid, sid;
    uint32_t money;
    uint8_t gender;
    bool has_coins;
    uint16_t coins;
    uint8_t badges; /* bitmask of the region's eight (Johto's in HG/SS) */
    bool has_kanto; /* HG/SS */
    uint8_t kanto_badges;
    uint16_t play_hours;
    uint8_t play_minutes, play_seconds;
    bool national_dex;
} np_trainer;

np_save_status np_save_trainer(const np_save *s, np_trainer *t);
uint32_t np_save_money_max(const np_save *s);
uint32_t np_save_coins_max(const np_save *s);
/* The eight badge names of the save's region ("Coal", ... / "Trio", ...). */
const char *const *np_save_badge_names(const np_save *s);
/* HG/SS's Kanto badges, else NULL. */
const char *const *np_save_kanto_badge_names(const np_save *s);
np_save_status np_save_set_name(np_save *s, const char *utf8);
np_save_status np_save_set_gender(np_save *s, uint8_t gender);
np_save_status np_save_set_ids(np_save *s, uint16_t tid, uint16_t sid);
np_save_status np_save_set_money(np_save *s, uint32_t money);
np_save_status np_save_set_coins(np_save *s, uint16_t coins);
np_save_status np_save_set_badges(np_save *s, uint8_t mask);
np_save_status np_save_set_kanto_badges(np_save *s, uint8_t mask);
np_save_status np_save_set_play_time(np_save *s, uint16_t h, uint8_t m, uint8_t sec);

/* --------------------------------------------------------- Pokemon */

typedef struct np_mon {
    int gen;
    pkm4 p4;
    pkm5 p5;
} np_mon;

typedef struct np_mon_info {
    uint32_t pid;
    uint16_t species, held_item, tid, sid;
    uint32_t exp;
    uint8_t friendship, ability, nature, gender, form;
    uint8_t evs[6], ivs[6];
    uint16_t moves[4];
    uint8_t pp[4], pp_ups[4];
    bool is_egg, has_nickname, shiny, hidden_ability;
    char nickname[64], ot_name[64];
    bool has_party_data;
    uint32_t status;
    uint8_t level;
    uint16_t hp, stats[6];
} np_mon_info;

int np_save_dex_max(const np_save *s); /* 493 / 649: also the last species id */
int np_save_box_count(const np_save *s);
int np_save_box_slots(const np_save *s);
uint8_t np_save_party_count(const np_save *s);
np_save_status np_save_get_party(const np_save *s, int slot, np_mon *m);
np_save_status np_save_set_party(np_save *s, int slot, np_mon *m);
np_save_status np_save_set_party_count(np_save *s, uint8_t count);
np_save_status np_save_get_box(const np_save *s, int box, int slot, np_mon *m);
np_save_status np_save_set_box(np_save *s, int box, int slot, np_mon *m);
np_save_status np_save_clear_box(np_save *s, int box, int slot);
np_save_status np_save_box_name(const np_save *s, int box, char *utf8, size_t cap);

/* A blank Pokemon of the save's generation (party tail present). */
void np_mon_blank(const np_save *s, np_mon *m);
bool np_mon_is_empty(const np_mon *m);
bool np_mon_is_party(const np_mon *m);
void np_mon_info_get(const np_mon *m, np_mon_info *in);
void np_mon_set_pid(np_mon *m, uint32_t pid);
void np_mon_set_species(np_mon *m, uint16_t species);
void np_mon_set_ot(np_mon *m, uint16_t tid, uint16_t sid);
np_save_status np_mon_set_ot_name(np_mon *m, const char *utf8);
void np_mon_set_language(np_mon *m, uint8_t lang);
void np_mon_set_ability(np_mon *m, uint8_t ability);
void np_mon_set_gender_form(np_mon *m, uint8_t gender, uint8_t form);
/* Gen 5 stores the nature; Gen 4 derives it from the PID (no-op there). */
void np_mon_set_nature(np_mon *m, uint8_t nature);
void np_mon_set_exp(np_mon *m, uint32_t exp);
void np_mon_set_held(np_mon *m, uint16_t item);
void np_mon_set_move(np_mon *m, int slot, uint16_t move, uint8_t pp, uint8_t ups);
void np_mon_set_iv(np_mon *m, int stat, uint8_t v);
void np_mon_set_ev(np_mon *m, int stat, uint8_t v);
void np_mon_set_friendship(np_mon *m, uint8_t v);
np_save_status np_mon_set_nickname(np_mon *m, const char *utf8, bool is_nickname);
void np_mon_set_origin(np_mon *m, uint8_t game);
void np_mon_set_met(np_mon *m, uint16_t location, uint8_t level, uint8_t ball, uint8_t ot_gender);
/* The save's game's ball fields for `ball_item` beyond np_mon_set_met's (HG/SS). */
np_save_status np_save_mon_set_ball(const np_save *s, np_mon *m, uint16_t ball_item);
void np_mon_set_party_stats(np_mon *m, uint8_t level, uint16_t hp, const uint16_t stats[6], uint32_t status);
/* The game's stat formula (the same in Gen 4 and 5). */
void np_mon_calc_stats(const uint8_t base[6], const uint8_t ivs[6], const uint8_t evs[6], uint8_t level,
                       uint8_t nature, bool shedinja, uint16_t out[6]);
/* The origin-game byte the save's own Pokemon carry (7 HG, 8 SS, 10 D,
 * 11 P, 12 Pt, 20 White, 21 Black). */
uint8_t np_save_origin_game(const np_save *s);
uint8_t np_save_language(const np_save *s);

/* ------------------------------------------------------------- bag */

int np_save_pocket_count(const np_save *s);
const char *np_save_pocket_name(const np_save *s, int pocket);
int np_save_pocket_capacity(const np_save *s, int pocket);
uint16_t np_save_pocket_max_qty(const np_save *s, int pocket);
np_save_status np_save_get_bag(const np_save *s, int pocket, int slot, uint16_t *item, uint16_t *qty);
np_save_status np_save_set_bag(np_save *s, int pocket, int slot, uint16_t item, uint16_t qty);

/* ------------------------------------------------------- Pokedex */

np_save_status np_save_dex_get(const np_save *s, uint16_t species, bool *seen, bool *caught);
np_save_status np_save_dex_set(np_save *s, uint16_t species, bool seen, bool caught);
np_save_status np_save_dex_national(const np_save *s, bool *on);
np_save_status np_save_set_dex_national(np_save *s, bool on);

/* ---------------------------------------------------- event data */

int np_save_flag_first(const np_save *s); /* lowest flag id */
int np_save_flag_count(const np_save *s); /* ids first .. count-1 */
int np_save_var_first(const np_save *s);  /* 0x4000 */
int np_save_var_count(const np_save *s);
np_save_status np_save_flag_get(const np_save *s, uint16_t id, bool *v);
np_save_status np_save_flag_set(np_save *s, uint16_t id, bool v);
np_save_status np_save_var_get(const np_save *s, uint16_t id, uint16_t *v);
np_save_status np_save_var_set(np_save *s, uint16_t id, uint16_t v);

/* ---------------------------------------------------- Mystery Gift */

/* Wonder Card slots: 3 in Gen 4 (.pcd), 12 in Gen 5 (.pgf). */
int np_save_card_slots(const np_save *s);
/* `title` (UTF-8) is the card's title when the slot holds a card. */
np_save_status np_save_card(const np_save *s, int slot, bool *used, char *title, size_t cap);
np_save_status np_save_remove_card(np_save *s, int slot);
/* A gift file: .pgt/.pcd (Gen 4) or .pgf (Gen 5), checked and stored. */
np_save_status np_save_gift_validate(const np_save *s, const uint8_t *data, size_t len, const char **why);
np_save_status np_save_gift_add(np_save *s, const uint8_t *data, size_t len);
/* "Mystery Gift (*.pgt, *.pcd)" style label and the extensions ("pgt;pcd"). */
const char *np_save_gift_filter_label(np_game game);
const char *np_save_gift_extensions(np_game game);

#endif
