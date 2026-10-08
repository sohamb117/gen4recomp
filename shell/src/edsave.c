/*
 * np_save: one interface over features/save4 and features/save5 (edsave.h).
 */
#include "edsave.h"

#include <string.h>

#include "ndsdata/ndsdata.h"

static np_save_status from4(save4_status st)
{
    switch (st) {
    case SAVE4_OK: return NP_SAVE_OK;
    case SAVE4_ERR_ARG: return NP_SAVE_ERR_ARG;
    case SAVE4_ERR_SIZE: return NP_SAVE_ERR_SIZE;
    case SAVE4_ERR_EMPTY: return NP_SAVE_ERR_EMPTY;
    case SAVE4_ERR_UNKNOWN_GAME: return NP_SAVE_ERR_WRONG_GAME;
    case SAVE4_ERR_CHECKSUM: return NP_SAVE_ERR_CHECKSUM;
    case SAVE4_ERR_RANGE: return NP_SAVE_ERR_RANGE;
    case SAVE4_ERR_NOMEM: return NP_SAVE_ERR_NOMEM;
    case SAVE4_ERR_ENCODE: return NP_SAVE_ERR_ENCODE;
    case SAVE4_ERR_PKM_CHECKSUM: return NP_SAVE_ERR_PKM_CHECKSUM;
    case SAVE4_ERR_LAYOUT: return NP_SAVE_ERR_FORMAT;
    case SAVE4_ERR_NOSPACE: return NP_SAVE_ERR_NOSPACE;
    case SAVE4_ERR_UNSUPPORTED: return NP_SAVE_ERR_UNSUPPORTED;
    }
    return NP_SAVE_ERR_ARG;
}

static np_save_status from5(save5_status st)
{
    switch (st) {
    case SAVE5_OK: return NP_SAVE_OK;
    case SAVE5_ERR_ARG: return NP_SAVE_ERR_ARG;
    case SAVE5_ERR_SIZE: return NP_SAVE_ERR_SIZE;
    case SAVE5_ERR_EMPTY: return NP_SAVE_ERR_EMPTY;
    case SAVE5_ERR_UNKNOWN_GAME: return NP_SAVE_ERR_WRONG_GAME;
    case SAVE5_ERR_CHECKSUM: return NP_SAVE_ERR_CHECKSUM;
    case SAVE5_ERR_RANGE: return NP_SAVE_ERR_RANGE;
    case SAVE5_ERR_NOMEM: return NP_SAVE_ERR_NOMEM;
    case SAVE5_ERR_ENCODE: return NP_SAVE_ERR_ENCODE;
    case SAVE5_ERR_PKM_CHECKSUM: return NP_SAVE_ERR_PKM_CHECKSUM;
    case SAVE5_ERR_NOSPACE: return NP_SAVE_ERR_NOSPACE;
    case SAVE5_ERR_FORMAT: return NP_SAVE_ERR_FORMAT;
    }
    return NP_SAVE_ERR_ARG;
}

const char *np_save_status_str(np_save_status st)
{
    switch (st) {
    case NP_SAVE_OK: return "ok";
    case NP_SAVE_ERR_ARG: return "invalid argument";
    case NP_SAVE_ERR_SIZE: return "save image smaller than 512 KiB";
    case NP_SAVE_ERR_EMPTY: return "no save data (blank image)";
    case NP_SAVE_ERR_WRONG_GAME: return "not a save of this game";
    case NP_SAVE_ERR_CHECKSUM: return "save checksum failure (no valid copy)";
    case NP_SAVE_ERR_RANGE: return "value out of range";
    case NP_SAVE_ERR_NOMEM: return "out of memory";
    case NP_SAVE_ERR_ENCODE: return "text not representable in the game's character set";
    case NP_SAVE_ERR_PKM_CHECKSUM: return "Pokemon data checksum mismatch";
    case NP_SAVE_ERR_NOSPACE: return "no free slot";
    case NP_SAVE_ERR_UNSUPPORTED: return "not available in this game";
    case NP_SAVE_ERR_FORMAT: return "malformed data";
    }
    return "unknown error";
}

