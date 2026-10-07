/*
 * Launch requests: what to start, from the command line
 *   nativeplat [--game diamond|pearl|platinum] [--slot <name|number>] [--launcher]
 *   nativeplat --editor --save <file.sav> [--game diamond|pearl|platinum]
 * or from a URL
 *   nativeplat://launch?game=platinum&slot=My%20Run
 * (SDL delivers opened URLs as SDL_EVENT_DROP_FILE on macOS and iOS).
 * Only parsing lives here (SDL-free, unit-tested); main.c resolves the slot
 * against the slots on disk and falls back to the launcher with a message.
 * --editor opens the save editor on any save file (no slot, no game core)
 * and quits when the editor closes.
 */
#ifndef NP_LAUNCH_H
#define NP_LAUNCH_H

#include <stddef.h>

#include "slots.h"

typedef struct np_launch {
    int game;          /* np_game, or -1 for none */
    char slot[NP_SLOT_NAME_MAX * 3 + 1]; /* as given (name or 1-based number); "" for none */
    int force_launcher;
    int editor;      /* --editor: edit `save` standalone */
    char save[1024]; /* --save */
} np_launch;

/* Returns 0, or -1 with a message in `err`. Ignores arguments macOS and
 * Xcode add on their own (-psn_*, -NS* / -Apple* key-value pairs). */
int np_launch_parse_args(int argc, char *const *argv, np_launch *out, char *err, size_t errn);

/* Whether `s` is a nativeplat: URL (case-insensitive scheme). */
int np_launch_is_url(const char *s);

/* Parses nativeplat://launch?game=..&slot=..; percent-escapes and '+' are
 * decoded. Returns 0, or -1 with a message in `err`. */
int np_launch_parse_url(const char *url, np_launch *out, char *err, size_t errn);

/* "diamond"/"pearl"/"platinum"/"black"/"white" (any case) -> np_game, else -1. */
int np_launch_game_from_name(const char *name);

/* If `slot` is all digits, its value (1-based index), else 0. */
int np_launch_slot_number(const char *slot);

#endif
