/*
 * Ruby and Sapphire's half of the end-to-end probe (games/gba-common/pc/
 * include/gba_e2e.h; the block is core/include/np_e2e.h). Called once per
 * frame from ruby_port.c's status hook, at the frame boundary. The
 * counterpart of games/emerald/pc/src/emerald_e2e.c; pokeruby's names and
 * the places it differs from Emerald are cited (pokeruby source files).
 *
 * Field: the player object (gObjectEvents[gPlayerAvatar.objectEventId],
 * global.fieldmap.h:304,326): its tile in map coordinates (currentCoords
 * less MAP_OFFSET 7, fieldmap.h:19), currentElevation (global.fieldmap.h:197;
 * pokeruby's functions call it the Z coordinate), facing; the other active
 * objects; the map's warp events (gMapHeader.events->warps,
 * global.fieldmap.h:83,116); and the terrain window through the game's own
 * grid queries (fieldmap.c MapGridGetMetatileBehaviorAt :357,
 * MapGridGetCollisionAt :337, MapGridGetElevationAt :327, GetMapBorderIdAt
 * :526, CONNECTION_INVALID -1 / CONNECTION_NONE 0 constants/global.h:92).
 * gSaveBlock1 is an object, not a pointer as Emerald's gSaveBlock1Ptr
 * (global.h:756): the flags/vars are always there (global.h:701,702).
 *
 * The step layers are the player's own collision check on foot
 * (field_player_avatar.c:592 CheckForObjectEventCollision ->
 * event_object_movement.c:4463 GetCollisionAtCoords) without the object half:
 * a ledge first (ShouldJumpLedge :631, GetLedgeJumpDirection
 * event_object_movement.c:7485, returning 0 for none: two tiles), then the
 * tile's collision, the map's border, the one-way metatile tables
 * IsMetatileDirectionallyImpassable (:4517) reads
 * (gOppositeDirectionBlockedMetatileFuncs :782 on the tile left,
 * gDirectionBlockedMetatileFuncs :789 on the tile entered), then
 * IsZCoordMismatchAt (:7528, global here, called as is: elevation 0 goes
 * anywhere, tiles at 0 or 15 take any, else they must match), and a surfing
 * player's landing (field_player_avatar.c:616 sub_8058EF0, Emerald's
 * CanStopSurfing: elevation 3; its "no object there" half is left to the
 * bots). The elevation a step lands at is ObjectEventUpdateZCoord's (:7586:
 * unchanged when either tile is 15, else the new tile's), reimplemented as it
 * works on an object. pokeruby has no ELEVATION_* names: 0, 3 and 15 are the
 * literals of those functions.
 *
 * Battle: the player controller's input loops (battle_controller_player.c
 * HandleInputChooseAction :340, global here; HandleInputChooseMove :580 and
 * HandleInputChooseTarget :445, static; found in gBattlerControllerFuncs),
 * the battle script's YES/NO boxes (battle_script_commands.c table :546:
 * atk5A_yesnoboxlearnmove 0x5A :5285, atk5B_yesnoboxstoplearningmove 0x5B
 * :5398, both on gBattleStruct->atk5A_StateTracker (battle.h:340, Emerald's
 * gBattleScripting.learnMoveState); atk67_yesnobox 0x67 :5656 and
 * atkF3_trygivecaughtmonnick 0xF3 :9625 on gBattleCommunication[0]; all wait
 * on input in their state 1 with the cursor in gBattleCommunication[1]), the
 * evolution scene's move-learning YES/NO (evolution_scene.c:529
 * Task_EvolutionScene: tState data[0] 21 :729, tLearnMoveState data[8] 4
 * :769, tData9 data[9] 5 "delete a move?" :756 or 10 "stop learning?" :875,
 * cursor gBattleCommunication[1] :77), and the in-battle party menu
 * (party_menu.c:482 CB2_PartyMenuMain with ePartyMenu2.menuType
 * PARTY_MENU_TYPE_BATTLE: the list is battle_party_menu.c:466
 * HandleBattlePartyMenu, then battle_party_menu.c:581
 * Task_HandlePopupMenuInput for SHIFT / SUMMARY / CANCEL; pokeruby keeps both
 * in battle_party_menu.c, not party_menu.c). The menu's cursor is the
 * selection sprite's data[0] (party_menu.c:1784 sub_806CA38): slots 0..5,
 * 7 CANCEL; DOWN steps a slot, from the last mon to 7, from 7 to 0
 * (party_menu.c:1406 ChangeDefaultPartyMenuSelection; a double battle's
 * layout moves otherwise, :1361). The menu numbers are the DS ones the
 * harness knows (np_e2e.h enum np_e2e_ui): 1 actions, 11 moves, 12 targets,
 * 13 nickname, 14 "delete a move?", 15 "stop learning?", 16 "use the next
 * Pokemon?" (a wild battle's yesnobox), 17 "switch Pokemon?" (a trainer
 * battle's).
 *
 * Field menus (NP_E2E_UI_FIELD_MENU): a script's multichoice or YES/NO
 * waiting on input (script_menu.c:684 Task_HandleMultichoiceInput, :782
 * Task_HandleYesNoInput, both static) reads menu.c's gMenu (static, menu.c:36;
 * struct Menu menu.c:14: cursorPos at +2, maxCursorPos at +4, the layout of
 * Emerald's sMenu). InitMenu sets maxCursorPos to the entry count less one
 * (menu.c:734), YES/NO through InitYesNoMenu with 2 (menu.c:603, YES 0);
 * Menu_GetCursorPos (menu.c:265) is global. The grid multichoice
 * (script_menu.c:853 Task_HandleMultichoiceGridInput) is not reported, as in
 * Emerald.
 *
 * The party order: during a battle gPlayerParty stays in field order; the
 * party menu's order is the battler's gBattleStruct->unk1606C (battle.h:361,
 * three bytes of two nybbles, the field slot shown at each menu slot:
 * battle_party_menu.c:236 sub_8094C20), which the battle sends with each
 * choice (battle_main.c:4339) and the menu copies gPlayerParty into while it
 * is open (battle_party_menu.c:286 pokemon_change_order, undone at :299).
 * So the battle report lists gPlayerParty through that order, and the party
 * menu's report lists gPlayerParty as it is.
 */
