/*
 * Save editor page: edits one save slot's 512 KiB backup image through
 * features/save4, with names and game tables (species base stats, exp
 * tables, move PP) read from the player's imported ROM via features/ndsdata.
 *
 * Tabs: Trainer, Party, Boxes, Bag, Pokedex. Every edit is applied to a
 * scratch copy first and recorded on a snapshot undo stack (undo.h) only if
 * save4 accepted it, so a rejected edit (text outside the game's charset,
 * out of range) leaves no trace. Save writes the image atomically with a
 * .bak of the previous one; a slot whose blocks fail their checksums is
 * refused before anything is shown.
 *
 * Consistency rules kept here, since the game recomputes nothing on load:
 *  - level and EXP move together (exp tables from the ROM); party stats are
 *    recomputed with the game's formula after any change that affects them,
 *    and current HP keeps its distance from max HP;
 *  - a species change resets the ability to the species' first, recomputes
 *    the PID-derived gender and renames unnicknamed Pokemon;
 *  - a new move gets its full PP for the current PP Ups; EVs are capped at
 *    510 in total.
 * Nature and shininess come from the PID and are shown read-only.
 *
 * Input: one selection model for keyboard, gamepad, mouse and touch. L/R
 * (Page Up/Down) switch tabs, X/Y (or Ctrl/Cmd+Z / Ctrl/Cmd+Shift+Z) undo
 * and redo, Ctrl/Cmd+S saves.
 */
#include "app.h"

#include <math.h>

#include "ndsdata/ndsdata.h"
#include "romdb.h"
#include "save4/save4.h"
#include "undo.h"

#define MAX_ROWS 520

enum { TAB_TRAINER, TAB_PARTY, TAB_BOXES, TAB_BAG, TAB_DEX, TAB_COUNT };
enum { OV_NONE, OV_NUMBER, OV_CHOOSER, OV_MENU, OV_DISCARD };
enum { FOCUS_TABS, FOCUS_LIST, FOCUS_FOOTER };
enum { FOOT_UNDO, FOOT_REDO, FOOT_SAVE, FOOT_CLOSE, FOOT_COUNT };

/* Hit id = NP_EDITOR_HIT_BASE + group * 1000 + index. */
enum { HG_TAB, HG_ROW, HG_FOOT, HG_BOX, HG_NUM, HG_CHOOSE, HG_MENU, HG_DISCARD, HG_SCROLL };

typedef enum field {
    F_NONE,
    F_TR_NAME,
    F_TR_GENDER,
    F_TR_TID,
    F_TR_SID,
    F_TR_MONEY,
    F_TR_COINS,
    F_TR_BADGE,
    F_TR_HOURS,
    F_TR_MINUTES,
    F_TR_SECONDS,
    F_PARTY_MON,
    F_MON_SPECIES,
    F_MON_NICK,
    F_MON_LEVEL,
    F_MON_EXP,
    F_MON_ABILITY,
    F_MON_ITEM,
    F_MON_MOVE,
    F_MON_IV,
    F_MON_EV,
    F_MON_FRIEND,
    F_INFO, /* read-only line */
    F_BAG_POCKET,
    F_BAG_SLOT,
    F_BAG_ADD,
    F_DEX_ALL,
    F_DEX_NONE,
    F_DEX_SPECIES,
} field;

typedef enum row_kind { RK_NUMBER, RK_CHOOSE, RK_TEXT, RK_TOGGLE, RK_ACTION, RK_INFO } row_kind;

typedef struct row {
    field f;
    int arg;
    row_kind kind;
    char label[40];
    char value[72];
} row;

typedef struct np_editor {
    np_game game;
    char slot[NP_SLOT_NAME_MAX + 1];
    char path[1100];
    save4 s;
    uint8_t *scratch; /* image before the edit in progress */
    np_undo undo;
    int dirty;

    SDL_IOStream *rom_io;
    nd_rom rom;
    int have_rom;
    nd_names names;
    int have_names;
    nd_gamedata gd;
    int have_gd;

    int tab, focus, sel, scroll, foot;
    int mon_open, mon_box, mon_slot; /* mon_box -1: party */
    int box, cursor;                 /* boxes tab; cursor -1 = box header */
    int held_box, held_slot;         /* a Pokemon being moved, held_box -1 none */
    int pocket;

    int ov;
    struct {
        field f;
        int arg;
        int64_t value, min, max;
        int digits, cursor;
        int typed; /* digits typed so far; the first replaces the value */
    } num;
    struct {
        field f;
        int arg;
        int ids[600];
        int nids;
        char filter[24];
        int sel, scroll;
    } ch;
    struct {
        int count, sel;
        const char *labels[6];
        int ids[6];
    } menu;
    int discard_sel;
    field text_field;

    row rows[MAX_ROWS];
    int nrows;
} np_editor;

static const SDL_Color white = {235, 238, 245, 255};
static const SDL_Color dim = {150, 158, 175, 255};
static const SDL_Color accent = {255, 205, 80, 255};
static const SDL_Color warn = {255, 140, 120, 255};
static const SDL_Color hilite = {255, 205, 80, 40};

static const char *const tab_names[TAB_COUNT] = {"Trainer", "Party", "Boxes", "Bag", "Pokedex"};
static const char *const stat_names[6] = {"HP", "Attack", "Defense", "Speed", "Sp. Atk", "Sp. Def"};
static const char *const badge_names[8] = {"Coal", "Forest", "Cobble", "Fen", "Relic", "Mine", "Icicle", "Beacon"};

static int hit_id(int group, int index) { return NP_EDITOR_HIT_BASE + group * 1000 + index; }
static int wrapi(int v, int n) { return n > 0 ? ((v % n) + n) % n : 0; }

/* ---- names --------------------------------------------------------------- */

static void name_of(const np_editor *e, nd_text_kind kind, uint32_t id, char *buf, size_t n)
{
    const char *s = e->have_names ? nd_name(&e->names, kind, id) : NULL;
    if (s && *s)
        SDL_strlcpy(buf, s, n);
    else
        SDL_snprintf(buf, n, "#%u", (unsigned)id);
}

static uint32_t kind_count(const np_editor *e, nd_text_kind kind)
{
    if (e->have_names && e->names.count[kind])
        return e->names.count[kind];
    switch (kind) {
    case ND_TEXT_SPECIES: return SAVE4_DEX_MAX + 1;
    case ND_TEXT_MOVES: return 468;
    case ND_TEXT_ITEMS: return 468;
    case ND_TEXT_ABILITIES: return 124;
    default: return 0;
    }
}

/* ---- ROM -------------------------------------------------------------------- */

static int rom_read(void *user, uint64_t offset, void *dst, size_t len)
{
    SDL_IOStream *io = user;
    if (SDL_SeekIO(io, (Sint64)offset, SDL_IO_SEEK_SET) < 0)
        return -1;
    return SDL_ReadIO(io, dst, len) == len ? 0 : -1;
}

static void load_rom_data(np_editor *e)
{
    char path[1100];
    np_storage_rom_path(e->game, path, sizeof path);
    e->rom_io = SDL_IOFromFile(path, "rb");
    if (!e->rom_io)
        return;
    Sint64 size = SDL_GetIOSize(e->rom_io);
    if (size <= 0 || nd_rom_open(&e->rom, rom_read, e->rom_io, (uint64_t)size) != ND_OK)
        return;
    e->have_rom = 1;
    e->have_names = nd_names_load(&e->names, &e->rom) == ND_OK;
    e->have_gd = nd_gamedata_load(&e->gd, &e->rom) == ND_OK;
    if (!e->have_names || !e->have_gd)
        SDL_Log("editor: ROM tables unavailable (names %d, data %d)", e->have_names, e->have_gd);
}

/* ---- open / close ------------------------------------------------------------ */

void np_editor_open(np_app *app, np_game game, const char *slot)
{
    np_editor_close(app);
    np_editor *e = SDL_calloc(1, sizeof *e);
    if (!e) {
        np_app_toast(app, "Out of memory");
        return;
    }
    e->game = game;
    SDL_strlcpy(e->slot, slot, sizeof e->slot);
    np_storage_slot_path(game, slot, e->path, sizeof e->path);
    size_t len = 0;
    uint8_t *data = SDL_LoadFile(e->path, &len);
    save4_status st = data ? save4_load(&e->s, data, len) : SAVE4_ERR_ARG;
    SDL_free(data);
    if (st != SAVE4_OK) {
        if (data)
            SDL_snprintf(app->status, sizeof app->status, "Cannot edit \"%s\": %s.", slot, save4_status_str(st));
        else
            SDL_snprintf(app->status, sizeof app->status, "Cannot read \"%s\": %s", slot, SDL_GetError());
        np_app_toast(app, "%s", app->status);
        SDL_Log("editor: %s", app->status);
        SDL_free(e);
        return;
    }
    e->scratch = SDL_malloc(e->s.len);
    if (!e->scratch) {
        save4_free(&e->s);
        SDL_free(e);
        np_app_toast(app, "Out of memory");
        return;
    }
    np_undo_init(&e->undo, e->s.len);
    load_rom_data(e);
    e->held_box = -1;
    e->focus = FOCUS_LIST;
    app->editor = e;
    np_app_open_page(app, NP_PAGE_EDITOR);
    if (e->s.load_result == SAVE4_LOAD_RECOVERED)
        np_app_toast(app, "One copy of this save was damaged; editing the intact copy");
    SDL_Log("editor: opened %s slot \"%s\" (%s)", np_game_id(game), slot, save4_game_name(e->s.game));
}

