/*
 * File I/O goes through SDL_IOStream and SDL's filesystem API so the same
 * code runs on macOS, iOS and Windows (SDL_RenamePath replaces the target
 * atomically on all three).
 */
#include "storage.h"

#include <SDL3/SDL.h>
#include <string.h>

#include "romdb.h"
#include "sha1.h"

#define IO_CHUNK (1u << 20)

static char root[1024];
static int portable;

static int dir_has_portable_marker(const char *dir)
{
    char p[1024];
    SDL_snprintf(p, sizeof p, "%sportable.txt", dir);
    return np_storage_exists(p);
}

/* Portable root candidates: the base path, and for a macOS/iOS bundle (whose
 * base path is <x>.app/Contents/Resources/) the folder holding the bundle,
 * which is where a player would actually drop portable.txt. */
static int find_portable_root(char *out, size_t n)
{
    const char *base = SDL_GetBasePath();
    if (!base || !*base)
        return 0;
    size_t len = strlen(base);
    char sep = base[len - 1];
    if (dir_has_portable_marker(base)) {
        SDL_snprintf(out, n, "%suserdata%c", base, sep);
        return 1;
    }
    char dir[1024];
    SDL_strlcpy(dir, base, sizeof dir);
    char *app = strstr(dir, ".app/Contents/");
    if (!app)
        app = strstr(dir, ".app/");
    if (app) {
        *app = '\0';
        char *slash = strrchr(dir, '/');
        if (slash) {
            slash[1] = '\0';
            if (dir_has_portable_marker(dir)) {
                SDL_snprintf(out, n, "%suserdata/", dir);
                return 1;
            }
        }
    }
    return 0;
}

int np_storage_init(int require_portable, char *err, size_t errn)
{
    portable = find_portable_root(root, sizeof root);
    if (portable) {
        if (!SDL_CreateDirectory(root)) {
            SDL_snprintf(err, errn, "cannot create %s: %s", root, SDL_GetError());
            return -1;
        }
    } else if (require_portable) {
        SDL_snprintf(err, errn, "portable mode required: put portable.txt beside the executable");
        return -1;
    } else {
        char *pref = SDL_GetPrefPath("nativeplat", "nativeplat");
        if (!pref) {
            SDL_snprintf(err, errn, "no user data directory: %s", SDL_GetError());
            return -1;
        }
        SDL_strlcpy(root, pref, sizeof root);
        SDL_free(pref);
    }
    static const char *const subdirs[] = {"roms", "saves", "screenshots"};
    for (size_t i = 0; i < sizeof subdirs / sizeof subdirs[0]; i++) {
        char p[1100];
        np_storage_path(p, sizeof p, subdirs[i]);
        if (!SDL_CreateDirectory(p)) {
            SDL_snprintf(err, errn, "cannot create %s: %s", p, SDL_GetError());
            return -1;
        }
    }
    return 0;
}

const char *np_storage_root(void) { return root; }
int np_storage_is_portable(void) { return portable; }

void np_storage_path(char *out, size_t n, const char *rel) { SDL_snprintf(out, n, "%s%s", root, rel); }

int np_storage_exists(const char *path)
{
    SDL_PathInfo info;
    return SDL_GetPathInfo(path, &info) && info.type == SDL_PATHTYPE_FILE;
}

static int write_whole(const char *path, const void *data, size_t len)
{
    SDL_IOStream *io = SDL_IOFromFile(path, "wb");
    if (!io)
        return -1;
    int ok = SDL_WriteIO(io, data, len) == len && SDL_FlushIO(io);
    if (!SDL_CloseIO(io))
        ok = 0;
    return ok ? 0 : -1;
}

int np_storage_write_atomic(const char *path, const void *data, size_t len, int keep_backup)
{
    char tmp[1100], bak[1100];
    SDL_snprintf(tmp, sizeof tmp, "%s.tmp", path);
    if (write_whole(tmp, data, len)) {
        SDL_RemovePath(tmp);
        return -1;
    }
    if (keep_backup && np_storage_exists(path)) {
        SDL_snprintf(bak, sizeof bak, "%s.bak", path);
        if (!SDL_RenamePath(path, bak)) {
            SDL_RemovePath(tmp);
            return -1;
        }
    }
    if (!SDL_RenamePath(tmp, path)) {
        SDL_RemovePath(tmp);
        return -1;
    }
    return 0;
}

void np_storage_rom_path(np_game game, char *out, size_t n)
{
    char rel[64];
    SDL_snprintf(rel, sizeof rel, "roms/%s.nds", np_game_id(game));
    np_storage_path(out, n, rel);
}

int np_storage_rom_present(np_game game)
{
    char p[1100];
    np_storage_rom_path(game, p, sizeof p);
    return np_storage_exists(p);
}

