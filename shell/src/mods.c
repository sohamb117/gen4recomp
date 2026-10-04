/*
 * Mod manager (Options > Mods...): runtime content packages for the
 * Platinum core. Packages live as plain directories in <user data>/mods/,
 * which the core sees read-only as /content (np_host.content_root). The
 * enabled packages and their load order are mods/loadorder.txt, the file
 * the core reads when PC_MODS is not set, so the folder stays meaningful
 * without the app.
 *
 * Installing takes a .zip holding one package (mod.toml at its root or in a
 * single top directory) with its cooked .cooked/digest. Every member name
 * must be a plain relative path and no member may be a symlink, because the
 * core confines lookups to the root and a package must not reach outside
 * it. The package is extracted beside the others under a temporary name
 * and renamed into place, so a failed install leaves nothing half-written.
 *
 * Changes take effect when the core boots: a running game shows that it
 * needs a restart. A broken package stops the boot with "modfs: ..."; the
 * page then opens with that message and the offending package selected.
 */
#include "app.h"

#include "modpkg.h"
#include "zip.h"

#define MAX_PKGS 64
#define MAX_INSTALL_BYTES (512u << 20)

typedef struct mod_pkg {
    char dir[NP_MOD_ID_MAX];
    np_mod_info info;
    int valid;   /* mod.toml parsed */
    int cooked;  /* .cooked/digest present */
    int enabled;
    char problem[96];
} mod_pkg;

typedef struct mods_state {
    mod_pkg pkg[MAX_PKGS]; /* enabled first, in load order; then the rest by name */
    int count;
    char banner[256];     /* a boot error to explain, or "" */
    int changed;          /* since the running game booted */
    uint64_t remove_armed; /* ns: a second X before this removes */
} mods_state;

static mods_state ms;

static void mods_dir(char *out, size_t n) { np_storage_path(out, n, "mods"); }

static void pkg_path(const char *dir, const char *rel, char *out, size_t n)
{
    char root[1100];
    mods_dir(root, sizeof root);
    SDL_snprintf(out, n, "%s/%s%s%s", root, dir, rel[0] ? "/" : "", rel);
}

int np_mods_content_root(const np_app *app, np_game game, char *out, size_t n)
{
    (void)app;
    if (game != NP_GAME_PLATINUM)
        return -1; /* the D/P cores have no runtime content yet */
    mods_dir(out, n);
    SDL_PathInfo info;
    return SDL_GetPathInfo(out, &info) && info.type == SDL_PATHTYPE_DIRECTORY ? 0 : -1;
}

/* ---- scanning ------------------------------------------------------------------ */

typedef struct name_list {
    char names[MAX_PKGS][NP_MOD_ID_MAX];
    int count;
} name_list;

static SDL_EnumerationResult SDLCALL collect_dir(void *user, const char *dirname, const char *fname)
{
    name_list *l = user;
    char path[1200];
    SDL_snprintf(path, sizeof path, "%s%s", dirname, fname);
    SDL_PathInfo info;
    if (fname[0] != '.' && l->count < MAX_PKGS && SDL_strlen(fname) < NP_MOD_ID_MAX && SDL_GetPathInfo(path, &info) &&
        info.type == SDL_PATHTYPE_DIRECTORY)
        SDL_strlcpy(l->names[l->count++], fname, NP_MOD_ID_MAX);
    return SDL_ENUM_CONTINUE;
}

static int cmp_names(const void *a, const void *b) { return SDL_strcasecmp(a, b); }

static void load_package(mod_pkg *p, const char *dir)
{
    SDL_zerop(p);
    SDL_strlcpy(p->dir, dir, sizeof p->dir);
    char path[1200];
    pkg_path(dir, "mod.toml", path, sizeof path);
    size_t len;
    char *text = SDL_LoadFile(path, &len);
    const char *why = "no mod.toml";
    p->valid = text && !np_mod_parse(text, len, &p->info, &why);
    SDL_free(text);
    if (!p->valid) {
        SDL_strlcpy(p->info.name, dir, sizeof p->info.name);
        SDL_strlcpy(p->problem, why, sizeof p->problem);
        return;
    }
    pkg_path(dir, ".cooked/digest", path, sizeof path);
    SDL_PathInfo info;
    p->cooked = SDL_GetPathInfo(path, &info) && info.type == SDL_PATHTYPE_FILE;
    if (!p->cooked)
        SDL_strlcpy(p->problem, "not cooked: run the cook step on it", sizeof p->problem);
}