void np_editor_close(np_app *app)
{
    np_editor *e = app->editor;
    if (!e)
        return;
    if (e->ov == OV_CHOOSER)
        SDL_StopTextInput(app->window);
    if (e->have_names)
        nd_names_free(&e->names);
    if (e->have_gd)
        nd_gamedata_free(&e->gd);
    if (e->have_rom)
        nd_rom_close(&e->rom);
    if (e->rom_io)
        SDL_CloseIO(e->rom_io);
    np_undo_free(&e->undo);
    save4_free(&e->s);
    SDL_free(e->scratch);
    SDL_free(e);
    app->editor = NULL;
}

/* ---- editing ------------------------------------------------------------------- */

static void begin_edit(np_editor *e) { SDL_memcpy(e->scratch, e->s.img, e->s.len); }

/* Keeps the edit (recording the previous image) or restores it. */
static int end_edit(np_app *app, np_editor *e, save4_status st)
{
    if (st != SAVE4_OK) {
        SDL_memcpy(e->s.img, e->scratch, e->s.len);
        save4_revalidate(&e->s);
        np_app_toast(app, st == SAVE4_ERR_ENCODE ? "Not in the game's character set" : "Not allowed: %s",
                     save4_status_str(st));
        return -1;
    }
    if (SDL_memcmp(e->scratch, e->s.img, e->s.len)) {
        if (np_undo_push(&e->undo, e->scratch))
            np_app_toast(app, "Out of memory: this edit cannot be undone");
        e->dirty = 1;
    }
    return 0;
}

static void restore(np_app *app, np_editor *e, int redo)
{
    if ((redo ? np_undo_redo : np_undo_undo)(&e->undo, e->s.img)) {
        np_app_toast(app, redo ? "Nothing to redo" : "Nothing to undo");
        return;
    }
    save4_revalidate(&e->s);
    e->dirty = 1;
    np_app_toast(app, redo ? "Redone" : "Undone");
}

static void save_now(np_app *app, np_editor *e)
{
    size_t len;
    const uint8_t *img = save4_image(&e->s, &len);
    if (np_storage_write_atomic(e->path, img, len, 1)) {
        np_app_toast(app, "Saving failed: %s", SDL_GetError());
        return;
    }
    e->dirty = 0;
    np_app_toast(app, "Saved \"%s\"", e->slot);
    SDL_Log("editor: saved %s", e->path);
}

static save4_status get_mon(const np_editor *e, int box, int slot, pkm4 *p)
{
    return box < 0 ? save4_get_party(&e->s, slot, p) : save4_get_box_mon(&e->s, box, slot, p);
}

static save4_status put_mon(np_editor *e, int box, int slot, pkm4 *p)
{
    return box < 0 ? save4_set_party(&e->s, slot, p) : save4_set_box_mon(&e->s, box, slot, p);
}

/* Level, stats and HP after species/EXP/IV/EV changes (party Pokemon). */
static void mon_recalc(np_editor *e, pkm4 *p)
{
    if (!e->have_gd || !p->party)
        return;
    pkm4_info in;
    pkm4_info_get(p, &in);
    const nd_species *sp = nd_species_get(&e->gd, in.species);
    if (!sp)
        return;
    uint8_t level = (uint8_t)nd_level_for_exp(&e->gd, in.species, in.exp);
    uint16_t st[6];
    pkm4_calc_stats(sp->base, in.ivs, in.evs, level, in.nature, in.species == 292 /* Shedinja */, st);
    int hp = in.hp;
    if (in.stats[0] == 0)
        hp = st[0];
    else if (hp > 0)
        hp = SDL_clamp(hp + (int)st[0] - (int)in.stats[0], 1, (int)st[0]);
    pkm4_set_party_stats(p, level, (uint16_t)hp, st, in.status);
}

static uint8_t gender_for(const nd_species *sp, uint32_t pid)
{
    if (sp->gender_ratio == 255)
        return 2;
    if (sp->gender_ratio == 254)
        return 1;
    if (sp->gender_ratio == 0)
        return 0;
    return (pid & 0xFF) < sp->gender_ratio ? 1 : 0;
}

/* Applies a numeric or chooser value to the current Pokemon. */
static save4_status edit_mon(np_editor *e, field f, int arg, int64_t v)
{
    pkm4 p;
    save4_status st = get_mon(e, e->mon_box, e->mon_slot, &p);
    if (st != SAVE4_OK)
        return st;
    pkm4_info in;
    pkm4_info_get(&p, &in);
    switch (f) {
    case F_MON_SPECIES: {
        pkm4_set_species(&p, (uint16_t)v);
        const nd_species *sp = e->have_gd ? nd_species_get(&e->gd, (uint32_t)v) : NULL;
        if (sp) {
            pkm4_set_ability(&p, sp->abilities[0]);
            pkm4_set_gender_form(&p, gender_for(sp, in.pid), 0);
            /* Keep the level: the new species may grow at another rate. */
            uint32_t level = e->have_gd ? nd_level_for_exp(&e->gd, in.species, in.exp) : 0;
            if (level)
                pkm4_set_exp(&p, nd_exp_for_level(&e->gd, (uint32_t)v, level));
        }
        if (!in.has_nickname && e->have_names) {
            const char *name = nd_name(&e->names, ND_TEXT_SPECIES, (uint32_t)v);
            if (name && pkm4_set_nickname(&p, name, false) != SAVE4_OK)
                return SAVE4_ERR_ENCODE;
        }
        break;
    }
    case F_MON_LEVEL: pkm4_set_exp(&p, nd_exp_for_level(&e->gd, in.species, (uint32_t)v)); break;
    case F_MON_EXP: pkm4_set_exp(&p, (uint32_t)v); break;
    case F_MON_ABILITY: pkm4_set_ability(&p, (uint8_t)v); break;
    case F_MON_ITEM: pkm4_set_held_item(&p, (uint16_t)v); break;
    case F_MON_MOVE: {
        uint8_t base = e->have_gd ? nd_move_base_pp(&e->gd, (uint32_t)v) : 5;
        uint8_t ups = v ? in.pp_ups[arg] : 0;
        pkm4_set_move(&p, arg, (uint16_t)v, (uint8_t)(base * (5 + ups) / 5), ups);
        break;
    }
    case F_MON_IV: pkm4_set_iv(&p, arg, (uint8_t)v); break;
    case F_MON_EV: {
        int others = 0;
        for (int i = 0; i < 6; i++)
            if (i != arg)
                others += in.evs[i];
        pkm4_set_ev(&p, arg, (uint8_t)SDL_min(v, (int64_t)SDL_max(0, 510 - others)));
        break;
    }
    case F_MON_FRIEND: pkm4_set_friendship(&p, (uint8_t)v); break;
    default: return SAVE4_ERR_ARG;
    }
    mon_recalc(e, &p);
    return put_mon(e, e->mon_box, e->mon_slot, &p);
}

/* Bag pockets are compact lists: removing a slot shifts the rest up. */
static save4_status bag_remove(np_editor *e, int pocket, int slot)
{
    int cap = save4_pocket_capacity((save4_pocket)pocket);
    for (int i = slot; i < cap; i++) {
        uint16_t item = 0, qty = 0;
        if (i + 1 < cap)
            save4_get_bag_slot(&e->s, (save4_pocket)pocket, i + 1, &item, &qty);
        save4_status st = save4_set_bag_slot(&e->s, (save4_pocket)pocket, i, item, qty);
        if (st != SAVE4_OK)
            return st;
        if (!item)
            break;
    }
    return SAVE4_OK;
}

static int bag_count(const np_editor *e, int pocket)
{
    int cap = save4_pocket_capacity((save4_pocket)pocket), n = 0;
    for (; n < cap; n++) {
        uint16_t item = 0, qty = 0;
        if (save4_get_bag_slot(&e->s, (save4_pocket)pocket, n, &item, &qty) != SAVE4_OK || !item)
            break;
    }
    return n;
}

static void apply_value(np_app *app, np_editor *e, field f, int arg, int64_t v)
{
    save4_trainer t;
    save4_get_trainer(&e->s, &t);
    begin_edit(e);
    save4_status st = SAVE4_OK;
    switch (f) {
    case F_TR_TID: st = save4_set_trainer_ids(&e->s, (uint16_t)v, t.sid); break;
    case F_TR_SID: st = save4_set_trainer_ids(&e->s, t.tid, (uint16_t)v); break;
    case F_TR_MONEY: st = save4_set_money(&e->s, (uint32_t)v); break;
    case F_TR_COINS: st = save4_set_coins(&e->s, (uint16_t)v); break;
    case F_TR_HOURS: st = save4_set_play_time(&e->s, (uint16_t)v, t.play_minutes, t.play_seconds); break;
    case F_TR_MINUTES: st = save4_set_play_time(&e->s, t.play_hours, (uint8_t)v, t.play_seconds); break;
    case F_TR_SECONDS: st = save4_set_play_time(&e->s, t.play_hours, t.play_minutes, (uint8_t)v); break;
    case F_BAG_SLOT: {
        /* arg = slot; quantity edits */
        uint16_t item = 0, qty = 0;
        save4_get_bag_slot(&e->s, (save4_pocket)e->pocket, arg, &item, &qty);
        st = save4_set_bag_slot(&e->s, (save4_pocket)e->pocket, arg, item, (uint16_t)v);
        break;
    }
    case F_BAG_ADD: {
        /* arg = item chosen; add with quantity 1 at the end */
        int n = bag_count(e, e->pocket);
        if (n >= save4_pocket_capacity((save4_pocket)e->pocket))
            st = SAVE4_ERR_RANGE;
        else
            st = save4_set_bag_slot(&e->s, (save4_pocket)e->pocket, n, (uint16_t)v, 1);
        break;
    }
    default: st = edit_mon(e, f, arg, v); break;
    }
    end_edit(app, e, st);
}