#include "global.h"
#include "battle.h"
#include "event_object_movement.h"
#include "ewram.h"
#include "fieldmap.h"
#include "main.h"
#include "menu.h"
#include "party_menu.h"
#include "pokemon.h"
#include "rom_8077ABC.h"
#include "task.h"
#include "constants/battle.h"
#include "gba_e2e.h"
#include "gba_port.h"

GBA_LOCAL_DECL(battle_controller_player, HandleInputChooseMove);
GBA_LOCAL_DECL(battle_controller_player, HandleInputChooseTarget);
GBA_LOCAL_DECL(battle_party_menu, Task_HandlePopupMenuInput);
GBA_LOCAL_DECL(evolution_scene, Task_EvolutionScene);
GBA_LOCAL_DECL(overworld, CB2_Overworld);
GBA_LOCAL_DECL(script_menu, Task_HandleMultichoiceInput);
GBA_LOCAL_DECL(script_menu, Task_HandleYesNoInput);
GBA_LOCAL_DECL(menu, gMenu);

/* pokeruby declares these in the files that use them, not in headers */
extern bool8 (*const gOppositeDirectionBlockedMetatileFuncs[])(u8); /* event_object_movement.c:782 */
extern bool8 (*const gDirectionBlockedMetatileFuncs[])(u8);         /* event_object_movement.c:789 */
extern struct BattlePokemon gBattleMons[MAX_BATTLERS_COUNT];        /* battle_script_commands.c:44 */
extern u8 gBattlersCount;                                           /* battle_script_commands.c:47 */
extern const u8 *gBattlescriptCurrInstr;                            /* battle_script_commands.c:70 */
extern u8 gBattleCommunication[];                                   /* battle_script_commands.c:75 */
extern void (*gBattlerControllerFuncs[])(void);                     /* battle_controller_player.c:42 */
extern u8 gActionSelectionCursor[];                                 /* battle_controller_player.c:49 */
extern u8 gMoveSelectionCursor[];                                   /* battle_controller_player.c:51 */
extern u8 gMultiUsePlayerCursor;                                    /* battle_controller_player.c:73 */
u8 sub_806CA38(u8 taskId);                                          /* party_menu.c:1784 */