/* Rereads the folder and loadorder.txt, then marks rule breaks. */
static void scan(void)
{
    char root[1100];
    mods_dir(root, sizeof root);
    static name_list dirs, order;
    dirs.count = order.count = 0;
    SDL_EnumerateDirectory(root, collect_dir, &dirs);
    SDL_qsort(dirs.names, (size_t)dirs.count, NP_MOD_ID_MAX, cmp_names);

    char path[1200];
    SDL_snprintf(path, sizeof path, "%s/loadorder.txt", root);
    char *text = SDL_LoadFile(path, NULL);
    char *save = NULL;
    for (char *line = text ? SDL_strtok_r(text, "\r\n", &save) : NULL; line; line = SDL_strtok_r(NULL, "\r\n", &save)) {
        while (*line == ' ' || *line == '\t')
            line++;
        if (*line && *line != '#' && order.count < MAX_PKGS)
            SDL_strlcpy(order.names[order.count++], line, NP_MOD_ID_MAX);
    }
    SDL_free(text);

    ms.count = 0;
    for (int o = 0; o < order.count; o++)
        for (int d = 0; d < dirs.count; d++)
            if (dirs.names[d][0] && !SDL_strcmp(order.names[o], dirs.names[d])) {
                load_package(&ms.pkg[ms.count], dirs.names[d]);
                ms.pkg[ms.count++].enabled = 1;
                dirs.names[d][0] = '\0'; /* listed once */
            }
    for (int d = 0; d < dirs.count; d++)
        if (dirs.names[d][0])
            load_package(&ms.pkg[ms.count++], dirs.names[d]);

    const np_mod_info *enabled[MAX_PKGS];
    int map[MAX_PKGS], n = 0;
    for (int i = 0; i < ms.count; i++)
        if (ms.pkg[i].enabled && ms.pkg[i].valid) {
            map[n] = i;
            enabled[n++] = &ms.pkg[i].info;
        }
    char why[96];
    int bad = np_mod_check_order(enabled, n, why, sizeof why);
    if (bad >= 0 && !ms.pkg[map[bad]].problem[0])
        SDL_strlcpy(ms.pkg[map[bad]].problem, why, sizeof ms.pkg[0].problem);
}

static int save_order(np_app *app)
{
    char root[1100], path[1200], buf[MAX_PKGS * (NP_MOD_ID_MAX + 1) + 128];
    mods_dir(root, sizeof root);
    size_t len = (size_t)SDL_snprintf(buf, sizeof buf, "# Enabled packages, first loaded first (nativeplat).\n");
    for (int i = 0; i < ms.count; i++)
        if (ms.pkg[i].enabled)
            len += (size_t)SDL_snprintf(buf + len, sizeof buf - len, "%s\n", ms.pkg[i].dir);
    SDL_snprintf(path, sizeof path, "%s/loadorder.txt", root);
    if (np_storage_write_atomic(path, buf, len, 0)) {
        np_app_toast(app, "Cannot save the load order: %s", SDL_GetError());
        return -1;
    }
    ms.changed = 1;
    return 0;
}

/* ---- installing ---------------------------------------------------------------- */

/* The package inside a zip: the prefix its members share ("" or "dir/")
 * and the directory name it gets. Returns 0 or -1 with *why. */