/* ---- row model ----------------------------------------------------------------- */

static row *add_row(np_editor *e, field f, int arg, row_kind kind, const char *label)
{
    if (e->nrows == MAX_ROWS)
        return &e->rows[MAX_ROWS - 1];
    row *r = &e->rows[e->nrows++];
    r->f = f;
    r->arg = arg;
    r->kind = kind;
    SDL_strlcpy(r->label, label, sizeof r->label);
    r->value[0] = '\0';
    return r;
}

static void build_trainer(np_editor *e)
{
    save4_trainer t;
    if (save4_get_trainer(&e->s, &t) != SAVE4_OK)
        return;
    SDL_strlcpy(add_row(e, F_TR_NAME, 0, RK_TEXT, "Name")->value, t.name, 72);
    SDL_strlcpy(add_row(e, F_TR_GENDER, 0, RK_TOGGLE, "Gender")->value, t.gender ? "Female" : "Male", 72);
    SDL_snprintf(add_row(e, F_TR_TID, 0, RK_NUMBER, "Trainer ID")->value, 72, "%05u", t.tid);
    SDL_snprintf(add_row(e, F_TR_SID, 0, RK_NUMBER, "Secret ID")->value, 72, "%05u", t.sid);
    SDL_snprintf(add_row(e, F_TR_MONEY, 0, RK_NUMBER, "Money")->value, 72, "$%u", t.money);
    SDL_snprintf(add_row(e, F_TR_COINS, 0, RK_NUMBER, "Coins")->value, 72, "%u", t.coins);
    for (int b = 0; b < 8; b++) {
        char label[40];
        SDL_snprintf(label, sizeof label, "%s Badge", badge_names[b]);
        SDL_strlcpy(add_row(e, F_TR_BADGE, b, RK_TOGGLE, label)->value, (t.badges >> b) & 1 ? "Yes" : "No", 72);
    }
    SDL_snprintf(add_row(e, F_TR_HOURS, 0, RK_NUMBER, "Play time: hours")->value, 72, "%u", t.play_hours);
    SDL_snprintf(add_row(e, F_TR_MINUTES, 0, RK_NUMBER, "Play time: minutes")->value, 72, "%u", t.play_minutes);
    SDL_snprintf(add_row(e, F_TR_SECONDS, 0, RK_NUMBER, "Play time: seconds")->value, 72, "%u", t.play_seconds);
}

static void mon_summary(const np_editor *e, const pkm4 *p, char *buf, size_t n)
{
    pkm4_info in;
    pkm4_info_get(p, &in);
    char species[40];
    name_of(e, ND_TEXT_SPECIES, in.species, species, sizeof species);
    uint32_t level = in.has_party_data ? in.level : e->have_gd ? nd_level_for_exp(&e->gd, in.species, in.exp) : 0;
    if (in.is_egg)
        SDL_snprintf(buf, n, "Egg (%s)", species);
    else if (in.has_nickname)
        SDL_snprintf(buf, n, "%s (%s) Lv%u", in.nickname, species, (unsigned)level);
    else
        SDL_snprintf(buf, n, "%s Lv%u", species, (unsigned)level);
}

static void build_party(np_editor *e)
{
    int n = save4_party_count(&e->s);
    for (int i = 0; i < n; i++) {
        pkm4 p;
        char label[16];
        SDL_snprintf(label, sizeof label, "Slot %d", i + 1);
        row *r = add_row(e, F_PARTY_MON, i, RK_ACTION, label);
        if (save4_get_party(&e->s, i, &p) == SAVE4_OK)
            mon_summary(e, &p, r->value, sizeof r->value);
        else
            SDL_strlcpy(r->value, "(bad checksum)", sizeof r->value);
    }
    if (!n)
        add_row(e, F_INFO, 0, RK_INFO, "No Pokemon in the party yet.");
}

static void build_mon(np_editor *e)
{
    pkm4 p;
    if (get_mon(e, e->mon_box, e->mon_slot, &p) != SAVE4_OK) {
        add_row(e, F_INFO, 0, RK_INFO, "This Pokemon's data fails its checksum.");
        return;
    }
    pkm4_info in;
    pkm4_info_get(&p, &in);
    name_of(e, ND_TEXT_SPECIES, in.species, add_row(e, F_MON_SPECIES, 0, RK_CHOOSE, "Species")->value, 72);
    SDL_strlcpy(add_row(e, F_MON_NICK, 0, RK_TEXT, "Nickname")->value, in.nickname, 72);
    uint32_t level = e->have_gd ? nd_level_for_exp(&e->gd, in.species, in.exp) : in.level;
    if (e->have_gd)
        SDL_snprintf(add_row(e, F_MON_LEVEL, 0, RK_NUMBER, "Level")->value, 72, "%u", (unsigned)level);
    SDL_snprintf(add_row(e, F_MON_EXP, 0, e->have_gd ? RK_NUMBER : RK_INFO, "EXP")->value, 72, "%u", in.exp);
    char nature[32];
    name_of(e, ND_TEXT_NATURES, in.nature, nature, sizeof nature);
    SDL_strlcpy(add_row(e, F_INFO, 0, RK_INFO, "Nature")->value, nature, 72);
    name_of(e, ND_TEXT_ABILITIES, in.ability, add_row(e, F_MON_ABILITY, 0, RK_CHOOSE, "Ability")->value, 72);
    name_of(e, ND_TEXT_ITEMS, in.held_item, add_row(e, F_MON_ITEM, 0, RK_CHOOSE, "Held item")->value, 72);
    for (int m = 0; m < 4; m++) {
        char label[16], move[40];
        SDL_snprintf(label, sizeof label, "Move %d", m + 1);
        name_of(e, ND_TEXT_MOVES, in.moves[m], move, sizeof move);
        row *r = add_row(e, F_MON_MOVE, m, RK_CHOOSE, label);
        if (in.moves[m])
            SDL_snprintf(r->value, sizeof r->value, "%s (%u PP)", move, in.pp[m]);
        else
            SDL_strlcpy(r->value, "-", sizeof r->value);
    }
    for (int s = 0; s < 6; s++) {
        char label[24];
        SDL_snprintf(label, sizeof label, "IV %s", stat_names[s]);
        SDL_snprintf(add_row(e, F_MON_IV, s, RK_NUMBER, label)->value, 72, "%u", in.ivs[s]);
    }
    int ev_total = 0;
    for (int s = 0; s < 6; s++) {
        char label[24];
        SDL_snprintf(label, sizeof label, "EV %s", stat_names[s]);
        SDL_snprintf(add_row(e, F_MON_EV, s, RK_NUMBER, label)->value, 72, "%u", in.evs[s]);
        ev_total += in.evs[s];
    }
    SDL_snprintf(add_row(e, F_INFO, 0, RK_INFO, "EV total")->value, 72, "%d / 510", ev_total);
    SDL_snprintf(add_row(e, F_MON_FRIEND, 0, RK_NUMBER, "Friendship")->value, 72, "%u", in.friendship);
    if (in.has_party_data)
        SDL_snprintf(add_row(e, F_INFO, 0, RK_INFO, "Stats")->value, 72, "HP %u/%u  %u/%u/%u/%u/%u", in.hp,
                     in.stats[0], in.stats[1], in.stats[2], in.stats[3], in.stats[4], in.stats[5]);
    SDL_strlcpy(add_row(e, F_INFO, 0, RK_INFO, "Shiny")->value, in.shiny ? "Yes" : "No", 72);
    SDL_snprintf(add_row(e, F_INFO, 0, RK_INFO, "PID")->value, 72, "%08X", in.pid);
    SDL_snprintf(add_row(e, F_INFO, 0, RK_INFO, "Original trainer")->value, 72, "%s %05u/%05u", in.ot_name, in.tid,
                 in.sid);
    if (in.is_egg)
        SDL_strlcpy(add_row(e, F_INFO, 0, RK_INFO, "Egg")->value, "Yes", 72);
}

static void build_bag(np_editor *e)
{
    SDL_strlcpy(add_row(e, F_BAG_POCKET, 0, RK_TOGGLE, "Pocket")->value, save4_pocket_name((save4_pocket)e->pocket),
                72);
    int n = bag_count(e, e->pocket);
    for (int i = 0; i < n; i++) {
        uint16_t item = 0, qty = 0;
        save4_get_bag_slot(&e->s, (save4_pocket)e->pocket, i, &item, &qty);
        char name[40];
        name_of(e, ND_TEXT_ITEMS, item, name, sizeof name);
        row *r = add_row(e, F_BAG_SLOT, i, RK_ACTION, name);
        SDL_snprintf(r->value, sizeof r->value, "x%u", qty);
    }
    if (n < save4_pocket_capacity((save4_pocket)e->pocket))
        add_row(e, F_BAG_ADD, 0, RK_CHOOSE, "Add item...");
}

