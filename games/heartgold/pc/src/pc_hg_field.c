/*
 * HeartGold/SoulSilver's half of the runtime's options (Platinum's
 * pc/include/pc_np_options.h; Platinum's own half is pc/src/pc_np_field.c,
 * D/P's pc/game/pc_dp_field.c): what needs the field engine. Game C
 * (pc/Makefile.wasm compiles pc/src with the game's headers and flags).
 *
 *   field_ready  the player is free in the field: the moment X would open
 *                the start menu (FieldSystem_Control: player movement
 *                allowed, i.e. not paused, the field map running, no field
 *                task; FieldInput_Update: the avatar between steps), with no
 *                application over the map.
 *   map_id       the field's current map (Location.mapId).
 *   quick save   NP_OPT_QUICKSAVE_SEQ moving on: the start menu's SAVE
 *                without the menu, through Field_SaveGameNormal (overlay 1),
 *                the save the save script command runs, and only where the
 *                start menu would offer SAVE at all.
 *   e2e probe    core/include/np_e2e.h through the plain-typed calls of
 *                Platinum's pc/src/pc_e2e.c (below).
 *
 * The FieldSystem is field_system.c's sFieldSysPtr, read through
 * pc_hg_field_system() (pc/patches/src/field_system.c.patch).
 *
 * pc_np_frame runs at the frame boundary on the game's main thread (from
 * the frame publisher in pc_view.c), so the field it reads is between
 * frames rather than mid-task.
 */
#include "global.h"

#include <string.h>

#include "constants/player_avatar.h"

#include "battle/battle.h"
#include "battle/battle_input.h"
#include "battle/battle_system.h"
#include "field_system.h"
#include "map_events_internal.h"
#include "map_matrix.h"
#include "map_object.h"
#include "overlay_01.h"
#include "party.h"
#include "player_avatar.h"
#include "pokemon.h"
#include "save.h"
#include "save_vars_flags.h"
#include "script.h"
#include "start_menu.h"
#include "sys_flags.h"
#include "task.h"
#include "unk_02054648.h"
#include "unk_02066EDC.h"

#include "pc_np_options.h"

/* wasi-libc's stderr, declared here: <stdio.h> clashes with MSL's stdarg.h,
 * which the game's include path puts first. */
struct _IO_FILE;
extern struct _IO_FILE *const stderr;
int fprintf(struct _IO_FILE *restrict stream, const char *restrict format, ...);

extern FieldSystem *pc_hg_field_system(void); /* pc/patches/src/field_system.c.patch */

static int field_ready(FieldSystem *fs) {
    u32 move;

    if (fs == NULL || fs->processManager == NULL || fs->location == NULL || fs->playerAvatar == NULL) return 0;
    if (!FieldSystem_IsPlayerMovementAllowed(fs)) return 0;
    /* FieldProcessManager: parent is the field map's own OverlayManager (FieldMap_*), child an application over
     * it (FieldSystem_LaunchApplication); sub_0203DF8C / sub_0203DFA4 (src/field_system.c:123-129) */
    if (fs->processManager->parent == NULL || fs->processManager->child != NULL) return 0;
    move = PlayerAvatar_GetPlayerMoveState(fs->playerAvatar);
    return move == PLAYER_MOVE_STATE_END || move == PLAYER_MOVE_STATE_NONE;
}

/* Where the start menu would not offer SAVE (start_menu.c StartMenu_Init and
 * its inhibit flags: the Safari Zone, the Bug-Catching Contest, Pal Park,
 * the Battle Tower's partner room; FieldSystem_Control's link rooms and the
 * Battle Tower, map load types 2..4, have menus of their own; a Mystery Zone
 * map opens none) or would ask before overwriting another adventure. */
static int save_allowed(FieldSystem *fs) {
    SaveVarsFlags *vf = Save_VarsFlags_Get(fs->saveData);

    if (fs->mapLoadType >= 2 && fs->mapLoadType <= 4) return 0;
    if (!FieldSystem_MapIsNotMysteryZone(fs)) return 0;
    if (Save_VarsFlags_CheckSafariSysFlag(vf) || Save_VarsFlags_CheckBugContestFlag(vf)
        || Save_VarsFlags_CheckPalParkSysFlag(vf)) {
        return 0;
    }
    if (FieldSystem_MapIsBattleTowerMultiPartnerSelectRoom(fs)) return 0;
    return !Save_FileDoesNotBelongToPlayer(fs->saveData);
}

