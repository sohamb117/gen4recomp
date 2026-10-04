/*
 * Rewind history: a byte budget of recent core snapshots (np_core_state_*).
 *
 * A snapshot is the whole guest machine, ~7 MB, but consecutive ones differ
 * in a small fraction of their bytes. The buffer therefore keeps only the
 * newest snapshot in full and, for every older one, the XOR between it and
 * its successor, run-length coded (zero runs + literal bytes). Stepping back
 * applies the newest delta to the full snapshot in place; when the budget is
 * exhausted the oldest deltas are dropped, which only shortens the history.
 *
 * All memory is allocated by np_rewind_create (and grown only if the core's
 * snapshots get larger), so recording and stepping back never allocate.
 * SDL-free for the unit tests.
 */
#ifndef NP_REWIND_H
#define NP_REWIND_H

#include <stddef.h>
#include <stdint.h>

typedef struct np_rewind np_rewind;

/* budget: bytes for the coded history; state_hint: expected snapshot size. */
np_rewind *np_rewind_create(size_t budget, size_t state_hint);
void np_rewind_destroy(np_rewind *r);
/* Forgets everything (a new or reloaded game). */
void np_rewind_clear(np_rewind *r);

/* Records `state` as the newest snapshot. Returns 0, or -1 when out of memory
 * (the history is then cleared). */
int np_rewind_push(np_rewind *r, const void *state, size_t len);

/* Steps back one snapshot: drops the newest and exposes the one recorded
 * before it in *state / *len (valid until the next call). Returns -1 when
 * there is nothing older. */
int np_rewind_step_back(np_rewind *r, const uint8_t **state, size_t *len);

/* Snapshots that np_rewind_step_back can still reach. */
int np_rewind_depth(const np_rewind *r);
/* Bytes of coded history in use. */
size_t np_rewind_used(const np_rewind *r);

#endif
