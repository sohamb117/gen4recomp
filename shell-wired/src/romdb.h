/*
 * The cartridge dumps the shell knows about, keyed by SHA-1. Only the
 * retail US releases the decompilations match are accepted; other known
 * dumps are recognised so the player gets a precise reason instead of
 * "unknown file".
 */
#ifndef NP_ROMDB_H
#define NP_ROMDB_H

#include "np_core.h"

typedef enum np_rom_status {
    NP_ROM_ACCEPTED,
    NP_ROM_UNSUPPORTED, /* a known dump of a revision the cores do not match */
} np_rom_status;

typedef struct np_rom_entry {
    const char *sha1; /* lower-case hex */
    np_game game;
    np_rom_status status;
    const char *label; /* e.g. "Pokemon Platinum (USA) (Rev 1)" */
} np_rom_entry;

/* NULL when the hash is not in the table. */
const np_rom_entry *np_romdb_lookup(const char *sha1_hex);

/* The accepted dump for `game`. */
const np_rom_entry *np_romdb_accepted(np_game game);

const char *np_game_title(np_game game); /* "Diamond" */
const char *np_game_id(np_game game);    /* "diamond": file names, options */

#endif