/* A request waits this long for the player to be free: walking, the avatar
 * is only between steps for a frame or two (the same moments X opens the
 * menu), and a request made mid-dialogue should still fail promptly. */
#define QUICKSAVE_PATIENCE 60

static unsigned sQsWait;

static void quicksave_done(unsigned result) {
    fprintf(stderr, "pc-np: quick save %u: %s (map %u)\n", pc_np_opt.quicksave_seq,
            result == PC_NP_QS_SAVED     ? "saved"
            : result == PC_NP_QS_REFUSED ? "refused, the player is not free in the field"
                                         : "FAILED",
            pc_np_stat.map_id);
    pc_np_stat.quicksave_seq = pc_np_opt.quicksave_seq;
    pc_np_stat.quicksave_result = result;
    sQsWait = 0;
}

/*
 * The probe's field half: the player's tile, height and facing, the other
 * map objects, the map's warps, where the flags, vars and party live, and,
 * while the player is free, the terrain around them through the queries the
 * movement code makes. fs->unk60 is the terrain provider (sub_0205489C:
 * [0] the height at a point, [1] a tile's attribute word): GetMetatileBehavior
 * is its low byte (0xFF off the loaded blocks), sub_020548C0 its collision
 * bit (asm/unk_02054648.s).
 *
 * Only tiles on the map matrix are asked. The normal provider
 * (sub_020547D8 -> ov01_021F654C on the MapLoadManager: the matrix is +0xC4
 * blocks wide and +0xC8 high, 32 tiles a block) GF_ASSERTs on a block index
 * past the matrix, and a column past the right edge reads the next block
 * row's tile. The simple provider (MapLoadMode.useSimpleTerrainCollisions:
 * sub_02054824, fs->mapMatrix and fs->terrainAttributes) has no check at
 * all; its matrix is MapMatrix_GetWidth/Height blocks.
 */
#define FS_LOADER_WORD(fs, off) (*(const u32 *)((const u8 *)(fs)->mapLoadManager + (off)))

static int e2e_on_matrix(FieldSystem *fs, int x, int z) {
    u32 w, h;

    if (x < 0 || z < 0) return 0;
    if (fs->mapLoadMode != NULL && fs->mapLoadMode->useSimpleTerrainCollisions) {
        if (fs->mapMatrix == NULL || fs->terrainAttributes == NULL) return 0;
        w = MapMatrix_GetWidth(fs->mapMatrix);
        h = MapMatrix_GetHeight(fs->mapMatrix);
    } else {
        if (fs->mapLoadManager == NULL) return 0;
        w = FS_LOADER_WORD(fs, 0xC4);
        h = FS_LOADER_WORD(fs, 0xC8);
    }
    return (u32)x < w * 32 && (u32)z < h * 32;
}

static unsigned e2e_tile(void *ctx, int x, int z) {
    FieldSystem *fs = ctx;
    u8 behavior;

    if (!e2e_on_matrix(fs, x, z)) return 0;
    behavior = GetMetatileBehavior(fs, x, z);
    if (behavior == 0xFF) return 0;
    return PC_E2E_TILE_KNOWN | behavior | (sub_020548C0(fs, x, z) ? PC_E2E_TILE_COLLISION : 0);
}

/*
 * The step layers (pc_e2e_steps): one step answered by the terrain half of
 * the player's own collision check. The avatar's collision flags
 * (asm/unk_0205CB48.s, the caller of sub_020549F4 that sets bit 2) ask
 * sub_020549F4 for the step from the avatar's position vector to the next
 * tile: sub_02054954, the height the tile gives an object at the current
 * height (sub_02054940, the provider's [0]: the plate nearest that height,
 * which is how a bridge deck and the path beneath it are told apart)
 * differing by 20 units or more; Gymmick_CheckCollision (the gyms' dynamic
 * floors and walls); else the tile's collision bit. Behaviors that turn a
 * step away (water without Surf, one-way tiles) and other objects are left
 * to the planner, which reads them from the grid. A ledge (JUMP_* 0x38..0x3B,
 * its direction) is jumped: two tiles, onto a tile without collision.
 */
