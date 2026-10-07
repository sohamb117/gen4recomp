/*
 * Player-facing options from the nativeplat runtime (core/include/
 * np_guest_abi.h, enum np_opt / enum np_status), as the port sees them.
 *
 * The wasm frame publisher (pc/src/pc_view.c) copies the descriptor's opt[]
 * into pc_np_opt after every np_host_vblank and the status below into the
 * descriptor before the next one; every other host leaves both at their
 * defaults. Each option's default is the cartridge's own behaviour, and
 * every reader treats the default as "do exactly what the game does", so a
 * run that never changes an option is the run it always was.
 *
 * Plain C with no game types, so the game's own sources (through
 * pc/patches) and the ARM7 sound driver's glue can include it.
 */
#ifndef PC_NP_OPTIONS_H
#define PC_NP_OPTIONS_H

typedef struct pc_np_options {
    unsigned bgm_volume;   /* 0..256: sequence players 1 FIELD, 2 ME, 7 BGM */
    unsigned se_volume;    /* 0..256: players 0 PV (cries) and 3..6 SE */
    unsigned render_scale; /* 1..4: 3D internal resolution */
    unsigned widescreen;   /* 0 or 1: PC_VIEW_WIDE_MAX-wide 3D view */
    unsigned camera_zoom;  /* field camera distance, 256 = the game's */
    int camera_tilt;       /* field camera pitch, 1/16 degree, + = toward the horizon */
    unsigned quicksave_seq; /* bumped by the host: one in-game save, no UI */
    unsigned rules;        /* PC_NP_RULE_* bits */
    unsigned text_instant; /* 1: message boxes print at once */
} pc_np_options;

/* NP_RULE_FIX_BUGS: the documented cartridge bugs pc/patches fixes behind
 * this bit (docs/bugs_and_glitches.md; see pc/src/pc_np_options.c). */
#define PC_NP_RULE_FIX_BUGS 1u

extern pc_np_options pc_np_opt;

/* Guest -> host, published with every frame (enum np_status). */
typedef struct pc_np_status {
    unsigned link_active;      /* a wireless session is running */
    unsigned field_ready;      /* the player is free in the field */
    unsigned quicksave_seq;    /* last quicksave_seq handled */
    unsigned quicksave_result; /* PC_NP_QS_* for it */
    unsigned map_id;           /* current field map header id */
    /* NP_STAT_IN_BATTLE is either of these: a field encounter task from its
     * intro effect to the fade back (src/encounter.c.patch), or the battle
     * application itself, which is all a facility battle has
     * (src/unk_0203D1B8.c.patch). */
    unsigned in_encounter;
    unsigned in_battle_app;
    unsigned e2e_block;        /* NP_STAT_E2E: the probe's address, 0 when off */
} pc_np_status;

enum { PC_NP_QS_NONE = 0, PC_NP_QS_SAVED = 1, PC_NP_QS_REFUSED = 2, PC_NP_QS_FAILED = 3 };

extern pc_np_status pc_np_stat;

/* Applies a fresh option set (the publisher, after vblank): range checks,
 * the sound driver's per-player attenuation. */
void pc_np_options_set(const pc_np_options *o);

/* Game-side work at the frame boundary, before the frame is published:
 * field readiness, the map id, a pending quick save. Platinum's lives in
 * pc/src/pc_np_field.c; weak, so a game without one links. */
void pc_np_frame(void) __attribute__((weak));

/* The field camera's per-frame override, called from the field renderer
 * (pc/patches/src/overlay005/fieldmap.c.patch) around its view matrix. A
 * no-op at zoom 256 and tilt 0. The argument is the game's Camera. */
struct Camera;
void pc_np_camera_begin(struct Camera *camera);
void pc_np_camera_end(struct Camera *camera);

/* The end-to-end test probe (core/include/np_e2e.h; pc/src/pc_e2e.c).
 * pc_e2e_on() is 0 unless the guest runs with PC_E2E=1, and the field half
 * then does nothing. Otherwise, once per frame boundary, the field half
 * reports the field (pc_e2e_field; field 0 when there is none), then while
 * the player is free the terrain (pc_e2e_grid: cell(x, z) answers one tile
 * as PC_E2E_TILE_* bits, asked only after the player moved or the map
 * changed) and each other map object (pc_e2e_object), and closes the frame
 * with pc_e2e_end_frame. pc_e2e_ui is called by the game's patched input
 * loops: a PC_E2E_UI_* kind and its argument. While the battle menu waits,
 * the game's half also reports the battle (pc_e2e_battle: the battlers by
 * id, the menu battler's party in its party screen's order). */
int pc_e2e_on(void);
void pc_e2e_field(int field, unsigned map, int x, int z, int y, unsigned facing, unsigned move_state);
void pc_e2e_grid(unsigned (*cell)(void *ctx, int x, int z), void *ctx);
void pc_e2e_object(int x, int z, unsigned local_id, unsigned gfx);
void pc_e2e_end_frame(void);
void pc_e2e_ui(unsigned kind, unsigned arg);
/* np_e2e_mon, field for field */
typedef struct pc_e2e_mon {
    unsigned short species;
    unsigned short hp, max_hp;
    unsigned short moves[4];
    unsigned short disabled_move;
    unsigned char pp[4];
    unsigned char level;
    unsigned char types[2];
    unsigned char egg;
} pc_e2e_mon;
void pc_e2e_battle(unsigned menu_battler, unsigned battle_type, const pc_e2e_mon *battlers, unsigned nbattlers,
                   const pc_e2e_mon *party, unsigned nparty);
/* Platinum's battle half (pc/src/pc_np_field.c), from the battle menu's
 * input loop (pc/patches/src/battle/battle_subscreen.c.patch): the menu's
 * BattleSystem and the battler type the menu is for. */
struct BattleSystem;
void pc_pl_e2e_battle(struct BattleSystem *battleSys, unsigned battler_type);
#define PC_E2E_UI_BATTLE_MENU 1     /* NP_E2E_UI_BATTLE_MENU */
#define PC_E2E_UI_BATTLE_PARTY 2    /* NP_E2E_UI_BATTLE_PARTY */
#define PC_E2E_TILE_COLLISION 0x0100u /* NP_E2E_TILE_COLLISION */
#define PC_E2E_TILE_KNOWN 0x8000u     /* NP_E2E_TILE_KNOWN */

#endif /* PC_NP_OPTIONS_H */