/* event_object_movement.c:7532,7537 IsZCoordMismatchAt, field_player_avatar.c:619 */
#define E2E_ELEVATION_TRANSITION 0
#define E2E_ELEVATION_DEFAULT 3
#define E2E_ELEVATION_MULTI_LEVEL 15

/* probe direction (0 up, 1 down, 2 left, 3 right) -> the game's DIR_* and back */
static const u8 sE2eGameDir[4] = {DIR_NORTH, DIR_SOUTH, DIR_WEST, DIR_EAST};
static const s8 sE2eDx[4] = {0, 0, -1, 1}, sE2eDy[4] = {-1, 1, 0, 0};

static unsigned e2e_facing(u8 dir) {
    switch (dir) {
    case DIR_NORTH: return 0;
    case DIR_WEST: return 2;
    case DIR_EAST: return 3;
    default: return 1;
    }
}

static unsigned e2e_cell(void *ctx, int x, int z) {
    const s32 gx = x + MAP_OFFSET, gy = z + MAP_OFFSET;
    s32 border;

    (void)ctx;
    border = GetMapBorderIdAt(gx, gy);
    if (border == CONNECTION_INVALID) return 0;
    return NP_E2E_TILE_KNOWN | ((u32)MapGridGetMetatileBehaviorAt(gx, gy) & NP_E2E_TILE_BEHAVIOR)
         | (MapGridGetCollisionAt(gx, gy) ? NP_E2E_TILE_COLLISION : 0)
         | (border != CONNECTION_NONE ? NP_E2E_TILE_CONNECTED : 0)
         | (u32)MapGridGetElevationAt(gx, gy) << NP_E2E_TILE_ELEVATION_SHIFT;
}

/* event_object_movement.c ObjectEventUpdateZCoord */
static int e2e_landing_elevation(int e, s32 fx, s32 fy, s32 tx, s32 ty) {
    u8 cur = MapGridGetElevationAt(tx, ty), prev = MapGridGetElevationAt(fx, fy);

    if (cur == E2E_ELEVATION_MULTI_LEVEL || prev == E2E_ELEVATION_MULTI_LEVEL) return e;
    return cur;
}

static int e2e_step(void *ctx, int x, int z, int e, int dir, int *te) {
    const s32 fx = x + MAP_OFFSET, fy = z + MAP_OFFSET;
    const s32 nx = fx + sE2eDx[dir], ny = fy + sE2eDy[dir];
    const u8 gdir = sE2eGameDir[dir];
    const u8 surfing = gPlayerAvatar.flags & PLAYER_AVATAR_FLAG_SURFING;

    (void)ctx;
    if (GetLedgeJumpDirection(nx, ny, gdir) != DIR_NONE) {
        const s32 lx = nx + sE2eDx[dir], ly = ny + sE2eDy[dir];

        if (GetMapBorderIdAt(lx, ly) == CONNECTION_INVALID) return 0;
        *te = e2e_landing_elevation(e, fx, fy, lx, ly);
        return 2;
    }
    if (MapGridGetCollisionAt(nx, ny) || GetMapBorderIdAt(nx, ny) == CONNECTION_INVALID
        || gOppositeDirectionBlockedMetatileFuncs[gdir - 1](MapGridGetMetatileBehaviorAt(fx, fy))
        || gDirectionBlockedMetatileFuncs[gdir - 1](MapGridGetMetatileBehaviorAt(nx, ny)))
        return 0;
    if (IsZCoordMismatchAt(e, nx, ny)) {
        /* field_player_avatar.c sub_8058EF0 (CanStopSurfing): off the water onto elevation 3 */
        if (!surfing || MapGridGetElevationAt(nx, ny) != E2E_ELEVATION_DEFAULT) return 0;
        *te = E2E_ELEVATION_DEFAULT;
        return 1;
    }
    *te = e2e_landing_elevation(e, fx, fy, nx, ny);
    return 1;
}