extern fx32 sub_02054940(FieldSystem *fieldSystem, fx32 y, fx32 x, fx32 z, u8 *heightSource);

#define TILE_CENTRE(t) ((fx32)(t) * 16 * FX32_ONE + 8 * FX32_ONE)

static int e2e_step(void *ctx, int x, int z, int y, int dir, int *ty) {
    static const s8 dx[4] = {0, 0, -1, 1}, dz[4] = {-1, 1, 0, 0};
    static const u8 jump[4] = {0x3A, 0x3B, 0x39, 0x38}; /* TILE_BEHAVIOR_JUMP_NORTH, _SOUTH, _WEST, _EAST */
    FieldSystem *fs = ctx;
    const int nx = x + dx[dir], nz = z + dz[dir];
    VecFx32 pos;
    u8 source, behavior;
    u32 vertical;

    if (!e2e_on_matrix(fs, nx, nz)) return 0;
    behavior = GetMetatileBehavior(fs, nx, nz);
    if (behavior == 0xFF) return 0;
    if (behavior == jump[dir]) {
        const int lx = nx + dx[dir], lz = nz + dz[dir];

        if (!e2e_on_matrix(fs, lx, lz) || GetMetatileBehavior(fs, lx, lz) == 0xFF || sub_020548C0(fs, lx, lz)) return 0;
        *ty = sub_02054940(fs, y, TILE_CENTRE(lx), TILE_CENTRE(lz), &source);
        return 2;
    }
    pos.x = TILE_CENTRE(x);
    pos.y = y;
    pos.z = TILE_CENTRE(z);
    vertical = 0;
    if (sub_020549F4(fs, &pos, (u32)nx, (u32)nz, &vertical)) return 0;
    *ty = sub_02054940(fs, y, TILE_CENTRE(nx), TILE_CENTRE(nz), &source);
    return 1;
}

static void e2e_frame(FieldSystem *fs, int ready) {
    LocalMapObject *player, *obj = NULL;
    s32 i = 0;
    u32 n;

    if (!pc_e2e_on()) return;
    if (fs == NULL || fs->processManager == NULL || fs->location == NULL || fs->playerAvatar == NULL
        || fs->mapObjectManager == NULL || fs->saveData == NULL || !fs->runningFieldMap) {
        pc_e2e_field(0, 0, 0, 0, 0, 0, 0);
        pc_e2e_end_frame();
        return;
    }
    player = PlayerAvatar_GetMapObject(fs->playerAvatar);
    pc_e2e_field(1, (unsigned)fs->location->mapId, (int)PlayerAvatar_GetXCoord(fs->playerAvatar),
                 (int)PlayerAvatar_GetZCoord(fs->playerAvatar), player != NULL ? (int)MapObject_GetYCoord(player) : 0,
                 PlayerAvatar_GetFacingDirection(fs->playerAvatar), PlayerAvatar_GetPlayerMoveState(fs->playerAvatar));
    {
        SaveVarsFlags *vf = Save_VarsFlags_Get(fs->saveData);
        Party *party = SaveArray_Party_Get(fs->saveData);
        const s32 state = PlayerAvatar_GetState(fs->playerAvatar);

        /* avatar_flags: bit n for the avatar's PLAYER_STATE_* n (0 walking, 1 cycling, 2 surfing) */
        pc_e2e_v4(state >= 0 && state < 32 ? 1u << state : 0, (unsigned)vf->flags, sizeof vf->flags,
                  (unsigned)vf->vars, NELEMS(vf->vars), (unsigned)party->core.mons,
                  (unsigned)party->core.curCount);
    }
    if (fs->mapEvents != NULL) {
        const MapEvents *ev = fs->mapEvents;

        for (n = 0; n < ev->num_warp_events && ev->warp_events != NULL; n++) {
            const WarpEvent *w = &ev->warp_events[n];

            pc_e2e_warp(w->x, w->z, w->header, w->anchor);
        }
    }
    if (ready && fs->unk60 != NULL) {
        if (player != NULL) {
            const int px = (int)PlayerAvatar_GetXCoord(fs->playerAvatar), pz = (int)PlayerAvatar_GetZCoord(fs->playerAvatar);
            u8 source;
            const fx32 y = sub_02054940(fs, MapObject_GetPositionVectorYCoord(player), TILE_CENTRE(px), TILE_CENTRE(pz),
                                        &source);

            pc_e2e_steps(e2e_step, fs, y);
        }
        pc_e2e_grid(e2e_tile, fs);
    }
    /* the walking Pokemon (FollowMon) is the player's own: it trails the
     * avatar and gives way, so it is no obstacle and not listed */
    while (MapObjectManager_GetNextObjectWithFlagFromIndex(fs->mapObjectManager, &obj, &i, MAPOBJECTFLAG_ACTIVE)) {
        if (obj == player || obj == fs->followMon.mapObject) continue;
        pc_e2e_object((int)MapObject_GetXCoord(obj), (int)MapObject_GetZCoord(obj), MapObject_GetID(obj),
                      MapObject_GetSpriteID(obj));
    }
    pc_e2e_end_frame();
}