int np_save_gen_of(np_game game)
{
    switch (game) {
    case NP_GAME_DIAMOND:
    case NP_GAME_PEARL:
    case NP_GAME_PLATINUM: return 4;
    case NP_GAME_BLACK:
    case NP_GAME_WHITE: return 5;
    default: return 0;
    }
}

static np_game game4(save4_game g, int slot_game, np_game dp_default)
{
    if (g == SAVE4_GAME_PT)
        return NP_GAME_PLATINUM;
    return slot_game == NP_GAME_DIAMOND || slot_game == NP_GAME_PEARL ? (np_game)slot_game : dp_default;
}

static np_game game5(save5_game g, int slot_game)
{
    if (slot_game == NP_GAME_BLACK || slot_game == NP_GAME_WHITE)
        return (np_game)slot_game; /* Black and White share a format */
    return g == SAVE5_GAME_WHITE ? NP_GAME_WHITE : NP_GAME_BLACK;
}

np_save_status np_save_load(np_save *s, int game, np_game dp_default, const uint8_t *data, size_t len)
{
    memset(s, 0, sizeof *s);
    const int gen = game < 0 ? 0 : np_save_gen_of((np_game)game);
    if (game >= 0 && !gen)
        return NP_SAVE_ERR_WRONG_GAME;
    np_save_status st4 = NP_SAVE_ERR_WRONG_GAME, st5 = NP_SAVE_ERR_WRONG_GAME;
    if (gen != 5) {
        st4 = from4(save4_load(&s->s4, data, len));
        if (st4 == NP_SAVE_OK) {
            const int pt = s->s4.game == SAVE4_GAME_PT;
            if (game >= 0 && pt != (game == NP_GAME_PLATINUM)) {
                save4_free(&s->s4);
                return NP_SAVE_ERR_WRONG_GAME;
            }
            s->gen = 4;
            s->game = game4(s->s4.game, game, dp_default);
            return NP_SAVE_OK;
        }
        save4_free(&s->s4);
    }
    if (gen != 4) {
        st5 = from5(save5_load(&s->s5, data, len));
        if (st5 == NP_SAVE_OK) {
            s->gen = 5;
            s->game = game5(s->s5.game, game);
            return NP_SAVE_OK;
        }
        save5_free(&s->s5);
    }
    if (gen == 4)
        return st4;
    if (gen == 5)
        return st5;
    /* Detecting: a damaged Black/White save says more than save4's "not
     * mine"; otherwise save4's verdict stands. */
    return st5 == NP_SAVE_ERR_CHECKSUM ? st5 : st4;
}

void np_save_free(np_save *s)
{
    save4_free(&s->s4);
    save5_free(&s->s5);
    s->gen = 0;
}

uint8_t *np_save_img(np_save *s) { return s->gen == 5 ? s->s5.img : s->s4.img; }
size_t np_save_len(const np_save *s) { return s->gen == 5 ? s->s5.len : s->s4.len; }

np_save_status np_save_revalidate(np_save *s)
{
    return s->gen == 5 ? from5(save5_revalidate(&s->s5)) : from4(save4_revalidate(&s->s4));
}

bool np_save_recovered(const np_save *s)
{
    return s->gen == 5 ? s->s5.load_result == SAVE5_LOAD_RECOVERED : s->s4.load_result == SAVE4_LOAD_RECOVERED;
}

const char *np_save_game_name(const np_save *s)
{
    return s->gen == 5 ? save5_game_name(s->s5.game) : save4_game_name(s->s4.game);
}

save4 *np_save_s4(np_save *s) { return s->gen == 4 ? &s->s4 : NULL; }
const save4 *np_save_s4c(const np_save *s) { return s->gen == 4 ? &s->s4 : NULL; }

/* ------------------------------------------------------------ trainer */

