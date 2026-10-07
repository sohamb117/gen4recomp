/*
 * Controller skins (Options > Controller skin): Delta's .deltaskin format
 * for the DS and the GBA (skinfmt.h). Importing unpacks info.json and the
 * art it names into <user data>/skins/<name>/; each console has its own
 * chosen skin (options skin / skin_gba), which replaces the built-in touch
 * controls for every orientation it covers while a game of that console
 * runs: its art fills the window (aspect-fitted), the game's screens go to
 * its screen frames (the layout code places them, so the stylus maps
 * through them as usual) and touches on its items press buttons or run its
 * actions.
 *
 * PNG art loads through SDL; PDF art ("resizable") is rasterized for the
 * window's size where a PDF renderer exists (pdfraster.h) and redrawn when
 * the window changes size.
 */
#include "app.h"

#include "pdfraster.h"
#include "romdb.h"
#include "skinfmt.h"
#include "slots.h"
#include "zip.h"

typedef struct skin_state {
    int active; /* a skin is loaded */
    char dir[NP_SLOT_NAME_MAX + 1];
    np_skin_def def;
    SDL_Texture *art[2];
    int art_w[2], art_h[2]; /* size the art was rasterized for (PDF) */
    void *pdf[2];           /* PDF bytes, kept for re-rasterizing */
    size_t pdf_len[2];
} skin_state;

static skin_state sk;

static void skins_root(char *out, size_t n) { np_storage_path(out, n, "skins"); }

static void skin_file(const char *dir, const char *name, char *out, size_t n)
{
    char root[1100];
    skins_root(root, sizeof root);
    SDL_snprintf(out, n, "%s/%s/%s", root, dir, name);
}

static void unload(void)
{
    for (int o = 0; o < 2; o++) {
        if (sk.art[o])
            SDL_DestroyTexture(sk.art[o]);
        SDL_free(sk.pdf[o]);
        sk.art[o] = NULL;
        sk.pdf[o] = NULL;
        sk.art_w[o] = sk.art_h[o] = 0;
    }
    sk.active = 0;
}

static int load_def(const char *dir, np_skin_def *def, const char **why)
{
    char path[1300];
    skin_file(dir, "info.json", path, sizeof path);
    size_t len;
    char *json = SDL_LoadFile(path, &len);
    if (!json) {
        *why = "info.json is missing";
        return -1;
    }
    int r = np_skin_parse(json, len, def, why);
    SDL_free(json);
    return r;
}

/* Loads the active skin's art for orientation o (PNG now, PDF on demand). */
static void load_art(np_app *app, int o)
{
    const np_skin_rep *rep = &sk.def.rep[o];
    if (!rep->present)
        return;
    char path[1300];
    skin_file(sk.dir, rep->asset, path, sizeof path);
    if (rep->asset_is_pdf) {
        sk.pdf[o] = SDL_LoadFile(path, &sk.pdf_len[o]);
        return;
    }
    SDL_Surface *s = SDL_LoadPNG(path);
    if (!s) {
        SDL_Log("skin: %s: %s", path, SDL_GetError());
        return;
    }
    sk.art[o] = SDL_CreateTextureFromSurface(app->renderer, s);
    SDL_DestroySurface(s);
    if (sk.art[o])
        SDL_SetTextureScaleMode(sk.art[o], SDL_SCALEMODE_LINEAR);
}

static int running_gba(const np_app *app) { return np_game_is_gba(app->game); }

/* The chosen skin for the running game's console. */
static char *chosen(np_app *app) { return running_gba(app) ? app->opt.skin_gba : app->opt.skin; }

void np_skin_apply(np_app *app)
{
    unload();
    const char *name = chosen(app);
    if (!name[0] || !app->renderer)
        return;
    const char *why = "";
    if (load_def(name, &sk.def, &why)) {
        SDL_Log("skin \"%s\" not loaded: %s", name, why);
        return;
    }
    if (sk.def.gba != running_gba(app)) {
        SDL_Log("skin \"%s\" not loaded: it is a %s skin", name, sk.def.gba ? "GBA" : "DS");
        return;
    }
    SDL_strlcpy(sk.dir, name, sizeof sk.dir);
    for (int o = 0; o < 2; o++)
        load_art(app, o);
    sk.active = 1;
    SDL_Log("skin: %s (%s, %s%s)", sk.def.name, sk.def.gba ? "GBA" : "DS", sk.def.rep[0].present ? "landscape " : "",
            sk.def.rep[1].present ? "portrait" : "");
}

