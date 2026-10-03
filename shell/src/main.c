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
 */
#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL_main.h>

#include <math.h>

#include "app.h"
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
    return np_storage_save_load(app->game, dst, len);
}

static int host_save_store(void *user, const void *src, uint32_t len)
{
    np_app *app = user;
    int r = np_storage_save_store(app->game, src, len);
    if (r)
        np_app_toast(app, "Saving failed: %s", SDL_GetError());
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

/* Autotest variants: cartridge header and backup chip live in memory. */
static int test_rom_read(void *user, uint32_t offset, void *dst, uint32_t len)
{
    np_app *app = user;
    if ((uint64_t)offset + len > sizeof app->autotest.rom)
        return -1;
    SDL_memcpy(dst, app->autotest.rom + offset, len);
    return 0;
}

static int test_save_load(void *user, void *dst, uint32_t len)
{
    np_app *app = user;
    np_autotest *t = &app->autotest;
    if (t->storage) {
        int r = np_storage_save_load(app->game, dst, len);
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
        int r = np_storage_save_store(app->game, src, len);
        if (!r) {
            t->saves++;
            t->save_len = len;
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
    app->toast_until = SDL_GetTicksNS() + 2 * SDL_NS_PER_SECOND;
}

int np_app_speed(const np_app *app)
{
    int ff = app->ff_hold || app->ff_toggle;
    return np_speeds[ff ? app->opt.ff_speed_index : app->opt.speed_index];
}

static void save_options(np_app *app)
{
    if (!app->options_dirty || app->autotest.active)
        return;
    if (np_options_save(&app->opt, app->options_path))
        SDL_Log("could not write %s: %s", app->options_path, SDL_GetError());
    app->options_dirty = 0;
}

void np_app_apply_video_options(np_app *app)
{
    const np_options *o = &app->opt;
    if (!app->autotest.active)
        SDL_SetRenderVSync(app->renderer, o->vsync ? 1 : SDL_RENDERER_VSYNC_DISABLED);
    char rate[16];
    int cap = np_fps_caps[o->fps_cap_index];
    /* Without VSync or a cap, menus would spin a core at 100%: idle at 60. */
    if (!cap && !o->vsync && app->view != NP_VIEW_GAME)
        cap = 60;
    SDL_snprintf(rate, sizeof rate, "%d", cap);
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
    app->page_parent = (page == NP_PAGE_CONTROLS || page == NP_PAGE_ABOUT) ? app->page : NP_PAGE_NONE;
    app->page = page;
    app->sel = app->col = app->scroll = 0;
    app->capture = 0;
    np_input_release_all(app);
    /* Do not "catch up" on the time spent in a menu. */
    app->accum_ns = 0;
    app->last_ns = SDL_GetTicksNS();
}

static int open_core(np_app *app, np_game game, const np_host *host)
{
    app->game = game;
    app->host = *host;
    app->core = np_core_create(game, &app->host, NULL);
    if (!app->core) {
        SDL_snprintf(app->status, sizeof app->status, "Could not start %s: %s", np_game_title(game),
                     np_core_create_error());
        return -1;
    }
    app->view = NP_VIEW_GAME;
    app->page = NP_PAGE_NONE;
    app->have_frame = 0;
    app->accum_ns = 0;
    app->last_ns = SDL_GetTicksNS();
    app->ff_toggle = app->ff_hold = 0;
    np_input_release_all(app);
    char title[64];
    SDL_snprintf(title, sizeof title, "nativeplat - Pokemon %s", np_game_title(game));
    SDL_SetWindowTitle(app->window, title);
    np_app_apply_video_options(app);
    return 0;
}

int np_app_start_game(np_app *app, np_game game)
{
    char path[1100];
    np_storage_rom_path(game, path, sizeof path);
    app->rom_io = SDL_IOFromFile(path, "rb");
    Sint64 size = app->rom_io ? SDL_GetIOSize(app->rom_io) : -1;
    if (size <= 0 || size > (Sint64)UINT32_MAX) {
        SDL_snprintf(app->status, sizeof app->status, "Cannot read %s: %s", path, SDL_GetError());
        if (app->rom_io)
            SDL_CloseIO(app->rom_io);
        app->rom_io = NULL;
        return -1;
    }
    np_host host = {app, (uint32_t)size, host_rom_read, host_save_load, host_save_store, host_rtc_now, host_log};
    if (open_core(app, game, &host)) {
        SDL_CloseIO(app->rom_io);
        app->rom_io = NULL;
        return -1;
    }
    app->status[0] = '\0';
    return 0;
}

static void close_core(np_app *app)
{
    if (!app->core)
        return;
    if (np_core_save_flush(app->core))
        SDL_Log("save flush failed: %s", np_core_last_error(app->core));
    np_core_destroy(app->core);
    app->core = NULL;
    if (app->rom_io)
        SDL_CloseIO(app->rom_io);
    app->rom_io = NULL;
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

/* ---- ROM import ---------------------------------------------------------- */

void np_app_request_import(np_app *app, const char *path)
{
    SDL_LockMutex(app->import_lock);
    SDL_strlcpy(app->import_path, path, sizeof app->import_path);
    app->import_stage = 1;
    SDL_UnlockMutex(app->import_lock);
}

/* May run on another thread: only hands the path over. */
static void SDLCALL dialog_done(void *user, const char *const *files, int filter)
{
    (void)filter;
    np_app *app = user;
    if (!files) {
        SDL_Log("file dialog failed: %s", SDL_GetError());
        SDL_LockMutex(app->import_lock);
        SDL_snprintf(app->import_path, sizeof app->import_path, "The file picker is unavailable: %s",
                     SDL_GetError());
        app->import_stage = 3;
        SDL_UnlockMutex(app->import_lock);
        return;
    }
    if (files[0])
        np_app_request_import(app, files[0]);
}

void np_app_open_import_dialog(np_app *app)
{
    static const SDL_DialogFileFilter filters[] = {{"Nintendo DS ROM (*.nds)", "nds"}};
    SDL_ShowOpenFileDialog(dialog_done, app, app->window, filters, 1, NULL, false);
}

/* Two-step so "Verifying" is on screen while a 128 MiB file is hashed. */
static void process_import(np_app *app)
{
    char path[1024];
    SDL_LockMutex(app->import_lock);
    int stage = app->import_stage;
    if (stage == 1) {
        if (app->view == NP_VIEW_GAME) {
            np_app_toast(app, "Quit to the launcher to import a ROM");
            app->import_stage = 0;
        } else {
            const char *base = SDL_strrchr(app->import_path, '/');
            const char *base2 = SDL_strrchr(app->import_path, '\\');
            if (base2 > base)
                base = base2;
            SDL_snprintf(app->status, sizeof app->status, "Verifying %s ...", base ? base + 1 : app->import_path);
            app->import_stage = 2;
        }
    } else if (stage == 2) {
        SDL_strlcpy(path, app->import_path, sizeof path);
        app->import_stage = 0;
    } else if (stage == 3) {
        SDL_strlcpy(app->status, app->import_path, sizeof app->status);
        app->import_stage = 0;
    }
    SDL_UnlockMutex(app->import_lock);
    if (stage != 2)
        return;
    np_import_result r;
    np_storage_import_rom(path, &r);
    SDL_strlcpy(app->status, r.message, sizeof app->status);
    SDL_Log("import %s: %s", path, r.message);
    if (r.ok)
        app->launcher_sel = r.game;
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

static void upload_frame(np_app *app)
{
    for (int i = 0; i < 2; i++)
        SDL_UpdateTexture(app->screen_tex[i], NULL, app->frame.screen[i], (int)(app->frame.stride * 4));
}

static int run_one(np_app *app, const np_input *in)
{
    int r = np_core_run_frame(app->core, in, &app->frame);
    if (r == 0) {
        app->have_frame = 1;
        return 0;
    }
    if (r > 0)
        SDL_strlcpy(app->status, "The game has exited.", sizeof app->status);
    else
        SDL_snprintf(app->status, sizeof app->status, "The game stopped: %s", np_core_last_error(app->core));
    SDL_Log("%s", app->status);
    np_app_stop_game(app);
    return -1;
}

static void run_game(np_app *app)
{
    uint64_t now = SDL_GetTicksNS();
    uint64_t dt = now - app->last_ns;
    app->last_ns = now;
    if (dt > MAX_DT_NS)
        dt = MAX_DT_NS; /* after a stall, resume instead of fast-forwarding */
    if (app->page != NP_PAGE_NONE || app->backgrounded || app->minimized)
        return;

    np_input in = {0};
    in.keys = np_input_poll_keys(app, &app->ff_hold);
    np_input_stylus(app, &in);
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
    np_layout_params lp = {app->opt.layout, app->opt.swap, app->opt.rotation, app->opt.scale};
    np_layout_compute(&app->layout, &lp, app->out_w, app->out_h);
    for (int i = 0; i < 2; i++) {
        const np_screen_place *sp = &app->layout.screen[i];
        if (!sp->visible)
            continue;
        SDL_FRect dst = {sp->cx - sp->w * 0.5f, sp->cy - sp->h * 0.5f, sp->w, sp->h};
        SDL_RenderTextureRotated(app->renderer, app->screen_tex[i], NULL, &dst, 90.0 * app->layout.rotation, NULL,
                                 SDL_FLIP_NONE);
    }
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

static int parse_game(const char *v)
{
    for (int g = 0; g < NP_GAME_COUNT; g++)
        if (!SDL_strcasecmp(v, np_game_id((np_game)g)))
            return g;
    return -1;
}

static uint16_t parse_keys(char *v)
{
    static const char *const names[12] = {"a", "b", "select", "start", "right", "left",
                                          "up", "down", "r", "l", "x", "y"};
    uint16_t keys = 0;
    char *save = NULL;
    for (char *t = SDL_strtok_r(v, "+", &save); t; t = SDL_strtok_r(NULL, "+", &save))
        for (int i = 0; i < 12; i++)
            if (!SDL_strcasecmp(t, names[i]))
                keys |= (uint16_t)(1u << i);
    return keys;
}

/*
 * NP_AUTOTEST="frames=120,png=/tmp/shot.png[,game=platinum][,layout=vertical|
 *   horizontal|hybrid|top|bottom][,rotation=0..3][,swap=1][,scale=integer]
 *   [,filter=linear][,touch=XxY][,keys=a+up][,controls=1][,size=WxH]
 *   [,page=launcher|options|controls|about]
 *   [,storage=1 (saves and options.ini in the real user-data root)]
 *   [,import=/path/to/rom.nds (run the importer first; implies storage=1)]
 *   [,script=F:kind:args;... (synthetic input, see autotest_script)]"
 * Returns -1 on a malformed value.
 */
static int parse_autotest(np_app *app, const char *spec, int *game, int *win_w, int *win_h)
{
    static const char *const layouts[NP_LAYOUT_COUNT] = {"vertical", "horizontal", "hybrid", "top", "bottom"};
    char buf[2048];
    SDL_strlcpy(buf, spec, sizeof buf);
    np_autotest *t = &app->autotest;
    t->frames = 120;
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
            if ((*game = parse_game(v)) < 0)
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
        else if (!SDL_strcmp(kv, "import")) {
            SDL_strlcpy(t->import, v, sizeof t->import);
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
    SDL_memset(t->rom, 0, sizeof t->rom);
    SDL_memcpy(t->rom, titles[game], SDL_strlen(titles[game]));
    SDL_memcpy(t->rom + 0x0C, codes[game], 4);
    SDL_memcpy(t->rom + 0x10, "01", 2);
}

/* Boot, run a frame, flush and destroy, then boot again so the second
 * session must load what the first one stored. */
static int autotest_boot(np_app *app, np_game game)
{
    fill_test_header(&app->autotest, game);
    np_host host = {app, sizeof app->autotest.rom, test_rom_read, test_save_load, test_save_store, NULL, host_log};
    np_input none = {0};
    if (open_core(app, game, &host) || run_one(app, &none))
        return -1;
    np_core_save_flush(app->core);
    close_core(app);
    if (open_core(app, game, &host))
        return -1;
    return 0;
}

/* storage=1: run the importer on `import=`, and round-trip an options file
 * (written beside options.ini, never over it) through save and load. */
static int autotest_storage(np_app *app)
{
    np_autotest *t = &app->autotest;
    SDL_Log("autotest: user data root %s%s", np_storage_root(), np_storage_is_portable() ? " (portable)" : "");
    if (t->import[0]) {
        np_import_result r;
        np_storage_import_rom(t->import, &r);
        SDL_Log("autotest: import %s: ok=%d %s", t->import, r.ok, r.message);
    }
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

/*
 * script=F:kind:args;... pushes synthetic events through SDL_AppEvent before
 * frame F (window coordinates in points):
 *   F:key:<SDL scancode name>        press and release a key
 *   F:down:X:Y  F:move:X:Y  F:up:X:Y left mouse button
 *   F:fdown:X:Y F:fmove:X:Y F:fup:X:Y one finger
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
        char kind[16], arg[64] = "";
        float x = 0, y = 0;
        if (SDL_sscanf(step, "%d:%15[a-z]:%63[^;]", &frame, kind, arg) < 2 || frame != t->ran)
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
        } else {
            continue;
        }
        SDL_PushEvent(&e);
    }
}

static SDL_AppResult autotest_iterate(np_app *app)
{
    np_autotest *t = &app->autotest;
    if (!app->core)
        return SDL_APP_FAILURE;
    np_input in = t->input;
    if (t->script[0]) {
        autotest_script(app);
        if (!app->core)
            return SDL_APP_FAILURE; /* the script quit the game */
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
    if (run_one(app, &in))
        return SDL_APP_FAILURE;
    t->ran++;
    upload_frame(app);
    int16_t buf[2048 * 2];
    size_t got;
    while ((got = np_core_audio_read(app->core, buf, 2048)) > 0) {
        t->audio_frames += got;
        for (size_t i = 0; i < got * 2; i++)
            t->audio_peak = SDL_max(t->audio_peak, SDL_abs(buf[i]));
    }
    draw(app);
    if (t->ran < t->frames) {
        SDL_RenderPresent(app->renderer);
        return SDL_APP_CONTINUE;
    }
    uint64_t guest_frame = app->frame.number;
    if (t->page) {
        /* Show a UI page over (or instead of) the game for the capture. */
        if (t->page < 0)
            np_app_stop_game(app);
        else
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
    int save_ok = t->saves >= 1 && t->loads >= 1;
    SDL_Log("autotest: game=%s frames=%d guest_frame=%llu audio_frames=%llu audio_peak=%d save_stores=%d "
            "save_loads=%d save_bytes=%u png=%s",
            np_game_id(app->game), t->ran, (unsigned long long)guest_frame,
            (unsigned long long)t->audio_frames, t->audio_peak, t->saves, t->loads, t->save_len, t->png);
    if (!save_ok || !t->audio_frames) {
        SDL_Log("autotest: FAILED (%s)", save_ok ? "no audio" : "save did not round-trip");
        return SDL_APP_FAILURE;
    }
    SDL_Log("autotest: OK");
    return SDL_APP_SUCCESS;
}

/* ---- SDL callbacks ------------------------------------------------------------ */

SDL_AppResult SDL_AppInit(void **appstate, int argc, char *argv[])
{
    (void)argc;
    (void)argv;
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
    if (spec && *spec) {
        app->autotest.active = 1;
        if (parse_autotest(app, spec, &test_game, &win_w, &win_h)) {
            SDL_Log("NP_AUTOTEST: cannot parse \"%s\"", spec);
            return SDL_APP_FAILURE;
        }
    }

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        SDL_Log("SDL_Init: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }
    app->import_lock = SDL_CreateMutex();
    if (!app->autotest.active || app->autotest.storage) {
        char err[512];
        if (np_storage_init(err, sizeof err)) {
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "nativeplat", err, NULL);
            return SDL_APP_FAILURE;
        }
        np_storage_path(app->options_path, sizeof app->options_path, "options.ini");
    }
    if (!app->autotest.active && np_options_load(&app->opt, app->options_path))
        SDL_Log("could not read %s; using defaults", app->options_path);

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
#if defined(SDL_PLATFORM_IOS)
    app->touch_seen = 1;
#endif
    np_app_apply_video_options(app);
    SDL_Log("renderer: %s%s", SDL_GetRendererName(app->renderer),
            app->autotest.active ? " (autotest)" : "");

    if (app->autotest.active) {
        SDL_SetHint(SDL_HINT_MAIN_CALLBACK_RATE, "0");
        if (app->autotest.storage && autotest_storage(app))
            return SDL_APP_FAILURE;
        if (autotest_boot(app, (np_game)test_game))
            return SDL_APP_FAILURE;
        return SDL_APP_CONTINUE;
    }
    if (np_audio_open(app))
        SDL_Log("audio unavailable: %s", SDL_GetError());
    np_audio_set_paused(app, 1);
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
    default: return 0;
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
            np_app_request_import(app, e->drop.data);
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

    process_import(app);
    if (app->view == NP_VIEW_GAME)
        run_game(app);
    update_audio_state(app);
    draw(app);
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
    save_options(app);
    np_audio_close(app);
    np_input_close_gamepads(app);
    np_ui_destroy(app);
    for (int i = 0; i < 2; i++)
        if (app->screen_tex[i])
            SDL_DestroyTexture(app->screen_tex[i]);
    if (app->renderer)
        SDL_DestroyRenderer(app->renderer);
    if (app->window)
        SDL_DestroyWindow(app->window);
    if (app->import_lock)
        SDL_DestroyMutex(app->import_lock);
    SDL_free(app->autotest.save);
    SDL_free(app);
}
