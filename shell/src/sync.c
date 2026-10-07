/*
 * Folder sync of save slots (Options > Sync folder): a folder the player
 * picks, typically inside iCloud Drive, Dropbox or a network share, mirrors
 * saves/<game>/<slot>.sav as <folder>/<game>/<slot>.sav. Cartridges are
 * never copied. Slots sync when the app starts, when it quits, after every
 * in-game or editor save, and on *Sync now*.
 *
 * Decisions are sync_plan.c's three-way rules against the content of the
 * last sync, remembered in sync-state.txt with each file's size and
 * modification time, so unchanged files are not re-read. When both copies
 * changed, this device's copy stays the slot (and is pushed), the folder's
 * copy becomes a new local slot "<slot> (conflict <date>)" (and is pushed
 * too, so no device loses either), and the conflict page asks the player
 * which one to keep.
 *
 * All file work is synchronous on the main thread: a slot is 512 KiB, so a
 * full pass over a few dozen slots is a few milliseconds plus the folder's
 * own latency.
 */
#include "app.h"

#include "romdb.h"
#include "save4/save4.h"
#include "sha1.h"
#include "sync_plan.h"

#define MAX_RECORDS (NP_GAME_COUNT * NP_MAX_SLOTS * 2)

typedef struct np_sync_db {
    int loaded;
    int count;
    np_sync_record rec[MAX_RECORDS];
} np_sync_db;

static np_sync_db db;

/* ---- state file ------------------------------------------------------------ */

static void state_path(char *out, size_t n) { np_storage_path(out, n, "sync-state.txt"); }

static const char *const *game_ids(void)
{
    static const char *ids[NP_GAME_COUNT];
    for (int g = 0; g < NP_GAME_COUNT; g++)
        ids[g] = np_game_id((np_game)g);
    return ids;
}

static void db_load(void)
{
    if (db.loaded)
        return;
    db.loaded = 1;
    db.count = 0;
    char path[1100];
    state_path(path, sizeof path);
    size_t len;
    char *text = SDL_LoadFile(path, &len);
    if (!text)
        return;
    for (char *line = text; *line && db.count < MAX_RECORDS;) {
        char *end = SDL_strchr(line, '\n');
        if (line[0] != '#' && !np_sync_record_parse(line, game_ids(), NP_GAME_COUNT, &db.rec[db.count]))
            db.count++;
        if (!end)
            break;
        line = end + 1;
    }
    SDL_free(text);
}

static void db_save(void)
{
    size_t cap = 64 + (size_t)db.count * 200;
    char *buf = SDL_malloc(cap);
    if (!buf)
        return;
    size_t len = (size_t)SDL_snprintf(buf, cap, "# nativeplat folder sync: per slot, the content both sides had at "
                                                 "the last sync.\n");
    for (int i = 0; i < db.count; i++) {
        int n = np_sync_record_format(&db.rec[i], game_ids(), buf + len, cap - len);
        if (n > 0 && (size_t)n < cap - len)
            len += (size_t)n;
    }
    char path[1100];
    state_path(path, sizeof path);
    if (np_storage_write_atomic(path, buf, len, 0))
        SDL_Log("sync: cannot write %s: %s", path, SDL_GetError());
    SDL_free(buf);
}

static np_sync_record *db_find(int game, const char *slot)
{
    for (int i = 0; i < db.count; i++)
        if (db.rec[i].game == game && np_slot_name_eq(db.rec[i].slot, slot))
            return &db.rec[i];
    return NULL;
}

static np_sync_record *db_get(int game, const char *slot)
{
    np_sync_record *r = db_find(game, slot);
    if (r || db.count == MAX_RECORDS)
        return r;
    r = &db.rec[db.count++];
    SDL_zerop(r);
    r->game = game;
    SDL_strlcpy(r->slot, slot, sizeof r->slot);
    return r;
}

static void db_remove(int game, const char *slot)
{
    np_sync_record *r = db_find(game, slot);
    if (r)
        *r = db.rec[--db.count];
}

/* ---- files ---------------------------------------------------------------- */

static void remote_dir(const np_app *app, np_game game, char *out, size_t n)
{
    SDL_snprintf(out, n, "%s/%s", app->opt.sync_folder, np_game_id(game));
}

static void remote_path(const np_app *app, np_game game, const char *slot, char *out, size_t n)
{
    SDL_snprintf(out, n, "%s/%s/%s.sav", app->opt.sync_folder, np_game_id(game), slot);
}

typedef struct file_state {
    int exists;
    uint64_t size;
    int64_t mtime;
} file_state;