/* The representation for the window's orientation, or NULL (none loaded,
 * or loaded for the other console). */
static const np_skin_rep *current(const np_app *app, int *orientation)
{
    if (!sk.active || sk.def.gba != running_gba(app))
        return NULL;
    int o = app->out_h > app->out_w;
    if (orientation)
        *orientation = o;
    return sk.def.rep[o].present ? &sk.def.rep[o] : NULL;
}

/* Mapping points -> window pixels: aspect-fit and centred. */
static void transform(const np_app *app, const np_skin_rep *r, float *sc, float *ox, float *oy)
{
    *sc = SDL_min(app->out_w / r->map_w, app->out_h / r->map_h);
    *ox = (app->out_w - r->map_w * *sc) * 0.5f;
    *oy = (app->out_h - r->map_h * *sc) * 0.5f;
}

int np_skin_active(const np_app *app) { return current(app, NULL) != NULL; }

int np_skin_frames(const np_app *app, float frames[8])
{
    const np_skin_rep *r = current(app, NULL);
    if (!r)
        return 0;
    float sc, ox, oy;
    transform(app, r, &sc, &ox, &oy);
    SDL_memset(frames, 0, 8 * sizeof *frames);
    for (int i = 0; i < r->nscreens; i++) {
        /* DS: top above bottom in the 256x384 output; GBA: one screen */
        int which = !sk.def.gba && r->screen[i].input.y >= 192.0f ? 1 : 0;
        float *f = frames + 4 * which;
        f[0] = ox + r->screen[i].output.x * sc;
        f[1] = oy + r->screen[i].output.y * sc;
        f[2] = r->screen[i].output.w * sc;
        f[3] = r->screen[i].output.h * sc;
    }
    return 1;
}

void np_skin_draw_art(np_app *app)
{
    int o;
    const np_skin_rep *r = current(app, &o);
    if (!r)
        return;
    float sc, ox, oy;
    transform(app, r, &sc, &ox, &oy);
    int w = (int)(r->map_w * sc + 0.5f), h = (int)(r->map_h * sc + 0.5f);
#ifdef NP_HAVE_PDF
    if (sk.pdf[o] && (!sk.art[o] || SDL_abs(sk.art_w[o] - w) > w / 10 || SDL_abs(sk.art_h[o] - h) > h / 10)) {
        SDL_Surface *s = np_pdf_render(sk.pdf[o], sk.pdf_len[o], w, h);
        if (s) {
            if (sk.art[o])
                SDL_DestroyTexture(sk.art[o]);
            sk.art[o] = SDL_CreateTextureFromSurface(app->renderer, s);
            SDL_DestroySurface(s);
            if (sk.art[o])
                SDL_SetTextureBlendMode(sk.art[o], SDL_BLENDMODE_BLEND_PREMULTIPLIED);
            sk.art_w[o] = w;
            sk.art_h[o] = h;
        }
    }
#endif
    if (sk.art[o])
        SDL_RenderTexture(app->renderer, sk.art[o], NULL, &(SDL_FRect){ox, oy, (float)w, (float)h});
}

/* Lights the pressed items so presses are visible on skins whose art has
 * no pressed state. */
void np_skin_draw_pressed(np_app *app, uint16_t keys)
{
    const np_skin_rep *r = current(app, NULL);
    if (!r || !keys)
        return;
    float sc, ox, oy;
    transform(app, r, &sc, &ox, &oy);
    for (int i = 0; i < r->nitems; i++) {
        const np_skin_item *it = &r->item[i];
        uint16_t mask = it->action == NP_SKIN_DPAD ? (NP_KEY_UP | NP_KEY_DOWN | NP_KEY_LEFT | NP_KEY_RIGHT)
                        : it->action == NP_SKIN_KEYS ? it->keys
                                                     : 0;
        if (keys & mask)
            np_ui_fill(app, (SDL_FRect){ox + it->frame.x * sc, oy + it->frame.y * sc, it->frame.w * sc, it->frame.h * sc},
                       (SDL_Color){255, 255, 255, 60});
    }
}

