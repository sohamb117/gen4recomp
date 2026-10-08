/*
 * Emerald's half of the end-to-end probe (games/gba-common/pc/include/
 * gba_e2e.h; the block is core/include/np_e2e.h). Called once per frame
 * from emerald_port.c's status hook, at the frame boundary.
 *
 * Field: the player object (gObjectEvents[gPlayerAvatar.objectEventId]):
 * its tile in map coordinates (currentCoords less MAP_OFFSET, the target
 * tile from a step's first frame), currentElevation, facing; the other
 * active objects; the map's warp events; and the terrain window through the
 * game's own grid queries (fieldmap.c MapGridGetMetatileBehaviorAt,
 * MapGridGetCollisionAt, MapGridGetElevationAt, GetMapBorderIdAt).
 *
 * The step layers are the player's own collision check on foot
 * (field_player_avatar.c CheckForObjectEventCollision ->
 * event_object_movement.c GetCollisionAtCoords) without the object half
 * (the bots plan around objects themselves): a ledge first (ShouldJumpLedge,
 * GetLedgeJumpDirection: two tiles), then the tile's collision, the map's
 * border, the one-way metatile tables IsMetatileDirectionallyImpassable
 * reads (gOppositeDirectionBlockedMetatileFuncs on the tile left,
 * gDirectionBlockedMetatileFuncs on the tile entered), then
 * IsElevationMismatchAt (event_object_movement.c:7673: elevation 0 goes
 * anywhere, tiles at 0 or 15 take any, else they must match), and a
 * surfing player's landing (CanStopSurfing: elevation 3). The elevation a
 * step lands at is ObjectEventUpdateElevation's (:7725): unchanged when
 * either tile is 15, else the new tile's.
 *
 * Battle: the player controller's input loops (battle_controller_player.c
 * HandleInputChooseAction, HandleInputChooseMove, HandleInputChooseTarget,
 * found in gBattlerControllerFuncs), the battle script's YES/NO boxes
 * (battle_script_commands.c Cmd_yesnoboxlearnmove 0x5A,
 * Cmd_yesnoboxstoplearningmove 0x5B, Cmd_yesnobox 0x67,
 * Cmd_trygivecaughtmonnick 0xF3, waiting on input in their state 1), the
 * evolution scene's move-learning YES/NO (evolution_scene.c
 * Task_EvolutionScene, EVOSTATE_REPLACE_MOVE / MVSTATE_HANDLE_YES_NO), and
 * the in-battle party menu (party_menu.c CB2_UpdatePartyMenu with
 * PARTY_MENU_TYPE_IN_BATTLE: Task_HandleChooseMonInput, then
 * Task_HandleSelectionMenuInput for SHIFT / SUMMARY / CANCEL). The menu
 * numbers are the DS ones the harness knows (np_e2e.h enum np_e2e_ui):
 * 1 actions, 11 moves, 12 targets, 13 nickname, 14 "delete a move?",
 * 15 "stop learning?", 16 "use the next Pokemon?" (a wild battle's
 * yesnobox), 17 "switch Pokemon?" (a trainer battle's).
 */
#include "global.h"
#include "battle.h"
#include "battle_anim.h"
#include "battle_main.h"
#include "event_object_movement.h"
#include "fieldmap.h"
#include "main.h"
#include "overworld.h"
#include "party_menu.h"
#include "pokemon.h"
#include "task.h"
#include "constants/battle.h"
#include "constants/party_menu.h"
#include "gba_e2e.h"
#include "gba_port.h"

GBA_LOCAL_DECL(battle_controller_player, HandleInputChooseAction);
GBA_LOCAL_DECL(battle_controller_player, HandleInputChooseMove);
GBA_LOCAL_DECL(battle_controller_player, HandleInputChooseTarget);
GBA_LOCAL_DECL(party_menu, CB2_UpdatePartyMenu);
GBA_LOCAL_DECL(party_menu, Task_HandleSelectionMenuInput);
GBA_LOCAL_DECL(evolution_scene, Task_EvolutionScene);

extern bool8 (*const gOppositeDirectionBlockedMetatileFuncs[])(u8); /* event_object_movement.c:893 */
extern bool8 (*const gDirectionBlockedMetatileFuncs[])(u8);         /* event_object_movement.c:900 */

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

/* event_object_movement.c IsElevationMismatchAt (static) */
static bool8 e2e_elevation_mismatch(u8 elevation, s32 x, s32 y) {
    u8 mapElevation;

    if (elevation == ELEVATION_TRANSITION) return FALSE;
    mapElevation = MapGridGetElevationAt(x, y);
    if (mapElevation == ELEVATION_TRANSITION || mapElevation == ELEVATION_MULTI_LEVEL) return FALSE;
    return mapElevation != elevation;
}

/* event_object_movement.c ObjectEventUpdateElevation */
static int e2e_landing_elevation(int e, s32 fx, s32 fy, s32 tx, s32 ty) {
    u8 cur = MapGridGetElevationAt(tx, ty), prev = MapGridGetElevationAt(fx, fy);

    if (cur == ELEVATION_MULTI_LEVEL || prev == ELEVATION_MULTI_LEVEL) return e;
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
    if (e2e_elevation_mismatch(e, nx, ny)) {
        /* field_player_avatar.c CanStopSurfing: off the water onto elevation 3 */
        if (!surfing || MapGridGetElevationAt(nx, ny) != ELEVATION_DEFAULT) return 0;
        *te = ELEVATION_DEFAULT;
        return 1;
    }
    *te = e2e_landing_elevation(e, fx, fy, nx, ny);
    return 1;
}

