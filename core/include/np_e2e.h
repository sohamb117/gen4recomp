/*
 * The end-to-end test probe (tests/e2e): what the input bots need to read
 * out of a running game, published by the guest into one block of its
 * memory.
 *
 * A guest started with PC_E2E=1 in its environment fills the block at every
 * frame boundary and reports the block's guest address in the status slot
 * NP_STAT_E2E (np_guest_abi.h); without PC_E2E the slot stays 0 and nothing
 * here runs, so ordinary runs and their hashes are untouched. The host reads
 * the block with np_core_guest_ptr (tests/gameplay/np_gp.c's `e2e` serve
 * command); tests/e2e/np_e2e.py unpacks it.
 *
 * Plain C with fixed-width fields only, little-endian, no padding: the
 * guest (wasm32) and every host see the same bytes.
 */
#ifndef NP_E2E_H
#define NP_E2E_H

#include <stdint.h>

#define NP_E2E_MAGIC 0x31453245u /* 'E2E1' */
#define NP_E2E_VERSION 4

/* The terrain window: GRID x GRID tiles, the player at (GRID/2, GRID/2). */
#define NP_E2E_GRID 64
#define NP_E2E_MAX_OBJECTS 64
#define NP_E2E_MAX_BATTLERS 4
#define NP_E2E_MAX_PARTY 6
#define NP_E2E_MAX_WARPS 64

/* What the game was waiting on during the last frame (np_e2e_block.ui). */
enum np_e2e_ui {
    NP_E2E_UI_NONE = 0,
    /* the battle's bottom-screen menu; ui_arg is its menu config index:
     * 1..10 the action menu (FIGHT/BAG/POKEMON/RUN variants), 11 the moves,
     * 12 the targets, 13 YES/NO (nickname, forfeit), 14 "forget another
     * move?", 15 "give up learning the move?", 16 "use the next Pokemon?",
     * 17 "switch Pokemon?" (Platinum battle_subscreen.c sBattleMenuConfigs;
     * D/P ov11_0225FAAC is the same table) */
    NP_E2E_UI_BATTLE_MENU = 1,
    /* the battle's party screen; ui_arg 0 the six slots, 1 the chosen
     * Pokemon's SHIFT/SUMMARY/MOVES page */
    NP_E2E_UI_BATTLE_PARTY = 2,
};

/* np_e2e_block.grid[] cells. */
#define NP_E2E_TILE_BEHAVIOR 0x00FFu  /* the map's tile behavior byte */
#define NP_E2E_TILE_COLLISION 0x0100u /* terrain collision bit set */
#define NP_E2E_TILE_CONNECTED 0x0200u /* v4 (GBA): the tile is a connected map's, seen across the border */
#define NP_E2E_TILE_ELEVATION 0x3C00u /* v4 (GBA): the tile's elevation (0..15) << 10 */
#define NP_E2E_TILE_ELEVATION_SHIFT 10
#define NP_E2E_TILE_KNOWN 0x8000u     /* the tile is on a loaded map block */

/* v3, np_e2e_block.steps[][]: the game's own step check over the window,
 * by layer. A tile can hold more than one place to stand (a bridge deck over
 * the path beneath it, a ledge of a gym's floor): each is a layer, at most
 * NP_E2E_LAYERS per tile, lowest first, its height in heights[][] (fx32
 * units, 16 per tile). Filled by a flood from the player over the moves the
 * movement code allows, carrying the height the way a step does. */
#define NP_E2E_LAYERS 2
#define NP_E2E_STEP_LAYER 0x8000u   /* the layer exists */
#define NP_E2E_STEP_DIRS 0x000Fu    /* bit d: a step in direction d (0 up, 1 down, 2 left, 3 right) is allowed */
#define NP_E2E_STEP_TARGET 0x00F0u  /* bit 4+d: the step lands on layer 1 of its tile (else layer 0) */
#define NP_E2E_STEP_JUMP 0x0F00u    /* bit 8+d: the step is a ledge jump, landing two tiles away */

typedef struct np_e2e_object {
    int16_t x, z;      /* tile */
    uint16_t local_id; /* the map's object event id */
    uint16_t gfx;      /* graphics id */
} np_e2e_object;

/* A Pokemon in the battle report: a battler (the battle's own copy: its
 * current types, its disabled move) or a party member (the party's copy;
 * types 0xFF, not reported). species 0: no Pokemon. */
