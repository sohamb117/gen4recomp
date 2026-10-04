/*
 * Folder sync decisions, kept SDL-free so the rules are unit-tested; sync.c
 * does the file work.
 *
 * Each save slot is compared three ways: this device's file, the synced
 * folder's file, and the "base" (the content both had after the last
 * successful sync, remembered per slot). A side that changed since the base
 * wins; when both changed (or there is no base and they differ) it is a
 * conflict. An empty file (a fresh slot) never overwrites a real save, and
 * deletions are not inferred from a missing file: a slot deleted in the app
 * removes its synced copy explicitly (sync.c), so nothing is lost by a
 * folder that is not mounted or not yet downloaded.
 */
#ifndef NP_SYNC_PLAN_H
#define NP_SYNC_PLAN_H

#include <stddef.h>
#include <stdint.h>

#include "slots.h"

typedef enum np_sync_kind { NP_SYNC_ABSENT, NP_SYNC_EMPTY, NP_SYNC_DATA } np_sync_kind;

typedef struct np_sync_side {
    np_sync_kind kind;
    uint8_t hash[20]; /* SHA-1 of the file, for NP_SYNC_DATA */
} np_sync_side;

typedef enum np_sync_action {
    NP_SYNC_SAME,     /* nothing to copy */
    NP_SYNC_PUSH,     /* this device -> folder */
    NP_SYNC_PULL,     /* folder -> this device */
    NP_SYNC_CONFLICT, /* both changed: keep both */
} np_sync_action;

/* base: the hash after the last sync of this slot, or NULL if never synced. */
np_sync_action np_sync_decide(const np_sync_side *local, const np_sync_side *remote, const uint8_t *base);

/* "<slot> (conflict YYYY-MM-DD)", shortening the slot part to fit, made
 * unique against `taken` (np_slot_unique). Returns 0 or -1. */
int np_sync_conflict_name(const char *slot, int year, int month, int day, const char *const *taken, int ntaken,
                          char out[NP_SLOT_NAME_MAX + 1]);

/* What sync.c remembers per slot between runs (one line of sync-state.txt):
 * the base hash, and each file's size and modification time when it had
 * that content, so an unchanged file is recognised without hashing it. */
typedef struct np_sync_record {
    int game;
    char slot[NP_SLOT_NAME_MAX + 1];
    uint8_t base[20];
    uint64_t local_size, remote_size;
    int64_t local_mtime, remote_mtime;
    /* Non-empty: a conflict copy of this slot awaits the player's choice. */
    char conflict[NP_SLOT_NAME_MAX + 1];
} np_sync_record;

/* Parses one line ("game<TAB>slot<TAB>hash<TAB>lsize<TAB>lmtime<TAB>rsize
 * <TAB>rmtime<TAB>conflict"); game ids are matched against `game_ids`.
 * Returns 0, or -1 for a malformed line. */
int np_sync_record_parse(const char *line, const char *const *game_ids, int ngames, np_sync_record *r);
/* Formats one line, newline included. Returns its length (as snprintf). */
int np_sync_record_format(const np_sync_record *r, const char *const *game_ids, char *out, size_t n);

#endif
