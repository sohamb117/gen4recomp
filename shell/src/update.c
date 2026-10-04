/*
 * Updater (Options > Updates...): checks a GitHub repository's latest
 * release and, if it is newer, downloads this platform's zip, verifies it
 * against the release's sha256sums.txt and reveals it in Finder / Explorer.
 * It never replaces the running app: installing stays the player's step, so
 * a bad release cannot break an installation and code signing is untouched.
 *
 * Nothing touches the network until the player presses Check; there are no
 * background checks. The repository is NP_UPDATE_REPO from the build (or
 * [updates] repo in options.ini); with none, the feature is hidden. Builds
 * without an HTTP backend (iOS, see http.h) do not compile the network part.
 *
 * Requests run on one worker thread; the page reads its state under a
 * mutex. A release is only offered when its zip and sha256sums.txt are both
 * attached, and the zip is renamed into place only after its digest
 * matches; until then it is "<name>.part" and removed on any failure.
 */
#include "app.h"

#include "http.h"
#include "release.h"
#include "sha256.h"

#ifndef NP_UPDATE_REPO
#define NP_UPDATE_REPO ""
#endif
#ifndef NP_VERSION_STRING
#define NP_VERSION_STRING "0.0.0"
#endif

#if defined(SDL_PLATFORM_WINDOWS)
#define PLATFORM_WORD "windows"
#define REVEAL_LABEL "Show in Explorer"
#else
#define PLATFORM_WORD "macos"
#define REVEAL_LABEL "Show in Finder"
#endif

typedef enum up_state {
    UP_IDLE,
    UP_CHECKING,
    UP_CURRENT,     /* nothing newer */
    UP_AVAILABLE,   /* newer release with this platform's zip */
    UP_DOWNLOADING,
    UP_READY,       /* verified zip in path */
    UP_ERROR,
} up_state;

typedef struct updater {
    SDL_Mutex *lock;
    SDL_Thread *thread;
    volatile int cancel;
    up_state state;
    char message[256];
    np_release rel;
    int asset, sums; /* indices into rel.asset */
    uint64_t got;    /* bytes downloaded */
    char path[1100]; /* the verified download */
    /* worker scratch */
    char repo[128], api[256], dir[1024];
} updater;

static updater up;

static void repo_of(const np_app *app, char *out, size_t n)
{
    SDL_strlcpy(out, app->opt.update_repo[0] ? app->opt.update_repo : NP_UPDATE_REPO, n);
}

int np_update_enabled(const np_app *app)
{
#ifdef NP_HAVE_HTTP
    char repo[128];
    repo_of(app, repo, sizeof repo);
    return repo[0] != 0;
#else
    (void)app;
    return 0;
#endif
}

#ifdef NP_HAVE_HTTP
static void set_state(up_state s, const char *fmt, ...) SDL_PRINTF_VARARG_FUNC(2);
static void set_state(up_state s, const char *fmt, ...)
{
    SDL_LockMutex(up.lock);
    up.state = s;
    va_list ap;
    va_start(ap, fmt);
    SDL_vsnprintf(up.message, sizeof up.message, fmt, ap);
    va_end(ap);
    SDL_UnlockMutex(up.lock);
    SDL_Log("update: %s", up.message);
}

static const char *user_agent(void) { return "nativeplat/" NP_VERSION_STRING; }

typedef struct membuf {
    char *data;
    size_t len, cap;
} membuf;

static int mem_sink(void *user, const void *data, size_t len)
{
    membuf *m = user;
    if (m->len + len + 1 > m->cap)
        return -1; /* larger than any release document we expect */
    SDL_memcpy(m->data + m->len, data, len);
    m->len += len;
    m->data[m->len] = '\0';
    return 0;
}

static int fetch(const char *url, const char *accept, membuf *m, char *err, size_t errn)
{
    m->len = 0;
    np_http_request r = {url, accept, user_agent(), mem_sink, m, &up.cancel};
    int status = np_http_get(&r, err, errn);
    if (status >= 0 && status != 200)
        SDL_snprintf(err, errn, "the server answered HTTP %d", status);
    return status == 200 ? 0 : -1;
}