typedef struct np_e2e_mon {
    uint16_t species;
    uint16_t hp, max_hp;
    uint16_t moves[4];
    uint16_t disabled_move; /* the move Disable blocks, 0 none */
    uint8_t pp[4];
    uint8_t level;
    uint8_t types[2];
    uint8_t egg;
} np_e2e_mon;

/* v4: a warp of the current map (GBA warp events), its tile and where it
 * leads (dest_map as map_id, the destination map's warp index). */
typedef struct np_e2e_warp {
    int16_t x, z;
    uint16_t dest_map;
    uint8_t dest_warp;
    uint8_t elevation;
} np_e2e_warp;

typedef struct np_e2e_block {
    uint32_t magic;   /* NP_E2E_MAGIC */
    uint32_t version; /* NP_E2E_VERSION */
    uint32_t frame;   /* frame boundaries seen since boot */
    uint32_t field;   /* 1 while a field with a player avatar exists */
    uint32_t map_id;  /* the field's map header id */
    int32_t x, z, y;  /* the player's tile (y: height in tiles) */
    uint32_t facing;  /* 0 up, 1 down, 2 left, 3 right */
    uint32_t move_state; /* the avatar's move state (0 none .. 3 end) */
    uint32_t ui;      /* enum np_e2e_ui seen during the last frame */
    uint32_t ui_arg;
    uint32_t ui_count; /* frames since ui began (the battle's loops report
                        * on every other frame; gaps of up to 3 frames hold) */
    int32_t grid_x0, grid_z0; /* the window's top-left tile */
    uint32_t grid_seq;        /* bumped whenever grid[] is refilled */
    uint32_t nobjects;        /* other active map objects (people, items) */
    np_e2e_object objects[NP_E2E_MAX_OBJECTS];
    uint16_t grid[NP_E2E_GRID * NP_E2E_GRID]; /* row-major, z then x */
    /* The battle, refreshed by the battle menu's input loop whenever the
     * menu waits (ui NP_E2E_UI_BATTLE_MENU); stale otherwise. */
    uint32_t battle_frame;  /* frame of the last refresh, 0 never */
    uint32_t battle_type;   /* the game's BATTLE_TYPE_* bits */
    uint32_t menu_battler;  /* the battler whose menu is up */
    uint32_t nparty;        /* party[] entries */
    np_e2e_mon battlers[NP_E2E_MAX_BATTLERS]; /* by battler: 0 and 2 the player's side, 1 and 3 the foe's */
    np_e2e_mon party[NP_E2E_MAX_PARTY]; /* menu_battler's party in the battle party screen's order */
    /* v3: the step layers (NP_E2E_STEP_*), refilled with grid[] by games that
     * report them (D/P); steps_seq 0: none, plan from grid[] alone. */
    uint32_t steps_seq;
    int32_t player_height; /* the player's layer: its height (as heights[][]) */
    uint16_t steps[NP_E2E_LAYERS][NP_E2E_GRID * NP_E2E_GRID];
    int16_t heights[NP_E2E_LAYERS][NP_E2E_GRID * NP_E2E_GRID];
    /* v4, filled by the GBA games (0 elsewhere): the cursor of the menu ui
     * reports (the action/move cursor 0 top left, 1 top right, 2 bottom
     * left, 3 bottom right; the target battler; a YES/NO cursor, 0 YES 1 NO;
     * the party menu's slot), the player avatar's state bits (the game's
     * PLAYER_AVATAR_FLAG_*: on foot, biking, surfing), and where the game
     * keeps what a bot may read with np_gp's `peek`: the event flags (bit n
     * of the bytes is flag n), the vars (u16 each, var 0x4000 + i), the party
     * (the cartridge's own Pokemon structs, party_count of them). steps
     * heights are elevations there (GBA tiles: 0 any, 15 multi-level), and
     * a GBA map's coordinates are its own: tiles of a connected map seen
     * across the border carry NP_E2E_TILE_CONNECTED, and walking across
     * the border changes map_id and the coordinates without a warp, which
     * connection_seq counts. soft_resets counts the game's own SoftReset
     * calls (the end of the credits), which restart the guest in place. */
    uint32_t ui_cursor;
    uint32_t avatar_flags;
    uint32_t flags_addr, flags_bytes;
    uint32_t vars_addr, vars_count;
    uint32_t party_addr, party_count;
    uint32_t connection_seq;
    uint32_t soft_resets;
    uint32_t nwarps;
    np_e2e_warp warps[NP_E2E_MAX_WARPS];
} np_e2e_block;

#endif /* NP_E2E_H */
