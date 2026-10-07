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

/* ---- removing trees ---------------------------------------------------------- */

static SDL_EnumerationResult SDLCALL remove_child(void *user, const char *dirname, const char *fname)
{
    char path[1200];
    SDL_snprintf(path, sizeof path, "%s%s", dirname, fname);
    if (np_storage_remove_tree(path))
        *(int *)user = -1;
    return SDL_ENUM_CONTINUE;
}

int np_storage_remove_tree(const char *path)
{
    if (SDL_RemovePath(path))
        return 0; /* a file, a link or an empty directory */
    SDL_PathInfo info;
    if (!SDL_GetPathInfo(path, &info))
        return 0; /* already gone */
    if (info.type != SDL_PATHTYPE_DIRECTORY)
        return -1;
    int err = 0;
    SDL_EnumerateDirectory(path, remove_child, &err);
    return SDL_RemovePath(path) ? err : -1;
}

void np_storage_rom_path(np_game game, char *out, size_t n)
{
    char rel[64];
    SDL_snprintf(rel, sizeof rel, "roms/%s.%s", np_game_id(game), np_game_is_gba(game) ? "gba" : "nds");
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

/* 1 with `len` bytes, 0 if the file is missing or empty (a fresh slot), -1
 * on a read error. */
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
    if (got == 0)
        return 0;
    /* A shorter file keeps its contents; the rest reads as erased flash. */
    if (got < len)
        memset((uint8_t *)dst + got, 0xFF, len - got);
    return 1;
}

static void slot_dir(np_game game, char *out, size_t n)
{
    char rel[64];
    SDL_snprintf(rel, sizeof rel, "saves/%s", np_game_id(game));
    np_storage_path(out, n, rel);
}

void np_storage_slot_path(np_game game, const char *slot, char *out, size_t n)
{
    char rel[128];
    SDL_snprintf(rel, sizeof rel, "saves/%s/%s.sav", np_game_id(game), slot);
    np_storage_path(out, n, rel);
}

static int file_size(const char *path, Sint64 *size)
{
    SDL_PathInfo info;
    if (!SDL_GetPathInfo(path, &info) || info.type != SDL_PATHTYPE_FILE)
        return -1;
    *size = (Sint64)info.size;
    return 0;
}

static int slot_cmp(const void *a, const void *b)
{
    return SDL_strcasecmp(((const np_slot_info *)a)->name, ((const np_slot_info *)b)->name);
}

int np_storage_list_slots(np_game game, np_slot_list *list)
{
    char dir[1100];
    slot_dir(game, dir, sizeof dir);
    list->count = 0;
    list->truncated = 0;
    if (!SDL_CreateDirectory(dir))
        return -1;
    int n = 0;
    char **files = SDL_GlobDirectory(dir, "*.sav", SDL_GLOB_CASEINSENSITIVE, &n);
    if (!files)
        return -1;
    for (int i = 0; i < n; i++) {
        size_t len = SDL_strlen(files[i]);
        if (len <= 4 || len - 4 > NP_SLOT_NAME_MAX)
            continue;
        char name[NP_SLOT_NAME_MAX + 1];
        SDL_memcpy(name, files[i], len - 4);
        name[len - 4] = '\0';
        /* Files dropped in by hand with names we would not create are
         * skipped rather than half-supported. */
        if (np_slot_name_problem(name))
            continue;
        if (list->count == NP_MAX_SLOTS) {
            list->truncated = 1;
            break;
        }
        np_slot_info *s = &list->slot[list->count++];
        SDL_strlcpy(s->name, name, sizeof s->name);
        char path[1300];
        SDL_snprintf(path, sizeof path, "%s/%s", dir, files[i]);
        SDL_PathInfo info;
        int ok = SDL_GetPathInfo(path, &info);
        s->mtime = ok ? info.modify_time : 0;
        s->size = ok ? info.size : 0;
    }
    SDL_free(files);
    SDL_qsort(list->slot, (size_t)list->count, sizeof list->slot[0], slot_cmp);
    return 0;
}

int np_slot_list_find(const np_slot_list *list, const char *name)
{
    for (int i = 0; i < list->count; i++)
        if (np_slot_name_eq(list->slot[i].name, name))
            return i;
    return -1;
}