/*
 * The probe's battle half, from the battle menu's input check
 * (pc/patches/src/battle/battle_input.c.patch: BattleInput_CheckTouch, run
 * every frame the bottom-screen menu waits): the menu (curMenuId, the
 * BattleMenuID numbering Platinum's config indices share: 1..10 actions, 11
 * moves, 12 targets, 13..17 the two-option prompts), its cursor (raw:
 * menuY << 4 | menuX, 0 while the cursor is hidden), every battler as the
 * battle has it (BattleMon: current HP, PP, types, Disable), and the menu
 * battler's party in its party screen's order (BattleContext.unk_312C, the
 * order the battle's party swaps keep).
 */
static void e2e_mon_from_party(pc_e2e_mon *out, Pokemon *mon) {
    int i;

    out->species = (unsigned short)GetMonData(mon, MON_DATA_SPECIES, NULL);
    out->egg = (unsigned char)GetMonData(mon, MON_DATA_IS_EGG, NULL);
    out->level = (unsigned char)GetMonData(mon, MON_DATA_LEVEL, NULL);
    out->hp = (unsigned short)GetMonData(mon, MON_DATA_HP, NULL);
    out->max_hp = (unsigned short)GetMonData(mon, MON_DATA_MAX_HP, NULL);
    for (i = 0; i < MAX_MON_MOVES; i++) {
        out->moves[i] = (unsigned short)GetMonData(mon, MON_DATA_MOVE1 + i, NULL);
        out->pp[i] = (unsigned char)GetMonData(mon, MON_DATA_MOVE1_PP + i, NULL);
    }
    out->types[0] = out->types[1] = 0xFF;
}

void PcHg_E2eBattleMenu(BattleInput *battleInput) {
    BattleSystem *bs;
    BattleContext *ctx;
    pc_e2e_mon battlers[4], party[PARTY_SIZE];
    int n, i, j, menu, count;

    if (!pc_e2e_on() || battleInput == NULL) return;
    pc_e2e_ui(PC_E2E_UI_BATTLE_MENU, (unsigned)battleInput->curMenuId);
    pc_e2e_cursor(battleInput->menuCursor.enabled
                      ? (unsigned)((battleInput->menuCursor.menuY & 0xF) << 4 | (battleInput->menuCursor.menuX & 0xF))
                      : 0);
    bs = battleInput->battleSystem;
    if (bs == NULL || (ctx = BattleSystem_GetBattleContext(bs)) == NULL) return;
    memset(battlers, 0, sizeof battlers);
    memset(party, 0, sizeof party);
    n = BattleSystem_GetMaxBattlers(bs);
    if (n > 4) n = 4;
    for (i = 0; i < n; i++) {
        const BattleMon *m = &ctx->battleMons[i];

        battlers[i].species = m->species;
        battlers[i].egg = (unsigned char)m->isEgg;
        battlers[i].level = m->level;
        battlers[i].hp = (unsigned short)(m->hp > 0 ? m->hp : 0);
        battlers[i].max_hp = (unsigned short)m->maxHp;
        battlers[i].types[0] = m->type1;
        battlers[i].types[1] = m->type2;
        battlers[i].disabled_move = m->unk88.disabledMove;
        for (j = 0; j < MAX_MON_MOVES; j++) {
            battlers[i].moves[j] = m->moves[j];
            battlers[i].pp[j] = m->movePPCur[j];
        }
    }
    menu = BattleSystem_GetBattlerFromBattlerType(bs, battleInput->battlerType);
    if (menu < 0 || menu >= n) menu = 0;
    count = BattleSystem_GetPartySize(bs, menu);
    if (count > PARTY_SIZE) count = PARTY_SIZE;
    for (i = 0; i < count; i++) {
        Pokemon *mon = BattleSystem_GetPartyMon(bs, menu, ctx->unk_312C[menu][i]);

        if (mon != NULL) e2e_mon_from_party(&party[i], mon);
    }
    pc_e2e_battle((unsigned)menu, BattleSystem_GetBattleType(bs), battlers, (unsigned)n, party, (unsigned)count);
}

