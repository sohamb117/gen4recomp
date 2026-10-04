/*
 * Snapshot undo/redo for the save editor. Every edit first records the whole
 * image as it was (512 KiB); undo swaps the current image with the newest
 * snapshot. Whole-image snapshots are dumb on purpose: save4 edits touch
 * checksums and block footers, and copying 512 KiB is cheaper than getting
 * a diff format right. Capacity is bounded; the oldest snapshot is dropped.
 * SDL-free so it is unit-tested.
 */
#ifndef NP_UNDO_H
#define NP_UNDO_H

#include <stddef.h>
#include <stdint.h>

#define NP_UNDO_DEPTH 16

typedef struct np_undo {
    size_t size;                     /* bytes per snapshot */
    uint8_t *undo[NP_UNDO_DEPTH];    /* [0] oldest .. [nundo-1] newest */
    int nundo;
    uint8_t *redo[NP_UNDO_DEPTH];
    int nredo;
    uint8_t *spare;                  /* recycled buffer */
} np_undo;

void np_undo_init(np_undo *u, size_t size);
void np_undo_free(np_undo *u);
/* Records `current` (the state before an edit) and clears redo. -1 on OOM. */
int np_undo_push(np_undo *u, const uint8_t *current);
/* Replace `current` with the previous/next state. Return 0, or -1 when there
 * is nothing to undo/redo (or no memory). */
int np_undo_undo(np_undo *u, uint8_t *current);
int np_undo_redo(np_undo *u, uint8_t *current);

#endif
