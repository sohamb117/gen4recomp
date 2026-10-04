/*
 * Runtime content packages ("mods") as the Platinum core loads them: a
 * directory with mod.toml, authored content/ and records/, and the cooked
 * .cooked/ the core serves (games/platinum/pc/mods/README.md). The manager
 * (mods.c) reads mod.toml to show names and to warn about dependencies and
 * load order before a boot; the core enforces the same rules and reports
 * "modfs: ..." if they are broken.
 *
 * SDL-free so the parsing and ordering rules are unit-tested.
 */
#ifndef NP_MODPKG_H
#define NP_MODPKG_H

#include <stddef.h>

#define NP_MOD_ID_MAX 48
#define NP_MOD_DEPS 8

typedef struct np_mod_info {
    char id[NP_MOD_ID_MAX];  /* [a-z0-9_]+ */
    char name[64];
    char version[24];
    char authors[96]; /* joined with ", " */
    int nrequires, nafter;
    char requires[NP_MOD_DEPS][NP_MOD_ID_MAX];
    char load_after[NP_MOD_DEPS][NP_MOD_ID_MAX];
} np_mod_info;

/* Parses mod.toml's flat `key = value` lines (strings and string arrays).
 * Returns 0, or -1 with *why set (missing or malformed id/name/version). */
int np_mod_parse(const char *text, size_t len, np_mod_info *m, const char **why);

/* Checks the enabled packages in load order: every `requires` is enabled,
 * and every enabled `load_after` package comes earlier. Returns the index
 * of the first package that breaks a rule (and explains it in why), or -1. */
int np_mod_check_order(const np_mod_info *const *enabled, int n, char *why, size_t whyn);

#endif
