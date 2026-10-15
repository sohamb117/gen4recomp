/*
 * The end-to-end test probe, the half every game shares (core/include/
 * np_e2e.h has the block and what reads it). Compiled for Platinum and for
 * Diamond/Pearl (pc/mk/host.mk takes pc/src whole). The field half that
 * knows the game is in each game's field file, Platinum's pc_np_field.c and
 * D/P's pc/game/pc_dp_field.c: at every frame boundary it reports the
 * player and the map objects through the plain-typed calls below
 * (pc_np_options.h), which keeps np_e2e.h out of the games' own builds.
 *
 * Off unless the environment has PC_E2E=1: the block is not published and
 * pc_e2e_on() is 0, so the field halves do nothing and an ordinary run is
 * the run it always was. The UI reports (pc_e2e_ui) come from the game's
 * own input loops, patched in (Platinum pc/patches/src/battle/
 * battle_subscreen.c.patch and battle_sub_menus/battle_party.c.patch; D/P
 * the matching overlay 11 / overlay 9 asm patches): a loop that ran during
 * a frame is a menu the game is waiting on.
 *
 * Only the wasm guest has a host to read the block; other builds compile
 * the entry points to nothing.
 */
#include "pc_np_options.h"

#if defined(__wasm__)

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "np_e2e.h"

static np_e2e_block sBlock;
static int sState; /* 0 unread, 1 on, 2 off */
static int sGridDirty = 1, sStepsDirty = 1;
static unsigned sUi, sUiArg, sUiSeen;

int pc_e2e_on(void)
{
    if (sState == 0) {
        const char *v = getenv("PC_E2E");

        sState = v != NULL && atoi(v) > 0 ? 1 : 2;
        if (sState == 1) {
            sBlock.magic = NP_E2E_MAGIC;
            sBlock.version = NP_E2E_VERSION;
            pc_np_stat.e2e_block = (unsigned)(uintptr_t)&sBlock;
        }
    }
    return sState == 1;
}

void pc_e2e_field(int field, unsigned map, int x, int z, int y, unsigned facing, unsigned move_state)
{
    np_e2e_block *b = &sBlock;

    b->field = field != 0;
    b->nobjects = 0;
    b->nwarps = 0;
    if (!field) return;
    if (map != b->map_id || x != b->x || z != b->z) sGridDirty = sStepsDirty = 1;
    b->map_id = map;
    b->x = x;
    b->z = z;
    b->y = y;
    b->facing = facing;
    b->move_state = move_state;
}

void pc_e2e_grid(unsigned (*cell)(void *ctx, int x, int z), void *ctx)
{
    np_e2e_block *b = &sBlock;
    int gx, gz;

    if (!sGridDirty) return;
    b->grid_x0 = b->x - NP_E2E_GRID / 2;
    b->grid_z0 = b->z - NP_E2E_GRID / 2;
    for (gz = 0; gz < NP_E2E_GRID; gz++) {
        for (gx = 0; gx < NP_E2E_GRID; gx++) {
            const int tx = b->grid_x0 + gx, tz = b->grid_z0 + gz;

            b->grid[gz * NP_E2E_GRID + gx] = tx >= 0 && tz >= 0 ? (uint16_t)cell(ctx, tx, tz) : 0;
        }
    }
    b->grid_seq++;
    sGridDirty = 0;
}

#define CELLS (NP_E2E_GRID * NP_E2E_GRID)
/* Two places to stand on one tile closer than this are one layer: a step
 * lands on a plate's height, the same from every side. */
#define SAME_HEIGHT (4 << 12)

static int32_t sStepY[NP_E2E_LAYERS][CELLS];
static uint16_t sStepQueue[NP_E2E_LAYERS * CELLS];
static uint8_t sStepSwap[CELLS];

/* The layer of window cell `cell` at height y, added (and queued) when the
 * tile has room for another; -1 when it has none. */
static int step_layer(int cell, int32_t y, unsigned *n)
{
    np_e2e_block *b = &sBlock;
    int l;

    for (l = 0; l < NP_E2E_LAYERS; l++) {
        if (!(b->steps[l][cell] & NP_E2E_STEP_LAYER)) {
            b->steps[l][cell] = NP_E2E_STEP_LAYER;
            sStepY[l][cell] = y;
            sStepQueue[(*n)++] = (uint16_t)(l * CELLS + cell);
            return l;
        }
        if (abs(sStepY[l][cell] - y) < SAME_HEIGHT) return l;
    }
    return -1;
}

