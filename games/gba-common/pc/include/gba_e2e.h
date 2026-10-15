/*
 * The end-to-end probe on the GBA guests (core/include/np_e2e.h has the
 * block and what reads it; tests/e2e drives the games with it).
 *
 * gba_e2e.c is the half every GBA game shares: the block, PC_E2E, the
 * terrain window and its step flood, the UI report's bookkeeping. The game
 * half (games/emerald/pc/src/emerald_e2e.c, games/ruby/pc/src/ruby_e2e.c)
 * knows the decomp: it is called once per frame from the game's status hook
 * (the frame boundary, scanline 160, time stopped) through gba_e2e_frame(),
 * which keeps the probe's calls into game functions out of the game's CPU
 * time (gba_tick's phase is put back), so a probed run plays the frames an
 * unprobed one plays.
 *
 * Off unless the guest runs with PC_E2E=1: gba_e2e_frame() returns at once
 * and the status slot stays 0.
 */
#ifndef GBA_E2E_H
#define GBA_E2E_H

#include <stdint.h>

#include "np_e2e.h"

/* 1 when PC_E2E=1 (read once) */
int gba_e2e_on(void);

/* The guest address of the block for NP_STAT_E2E (0 when off). */
uint32_t gba_e2e_addr(void);

/* Runs fill() for this frame when the probe is on: fill writes the block
 * (gba_e2e_block) through the calls below; then the frame's UI report is
 * settled and the frame counted. */
void gba_e2e_frame(void (*fill)(np_e2e_block *b));

/* The player: field 0 clears the object list and leaves the rest. A change
 * of map, tile or elevation marks the window for a refill. */
void gba_e2e_field(np_e2e_block *b, int field, unsigned map, int x, int z, int elevation, unsigned facing,
                   unsigned move_state);

/* Asks for a refill of the window and the step layers at the next
 * gba_e2e_grid / gba_e2e_steps (a map's metatiles changed under the player,
 * or the window is old). */
void gba_e2e_dirty(void);

/* Refills grid[] (when dirty) from cell(ctx, x, z) at each tile of the
 * window around the player: NP_E2E_TILE_* bits, 0 off the map. */
void gba_e2e_grid(np_e2e_block *b, unsigned (*cell)(void *ctx, int x, int z), void *ctx);

/* Refills the step layers (when dirty): a flood from the player at its
 * elevation over step(ctx, x, z, elevation, dir, &target_elevation), which
 * returns 0 (refused), 1 (a step to the next tile) or 2 (a ledge jump,
 * landing two tiles on). Layers on one tile are told apart by elevation. */
void gba_e2e_steps(np_e2e_block *b, int (*step)(void *ctx, int x, int z, int e, int dir, int *te), void *ctx,
                   int elevation);

void gba_e2e_object(np_e2e_block *b, int x, int z, unsigned local_id, unsigned gfx);
void gba_e2e_warp(np_e2e_block *b, int x, int z, unsigned dest_map, unsigned dest_warp, unsigned elevation);

/* What the game waits on this frame (NP_E2E_UI_*, its argument, the
 * cursor); not called: nothing. */
void gba_e2e_ui(np_e2e_block *b, unsigned kind, unsigned arg, unsigned cursor);

#endif