static file_state stat_file(const char *path)
{
    SDL_PathInfo info;
    file_state f = {0, 0, 0};
    if (SDL_GetPathInfo(path, &info) && info.type == SDL_PATHTYPE_FILE) {
        f.exists = 1;
        f.size = info.size;
        f.mtime = info.modify_time;
    }
    return f;
}

static void sha1_of(const void *data, size_t len, uint8_t out[20])
{
    np_sha1 s;
    np_sha1_init(&s);
    np_sha1_update(&s, data, len);
    np_sha1_final(&s, out);
}

/* One side of a slot: its state and, when it holds a save, its hash. The
 * file is read only if it changed since the recorded sync. */
static int side_of(const char *path, const file_state *f, const uint8_t *base, uint64_t rec_size, int64_t rec_mtime,
                   np_sync_side *side)
{
    if (!f->exists) {
        side->kind = NP_SYNC_ABSENT;
        return 0;
    }
    if (!f->size) {
        side->kind = NP_SYNC_EMPTY;
        return 0;
    }
    side->kind = NP_SYNC_DATA;
    if (base && f->size == rec_size && f->mtime == rec_mtime) {
        SDL_memcpy(side->hash, base, 20);
        return 0;
    }
    size_t len;
    void *data = SDL_LoadFile(path, &len);
    if (!data)
        return -1;
    sha1_of(data, len, side->hash);
    SDL_free(data);
    return 0;
}

/* Copies src to dst (atomically); keeps dst's old contents as .bak when
 * asked. Returns 0. */
static int copy_file(const char *src, const char *dst, int keep_backup)
{
    size_t len;
    void *data = SDL_LoadFile(src, &len);
    if (!data)
        return -1;
    int r = np_storage_write_atomic(dst, data, len, keep_backup);
    SDL_free(data);
    return r;
}

static int running_slot(const np_app *app, np_game game, const char *slot)
{
    return app->core && app->game == game && np_slot_name_eq(app->slot, slot);
}

typedef struct sync_tally {
    int pushed, pulled, conflicts, errors;
} sync_tally;

static void record_synced(np_sync_record *r, const uint8_t *hash, const char *local, const char *remote)
{
    if (!r)
        return;
    file_state l = stat_file(local), m = stat_file(remote);
    SDL_memcpy(r->base, hash, 20);
    r->local_size = l.size;
    r->local_mtime = l.mtime;
    r->remote_size = m.size;
    r->remote_mtime = m.mtime;
}

static void sync_one(np_app *app, np_game game, const char *slot, const char *const *taken, int ntaken, sync_tally *t);

/* The folder's copy becomes a new local slot; this device's copy stays. */
static void make_conflict(np_app *app, np_game game, const char *slot, const char *local, const char *remote,
                          const np_sync_side *ls, const char *const *taken, int ntaken, sync_tally *t)
{
    SDL_Time now;
    SDL_DateTime dt = {0};
    if (SDL_GetCurrentTime(&now))
        SDL_TimeToDateTime(now, &dt, true);
    char copy[NP_SLOT_NAME_MAX + 1], copy_path[1100];
    if (np_sync_conflict_name(slot, dt.year, dt.month, dt.day, taken, ntaken, copy)) {
        t->errors++;
        return;
    }
    np_storage_slot_path(game, copy, copy_path, sizeof copy_path);
    if (copy_file(remote, copy_path, 0) || copy_file(local, remote, 0)) {
        SDL_Log("sync: conflict copy for %s/%s failed: %s", np_game_id(game), slot, SDL_GetError());
        t->errors++;
        return;
    }
    np_sync_record *r = db_get(game, slot);
    record_synced(r, ls->hash, local, remote);
    if (r)
        SDL_strlcpy(r->conflict, copy, sizeof r->conflict);
    t->conflicts++;
    SDL_Log("sync: %s/%s changed on both sides; the folder's copy is now \"%s\"", np_game_id(game), slot, copy);
    sync_one(app, game, copy, NULL, 0, t); /* pushes the copy */
}

