/*
 * The end-to-end probe's shared half on the GBA guests (gba_e2e.h; the
 * block is core/include/np_e2e.h's). Platinum's pc/src/pc_e2e.c is the DS
 * games' twin: the same window, the same flood over the game's own step
 * check, with GBA elevations in place of DS plate heights.
 */
#include <stdlib.h>
#include <string.h>

#include "gba_e2e.h"
#include "gba_port.h"

static np_e2e_block s_e2e_block;
static int s_e2e_state; /* 0 unread, 1 on, 2 off */
static int s_e2e_grid_dirty = 1, s_e2e_steps_dirty = 1;
static unsigned s_e2e_ui, s_e2e_ui_arg, s_e2e_ui_cursor, s_e2e_ui_seen;

uint32_t gba_tick_phase(void);
void gba_tick_set_phase(uint32_t phase);

int gba_e2e_on(void) {
    if (s_e2e_state == 0) {
        const char *v = getenv("PC_E2E");
        s_e2e_state = v != NULL && atoi(v) > 0 ? 1 : 2;
        if (s_e2e_state == 1) {
            s_e2e_block.magic = NP_E2E_MAGIC;
            s_e2e_block.version = NP_E2E_VERSION;
        }
    }
    return s_e2e_state == 1;
}

uint32_t gba_e2e_addr(void) { return gba_e2e_on() ? (uint32_t)(uintptr_t)&s_e2e_block : 0; }

/* The battle and party menus are polled every frame on the GBA, so a gap
 * means the menu is gone; one frame of slack covers a menu that hands over
 * to the next within the frame. */
#define UI_GAP 1

static void end_frame(np_e2e_block *b) {
    static unsigned gap;

    b->frame++;
    if (s_e2e_ui_seen) {
        b->ui_count = b->ui == s_e2e_ui && b->ui_arg == s_e2e_ui_arg ? b->ui_count + 1 : 1;
        b->ui = s_e2e_ui;
        b->ui_arg = s_e2e_ui_arg;
        b->ui_cursor = s_e2e_ui_cursor;
        gap = 0;
    } else if (b->ui != NP_E2E_UI_NONE && ++gap <= UI_GAP) {
        b->ui_count++;
    } else {
        b->ui = NP_E2E_UI_NONE;
        b->ui_arg = 0;
        b->ui_count = 0;
        b->ui_cursor = 0;
    }
    s_e2e_ui_seen = 0;
}

void gba_e2e_frame(void (*fill)(np_e2e_block *b)) {
    if (!gba_e2e_on()) return;
    uint32_t phase = gba_tick_phase();
    s_e2e_block.soft_resets = gba_soft_resets;
    fill(&s_e2e_block);
    end_frame(&s_e2e_block);
    gba_tick_set_phase(phase);
}

/* A map change seen while the field ran on through the frame before is a
 * walk across a map connection (a warp first leaves the field for the map
 * loader, which reports field 0). */
void gba_e2e_field(np_e2e_block *b, int field, unsigned map, int x, int z, int elevation, unsigned facing,
                   unsigned move_state) {
    static int prev_field;

    b->nobjects = 0;
    b->nwarps = 0;
    if (field && prev_field && map != b->map_id) b->connection_seq++;
    prev_field = field != 0;
    b->field = field != 0;
    if (!field) return;
    if (map != b->map_id || x != b->x || z != b->z || elevation != b->y) s_e2e_grid_dirty = s_e2e_steps_dirty = 1;
    b->map_id = map;
    b->x = x;
    b->z = z;
    b->y = elevation;
    b->facing = facing;
    b->move_state = move_state;
}

void gba_e2e_dirty(void) { s_e2e_grid_dirty = s_e2e_steps_dirty = 1; }

/* A GBA map's coordinates start at its own top left corner, and the tiles a
 * connected map shows across the border have negative ones on the west and
 * north: every tile of the window is asked, cell() says which are off. */
void gba_e2e_grid(np_e2e_block *b, unsigned (*cell)(void *ctx, int x, int z), void *ctx) {
    if (!s_e2e_grid_dirty) return;
    b->grid_x0 = b->x - NP_E2E_GRID / 2;
    b->grid_z0 = b->z - NP_E2E_GRID / 2;
    for (int gz = 0; gz < NP_E2E_GRID; gz++)
        for (int gx = 0; gx < NP_E2E_GRID; gx++)
            b->grid[gz * NP_E2E_GRID + gx] = (uint16_t)cell(ctx, b->grid_x0 + gx, b->grid_z0 + gz);
    b->grid_seq++;
    s_e2e_grid_dirty = 0;
}

#define CELLS (NP_E2E_GRID * NP_E2E_GRID)

static uint16_t s_e2e_queue[NP_E2E_LAYERS * CELLS];
static uint8_t s_e2e_swap[CELLS];

/* The layer of window cell `cell` at elevation e, added (and queued) when
 * the tile has room for another; -1 when it has none. */