np_save_status np_save_trainer(const np_save *s, np_trainer *t)
{
    memset(t, 0, sizeof *t);
    if (s->gen == 5) {
        save5_trainer x;
        save5_status st = save5_get_trainer(&s->s5, &x);
        if (st != SAVE5_OK)
            return from5(st);
        memcpy(t->name, x.name, sizeof t->name);
        t->tid = x.tid;
        t->sid = x.sid;
        t->money = x.money;
        t->gender = x.gender;
        t->badges = x.badges;
        t->play_hours = x.play_hours;
        t->play_minutes = x.play_minutes;
        t->play_seconds = x.play_seconds;
        bool nat = false;
        save5_dex_get_national(&s->s5, &nat);
        t->national_dex = nat;
        return NP_SAVE_OK;
    }
    save4_trainer x;
    save4_status st = save4_get_trainer(&s->s4, &x);
    if (st != SAVE4_OK)
        return from4(st);
    memcpy(t->name, x.name, sizeof t->name);
    t->tid = x.tid;
    t->sid = x.sid;
    t->money = x.money;
    t->gender = x.gender;
    t->has_coins = true;
    t->coins = x.coins;
    t->badges = x.badges;
    t->play_hours = x.play_hours;
    t->play_minutes = x.play_minutes;
    t->play_seconds = x.play_seconds;
    t->national_dex = x.has_national_dex;
    return NP_SAVE_OK;
}

uint32_t np_save_money_max(const np_save *s) { return s->gen == 5 ? SAVE5_MONEY_MAX : SAVE4_MONEY_MAX; }
uint32_t np_save_coins_max(const np_save *s) { return s->gen == 5 ? 0 : SAVE4_COINS_MAX; }

const char *const *np_save_badge_names(const np_save *s)
{
    static const char *const sinnoh[8] = {"Coal", "Forest", "Cobble", "Fen", "Relic", "Mine", "Icicle", "Beacon"};
    static const char *const unova[8] = {"Trio", "Basic", "Insect", "Bolt", "Quake", "Jet", "Freeze", "Legend"};
    return s->gen == 5 ? unova : sinnoh;
}

np_save_status np_save_set_name(np_save *s, const char *utf8)
{
    return s->gen == 5 ? from5(save5_set_trainer_name(&s->s5, utf8)) : from4(save4_set_trainer_name(&s->s4, utf8));
}

np_save_status np_save_set_gender(np_save *s, uint8_t gender)
{
    return s->gen == 5 ? from5(save5_set_gender(&s->s5, gender)) : from4(save4_set_gender(&s->s4, gender));
}

np_save_status np_save_set_ids(np_save *s, uint16_t tid, uint16_t sid)
{
    return s->gen == 5 ? from5(save5_set_trainer_ids(&s->s5, tid, sid))
                       : from4(save4_set_trainer_ids(&s->s4, tid, sid));
}

np_save_status np_save_set_money(np_save *s, uint32_t money)
{
    return s->gen == 5 ? from5(save5_set_money(&s->s5, money)) : from4(save4_set_money(&s->s4, money));
}

np_save_status np_save_set_coins(np_save *s, uint16_t coins)
{
    return s->gen == 5 ? NP_SAVE_ERR_UNSUPPORTED : from4(save4_set_coins(&s->s4, coins));
}

np_save_status np_save_set_badges(np_save *s, uint8_t mask)
{
    return s->gen == 5 ? from5(save5_set_badges(&s->s5, mask)) : from4(save4_set_badges(&s->s4, mask));
}

np_save_status np_save_set_play_time(np_save *s, uint16_t h, uint8_t m, uint8_t sec)
{
    return s->gen == 5 ? from5(save5_set_play_time(&s->s5, h, m, sec))
                       : from4(save4_set_play_time(&s->s4, h, m, sec));
}

/* ------------------------------------------------------------ Pokemon */

int np_save_dex_max(const np_save *s) { return s->gen == 5 ? SAVE5_DEX_MAX : SAVE4_DEX_MAX; }
int np_save_box_count(const np_save *s) { return s->gen == 5 ? SAVE5_BOX_COUNT : SAVE4_BOX_COUNT; }
int np_save_box_slots(const np_save *s) { return s->gen == 5 ? SAVE5_BOX_SLOTS : SAVE4_BOX_SLOTS; }

uint8_t np_save_party_count(const np_save *s)
{
    return s->gen == 5 ? save5_party_count(&s->s5) : save4_party_count(&s->s4);
}

np_save_status np_save_get_party(const np_save *s, int slot, np_mon *m)
{
    m->gen = s->gen;
    return s->gen == 5 ? from5(save5_get_party(&s->s5, slot, &m->p5)) : from4(save4_get_party(&s->s4, slot, &m->p4));
}