static int find_package_root(const np_zip *z, char *prefix, size_t pn, char *name, size_t nn, np_mod_info *info,
                             const char **why)
{
    np_zip_entry e;
    int found = 0;
    for (uint32_t i = 0; i < z->count; i++) {
        if (np_zip_entry_at(z, i, &e)) {
            *why = "damaged zip";
            return -1;
        }
        const char *base = SDL_strrchr(e.name, '/');
        base = base ? base + 1 : e.name;
        size_t depth = 0;
        for (const char *c = e.name; *c; c++)
            depth += *c == '/';
        if (SDL_strcmp(base, "mod.toml") || depth > 1)
            continue;
        if (found++) {
            *why = "the zip holds more than one package";
            return -1;
        }
        SDL_strlcpy(prefix, e.name, pn);
        prefix[base - e.name] = '\0';
        char *text = SDL_malloc((size_t)e.size + 1);
        if (!text || e.size > 65536 || np_zip_extract(z, &e, text)) {
            SDL_free(text);
            *why = "cannot read mod.toml";
            return -1;
        }
        int bad = np_mod_parse(text, e.size, info, why);
        SDL_free(text);
        if (bad)
            return -1;
    }
    if (!found) {
        *why = "no mod.toml in the zip";
        return -1;
    }
    if (prefix[0]) {
        SDL_strlcpy(name, prefix, nn);
        name[SDL_strlen(name) - 1] = '\0'; /* "dir/" -> "dir" */
    } else {
        SDL_strlcpy(name, info->id, nn);
    }
    if (!name[0] || name[0] == '.' || SDL_strlen(name) >= NP_MOD_ID_MAX || SDL_strpbrk(name, " /\\:")) {
        *why = "unusable package directory name";
        return -1;
    }
    char digest[600];
    SDL_snprintf(digest, sizeof digest, "%s.cooked/digest", prefix);
    if (np_zip_find(z, digest, &e) < 0) {
        *why = "the package is not cooked (.cooked/digest missing)";
        return -1;
    }
    return 0;
}

static int extract_all(const np_zip *z, const char *prefix, const char *dest, const char **why)
{
    size_t plen = SDL_strlen(prefix);
    uint64_t total = 0;
    np_zip_entry e;
    for (uint32_t i = 0; i < z->count; i++) {
        if (np_zip_entry_at(z, i, &e) || !np_zip_name_safe(e.name)) {
            *why = "the zip has an unsafe member name";
            return -1;
        }
        if (e.is_symlink) {
            *why = "the zip contains a symbolic link";
            return -1;
        }
        total += e.size;
    }
    if (total > MAX_INSTALL_BYTES) {
        *why = "the package is larger than 512 MB";
        return -1;
    }
    for (uint32_t i = 0; i < z->count; i++) {
        np_zip_entry_at(z, i, &e);
        if (SDL_strncmp(e.name, prefix, plen) || !e.name[plen])
            continue; /* outside the package (e.g. __MACOSX/) */
        char path[1200];
        SDL_snprintf(path, sizeof path, "%s/%s", dest, e.name + plen);
        if (e.is_dir) {
            if (!SDL_CreateDirectory(path))
                goto io;
            continue;
        }
        char *slash = SDL_strrchr(path, '/');
        *slash = '\0';
        int made = SDL_CreateDirectory(path);
        *slash = '/';
        void *data = SDL_malloc(e.size ? e.size : 1);
        if (!made || !data || np_zip_extract(z, &e, data)) {
            SDL_free(data);
            *why = made && data ? "a member failed its checksum" : SDL_GetError();
            return -1;
        }
        int ok = SDL_SaveFile(path, data, e.size);
        SDL_free(data);
        if (!ok)
            goto io;
    }
    return 0;
io:
    *why = SDL_GetError();
    return -1;
}

void np_mods_install(np_app *app, const char *zip_path)
{
    size_t len;
    void *data = SDL_LoadFile(zip_path, &len);
    np_zip z;
    char prefix[512], name[NP_MOD_ID_MAX];
    np_mod_info info;
    const char *why = "cannot read the file";
    char root[1100], tmp[1200], dest[1200];
    mods_dir(root, sizeof root);
    if (!data)
        goto fail;
    why = "not a zip archive";
    if (np_zip_open(&z, data, len) || find_package_root(&z, prefix, sizeof prefix, name, sizeof name, &info, &why))
        goto fail;
    SDL_snprintf(tmp, sizeof tmp, "%s/.installing-%s", root, name);
    SDL_snprintf(dest, sizeof dest, "%s/%s", root, name);
    np_storage_remove_tree(tmp);
    if (!SDL_CreateDirectory(tmp)) {
        why = SDL_GetError();
        goto fail;
    }
    if (extract_all(&z, prefix, tmp, &why)) {
        np_storage_remove_tree(tmp);
        goto fail;
    }
    if (np_storage_remove_tree(dest) || !SDL_RenamePath(tmp, dest)) {
        why = SDL_GetError();
        np_storage_remove_tree(tmp);
        goto fail;
    }
    SDL_free(data);
    scan();
    for (int i = 0; i < ms.count; i++)
        if (!SDL_strcmp(ms.pkg[i].dir, name) && !ms.pkg[i].enabled) {
            /* New packages start enabled, loaded last. */
            mod_pkg p = ms.pkg[i];
            p.enabled = 1;
            int last = 0;
            while (last < ms.count && ms.pkg[last].enabled)
                last++;
            SDL_memmove(&ms.pkg[last + 1], &ms.pkg[last], (size_t)(i - last) * sizeof p);
            ms.pkg[last] = p;
        }
    save_order(app);
    scan();
    np_app_toast(app, "Installed %s %s", info.name, info.version);
    SDL_Log("mods: installed %s as %s", zip_path, name);
    return;
fail:
    SDL_free(data);
    np_app_toast(app, "Cannot install: %s", why);
    SDL_Log("mods: install %s failed: %s", zip_path, why);
}