static void build_dex(np_editor *e)
{
    add_row(e, F_DEX_ALL, 0, RK_ACTION, "Mark every species caught");
    add_row(e, F_DEX_NONE, 0, RK_ACTION, "Clear the Pokedex");
    for (int sp = 1; sp <= SAVE4_DEX_MAX; sp++) {
        bool seen = false, caught = false;
        save4_dex_get(&e->s, (uint16_t)sp, &seen, &caught);
        char name[40], label[40];
        name_of(e, ND_TEXT_SPECIES, (uint32_t)sp, name, sizeof name);
        SDL_snprintf(label, sizeof label, "%03d %s", sp, name);
        SDL_strlcpy(add_row(e, F_DEX_SPECIES, sp, RK_TOGGLE, label)->value, caught ? "Caught" : seen ? "Seen" : "-",
                    72);
    }
}

static void build_rows(np_editor *e)
{
    e->nrows = 0;
    if (e->mon_open) {
        build_mon(e);
        return;
    }
    switch (e->tab) {
    case TAB_TRAINER: build_trainer(e); break;
    case TAB_PARTY: build_party(e); break;
    case TAB_BAG: build_bag(e); break;
    case TAB_DEX: build_dex(e); break;
    default: break;
    }
}

/* ---- overlays ------------------------------------------------------------------- */

static void open_number(np_editor *e, field f, int arg, int64_t value, int64_t min, int64_t max)
{
    e->ov = OV_NUMBER;
    e->num.f = f;
    e->num.arg = arg;
    e->num.min = min;
    e->num.max = max;
    e->num.value = SDL_clamp(value, min, max);
    e->num.digits = 1;
    for (int64_t m = max; m >= 10; m /= 10)
        e->num.digits++;
    e->num.cursor = e->num.digits - 1;
    e->num.typed = 0;
}

static int64_t pow10i(int n)
{
    int64_t v = 1;
    while (n-- > 0)
        v *= 10;
    return v;
}

static void chooser_filter(np_editor *e)
{
    /* For abilities with species data, the species' own (one or two). */
    e->ch.nids = 0;
    nd_text_kind kind = e->ch.f == F_MON_SPECIES   ? ND_TEXT_SPECIES
                        : e->ch.f == F_MON_MOVE    ? ND_TEXT_MOVES
                        : e->ch.f == F_MON_ABILITY ? ND_TEXT_ABILITIES
                                                   : ND_TEXT_ITEMS;
    if (e->ch.f == F_MON_ABILITY && e->have_gd && !e->ch.filter[0]) {
        pkm4 p;
        pkm4_info in;
        if (get_mon(e, e->mon_box, e->mon_slot, &p) == SAVE4_OK) {
            pkm4_info_get(&p, &in);
            const nd_species *sp = nd_species_get(&e->gd, in.species);
            if (sp) {
                e->ch.ids[e->ch.nids++] = sp->abilities[0];
                if (sp->abilities[1] && sp->abilities[1] != sp->abilities[0])
                    e->ch.ids[e->ch.nids++] = sp->abilities[1];
                return;
            }
        }
    }
    uint32_t count = kind_count(e, kind);
    uint32_t first = kind == ND_TEXT_SPECIES ? 1 : 0;
    if (kind == ND_TEXT_SPECIES && count > SAVE4_DEX_MAX + 1)
        count = SAVE4_DEX_MAX + 1; /* no form entries */
    for (uint32_t id = first; id < count && e->ch.nids < (int)SDL_arraysize(e->ch.ids); id++) {
        char name[48];
        name_of(e, kind, id, name, sizeof name);
        if (e->ch.filter[0] && !SDL_strcasestr(name, e->ch.filter))
            continue;
        if (kind != ND_TEXT_SPECIES && id && !SDL_strcmp(name, "-"))
            continue;
        e->ch.ids[e->ch.nids++] = (int)id;
    }
}

static nd_text_kind chooser_kind(const np_editor *e)
{
    return e->ch.f == F_MON_SPECIES   ? ND_TEXT_SPECIES
           : e->ch.f == F_MON_MOVE    ? ND_TEXT_MOVES
           : e->ch.f == F_MON_ABILITY ? ND_TEXT_ABILITIES
                                      : ND_TEXT_ITEMS;
}

static void open_chooser(np_app *app, np_editor *e, field f, int arg, int current)
{
    e->ov = OV_CHOOSER;
    e->ch.f = f;
    e->ch.arg = arg;
    e->ch.filter[0] = '\0';
    e->ch.sel = e->ch.scroll = 0;
    chooser_filter(e);
    for (int i = 0; i < e->ch.nids; i++)
        if (e->ch.ids[i] == current)
            e->ch.sel = i;
    e->ch.scroll = SDL_max(0, e->ch.sel - 4);
    SDL_StartTextInput(app->window);
}

static void close_overlay(np_app *app, np_editor *e)
{
    if (e->ov == OV_CHOOSER)
        SDL_StopTextInput(app->window);
    e->ov = OV_NONE;
}

static void chooser_pick(np_app *app, np_editor *e)
{
    if (e->ch.sel < 0 || e->ch.sel >= e->ch.nids)
        return;
    int id = e->ch.ids[e->ch.sel];
    field f = e->ch.f;
    int arg = e->ch.arg;
    close_overlay(app, e);
    if (f == F_BAG_SLOT) {
        uint16_t item = 0, qty = 0;
        save4_get_bag_slot(&e->s, (save4_pocket)e->pocket, arg, &item, &qty);
        begin_edit(e);
        end_edit(app, e, save4_set_bag_slot(&e->s, (save4_pocket)e->pocket, arg, (uint16_t)id, qty ? qty : 1));
        return;
    }
    apply_value(app, e, f, arg, id);
}

static void open_menu(np_editor *e, int count, const char *const *labels, const int *ids)
{
    e->ov = OV_MENU;
    e->menu.count = count;
    e->menu.sel = 0;
    for (int i = 0; i < count; i++) {
        e->menu.labels[i] = labels[i];
        e->menu.ids[i] = ids[i];
    }
}

/* ---- activation --------------------------------------------------------------- */

enum { MA_EDIT, MA_MOVE, MA_DELETE, MA_ITEM, MA_QTY, MA_REMOVE, MA_CANCEL, MA_PLACE };

static void open_mon(np_editor *e, int box, int slot)
{
    e->mon_open = 1;
    e->mon_box = box;
    e->mon_slot = slot;
    e->sel = e->scroll = 0;
    e->focus = FOCUS_LIST;
}

static void box_place(np_app *app, np_editor *e, int box, int slot)
{
    pkm4 a, b;
    if (save4_get_box_mon(&e->s, e->held_box, e->held_slot, &a) != SAVE4_OK ||
        save4_get_box_mon(&e->s, box, slot, &b) != SAVE4_OK) {
        np_app_toast(app, "That Pokemon's data fails its checksum");
        e->held_box = -1;
        return;
    }
    begin_edit(e);
    save4_status st = save4_set_box_mon(&e->s, box, slot, &a);
    if (st == SAVE4_OK)
        st = pkm4_is_empty(&b) ? save4_clear_box_mon(&e->s, e->held_box, e->held_slot)
                               : save4_set_box_mon(&e->s, e->held_box, e->held_slot, &b);
    end_edit(app, e, st);
    e->held_box = -1;
}

static void menu_pick(np_app *app, np_editor *e)
{
    int id = e->menu.ids[e->menu.sel];
    close_overlay(app, e);
    switch (id) {
    case MA_EDIT: open_mon(e, e->tab == TAB_PARTY ? -1 : e->box, e->tab == TAB_PARTY ? e->sel : e->cursor); break;
    case MA_MOVE:
        e->held_box = e->box;
        e->held_slot = e->cursor;
        np_app_toast(app, "Pick the slot to move it to");
        break;
    case MA_DELETE:
        begin_edit(e);
        end_edit(app, e, save4_clear_box_mon(&e->s, e->box, e->cursor));
        break;
    case MA_ITEM: {
        uint16_t item = 0, qty = 0;
        int slot = e->rows[e->sel].arg;
        save4_get_bag_slot(&e->s, (save4_pocket)e->pocket, slot, &item, &qty);
        open_chooser(app, e, F_BAG_SLOT, slot, item);
        break;
    }
    case MA_QTY: {
        uint16_t item = 0, qty = 0;
        int slot = e->rows[e->sel].arg;
        save4_get_bag_slot(&e->s, (save4_pocket)e->pocket, slot, &item, &qty);
        open_number(e, F_BAG_SLOT, slot, qty, 1, 999);
        break;
    }
    case MA_REMOVE:
        begin_edit(e);
        end_edit(app, e, bag_remove(e, e->pocket, e->rows[e->sel].arg));
        break;
    default: break;
    }
}

static void activate_box_cell(np_app *app, np_editor *e)
{
    if (e->cursor < 0)
        return;
    if (e->held_box >= 0) {
        if (e->held_box == e->box && e->held_slot == e->cursor)
            e->held_box = -1;
        else
            box_place(app, e, e->box, e->cursor);
        return;
    }
    pkm4 p;
    save4_status st = save4_get_box_mon(&e->s, e->box, e->cursor, &p);
    if (st == SAVE4_OK && pkm4_is_empty(&p))
        return;
    static const char *const labels[4] = {"Edit...", "Move", "Release (delete)", "Cancel"};
    static const int ids[4] = {MA_EDIT, MA_MOVE, MA_DELETE, MA_CANCEL};
    if (st != SAVE4_OK) /* corrupt data can still be released */
        open_menu(e, 2, labels + 2, ids + 2);
    else
        open_menu(e, 4, labels, ids);
}