static void e2e_mon(np_e2e_mon *m, struct Pokemon *mon) {
    int i;

    memset(m, 0, sizeof *m);
    m->species = GetMonData(mon, MON_DATA_SPECIES);
    if (!m->species) return;
    m->hp = GetMonData(mon, MON_DATA_HP);
    m->max_hp = GetMonData(mon, MON_DATA_MAX_HP);
    for (i = 0; i < MAX_MON_MOVES; i++) {
        m->moves[i] = GetMonData(mon, MON_DATA_MOVE1 + i);
        m->pp[i] = GetMonData(mon, MON_DATA_PP1 + i);
    }
    m->level = GetMonData(mon, MON_DATA_LEVEL);
    m->types[0] = m->types[1] = 0xFF;
    m->egg = GetMonData(mon, MON_DATA_IS_EGG) ? 1 : 0;
}

static void e2e_party(np_e2e_block *b) {
    int i;

    b->nparty = gPlayerPartyCount <= PARTY_SIZE ? gPlayerPartyCount : PARTY_SIZE;
    for (i = 0; i < NP_E2E_MAX_PARTY; i++) {
        if (i < (int)b->nparty) e2e_mon(&b->party[i], &gPlayerParty[i]);
        else memset(&b->party[i], 0, sizeof b->party[i]);
    }
}

/* The battle report: every battler's battle copy, the party as the party
 * menu lists it (gPlayerParty in slot order: the battle swaps the mon it
 * sends out into the slot of the one it replaces). */
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
        m->types[0] = bm->types[0];
        m->types[1] = bm->types[1];
        m->egg = bm->isEgg;
    }
    e2e_party(b);
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

static void e2e_battle_ui(np_e2e_block *b) {
    int i;
    const u8 *ip;

    if (gMain.callback2 == (MainCallback)GBA_LOCAL(party_menu, CB2_UpdatePartyMenu)) {
        if (gPartyMenu.menuType != PARTY_MENU_TYPE_IN_BATTLE) return;
        e2e_party(b);
        gba_e2e_ui(b, NP_E2E_UI_BATTLE_PARTY,
                   e2e_task_running(GBA_LOCAL(party_menu, Task_HandleSelectionMenuInput)) >= 0 ? 1 : 0,
                   (u8)gPartyMenu.slotId);
        return;
    }
    i = e2e_task_running(GBA_LOCAL(evolution_scene, Task_EvolutionScene));
    if (i >= 0) {
        /* evolution_scene.c: tState data[0], tLearnMoveState data[6], tLearnMoveYesState data[7];
         * EVOSTATE_REPLACE_MOVE 22, MVSTATE_HANDLE_YES_NO 4, MVSTATE_SHOW_MOVE_SELECT 5 */
        if (gTasks[i].data[0] == 22 && gTasks[i].data[6] == 4)
            gba_e2e_ui(b, NP_E2E_UI_BATTLE_MENU, gTasks[i].data[7] == 5 ? 14 : 15, gBattleCommunication[1]);
        return;
    }
    if (gMain.callback2 != BattleMainCB2) return;
    for (i = 0; i < gBattlersCount && i < MAX_BATTLERS_COUNT; i++) {
        u32 f = (u32)gBattlerControllerFuncs[i];

        if (GetBattlerSide(i) != B_SIDE_PLAYER) continue;
        if (f == GBA_LOCAL(battle_controller_player, HandleInputChooseAction)) {
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
        if (gBattleScripting.learnMoveState == 1) gba_e2e_ui(b, NP_E2E_UI_BATTLE_MENU, 14, gBattleCommunication[1]);
        break;
    case 0x5B: /* yesnoboxstoplearningmove: "Stop learning?" */
        if (gBattleScripting.learnMoveState == 1) gba_e2e_ui(b, NP_E2E_UI_BATTLE_MENU, 15, gBattleCommunication[1]);
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

    b->flags_addr = gSaveBlock1Ptr ? (u32)gSaveBlock1Ptr->flags : 0;
    b->flags_bytes = sizeof gSaveBlock1Ptr->flags;
    b->vars_addr = gSaveBlock1Ptr ? (u32)gSaveBlock1Ptr->vars : 0;
    b->vars_count = VARS_COUNT;
    b->party_addr = (u32)gPlayerParty;
    b->party_count = gPlayerPartyCount;
    b->avatar_flags = gPlayerAvatar.flags;
    field = !gMain.inBattle && gSaveBlock1Ptr && gMain.callback2 == CB2_Overworld
         && gPlayerAvatar.objectEventId < OBJECT_EVENTS_COUNT && gObjectEvents[gPlayerAvatar.objectEventId].active;
    if (!field) {
        gba_e2e_field(b, 0, 0, 0, 0, 0, 0, 0);
        if (gMain.inBattle) e2e_battle_ui(b);
        return;
    }
    player = &gObjectEvents[gPlayerAvatar.objectEventId];
    gba_e2e_field(b, 1, (u32)gSaveBlock1Ptr->location.mapGroup << 8 | (u8)gSaveBlock1Ptr->location.mapNum,
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
}

void emerald_e2e_frame(void) { gba_e2e_frame(e2e_fill); }