static void sync_one(np_app *app, np_game game, const char *slot, const char *const *taken, int ntaken, sync_tally *t)
{
    char local[1100], remote[1100];
    np_storage_slot_path(game, slot, local, sizeof local);
    remote_path(app, game, slot, remote, sizeof remote);
    file_state lf = stat_file(local), rf = stat_file(remote);
    np_sync_record *r = db_find(game, slot);
    np_sync_side ls, rs;
    if (side_of(local, &lf, r ? r->base : NULL, r ? r->local_size : 0, r ? r->local_mtime : 0, &ls) ||
        side_of(remote, &rf, r ? r->base : NULL, r ? r->remote_size : 0, r ? r->remote_mtime : 0, &rs)) {
        SDL_Log("sync: cannot read %s/%s: %s", np_game_id(game), slot, SDL_GetError());
        t->errors++;
        return;
    }
    np_sync_action a = np_sync_decide(&ls, &rs, r ? r->base : NULL);
    switch (a) {
    case NP_SYNC_SAME:
        if (ls.kind == NP_SYNC_DATA)
            record_synced(db_get(game, slot), ls.hash, local, remote);
        break;
    case NP_SYNC_PUSH:
        if (copy_file(local, remote, 0)) {
            t->errors++;
            break;
        }
        if (ls.kind == NP_SYNC_DATA)
            record_synced(db_get(game, slot), ls.hash, local, remote);
        t->pushed++;
        SDL_Log("sync: %s/%s -> folder", np_game_id(game), slot);
        break;
    case NP_SYNC_PULL:
        /* A running game would overwrite it with its next save, and keeps
         * playing the old one: the pull waits for the game to close. */
        if (running_slot(app, game, slot))
            break;
        if (copy_file(remote, local, ls.kind == NP_SYNC_DATA)) {
            t->errors++;
            break;
        }
        if (rs.kind == NP_SYNC_DATA)
            record_synced(db_get(game, slot), rs.hash, local, remote);
        t->pulled++;
        SDL_Log("sync: folder -> %s/%s", np_game_id(game), slot);
        break;
    case NP_SYNC_CONFLICT: make_conflict(app, game, slot, local, remote, &ls, taken, ntaken, t); break;
    }
}

/* Every slot of `game` on either side (or just `only`). */
static void sync_game(np_app *app, np_game game, const char *only, sync_tally *t)
{
    char dir[1100];
    remote_dir(app, game, dir, sizeof dir);
    if (!SDL_CreateDirectory(dir)) {
        SDL_Log("sync: cannot create %s: %s", dir, SDL_GetError());
        t->errors++;
        return;
    }
    np_slot_list *local = SDL_malloc(sizeof *local);
    if (!local)
        return;
    np_storage_list_slots(game, local);
    int nremote = 0;
    char **remote = SDL_GlobDirectory(dir, "*.sav", SDL_GLOB_CASEINSENSITIVE, &nremote);
    /* Names on either side; `taken` keeps conflict copies unique. */
    const char *names[NP_MAX_SLOTS * 2];
    int n = 0;
    for (int i = 0; i < local->count; i++)
        names[n++] = local->slot[i].name;
    static char remote_names[NP_MAX_SLOTS][NP_SLOT_NAME_MAX + 1];
    int nr = 0;
    for (int i = 0; remote && i < nremote && nr < NP_MAX_SLOTS; i++) {
        size_t len = SDL_strlen(remote[i]);
        if (len <= 4 || len - 4 > NP_SLOT_NAME_MAX)
            continue;
        char name[NP_SLOT_NAME_MAX + 1];
        SDL_memcpy(name, remote[i], len - 4);
        name[len - 4] = '\0';
        if (np_slot_name_problem(name) || np_slot_list_find(local, name) >= 0)
            continue;
        SDL_strlcpy(remote_names[nr], name, sizeof remote_names[nr]);
        names[n++] = remote_names[nr++];
    }
    for (int i = 0; i < n; i++)
        if (!only || np_slot_name_eq(only, names[i]))
            sync_one(app, game, names[i], names, n, t);
    SDL_free(remote);
    SDL_free(local);
}

static int sync_enabled(const np_app *app)
{
    /* Autotests sync only on their own portable storage. */
    return app->opt.sync_folder[0] && (!app->autotest.active || app->autotest.storage);
}

static void report(np_app *app, const sync_tally *t, int quiet)
{
    if (t->errors)
        SDL_snprintf(app->sync_status, sizeof app->sync_status, "%d error%s (see log)", t->errors,
                     t->errors == 1 ? "" : "s");
    else
        SDL_snprintf(app->sync_status, sizeof app->sync_status, "OK: %d sent, %d received%s", t->pushed, t->pulled,
                     t->conflicts ? ", conflicts" : "");
    if (t->conflicts) {
        if (app->view == NP_VIEW_GAME)
            np_app_toast(app, "Sync conflict: choose in Options > Sync status");
        else
            np_app_open_page(app, NP_PAGE_SYNC);
    } else if (!quiet || t->errors || t->pulled) {
        np_app_toast(app, "Sync: %s", app->sync_status);
    }
}

