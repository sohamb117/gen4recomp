/*
 * Custom carts: a sealed, named set of mod packages (Gen1Recomp's custom
 * carts). Sealing records the enabled packages in load order and a SHA-256
 * over each package's name, mod.toml and cooked digest, in
 * <user data>/carts/<game>/<name>.cart (a cart holds one game's packages). A save slot can be bound to a cart
 * (saves/<game>/<slot>.cart); booting it loads exactly that set, and refuses
 * to start if any package changed since sealing, so a playthrough never
 * silently runs on different content.
 *
 * The same hash pins link play: every boot derives a realm from the active
 * set (the cart's, or the loose loadorder.txt set) and local wireless only
 * meets stations of the same realm (net.c), so modded and vanilla games,
 * or games with different mods, never trade or battle.
 */
#include "app.h"

#include "romdb.h"
#include "sha256.h"

static void carts_dir(np_game game, char *out, size_t n)
{
    char rel[32];
    SDL_snprintf(rel, sizeof rel, "carts/%s", np_game_id(game));
    np_storage_path(out, n, rel);
}

static void cart_path(np_game game, const char *name, char *out, size_t n)
{
    char dir[1100];
    carts_dir(game, dir, sizeof dir);
    SDL_snprintf(out, n, "%s/%s.cart", dir, name);
}

/* Hash of packages `dirs` (in order): each name, mod.toml and .cooked/digest
 * with separators. Returns 0, or -1 naming the package that is missing. */
static int set_hash(np_game game, char (*dirs)[NP_MOD_ID_MAX], int n, uint8_t out[32], char *why, size_t whyn)
{
    np_sha256 h;
    np_sha256_init(&h);
    char root[1100];
    if (np_mods_content_root(NULL, game, root, sizeof root)) {
        SDL_snprintf(why, whyn, "no %s mods folder", np_game_title(game));
        return -1;
    }
    for (int i = 0; i < n; i++) {
        static const char *const files[2] = {"mod.toml", ".cooked/digest"};
        np_sha256_update(&h, dirs[i], SDL_strlen(dirs[i]) + 1);
        for (int f = 0; f < 2; f++) {
            char path[1300];
            SDL_snprintf(path, sizeof path, "%s/%s/%s", root, dirs[i], files[f]);
            size_t len;
            void *data = SDL_LoadFile(path, &len);
            if (!data) {
                SDL_snprintf(why, whyn, "package '%s' is missing or not cooked", dirs[i]);
                return -1;
            }
            np_sha256_update(&h, data, len);
            np_sha256_update(&h, "", 1);
            SDL_free(data);
        }
    }
    np_sha256_final(&h, out);
    return 0;
}

typedef struct cart {
    char name[NP_SLOT_NAME_MAX + 1];
    char pkg[NP_CART_MAX_PKGS][NP_MOD_ID_MAX];
    int npkg;
    char hex[65];
} cart;

static int load_cart(np_game game, const char *name, cart *c)
{
    char path[1200];
    cart_path(game, name, path, sizeof path);
    char *text = SDL_LoadFile(path, NULL);
    if (!text)
        return -1;
    SDL_zerop(c);
    SDL_strlcpy(c->name, name, sizeof c->name);
    char *save = NULL;
    for (char *line = SDL_strtok_r(text, "\r\n", &save); line; line = SDL_strtok_r(NULL, "\r\n", &save)) {
        char *eq = SDL_strchr(line, '=');
        if (line[0] == '#' || !eq)
            continue;
        char *v = eq + 1;
        while (*v == ' ')
            v++;
        if (!SDL_strncmp(line, "package", 7) && c->npkg < NP_CART_MAX_PKGS)
            SDL_strlcpy(c->pkg[c->npkg++], v, NP_MOD_ID_MAX);
        else if (!SDL_strncmp(line, "sha256", 6))
            SDL_strlcpy(c->hex, v, sizeof c->hex);
    }
    SDL_free(text);
    return SDL_strlen(c->hex) == 64 ? 0 : -1;
}

typedef struct cart_list {
    char (*names)[NP_SLOT_NAME_MAX + 1];
    int count, max;
} cart_list;

static SDL_EnumerationResult SDLCALL collect(void *user, const char *dirname, const char *fname)
{
    (void)dirname;
    cart_list *l = user;
    size_t len = SDL_strlen(fname);
    if (l->count < l->max && len > 5 && len - 5 <= NP_SLOT_NAME_MAX && !SDL_strcmp(fname + len - 5, ".cart")) {
        SDL_memcpy(l->names[l->count], fname, len - 5);
        l->names[l->count++][len - 5] = '\0';
    }
    return SDL_ENUM_CONTINUE;
}

static int cmp(const void *a, const void *b) { return SDL_strcasecmp(a, b); }