/* ---- page ---------------------------------------------------------------------- */

enum { ROW_INSTALL = 0, ROW_FOLDER, ROW_FIXED };

void np_mods_open(np_app *app, const char *banner)
{
    char root[1100];
    mods_dir(root, sizeof root);
    SDL_CreateDirectory(root);
    scan();
    SDL_strlcpy(ms.banner, banner ? banner : "", sizeof ms.banner);
    np_app_open_page(app, NP_PAGE_MODS);
    app->sel = 0;
    /* After a boot error, select the package it names ('pkg'). */
    const char *q = banner ? SDL_strchr(banner, '\'') : NULL;
    if (q) {
        char name[NP_MOD_ID_MAX];
        size_t k = 0;
        for (q++; *q && *q != '\'' && k + 1 < sizeof name; q++)
            name[k++] = *q;
        name[k] = '\0';
        for (int i = 0; i < ms.count; i++)
            if (!SDL_strcmp(ms.pkg[i].dir, name) || !SDL_strcmp(ms.pkg[i].info.id, name))
                app->sel = i;
    }
}

void np_mods_boot_failed(np_app *app, const char *error)
{
    if (!SDL_strstr(error, "modfs:"))
        return;
    char msg[256];
    SDL_snprintf(msg, sizeof msg, "The game did not start: %s. Turn the package off or fix it.",
                 SDL_strstr(error, "modfs:") + 7);
    np_mods_open(app, msg);
}

static void toggle(np_app *app, int i)
{
    mod_pkg p = ms.pkg[i];
    if (!p.enabled && (!p.valid || !p.cooked)) {
        np_app_toast(app, "Cannot enable: %s", p.problem);
        return;
    }
    /* Enabling appends to the load order; disabling moves it below. */
    SDL_memmove(&ms.pkg[i], &ms.pkg[i + 1], (size_t)(ms.count - i - 1) * sizeof p);
    int at = 0;
    while (at < ms.count - 1 && ms.pkg[at].enabled)
        at++;
    p.enabled = !p.enabled;
    SDL_memmove(&ms.pkg[at + 1], &ms.pkg[at], (size_t)(ms.count - 1 - at) * sizeof p);
    ms.pkg[at] = p;
    app->sel = at;
    save_order(app);
    scan();
}

static void move(np_app *app, int i, int dir)
{
    int j = i + dir;
    if (i < 0 || i >= ms.count || !ms.pkg[i].enabled || j < 0 || j >= ms.count || !ms.pkg[j].enabled)
        return;
    mod_pkg t = ms.pkg[i];
    ms.pkg[i] = ms.pkg[j];
    ms.pkg[j] = t;
    app->sel = j;
    save_order(app);
    scan();
}

static void remove_pkg(np_app *app, int i)
{
    uint64_t now = SDL_GetTicksNS();
    if (now >= ms.remove_armed) {
        ms.remove_armed = now + 3 * SDL_NS_PER_SECOND;
        np_app_toast(app, "Press X again to delete \"%s\" from the mods folder", ms.pkg[i].info.name);
        return;
    }
    ms.remove_armed = 0;
    char path[1200];
    pkg_path(ms.pkg[i].dir, "", path, sizeof path);
    if (np_storage_remove_tree(path)) {
        np_app_toast(app, "Cannot delete it: %s", SDL_GetError());
        return;
    }
    np_app_toast(app, "Deleted \"%s\"", ms.pkg[i].info.name);
    ms.pkg[i].enabled = 0;
    save_order(app);
    scan();
}

