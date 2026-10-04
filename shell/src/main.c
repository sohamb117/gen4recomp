/*
 * nativeplat shell entry point. Uses SDL3's main callbacks (SDL_AppInit /
 * SDL_AppIterate / SDL_AppEvent / SDL_AppQuit) instead of a main loop:
 * iOS owns the run loop, and the same structure works unchanged on macOS
 * and Windows.
 *
 * Frame pacing: the guest is a DS, which refreshes at 33513982 / (6 * 355 *
 * 263) = 59.8261 Hz. Each iterate adds the elapsed wall time (times the
 * speed multiplier) to an accumulator and runs as many guest frames as fit,
 * independently of how often the display presents (VSync, FPS cap). The
 * "logic clock" option runs the guest at exactly 60 Hz instead so a 60 Hz
 * display shows every frame exactly once. Uncapped speed runs frames for a
 * fixed slice of each iterate.
 *
 * Launching: `--game/--slot/--launcher` on the command line and
 * nativeplat://launch?game=..&slot=.. links (which SDL delivers as
 * SDL_EVENT_DROP_FILE on macOS and iOS) become an np_launch request; see
 * np_app_launch. Anything invalid lands on the launcher with a message.
 */
#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL_main.h>

#include <math.h>

#include "app.h"
#include "card.h"
#include "fx.h"
#include "net.h"
#include "png.h"
#include "romdb.h"
#include "storage.h"

#define DS_FRAME_HZ (33513982.0 / (6.0 * 355.0 * 263.0))
#define MAX_DT_NS (100 * SDL_NS_PER_MS)
#define UNCAPPED_SLICE_NS (12 * SDL_NS_PER_MS)

/* ---- host callbacks for the core --------------------------------------- */

static int host_rom_read(void *user, uint32_t offset, void *dst, uint32_t len)
{
    np_app *app = user;
    if ((uint64_t)offset + len > app->host.rom_size)
        return -1;
    if (SDL_SeekIO(app->rom_io, offset, SDL_IO_SEEK_SET) < 0)
        return -1;
    return SDL_ReadIO(app->rom_io, dst, len) == len ? 0 : -1;
}

static int host_save_load(void *user, void *dst, uint32_t len)
{
    np_app *app = user;
    return np_storage_save_load(app->game, app->slot, dst, len);
}

static int host_save_store(void *user, const void *src, uint32_t len)
{
    np_app *app = user;
    int r = np_storage_save_store(app->game, app->slot, src, len);
    if (r)
        np_app_toast(app, "Saving failed: %s", SDL_GetError());
    else
        np_sync_slot(app, app->game, app->slot);
    return r;
}