/* The battle party screen's two input checks (overlay 8: ov08_0221D438, the
 * six slots, and ov08_0221D4B0, the chosen Pokemon's SHIFT page; Platinum's
 * CheckPartyPokemonScreenButtonPressed / CheckSelectPokemonScreenButtons
 * Pressed, found by the same touch-rect tables ov08_02224F1C / _02224E54)
 * run once per frame while the game waits on them. Their callers reach them
 * through these instead (pc/patches/asm/overlay_08.s.patch), which report
 * to the probe and then run the original. */
extern BOOL ov08_0221D438(void *battleParty);
extern u8 ov08_0221D4B0(void *battleParty);

BOOL PcHg_E2ePartySlots(void *battleParty) {
    if (pc_e2e_on()) pc_e2e_ui(PC_E2E_UI_BATTLE_PARTY, 0);
    return ov08_0221D438(battleParty);
}

u8 PcHg_E2ePartyShift(void *battleParty) {
    if (pc_e2e_on()) pc_e2e_ui(PC_E2E_UI_BATTLE_PARTY, 1);
    return ov08_0221D4B0(battleParty);
}

/*
 * PC_NP_RULES_CHECK: the NP_RULE_FIX_BUGS fix HG/SS have, run through the
 * patched game code itself with the bit off and then on, so the log shows
 * the cartridge's bug and the fix side by side (Platinum's pc_np_field.c
 * pc_np_rules_check, its Rage case):
 *
 *   Rage   the battle controller's before-turn pass
 *          (pc/patches/src/battle/battle_controller_player.c.patch) on a
 *          raging, confused battler that picked another move: the bug
 *          keeps only rage, the fix clears only rage.
 *
 * Platinum's other two have nothing to fix here: HG/SS's multi-turn list
 * already names Shadow Force's effect (BattleCtx_IsIdenticalToCurrentMove),
 * and no HG/SS trainer's Pokemon carries a form (files/poketool/trainer).
 * The battle is a zeroed BattleSystem / BattleContext with only what the
 * pass reads filled in. A diagnostic, run once at the first free moment in
 * the field; the option is put back as it was.
 */
extern char *getenv(const char *name);
extern void PcHg_BeforeTurn(BattleSystem *battleSystem, BattleContext *ctx); /* the patch above */

static u32 rules_rage(BattleSystem *sys, BattleContext *ctx) {
    const u32 confused = 1; /* one turn of confusion left */

    memset(ctx->battleMons, 0, sizeof ctx->battleMons);
    memset(ctx->playerActions, 0, sizeof ctx->playerActions); /* no move selected: not Rage */
    ctx->stateBeforeTurn = 1; /* BT_STATE_RAGE */
    ctx->beforeTurnData = 0;
    ctx->battleMons[0].status2 = STATUS2_RAGE | confused;
    PcHg_BeforeTurn(sys, ctx);
    return ctx->battleMons[0].status2;
}