static void activate_row(np_app *app, np_editor *e, int dir)
{
    if (e->sel < 0 || e->sel >= e->nrows)
        return;
    row *r = &e->rows[e->sel];
    save4_trainer t;
    save4_get_trainer(&e->s, &t);
    pkm4 p;
    pkm4_info in = {0};
    if (e->mon_open && get_mon(e, e->mon_box, e->mon_slot, &p) == SAVE4_OK)
        pkm4_info_get(&p, &in);
    switch (r->f) {
    case F_TR_NAME:
        e->text_field = F_TR_NAME;
        np_ui_open_text(app, NP_TEXT_TRAINER_NAME, t.name, 7);
        break;
    case F_TR_GENDER:
        begin_edit(e);
        end_edit(app, e, save4_set_gender(&e->s, t.gender ? 0 : 1));
        break;
    case F_TR_BADGE:
        begin_edit(e);
        end_edit(app, e, save4_set_badges(&e->s, (uint8_t)(t.badges ^ (1u << r->arg))));
        break;
    case F_TR_TID: open_number(e, r->f, 0, t.tid, 0, 65535); break;
    case F_TR_SID: open_number(e, r->f, 0, t.sid, 0, 65535); break;
    case F_TR_MONEY: open_number(e, r->f, 0, t.money, 0, SAVE4_MONEY_MAX); break;
    case F_TR_COINS: open_number(e, r->f, 0, t.coins, 0, SAVE4_COINS_MAX); break;
    case F_TR_HOURS: open_number(e, r->f, 0, t.play_hours, 0, 999); break;
    case F_TR_MINUTES: open_number(e, r->f, 0, t.play_minutes, 0, 59); break;
    case F_TR_SECONDS: open_number(e, r->f, 0, t.play_seconds, 0, 59); break;
    case F_PARTY_MON: {
        static const char *const labels[2] = {"Edit...", "Cancel"};
        static const int ids[2] = {MA_EDIT, MA_CANCEL};
        open_menu(e, 2, labels, ids);
        break;
    }
    case F_MON_SPECIES: open_chooser(app, e, r->f, 0, in.species); break;
    case F_MON_NICK:
        e->text_field = F_MON_NICK;
        np_ui_open_text(app, NP_TEXT_NICKNAME, in.nickname, 10);
        break;
    case F_MON_LEVEL:
        open_number(e, r->f, 0, nd_level_for_exp(&e->gd, in.species, in.exp), 1, 100);
        break;
    case F_MON_EXP: open_number(e, r->f, 0, in.exp, 0, nd_exp_for_level(&e->gd, in.species, 100)); break;
    case F_MON_ABILITY: open_chooser(app, e, r->f, 0, in.ability); break;
    case F_MON_ITEM: open_chooser(app, e, r->f, 0, in.held_item); break;
    case F_MON_MOVE: open_chooser(app, e, r->f, r->arg, in.moves[r->arg]); break;
    case F_MON_IV: open_number(e, r->f, r->arg, in.ivs[r->arg], 0, 31); break;
    case F_MON_EV: open_number(e, r->f, r->arg, in.evs[r->arg], 0, 255); break;
    case F_MON_FRIEND: open_number(e, r->f, 0, in.friendship, 0, 255); break;
    case F_BAG_POCKET: e->pocket = wrapi(e->pocket + (dir ? dir : 1), SAVE4_POCKET_COUNT); break;
    case F_BAG_SLOT: {
        static const char *const labels[4] = {"Change item...", "Quantity...", "Remove", "Cancel"};
        static const int ids[4] = {MA_ITEM, MA_QTY, MA_REMOVE, MA_CANCEL};
        open_menu(e, 4, labels, ids);
        break;
    }
    case F_BAG_ADD: open_chooser(app, e, F_BAG_ADD, 0, 1); break;
    case F_DEX_ALL:
    case F_DEX_NONE: {
        begin_edit(e);
        save4_status st = SAVE4_OK;
        for (int sp = 1; sp <= SAVE4_DEX_MAX && st == SAVE4_OK; sp++)
            st = save4_dex_set(&e->s, (uint16_t)sp, r->f == F_DEX_ALL, r->f == F_DEX_ALL);
        end_edit(app, e, st);
        break;
    }
    case F_DEX_SPECIES: {
        bool seen = false, caught = false;
        save4_dex_get(&e->s, (uint16_t)r->arg, &seen, &caught);
        int state = caught ? 2 : seen ? 1 : 0;
        state = wrapi(state + (dir < 0 ? -1 : 1), 3);
        begin_edit(e);
        end_edit(app, e, save4_dex_set(&e->s, (uint16_t)r->arg, state >= 1, state == 2));
        break;
    }
    default: break;
    }
}

const char *np_editor_text_done(np_app *app, const char *text)
{
    np_editor *e = app->editor;
    if (!e)
        return "The editor is closed.";
    begin_edit(e);
    save4_status st;
    if (e->text_field == F_TR_NAME) {
        st = *text ? save4_set_trainer_name(&e->s, text) : SAVE4_ERR_ARG;
    } else {
        pkm4 p;
        st = get_mon(e, e->mon_box, e->mon_slot, &p);
        if (st == SAVE4_OK) {
            pkm4_info in;
            pkm4_info_get(&p, &in);
            const char *species = e->have_names ? nd_name(&e->names, ND_TEXT_SPECIES, in.species) : NULL;
            /* An empty nickname, or the species name, means "no nickname". */
            if (!*text && species)
                st = pkm4_set_nickname(&p, species, false);
            else if (*text)
                st = pkm4_set_nickname(&p, text, !(species && !SDL_strcmp(species, text)));
            else
                st = SAVE4_ERR_ARG;
            if (st == SAVE4_OK)
                st = put_mon(e, e->mon_box, e->mon_slot, &p);
        }
    }
    if (st != SAVE4_OK) {
        SDL_memcpy(e->s.img, e->scratch, e->s.len);
        save4_revalidate(&e->s);
        return st == SAVE4_ERR_ENCODE ? "Some characters are not in the game's character set."
                                      : "That name cannot be used.";
    }
    end_edit(app, e, st);
    np_app_open_page(app, NP_PAGE_EDITOR);
    return NULL;
}

void np_editor_text_cancel(np_app *app) { np_app_open_page(app, NP_PAGE_EDITOR); }

/* ---- commands --------------------------------------------------------------------- */

static void leave_editor(np_app *app)
{
    np_game g = app->editor->game;
    char slot[NP_SLOT_NAME_MAX + 1];
    SDL_strlcpy(slot, app->editor->slot, sizeof slot);
    np_editor_close(app);
    app->slots_game = g;
    np_app_refresh_slots(app, slot);
    np_app_open_page(app, NP_PAGE_SLOT_MENU);
}

static void request_close(np_app *app, np_editor *e)
{
    if (e->dirty) {
        e->ov = OV_DISCARD;
        e->discard_sel = 0;
    } else {
        leave_editor(app);
    }
}

static void footer_activate(np_app *app, np_editor *e, int which)
{
    switch (which) {
    case FOOT_UNDO: restore(app, e, 0); break;
    case FOOT_REDO: restore(app, e, 1); break;
    case FOOT_SAVE: save_now(app, e); break;
    default: request_close(app, e); break;
    }
}

static void switch_tab(np_editor *e, int tab)
{
    e->tab = wrapi(tab, TAB_COUNT);
    e->mon_open = 0;
    e->sel = e->scroll = 0;
    e->cursor = 0;
    e->held_box = -1;
}

static void number_command(np_app *app, np_editor *e, np_menu_cmd cmd)
{
    int64_t step = pow10i(e->num.digits - 1 - e->num.cursor);
    switch (cmd) {
    case NP_CMD_UP: e->num.value = SDL_min(e->num.max, e->num.value + step); break;
    case NP_CMD_DOWN: e->num.value = SDL_max(e->num.min, e->num.value - step); break;
    case NP_CMD_LEFT: e->num.cursor = SDL_max(0, e->num.cursor - 1); break;
    case NP_CMD_RIGHT: e->num.cursor = SDL_min(e->num.digits - 1, e->num.cursor + 1); break;
    case NP_CMD_CONFIRM:
    case NP_CMD_CLOSE:
        close_overlay(app, e);
        apply_value(app, e, e->num.f, e->num.arg, e->num.value);
        break;
    case NP_CMD_BACK: close_overlay(app, e); break;
    default: break;
    }
}

static void chooser_command(np_app *app, np_editor *e, np_menu_cmd cmd)
{
    switch (cmd) {
    case NP_CMD_UP: e->ch.sel = SDL_max(0, e->ch.sel - 1); break;
    case NP_CMD_DOWN: e->ch.sel = SDL_min(e->ch.nids - 1, e->ch.sel + 1); break;
    case NP_CMD_LEFT:
    case NP_CMD_TAB_PREV: e->ch.sel = SDL_max(0, e->ch.sel - 10); break;
    case NP_CMD_RIGHT:
    case NP_CMD_TAB_NEXT: e->ch.sel = SDL_min(e->ch.nids - 1, e->ch.sel + 10); break;
    case NP_CMD_CONFIRM:
    case NP_CMD_CLOSE: chooser_pick(app, e); break;
    case NP_CMD_BACK: close_overlay(app, e); break;
    default: break;
    }
}