static void activate(np_app *app, int row)
{
    if (row < ms.count) {
        toggle(app, row);
        return;
    }
    switch (row - ms.count) {
    case ROW_INSTALL: np_app_open_mod_install_dialog(app); break;
    case ROW_FOLDER: {
        char root[1100], url[1200];
        mods_dir(root, sizeof root);
        SDL_snprintf(url, sizeof url, "file://%s", root);
        if (!SDL_OpenURL(url))
            np_app_toast(app, "Cannot open %s: %s", root, SDL_GetError());
        break;
    }
    default: break;
    }
}

void np_mods_command(np_app *app, np_menu_cmd cmd)
{
    int rows = ms.count + ROW_FIXED;
    switch (cmd) {
    case NP_CMD_UP: app->sel = (app->sel + rows - 1) % rows; break;
    case NP_CMD_DOWN: app->sel = (app->sel + 1) % rows; break;
    case NP_CMD_LEFT: move(app, app->sel, -1); break;
    case NP_CMD_RIGHT: move(app, app->sel, 1); break;
    case NP_CMD_CONFIRM: activate(app, app->sel); break;
    case NP_CMD_X:
        if (app->sel < ms.count)
            remove_pkg(app, app->sel);
        break;
    default: break;
    }
}

void np_mods_hit(np_app *app, int id) { activate(app, id); }

void np_mods_draw(np_app *app)
{
    static const SDL_Color white = {235, 235, 235, 255}, dim = {150, 150, 160, 255}, accent = {255, 205, 80, 255},
                           warn = {255, 120, 100, 255}, good = {140, 220, 140, 255};
    np_page_frame f;
    np_ui_begin_page(app, &f, "Mods (Platinum)");
    float x = f.panel.x + 2 * f.cw, y = f.list_y;
    if (ms.banner[0]) {
        y += (float)np_ui_text_wrap(app, x, y, f.s, f.lh, f.cols - 4, ms.banner, warn, 1) * f.lh;
    } else if (app->view == NP_VIEW_GAME && ms.changed) {
        np_ui_text_clip(app, x, y, f.s, "Changes apply when the game restarts (F2 twice reloads it).", f.cols - 4,
                        accent);
        y += f.lh;
    }
    if (!ms.count) {
        np_ui_text_clip(app, x, y, f.s, "No packages yet. Install a .zip, or copy package folders into the mods folder.",
                        f.cols - 4, dim);
        y += f.lh;
    }
    int rows = ms.count + ROW_FIXED;
    app->sel = SDL_clamp(app->sel, 0, rows - 1);
    int visible = (int)((f.list_y + (float)f.rows * f.lh - y) / f.lh);
    if (visible < 1)
        visible = 1;
    if (app->sel < app->scroll)
        app->scroll = app->sel;
    if (app->sel >= app->scroll + visible)
        app->scroll = app->sel - visible + 1;
    for (int r = 0; r < visible && app->scroll + r < rows; r++) {
        int i = app->scroll + r, sel = i == app->sel;
        float ry = y + (float)r * f.lh;
        SDL_FRect row = {f.panel.x + f.cw, ry - 2 * f.s, f.panel.w - 2 * f.cw, f.lh};
        if (sel)
            np_ui_fill(app, row, (SDL_Color){255, 205, 80, 40});
        char text[200];
        if (i < ms.count) {
            const mod_pkg *p = &ms.pkg[i];
            SDL_snprintf(text, sizeof text, "%s %s %s", p->enabled ? "[on] " : "[off]", p->info.name,
                         p->valid ? p->info.version : "");
            np_ui_text_clip(app, x, ry, f.s, text, f.cols / 2, sel ? accent : p->enabled ? white : dim);
            if (p->problem[0])
                np_ui_text_clip(app, x + (float)(f.cols / 2 + 1) * f.cw, ry, f.s, p->problem, f.cols / 2 - 4, warn);
            else if (p->enabled)
                np_ui_text_clip(app, x + (float)(f.cols / 2 + 1) * f.cw, ry, f.s, "ready", f.cols / 2 - 4, good);
        } else {
            static const char *const fixed[ROW_FIXED] = {"Install package (.zip)...", "Open mods folder"};
            np_ui_text_clip(app, x, ry, f.s, fixed[i - ms.count], f.cols - 4, sel ? accent : white);
        }
        np_ui_hit(app, row, i);
    }
    np_ui_end_page(app, &f, "Enter: on/off  Left/Right: order  X: delete", 0);
}