static void rules_check(void) {
    static int done;
    static BattleSystem sys;
    static BattleContext ctx;
    const unsigned saved = pc_np_opt.rules;
    u32 rage[2];
    unsigned bit;

    if (done || getenv("PC_NP_RULES_CHECK") == NULL) return;
    done = 1;
    sys.maxBattlers = 2;
    for (bit = 0; bit < 2; bit++) {
        pc_np_opt.rules = bit ? (saved | PC_NP_RULE_FIX_BUGS) : (saved & ~PC_NP_RULE_FIX_BUGS);
        rage[bit] = rules_rage(&sys, &ctx);
    }
    pc_np_opt.rules = saved;
    fprintf(stderr, "pc-np: rules check: Rage with another move, confused: status2 off %#lx, on %#lx\n",
            (unsigned long)rage[0], (unsigned long)rage[1]);
    fprintf(stderr, "pc-np: rules check: %s\n",
            rage[0] == STATUS2_RAGE && rage[1] == 1 ? "PASS" : "FAIL");
}

static void np_frame(void) {
    FieldSystem *fs = pc_hg_field_system();
    const int ready = field_ready(fs);

    pc_np_stat.field_ready = (unsigned)ready;
    pc_np_stat.map_id = fs != NULL && fs->location != NULL ? (unsigned)fs->location->mapId : 0;

    e2e_frame(fs, ready);
    if (ready) rules_check();

    if (pc_np_opt.quicksave_seq == pc_np_stat.quicksave_seq) return;
    if (ready) {
        if (!save_allowed(fs)) {
            quicksave_done(PC_NP_QS_REFUSED);
        } else {
            quicksave_done(Field_SaveGameNormal(fs) ? PC_NP_QS_SAVED : PC_NP_QS_FAILED);
        }
    } else if (++sQsWait > QUICKSAVE_PATIENCE) {
        quicksave_done(PC_NP_QS_REFUSED);
    }
}

/*
 * The frame boundary is OS_Halt, which normally runs on the SDK's idle
 * thread, and the recompiled code called from here (the terrain queries,
 * Field_SaveGameNormal) pushes its frames on the running thread's guest
 * stack, armrec_sp: for the idle thread a few hundred bytes with the thread
 * structures below it. The work gets a guest stack of its own, as D/P's
 * pc_dp_on_guest_stack does; a nested frame boundary (the save waiting on
 * another thread) skips rather than share it.
 */
void pc_np_frame(void) {
    extern u32 armrec_sp;
    static u32 stack[0x2000] __attribute__((aligned(8))); /* 32 KiB */
    static int busy;
    u32 saved;

    if (busy) return;
    busy = 1;
    saved = armrec_sp;
    armrec_sp = (u32)(stack + 0x2000);
    np_frame();
    armrec_sp = saved;
    busy = 0;
}

/*
 * NP_STAT_IN_BATTLE (pc_np_stat.in_encounter): set by
 * pc/patches/src/encounter.c.patch from Encounter_New / WildEncounter_New
 * to their _Delete, which every field battle (wild, trainer, scripted)
 * goes through.
 */

/*
 * The field camera (NP_OPT_CAMERA_ZOOM / NP_OPT_CAMERA_TILT): Platinum's
 * pc/src/pc_np_field.c pc_np_camera_begin / _end on HG's Camera, called from
 * the field's 3D draw (src/field/fieldmap.c ov01_021E6220, through
 * pc/patches/src/field/fieldmap.c.patch) once Camera_PushLookAtToNNSGlb has
 * advanced the game's camera and loaded its view. Only while an option is
 * off its default, the view and projection the renderer uses this frame are
 * replaced by the same camera seen from farther away (zoom) and from a
 * different pitch (tilt), still looking at the game's target. Nothing in
 * the Camera is written, so putting the options back puts back the
 * original picture, and every scripted camera move keeps working on the
 * real values. The draw's own depth tweak of the projection then applies to
 * this one; _end puts the game's projection matrix back as it was.
 */
static BOOL sCamOverride;
static MtxFx44 sCamProj;

#define TILT_UNIT_TO_IDX(t) ((s32)(t) * 65536 / (360 * 16))