void np_editor_command(np_app *app, np_menu_cmd cmd)
{
    np_editor *e = app->editor;
    if (!e) {
        np_app_open_page(app, NP_PAGE_SLOT_MENU);
        return;
    }
    switch (e->ov) {
    case OV_NUMBER: number_command(app, e, cmd); return;
    case OV_CHOOSER: chooser_command(app, e, cmd); return;
    case OV_MENU:
        if (cmd == NP_CMD_UP || cmd == NP_CMD_DOWN)
            e->menu.sel = wrapi(e->menu.sel + (cmd == NP_CMD_UP ? -1 : 1), e->menu.count);
        else if (cmd == NP_CMD_CONFIRM)
            menu_pick(app, e);
        else if (cmd == NP_CMD_BACK || cmd == NP_CMD_CLOSE)
            close_overlay(app, e);
        return;
    case OV_DISCARD:
        if (cmd == NP_CMD_LEFT || cmd == NP_CMD_RIGHT || cmd == NP_CMD_UP || cmd == NP_CMD_DOWN)
            e->discard_sel = wrapi(e->discard_sel + (cmd == NP_CMD_LEFT || cmd == NP_CMD_UP ? -1 : 1), 3);
        else if (cmd == NP_CMD_BACK)
            e->ov = OV_NONE;
        else if (cmd == NP_CMD_CONFIRM) {
            e->ov = OV_NONE;
            if (e->discard_sel == 0) {
                save_now(app, e);
                if (!e->dirty)
                    leave_editor(app);
            } else if (e->discard_sel == 1) {
                leave_editor(app);
            }
        }
        return;
    default: break;
    }

    switch (cmd) {
    case NP_CMD_TAB_PREV:
    case NP_CMD_TAB_NEXT: switch_tab(e, e->tab + (cmd == NP_CMD_TAB_PREV ? -1 : 1)); return;
    case NP_CMD_X: restore(app, e, 0); return;
    case NP_CMD_Y: restore(app, e, 1); return;
    case NP_CMD_CLOSE: request_close(app, e); return;
    case NP_CMD_BACK:
        if (e->held_box >= 0)
            e->held_box = -1;
        else if (e->mon_open) {
            e->mon_open = 0;
            e->sel = e->tab == TAB_PARTY ? e->mon_slot : 0;
            e->scroll = 0;
        } else if (e->focus != FOCUS_LIST)
            e->focus = FOCUS_LIST;
        else
            request_close(app, e);
        return;
    default: break;
    }

    if (e->focus == FOCUS_TABS) {
        if (cmd == NP_CMD_LEFT || cmd == NP_CMD_RIGHT)
            switch_tab(e, e->tab + (cmd == NP_CMD_LEFT ? -1 : 1));
        else if (cmd == NP_CMD_DOWN || cmd == NP_CMD_CONFIRM)
            e->focus = FOCUS_LIST;
        else if (cmd == NP_CMD_UP)
            e->focus = FOCUS_FOOTER;
        return;
    }
    if (e->focus == FOCUS_FOOTER) {
        if (cmd == NP_CMD_LEFT || cmd == NP_CMD_RIGHT)
            e->foot = wrapi(e->foot + (cmd == NP_CMD_LEFT ? -1 : 1), FOOT_COUNT);
        else if (cmd == NP_CMD_UP)
            e->focus = FOCUS_LIST;
        else if (cmd == NP_CMD_DOWN)
            e->focus = FOCUS_TABS;
        else if (cmd == NP_CMD_CONFIRM)
            footer_activate(app, e, e->foot);
        return;
    }

    if (e->tab == TAB_BOXES && !e->mon_open) {
        int c = e->cursor;
        switch (cmd) {
        case NP_CMD_LEFT:
            if (c < 0)
                e->box = wrapi(e->box - 1, SAVE4_BOX_COUNT);
            else if (c % 6)
                e->cursor--;
            break;
        case NP_CMD_RIGHT:
            if (c < 0)
                e->box = wrapi(e->box + 1, SAVE4_BOX_COUNT);
            else if (c % 6 < 5)
                e->cursor++;
            break;
        case NP_CMD_UP:
            if (c < 0)
                e->focus = FOCUS_TABS;
            else
                e->cursor = c < 6 ? -1 : c - 6;
            break;
        case NP_CMD_DOWN:
            if (c < 0)
                e->cursor = 0;
            else if (c >= 24)
                e->focus = FOCUS_FOOTER;
            else
                e->cursor = c + 6;
            break;
        case NP_CMD_CONFIRM: activate_box_cell(app, e); break;
        default: break;
        }
        return;
    }

    switch (cmd) {
    case NP_CMD_UP:
        if (e->sel == 0)
            e->focus = FOCUS_TABS;
        else
            e->sel--;
        break;
    case NP_CMD_DOWN:
        if (e->sel >= e->nrows - 1)
            e->focus = FOCUS_FOOTER;
        else
            e->sel++;
        break;
    case NP_CMD_LEFT:
    case NP_CMD_RIGHT:
        if (e->sel < e->nrows && e->rows[e->sel].kind == RK_TOGGLE)
            activate_row(app, e, cmd == NP_CMD_LEFT ? -1 : 1);
        break;
    case NP_CMD_CONFIRM: activate_row(app, e, 1); break;
    default: break;
    }
}

int np_editor_event(np_app *app, const SDL_Event *e)
{
    np_editor *ed = app->editor;
    if (!ed)
        return 0;
    if (e->type == SDL_EVENT_TEXT_INPUT) {
        if (ed->ov == OV_CHOOSER) {
            SDL_strlcat(ed->ch.filter, e->text.text, sizeof ed->ch.filter);
            chooser_filter(ed);
            ed->ch.sel = ed->ch.scroll = 0;
        }
        return 1;
    }
    if (e->type != SDL_EVENT_KEY_DOWN)
        return 0;
    SDL_Keymod mod = e->key.mod;
    int cmd_mod = (mod & (SDL_KMOD_CTRL | SDL_KMOD_GUI)) != 0;
    if (cmd_mod && e->key.scancode == SDL_SCANCODE_Z) {
        restore(app, ed, (mod & SDL_KMOD_SHIFT) != 0);
        return 1;
    }
    if (cmd_mod && e->key.scancode == SDL_SCANCODE_Y) {
        restore(app, ed, 1);
        return 1;
    }
    if (cmd_mod && e->key.scancode == SDL_SCANCODE_S) {
        save_now(app, ed);
        return 1;
    }
    if (ed->ov == OV_CHOOSER) {
        /* While filtering, letters are text; keep Backspace for the filter. */
        if (e->key.scancode == SDL_SCANCODE_BACKSPACE) {
            size_t n = SDL_strlen(ed->ch.filter);
            if (n)
                ed->ch.filter[n - 1] = '\0';
            chooser_filter(ed);
            ed->ch.sel = ed->ch.scroll = 0;
            return 1;
        }
        SDL_Scancode sc = e->key.scancode;
        int nav = sc == SDL_SCANCODE_UP || sc == SDL_SCANCODE_DOWN || sc == SDL_SCANCODE_LEFT ||
                  sc == SDL_SCANCODE_RIGHT || sc == SDL_SCANCODE_RETURN || sc == SDL_SCANCODE_KP_ENTER ||
                  sc == SDL_SCANCODE_ESCAPE || sc == SDL_SCANCODE_PAGEUP || sc == SDL_SCANCODE_PAGEDOWN;
        return !nav; /* everything else is typed text */
    }
    if (ed->ov == OV_NUMBER) {
        SDL_Keycode k = e->key.key;
        if (k >= SDLK_0 && k <= SDLK_9) {
            int64_t v = (ed->num.typed++ ? ed->num.value * 10 : 0) + (k - SDLK_0);
            if (v > ed->num.max)
                v = k - SDLK_0;
            ed->num.value = SDL_clamp(v, ed->num.min, ed->num.max);
            return 1;
        }
        if (e->key.scancode == SDL_SCANCODE_BACKSPACE) {
            ed->num.value = SDL_max(ed->num.min, ed->num.value / 10);
            return 1;
        }
    }
    return 0;
}

/* ---- pointer -------------------------------------------------------------------------- */