np_save_status np_save_set_party(np_save *s, int slot, np_mon *m)
{
    if (m->gen != s->gen)
        return NP_SAVE_ERR_ARG;
    return s->gen == 5 ? from5(save5_set_party(&s->s5, slot, &m->p5)) : from4(save4_set_party(&s->s4, slot, &m->p4));
}

np_save_status np_save_set_party_count(np_save *s, uint8_t count)
{
    return s->gen == 5 ? from5(save5_set_party_count(&s->s5, count)) : from4(save4_set_party_count(&s->s4, count));
}

np_save_status np_save_get_box(const np_save *s, int box, int slot, np_mon *m)
{
    m->gen = s->gen;
    return s->gen == 5 ? from5(save5_get_box_mon(&s->s5, box, slot, &m->p5))
                       : from4(save4_get_box_mon(&s->s4, box, slot, &m->p4));
}

np_save_status np_save_set_box(np_save *s, int box, int slot, np_mon *m)
{
    if (m->gen != s->gen)
        return NP_SAVE_ERR_ARG;
    return s->gen == 5 ? from5(save5_set_box_mon(&s->s5, box, slot, &m->p5))
                       : from4(save4_set_box_mon(&s->s4, box, slot, &m->p4));
}

np_save_status np_save_clear_box(np_save *s, int box, int slot)
{
    return s->gen == 5 ? from5(save5_clear_box_mon(&s->s5, box, slot)) : from4(save4_clear_box_mon(&s->s4, box, slot));
}

np_save_status np_save_box_name(const np_save *s, int box, char *utf8, size_t cap)
{
    return s->gen == 5 ? from5(save5_get_box_name(&s->s5, box, utf8, cap))
                       : from4(save4_get_box_name(&s->s4, box, utf8, cap));
}

void np_mon_blank(const np_save *s, np_mon *m)
{
    memset(m, 0, sizeof *m);
    m->gen = s->gen;
    m->p4.party = m->p5.party = true;
}

bool np_mon_is_empty(const np_mon *m) { return m->gen == 5 ? pkm5_is_empty(&m->p5) : pkm4_is_empty(&m->p4); }
bool np_mon_is_party(const np_mon *m) { return m->gen == 5 ? m->p5.party : m->p4.party; }

void np_mon_info_get(const np_mon *m, np_mon_info *in)
{
    memset(in, 0, sizeof *in);
    if (m->gen == 5) {
        pkm5_info x;
        pkm5_info_get(&m->p5, &x);
        in->pid = x.pid;
        in->species = x.species;
        in->held_item = x.held_item;
        in->tid = x.tid;
        in->sid = x.sid;
        in->exp = x.exp;
        in->friendship = x.friendship;
        in->ability = x.ability;
        in->nature = x.nature;
        in->gender = x.gender;
        in->form = x.form;
        memcpy(in->evs, x.evs, 6);
        memcpy(in->ivs, x.ivs, 6);
        memcpy(in->moves, x.moves, sizeof in->moves);
        memcpy(in->pp, x.pp, 4);
        memcpy(in->pp_ups, x.pp_ups, 4);
        in->is_egg = x.is_egg;
        in->has_nickname = x.has_nickname;
        in->shiny = x.shiny;
        in->hidden_ability = x.hidden_ability;
        memcpy(in->nickname, x.nickname, sizeof in->nickname);
        memcpy(in->ot_name, x.ot_name, sizeof in->ot_name);
        in->has_party_data = x.has_party_data;
        in->status = x.status;
        in->level = x.level;
        in->hp = x.hp;
        memcpy(in->stats, x.stats, sizeof in->stats);
        return;
    }
    pkm4_info x;
    pkm4_info_get(&m->p4, &x);
    in->pid = x.pid;
    in->species = x.species;
    in->held_item = x.held_item;
    in->tid = x.tid;
    in->sid = x.sid;
    in->exp = x.exp;
    in->friendship = x.friendship;
    in->ability = x.ability;
    in->nature = x.nature;
    in->gender = x.gender;
    in->form = x.form;
    memcpy(in->evs, x.evs, 6);
    memcpy(in->ivs, x.ivs, 6);
    memcpy(in->moves, x.moves, sizeof in->moves);
    memcpy(in->pp, x.pp, 4);
    memcpy(in->pp_ups, x.pp_ups, 4);
    in->is_egg = x.is_egg;
    in->has_nickname = x.has_nickname;
    in->shiny = x.shiny;
    memcpy(in->nickname, x.nickname, sizeof in->nickname);
    memcpy(in->ot_name, x.ot_name, sizeof in->ot_name);
    in->has_party_data = x.has_party_data;
    in->status = x.status;
    in->level = x.level;
    in->hp = x.hp;
    memcpy(in->stats, x.stats, sizeof in->stats);
}

