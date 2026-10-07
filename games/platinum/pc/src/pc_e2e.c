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

#include "np_e2e.h"

static np_e2e_block sBlock;
static int sState; /* 0 unread, 1 on, 2 off */
static int sGridDirty = 1;
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
    if (!field) return;
    if (map != b->map_id || x != b->x || z != b->z) sGridDirty = 1;
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

void pc_e2e_object(int x, int z, unsigned local_id, unsigned gfx)
{
    (void)x, (void)z, (void)local_id, (void)gfx;
}

void pc_e2e_ui(unsigned kind, unsigned arg)
{
    (void)kind, (void)arg;
}

void pc_e2e_end_frame(void)
{
}

#endif