static int SDLCALL check_thread(void *unused)
{
    (void)unused;
    char url[512], err[256];
    SDL_snprintf(url, sizeof url, "%s/repos/%s/releases/latest", up.api, up.repo);
    membuf m = {SDL_malloc(1 << 20), 0, 1 << 20};
    np_release *rel = SDL_malloc(sizeof *rel);
    if (!m.data || !rel) {
        set_state(UP_ERROR, "Out of memory");
    } else if (fetch(url, "application/vnd.github+json", &m, err, sizeof err)) {
        set_state(UP_ERROR, "Could not check: %s", err);
    } else if (np_release_parse(m.data, m.len, rel)) {
        set_state(UP_ERROR, "The release information could not be read");
    } else if (np_version_compare(rel->tag, NP_VERSION_STRING) <= 0) {
        SDL_LockMutex(up.lock);
        up.rel = *rel;
        SDL_UnlockMutex(up.lock);
        set_state(UP_CURRENT, "You have the latest version (newest release: %s)", rel->tag);
    } else {
        int asset = np_release_pick_asset(rel, PLATFORM_WORD);
        int sums = np_release_find_asset(rel, "sha256sums.txt");
        SDL_LockMutex(up.lock);
        up.rel = *rel;
        up.asset = asset;
        up.sums = sums;
        SDL_UnlockMutex(up.lock);
        if (asset < 0)
            set_state(UP_ERROR, "%s is out, but has no download for this platform", rel->tag);
        else if (sums < 0)
            set_state(UP_ERROR, "%s is out, but lists no sha256sums.txt to verify it with", rel->tag);
        else
            set_state(UP_AVAILABLE, "%s is available", rel->tag);
    }
    SDL_free(m.data);
    SDL_free(rel);
    return 0;
}

typedef struct file_sink {
    SDL_IOStream *io;
    np_sha256 hash;
} file_sink;

static int file_write(void *user, const void *data, size_t len)
{
    file_sink *f = user;
    if (SDL_WriteIO(f->io, data, len) != len)
        return -1;
    np_sha256_update(&f->hash, data, len);
    SDL_LockMutex(up.lock);
    up.got += len;
    SDL_UnlockMutex(up.lock);
    return 0;
}

static int SDLCALL download_thread(void *unused)
{
    (void)unused;
    const np_release_asset *a = &up.rel.asset[up.asset];
    char err[256], part[1100], final_path[1100], want[65], have[65];
    SDL_snprintf(final_path, sizeof final_path, "%s%s", up.dir, a->name);
    SDL_snprintf(part, sizeof part, "%s.part", final_path);

    membuf sums = {SDL_malloc(65536), 0, 65536};
    if (!sums.data || fetch(up.rel.asset[up.sums].url, NULL, &sums, err, sizeof err)) {
        set_state(UP_ERROR, "Could not get sha256sums.txt: %s", sums.data ? err : "out of memory");
        SDL_free(sums.data);
        return 0;
    }
    int listed = np_sha256sums_lookup(sums.data, sums.len, a->name, want);
    SDL_free(sums.data);
    if (listed) {
        set_state(UP_ERROR, "sha256sums.txt does not list %s", a->name);
        return 0;
    }

    file_sink f = {SDL_IOFromFile(part, "wb"), {{0}, 0, {0}, 0}};
    if (!f.io) {
        set_state(UP_ERROR, "Cannot write %s: %s", part, SDL_GetError());
        return 0;
    }
    np_sha256_init(&f.hash);
    np_http_request r = {a->url, "application/octet-stream", user_agent(), file_write, &f, &up.cancel};
    int status = np_http_get(&r, err, sizeof err);
    int closed = SDL_CloseIO(f.io);
    uint8_t digest[32];
    np_sha256_final(&f.hash, digest);
    np_sha256_hex(digest, have);
    if (status != 200 || !closed) {
        SDL_RemovePath(part);
        if (status > 0)
            SDL_snprintf(err, sizeof err, "the server answered HTTP %d", status);
        set_state(UP_ERROR, "Download failed: %s", closed ? err : SDL_GetError());
        return 0;
    }
    if (SDL_strcmp(have, want)) {
        SDL_RemovePath(part);
        set_state(UP_ERROR, "The download does not match its SHA-256 (got %.12s..., expected %.12s...); deleted", have,
                  want);
        return 0;
    }
    SDL_RemovePath(final_path);
    if (!SDL_RenamePath(part, final_path)) {
        set_state(UP_ERROR, "Cannot rename the download: %s", SDL_GetError());
        return 0;
    }
    SDL_LockMutex(up.lock);
    SDL_strlcpy(up.path, final_path, sizeof up.path);
    SDL_UnlockMutex(up.lock);
    set_state(UP_READY, "Verified (SHA-256 %.16s...). Quit nativeplat and install it from there.", have);
    return 0;
}