int np_skin_hit(const np_app *app, float x, float y, np_touch_hit *h)
{
    const np_skin_rep *r = current(app, NULL);
    if (!r)
        return 0;
    float sc, ox, oy;
    transform(app, r, &sc, &ox, &oy);
    float mx = (x - ox) / sc, my = (y - oy) / sc;
    int i = np_skin_item_at(r, mx, my);
    if (i < 0 || r->item[i].action == NP_SKIN_TOUCH_SCREEN)
        return 1; /* the skin covers the window: not a built-in control */
    const np_skin_item *it = &r->item[i];
    h->any = 1;
    switch (it->action) {
    case NP_SKIN_KEYS: h->keys |= it->keys; break;
    case NP_SKIN_DPAD: h->keys |= np_skin_dpad_keys(it, mx, my); break;
    case NP_SKIN_MENU: h->menu = 1; break;
    case NP_SKIN_FF_HOLD: h->ff_hold = 1; break;
    case NP_SKIN_FF_TOGGLE: h->ff_toggle = 1; break;
    case NP_SKIN_QUICK_SAVE: h->quick_save = 1; break;
    case NP_SKIN_QUICK_LOAD: h->quick_load = 1; break;
    default: break;
    }
    return 1;
}

const char *np_skin_name(void) { return sk.active ? sk.def.name : NULL; }

/* ---- installed skins ---------------------------------------------------------- */

typedef struct dir_list {
    char names[64][NP_SLOT_NAME_MAX + 1];
    int count;
} dir_list;

static SDL_EnumerationResult SDLCALL collect(void *user, const char *dirname, const char *fname)
{
    dir_list *l = user;
    char path[1300];
    SDL_snprintf(path, sizeof path, "%s%s", dirname, fname);
    SDL_PathInfo info;
    if (fname[0] != '.' && l->count < 64 && SDL_strlen(fname) <= NP_SLOT_NAME_MAX && SDL_GetPathInfo(path, &info) &&
        info.type == SDL_PATHTYPE_DIRECTORY)
        SDL_strlcpy(l->names[l->count++], fname, NP_SLOT_NAME_MAX + 1);
    return SDL_ENUM_CONTINUE;
}

static int cmp(const void *a, const void *b) { return SDL_strcasecmp(a, b); }

/* Next/previous installed skin of the running game's console, or none. */
void np_skin_cycle(np_app *app, int dir)
{
    static dir_list l;
    l.count = 0;
    char root[1100];
    skins_root(root, sizeof root);
    SDL_EnumerateDirectory(root, collect, &l);
    np_skin_def *def = SDL_malloc(sizeof *def);
    int n = 0;
    for (int i = 0; i < l.count; i++) {
        const char *why;
        if (def && !load_def(l.names[i], def, &why) && def->gba == running_gba(app))
            SDL_memmove(l.names[n++], l.names[i], sizeof l.names[0]);
    }
    SDL_free(def);
    l.count = n;
    SDL_qsort(l.names, (size_t)l.count, sizeof l.names[0], cmp);
    char *opt = chosen(app);
    int at = -1; /* -1 = none */
    for (int i = 0; i < l.count; i++)
        if (!SDL_strcmp(l.names[i], opt))
            at = i;
    n = l.count + 1;
    at = ((at + 1 + dir) % n + n) % n - 1;
    SDL_strlcpy(opt, at < 0 ? "" : l.names[at], sizeof app->opt.skin);
    app->options_dirty = 1;
    np_skin_apply(app);
}