void np_editor_hit(np_app *app, int id, int activate, int dir)
{
    np_editor *e = app->editor;
    if (!e)
        return;
    int group = (id - NP_EDITOR_HIT_BASE) / 1000, idx = (id - NP_EDITOR_HIT_BASE) % 1000;
    switch (group) {
    case HG_TAB:
        if (activate)
            switch_tab(e, idx);
        break;
    case HG_ROW:
        e->focus = FOCUS_LIST;
        e->sel = idx;
        if (activate)
            activate_row(app, e, dir);
        break;
    case HG_FOOT:
        e->focus = FOCUS_FOOTER;
        e->foot = idx;
        if (activate)
            footer_activate(app, e, idx);
        break;
    case HG_BOX:
        e->focus = FOCUS_LIST;
        if (idx >= 30) {
            if (activate)
                e->box = wrapi(e->box + (idx == 30 ? -1 : 1), SAVE4_BOX_COUNT);
            break;
        }
        e->cursor = idx;
        if (activate)
            activate_box_cell(app, e);
        break;
    case HG_NUM:
        if (!activate)
            break;
        if (idx < 100) { /* digit idx up */
            e->num.cursor = idx;
            number_command(app, e, NP_CMD_UP);
        } else if (idx < 200) {
            e->num.cursor = idx - 100;
            number_command(app, e, NP_CMD_DOWN);
        } else {
            number_command(app, e, idx == 200 ? NP_CMD_CONFIRM : NP_CMD_BACK);
        }
        break;
    case HG_CHOOSE:
        if (idx == 998 || idx == 999) {
            if (activate)
                chooser_command(app, e, idx == 998 ? NP_CMD_TAB_PREV : NP_CMD_TAB_NEXT);
            break;
        }
        if (idx == 997) {
            if (activate)
                close_overlay(app, e);
            break;
        }
        e->ch.sel = e->ch.scroll + idx;
        if (activate)
            chooser_pick(app, e);
        break;
    case HG_MENU:
        e->menu.sel = idx;
        if (activate)
            menu_pick(app, e);
        break;
    case HG_DISCARD:
        e->discard_sel = idx;
        if (activate)
            np_editor_command(app, NP_CMD_CONFIRM);
        break;
    case HG_SCROLL:
        if (activate) {
            for (int i = 0; i < 8; i++)
                np_editor_command(app, idx ? NP_CMD_DOWN : NP_CMD_UP);
        }
        break;
    default: break;
    }
}

/* ---- drawing ----------------------------------------------------------------------------- */

static void draw_tabs(np_app *app, np_editor *e, const np_page_frame *f, float y)
{
    float x = f->panel.x + 2 * f->cw;
    for (int t = 0; t < TAB_COUNT; t++) {
        float w = (float)(SDL_strlen(tab_names[t]) + 2) * f->cw;
        SDL_FRect r = {x, y - 3 * f->s, w, f->lh + 2 * f->s};
        int on = t == e->tab;
        np_ui_fill(app, r, on ? (SDL_Color){255, 205, 80, 70} : (SDL_Color){255, 255, 255, 14});
        if (on && e->focus == FOCUS_TABS)
            np_ui_frame(app, r, f->s, accent);
        np_ui_text(app, x + f->cw, y, f->s, tab_names[t], on ? accent : white);
        np_ui_hit(app, r, hit_id(HG_TAB, t));
        x += w + f->cw * 0.5f;
    }
    char info[64];
    SDL_snprintf(info, sizeof info, "%s%s", e->dirty ? "* " : "", e->slot);
    float iw = (float)SDL_strlen(info) * f->cw;
    if (x + iw < f->panel.x + f->panel.w - 2 * f->cw)
        np_ui_text(app, f->panel.x + f->panel.w - 2 * f->cw - iw, y, f->s, info, e->dirty ? warn : dim);
}

static void draw_rows(np_app *app, np_editor *e, const np_page_frame *f, float y0, int rows)
{
    e->sel = SDL_clamp(e->sel, 0, SDL_max(0, e->nrows - 1));
    if (e->sel < e->scroll)
        e->scroll = e->sel;
    if (e->sel >= e->scroll + rows)
        e->scroll = e->sel - rows + 1;
    e->scroll = SDL_clamp(e->scroll, 0, SDL_max(0, e->nrows - rows));
    float x = f->panel.x + 2 * f->cw, vx = f->panel.x + f->panel.w * 0.45f;
    int vcols = (int)((f->panel.x + f->panel.w - 2 * f->cw - vx) / f->cw);
    for (int i = 0; i < rows && e->scroll + i < e->nrows; i++) {
        int idx = e->scroll + i;
        const row *r = &e->rows[idx];
        float y = y0 + (float)i * f->lh;
        SDL_FRect rr = {f->panel.x + f->cw, y - 2 * f->s, f->panel.w - 2 * f->cw, f->lh};
        int on = idx == e->sel && e->focus == FOCUS_LIST && e->ov == OV_NONE;
        if (on)
            np_ui_fill(app, rr, hilite);
        SDL_Color lc = r->kind == RK_INFO ? dim : on ? accent : white;
        np_ui_text_clip(app, x, y, f->s, r->label, (int)((vx - x) / f->cw) - 1, lc);
        if (r->value[0]) {
            char shown[96];
            SDL_snprintf(shown, sizeof shown, on && r->kind == RK_TOGGLE ? "< %s >" : "%s", r->value);
            np_ui_text_clip(app, vx, y, f->s, shown, vcols, r->kind == RK_INFO ? dim : on ? accent : white);
        }
        if (r->kind != RK_INFO)
            np_ui_hit(app, rr, hit_id(HG_ROW, idx));
    }
    if (e->nrows > rows) {
        float bx = f->panel.x + f->panel.w - 2 * f->cw - 3 * f->cw;
        np_ui_button(app, (SDL_FRect){bx, y0 - 2 * f->s, 3 * f->cw, f->lh}, "^", 0, hit_id(HG_SCROLL, 0), f->s);
        np_ui_button(app, (SDL_FRect){bx, y0 + (float)(rows - 1) * f->lh - 2 * f->s, 3 * f->cw, f->lh}, "v", 0,
                     hit_id(HG_SCROLL, 1), f->s);
    }
}

static void draw_boxes(np_app *app, np_editor *e, const np_page_frame *f, float y0, float h)
{
    char name[48], title[64];
    if (save4_get_box_name(&e->s, e->box, name, sizeof name) != SAVE4_OK)
        SDL_snprintf(name, sizeof name, "Box %d", e->box + 1);
    SDL_snprintf(title, sizeof title, "%s (%d/%d)", name, e->box + 1, SAVE4_BOX_COUNT);
    float x0 = f->panel.x + 2 * f->cw, w = f->panel.w - 4 * f->cw;
    int head_on = e->cursor < 0 && e->focus == FOCUS_LIST;
    np_ui_button(app, (SDL_FRect){x0, y0 - 2 * f->s, 3 * f->cw, f->lh}, "<", 0, hit_id(HG_BOX, 30), f->s);
    np_ui_button(app, (SDL_FRect){x0 + w - 3 * f->cw, y0 - 2 * f->s, 3 * f->cw, f->lh}, ">", 0, hit_id(HG_BOX, 31),
                 f->s);
    float tw = (float)SDL_strlen(title) * f->cw;
    np_ui_text(app, x0 + (w - tw) * 0.5f, y0, f->s, title, head_on ? accent : white);
    float gy = y0 + 1.5f * f->lh;
    float cw = w / 6.0f, ch = SDL_min((h - 1.5f * f->lh) / 5.0f, 3.2f * f->lh);
    for (int i = 0; i < SAVE4_BOX_SLOTS; i++) {
        SDL_FRect r = {x0 + (float)(i % 6) * cw, gy + (float)(i / 6) * ch, cw - f->s * 2, ch - f->s * 2};
        pkm4 p;
        save4_status st = save4_get_box_mon(&e->s, e->box, i, &p);
        int empty = st == SAVE4_OK && pkm4_is_empty(&p);
        int held = e->held_box == e->box && e->held_slot == i;
        int on = i == e->cursor && e->focus == FOCUS_LIST && e->ov == OV_NONE;
        np_ui_fill(app, r, held ? (SDL_Color){120, 200, 255, 80} : empty ? (SDL_Color){255, 255, 255, 10}
                                                                        : (SDL_Color){255, 255, 255, 30});
        if (on)
            np_ui_frame(app, r, f->s, accent);
        int cols = (int)(r.w / f->cw) - 1;
        if (st != SAVE4_OK) {
            np_ui_text_clip(app, r.x + f->cw * 0.5f, r.y + f->s * 3, f->s, "(bad)", cols, warn);
        } else if (!empty) {
            pkm4_info in;
            pkm4_info_get(&p, &in);
            char sp[40], lv[16];
            name_of(e, ND_TEXT_SPECIES, in.species, sp, sizeof sp);
            np_ui_text_clip(app, r.x + f->cw * 0.5f, r.y + f->s * 3, f->s, in.is_egg ? "Egg" : sp, cols,
                            on ? accent : white);
            uint32_t level = e->have_gd ? nd_level_for_exp(&e->gd, in.species, in.exp) : 0;
            if (level && r.h > 2.2f * f->lh) {
                SDL_snprintf(lv, sizeof lv, "Lv%u%s", (unsigned)level, in.shiny ? " *" : "");
                np_ui_text_clip(app, r.x + f->cw * 0.5f, r.y + f->s * 3 + f->lh, f->s, lv, cols, dim);
            }
        }
        np_ui_hit(app, r, hit_id(HG_BOX, i));
    }
}

static void draw_footer(np_app *app, np_editor *e, const np_page_frame *f)
{
    static const char *const labels[FOOT_COUNT] = {"Undo", "Redo", "Save", "Close"};
    float bh = 1.8f * f->lh, by = f->panel.y + f->panel.h - bh - 0.6f * f->lh;
    float bw = 8 * f->cw, x = f->panel.x + 2 * f->cw;
    for (int i = 0; i < FOOT_COUNT; i++) {
        int on = e->focus == FOCUS_FOOTER && e->foot == i && e->ov == OV_NONE;
        np_ui_button(app, (SDL_FRect){x, by, bw, bh}, labels[i], on, hit_id(HG_FOOT, i), f->s);
        x += bw + f->cw;
    }
    const char *hint = e->held_box >= 0   ? "Pick a slot to move to. B: cancel"
                       : e->tab == TAB_BOXES ? "L/R: tab  A: select  X/Y: undo/redo"
                                             : "L/R: tab  A: edit  X/Y: undo/redo  Ctrl+S: save";
    if (x + 4 * f->cw < f->panel.x + f->panel.w)
        np_ui_text_clip(app, x + f->cw, by + (bh - 8 * f->s) * 0.5f, f->s, hint,
                        (int)((f->panel.x + f->panel.w - x - 3 * f->cw) / f->cw), dim);
}