static int busy(void)
{
    SDL_LockMutex(up.lock);
    int b = up.state == UP_CHECKING || up.state == UP_DOWNLOADING;
    SDL_UnlockMutex(up.lock);
    return b;
}

static void join(void)
{
    if (up.thread) {
        SDL_WaitThread(up.thread, NULL);
        up.thread = NULL;
    }
}

static void start(int (*fn)(void *), up_state s, const char *what)
{
    join();
    up.cancel = 0;
    set_state(s, "%s", what);
    up.thread = SDL_CreateThread(fn, "np_update", NULL);
    if (!up.thread)
        set_state(UP_ERROR, "Cannot start: %s", SDL_GetError());
}

static void begin_check(np_app *app)
{
    repo_of(app, up.repo, sizeof up.repo);
    SDL_strlcpy(up.api, app->opt.update_api[0] ? app->opt.update_api : "https://api.github.com", sizeof up.api);
    start(check_thread, UP_CHECKING, "Checking...");
}

static void begin_download(np_app *app)
{
    /* The Downloads folder, or the user-data root for autotests (which
     * must not write outside portable storage). */
    const char *dl = app->autotest.active ? NULL : SDL_GetUserFolder(SDL_FOLDER_DOWNLOADS);
    if (dl)
        SDL_strlcpy(up.dir, dl, sizeof up.dir);
    else
        np_storage_path(up.dir, sizeof up.dir, "");
    up.got = 0;
    start(download_thread, UP_DOWNLOADING, "Downloading...");
}

static void reveal(np_app *app)
{
    char path[1100];
    SDL_LockMutex(up.lock);
    SDL_strlcpy(path, up.path, sizeof path);
    SDL_UnlockMutex(up.lock);
    if (app->autotest.active) {
        SDL_Log("update: reveal %s", path);
        return;
    }
#if defined(SDL_PLATFORM_WINDOWS)
    for (char *c = path; *c; c++)
        if (*c == '/')
            *c = '\\';
    char arg[1200];
    SDL_snprintf(arg, sizeof arg, "/select,%s", path);
    const char *argv[] = {"explorer.exe", arg, NULL};
#else
    const char *argv[] = {"/usr/bin/open", "-R", path, NULL};
#endif
    SDL_Process *p = SDL_CreateProcess(argv, false);
    if (!p)
        np_app_toast(app, "Cannot show it: %s", SDL_GetError());
    else
        SDL_DestroyProcess(p); /* runs on; nothing to wait for */
}

#endif /* NP_HAVE_HTTP */

void np_update_shutdown(void)
{
#ifdef NP_HAVE_HTTP
    up.cancel = 1;
    join();
#endif
    if (up.lock)
        SDL_DestroyMutex(up.lock);
    up.lock = NULL;
}

/* ---- page ---------------------------------------------------------------------- */

enum { ACT_CHECK, ACT_DOWNLOAD, ACT_CANCEL, ACT_REVEAL, ACT_PAGE, ACT_COUNT };

static int actions(int *out)
{
    SDL_LockMutex(up.lock);
    up_state s = up.state;
    int has_page = up.rel.html_url[0] != 0;
    SDL_UnlockMutex(up.lock);
    int n = 0;
    if (s == UP_DOWNLOADING || s == UP_CHECKING) {
        out[n++] = ACT_CANCEL;
    } else {
        if (s == UP_AVAILABLE)
            out[n++] = ACT_DOWNLOAD;
        if (s == UP_READY)
            out[n++] = ACT_REVEAL;
        out[n++] = ACT_CHECK;
    }
    if (has_page)
        out[n++] = ACT_PAGE;
    return n;
}

void np_update_open(np_app *app)
{
    if (!up.lock)
        up.lock = SDL_CreateMutex();
    np_app_open_page(app, NP_PAGE_UPDATES);
    app->sel = 0;
}

static void act(np_app *app, int a)
{
#ifdef NP_HAVE_HTTP
    switch (a) {
    case ACT_CHECK:
        if (!busy())
            begin_check(app);
        break;
    case ACT_DOWNLOAD:
        if (!busy())
            begin_download(app);
        break;
    case ACT_CANCEL: up.cancel = 1; break;
    case ACT_REVEAL: reveal(app); break;
    case ACT_PAGE:
        if (!SDL_OpenURL(up.rel.html_url))
            np_app_toast(app, "Cannot open the page: %s", SDL_GetError());
        break;
    default: break;
    }
#else
    (void)app, (void)a;
#endif
}