void np_skin_install(np_app *app, const char *path)
{
    size_t len;
    void *data = SDL_LoadFile(path, &len);
    np_zip z;
    np_zip_entry e;
    np_skin_def *def = SDL_malloc(sizeof *def);
    char *json = NULL;
    const char *why = "cannot read the file";
    char root[1100], dest[1300], tmp[1300], dir[NP_SLOT_NAME_MAX + 1];
    if (!data || !def)
        goto fail;
    why = "not a .deltaskin (zip) file";
    if (np_zip_open(&z, data, len) || np_zip_find(&z, "info.json", &e) < 0 || e.size > (4u << 20))
        goto fail;
    json = SDL_malloc(e.size ? e.size : 1);
    if (!json || np_zip_extract(&z, &e, json)) {
        why = "info.json is damaged";
        goto fail;
    }
    if (np_skin_parse(json, e.size, def, &why))
        goto fail;
    /* The folder is named after the skin, made safe as a file name. */
    np_slot_sanitize(def->name, "Skin", dir);
    skins_root(root, sizeof root);
    SDL_CreateDirectory(root);
    SDL_snprintf(dest, sizeof dest, "%s/%s", root, dir);
    SDL_snprintf(tmp, sizeof tmp, "%s/.installing", root);
    SDL_RemovePath(tmp); /* leftovers of an interrupted install are files only */
    if (!SDL_CreateDirectory(tmp)) {
        why = SDL_GetError();
        goto fail;
    }
    /* info.json plus every art file a representation names (only plain
     * file names pass the parser, so nothing lands outside the folder). */
    char file[1400];
    SDL_snprintf(file, sizeof file, "%s/info.json", tmp);
    int ok = SDL_SaveFile(file, json, e.size);
    for (int o = 0; ok && o < 2; o++) {
        const np_skin_rep *rep = &def->rep[o];
        if (!rep->present || (o == 1 && def->rep[0].present && !SDL_strcmp(rep->asset, def->rep[0].asset)))
            continue;
#ifndef NP_HAVE_PDF
        if (rep->asset_is_pdf) {
            why = "its art is PDF, which this platform cannot draw";
            ok = 0;
            break;
        }
#endif
        np_zip_entry a;
        void *buf = NULL;
        if (np_zip_find(&z, rep->asset, &a) < 0 || a.size > (64u << 20) || !(buf = SDL_malloc(a.size ? a.size : 1)) ||
            np_zip_extract(&z, &a, buf)) {
            SDL_free(buf);
            why = "the skin's art file is missing or damaged";
            ok = 0;
            break;
        }
        SDL_snprintf(file, sizeof file, "%s/%s", tmp, rep->asset);
        ok = SDL_SaveFile(file, buf, a.size);
        SDL_free(buf);
        if (!ok)
            why = SDL_GetError();
    }
    if (!ok)
        goto cleanup;
    /* Replace an earlier install of the same skin. */
    for (int o = 0; o < 2; o++) {
        if (!def->rep[o].present)
            continue;
        SDL_snprintf(file, sizeof file, "%s/%s", dest, def->rep[o].asset);
        SDL_RemovePath(file);
    }
    SDL_snprintf(file, sizeof file, "%s/info.json", dest);
    SDL_RemovePath(file);
    SDL_RemovePath(dest); /* only empty now; a non-empty one keeps stray files */
    if (!SDL_RenamePath(tmp, dest)) {
        why = SDL_GetError();
        goto cleanup;
    }
    SDL_strlcpy(def->gba ? app->opt.skin_gba : app->opt.skin, dir, sizeof app->opt.skin);
    app->options_dirty = 1;
    np_skin_apply(app);
    np_app_toast(app, "Skin \"%s\" installed and selected", def->name);
    SDL_Log("skin: installed %s as %s", path, dir);
    SDL_free(json);
    SDL_free(def);
    SDL_free(data);
    return;
cleanup:
    for (int o = 0; o < 2; o++)
        if (def->rep[o].present) {
            SDL_snprintf(file, sizeof file, "%s/%s", tmp, def->rep[o].asset);
            SDL_RemovePath(file);
        }
    SDL_snprintf(file, sizeof file, "%s/info.json", tmp);
    SDL_RemovePath(file);
    SDL_RemovePath(tmp);
fail:
    np_app_toast(app, "Cannot use this skin: %s", why);
    SDL_Log("skin: %s: %s", path, why);
    SDL_free(json);
    SDL_free(def);
    SDL_free(data);
}

void np_skin_shutdown(void) { unload(); }