/* pokeruby declares GetMonData K&R (pokemon.h:450) and defines it with three
 * parameters; a wasm call must pass all three (gba_prelude.h does this for
 * the decomp's own TUs, not for the port's). */
static void e2e_mon(np_e2e_mon *m, struct Pokemon *mon) {
    int i;

    memset(m, 0, sizeof *m);
    m->species = GetMonData(mon, MON_DATA_SPECIES, NULL);
    if (!m->species) return;
    m->hp = GetMonData(mon, MON_DATA_HP, NULL);
    m->max_hp = GetMonData(mon, MON_DATA_MAX_HP, NULL);
    for (i = 0; i < MAX_MON_MOVES; i++) {
        m->moves[i] = GetMonData(mon, MON_DATA_MOVE1 + i, NULL);
        m->pp[i] = GetMonData(mon, MON_DATA_PP1 + i, NULL);
    }
    m->level = GetMonData(mon, MON_DATA_LEVEL, NULL);
    m->types[0] = m->types[1] = 0xFF;
    m->egg = GetMonData(mon, MON_DATA_IS_EGG, NULL) ? 1 : 0;
}

/* The party; order (a battler's gBattleStruct->unk1606C) lists it in the
 * battle party menu's order: menu slot i shows field slot nybble i
 * (battle_party_menu.c:236 sub_8094C20). */
static void e2e_party(np_e2e_block *b, const u8 *order) {
    int i;

    b->nparty = gPlayerPartyCount <= PARTY_SIZE ? gPlayerPartyCount : PARTY_SIZE;
    for (i = 0; i < NP_E2E_MAX_PARTY; i++) {
        if (i < (int)b->nparty) {
            int slot = order ? (i & 1 ? order[i / 2] & 0xF : order[i / 2] >> 4) : i;

            e2e_mon(&b->party[i], &gPlayerParty[slot < PARTY_SIZE ? slot : i]);
        } else memset(&b->party[i], 0, sizeof b->party[i]);
    }
}

/* The battle report: every battler's battle copy, the party as the party
 * menu lists it for menu_battler (header comment: the party order). */
static void e2e_battle(np_e2e_block *b, unsigned menu_battler) {
    int i, j;

    for (i = 0; i < NP_E2E_MAX_BATTLERS; i++) {
        np_e2e_mon *m = &b->battlers[i];
        const struct BattlePokemon *bm = &gBattleMons[i];

        memset(m, 0, sizeof *m);
        if (i >= gBattlersCount || !bm->species) continue;
        m->species = bm->species;
        m->hp = bm->hp;
        m->max_hp = bm->maxHP;
        for (j = 0; j < MAX_MON_MOVES; j++) {
            m->moves[j] = bm->moves[j];
            m->pp[j] = bm->pp[j];
        }
        m->disabled_move = gDisableStructs[i].disabledMove;
        m->level = bm->level;
        m->types[0] = bm->type1; /* pokemon.h:232 type1/type2, Emerald's types[2] */
        m->types[1] = bm->type2;
        m->egg = bm->isEgg;
    }
    e2e_party(b, gBattleStruct->unk1606C[menu_battler]);
    b->menu_battler = menu_battler;
    b->battle_type = gBattleTypeFlags;
    b->battle_frame = b->frame;
}