void np_sync_all(np_app *app, int quiet)
{
    if (!sync_enabled(app))
        return;
    db_load();
    sync_tally t = {0};
    for (int g = 0; g < NP_GAME_COUNT; g++)
        if (np_game_known((np_game)g))
            sync_game(app, (np_game)g, NULL, &t);
    db_save();
    if (t.pulled)
        np_app_refresh_slots(app, NULL);
    report(app, &t, quiet);
    SDL_Log("sync: all slots, %d sent, %d received, %d conflicts, %d errors", t.pushed, t.pulled, t.conflicts,
            t.errors);
}

void np_sync_slot(np_app *app, np_game game, const char *slot)
{
    if (!sync_enabled(app))
        return;
    db_load();
    sync_tally t = {0};
    sync_game(app, game, slot, &t);
    db_save();
    if (t.errors || t.conflicts)
        report(app, &t, 1);
    else
        SDL_snprintf(app->sync_status, sizeof app->sync_status, "OK: \"%s\" sent", slot);
}

void np_sync_forget(np_app *app, np_game game, const char *slot)
{
    if (!sync_enabled(app))
        return;
    db_load();
    np_sync_record *r = db_find(game, slot);
    char remote[1100];
    remote_path(app, game, slot, remote, sizeof remote);
    file_state f = stat_file(remote);
    /* Only a copy this device synced and nobody changed since is removed;
     * anything newer in the folder comes back as a slot on the next sync. */
    if (r && f.exists && f.size == r->remote_size && f.mtime == r->remote_mtime) {
        if (SDL_RemovePath(remote))
            SDL_Log("sync: removed %s/%s from the folder", np_game_id(game), slot);
        else
            SDL_Log("sync: cannot remove %s: %s", remote, SDL_GetError());
    }
    db_remove(game, slot);
    db_save();
}

int np_sync_conflicts(const np_app *app)
{
    if (!sync_enabled(app))
        return 0;
    db_load();
    int n = 0;
    for (int i = 0; i < db.count; i++)
        n += db.rec[i].conflict[0] != 0;
    return n;
}

/* ---- conflict page ---------------------------------------------------------- */

enum { CH_KEEP_LOCAL, CH_USE_OTHER, CH_KEEP_BOTH, CH_LATER, CH_COUNT };

static np_sync_record *first_conflict(void)
{
    db_load();
    for (int i = 0; i < db.count; i++)
        if (db.rec[i].conflict[0])
            return &db.rec[i];
    return NULL;
}

/* "NAME, 12:34 played, 3 badges, saved 2026-10-04 21:40" for a slot. */
static void describe(np_game game, const char *slot, char *out, size_t n)
{
    char path[1100];
    np_storage_slot_path(game, slot, path, sizeof path);
    size_t len;
    uint8_t *data = SDL_LoadFile(path, &len);
    save4 s;
    save4_trainer t;
    char when[40] = "";
    file_state f = stat_file(path);
    SDL_DateTime dt;
    if (f.exists && SDL_TimeToDateTime(f.mtime, &dt, true))
        SDL_snprintf(when, sizeof when, "saved %04d-%02d-%02d %02d:%02d", dt.year, dt.month, dt.day, dt.hour,
                     dt.minute);
    if (data && save4_load(&s, data, len) == SAVE4_OK) {
        if (save4_get_trainer(&s, &t) == SAVE4_OK) {
            int badges = 0;
            for (int b = 0; b < 8; b++)
                badges += t.badges >> b & 1;
            SDL_snprintf(out, n, "%s, %u:%02u played, %d badge%s, %s", t.name, t.play_hours, t.play_minutes, badges,
                         badges == 1 ? "" : "s", when);
        } else {
            SDL_snprintf(out, n, "%s", when);
        }
        save4_free(&s);
    } else {
        SDL_snprintf(out, n, "unreadable save, %s", when);
    }
    SDL_free(data);
}