#define BOTH(call4, call5)    \
    do {                      \
        if (m->gen == 5)      \
            call5;            \
        else                  \
            call4;            \
    } while (0)

void np_mon_set_pid(np_mon *m, uint32_t pid) { BOTH(pkm4_set_pid(&m->p4, pid), pkm5_set_pid(&m->p5, pid)); }
void np_mon_set_species(np_mon *m, uint16_t v) { BOTH(pkm4_set_species(&m->p4, v), pkm5_set_species(&m->p5, v)); }
void np_mon_set_ot(np_mon *m, uint16_t tid, uint16_t sid)
{
    BOTH(pkm4_set_ot_ids(&m->p4, tid, sid), pkm5_set_ot_ids(&m->p5, tid, sid));
}
np_save_status np_mon_set_ot_name(np_mon *m, const char *utf8)
{
    return m->gen == 5 ? from5(pkm5_set_ot_name(&m->p5, utf8)) : from4(pkm4_set_ot_name(&m->p4, utf8));
}
void np_mon_set_language(np_mon *m, uint8_t v) { BOTH(pkm4_set_language(&m->p4, v), pkm5_set_language(&m->p5, v)); }
/* Gen 5: a chosen ability is a normal one (the hidden flag is cleared). */
void np_mon_set_ability(np_mon *m, uint8_t v) { BOTH(pkm4_set_ability(&m->p4, v), pkm5_set_ability(&m->p5, v, false)); }
void np_mon_set_gender_form(np_mon *m, uint8_t g, uint8_t f)
{
    BOTH(pkm4_set_gender_form(&m->p4, g, f), pkm5_set_gender_form(&m->p5, g, f));
}
void np_mon_set_nature(np_mon *m, uint8_t v)
{
    if (m->gen == 5)
        pkm5_set_nature(&m->p5, v);
}
void np_mon_set_exp(np_mon *m, uint32_t v) { BOTH(pkm4_set_exp(&m->p4, v), pkm5_set_exp(&m->p5, v)); }
void np_mon_set_held(np_mon *m, uint16_t v) { BOTH(pkm4_set_held_item(&m->p4, v), pkm5_set_held_item(&m->p5, v)); }
void np_mon_set_move(np_mon *m, int slot, uint16_t move, uint8_t pp, uint8_t ups)
{
    BOTH(pkm4_set_move(&m->p4, slot, move, pp, ups), pkm5_set_move(&m->p5, slot, move, pp, ups));
}
void np_mon_set_iv(np_mon *m, int stat, uint8_t v) { BOTH(pkm4_set_iv(&m->p4, stat, v), pkm5_set_iv(&m->p5, stat, v)); }
void np_mon_set_ev(np_mon *m, int stat, uint8_t v) { BOTH(pkm4_set_ev(&m->p4, stat, v), pkm5_set_ev(&m->p5, stat, v)); }
void np_mon_set_friendship(np_mon *m, uint8_t v)
{
    BOTH(pkm4_set_friendship(&m->p4, v), pkm5_set_friendship(&m->p5, v));
}
np_save_status np_mon_set_nickname(np_mon *m, const char *utf8, bool is_nickname)
{
    return m->gen == 5 ? from5(pkm5_set_nickname(&m->p5, utf8, is_nickname))
                       : from4(pkm4_set_nickname(&m->p4, utf8, is_nickname));
}
void np_mon_set_origin(np_mon *m, uint8_t v) { BOTH(pkm4_set_origin_game(&m->p4, v), pkm5_set_origin_game(&m->p5, v)); }
void np_mon_set_met(np_mon *m, uint16_t location, uint8_t level, uint8_t ball, uint8_t ot_gender)
{
    BOTH(pkm4_set_met(&m->p4, location, level, ball, ot_gender), pkm5_set_met(&m->p5, location, level, ball, ot_gender));
}
void np_mon_set_party_stats(np_mon *m, uint8_t level, uint16_t hp, const uint16_t stats[6], uint32_t status)
{
    BOTH(pkm4_set_party_stats(&m->p4, level, hp, stats, status), pkm5_set_party_stats(&m->p5, level, hp, stats, status));
}