/* Days from 1970-01-01 to a proleptic Gregorian date (H. Hinnant). */
static int64_t days_from_civil(int64_t y, unsigned m, unsigned d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

/* The device's local wall clock, as seconds since 2000-01-01 00:00:00. */
static int64_t host_rtc_now(void *user)
{
    (void)user;
    SDL_Time now;
    SDL_DateTime dt;
    if (!SDL_GetCurrentTime(&now) || !SDL_TimeToDateTime(now, &dt, true))
        return 0;
    int64_t days = days_from_civil(dt.year, (unsigned)dt.month, (unsigned)dt.day) - days_from_civil(2000, 1, 1);
    return days * 86400 + dt.hour * 3600 + dt.minute * 60 + dt.second;
}

static void host_log(void *user, const char *line)
{
    (void)user;
    SDL_Log("core: %s", line);
}

/* Local wireless. The station id is offered even with wireless off: the
 * guest derives the console's MAC address from it and the game stores that
 * in the save, so it must not change between sessions. */
static uint32_t host_net_self(void *user) { return ((np_app *)user)->opt.station_id; }

static int host_net_send(void *user, uint32_t peer, const void *buf, uint32_t len)
{
    np_app *app = user;
    return app->net ? np_net_send(app->net, peer, buf, len) : 0;
}

static int host_net_recv(void *user, uint32_t *peer, void *buf, uint32_t cap)
{
    np_app *app = user;
    return app->net ? np_net_recv(app->net, peer, buf, cap) : 0;
}

static void net_log(void *user, const char *line)
{
    (void)user;
    SDL_Log("net: %s", line);
}

void np_app_net_apply(np_app *app)
{
    if (app->net)
        np_net_close(app->net);
    app->net = NULL;
    app->net_error[0] = '\0';
    if (!app->opt.lan_enabled)
        return;
    /* A relay replaces LAN discovery with a room on an internet server. */
    np_net_config cfg = {.port = (uint16_t)app->opt.lan_port,
                         .station_id = app->opt.station_id,
                         .lan_discovery = !app->opt.lan_relay[0],
                         .relay = app->opt.lan_relay[0] ? app->opt.lan_relay : NULL,
                         .pin = app->opt.lan_pin,
                         .log = net_log};
    app->net = np_net_open(&cfg, app->net_error, sizeof app->net_error);
    if (!app->net) {
        SDL_Log("local wireless: %s", app->net_error);
        return;
    }
    if (app->opt.lan_peer[0] && np_net_add_peer(app->net, app->opt.lan_peer, app->net_error, sizeof app->net_error))
        SDL_Log("local wireless peer %s: %s", app->opt.lan_peer, app->net_error);
    SDL_Log("local wireless on, port %u, station %06X", np_net_port(app->net), (unsigned)app->opt.station_id);
}

int np_app_net_peers(const np_app *app) { return app->net ? np_net_peer_count(app->net) : -1; }

/* While a wireless session runs the partner expects real time. */
static int link_active(const np_app *app) { return app->core && np_core_status(app->core, NP_STAT_LINK_ACTIVE); }

/* Autotest variants: a synthetic cartridge header, and a backup chip in
 * memory unless the test runs on (portable) storage. */
static int test_rom_read(void *user, uint32_t offset, void *dst, uint32_t len)
{
    np_app *app = user;
    if ((uint64_t)offset + len > sizeof app->autotest.header)
        return -1;
    SDL_memcpy(dst, app->autotest.header + offset, len);
    return 0;
}

static int test_save_load(void *user, void *dst, uint32_t len)
{
    np_app *app = user;
    np_autotest *t = &app->autotest;
    if (t->storage) {
        int r = np_storage_save_load(app->game, app->slot, dst, len);
        t->loads += r == 1;
        return r;
    }
    if (!t->save)
        return 0;
    SDL_memcpy(dst, t->save, SDL_min(len, t->save_len));
    if (len > t->save_len)
        SDL_memset((uint8_t *)dst + t->save_len, 0xFF, len - t->save_len);
    t->loads++;
    return 1;
}

static int test_save_store(void *user, const void *src, uint32_t len)
{
    np_app *app = user;
    np_autotest *t = &app->autotest;
    if (t->storage) {
        int r = np_storage_save_store(app->game, app->slot, src, len);
        if (!r) {
            t->saves++;
            t->save_len = len;
            np_sync_slot(app, app->game, app->slot);
        }
        return r;
    }
    uint8_t *p = SDL_realloc(t->save, len);
    if (!p)
        return -1;
    SDL_memcpy(p, src, len);
    t->save = p;
    t->save_len = len;
    t->saves++;
    return 0;
}

/* ---- shell services used by the other modules -------------------------- */

void np_app_toast(np_app *app, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    SDL_vsnprintf(app->toast, sizeof app->toast, fmt, ap);
    va_end(ap);
    app->toast_until = SDL_GetTicksNS() + 3 * SDL_NS_PER_SECOND;
}

int np_app_speed(const np_app *app)
{
    if (link_active(app))
        return 1;
    int ff = app->ff_hold || app->ff_toggle;
    return np_speeds[ff ? app->opt.ff_speed_index : app->opt.speed_index];
}

static void save_options(np_app *app)
{
    /* Autotests only persist settings on their own (portable) storage. */
    if (!app->options_dirty || (app->autotest.active && !app->autotest.storage))
        return;
    if (np_options_save(&app->opt, app->options_path))
        SDL_Log("could not write %s: %s", app->options_path, SDL_GetError());
    app->options_dirty = 0;
}

void np_app_apply_video_options(np_app *app)
{
    const np_options *o = &app->opt;
    np_present p;
    np_fx_resolve(app, &p);
    if (!app->autotest.active)
        SDL_SetRenderVSync(app->renderer, p.vsync ? 1 : SDL_RENDERER_VSYNC_DISABLED);
    char rate[16];
    int cap = p.fps_cap;
    /* Without VSync or a cap, menus would spin a core at 100%: idle at 60. */
    if (!cap && !p.vsync && app->view != NP_VIEW_GAME)
        cap = 60;
    SDL_snprintf(rate, sizeof rate, "%d", app->autotest.active ? 0 : cap);
    SDL_SetHint(SDL_HINT_MAIN_CALLBACK_RATE, rate);
    int is_full = (SDL_GetWindowFlags(app->window) & SDL_WINDOW_FULLSCREEN) != 0;
    if (is_full != o->fullscreen)
        SDL_SetWindowFullscreen(app->window, o->fullscreen);
    for (int i = 0; i < 2; i++)
        SDL_SetTextureScaleMode(app->screen_tex[i], o->linear_filter ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST);
}

void np_app_open_page(np_app *app, np_page page)
{
    if (page == NP_PAGE_NONE)
        save_options(app);
    if (page == NP_PAGE_TEXT && app->page != NP_PAGE_TEXT)
        SDL_StartTextInput(app->window); /* also raises the iOS keyboard */
    else if (page != NP_PAGE_TEXT && app->page == NP_PAGE_TEXT)
        SDL_StopTextInput(app->window);
    app->page_parent = (page == NP_PAGE_CONTROLS || page == NP_PAGE_ABOUT) ? app->page : NP_PAGE_NONE;
    app->page = page;
    app->sel = app->col = app->scroll = 0;
    app->capture = 0;
    np_input_release_all(app);
    /* Do not "catch up" on the time spent in a menu. */
    app->accum_ns = 0;
    app->last_ns = SDL_GetTicksNS();
}

/* ---- save slots ---------------------------------------------------------- */

void np_app_refresh_slots(np_app *app, const char *select)
{
    char keep[NP_SLOT_NAME_MAX + 1] = "";
    if (select)
        SDL_strlcpy(keep, select, sizeof keep);
    if (np_storage_list_slots(app->slots_game, &app->slots)) {
        np_app_toast(app, "Cannot list save slots: %s", SDL_GetError());
        app->slots.count = 0;
    }
    if (keep[0]) {
        int i = np_slot_list_find(&app->slots, keep);
        if (i >= 0)
            app->slot_sel = i;
    }
    if (app->slot_sel >= app->slots.count)
        app->slot_sel = app->slots.count - 1;
    if (app->slot_sel < 0)
        app->slot_sel = 0;
}

void np_app_open_slots(np_app *app, np_game game)
{
    app->slots_game = game;
    app->slot_sel = 0;
    np_app_refresh_slots(app, app->opt.last_slot[game]);
    np_app_open_page(app, NP_PAGE_SLOTS);
}

/* ---- GBA slot (Pal Park) ---------------------------------------------------- */

/* The save file of the inserted cartridge: the one picked, else the ROM's
 * name with .sav (what mGBA and most emulators use). */
static void gba_save_path(const np_app *app, char *out, size_t n)
{
    if (app->opt.gba_save[0]) {
        SDL_strlcpy(out, app->opt.gba_save, n);
        return;
    }
    SDL_strlcpy(out, app->opt.gba_rom, n);
    char *dot = SDL_strrchr(out, '.'), *slash = SDL_strrchr(out, '/');
    if (dot && (!slash || dot > slash))
        *dot = '\0';
    SDL_strlcat(out, ".sav", n);
}

static int host_gba_rom_read(void *user, uint32_t offset, void *dst, uint32_t len)
{
    np_app *app = user;
    if (SDL_SeekIO(app->gba_io, offset, SDL_IO_SEEK_SET) < 0)
        return -1;
    return SDL_ReadIO(app->gba_io, dst, len) == len ? 0 : -1;
}

/* Fills the chip image; a missing file is an erased chip, a short one is
 * padded with 0xFF (an erased flash reads 0xFF). */
static int host_gba_save_load(void *user, void *dst, uint32_t len)
{
    np_app *app = user;
    char path[1100];
    gba_save_path(app, path, sizeof path);
    size_t size;
    void *data = SDL_LoadFile(path, &size);
    if (!data)
        return np_storage_exists(path) ? -1 : 0;
    SDL_memset(dst, 0xFF, len);
    SDL_memcpy(dst, data, size < len ? size : len);
    SDL_free(data);
    return 1;
}

/* The player's own GBA save: replaced atomically, previous kept as .bak. */
static int host_gba_save_store(void *user, const void *src, uint32_t len)
{
    np_app *app = user;
    char path[1100];
    gba_save_path(app, path, sizeof path);
    int r = np_storage_write_atomic(path, src, len, 1);
    if (r)
        np_app_toast(app, "Saving the GBA cartridge failed: %s", SDL_GetError());
    else
        SDL_Log("GBA save written: %s", path);
    return r;
}

static void eject_gba(np_app *app)
{
    if (app->gba_io)
        SDL_CloseIO(app->gba_io);
    app->gba_io = NULL;
    app->host.gba_rom_size = 0;
    app->host.gba_rom_read = NULL;
    app->host.gba_save_load = NULL;
    app->host.gba_save_store = NULL;
}

/* Puts the chosen cartridge in the slot for the core about to boot (only
 * Platinum's core has the GBA slot). A file that is not a GBA ROM leaves the
 * slot empty. */
static void insert_gba(np_app *app, np_game game)
{
    eject_gba(app);
    if (game != NP_GAME_PLATINUM || !app->opt.gba_rom[0])
        return;
    app->gba_io = SDL_IOFromFile(app->opt.gba_rom, "rb");
    Sint64 size = app->gba_io ? SDL_GetIOSize(app->gba_io) : -1;
    uint8_t header[0xC0];
    const char *why = NULL;
    if (size < (Sint64)sizeof header)
        why = "cannot read it";
    else if (size > (Sint64)NP_GBA_ROM_MAX)
        why = "larger than 32 MB";
    else if (SDL_ReadIO(app->gba_io, header, sizeof header) != sizeof header || header[0xB2] != 0x96)
        why = "not a GBA cartridge image";
    if (why) {
        np_app_toast(app, "GBA cartridge %s: %s", app->opt.gba_rom, why);
        SDL_Log("GBA slot left empty: %s: %s", app->opt.gba_rom, why);
        eject_gba(app);
        return;
    }
    app->host.gba_rom_size = (uint32_t)size;
    app->host.gba_rom_read = host_gba_rom_read;
    app->host.gba_save_load = host_gba_save_load;
    app->host.gba_save_store = host_gba_save_store;
    char game_title[13] = {0};
    SDL_memcpy(game_title, header + 0xA0, 12);
    SDL_Log("GBA slot: %s (%s, %lld bytes)", app->opt.gba_rom, game_title, (long long)size);
}

int np_app_gba_inserted(const np_app *app) { return app->gba_io != NULL; }

/* ---- game sessions ----------------------------------------------------- */

static int open_core(np_app *app, np_game game, const char *slot, const np_host *host)
{
    app->game = game;
    SDL_strlcpy(app->slot, slot, sizeof app->slot);
    app->host = *host;
    /* Runtime content packages, read by the core at boot (mods.c). */
    app->host.content_root =
        np_mods_content_root(app, game, app->mods_root, sizeof app->mods_root) ? NULL : app->mods_root;
    insert_gba(app, game);
    /* PC_* variables configure the port layer (debug switches such as
     * PC_TP_DEBUG); pass the process's own through, as np_headless does. */
    char **env = SDL_GetEnvironmentVariables(SDL_GetEnvironment());
    const char *options[33];
    int nopt = 0;
    for (char **e = env; e && *e && nopt < 32; e++)
        if (!SDL_strncmp(*e, "PC_", 3))
            options[nopt++] = *e;
    options[nopt] = NULL;
    app->core = np_core_create(game, &app->host, options);
    SDL_free(env);
    if (!app->core) {
        SDL_snprintf(app->status, sizeof app->status, "Could not start %s: %s", np_game_title(game),
                     np_core_create_error());
        SDL_Log("%s", app->status);
        eject_gba(app);
        return -1;
    }
    app->view = NP_VIEW_GAME;
    np_app_open_page(app, NP_PAGE_NONE);
    app->have_frame = 0;
    app->ff_toggle = app->ff_hold = 0;
    char title[96];
    SDL_snprintf(title, sizeof title, "nativeplat - Pokemon %s - %s", np_game_title(game), slot);
    SDL_SetWindowTitle(app->window, title);
    np_app_apply_video_options(app);
    np_session_begin(app);
    return 0;
}

static void close_core(np_app *app)
{
    if (!app->core)
        return;
    np_session_end(app);
    if (np_core_save_flush(app->core))
        SDL_Log("save flush failed: %s", np_core_last_error(app->core));
    np_core_destroy(app->core);
    app->core = NULL;
    if (app->rom_io)
        SDL_CloseIO(app->rom_io);
    app->rom_io = NULL;
    eject_gba(app);
}

/* Opens a cartridge file as the host's ROM. */
static int open_rom(np_app *app, const char *path, np_host *host)
{
    app->rom_io = SDL_IOFromFile(path, "rb");
    Sint64 size = app->rom_io ? SDL_GetIOSize(app->rom_io) : -1;
    if (size <= 0 || size > (Sint64)UINT32_MAX) {
        SDL_snprintf(app->status, sizeof app->status, "Cannot read %s: %s", path, SDL_GetError());
        if (app->rom_io)
            SDL_CloseIO(app->rom_io);
        app->rom_io = NULL;
        return -1;
    }
    *host = (np_host){.user = app,
                      .rom_size = (uint32_t)size,
                      .rom_read = host_rom_read,
                      .save_load = host_save_load,
                      .save_store = host_save_store,
                      .rtc_now = app->opt.real_clock ? host_rtc_now : NULL,
                      .log = host_log,
                      .net_self = app->opt.station_id ? host_net_self : NULL,
                      .net_send = app->opt.station_id ? host_net_send : NULL,
                      .net_recv = app->opt.station_id ? host_net_recv : NULL};
    return 0;
}

int np_app_start_game(np_app *app, np_game game, const char *slot)
{
    close_core(app);
    if (!np_core_available(game)) {
        SDL_snprintf(app->status, sizeof app->status, "The %s core is not included in this build.",
                     np_game_title(game));
        return -1;
    }
    char path[1100];
    np_storage_rom_path(game, path, sizeof path);
    np_host host;
    if (open_rom(app, path, &host))
        return -1;
    if (open_core(app, game, slot, &host)) {
        SDL_CloseIO(app->rom_io);
        app->rom_io = NULL;
        return -1;
    }
    SDL_Log("started %s, save slot \"%s\"", np_game_id(game), slot);
    app->status[0] = '\0';
    app->opt.last_game = game;
    SDL_strlcpy(app->opt.last_slot[game], slot, sizeof app->opt.last_slot[game]);
    app->options_dirty = 1;
    save_options(app);
    return 0;
}

int np_app_continue(np_app *app, np_game game)
{
    app->slots_game = game;
    np_app_refresh_slots(app, NULL);
    const np_slot_list *l = &app->slots;
    /* The last used slot, else the most recently written one. */
    int pick = np_slot_list_find(l, app->opt.last_slot[game]);
    if (pick < 0 && l->count) {
        pick = 0;
        for (int i = 1; i < l->count; i++)
            if (l->slot[i].mtime > l->slot[pick].mtime)
                pick = i;
    }
    char name[NP_SLOT_NAME_MAX + 1];
    if (pick >= 0) {
        SDL_strlcpy(name, l->slot[pick].name, sizeof name);
    } else {
        np_slot_default_name(NULL, 0, name);
        if (np_storage_slot_create(game, name)) {
            SDL_snprintf(app->status, sizeof app->status, "Cannot create a save slot: %s", SDL_GetError());
            return -1;
        }
    }
    return np_app_start_game(app, game, name);
}

void np_app_stop_game(np_app *app)
{
    close_core(app);
    app->view = NP_VIEW_LAUNCHER;
    app->have_frame = 0;
    app->launcher_sel = app->game;
    SDL_SetWindowTitle(app->window, "nativeplat");
    np_input_release_all(app);
    np_app_apply_video_options(app);
}

int np_app_reload_game(np_app *app)
{
    if (!app->core)
        return -1;
    /* Keep the ROM stream and host (autotests' in-memory chip included);
     * only the machine restarts, from the save the host holds. */
    if (np_core_save_flush(app->core))
        SDL_Log("save flush failed: %s", np_core_last_error(app->core));
    np_session_end(app);
    np_core_destroy(app->core);
    app->core = NULL;
    np_host host = app->host;
    char slot[NP_SLOT_NAME_MAX + 1];
    SDL_strlcpy(slot, app->slot, sizeof slot);
    if (open_core(app, app->game, slot, &host)) {
        np_app_stop_game(app);
        return -1;
    }
    SDL_Log("reloaded %s, save slot \"%s\"", np_game_id(app->game), slot);
    np_app_toast(app, "Reloaded the last save");
    return 0;
}

/* Lands on the launcher showing `fmt`. */
static void launch_fail(np_app *app, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    SDL_vsnprintf(app->status, sizeof app->status, fmt, ap);
    va_end(ap);
    SDL_Log("launch: %s", app->status);
    if (app->view == NP_VIEW_GAME)
        np_app_stop_game(app);
    np_app_open_page(app, NP_PAGE_NONE);
}

void np_app_launch(np_app *app, const np_launch *req)
{
    if (req->force_launcher || req->game < 0) {
        if (req->force_launcher && app->view == NP_VIEW_GAME)
            np_app_stop_game(app);
        if (app->view == NP_VIEW_LAUNCHER)
            np_app_open_page(app, NP_PAGE_NONE);
        return;
    }
    np_game game = (np_game)req->game;
    app->launcher_sel = game;
    if (!np_core_available(game)) {
        launch_fail(app, "The %s core is not included in this build.", np_game_title(game));
        return;
    }
    if (!np_storage_rom_present(game)) {
        launch_fail(app, "Import your %s cartridge first.", np_game_title(game));
        return;
    }
    if (!req->slot[0]) {
        if (app->view == NP_VIEW_GAME)
            np_app_stop_game(app);
        if (np_app_continue(app, game))
            launch_fail(app, "%s", app->status);
        return;
    }
    app->slots_game = game;
    np_app_refresh_slots(app, NULL);
    int idx = np_slot_list_find(&app->slots, req->slot);
    int number = np_launch_slot_number(req->slot);
    if (idx < 0 && number >= 1 && number <= app->slots.count)
        idx = number - 1; /* the slot list is sorted by name, as shown */
    if (idx < 0) {
        launch_fail(app, "%s has no save slot \"%s\".", np_game_title(game), req->slot);
        return;
    }
    char name[NP_SLOT_NAME_MAX + 1];
    SDL_strlcpy(name, app->slots.slot[idx].name, sizeof name);
    if (np_app_start_game(app, game, name))
        launch_fail(app, "%s", app->status);
}

/* ---- dialogs, drops and other deferred work ------------------------------- */

void np_app_request(np_app *app, np_pending_kind kind, const char *path)
{
    SDL_LockMutex(app->pending_lock);
    app->pending = kind;
    app->pending_shown = 0;
    SDL_strlcpy(app->pending_path, path, sizeof app->pending_path);
    SDL_UnlockMutex(app->pending_lock);
}

/* May run on another thread: only hands the result over. */
static void SDLCALL dialog_done(void *user, const char *const *files, int filter)
{
    (void)filter;
    np_app *app = user;
    if (!files) {
        char msg[256];
        SDL_snprintf(msg, sizeof msg, "The file picker is unavailable: %s", SDL_GetError());
        np_app_request(app, NP_PENDING_MESSAGE, msg);
        return;
    }
    if (files[0])
        np_app_request(app, app->dialog_kind, files[0]);
}

/* Autotests never show native dialogs (they would block on a person); the
 * script's "dialog:" step answers instead. */
static int autotest_dialog(np_app *app, const char *what)
{
    if (!app->autotest.active)
        return 0;
    SDL_Log("autotest: %s dialog opened", what);
    return 1;
}

void np_app_open_rom_dialog(np_app *app)
{
    static const SDL_DialogFileFilter filters[] = {{"Nintendo DS ROM (*.nds)", "nds"}};
    app->dialog_kind = NP_PENDING_ROM;
    if (!autotest_dialog(app, "ROM import"))
        SDL_ShowOpenFileDialog(dialog_done, app, app->window, filters, 1, NULL, false);
}

void np_app_open_sav_import_dialog(np_app *app, np_game game)
{
    static const SDL_DialogFileFilter filters[] = {{"Save file (*.sav, *.dsv)", "sav;dsv"}};
    app->dialog_kind = NP_PENDING_SAV_IMPORT;
    app->pending_game = game;
    if (!autotest_dialog(app, "save import"))
        SDL_ShowOpenFileDialog(dialog_done, app, app->window, filters, 1, NULL, false);
}

void np_app_open_gift_import_dialog(np_app *app)
{
    static const SDL_DialogFileFilter filters[] = {{"Mystery Gift (*.pgt, *.pcd)", "pgt;pcd"}};
    app->dialog_kind = NP_PENDING_GIFT_IMPORT;
    if (!autotest_dialog(app, "gift import"))
        SDL_ShowOpenFileDialog(dialog_done, app, app->window, filters, 1, NULL, false);
}

void np_app_open_card_export_dialog(np_app *app, int kind)
{
    static const SDL_DialogFileFilter filters[] = {{"PNG image (*.png)", "png"}};
    app->dialog_kind = NP_PENDING_CARD_EXPORT;
    app->pending_card = kind;
    char name[64];
    SDL_snprintf(name, sizeof name, "%s.png", kind == NP_CARD_DIPLOMA ? "Pokedex Diploma" : "Trainer Card");
    if (!autotest_dialog(app, "card export"))
        SDL_ShowSaveFileDialog(dialog_done, app, app->window, filters, 1, name);
}

void np_app_open_sync_folder_dialog(np_app *app)
{
    app->dialog_kind = NP_PENDING_SYNC_FOLDER;
    if (!autotest_dialog(app, "sync folder"))
        SDL_ShowOpenFolderDialog(dialog_done, app, app->window, app->opt.sync_folder[0] ? app->opt.sync_folder : NULL,
                                 false);
}

void np_app_open_mod_install_dialog(np_app *app)
{
    static const SDL_DialogFileFilter filters[] = {{"Mod package (*.zip)", "zip"}};
    app->dialog_kind = NP_PENDING_MOD_INSTALL;
    if (!autotest_dialog(app, "mod install"))
        SDL_ShowOpenFileDialog(dialog_done, app, app->window, filters, 1, NULL, false);
}

void np_app_open_gba_dialog(np_app *app, int save)
{
    static const SDL_DialogFileFilter rom[] = {{"GBA cartridge (*.gba)", "gba"}};
    static const SDL_DialogFileFilter sav[] = {{"GBA save (*.sav)", "sav"}};
    app->dialog_kind = save ? NP_PENDING_GBA_SAVE : NP_PENDING_GBA_ROM;
    if (!autotest_dialog(app, save ? "GBA save" : "GBA cartridge"))
        SDL_ShowOpenFileDialog(dialog_done, app, app->window, save ? sav : rom, 1, NULL, false);
}

void np_app_open_sav_export_dialog(np_app *app, np_game game, const char *slot)
{
    static const SDL_DialogFileFilter filters[] = {{"Raw save (*.sav)", "sav"}};
    app->dialog_kind = NP_PENDING_SAV_EXPORT;
    app->pending_game = game;
    SDL_strlcpy(app->pending_slot, slot, sizeof app->pending_slot);
    char name[96];
    SDL_snprintf(name, sizeof name, "%s - %s.sav", np_game_title(game), slot);
    if (!autotest_dialog(app, "save export"))
        SDL_ShowSaveFileDialog(dialog_done, app, app->window, filters, 1, name);
}

static const char *base_name(const char *path)
{
    const char *base = path;
    for (const char *p = path; *p; p++)
        if (*p == '/' || *p == '\\')
            base = p + 1;
    return base;
}

static void import_rom(np_app *app, const char *path)
{
    np_import_result r;
    np_storage_import_rom(path, &r);
    if (r.ok && !np_core_available(r.game))
        SDL_snprintf(app->status, sizeof app->status, "%s The %s core is not included in this build yet.", r.message,
                     np_game_title(r.game));
    else
        SDL_strlcpy(app->status, r.message, sizeof app->status);
    SDL_Log("import %s: %s", path, app->status);
    if (r.ok)
        app->launcher_sel = r.game;
}

/* A new cartridge or save takes effect when the core next boots: the
 * console is off whenever cartridges are swapped. */
static void gba_changed(np_app *app)
{
    app->options_dirty = 1;
    save_options(app);
    char save[1100];
    gba_save_path(app, save, sizeof save);
    if (!app->opt.gba_rom[0])
        np_app_toast(app, "GBA slot empty%s", app->core ? " from the next boot" : "");
    else
        np_app_toast(app, "GBA: %s, save %s%s", base_name(app->opt.gba_rom), base_name(save),
                     app->core ? " (from the next boot: F2 twice)" : "");
}

/* Two-step for ROMs so "Verifying" is on screen while 128 MiB is hashed. */
static void process_pending(np_app *app)
{
    char path[1024];
    SDL_LockMutex(app->pending_lock);
    np_pending_kind kind = app->pending;
    if (kind == NP_PENDING_ROM && !app->pending_shown && app->view == NP_VIEW_LAUNCHER) {
        SDL_snprintf(app->status, sizeof app->status, "Verifying %s ...", base_name(app->pending_path));
        app->pending_shown = 1;
        kind = NP_PENDING_NONE;
    } else {
        app->pending = NP_PENDING_NONE;
    }
    SDL_strlcpy(path, app->pending_path, sizeof path);
    SDL_UnlockMutex(app->pending_lock);

    char err[320];
    switch (kind) {
    case NP_PENDING_ROM:
        if (app->view == NP_VIEW_GAME)
            np_app_toast(app, "Quit to the launcher to import a ROM");
        else
            import_rom(app, path);
        break;
    case NP_PENDING_SAV_IMPORT: {
        char name[NP_SLOT_NAME_MAX + 1];
        if (np_storage_slot_import(app->pending_game, path, name, err, sizeof err)) {
            np_app_toast(app, "%s", err);
            SDL_strlcpy(app->status, err, sizeof app->status);
        } else {
            np_app_toast(app, "Imported %s as slot \"%s\"", base_name(path), name);
            if (app->slots_game == app->pending_game)
                np_app_refresh_slots(app, name);
        }
        SDL_Log("save import %s: %s", path, app->toast);
        break;
    }
    case NP_PENDING_SAV_EXPORT:
        if (np_storage_slot_export(app->pending_game, app->pending_slot, path, err, sizeof err))
            np_app_toast(app, "%s", err);
        else
            np_app_toast(app, "Exported \"%s\" to %s", app->pending_slot, base_name(path));
        SDL_Log("save export %s: %s", path, app->toast);
        break;
    case NP_PENDING_CARD_EXPORT:
        if (app->editor)
            np_editor_export_card(app, app->pending_card, path);
        break;
    case NP_PENDING_SYNC_FOLDER:
        SDL_strlcpy(app->opt.sync_folder, path, sizeof app->opt.sync_folder);
        app->options_dirty = 1;
        save_options(app);
        SDL_Log("sync folder: %s", path);
        np_sync_all(app, 0);
        break;
    case NP_PENDING_MOD_INSTALL:
        np_mods_install(app, path);
        break;
    case NP_PENDING_GBA_ROM:
        SDL_strlcpy(app->opt.gba_rom, path, sizeof app->opt.gba_rom);
        app->opt.gba_save[0] = '\0'; /* its own <name>.sav until another is picked */
        gba_changed(app);
        break;
    case NP_PENDING_GBA_SAVE:
        SDL_strlcpy(app->opt.gba_save, path, sizeof app->opt.gba_save);
        gba_changed(app);
        break;
    case NP_PENDING_GIFT_IMPORT:
        if (app->editor)
            np_editor_import_gift(app, path);
        break;
    case NP_PENDING_MESSAGE:
        SDL_strlcpy(app->status, path, sizeof app->status);
        np_app_toast(app, "%s", path);
        break;
    default: break;
    }
}

static int has_extension(const char *path, const char *ext)
{
    const char *dot = SDL_strrchr(path, '.');
    return dot && !SDL_strcasecmp(dot + 1, ext);
}

static int slot_pages_open(const np_app *app)
{
    return app->page == NP_PAGE_SLOTS || app->page == NP_PAGE_SLOT_MENU;
}

/* A file dropped on the window or the Dock icon, or an opened link. */
static void handle_drop(np_app *app, const char *data)
{
    if (np_launch_is_url(data)) {
        np_launch req;
        char err[160];
        SDL_Log("link: %s", data);
        if (np_launch_parse_url(data, &req, err, sizeof err))
            launch_fail(app, "Cannot open link: %s", err);
        else
            np_app_launch(app, &req);
        return;
    }
    if (has_extension(data, "zip") && app->page == NP_PAGE_MODS) {
        np_app_request(app, NP_PENDING_MOD_INSTALL, data);
        return;
    }
    if (has_extension(data, "pgt") || has_extension(data, "pcd")) {
        if (app->page != NP_PAGE_EDITOR) {
            np_app_toast(app, "Open a save in the editor, then drop the gift there");
            return;
        }
        np_app_request(app, NP_PENDING_GIFT_IMPORT, data);
        return;
    }
    if (has_extension(data, "sav") || has_extension(data, "dsv")) {
        if (!slot_pages_open(app)) {
            np_app_toast(app, "Open a game's save slots, then drop the save there");
            return;
        }
        app->pending_game = app->slots_game;
        np_app_request(app, NP_PENDING_SAV_IMPORT, data);
        return;
    }
    np_app_request(app, NP_PENDING_ROM, data);
}

/* ---- screenshots ----------------------------------------------------------- */

static int io_sink(void *user, const void *data, size_t len)
{
    return SDL_WriteIO((SDL_IOStream *)user, data, len) == len ? 0 : -1;
}

static int write_png(const char *path, const uint32_t *px, uint32_t w, uint32_t h, size_t stride)
{
    SDL_IOStream *io = SDL_IOFromFile(path, "wb");
    if (!io)
        return -1;
    int r = np_png_encode(io_sink, io, px, w, h, stride);
    if (!SDL_CloseIO(io))
        r = -1;
    return r;
}

/* F12: both screens at native resolution, top above bottom. */
static void take_screenshot(np_app *app)
{
    if (!app->have_frame)
        return;
    uint32_t w = app->frame.width, h = app->frame.height;
    uint32_t *buf = SDL_malloc((size_t)w * h * 2 * 4);
    if (!buf)
        return;
    for (int s = 0; s < 2; s++)
        for (uint32_t y = 0; y < h; y++)
            SDL_memcpy(buf + ((size_t)s * h + y) * w, app->frame.screen[s] + (size_t)y * app->frame.stride, w * 4);
    SDL_Time now;
    SDL_DateTime dt = {0};
    if (SDL_GetCurrentTime(&now))
        SDL_TimeToDateTime(now, &dt, true);
    char rel[128], path[1200];
    for (int n = 0; n < 100; n++) {
        char suffix[8] = "";
        if (n)
            SDL_snprintf(suffix, sizeof suffix, "-%d", n);
        SDL_snprintf(rel, sizeof rel, "screenshots/%s-%04d%02d%02d-%02d%02d%02d%s.png", np_game_id(app->game),
                     dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second, suffix);
        np_storage_path(path, sizeof path, rel);
        if (!np_storage_exists(path))
            break;
    }
    if (write_png(path, buf, w, h * 2, w))
        np_app_toast(app, "Screenshot failed: %s", SDL_GetError());
    else
        np_app_toast(app, "Saved %s", rel);
    SDL_free(buf);
}

/* The whole window as presented, for the autotest. */
static int capture_window(np_app *app, const char *path)
{
    SDL_Surface *shot = SDL_RenderReadPixels(app->renderer, NULL);
    if (!shot)
        return -1;
    SDL_Surface *rgb = SDL_ConvertSurface(shot, SDL_PIXELFORMAT_XRGB8888);
    SDL_DestroySurface(shot);
    if (!rgb)
        return -1;
    int r = write_png(path, rgb->pixels, (uint32_t)rgb->w, (uint32_t)rgb->h, (size_t)rgb->pitch / 4);
    SDL_DestroySurface(rgb);
    return r;
}

/* ---- running and drawing the game ----------------------------------------- */

static void upload_frame(np_app *app) { np_fx_upload(app); }

static int run_one(np_app *app, const np_input *in)
{
    if (app->net)
        np_net_poll(app->net);
    int r = np_core_run_frame(app->core, in, &app->frame);
    if (r == 0) {
        app->have_frame = 1;
        if (!app->rewind_hold)
            np_session_frame_done(app);
        return 0;
    }
    char error[256] = "";
    if (r > 0) {
        SDL_strlcpy(app->status, "The game has exited.", sizeof app->status);
    } else {
        SDL_strlcpy(error, np_core_last_error(app->core), sizeof error);
        SDL_snprintf(app->status, sizeof app->status, "The game stopped: %s", error);
    }
    SDL_Log("%s", app->status);
    np_app_stop_game(app);
    np_mods_boot_failed(app, error); /* a broken package: offer to turn it off */
    return -1;
}

static void run_game(np_app *app)
{
    uint64_t now = SDL_GetTicksNS();
    uint64_t dt = now - app->last_ns;
    app->last_ns = now;
    if (dt > MAX_DT_NS)
        dt = MAX_DT_NS; /* after a stall, resume instead of fast-forwarding */
    /* Menus pause the game; minimizing or backgrounding does too, except
     * during a wireless session, which the partner drops after 4 s. */
    if (app->page != NP_PAGE_NONE || ((app->backgrounded || app->minimized) && !link_active(app))) {
        if (app->net)
            np_net_poll(app->net); /* keep answering discovery while paused */
        return;
    }

    np_input in = {0};
    in.keys = np_input_poll_keys(app, &app->ff_hold);
    np_input_stylus(app, &in);
    if (app->rewind_hold) {
        /* Steps back through the history (session.c paces it), each
         * restored point shown by running one frame from it with no input;
         * that frame's audio is dropped. */
        app->accum_ns = 0;
        if (np_session_rewind_step(app))
            return;
        np_input none = {0};
        if (run_one(app, &none))
            return;
        upload_frame(app);
        int16_t drop[1024 * 2];
        while (np_core_audio_read(app->core, drop, 1024) > 0) {
        }
        return;
    }
    int speed = np_app_speed(app);
    int ran = 0;
    if (speed == 0) {
        do {
            if (run_one(app, &in))
                return;
            ran++;
        } while (SDL_GetTicksNS() - now < UNCAPPED_SLICE_NS);
        app->accum_ns = 0;
    } else {
        double period = 1e9 / (app->opt.logic_clock_60 ? 60.0 : DS_FRAME_HZ);
        app->accum_ns += (double)dt * speed;
        int n = (int)(app->accum_ns / period);
        app->accum_ns -= n * period;
        if (n > speed * 4) { /* too slow to keep up: drop time, not frames */
            n = speed * 4;
            app->accum_ns = 0;
        }
        for (; ran < n; ran++)
            if (run_one(app, &in))
                return;
    }
    if (ran)
        upload_frame(app);
    np_audio_pump(app, speed);
}

static void draw_screens(np_app *app)
{
    /* Each DS pixel is an s x s block, s = height / 192; widescreen frames
     * are wider than 256 blocks with the DS picture centred. */
    uint32_t s = app->have_frame && app->frame.height >= 192 ? app->frame.height / 192 : 1;
    int screen_w = app->have_frame ? (int)(app->frame.width / s) : 256;
    np_layout_params lp = {app->opt.layout, app->opt.swap, app->opt.rotation, app->opt.scale, screen_w};
    np_layout_compute(&app->layout, &lp, app->out_w, app->out_h);
    for (int i = 0; i < 2; i++)
        np_fx_draw_screen(app, i);
}

static void draw(np_app *app)
{
    SDL_SetRenderDrawColor(app->renderer, 0, 0, 0, 255);
    SDL_RenderClear(app->renderer);
    if (app->view == NP_VIEW_GAME) {
        draw_screens(app);
        np_touchpad_draw(app);
    }
    np_ui_draw(app);
}

static void update_audio_state(np_app *app)
{
    int want = app->view == NP_VIEW_GAME && app->page == NP_PAGE_NONE && !app->backgrounded && !app->minimized;
    if (want != app->audio_running) {
        np_audio_set_paused(app, !want);
        app->audio_running = want;
    }
}

/* ---- autotest ---------------------------------------------------------------- */

static uint16_t parse_keys(const char *v)
{
    static const char *const names[12] = {"a", "b", "select", "start", "right", "left",
                                          "up", "down", "r", "l", "x", "y"};
    char buf[128];
    SDL_strlcpy(buf, v, sizeof buf);
    uint16_t keys = 0;
    char *save = NULL;
    for (char *t = SDL_strtok_r(buf, "+", &save); t; t = SDL_strtok_r(NULL, "+", &save))
        for (int i = 0; i < 12; i++)
            if (!SDL_strcasecmp(t, names[i]))
                keys |= (uint16_t)(1u << i);
    return keys;
}

/*
 * press= schedule, parsed once. Steps are separated by ';' or newlines (in a
 * file given as press=@path, where '#' starts a comment):
 *   F:keys[:N[:R:C]]   hold keys (a+up, start, ...) for N frames (default 6)
 *                      from frame F; with R and C, repeat every R frames,
 *                      C times in all
 *   F:tap:X:Y[:N[:R:C]] touch the bottom screen at X,Y, held and repeated
 *                      like keys
 *   F:none             no input; only a time marker for "+D" steps
 * F may be "+D": D frames after the previous step's first frame, which keeps
 * long input scripts editable.
 */
static int autotest_parse_press(np_autotest *t, const char *spec)
{
    char *text = NULL;
    if (spec[0] == '@') {
        size_t len;
        text = SDL_LoadFile(spec + 1, &len);
        if (!text) {
            SDL_Log("autotest: cannot read %s: %s", spec + 1, SDL_GetError());
            return -1;
        }
    } else {
        text = SDL_strdup(spec);
    }
    int prev = 0, line = 0;
    char *save = NULL;
    for (char *l = SDL_strtok_r(text, "\n", &save); l; l = SDL_strtok_r(NULL, "\n", &save)) {
        line++;
        char *hash = SDL_strchr(l, '#');
        if (hash)
            *hash = '\0';
        char *save2 = NULL;
        for (char *step = SDL_strtok_r(l, ";", &save2); step; step = SDL_strtok_r(NULL, ";", &save2)) {
            while (*step == ' ' || *step == '\t' || *step == '\r')
                step++;
            if (!*step)
                continue;
            if (t->npress == NP_AUTOTEST_MAX_PRESS) {
                SDL_Log("autotest: press schedule too long");
                SDL_free(text);
                return -1;
            }
            np_press *p = &t->presses[t->npress];
            *p = (np_press){.n = 6, .count = 1};
            int rel = *step == '+', frame = 0;
            char k[64];
            int v[5] = {0};
            int got = SDL_sscanf(step + rel, "%d:%63[a-zA-Z+]:%d:%d:%d:%d:%d", &frame, k, &v[0], &v[1], &v[2],
                                 &v[3], &v[4]) - 2;
            if (got < 0) {
                SDL_Log("autotest: bad press step \"%s\" (line %d)", step, line);
                SDL_free(text);
                return -1;
            }
            p->frame = rel ? prev + frame : frame;
            prev = p->frame;
            /* Fields after the keys: N R C, or for a tap X Y N R C. */
            int *rest = v;
            if (!SDL_strcasecmp(k, "tap")) {
                if (got < 2 || v[0] < 0 || v[0] > 255 || v[1] < 0 || v[1] > 191) {
                    SDL_Log("autotest: bad tap \"%s\" (line %d)", step, line);
                    SDL_free(text);
                    return -1;
                }
                p->tap = 1;
                p->x = (uint16_t)v[0];
                p->y = (uint16_t)v[1];
                rest += 2;
                got -= 2;
            } else {
                p->keys = parse_keys(k);
                if (!p->keys && SDL_strcasecmp(k, "none")) {
                    SDL_Log("autotest: unknown keys \"%s\" (line %d)", k, line);
                    SDL_free(text);
                    return -1;
                }
            }
            if (got >= 1 && rest[0] >= 1)
                p->n = rest[0];
            if (got >= 3) {
                p->every = rest[1];
                p->count = rest[2];
            }
            t->npress++;
        }
    }
    SDL_free(text);
    return 0;
}

/* Keys (and the stylus) the schedule holds at the current frame. */
static uint16_t autotest_pressed(const np_autotest *t, np_input *in)
{
    uint16_t keys = 0;
    for (int i = 0; i < t->npress; i++) {
        const np_press *p = &t->presses[i];
        int off = t->ran - p->frame;
        if (off < 0)
            continue;
        if (p->every > 0) {
            if (off / p->every >= p->count)
                continue;
            off %= p->every;
        }
        if (off >= p->n)
            continue;
        if (p->tap) {
            in->touch = 1;
            in->touch_x = p->x;
            in->touch_y = p->y;
        } else {
            keys |= p->keys;
        }
    }
    return keys;
}

/*
 * NP_AUTOTEST="frames=120,png=/tmp/shot.png[,game=platinum][,layout=vertical|
 *   horizontal|hybrid|top|bottom][,rotation=0..3][,swap=1][,scale=integer]
 *   [,filter=linear][,touch=XxY][,keys=a+up][,controls=1][,size=WxH]
 *   [,page=launcher|options|controls|about]
 *   [,rom=/path/cart.nds (boot this cartridge instead of a synthetic header)]
 *   [,boot=app (start like the real app: launch options, launcher)]
 *   [,storage=1 (saves and options in the user-data root; portable mode
 *    only, so a test never touches a player's saves; boot=app implies it)]
 *   [,import=/path/to/rom.nds (run the importer first, repeatable)]
 *   [,press=SCHEDULE or press=@file (DS keys and taps, see autotest_parse_press)]
 *   [,shots=N (also write <png minus .png>-<iteration>.png every N iterations)]
 *   [,clock=real (device RTC; default fixed, so runs are deterministic)]
 *   [,slot=NAME (save slot for rom=/synthetic boots with storage=1)]
 *   [,script=F:kind:args;... (synthetic events, see autotest_script)]"
 * Returns -1 on a malformed value.
 */
static int parse_autotest(np_app *app, const char *spec, int *game, int *win_w, int *win_h)
{
    static const char *const layouts[NP_LAYOUT_COUNT] = {"vertical", "horizontal", "hybrid", "top", "bottom"};
    static char buf[8192];
    SDL_strlcpy(buf, spec, sizeof buf);
    np_autotest *t = &app->autotest;
    t->frames = 120;
    app->opt.real_clock = 0;
    SDL_strlcpy(t->slot, "Autotest", sizeof t->slot);
    *game = NP_GAME_PLATINUM;
    char *save = NULL;
    for (char *kv = SDL_strtok_r(buf, ",", &save); kv; kv = SDL_strtok_r(NULL, ",", &save)) {
        char *v = SDL_strchr(kv, '=');
        if (!v)
            v = SDL_strchr(kv, ':'); /* also accept frames:120 */
        if (!v)
            return -1;
        *v++ = '\0';
        if (!SDL_strcmp(kv, "frames"))
            t->frames = SDL_atoi(v);
        else if (!SDL_strcmp(kv, "png"))
            SDL_strlcpy(t->png, v, sizeof t->png);
        else if (!SDL_strcmp(kv, "game")) {
            if ((*game = np_launch_game_from_name(v)) < 0)
                return -1;
        } else if (!SDL_strcmp(kv, "layout")) {
            int m = -1;
            for (int i = 0; i < NP_LAYOUT_COUNT; i++)
                if (!SDL_strcasecmp(v, layouts[i]))
                    m = i;
            if (m < 0)
                return -1;
            app->opt.layout = (np_layout_mode)m;
        } else if (!SDL_strcmp(kv, "rotation"))
            app->opt.rotation = SDL_atoi(v) & 3;
        else if (!SDL_strcmp(kv, "swap"))
            app->opt.swap = SDL_atoi(v) != 0;
        else if (!SDL_strcmp(kv, "scale"))
            app->opt.scale = !SDL_strcmp(v, "integer") ? NP_SCALE_INTEGER : NP_SCALE_FIT;
        else if (!SDL_strcmp(kv, "filter"))
            app->opt.linear_filter = !SDL_strcmp(v, "linear");
        else if (!SDL_strcmp(kv, "fx1") || !SDL_strcmp(kv, "fx2")) {
            /* fx1=crt or fx1=crt:80 (intensity) */
            int slot = kv[2] - '1', m = -1;
            char *colon = SDL_strchr(v, ':');
            if (colon) {
                *colon = '\0';
                app->opt.fx_intensity[slot] = SDL_clamp(SDL_atoi(colon + 1), 0, 100);
            }
            for (int i = 0; i < NP_FX_COUNT; i++)
                if (!SDL_strcasecmp(v, np_fx_ids[i]))
                    m = i;
            if (m < 0)
                return -1;
            app->opt.fx[slot] = m;
        } else if (!SDL_strcmp(kv, "curvature"))
            app->opt.crt_curvature = SDL_atoi(v) != 0;
        else if (!SDL_strcmp(kv, "perf")) {
            int m = -1;
            for (int i = 0; i < NP_PERF_COUNT; i++)
                if (!SDL_strcasecmp(v, np_perf_ids[i]))
                    m = i;
            if (m < 0)
                return -1;
            app->opt.perf = m;
        } else if (!SDL_strcmp(kv, "lan")) {
            app->opt.lan_enabled = 1;
            app->opt.lan_port = SDL_clamp(SDL_atoi(v), 1024, 65531);
        } else if (!SDL_strcmp(kv, "peer"))
            SDL_strlcpy(app->opt.lan_peer, v, sizeof app->opt.lan_peer);
        else if (!SDL_strcmp(kv, "rewind")) {
            if (SDL_sscanf(v, "%d+%d", &t->rewind_from, &t->rewind_frames) != 2)
                return -1;
        } else if (!SDL_strcmp(kv, "station")) {
            app->opt.station_id = (uint32_t)SDL_strtoul(v, NULL, 16) & 0xFFFFFFu;
            if (!app->opt.station_id)
                return -1;
        } else if (!SDL_strcmp(kv, "render_scale"))
            app->opt.render_scale = SDL_clamp(SDL_atoi(v), 1, 4);
        else if (!SDL_strcmp(kv, "widescreen"))
            app->opt.widescreen = SDL_atoi(v) != 0;
        else if (!SDL_strcmp(kv, "zoom"))
            app->opt.camera_zoom = SDL_clamp(SDL_atoi(v), 64, 1024);
        else if (!SDL_strcmp(kv, "tilt"))
            app->opt.camera_tilt = SDL_clamp(SDL_atoi(v), -720, 720);
        else if (!SDL_strcmp(kv, "instant_text"))
            app->opt.text_instant = SDL_atoi(v) != 0;
        else if (!SDL_strcmp(kv, "fix_bugs"))
            app->opt.fix_bugs = SDL_atoi(v) != 0;
        else if (!SDL_strcmp(kv, "rewind_seconds"))
            app->opt.rewind_seconds = SDL_clamp(SDL_atoi(v), 0, 120);
        else if (!SDL_strcmp(kv, "sync"))
            SDL_strlcpy(t->sync_folder, v, sizeof t->sync_folder);
        else if (!SDL_strcmp(kv, "gba"))
            SDL_strlcpy(t->gba_rom, v, sizeof t->gba_rom);
        else if (!SDL_strcmp(kv, "gbasave"))
            SDL_strlcpy(t->gba_save, v, sizeof t->gba_save);
        else if (!SDL_strcmp(kv, "touch")) {
            int x, y;
            if (SDL_sscanf(v, "%dx%d", &x, &y) != 2 || x < 0 || x > 255 || y < 0 || y > 191)
                return -1;
            t->input.touch = 1;
            t->input.touch_x = (uint16_t)x;
            t->input.touch_y = (uint16_t)y;
        } else if (!SDL_strcmp(kv, "keys"))
            t->input.keys = parse_keys(v);
        else if (!SDL_strcmp(kv, "controls"))
            app->opt.touch_controls = SDL_atoi(v) ? NP_TOUCH_ON : NP_TOUCH_OFF;
        else if (!SDL_strcmp(kv, "storage"))
            t->storage = SDL_atoi(v) != 0;
        else if (!SDL_strcmp(kv, "script"))
            SDL_strlcpy(t->script, v, sizeof t->script);
        else if (!SDL_strcmp(kv, "press")) {
            if (autotest_parse_press(t, v))
                return -1;
        } else if (!SDL_strcmp(kv, "shots"))
            t->shot_every = SDL_atoi(v);
        else if (!SDL_strcmp(kv, "clock"))
            app->opt.real_clock = !SDL_strcmp(v, "real");
        else if (!SDL_strcmp(kv, "slot")) {
            if (np_slot_name_problem(v))
                return -1;
            SDL_strlcpy(t->slot, v, sizeof t->slot);
        } else if (!SDL_strcmp(kv, "rom")) {
            SDL_strlcpy(t->rom, v, sizeof t->rom);
            t->boot = NP_AT_ROM;
        } else if (!SDL_strcmp(kv, "boot")) {
            if (SDL_strcmp(v, "app"))
                return -1;
            t->boot = NP_AT_APP;
            t->storage = 1;
        } else if (!SDL_strcmp(kv, "import")) {
            if (t->imports[0])
                SDL_strlcat(t->imports, "\n", sizeof t->imports);
            SDL_strlcat(t->imports, v, sizeof t->imports);
            t->storage = 1;
        } else if (!SDL_strcmp(kv, "page")) {
            if (!SDL_strcmp(v, "launcher"))
                t->page = -1;
            else if (!SDL_strcmp(v, "options"))
                t->page = NP_PAGE_OPTIONS;
            else if (!SDL_strcmp(v, "controls"))
                t->page = NP_PAGE_CONTROLS;
            else if (!SDL_strcmp(v, "about"))
                t->page = NP_PAGE_ABOUT;
            else if (!SDL_strcmp(v, "mods"))
                t->page = NP_PAGE_MODS;
            else
                return -1;
        } else if (!SDL_strcmp(kv, "size")) {
            if (SDL_sscanf(v, "%dx%d", win_w, win_h) != 2 || *win_w < 64 || *win_h < 64)
                return -1;
        } else
            return -1;
    }
    return t->frames > 0 && t->png[0] ? 0 : -1;
}

static void fill_test_header(np_autotest *t, np_game game)
{
    static const char *const titles[NP_GAME_COUNT] = {"POKEMON D", "POKEMON P", "POKEMON PL"};
    static const char *const codes[NP_GAME_COUNT] = {"ADAE", "APAE", "CPUE"};
    SDL_memset(t->header, 0, sizeof t->header);
    SDL_memcpy(t->header, titles[game], SDL_strlen(titles[game]));
    SDL_memcpy(t->header + 0x0C, codes[game], 4);
    SDL_memcpy(t->header + 0x10, "01", 2);
}

/* storage=1: run the importer on each `import=`, and round-trip an options
 * file (written beside options.ini, never over it) through save and load. */
static int autotest_storage(np_app *app)
{
    np_autotest *t = &app->autotest;
    SDL_Log("autotest: user data root %s%s", np_storage_root(), np_storage_is_portable() ? " (portable)" : "");
    char list[sizeof t->imports];
    SDL_strlcpy(list, t->imports, sizeof list);
    char *save = NULL;
    for (char *p = SDL_strtok_r(list, "\n", &save); p; p = SDL_strtok_r(NULL, "\n", &save))
        import_rom(app, p);
    char path[1100];
    np_storage_path(path, sizeof path, "options-autotest.ini");
    np_options saved = app->opt, loaded;
    saved.layout = NP_LAYOUT_HYBRID;
    saved.rotation = 3;
    saved.swap = 1;
    saved.volume = 35;
    saved.fps_cap_index = 3;
    saved.speed_index = NP_SPEED_COUNT - 1;
    saved.touch_controls = NP_TOUCH_OFF;
    saved.real_clock = 0;
    saved.startup_continue = 1;
    saved.last_game = NP_GAME_PEARL;
    SDL_strlcpy(saved.last_slot[NP_GAME_PEARL], "My (2nd) run", sizeof saved.last_slot[0]);
    np_bind_key(&saved.bind, NP_ACT_L, 1, SDL_SCANCODE_LSHIFT); /* a name with a space */
    np_bind_pad(&saved.bind, NP_ACT_FF_HOLD, SDL_GAMEPAD_BUTTON_LEFT_STICK);
    if (np_options_save(&saved, path) || np_options_load(&loaded, path)) {
        SDL_Log("autotest: cannot write/read %s: %s", path, SDL_GetError());
        return -1;
    }
    if (SDL_memcmp(&saved, &loaded, sizeof saved)) {
        SDL_Log("autotest: FAILED options round-trip through %s", path);
        return -1;
    }
    SDL_Log("autotest: options round-trip OK (%s)", path);
    return 0;
}

/* The slot an autotest core saves to on storage (created when missing). */
static int autotest_slot(np_app *app, np_game game)
{
    if (!app->autotest.storage)
        return 0;
    np_slot_list *l = &app->slots;
    if (np_storage_list_slots(game, l))
        return -1;
    return np_slot_list_find(l, app->autotest.slot) >= 0 ? 0 : np_storage_slot_create(game, app->autotest.slot);
}

/* Synthetic: boot, run a frame, flush and destroy, then boot again so the
 * second session must load what the first one stored. Real cartridge: one
 * boot. */
static int autotest_boot(np_app *app, np_game game)
{
    np_autotest *t = &app->autotest;
    if (autotest_slot(app, game))
        return -1;
    np_host host;
    if (t->boot == NP_AT_ROM) {
        if (open_rom(app, t->rom, &host))
            return -1;
        host.save_load = test_save_load;
        host.save_store = test_save_store;
        return open_core(app, game, t->slot, &host);
    }
    fill_test_header(t, game);
    host = (np_host){.user = app,
                     .rom_size = sizeof t->header,
                     .rom_read = test_rom_read,
                     .save_load = test_save_load,
                     .save_store = test_save_store,
                     .log = host_log};
    np_input none = {0};
    if (open_core(app, game, t->slot, &host) || run_one(app, &none))
        return -1;
    np_core_save_flush(app->core);
    close_core(app);
    return open_core(app, game, t->slot, &host);
}

/*
 * script=F:kind:args;... pushes synthetic events through SDL_AppEvent before
 * frame F (window coordinates in points):
 *   F:key:<SDL scancode name>        press and release a key
 *   F:down:X:Y  F:move:X:Y  F:up:X:Y left mouse button
 *   F:fdown:X:Y F:fmove:X:Y F:fup:X:Y one finger
 *   F:text:<chars>                   typed text (SDL_EVENT_TEXT_INPUT)
 *   F:drop:<path or link>            a file dropped / a nativeplat: link opened
 *   F:dialog:<path>                  the player picks <path> in the open file dialog
 */
static void autotest_script(np_app *app)
{
    np_autotest *t = &app->autotest;
    char buf[sizeof t->script];
    SDL_strlcpy(buf, t->script, sizeof buf);
    int ww = 1, wh = 1;
    SDL_GetWindowSize(app->window, &ww, &wh);
    SDL_WindowID win = SDL_GetWindowID(app->window);
    char *save = NULL;
    for (char *step = SDL_strtok_r(buf, ";", &save); step; step = SDL_strtok_r(NULL, ";", &save)) {
        int frame;
        char kind[16], arg[1024] = "";
        float x = 0, y = 0;
        if (SDL_sscanf(step, "%d:%15[a-z]:%1023[^;]", &frame, kind, arg) < 2 || frame != t->ran)
            continue;
        SDL_sscanf(arg, "%f:%f", &x, &y);
        SDL_Event e;
        SDL_zero(e);
        if (!SDL_strcmp(kind, "key")) {
            SDL_Scancode sc = SDL_GetScancodeFromName(arg);
            e.type = SDL_EVENT_KEY_DOWN;
            e.key.windowID = win;
            e.key.scancode = sc;
            e.key.key = SDL_GetKeyFromScancode(sc, SDL_KMOD_NONE, false);
            e.key.down = true;
            SDL_PushEvent(&e);
            e.type = SDL_EVENT_KEY_UP;
            e.key.down = false;
        } else if (!SDL_strcmp(kind, "down") || !SDL_strcmp(kind, "up")) {
            e.type = kind[0] == 'd' ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
            e.button.windowID = win;
            e.button.button = SDL_BUTTON_LEFT;
            e.button.down = kind[0] == 'd';
            e.button.x = x;
            e.button.y = y;
        } else if (!SDL_strcmp(kind, "move")) {
            e.type = SDL_EVENT_MOUSE_MOTION;
            e.motion.windowID = win;
            e.motion.x = x;
            e.motion.y = y;
        } else if (kind[0] == 'f') {
            e.type = !SDL_strcmp(kind, "fdown") ? SDL_EVENT_FINGER_DOWN
                     : !SDL_strcmp(kind, "fup") ? SDL_EVENT_FINGER_UP
                                                : SDL_EVENT_FINGER_MOTION;
            e.tfinger.windowID = win;
            e.tfinger.touchID = 1;
            e.tfinger.fingerID = 1;
            e.tfinger.x = x / (float)ww;
            e.tfinger.y = y / (float)wh;
            e.tfinger.pressure = 1.0f;
        } else if (!SDL_strcmp(kind, "text") || !SDL_strcmp(kind, "drop")) {
            /* The event points at text that must outlive this function. */
            SDL_strlcpy(t->drop, arg, sizeof t->drop);
            if (kind[0] == 't') {
                e.type = SDL_EVENT_TEXT_INPUT;
                e.text.windowID = win;
                e.text.text = t->drop;
            } else {
                e.type = SDL_EVENT_DROP_FILE;
                e.drop.windowID = win;
                e.drop.data = t->drop;
            }
        } else if (!SDL_strcmp(kind, "dialog")) {
            const char *files[2] = {arg, NULL};
            dialog_done(app, files, 0);
            continue;
        } else {
            continue;
        }
        SDL_PushEvent(&e);
    }
}

/* One guest frame per iteration, with the fixed, pressed and scripted input. */
static int autotest_frame(np_app *app)
{
    np_autotest *t = &app->autotest;
    np_input in = t->input;
    in.keys |= autotest_pressed(t, &in);
    if (t->script[0]) {
        int ff;
        np_input live = {0};
        in.keys |= np_input_poll_keys(app, &ff);
        np_input_stylus(app, &live);
        if (live.touch) {
            in.touch = 1;
            in.touch_x = live.touch_x;
            in.touch_y = live.touch_y;
        }
    }
    if (t->rewind_frames && t->ran >= t->rewind_from && t->ran < t->rewind_from + t->rewind_frames)
        app->rewind_hold = 1;
    else if (!t->script[0])
        app->rewind_hold = 0;
    if (app->rewind_hold) {
        int r = np_session_rewind_step(app);
        if (r == 1)
            return 0; /* still showing the previous step */
        if (r == 0)
            in = (np_input){0}; /* the frame that shows the restored point */
        else
            app->rewind_hold = 0; /* history exhausted: play on */
    }
    if (run_one(app, &in))
        return -1;
    if (t->rewind_frames && (t->ran == t->rewind_from - 1 || t->ran == t->rewind_from + t->rewind_frames))
        SDL_Log("autotest: frame %d rewind depth %d, %zu KB of history", t->ran, np_session_rewind_depth(app),
                np_session_rewind_bytes(app) / 1024);
    upload_frame(app);
    int16_t buf[2048 * 2];
    size_t got;
    while ((got = np_core_audio_read(app->core, buf, 2048)) > 0) {
        t->audio_frames += got;
        for (size_t i = 0; i < got * 2; i++)
            t->audio_peak = SDL_max(t->audio_peak, SDL_abs(buf[i]));
    }
    return 0;
}

static SDL_AppResult autotest_iterate(np_app *app)
{
    np_autotest *t = &app->autotest;
    autotest_script(app);
    process_pending(app);
    if (app->core && app->page == NP_PAGE_NONE && autotest_frame(app) && t->boot != NP_AT_APP)
        return SDL_APP_FAILURE;
    t->ran++;
    draw(app);
    if (t->shot_every > 0 && t->ran % t->shot_every == 0 && t->ran < t->frames) {
        char path[1100];
        size_t len = SDL_strlen(t->png);
        if (len > 4 && !SDL_strcasecmp(t->png + len - 4, ".png"))
            len -= 4;
        SDL_snprintf(path, sizeof path, "%.*s-%06d.png", (int)len, t->png, t->ran);
        if (capture_window(app, path))
            SDL_Log("autotest: writing %s failed: %s", path, SDL_GetError());
    }
    if (t->ran < t->frames) {
        SDL_RenderPresent(app->renderer);
        return SDL_APP_CONTINUE;
    }
    uint64_t guest_frame = app->have_frame ? app->frame.number : 0;
    if (t->page) {
        /* Show a UI page over (or instead of) the game for the capture. */
        if (t->page < 0 && app->core)
            np_app_stop_game(app);
        else if (t->page == NP_PAGE_MODS)
            np_mods_open(app, NULL);
        else if (t->page > 0)
            np_app_open_page(app, (np_page)t->page);
        draw(app);
    }
    if (capture_window(app, t->png)) {
        SDL_Log("autotest: writing %s failed: %s", t->png, SDL_GetError());
        return SDL_APP_FAILURE;
    }
    SDL_RenderPresent(app->renderer);
    if (app->core)
        np_core_save_flush(app->core);
    SDL_Log("autotest: boot=%s game=%s slot=\"%s\" iterations=%d guest_frame=%llu audio_frames=%llu audio_peak=%d "
            "save_stores=%d save_loads=%d save_bytes=%u view=%s page=%d png=%s",
            t->boot == NP_AT_ROM ? "rom" : t->boot == NP_AT_APP ? "app" : "synthetic", np_game_id(app->game),
            app->slot, t->ran, (unsigned long long)guest_frame, (unsigned long long)t->audio_frames, t->audio_peak,
            t->saves, t->loads, t->save_len, app->core ? "game" : "launcher", (int)app->page, t->png);
    if (app->status[0])
        SDL_Log("autotest: status: %s", app->status);
    if (t->boot == NP_AT_SYNTHETIC) {
        int save_ok = t->saves >= 1 && t->loads >= 1;
        if (!save_ok || !t->audio_frames) {
            SDL_Log("autotest: FAILED (%s)", save_ok ? "no audio" : "save did not round-trip");
            return SDL_APP_FAILURE;
        }
    } else if (t->boot == NP_AT_ROM && !app->core && t->page >= 0) {
        SDL_Log("autotest: FAILED (the core stopped)");
        return SDL_APP_FAILURE;
    }
    SDL_Log("autotest: OK");
    return SDL_APP_SUCCESS;
}

/* ---- SDL callbacks ------------------------------------------------------------ */

/* Command line, then the "continue last game" option. */
static void startup_launch(np_app *app, int argc, char *argv[])
{
    np_launch req;
    char err[160];
    if (np_launch_parse_args(argc, argv, &req, err, sizeof err)) {
        launch_fail(app, "Ignoring the command line: %s", err);
        return;
    }
    if (req.game >= 0 || req.force_launcher) {
        np_app_launch(app, &req);
        return;
    }
    int g = app->opt.last_game;
    if (app->opt.startup_continue && g >= 0 && g < NP_GAME_COUNT && np_core_available((np_game)g) &&
        np_storage_rom_present((np_game)g))
        np_app_continue(app, (np_game)g);
}

/* Unresolved folder sync conflicts are put in front of the player once per
 * launch: the chooser on the launcher, a hint in a game. */
static void show_sync_conflicts(np_app *app)
{
    if (!np_sync_conflicts(app))
        return;
    if (app->view == NP_VIEW_GAME)
        np_app_toast(app, "Sync conflict: choose in Options > Sync status");
    else if (app->page == NP_PAGE_NONE)
        np_app_open_page(app, NP_PAGE_SYNC);
}

SDL_AppResult SDL_AppInit(void **appstate, int argc, char *argv[])
{
    SDL_SetAppMetadata("nativeplat", "0.1.0", "org.nativeplat.nativeplat");
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight Portrait PortraitUpsideDown");
    np_app *app = SDL_calloc(1, sizeof *app);
    if (!app)
        return SDL_APP_FAILURE;
    *appstate = app;
    app->focused = 1;
    app->ui_press_hit = -1;
    np_options_defaults(&app->opt);

    int win_w = 960, win_h = 720, test_game = NP_GAME_PLATINUM;
    const char *spec = SDL_getenv("NP_AUTOTEST");
    np_autotest *t = &app->autotest;
    if (spec && *spec) {
        t->active = 1;
        if (parse_autotest(app, spec, &test_game, &win_w, &win_h)) {
            SDL_Log("NP_AUTOTEST: cannot parse \"%s\"", spec);
            return SDL_APP_FAILURE;
        }
    }

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        SDL_Log("SDL_Init: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }
    app->pending_lock = SDL_CreateMutex();
    if (!t->active || t->storage) {
        char err[512];
        /* Autotests may only write to a portable root, never a player's. */
        if (np_storage_init(t->active, err, sizeof err)) {
            if (t->active)
                SDL_Log("autotest: %s", err);
            else
                SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "nativeplat", err, NULL);
            return SDL_APP_FAILURE;
        }
        np_storage_path(app->options_path, sizeof app->options_path, "options.ini");
    }
    if ((!t->active || t->boot == NP_AT_APP) && np_options_load(&app->opt, app->options_path))
        SDL_Log("could not read %s; using defaults", app->options_path);
    if (t->sync_folder[0])
        SDL_strlcpy(app->opt.sync_folder, t->sync_folder, sizeof app->opt.sync_folder);
    if (t->gba_rom[0])
        SDL_strlcpy(app->opt.gba_rom, t->gba_rom, sizeof app->opt.gba_rom);
    if (t->gba_save[0])
        SDL_strlcpy(app->opt.gba_save, t->gba_save, sizeof app->opt.gba_save);
    if (!app->opt.station_id && t->active) {
        app->opt.station_id = 0x4E5001; /* fixed so scripted runs replay exactly */
    } else if (!app->opt.station_id) {
        app->opt.station_id = np_net_random_id() & 0xFFFFFFu;
        if (!app->opt.station_id)
            app->opt.station_id = 1;
        app->options_dirty = 1;
        save_options(app);
    }
    np_app_net_apply(app);

    SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (!SDL_CreateWindowAndRenderer("nativeplat", win_w, win_h, flags, &app->window, &app->renderer)) {
        SDL_Log("window: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }
    SDL_SetWindowMinimumSize(app->window, 256, 192);
    static const uint32_t black[NP_SCREEN_W * NP_SCREEN_H];
    for (int i = 0; i < 2; i++) {
        app->screen_tex[i] = SDL_CreateTexture(app->renderer, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING,
                                               NP_SCREEN_W, NP_SCREEN_H);
        if (!app->screen_tex[i]) {
            SDL_Log("texture: %s", SDL_GetError());
            return SDL_APP_FAILURE;
        }
        SDL_UpdateTexture(app->screen_tex[i], NULL, black, NP_SCREEN_W * 4);
    }
    if (np_ui_init(app)) {
        SDL_Log("font: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }
    if (np_fx_init(app)) {
        SDL_Log("display effects: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }
#if defined(SDL_PLATFORM_IOS)
    app->touch_seen = 1;
#endif
    np_app_apply_video_options(app);
    SDL_Log("renderer: %s%s", SDL_GetRendererName(app->renderer), t->active ? " (autotest)" : "");

    if (t->active) {
        if (t->storage && autotest_storage(app))
            return SDL_APP_FAILURE;
        np_sync_all(app, 1);
        if (t->boot == NP_AT_APP)
            startup_launch(app, argc, argv);
        else if (autotest_boot(app, (np_game)test_game))
            return SDL_APP_FAILURE;
        show_sync_conflicts(app);
        return SDL_APP_CONTINUE;
    }
    if (np_audio_open(app))
        SDL_Log("audio unavailable: %s", SDL_GetError());
    np_audio_set_paused(app, 1);
    np_sync_all(app, 1); /* before a game can start: pulls land first */
    startup_launch(app, argc, argv);
    show_sync_conflicts(app);
    return SDL_APP_CONTINUE;
}

static void toggle_options(np_app *app)
{
    np_app_open_page(app, app->page == NP_PAGE_NONE ? NP_PAGE_OPTIONS : NP_PAGE_NONE);
}

/* Fixed shell hotkeys; returns 1 if handled. */
static int hotkey(np_app *app, const SDL_KeyboardEvent *k)
{
    int alt = (k->mod & SDL_KMOD_ALT) != 0, gui = (k->mod & SDL_KMOD_GUI) != 0;
    switch (k->scancode) {
    case SDL_SCANCODE_F10: toggle_options(app); return 1;
    case SDL_SCANCODE_COMMA:
        if (!gui)
            return 0;
        toggle_options(app);
        return 1;
    case SDL_SCANCODE_RETURN:
        if (!alt)
            return 0;
        /* fallthrough */
    case SDL_SCANCODE_F11:
        app->opt.fullscreen = !app->opt.fullscreen;
        app->options_dirty = 1;
        np_app_apply_video_options(app);
        save_options(app);
        return 1;
    case SDL_SCANCODE_F12:
        if (app->view != NP_VIEW_GAME)
            return 0;
        take_screenshot(app);
        return 1;
    case SDL_SCANCODE_F9:
        app->opt.touch_controls = np_touchpad_visible(app) ? NP_TOUCH_OFF : NP_TOUCH_ON;
        app->options_dirty = 1;
        save_options(app);
        return 1;
    case SDL_SCANCODE_1:
        if (app->view != NP_VIEW_GAME || app->page != NP_PAGE_NONE)
            return 0;
        app->opt.speed_index = (app->opt.speed_index + 1) % NP_SPEED_COUNT;
        app->options_dirty = 1;
        save_options(app);
        if (np_speeds[app->opt.speed_index])
            np_app_toast(app, "Speed %dx", np_speeds[app->opt.speed_index]);
        else
            np_app_toast(app, "Speed uncapped");
        return 1;
    default: return !alt && !gui && np_session_hotkey(app, k->scancode);
    }
}

static void enter_background(np_app *app)
{
    /* iOS may kill a backgrounded app without warning: persist now. */
    if (app->core && np_core_save_flush(app->core))
        SDL_Log("save flush failed: %s", np_core_last_error(app->core));
    save_options(app);
    app->backgrounded = 1;
    np_input_release_all(app);
    update_audio_state(app);
}

SDL_AppResult SDL_AppEvent(void *appstate, SDL_Event *e)
{
    np_app *app = appstate;
    if (e->type == SDL_EVENT_QUIT || e->type == SDL_EVENT_TERMINATING)
        return SDL_APP_SUCCESS;
    if (app->autotest.active && !app->autotest.script[0])
        return SDL_APP_CONTINUE; /* deterministic: only scripted input */
    SDL_ConvertEventToRenderCoordinates(app->renderer, e);

    switch (e->type) {
    case SDL_EVENT_WILL_ENTER_BACKGROUND: enter_background(app); return SDL_APP_CONTINUE;
    case SDL_EVENT_DID_ENTER_FOREGROUND:
        app->backgrounded = 0;
        app->last_ns = SDL_GetTicksNS();
        app->accum_ns = 0;
        return SDL_APP_CONTINUE;
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        app->focused = e->type == SDL_EVENT_WINDOW_FOCUS_GAINED;
        if (!app->focused)
            np_input_release_all(app); /* the button-up may go to another app */
        np_audio_update_gain(app);
        return SDL_APP_CONTINUE;
    case SDL_EVENT_WINDOW_MINIMIZED: app->minimized = 1; return SDL_APP_CONTINUE;
    case SDL_EVENT_WINDOW_RESTORED:
    case SDL_EVENT_WINDOW_MAXIMIZED:
        app->minimized = 0;
        app->last_ns = SDL_GetTicksNS();
        return SDL_APP_CONTINUE;
    case SDL_EVENT_WINDOW_ENTER_FULLSCREEN:
    case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN: {
        int full = e->type == SDL_EVENT_WINDOW_ENTER_FULLSCREEN;
        if (app->opt.fullscreen != full) { /* changed by the OS, e.g. the green button */
            app->opt.fullscreen = full;
            app->options_dirty = 1;
        }
        return SDL_APP_CONTINUE;
    }
    case SDL_EVENT_DROP_FILE:
        if (e->drop.data)
            handle_drop(app, e->drop.data);
        return SDL_APP_CONTINUE;
    case SDL_EVENT_GAMEPAD_ADDED: np_input_gamepad_added(app, e->gdevice.which); return SDL_APP_CONTINUE;
    case SDL_EVENT_GAMEPAD_REMOVED: np_input_gamepad_removed(app, e->gdevice.which); return SDL_APP_CONTINUE;
    default: break;
    }

    int pad = np_input_pad_press(app, e);
    if (np_ui_capture_event(app, e, pad))
        return SDL_APP_CONTINUE;
    if (e->type == SDL_EVENT_KEY_DOWN && !e->key.repeat && hotkey(app, &e->key))
        return SDL_APP_CONTINUE;
    int in_game = app->view == NP_VIEW_GAME;
    if (in_game && (pad == SDL_GAMEPAD_BUTTON_GUIDE ||
                    (pad == SDL_GAMEPAD_BUTTON_RIGHT_STICK && app->page == NP_PAGE_NONE))) {
        toggle_options(app);
        return SDL_APP_CONTINUE;
    }
    if (np_input_pointer_event(app, e))
        return SDL_APP_CONTINUE;
    if (!in_game || app->page != NP_PAGE_NONE) {
        np_ui_command(app, np_input_menu_cmd(app, e, pad));
        return SDL_APP_CONTINUE;
    }
    int key = e->type == SDL_EVENT_KEY_DOWN && !e->key.repeat ? (int)e->key.scancode : 0;
    if ((key || pad != NP_PAD_NONE) && np_input_action_for(app, key, pad) == NP_ACT_FF_TOGGLE) {
        app->ff_toggle = !app->ff_toggle;
        np_app_toast(app, app->ff_toggle ? "Fast-forward on" : "Fast-forward off");
    }
    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppIterate(void *appstate)
{
    np_app *app = appstate;
    int w, h;
    SDL_GetCurrentRenderOutputSize(app->renderer, &w, &h);
    app->out_w = (float)w;
    app->out_h = (float)h;
    if (app->autotest.active)
        return autotest_iterate(app);

    uint64_t start = SDL_GetTicksNS();
    process_pending(app);
    if (app->view == NP_VIEW_GAME)
        run_game(app);
    else if (app->net)
        np_net_poll(app->net); /* launcher: discovery and the station count */
    update_audio_state(app);
    draw(app);
    /* Work done before the present (which may wait for VSync). */
    if (app->view == NP_VIEW_GAME && app->page == NP_PAGE_NONE &&
        np_fx_auto_sample(app, (double)(SDL_GetTicksNS() - start) / 1e6))
        np_app_apply_video_options(app);
    SDL_RenderPresent(app->renderer);
    return SDL_APP_CONTINUE;
}

void SDL_AppQuit(void *appstate, SDL_AppResult result)
{
    (void)result;
    np_app *app = appstate;
    if (!app)
        return;
    close_core(app);
    np_sync_all(app, 1);
    save_options(app);
    np_audio_close(app);
    np_input_close_gamepads(app);
    np_ui_destroy(app);
    np_fx_destroy(app);
    if (app->net)
        np_net_close(app->net);
    for (int i = 0; i < 2; i++)
        if (app->screen_tex[i])
            SDL_DestroyTexture(app->screen_tex[i]);
    if (app->renderer)
        SDL_DestroyRenderer(app->renderer);
    if (app->window)
        SDL_DestroyWindow(app->window);
    if (app->pending_lock)
        SDL_DestroyMutex(app->pending_lock);
    SDL_free(app->autotest.save);
    SDL_free(app);
}