static int e2e_task_running(u32 func) {
    int i;

    for (i = 0; i < NUM_TASKS; i++)
        if (gTasks[i].isActive && (u32)gTasks[i].func == func) return i;
    return -1;
}

/* A script's menu waiting in the field (script_menu.c Task_HandleMultichoiceInput / Task_HandleYesNoInput, both
 * on menu.c's gMenu: cursorPos at +2, maxCursorPos at +4, struct Menu menu.c:14-25): NP_E2E_UI_FIELD_MENU with
 * the number of entries and the cursor (Menu_GetCursorPos). */
static void e2e_field_menu(np_e2e_block *b) {
    const s8 *menu = (const s8 *)GBA_LOCAL(menu, gMenu);

    if (e2e_task_running(GBA_LOCAL(script_menu, Task_HandleMultichoiceInput)) < 0
        && e2e_task_running(GBA_LOCAL(script_menu, Task_HandleYesNoInput)) < 0)
        return;
    gba_e2e_ui(b, NP_E2E_UI_FIELD_MENU, (unsigned)(menu[4] + 1), Menu_GetCursorPos());
}

static void e2e_battle_ui(np_e2e_block *b) {
    int i;
    const u8 *ip;

    if (gMain.callback2 == CB2_PartyMenuMain) {
        if (ePartyMenu2.menuType != PARTY_MENU_TYPE_BATTLE) return;
        e2e_party(b, NULL);
        gba_e2e_ui(b, NP_E2E_UI_BATTLE_PARTY,
                   e2e_task_running(GBA_LOCAL(battle_party_menu, Task_HandlePopupMenuInput)) >= 0 ? 1 : 0,
                   sub_806CA38(ePartyMenu2.menuHandlerTaskId));
        return;
    }
    i = e2e_task_running(GBA_LOCAL(evolution_scene, Task_EvolutionScene));
    if (i >= 0) {
        /* evolution_scene.c: tState data[0], tLearnMoveState data[8], tData9 data[9];
         * state 21 (learn a move), learn-move state 4 (YES/NO), tData9 5 (to the move select) */
        if (gTasks[i].data[0] == 21 && gTasks[i].data[8] == 4)
            gba_e2e_ui(b, NP_E2E_UI_BATTLE_MENU, gTasks[i].data[9] == 5 ? 14 : 15, gBattleCommunication[1]);
        return;
    }
    if (gMain.callback2 != BattleMainCB2) return;
    for (i = 0; i < gBattlersCount && i < MAX_BATTLERS_COUNT; i++) {
        u32 f = (u32)gBattlerControllerFuncs[i];

        if (GetBattlerSide(i) != B_SIDE_PLAYER) continue;
        if (f == (u32)HandleInputChooseAction) {
            e2e_battle(b, i);
            gba_e2e_ui(b, NP_E2E_UI_BATTLE_MENU, 1, gActionSelectionCursor[i]);
            return;
        }
        if (f == GBA_LOCAL(battle_controller_player, HandleInputChooseMove)) {
            e2e_battle(b, i);
            gba_e2e_ui(b, NP_E2E_UI_BATTLE_MENU, 11, gMoveSelectionCursor[i]);
            return;
        }
        if (f == GBA_LOCAL(battle_controller_player, HandleInputChooseTarget)) {
            e2e_battle(b, i);
            gba_e2e_ui(b, NP_E2E_UI_BATTLE_MENU, 12, gMultiUsePlayerCursor);
            return;
        }
    }
    ip = gBattlescriptCurrInstr;
    if ((u32)ip < GBA_ROM || (u32)ip >= GBA_ROM + 0x2000000) return;
    switch (*ip) {
    case 0x5A: /* yesnoboxlearnmove: "Delete a move to make room?" */
        if (gBattleStruct->atk5A_StateTracker == 1) gba_e2e_ui(b, NP_E2E_UI_BATTLE_MENU, 14, gBattleCommunication[1]);
        break;
    case 0x5B: /* yesnoboxstoplearningmove: "Stop learning?" */
        if (gBattleStruct->atk5A_StateTracker == 1) gba_e2e_ui(b, NP_E2E_UI_BATTLE_MENU, 15, gBattleCommunication[1]);
        break;
    case 0x67: /* yesnobox: a wild battle's "Use next Pokemon?", a trainer battle's "Switch Pokemon?" */
        if (gBattleCommunication[0] == 1)
            gba_e2e_ui(b, NP_E2E_UI_BATTLE_MENU, gBattleTypeFlags & BATTLE_TYPE_TRAINER ? 17 : 16,
                       gBattleCommunication[1]);
        break;
    case 0xF3: /* trygivecaughtmonnick */
        if (gBattleCommunication[0] == 1) gba_e2e_ui(b, NP_E2E_UI_BATTLE_MENU, 13, gBattleCommunication[1]);
        break;
    }
}