int np_storage_slot_create(np_game game, const char *name)
{
    char path[1100];
    np_storage_slot_path(game, name, path, sizeof path);
    if (np_slot_name_problem(name) || np_storage_exists(path))
        return SDL_SetError("slot \"%s\" already exists or is invalid", name), -1;
    return np_storage_write_atomic(path, "", 0, 0);
}

/* saves/<game>/<slot>.cart */
static void cart_sidecar(np_game game, const char *slot, char *out, size_t n)
{
    np_storage_slot_path(game, slot, out, n);
    char *dot = SDL_strrchr(out, '.');
    if (dot)
        *dot = '\0';
    SDL_strlcat(out, ".cart", n);
}

int np_storage_slot_cart(np_game game, const char *slot, char *out, size_t n)
{
    char path[1100];
    cart_sidecar(game, slot, path, sizeof path);
    size_t len;
    char *text = SDL_LoadFile(path, &len);
    if (!text)
        return -1;
    text[strcspn(text, "\r\n")] = '\0';
    SDL_strlcpy(out, text, n);
    SDL_free(text);
    return out[0] && !np_slot_name_problem(out) ? 0 : -1;
}

int np_storage_set_slot_cart(np_game game, const char *slot, const char *cart)
{
    char path[1100];
    cart_sidecar(game, slot, path, sizeof path);
    if (!cart[0])
        return SDL_RemovePath(path) || !np_storage_exists(path) ? 0 : -1;
    char line[NP_SLOT_NAME_MAX + 2];
    size_t len = (size_t)SDL_snprintf(line, sizeof line, "%s\n", cart);
    return np_storage_write_atomic(path, line, len, 0);
}

int np_storage_slot_rename(np_game game, const char *from, const char *to)
{
    char a[1100], b[1100], abak[1110], bbak[1110];
    np_storage_slot_path(game, from, a, sizeof a);
    np_storage_slot_path(game, to, b, sizeof b);
    SDL_snprintf(abak, sizeof abak, "%s.bak", a);
    SDL_snprintf(bbak, sizeof bbak, "%s.bak", b);
    /* A case-only change names the same file on case-insensitive volumes. */
    if (np_slot_name_problem(to) || (!np_slot_name_eq(from, to) && np_storage_exists(b)))
        return SDL_SetError("slot \"%s\" already exists or is invalid", to), -1;
    if (!SDL_RenamePath(a, b))
        return -1;
    if (np_storage_exists(abak) && !SDL_RenamePath(abak, bbak))
        SDL_Log("could not move %s: %s", abak, SDL_GetError());
    char ca[1100], cb[1100];
    cart_sidecar(game, from, ca, sizeof ca);
    cart_sidecar(game, to, cb, sizeof cb);
    if (np_storage_exists(ca) && !SDL_RenamePath(ca, cb))
        SDL_Log("could not move %s: %s", ca, SDL_GetError());
    return 0;
}

int np_storage_slot_duplicate(np_game game, const char *from, const char *to)
{
    char a[1100], b[1100];
    np_storage_slot_path(game, from, a, sizeof a);
    np_storage_slot_path(game, to, b, sizeof b);
    if (np_slot_name_problem(to) || np_storage_exists(b))
        return SDL_SetError("slot \"%s\" already exists or is invalid", to), -1;
    size_t size;
    void *data = SDL_LoadFile(a, &size);
    if (!data)
        return -1;
    int r = np_storage_write_atomic(b, data, size, 0);
    SDL_free(data);
    char cart[NP_SLOT_NAME_MAX + 1];
    if (!r && !np_storage_slot_cart(game, from, cart, sizeof cart))
        np_storage_set_slot_cart(game, to, cart); /* the copy plays the same cart */
    return r;
}

int np_storage_slot_delete(np_game game, const char *name)
{
    char path[1100], extra[1110];
    np_storage_slot_path(game, name, path, sizeof path);
    if (!SDL_RemovePath(path))
        return -1;
    static const char *const suffixes[] = {".bak", ".tmp"};
    for (size_t i = 0; i < SDL_arraysize(suffixes); i++) {
        SDL_snprintf(extra, sizeof extra, "%s%s", path, suffixes[i]);
        if (np_storage_exists(extra))
            SDL_RemovePath(extra);
    }
    np_storage_set_slot_cart(game, name, "");
    return 0;
}