static int step_layer(np_e2e_block *b, int cell, int e, unsigned *n) {
    for (int l = 0; l < NP_E2E_LAYERS; l++) {
        if (!(b->steps[l][cell] & NP_E2E_STEP_LAYER)) {
            b->steps[l][cell] = NP_E2E_STEP_LAYER;
            b->heights[l][cell] = (int16_t)e;
            s_e2e_queue[(*n)++] = (uint16_t)(l * CELLS + cell);
            return l;
        }
        if (b->heights[l][cell] == e) return l;
    }
    return -1;
}

void gba_e2e_steps(np_e2e_block *b, int (*step)(void *ctx, int x, int z, int e, int dir, int *te), void *ctx,
                   int elevation) {
    static const int dx[4] = {0, 0, -1, 1}, dz[4] = {-1, 1, 0, 0};
    const int x0 = b->x - NP_E2E_GRID / 2, z0 = b->z - NP_E2E_GRID / 2;
    unsigned head = 0, n = 0;

    if (!s_e2e_steps_dirty) return;
    s_e2e_steps_dirty = 0;
    memset(b->steps, 0, sizeof b->steps);
    memset(b->heights, 0, sizeof b->heights);
    step_layer(b, NP_E2E_GRID / 2 * NP_E2E_GRID + NP_E2E_GRID / 2, elevation, &n);
    while (head < n) {
        const unsigned s = s_e2e_queue[head++];
        const int sl = (int)(s / CELLS), sc = (int)(s % CELLS);
        const int gx = sc % NP_E2E_GRID, gz = sc / NP_E2E_GRID;
        for (int d = 0; d < 4; d++) {
            int te = 0;
            const int r = step(ctx, x0 + gx, z0 + gz, b->heights[sl][sc], d, &te);
            const int tx = gx + dx[d] * r, tz = gz + dz[d] * r;

            if (r == 0) continue;
            if (tx < 0 || tz < 0 || tx >= NP_E2E_GRID || tz >= NP_E2E_GRID) {
                /* off the window: allowed, its layer unknown here */
                b->steps[sl][sc] |= (uint16_t)(1u << d | (r == 2 ? 0x100u << d : 0));
                continue;
            }
            const int tl = step_layer(b, tz * NP_E2E_GRID + tx, te, &n);
            if (tl < 0) continue; /* a third layer: not reported */
            b->steps[sl][sc] |= (uint16_t)(1u << d | (r == 2 ? 0x100u << d : 0) | (tl == 1 ? 0x10u << d : 0));
        }
    }
    /* lowest layer first, so a tile's layers keep their numbers whichever
     * side the flood came from */
    for (int cell = 0; cell < CELLS; cell++) {
        s_e2e_swap[cell] = (b->steps[1][cell] & NP_E2E_STEP_LAYER) && b->heights[1][cell] < b->heights[0][cell];
        if (s_e2e_swap[cell]) {
            const uint16_t st = b->steps[0][cell];
            const int16_t h = b->heights[0][cell];
            b->steps[0][cell] = b->steps[1][cell];
            b->heights[0][cell] = b->heights[1][cell];
            b->steps[1][cell] = st;
            b->heights[1][cell] = h;
        }
    }
    for (int l = 0; l < NP_E2E_LAYERS; l++)
        for (int cell = 0; cell < CELLS; cell++) {
            uint16_t *st = &b->steps[l][cell];
            if (!(*st & NP_E2E_STEP_LAYER)) continue;
            for (int d = 0; d < 4; d++) {
                const int r = *st & 0x100u << d ? 2 : 1;
                const int tx = cell % NP_E2E_GRID + dx[d] * r, tz = cell / NP_E2E_GRID + dz[d] * r;
                if (!(*st & 1u << d) || tx < 0 || tz < 0 || tx >= NP_E2E_GRID || tz >= NP_E2E_GRID) continue;
                if (s_e2e_swap[tz * NP_E2E_GRID + tx]) *st ^= (uint16_t)(0x10u << d);
            }
        }
    b->player_height = elevation;
    b->steps_seq++;
}

void gba_e2e_object(np_e2e_block *b, int x, int z, unsigned local_id, unsigned gfx) {
    if (b->nobjects >= NP_E2E_MAX_OBJECTS) return;
    np_e2e_object *o = &b->objects[b->nobjects++];
    o->x = (int16_t)x;
    o->z = (int16_t)z;
    o->local_id = (uint16_t)local_id;
    o->gfx = (uint16_t)gfx;
}

void gba_e2e_warp(np_e2e_block *b, int x, int z, unsigned dest_map, unsigned dest_warp, unsigned elevation) {
    if (b->nwarps >= NP_E2E_MAX_WARPS) return;
    np_e2e_warp *w = &b->warps[b->nwarps++];
    w->x = (int16_t)x;
    w->z = (int16_t)z;
    w->dest_map = (uint16_t)dest_map;
    w->dest_warp = (uint8_t)dest_warp;
    w->elevation = (uint8_t)elevation;
}

void gba_e2e_ui(np_e2e_block *b, unsigned kind, unsigned arg, unsigned cursor) {
    (void)b;
    s_e2e_ui = kind;
    s_e2e_ui_arg = arg;
    s_e2e_ui_cursor = cursor;
    s_e2e_ui_seen = 1;
}