static SDL_FRect overlay_box(np_app *app, const np_page_frame *f, float w, float h)
{
    SDL_FRect r = {floorf(f->panel.x + (f->panel.w - w) * 0.5f), floorf(f->panel.y + (f->panel.h - h) * 0.5f), w, h};
    np_ui_fill(app, (SDL_FRect){f->panel.x, f->panel.y, f->panel.w, f->panel.h}, (SDL_Color){0, 0, 0, 120});
    np_ui_fill(app, r, (SDL_Color){30, 34, 48, 250});
    np_ui_frame(app, r, f->s, accent);
    return r;
}

static void draw_number(np_app *app, np_editor *e, const np_page_frame *f)
{
    float s2 = 2 * f->s, dw = 8 * s2 + 2 * f->cw;
    float w = SDL_max((float)e->num.digits * dw + 4 * f->cw, 30 * f->cw), h = 11 * f->lh;
    SDL_FRect r = overlay_box(app, f, w, h);
    char range[64];
    SDL_snprintf(range, sizeof range, "%lld .. %lld", (long long)e->num.min, (long long)e->num.max);
    np_ui_text(app, r.x + 2 * f->cw, r.y + f->lh, f->s, range, dim);
    float x0 = r.x + (r.w - (float)e->num.digits * dw) * 0.5f, y = r.y + 4 * f->lh;
    for (int d = 0; d < e->num.digits; d++) {
        int64_t digit = e->num.value / pow10i(e->num.digits - 1 - d) % 10;
        char ch[2] = {(char)('0' + digit), 0};
        float x = x0 + (float)d * dw;
        int on = d == e->num.cursor;
        np_ui_button(app, (SDL_FRect){x, y - 2.2f * f->lh, dw - f->cw, 1.6f * f->lh}, "+", 0, hit_id(HG_NUM, d),
                     f->s);
        np_ui_text(app, x + f->cw * 0.5f, y, s2, ch, on ? accent : white);
        np_ui_button(app, (SDL_FRect){x, y + 2.4f * f->lh, dw - f->cw, 1.6f * f->lh}, "-", 0,
                     hit_id(HG_NUM, 100 + d), f->s);
    }
    float by = r.y + r.h - 2.4f * f->lh;
    np_ui_button(app, (SDL_FRect){r.x + 2 * f->cw, by, 8 * f->cw, 1.8f * f->lh}, "OK", 0, hit_id(HG_NUM, 200), f->s);
    np_ui_button(app, (SDL_FRect){r.x + r.w - 10 * f->cw, by, 8 * f->cw, 1.8f * f->lh}, "Cancel", 0,
                 hit_id(HG_NUM, 201), f->s);
}

static void draw_chooser(np_app *app, np_editor *e, const np_page_frame *f)
{
    float w = SDL_min(f->panel.w - 4 * f->cw, 44 * f->cw), h = f->panel.h - 4 * f->lh;
    SDL_FRect r = overlay_box(app, f, w, h);
    char head[64];
    SDL_snprintf(head, sizeof head, "Filter: %s_", e->ch.filter);
    np_ui_text_clip(app, r.x + 2 * f->cw, r.y + f->lh, f->s, head, (int)(r.w / f->cw) - 4, accent);
    int rows = (int)((r.h - 5.5f * f->lh) / f->lh);
    if (rows < 1)
        rows = 1;
    e->ch.sel = SDL_clamp(e->ch.sel, 0, SDL_max(0, e->ch.nids - 1));
    if (e->ch.sel < e->ch.scroll)
        e->ch.scroll = e->ch.sel;
    if (e->ch.sel >= e->ch.scroll + rows)
        e->ch.scroll = e->ch.sel - rows + 1;
    nd_text_kind kind = chooser_kind(e);
    for (int i = 0; i < rows && e->ch.scroll + i < e->ch.nids; i++) {
        int idx = e->ch.scroll + i, on = idx == e->ch.sel;
        float y = r.y + 2.8f * f->lh + (float)i * f->lh;
        SDL_FRect rr = {r.x + f->cw, y - 2 * f->s, r.w - 2 * f->cw, f->lh};
        if (on)
            np_ui_fill(app, rr, hilite);
        char name[48], line[64];
        name_of(e, kind, (uint32_t)e->ch.ids[idx], name, sizeof name);
        SDL_snprintf(line, sizeof line, "%3d %s", e->ch.ids[idx], name);
        np_ui_text_clip(app, r.x + 2 * f->cw, y, f->s, line, (int)(r.w / f->cw) - 4, on ? accent : white);
        np_ui_hit(app, rr, hit_id(HG_CHOOSE, i));
    }
    if (!e->ch.nids)
        np_ui_text(app, r.x + 2 * f->cw, r.y + 2.8f * f->lh, f->s, "No match", dim);
    float by = r.y + r.h - 2.4f * f->lh;
    np_ui_button(app, (SDL_FRect){r.x + 2 * f->cw, by, 6 * f->cw, 1.8f * f->lh}, "Up", 0, hit_id(HG_CHOOSE, 998), f->s);
    np_ui_button(app, (SDL_FRect){r.x + 9 * f->cw, by, 6 * f->cw, 1.8f * f->lh}, "Down", 0, hit_id(HG_CHOOSE, 999),
                 f->s);
    np_ui_button(app, (SDL_FRect){r.x + r.w - 10 * f->cw, by, 8 * f->cw, 1.8f * f->lh}, "Cancel", 0,
                 hit_id(HG_CHOOSE, 997), f->s);
}

static void draw_menu(np_app *app, np_editor *e, const np_page_frame *f)
{
    float w = 24 * f->cw, h = (float)(e->menu.count + 2) * f->lh * 1.4f;
    SDL_FRect r = overlay_box(app, f, w, h);
    for (int i = 0; i < e->menu.count; i++) {
        float y = r.y + f->lh + (float)i * 1.4f * f->lh;
        SDL_FRect rr = {r.x + f->cw, y - 3 * f->s, r.w - 2 * f->cw, 1.3f * f->lh};
        int on = i == e->menu.sel;
        if (on)
            np_ui_fill(app, rr, hilite);
        np_ui_text(app, r.x + 2 * f->cw, y, f->s, e->menu.labels[i], on ? accent : white);
        np_ui_hit(app, rr, hit_id(HG_MENU, i));
    }
}

static void draw_discard(np_app *app, np_editor *e, const np_page_frame *f)
{
    SDL_FRect r = overlay_box(app, f, SDL_min(f->panel.w - 4 * f->cw, 52 * f->cw), 7 * f->lh);
    np_ui_text_clip(app, r.x + 2 * f->cw, r.y + f->lh, f->s, "This save has unsaved changes.", (int)(r.w / f->cw) - 4,
                    white);
    static const char *const labels[3] = {"Save", "Discard", "Keep editing"};
    float bw = (r.w - 6 * f->cw) / 3.0f, by = r.y + r.h - 2.8f * f->lh;
    for (int i = 0; i < 3; i++)
        np_ui_button(app, (SDL_FRect){r.x + 2 * f->cw + (float)i * (bw + f->cw), by, bw, 2 * f->lh}, labels[i],
                     e->discard_sel == i, hit_id(HG_DISCARD, i), f->s);
}

void np_editor_draw(np_app *app)
{
    np_editor *e = app->editor;
    if (!e)
        return;
    np_page_frame f;
    char title[96];
    if (e->mon_open) {
        if (e->mon_box < 0)
            SDL_snprintf(title, sizeof title, "Party slot %d", e->mon_slot + 1);
        else
            SDL_snprintf(title, sizeof title, "Box %d, slot %d", e->mon_box + 1, e->mon_slot + 1);
    } else {
        SDL_snprintf(title, sizeof title, "Edit %s save", np_game_title(e->game));
    }
    np_ui_begin_page(app, &f, title);
    float y0 = f.list_y;
    draw_tabs(app, e, &f, y0);
    float list_y = y0 + 1.8f * f.lh;
    float bottom = f.panel.y + f.panel.h - 3.2f * f.lh;
    int rows = (int)((bottom - list_y) / f.lh);
    if (rows < 1)
        rows = 1;
    build_rows(e);
    if (e->tab == TAB_BOXES && !e->mon_open)
        draw_boxes(app, e, &f, list_y, bottom - list_y);
    else
        draw_rows(app, e, &f, list_y, rows);
    draw_footer(app, e, &f);
    if (!e->have_names)
        np_ui_text_clip(app, f.panel.x + 2 * f.cw, bottom - 0.2f * f.lh, f.s,
                        "Names unavailable: import this game's ROM to see them.", f.cols, warn);
    switch (e->ov) {
    case OV_NUMBER: draw_number(app, e, &f); break;
    case OV_CHOOSER: draw_chooser(app, e, &f); break;
    case OV_MENU: draw_menu(app, e, &f); break;
    case OV_DISCARD: draw_discard(app, e, &f); break;
    default: break;
    }
}