int np_carts_list(np_game game, char (*names)[NP_SLOT_NAME_MAX + 1], int max)
{
    cart_list l = {names, 0, max};
    char dir[1100];
    carts_dir(game, dir, sizeof dir);
    SDL_EnumerateDirectory(dir, collect, &l);
    SDL_qsort(names, (size_t)l.count, sizeof names[0], cmp);
    return l.count;
}

int np_cart_seal(np_app *app, np_game game, const char *name)
{
    cart c;
    SDL_zero(c);
    c.npkg = np_mods_enabled(game, c.pkg, NP_CART_MAX_PKGS);
    if (c.npkg <= 0) {
        np_app_toast(app, c.npkg < 0 ? "Fix the packages marked in red first" : "Enable the packages to seal first");
        return -1;
    }
    uint8_t digest[32];
    char why[128];
    if (set_hash(game, c.pkg, c.npkg, digest, why, sizeof why)) {
        np_app_toast(app, "Cannot seal: %s", why);
        return -1;
    }
    np_sha256_hex(digest, c.hex);
    char text[NP_CART_MAX_PKGS * (NP_MOD_ID_MAX + 12) + 256];
    size_t len = (size_t)SDL_snprintf(text, sizeof text,
                                      "# nativeplat custom cart: these packages in this order, sealed.\nname = %s\n", name);
    for (int i = 0; i < c.npkg; i++)
        len += (size_t)SDL_snprintf(text + len, sizeof text - len, "package = %s\n", c.pkg[i]);
    len += (size_t)SDL_snprintf(text + len, sizeof text - len, "sha256 = %s\n", c.hex);
    char dir[1100], path[1200];
    np_storage_path(dir, sizeof dir, "carts");
    SDL_CreateDirectory(dir);
    carts_dir(game, dir, sizeof dir);
    SDL_CreateDirectory(dir);
    cart_path(game, name, path, sizeof path);
    if (np_storage_write_atomic(path, text, len, 0)) {
        np_app_toast(app, "Cannot save the cart: %s", SDL_GetError());
        return -1;
    }
    np_app_toast(app, "Sealed cart \"%s\" (%d package%s, %.8s)", name, c.npkg, c.npkg == 1 ? "" : "s", c.hex);
    SDL_Log("carts: sealed %s: %s", name, c.hex);
    return 0;
}

int np_cart_delete(np_game game, const char *name)
{
    char path[1200];
    cart_path(game, name, path, sizeof path);
    return SDL_RemovePath(path) ? 0 : -1;
}

int np_cart_describe(np_game game, const char *name, char *out, size_t n)
{
    cart c;
    if (load_cart(game, name, &c))
        return -1;
    SDL_snprintf(out, n, "%d package%s, %.8s", c.npkg, c.npkg == 1 ? "" : "s", c.hex);
    return 0;
}

int np_carts_for_boot(np_app *app, np_game game, const char *slot, char *pc_mods, size_t n, uint32_t *realm)
{
    pc_mods[0] = '\0';
    *realm = 0;
    char bound[NP_SLOT_NAME_MAX + 1];
    uint8_t digest[32];
    char why[128];
    if (np_storage_slot_cart(game, slot, bound, sizeof bound) == 0) {
        cart c;
        if (load_cart(game, bound, &c)) {
            SDL_snprintf(app->status, sizeof app->status, "This slot uses cart \"%s\", which is missing.", bound);
            return -1;
        }
        if (set_hash(game, c.pkg, c.npkg, digest, why, sizeof why)) {
            SDL_snprintf(app->status, sizeof app->status, "Cart \"%s\": %s.", bound, why);
            return -1;
        }
        char hex[65];
        np_sha256_hex(digest, hex);
        if (SDL_strcmp(hex, c.hex)) {
            SDL_snprintf(app->status, sizeof app->status,
                         "Cart \"%s\" no longer matches its packages (one changed since sealing). Seal it again or "
                         "pick another cart for this slot.",
                         bound);
            return -1;
        }
        size_t len = (size_t)SDL_snprintf(pc_mods, n, "PC_MODS=");
        for (int i = 0; i < c.npkg; i++)
            len += (size_t)SDL_snprintf(pc_mods + len, n > len ? n - len : 0, "%s%s", i ? " " : "", c.pkg[i]);
        SDL_Log("carts: \"%s\" boots with cart %s (%.16s)", slot, bound, hex);
    } else {
        /* No cart: the loose set the core reads from loadorder.txt. */
        char pkg[NP_CART_MAX_PKGS][NP_MOD_ID_MAX];
        int npkg = np_mods_enabled(game, pkg, NP_CART_MAX_PKGS);
        if (npkg <= 0 || set_hash(game, pkg, npkg, digest, why, sizeof why))
            return 0; /* vanilla (or broken: the core explains at boot) */
    }
    *realm = (uint32_t)digest[0] << 24 | (uint32_t)digest[1] << 16 | (uint32_t)digest[2] << 8 | digest[3];
    if (!*realm)
        *realm = 1; /* 0 means vanilla */
    return 0;
}