static void e2e_fill(np_e2e_block *b) {
    static unsigned still;
    struct ObjectEvent *player;
    int i, field;

    b->flags_addr = (u32)gSaveBlock1.flags;
    b->flags_bytes = sizeof gSaveBlock1.flags;
    b->vars_addr = (u32)gSaveBlock1.vars;
    b->vars_count = VARS_COUNT;
    b->party_addr = (u32)gPlayerParty;
    b->party_count = gPlayerPartyCount;
    b->avatar_flags = gPlayerAvatar.flags;
    field = !gMain.inBattle && gMain.callback2 == (MainCallback)GBA_LOCAL(overworld, CB2_Overworld)
         && gPlayerAvatar.objectEventId < OBJECT_EVENTS_COUNT && gObjectEvents[gPlayerAvatar.objectEventId].active;
    if (!field) {
        gba_e2e_field(b, 0, 0, 0, 0, 0, 0, 0);
        if (gMain.inBattle) e2e_battle_ui(b);
        return;
    }
    player = &gObjectEvents[gPlayerAvatar.objectEventId];
    gba_e2e_field(b, 1, (u32)gSaveBlock1.location.mapGroup << 8 | (u8)gSaveBlock1.location.mapNum,
                  player->currentCoords.x - MAP_OFFSET, player->currentCoords.y - MAP_OFFSET, player->currentElevation,
                  e2e_facing(player->facingDirection), gPlayerAvatar.tileTransitionState);
    /* metatiles a script changes under a standing player (doors, switches):
     * the window is asked again after a second of standing still */
    still = gPlayerAvatar.tileTransitionState == T_NOT_MOVING ? still + 1 : 0;
    if (still >= 60) {
        still = 0;
        gba_e2e_dirty();
    }
    gba_e2e_steps(b, e2e_step, NULL, player->currentElevation);
    gba_e2e_grid(b, e2e_cell, NULL);
    for (i = 0; i < OBJECT_EVENTS_COUNT; i++) {
        const struct ObjectEvent *o = &gObjectEvents[i];

        if (!o->active || i == gPlayerAvatar.objectEventId) continue;
        gba_e2e_object(b, o->currentCoords.x - MAP_OFFSET, o->currentCoords.y - MAP_OFFSET, o->localId, o->graphicsId);
    }
    if (gMapHeader.events)
        for (i = 0; i < gMapHeader.events->warpCount; i++) {
            const struct WarpEvent *w = &gMapHeader.events->warps[i];

            gba_e2e_warp(b, w->x, w->y, (u32)w->mapGroup << 8 | w->mapNum, w->warpId, w->elevation);
        }
    e2e_field_menu(b);
}

void ruby_e2e_frame(void) { gba_e2e_frame(e2e_fill); }