void pc_e2e_steps(int (*step)(void *ctx, int x, int z, int y, int dir, int *ty), void *ctx, int y)
{
    static const int dx[4] = {0, 0, -1, 1}, dz[4] = {-1, 1, 0, 0};
    np_e2e_block *b = &sBlock;
    const int x0 = b->x - NP_E2E_GRID / 2, z0 = b->z - NP_E2E_GRID / 2;
    unsigned head = 0, n = 0;
    int l, cell, d;

    if (!sStepsDirty) return;
    sStepsDirty = 0;
    memset(b->steps, 0, sizeof b->steps);
    memset(b->heights, 0, sizeof b->heights);
    step_layer(NP_E2E_GRID / 2 * NP_E2E_GRID + NP_E2E_GRID / 2, y, &n);
    while (head < n) {
        const unsigned s = sStepQueue[head++];
        const int sl = (int)(s / CELLS), sc = (int)(s % CELLS);
        const int gx = sc % NP_E2E_GRID, gz = sc / NP_E2E_GRID;

        for (d = 0; d < 4; d++) {
            int ty = 0, tl;
            const int r = step(ctx, x0 + gx, z0 + gz, sStepY[sl][sc], d, &ty);
            const int tx = gx + dx[d] * r, tz = gz + dz[d] * r;

            if (r == 0 || x0 + tx < 0 || z0 + tz < 0) continue;
            if (tx < 0 || tz < 0 || tx >= NP_E2E_GRID || tz >= NP_E2E_GRID) {
                /* off the window: allowed, its layer unknown here */
                b->steps[sl][sc] |= (uint16_t)(1u << d | (r == 2 ? 0x100u << d : 0));
                continue;
            }
            tl = step_layer(tz * NP_E2E_GRID + tx, ty, &n);
            if (tl < 0) continue; /* a third layer: not reported */
            b->steps[sl][sc] |= (uint16_t)(1u << d | (r == 2 ? 0x100u << d : 0) | (tl == 1 ? 0x10u << d : 0));
        }
    }
    /* lowest layer first, so a tile's layers keep their numbers whichever
     * side the flood came from */
    for (cell = 0; cell < CELLS; cell++) {
        sStepSwap[cell] = (b->steps[1][cell] & NP_E2E_STEP_LAYER) && sStepY[1][cell] < sStepY[0][cell];
        if (sStepSwap[cell]) {
            const uint16_t st = b->steps[0][cell];
            const int32_t sy = sStepY[0][cell];

            b->steps[0][cell] = b->steps[1][cell];
            sStepY[0][cell] = sStepY[1][cell];
            b->steps[1][cell] = st;
            sStepY[1][cell] = sy;
        }
    }
    for (l = 0; l < NP_E2E_LAYERS; l++) {
        for (cell = 0; cell < CELLS; cell++) {
            uint16_t *st = &b->steps[l][cell];

            if (!(*st & NP_E2E_STEP_LAYER)) continue;
            b->heights[l][cell] = (int16_t)(sStepY[l][cell] >> 12);
            for (d = 0; d < 4; d++) {
                const int r = *st & 0x100u << d ? 2 : 1;
                const int tx = cell % NP_E2E_GRID + dx[d] * r, tz = cell / NP_E2E_GRID + dz[d] * r;

                if (!(*st & 1u << d) || tx < 0 || tz < 0 || tx >= NP_E2E_GRID || tz >= NP_E2E_GRID) continue;
                if (sStepSwap[tz * NP_E2E_GRID + tx]) *st ^= (uint16_t)(0x10u << d);
            }
        }
    }
    b->player_height = y >> 12;
    b->steps_seq++;
}

void pc_e2e_object(int x, int z, unsigned local_id, unsigned gfx)
{
    np_e2e_block *b = &sBlock;
    np_e2e_object *o;

    if (b->nobjects >= NP_E2E_MAX_OBJECTS) return;
    o = &b->objects[b->nobjects++];
    o->x = (int16_t)x;
    o->z = (int16_t)z;
    o->local_id = (uint16_t)local_id;
    o->gfx = (uint16_t)gfx;
}

void pc_e2e_ui(unsigned kind, unsigned arg)
{
    sUi = kind;
    sUiArg = arg;
    sUiSeen = 1;
}

void pc_e2e_cursor(unsigned cursor)
{
    sBlock.ui_cursor = cursor;
}

void pc_e2e_v4(unsigned avatar_flags, unsigned flags_addr, unsigned flags_bytes, unsigned vars_addr,
               unsigned vars_count, unsigned party_addr, unsigned party_count)
{
    np_e2e_block *b = &sBlock;

    b->avatar_flags = avatar_flags;
    b->flags_addr = flags_addr;
    b->flags_bytes = flags_bytes;
    b->vars_addr = vars_addr;
    b->vars_count = vars_count;
    b->party_addr = party_addr;
    b->party_count = party_count;
}