void np_mon_calc_stats(const uint8_t base[6], const uint8_t ivs[6], const uint8_t evs[6], uint8_t level,
                       uint8_t nature, bool shedinja, uint16_t out[6])
{
    pkm5_calc_stats(base, ivs, evs, level, nature, shedinja, out);
}

uint8_t np_save_origin_game(const np_save *s)
{
    if (s->gen == 5)
        return s->game == NP_GAME_WHITE ? 20 : 21;
    return s->game == NP_GAME_DIAMOND ? 10 : s->game == NP_GAME_PEARL ? 11 : 12;
}

uint8_t np_save_language(const np_save *s)
{
    if (s->gen == 5) {
        save5_trainer t;
        return save5_get_trainer(&s->s5, &t) == SAVE5_OK && t.language ? t.language : 2;
    }
    save4_trainer t;
    return save4_get_trainer(&s->s4, &t) == SAVE4_OK && t.language ? t.language : 2;
}

/* ---------------------------------------------------------------- bag */

int np_save_pocket_count(const np_save *s) { return s->gen == 5 ? SAVE5_POCKET_COUNT : SAVE4_POCKET_COUNT; }

const char *np_save_pocket_name(const np_save *s, int pocket)
{
    return s->gen == 5 ? save5_pocket_name((save5_pocket)pocket) : save4_pocket_name((save4_pocket)pocket);
}

int np_save_pocket_capacity(const np_save *s, int pocket)
{
    return s->gen == 5 ? save5_pocket_capacity((save5_pocket)pocket) : save4_pocket_capacity((save4_pocket)pocket);
}

uint16_t np_save_pocket_max_qty(const np_save *s, int pocket)
{
    return s->gen == 5 ? save5_pocket_max_qty((save5_pocket)pocket) : 999;
}

np_save_status np_save_get_bag(const np_save *s, int pocket, int slot, uint16_t *item, uint16_t *qty)
{
    return s->gen == 5 ? from5(save5_get_bag_slot(&s->s5, (save5_pocket)pocket, slot, item, qty))
                       : from4(save4_get_bag_slot(&s->s4, (save4_pocket)pocket, slot, item, qty));
}

np_save_status np_save_set_bag(np_save *s, int pocket, int slot, uint16_t item, uint16_t qty)
{
    return s->gen == 5 ? from5(save5_set_bag_slot(&s->s5, (save5_pocket)pocket, slot, item, qty))
                       : from4(save4_set_bag_slot(&s->s4, (save4_pocket)pocket, slot, item, qty));
}

/* ------------------------------------------------------------ Pokedex */

np_save_status np_save_dex_get(const np_save *s, uint16_t species, bool *seen, bool *caught)
{
    return s->gen == 5 ? from5(save5_dex_get(&s->s5, species, seen, caught))
                       : from4(save4_dex_get(&s->s4, species, seen, caught));
}

np_save_status np_save_dex_set(np_save *s, uint16_t species, bool seen, bool caught)
{
    return s->gen == 5 ? from5(save5_dex_set(&s->s5, species, seen, caught))
                       : from4(save4_dex_set(&s->s4, species, seen, caught));
}

np_save_status np_save_dex_national(const np_save *s, bool *on)
{
    return s->gen == 5 ? from5(save5_dex_get_national(&s->s5, on)) : from4(save4_dex_get_national(&s->s4, on));
}

np_save_status np_save_set_dex_national(np_save *s, bool on)
{
    return s->gen == 5 ? from5(save5_dex_set_national(&s->s5, on)) : from4(save4_dex_set_national(&s->s4, on));
}