int np_storage_slot_export(np_game game, const char *name, const char *dest, char *err, size_t errn)
{
    char path[1100];
    np_storage_slot_path(game, name, path, sizeof path);
    size_t size;
    void *data = SDL_LoadFile(path, &size);
    if (!data) {
        SDL_snprintf(err, errn, "Cannot read slot \"%s\": %s", name, SDL_GetError());
        return -1;
    }
    int r = -1;
    if (size == 0)
        SDL_snprintf(err, errn, "Slot \"%s\" has no save yet.", name);
    else if (write_whole(dest, data, size))
        SDL_snprintf(err, errn, "Cannot write %s: %s", dest, SDL_GetError());
    else
        r = 0;
    SDL_free(data);
    return r;
}

int np_storage_slot_import(np_game game, const char *src, char name_out[NP_SLOT_NAME_MAX + 1], char *err,
                           size_t errn)
{
    Sint64 size;
    if (file_size(src, &size)) {
        SDL_snprintf(err, errn, "Cannot open %s.", src);
        return -1;
    }
    if (size > 4 * (Sint64)NP_SAVE_BYTES) {
        SDL_snprintf(err, errn, "Not a Diamond/Pearl/Platinum save: the file is too large.");
        return -1;
    }
    size_t len;
    uint8_t *data = SDL_LoadFile(src, &len);
    if (!data) {
        SDL_snprintf(err, errn, "Cannot read %s: %s", src, SDL_GetError());
        return -1;
    }
    size_t raw;
    const char *why = NULL;
    int r = -1;
    if (np_sav_normalize(data, len, &raw, &why)) {
        SDL_snprintf(err, errn, "%s", why);
    } else {
        /* Name the slot after the file: "Platinum (USA).sav" -> "Platinum (USA)". */
        const char *base = src;
        for (const char *p = src; *p; p++)
            if (*p == '/' || *p == '\\')
                base = p + 1;
        char stem[256];
        SDL_strlcpy(stem, base, sizeof stem);
        char *dot = SDL_strrchr(stem, '.');
        if (dot && dot != stem)
            *dot = '\0';
        char clean[NP_SLOT_NAME_MAX + 1];
        np_slot_sanitize(stem, "Imported", clean);
        np_slot_list *list = SDL_malloc(sizeof *list);
        const char *taken[NP_MAX_SLOTS];
        if (!list || np_storage_list_slots(game, list)) {
            SDL_snprintf(err, errn, "Cannot list save slots: %s", SDL_GetError());
        } else {
            for (int i = 0; i < list->count; i++)
                taken[i] = list->slot[i].name;
            char path[1100];
            if (list->truncated || np_slot_unique(clean, taken, list->count, name_out)) {
                SDL_snprintf(err, errn, "Too many save slots.");
            } else {
                np_storage_slot_path(game, name_out, path, sizeof path);
                if (np_storage_write_atomic(path, data, raw, 0))
                    SDL_snprintf(err, errn, "Cannot write %s: %s", path, SDL_GetError());
                else
                    r = 0;
            }
        }
        SDL_free(list);
    }
    SDL_free(data);
    return r;
}

int np_storage_save_load(np_game game, const char *slot, void *dst, uint32_t len)
{
    char path[1100], bak[1110];
    np_storage_slot_path(game, slot, path, sizeof path);
    SDL_snprintf(bak, sizeof bak, "%s.bak", path);
    int r = read_into(path, dst, len);
    if (r == 1 || (r == 0 && np_storage_exists(path)))
        return r; /* a save, or a fresh empty slot */
    /* Missing or unreadable main file: an interrupted store leaves the
     * previous image in .bak. */
    if (np_storage_exists(bak))
        return read_into(bak, dst, len) == 1 ? 1 : -1;
    return r;
}

int np_storage_save_store(np_game game, const char *slot, const void *src, uint32_t len)
{
    char path[1100];
    np_storage_slot_path(game, slot, path, sizeof path);
    Sint64 old = 0;
    /* An empty file is a fresh slot: nothing worth keeping as .bak. */
    int keep = file_size(path, &old) == 0 && old > 0;
    return np_storage_write_atomic(path, src, len, keep);
}
