/*
 * Everything the shell writes lives under one root: SDL's per-user pref dir,
 * or `userdata/` beside the executable when `portable.txt` is there. iOS
 * only allows writes inside the app container, and players expect a single
 * folder to back up. The only writes elsewhere are .sav exports to a file
 * the player picked in a save dialog.
 *
 *   roms/<game>.nds                 imported cartridge (verified by SHA-1)
 *   saves/<game>/<slot>.sav[.bak]   save slots: backup chip image, previous image
 *   screenshots/                    F12 captures
 *   options.ini
 */
#ifndef NP_STORAGE_H
#define NP_STORAGE_H

#include <stddef.h>
#include <stdint.h>

#include "np_core.h"
#include "slots.h"

#define NP_MAX_SLOTS 64

typedef struct np_slot_info {
    char name[NP_SLOT_NAME_MAX + 1];
    int64_t mtime;  /* SDL_Time, ns since the Unix epoch */
    uint64_t size;  /* 0: fresh slot, no save written yet */
} np_slot_info;

typedef struct np_slot_list {
    int count;
    int truncated; /* more than NP_MAX_SLOTS on disk */
    np_slot_info slot[NP_MAX_SLOTS]; /* sorted by name, case-insensitively */
} np_slot_list;

/* Picks the root and creates the subdirectories. With `require_portable`,
 * fails (writing nothing) unless portable.txt is present. Returns 0 on
 * success. */
int np_storage_init(int require_portable, char *err, size_t errn);
const char *np_storage_root(void);
int np_storage_is_portable(void);

/* root + `rel` into `out`. */
void np_storage_path(char *out, size_t n, const char *rel);
int np_storage_exists(const char *path);

/* Writes `path` via a temp file and rename, so a crash leaves either the old
 * or the new contents. With `keep_backup`, the previous file becomes
 * `path.bak`. Returns 0 on success. */
int np_storage_write_atomic(const char *path, const void *data, size_t len, int keep_backup);

/* Removes a file, a link (never what it points to) or a directory tree;
 * 0 when nothing is left (or nothing was there). */
int np_storage_remove_tree(const char *path);

/* The cart bound to a slot (saves/<game>/<slot>.cart): 0 and its name, or -1
 * for none. Setting "" unbinds. Rename, duplicate and delete carry it. */
int np_storage_slot_cart(np_game game, const char *slot, char *out, size_t n);
int np_storage_set_slot_cart(np_game game, const char *slot, const char *cart);

/* Whether roms/<game>.nds has been imported. */
int np_storage_rom_present(np_game game);
void np_storage_rom_path(np_game game, char *out, size_t n);

typedef struct np_import_result {
    int ok;
    np_game game; /* valid when ok, or when a known-but-unsupported dump */
    char message[320];
} np_import_result;

/* Hashes `src`; if it is an accepted dump, copies it to roms/<game>.nds. */
void np_storage_import_rom(const char *src, np_import_result *r);

/* Save slots. Names must pass np_slot_name_problem(). Functions return 0 on
 * success, -1 with SDL_GetError() (or `err`) set. */
void np_storage_slot_path(np_game game, const char *slot, char *out, size_t n);
int np_storage_list_slots(np_game game, np_slot_list *list);
int np_slot_list_find(const np_slot_list *list, const char *name); /* index or -1 */
int np_storage_slot_create(np_game game, const char *name);        /* empty: no save yet */
int np_storage_slot_rename(np_game game, const char *from, const char *to);
int np_storage_slot_duplicate(np_game game, const char *from, const char *to);
int np_storage_slot_delete(np_game game, const char *name);
/* Raw image to a player-chosen path. */
int np_storage_slot_export(np_game game, const char *name, const char *dest, char *err, size_t errn);
/* Validates `src` (np_sav_normalize) and stores it as a new slot named after
 * the file; the chosen name is written to `name_out`. */
int np_storage_slot_import(np_game game, const char *src, char name_out[NP_SLOT_NAME_MAX + 1], char *err,
                           size_t errn);

/* np_host save_load/save_store semantics (see np_core.h) for one slot. */
int np_storage_save_load(np_game game, const char *slot, void *dst, uint32_t len);
int np_storage_save_store(np_game game, const char *slot, const void *src, uint32_t len);

#endif
