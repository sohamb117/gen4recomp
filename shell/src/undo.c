/*
 * See undo.h. Buffers move between the undo stack, the redo stack and one
 * spare, so steady-state editing allocates nothing.
 */
#include "undo.h"

#include <stdlib.h>
#include <string.h>

void np_undo_init(np_undo *u, size_t size)
{
    memset(u, 0, sizeof *u);
    u->size = size;
}

static void release(np_undo *u, uint8_t *buf)
{
    if (!u->spare)
        u->spare = buf;
    else
        free(buf);
}

static uint8_t *acquire(np_undo *u)
{
    uint8_t *b = u->spare;
    u->spare = NULL;
    return b ? b : malloc(u->size);
}

void np_undo_free(np_undo *u)
{
    for (int i = 0; i < u->nundo; i++)
        free(u->undo[i]);
    for (int i = 0; i < u->nredo; i++)
        free(u->redo[i]);
    free(u->spare);
    np_undo_init(u, u->size);
}

int np_undo_push(np_undo *u, const uint8_t *current)
{
    while (u->nredo)
        release(u, u->redo[--u->nredo]);
    uint8_t *b;
    if (u->nundo == NP_UNDO_DEPTH) {
        b = u->undo[0];
        memmove(u->undo, u->undo + 1, (NP_UNDO_DEPTH - 1) * sizeof u->undo[0]);
        u->nundo--;
    } else {
        b = acquire(u);
        if (!b)
            return -1;
    }
    memcpy(b, current, u->size);
    u->undo[u->nundo++] = b;
    return 0;
}

/* Moves the top of `from` into `current` and `current` onto `to`. */
static int step(np_undo *u, uint8_t **from, int *nfrom, uint8_t **to, int *nto, uint8_t *current)
{
    if (!*nfrom)
        return -1;
    uint8_t *b = acquire(u);
    if (!b)
        return -1;
    memcpy(b, current, u->size);
    uint8_t *prev = from[--*nfrom];
    memcpy(current, prev, u->size);
    release(u, prev);
    if (*nto == NP_UNDO_DEPTH) {
        free(to[0]);
        memmove(to, to + 1, (NP_UNDO_DEPTH - 1) * sizeof to[0]);
        (*nto)--;
    }
    to[(*nto)++] = b;
    return 0;
}

int np_undo_undo(np_undo *u, uint8_t *current) { return step(u, u->undo, &u->nundo, u->redo, &u->nredo, current); }

int np_undo_redo(np_undo *u, uint8_t *current) { return step(u, u->redo, &u->nredo, u->undo, &u->nundo, current); }