static void resolve(np_app *app, int choice)
{
    np_sync_record *r = first_conflict();
    if (!r || choice == CH_LATER) {
        np_app_open_page(app, NP_PAGE_NONE);
        return;
    }
    np_game game = (np_game)r->game;
    char slot[NP_SLOT_NAME_MAX + 1], copy[NP_SLOT_NAME_MAX + 1];
    SDL_strlcpy(slot, r->slot, sizeof slot);
    SDL_strlcpy(copy, r->conflict, sizeof copy);
    if (choice == CH_USE_OTHER) {
        if (running_slot(app, game, slot) || running_slot(app, game, copy)) {
            np_app_toast(app, "Close the game first");
            return;
        }
        char from[1100], to[1100];
        np_storage_slot_path(game, copy, from, sizeof from);
        np_storage_slot_path(game, slot, to, sizeof to);
        if (copy_file(from, to, 1)) {
            np_app_toast(app, "Could not replace \"%s\": %s", slot, SDL_GetError());
            return;
        }
    }
    r->conflict[0] = '\0';
    if (choice != CH_KEEP_BOTH) {
        np_storage_slot_delete(game, copy);
        np_sync_forget(app, game, copy);
    }
    db_save();
    np_sync_slot(app, game, slot);
    np_app_refresh_slots(app, NULL);
    np_app_toast(app, choice == CH_KEEP_BOTH ? "Kept both as separate slots" : "Kept one copy of \"%s\"", slot);
    app->sel = 0;
    if (!first_conflict())
        np_app_open_page(app, NP_PAGE_NONE);
}

void np_sync_draw(np_app *app)
{
    static const SDL_Color white = {235, 235, 235, 255}, dim = {150, 150, 160, 255}, accent = {255, 205, 80, 255};
    np_page_frame f;
    np_ui_begin_page(app, &f, "Sync conflict");
    np_sync_record *r = first_conflict();
    if (!r) {
        np_ui_text(app, f.panel.x + 2 * f.cw, f.list_y, f.s, "No conflicts.", white);
        np_ui_end_page(app, &f, "Esc/B: back", 0);
        return;
    }
    float x = f.panel.x + 2 * f.cw, y = f.list_y;
    char text[320], a[160], b[160];
    SDL_snprintf(text, sizeof text,
                 "\"%s\" (%s) changed on this device and in the sync folder since they last matched. Both copies "
                 "are kept until you choose:",
                 r->slot, np_game_title((np_game)r->game));
    y += (float)np_ui_text_wrap(app, x, y, f.s, f.lh, f.cols - 4, text, white, 1) * f.lh + f.lh * 0.5f;
    describe((np_game)r->game, r->slot, a, sizeof a);
    describe((np_game)r->game, r->conflict, b, sizeof b);
    np_ui_text(app, x, y, f.s, "This device:", accent);
    y += f.lh;
    y += (float)np_ui_text_wrap(app, x + 2 * f.cw, y, f.s, f.lh, f.cols - 6, a, white, 1) * f.lh;
    SDL_snprintf(text, sizeof text, "Other copy (now slot \"%s\"):", r->conflict);
    np_ui_text_clip(app, x, y, f.s, text, f.cols - 4, accent);
    y += f.lh;
    y += (float)np_ui_text_wrap(app, x + 2 * f.cw, y, f.s, f.lh, f.cols - 6, b, white, 1) * f.lh + f.lh * 0.5f;
    static const char *const labels[CH_COUNT] = {"Keep this device's", "Use the other copy", "Keep both",
                                                 "Decide later"};
    app->sel = SDL_clamp(app->sel, 0, CH_COUNT - 1);
    float bw = 22 * f.cw, bh = 1.6f * f.lh;
    for (int i = 0; i < CH_COUNT; i++) {
        float bx = x + (float)(i % 2) * (bw + 2 * f.cw), by = y + (float)(i / 2) * (bh + 0.5f * f.lh);
        np_ui_button(app, (SDL_FRect){bx, by, bw, bh}, labels[i], app->sel == i, i, f.s);
    }
    int more = np_sync_conflicts(app) - 1;
    if (more > 0) {
        SDL_snprintf(text, sizeof text, "%d more after this one", more);
        np_ui_text(app, x, y + 2 * (bh + 0.5f * f.lh) + 0.25f * f.lh, f.s, text, dim);
    }
    np_ui_end_page(app, &f, "Arrows: choose  Enter/A: confirm  Esc/B: later", 0);
}

/* Back and Close are the generic page commands (ui.c). */
void np_sync_command(np_app *app, np_menu_cmd cmd)
{
    switch (cmd) {
    case NP_CMD_LEFT:
    case NP_CMD_RIGHT: app->sel ^= 1; break;
    case NP_CMD_UP:
    case NP_CMD_DOWN: app->sel ^= 2; break;
    case NP_CMD_CONFIRM: resolve(app, app->sel); break;
    default: break;
    }
}

void np_sync_hit(np_app *app, int id)
{
    if (id >= 0 && id < CH_COUNT)
        resolve(app, id);
}