static int hash_file(const char *path, uint8_t *buf, char hex[41], char *err, size_t errn)
{
    SDL_IOStream *io = SDL_IOFromFile(path, "rb");
    if (!io) {
        SDL_snprintf(err, errn, "Cannot open %s: %s", path, SDL_GetError());
        return -1;
    }
    np_sha1 s;
    np_sha1_init(&s);
    size_t got;
    while ((got = SDL_ReadIO(io, buf, IO_CHUNK)) > 0)
        np_sha1_update(&s, buf, got);
    int failed = SDL_GetIOStatus(io) == SDL_IO_STATUS_ERROR;
    SDL_CloseIO(io);
    if (failed) {
        SDL_snprintf(err, errn, "Read error in %s", path);
        return -1;
    }
    uint8_t digest[20];
    np_sha1_final(&s, digest);
    np_sha1_hex(digest, hex);
    return 0;
}

static int copy_atomic(const char *src, const char *dst, uint8_t *buf)
{
    char tmp[1100];
    SDL_snprintf(tmp, sizeof tmp, "%s.tmp", dst);
    SDL_IOStream *in = SDL_IOFromFile(src, "rb");
    if (!in)
        return -1;
    SDL_IOStream *out = SDL_IOFromFile(tmp, "wb");
    if (!out) {
        SDL_CloseIO(in);
        return -1;
    }
    int ok = 1;
    size_t got;
    while (ok && (got = SDL_ReadIO(in, buf, IO_CHUNK)) > 0)
        ok = SDL_WriteIO(out, buf, got) == got;
    if (SDL_GetIOStatus(in) == SDL_IO_STATUS_ERROR)
        ok = 0;
    SDL_CloseIO(in);
    if (ok)
        ok = SDL_FlushIO(out);
    if (!SDL_CloseIO(out))
        ok = 0;
    if (ok)
        ok = SDL_RenamePath(tmp, dst);
    if (!ok)
        SDL_RemovePath(tmp);
    return ok ? 0 : -1;
}

void np_storage_import_rom(const char *src, np_import_result *r)
{
    memset(r, 0, sizeof *r);
    uint8_t *buf = SDL_malloc(IO_CHUNK);
    if (!buf) {
        SDL_snprintf(r->message, sizeof r->message, "Out of memory.");
        return;
    }
    char hex[41];
    if (hash_file(src, buf, hex, r->message, sizeof r->message)) {
        SDL_free(buf);
        return;
    }
    const np_rom_entry *e = np_romdb_lookup(hex);
    if (!e) {
        SDL_snprintf(r->message, sizeof r->message,
                     "Not a supported ROM. Expected a US Diamond, Pearl or Platinum (Rev 1) cartridge dump. "
                     "SHA-1: %s",
                     hex);
    } else if (e->status != NP_ROM_ACCEPTED) {
        const np_rom_entry *want = np_romdb_accepted(e->game);
        r->game = e->game;
        SDL_snprintf(r->message, sizeof r->message,
                     "%s is a revision this version cannot run. Please use %s (SHA-1 %s).", e->label,
                     want ? want->label : "the supported revision", want ? want->sha1 : "?");
    } else {
        char dst[1100];
        np_storage_rom_path(e->game, dst, sizeof dst);
        r->game = e->game;
        if (!strcmp(src, dst) || !copy_atomic(src, dst, buf)) {
            r->ok = 1;
            SDL_snprintf(r->message, sizeof r->message, "Imported %s.", e->label);
        } else {
            SDL_snprintf(r->message, sizeof r->message, "Could not copy the ROM into %s: %s", dst,
                         SDL_GetError());
        }
    }
    SDL_free(buf);
}

static int read_into(const char *path, void *dst, uint32_t len)
{
    SDL_IOStream *io = SDL_IOFromFile(path, "rb");
    if (!io)
        return 0;
    size_t got = SDL_ReadIO(io, dst, len);
    int failed = SDL_GetIOStatus(io) == SDL_IO_STATUS_ERROR;
    SDL_CloseIO(io);
    if (failed)
        return -1;
    /* A shorter file (e.g. from an older build with a smaller chip) keeps
     * its contents; the rest reads as erased flash. */
    if (got < len)
        memset((uint8_t *)dst + got, 0xFF, len - got);
    return 1;
}

int np_storage_save_load(np_game game, void *dst, uint32_t len)
{
    char rel[64], path[1100], bak[1110];
    SDL_snprintf(rel, sizeof rel, "saves/%s.sav", np_game_id(game));
    np_storage_path(path, sizeof path, rel);
    SDL_snprintf(bak, sizeof bak, "%s.bak", path);
    if (np_storage_exists(path)) {
        int r = read_into(path, dst, len);
        if (r == 1)
            return 1;
    }
    /* Missing or unreadable main file: an interrupted store leaves the
     * previous image in .bak. */
    if (np_storage_exists(bak))
        return read_into(bak, dst, len) == 1 ? 1 : -1;
    return np_storage_exists(path) ? -1 : 0;
}

int np_storage_save_store(np_game game, const void *src, uint32_t len)
{
    char rel[64], path[1100];
    SDL_snprintf(rel, sizeof rel, "saves/%s.sav", np_game_id(game));
    np_storage_path(path, sizeof path, rel);
    return np_storage_write_atomic(path, src, len, 1);
}