/* --------------------------------------------------------- event data */

int np_save_flag_first(const np_save *s) { return s->gen == 5 ? 0 : 1; }
int np_save_flag_count(const np_save *s) { return s->gen == 5 ? SAVE5_NUM_FLAGS : SAVE4_NUM_FLAGS; }
int np_save_var_first(const np_save *s) { return s->gen == 5 ? SAVE5_VARS_START : SAVE4_VARS_START; }
int np_save_var_count(const np_save *s) { return s->gen == 5 ? SAVE5_NUM_VARS : SAVE4_NUM_VARS; }

np_save_status np_save_flag_get(const np_save *s, uint16_t id, bool *v)
{
    return s->gen == 5 ? from5(save5_flag_get(&s->s5, id, v)) : from4(save4_flag_get(&s->s4, id, v));
}

np_save_status np_save_flag_set(np_save *s, uint16_t id, bool v)
{
    return s->gen == 5 ? from5(save5_flag_set(&s->s5, id, v)) : from4(save4_flag_set(&s->s4, id, v));
}

np_save_status np_save_var_get(const np_save *s, uint16_t id, uint16_t *v)
{
    return s->gen == 5 ? from5(save5_var_get(&s->s5, id, v)) : from4(save4_var_get(&s->s4, id, v));
}

np_save_status np_save_var_set(np_save *s, uint16_t id, uint16_t v)
{
    return s->gen == 5 ? from5(save5_var_set(&s->s5, id, v)) : from4(save4_var_set(&s->s4, id, v));
}

/* ------------------------------------------------------- Mystery Gift */

int np_save_card_slots(const np_save *s) { return s->gen == 5 ? SAVE5_MG_SLOTS : SAVE4_WONDERCARD_SLOTS; }

np_save_status np_save_card(const np_save *s, int slot, bool *used, char *title, size_t cap)
{
    *used = false;
    if (cap)
        title[0] = '\0';
    if (s->gen == 5) {
        uint8_t card[SAVE5_PGF_SIZE];
        save5_status st = save5_mg_get_card(&s->s5, slot, card, used);
        if (st == SAVE5_OK && *used && cap)
            save5_text_decode(card + 0x60, SAVE5_PGF_TITLE_LEN, title, cap); /* PGF title */
        return from5(st);
    }
    uint8_t card[SAVE4_WONDERCARD_SIZE];
    save4_status st = save4_mg_get_card(&s->s4, slot, card, used);
    if (st == SAVE4_OK && *used && cap) {
        /* The WonderCard's title follows its 0x104-byte gift (save4.h). */
        uint16_t codes[SAVE4_WC_TITLE_LEN];
        for (int i = 0; i < SAVE4_WC_TITLE_LEN; i++)
            codes[i] = (uint16_t)(card[0x104 + 2 * i] | card[0x105 + 2 * i] << 8);
        g4_text_decode(codes, SAVE4_WC_TITLE_LEN, title, cap);
    }
    return from4(st);
}

np_save_status np_save_remove_card(np_save *s, int slot)
{
    return s->gen == 5 ? from5(save5_mg_remove_card(&s->s5, slot)) : from4(save4_mg_remove_card(&s->s4, slot));
}

np_save_status np_save_gift_validate(const np_save *s, const uint8_t *data, size_t len, const char **why)
{
    /* Format only; adding refuses a gift type the game cannot deliver. */
    return s->gen == 5 ? from5(save5_mg_validate(data, len, why)) : from4(save4_mg_validate(data, len, why));
}

np_save_status np_save_gift_add(np_save *s, const uint8_t *data, size_t len)
{
    return s->gen == 5 ? from5(save5_mg_add(&s->s5, data, len)) : from4(save4_mg_add(&s->s4, data, len));
}

const char *np_save_gift_filter_label(np_game game)
{
    return np_save_gen_of(game) == 5 ? "Wonder Card (*.pgf)" : "Mystery Gift (*.pgt, *.pcd)";
}

const char *np_save_gift_extensions(np_game game) { return np_save_gen_of(game) == 5 ? "pgf" : "pgt;pcd"; }