void pc_e2e_warp(int x, int z, unsigned dest_map, unsigned dest_warp)
{
    np_e2e_block *b = &sBlock;
    np_e2e_warp *w;

    if (b->nwarps >= NP_E2E_MAX_WARPS) return;
    w = &b->warps[b->nwarps++];
    w->x = (int16_t)x;
    w->z = (int16_t)z;
    w->dest_map = (uint16_t)dest_map;
    w->dest_warp = (uint8_t)dest_warp;
    w->elevation = 0;
}

_Static_assert(sizeof(pc_e2e_mon) == sizeof(np_e2e_mon), "pc_e2e_mon mirrors np_e2e_mon");

void pc_e2e_battle(unsigned menu_battler, unsigned battle_type, const pc_e2e_mon *battlers, unsigned nbattlers,
                   const pc_e2e_mon *party, unsigned nparty)
{
    np_e2e_block *b = &sBlock;
    unsigned i;

    if (nbattlers > NP_E2E_MAX_BATTLERS) nbattlers = NP_E2E_MAX_BATTLERS;
    if (nparty > NP_E2E_MAX_PARTY) nparty = NP_E2E_MAX_PARTY;
    memset(b->battlers, 0, sizeof b->battlers);
    memset(b->party, 0, sizeof b->party);
    for (i = 0; i < nbattlers; i++) memcpy(&b->battlers[i], &battlers[i], sizeof b->battlers[i]);
    for (i = 0; i < nparty; i++) memcpy(&b->party[i], &party[i], sizeof b->party[i]);
    b->nparty = nparty;
    b->menu_battler = menu_battler;
    b->battle_type = battle_type;
    b->battle_frame = b->frame;
}

/* The battle runs its menu loop on every other frame boundary (and a busy
 * frame can skip one more), so a report holds through up to UI_GAP frames
 * without one: ui_count counts frames since the report began. */
#define UI_GAP 3

void pc_e2e_end_frame(void)
{
    static unsigned gap;
    np_e2e_block *b = &sBlock;

    b->frame++;
    if (sUiSeen) {
        b->ui_count = b->ui == sUi && b->ui_arg == sUiArg ? b->ui_count + 1 : 1;
        b->ui = sUi;
        b->ui_arg = sUiArg;
        gap = 0;
    } else if (b->ui != NP_E2E_UI_NONE && ++gap <= UI_GAP) {
        b->ui_count++;
    } else {
        b->ui = NP_E2E_UI_NONE;
        b->ui_arg = 0;
        b->ui_count = 0;
    }
    sUiSeen = 0;
}

#else

int pc_e2e_on(void)
{
    return 0;
}

void pc_e2e_field(int field, unsigned map, int x, int z, int y, unsigned facing, unsigned move_state)
{
    (void)field, (void)map, (void)x, (void)z, (void)y, (void)facing, (void)move_state;
}

void pc_e2e_grid(unsigned (*cell)(void *ctx, int x, int z), void *ctx)
{
    (void)cell, (void)ctx;
}

void pc_e2e_steps(int (*step)(void *ctx, int x, int z, int y, int dir, int *ty), void *ctx, int y)
{
    (void)step, (void)ctx, (void)y;
}

void pc_e2e_object(int x, int z, unsigned local_id, unsigned gfx)
{
    (void)x, (void)z, (void)local_id, (void)gfx;
}

void pc_e2e_ui(unsigned kind, unsigned arg)
{
    (void)kind, (void)arg;
}

void pc_e2e_cursor(unsigned cursor)
{
    (void)cursor;
}

void pc_e2e_v4(unsigned avatar_flags, unsigned flags_addr, unsigned flags_bytes, unsigned vars_addr,
               unsigned vars_count, unsigned party_addr, unsigned party_count)
{
    (void)avatar_flags, (void)flags_addr, (void)flags_bytes, (void)vars_addr, (void)vars_count, (void)party_addr,
        (void)party_count;
}

void pc_e2e_warp(int x, int z, unsigned dest_map, unsigned dest_warp)
{
    (void)x, (void)z, (void)dest_map, (void)dest_warp;
}

void pc_e2e_battle(unsigned menu_battler, unsigned battle_type, const pc_e2e_mon *battlers, unsigned nbattlers,
                   const pc_e2e_mon *party, unsigned nparty)
{
    (void)menu_battler, (void)battle_type, (void)battlers, (void)nbattlers, (void)party, (void)nparty;
}

void pc_e2e_end_frame(void)
{
}

#endif