void pc_np_camera_begin(struct Camera *cam) {
    const unsigned zoom = pc_np_opt.camera_zoom;
    const int tilt = pc_np_opt.camera_tilt;
    fx32 dist, nearClip, farClip;
    s32 pitch;
    u16 px, py;
    VecFx32 pos;

    sCamOverride = FALSE;
    if (cam == NULL || (zoom == 256 && tilt == 0)) return;

    dist = (fx32)(((s64)cam->distance * zoom) >> 8);
    /* Pitch is negative looking down; a positive tilt brings it toward the
     * horizon. Kept between 5 and 85 degrees below it. */
    pitch = (s16)cam->angle.x + TILT_UNIT_TO_IDX(tilt);
    if (pitch > -TILT_UNIT_TO_IDX(5 * 16)) pitch = -TILT_UNIT_TO_IDX(5 * 16);
    if (pitch < -TILT_UNIT_TO_IDX(85 * 16)) pitch = -TILT_UNIT_TO_IDX(85 * 16);
    px = (u16)pitch;
    py = cam->angle.y;

    /* camera.c's Camera_CalcLookAtPosFromTargetAndAngle, on a copy. */
    pos.x = FX_Mul(FX_Mul(FX_SinIdx(py), dist), FX_CosIdx(px));
    pos.z = FX_Mul(FX_Mul(FX_CosIdx(py), dist), FX_CosIdx(px));
    pos.y = FX_Mul(FX_SinIdx((u16)-px), dist);
    VEC_Add(&pos, &cam->lookAt.camTarget, &pos);

    sCamProj = NNS_G3dGlb.projMtx;
    NNS_G3dGlbLookAt(&pos, &cam->lookAt.camUp, &cam->lookAt.camTarget);

    nearClip = cam->perspective.near;
    farClip = cam->perspective.far;
    if (zoom < 256) nearClip = (fx32)(((s64)nearClip * zoom) >> 8);
    if (zoom > 256) farClip = (fx32)(((s64)farClip * zoom) >> 8);
    if (tilt > 0) farClip += (fx32)(((s64)farClip * tilt) / (15 * 16)); /* +100% per 15 degrees */
    if (farClip > FX32_CONST(30000)) farClip = FX32_CONST(30000);
    /* MTX_PerspectiveW forms 2 * near * far in fx32, which wraps once near
     * x far passes 2^18 square world units (Platinum's note): a farther far
     * plane needs a nearer near plane. */
    {
        const s64 maxNearFar = 250000;
        const s64 nearUnits = (s64)nearClip >> FX32_SHIFT, farUnits = (s64)farClip >> FX32_SHIFT;

        if (nearUnits * farUnits > maxNearFar) nearClip = (fx32)((maxNearFar / farUnits) << FX32_SHIFT);
    }
    /* Camera_ApplyPerspectiveType's two cases, without its side effects
     * (it also sets the perspective type and the depth buffering mode). */
    if (cam->perspectiveType == CAMERA_PERSPECTIVE_TYPE_PERSPECTIVE) {
        NNS_G3dGlbPerspective(cam->perspective.fovySin, cam->perspective.fovyCos, cam->perspective.aspect, nearClip,
                              farClip);
    } else {
        fx32 top = FX_Mul(FX_Div(cam->perspective.fovySin, cam->perspective.fovyCos), dist);
        fx32 right = FX_Mul(top, cam->perspective.aspect);

        NNS_G3dGlbOrtho(top, -top, -right, right, nearClip, farClip);
    }
    sCamOverride = TRUE;
}

void pc_np_camera_end(struct Camera *cam) {
    (void)cam;
    if (!sCamOverride) return;
    /* Back to the game's projection for everything drawn outside the field
     * renderer and for the next frame (as ov01_021E6220 restores its own
     * copy: MI_CpuCopyFast and the two flags). */
    MI_CpuCopyFast((u32 *)&sCamProj, (u32 *)&NNS_G3dGlb.projMtx, sizeof(MtxFx44));
    NNS_G3dGlb.flag &= ~(NNS_G3D_GLB_FLAG_INVPROJ_UPTODATE | NNS_G3D_GLB_FLAG_INVCAMERAPROJ_UPTODATE);
    sCamOverride = FALSE;
}
