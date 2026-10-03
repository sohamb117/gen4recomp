/*
 * Everything the shell writes lives under one root: SDL's per-user pref dir,
 * or `userdata/` beside the executable when `portable.txt` is there. iOS
 * only allows writes inside the app container, and players expect a single
 * folder to back up, so no other path is ever written.
 *
 *   roms/<game>.nds        imported cartridge (verified by SHA-1)
 *   saves/<game>.sav[.bak] backup chip image, previous image
 *   screenshots/           F12 captures
 *   options.ini
 */
#ifndef NP_STORAGE_H
#define NP_STORAGE_H

#include <stddef.h>
#include <stdint.h>

#include "np_core.h"

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

/* np_host save_load/save_store semantics (see np_core.h). */
int np_storage_save_load(np_game game, void *dst, uint32_t len);
int np_storage_save_store(np_game game, const void *src, uint32_t len);

#endif