void np_update_command(np_app *app, np_menu_cmd cmd)
{
    int list[ACT_COUNT], n = actions(list);
    switch (cmd) {
    case NP_CMD_UP:
    case NP_CMD_LEFT: app->sel = (app->sel + n - 1) % n; break;
    case NP_CMD_DOWN:
    case NP_CMD_RIGHT: app->sel = (app->sel + 1) % n; break;
    case NP_CMD_CONFIRM: act(app, list[SDL_clamp(app->sel, 0, n - 1)]); break;
    default: break;
    }
}

void np_update_hit(np_app *app, int id)
{
    int list[ACT_COUNT], n = actions(list);
    if (id >= 0 && id < n)
        act(app, list[id]);
}

void np_update_draw(np_app *app)
{
    static const SDL_Color white = {235, 235, 235, 255}, dim = {150, 150, 160, 255}, accent = {255, 205, 80, 255},
                           warn = {255, 120, 100, 255}, good = {140, 220, 140, 255};
    np_page_frame f;
    np_ui_begin_page(app, &f, "Updates");
    float x = f.panel.x + 2 * f.cw, y = f.list_y;
    char line[320], repo[128];
    repo_of(app, repo, sizeof repo);
    SDL_snprintf(line, sizeof line, "Installed: nativeplat %s", NP_VERSION_STRING);
    np_ui_text_clip(app, x, y, f.s, line, f.cols - 4, white);
    y += f.lh;
    SDL_snprintf(line, sizeof line, "Releases from: github.com/%s", repo);
    np_ui_text_clip(app, x, y, f.s, line, f.cols - 4, dim);
    y += 1.5f * f.lh;

    SDL_LockMutex(up.lock);
    up_state s = up.state;
    char msg[256];
    SDL_strlcpy(msg, up.message, sizeof msg);
    uint64_t got = up.got, size = up.asset >= 0 && up.asset < up.rel.nassets ? up.rel.asset[up.asset].size : 0;
    char asset[128] = "", path[1100];
    if (up.asset >= 0 && up.asset < up.rel.nassets)
        SDL_strlcpy(asset, up.rel.asset[up.asset].name, sizeof asset);
    SDL_strlcpy(path, up.path, sizeof path);
    SDL_UnlockMutex(up.lock);

    if (s == UP_IDLE)
        SDL_strlcpy(msg, "Nothing is checked until you choose Check for updates.", sizeof msg);
    SDL_Color c = s == UP_ERROR ? warn : s == UP_READY || s == UP_AVAILABLE ? good : white;
    y += (float)np_ui_text_wrap(app, x, y, f.s, f.lh, f.cols - 4, msg, c, 1) * f.lh;
    if ((s == UP_AVAILABLE || s == UP_DOWNLOADING) && asset[0]) {
        SDL_snprintf(line, sizeof line, "%s, %.1f MB", asset, (double)size / 1048576.0);
        np_ui_text_clip(app, x, y, f.s, line, f.cols - 4, dim);
        y += f.lh;
    }
    if (s == UP_DOWNLOADING && size) {
        float w = f.panel.w - 4 * f.cw, frac = got >= size ? 1.0f : (float)got / (float)size;
        np_ui_frame(app, (SDL_FRect){x, y, w, f.lh * 0.8f}, f.s, dim);
        np_ui_fill(app, (SDL_FRect){x + 2 * f.s, y + 2 * f.s, (w - 4 * f.s) * frac, f.lh * 0.8f - 4 * f.s}, accent);
        y += f.lh;
    }
    if (s == UP_READY) {
        y += (float)np_ui_text_wrap(app, x, y, f.s, f.lh, f.cols - 4, path, dim, 1) * f.lh;
    }
    y += 0.5f * f.lh;

    static const char *const labels[ACT_COUNT] = {"Check for updates", "Download and verify", "Cancel", REVEAL_LABEL,
                                                  "Open release page"};
    int list[ACT_COUNT], n = actions(list);
    app->sel = SDL_clamp(app->sel, 0, n - 1);
    float bw = 24 * f.cw, bh = 1.6f * f.lh;
    for (int i = 0; i < n; i++)
        np_ui_button(app, (SDL_FRect){x, y + (float)i * (bh + 0.4f * f.lh), bw, bh}, labels[list[i]], app->sel == i, i,
                     f.s);
    y += (float)n * (bh + 0.4f * f.lh) + 0.5f * f.lh;
    np_ui_text_wrap(app, x, y, f.s, f.lh, f.cols - 4,
                    "nativeplat never replaces itself: a verified download is shown to you to install.", dim, 1);
    np_ui_end_page(app, &f, "Enter/A: select  Esc/B: back", 0);
}
