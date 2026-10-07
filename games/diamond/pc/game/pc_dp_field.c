/*
 * Diamond/Pearl's half of the runtime's options (Platinum's
 * pc/include/pc_np_options.h; Platinum's own half is pc/src/pc_np_field.c):
 * what needs the field engine. Game-side code (pc/mk/game.mk compiles
 * pc/game/*.c with the game's headers and flags).
 *
 *   field_ready  the player is free in the field: the game would open the
 *                start menu on X right now (the gate of the field input
 *                handler ov05_021D8320 / sub_02035068: no field task, no
 *                application over the map, the avatar not mid-step, a
 *                named location).
 *   map_id       the field's current map (Location.mapId).
 *   quick save   NP_OPT_QUICKSAVE_SEQ moving on: the start menu's SAVE
 *                without the menu, through the same Field_SaveGame the save
 *                script command (ScrCmd_SaveGame) runs, and only where that
 *                menu would offer SAVE at all.
 *   camera       survey zoom and perspective tilt for the field renderer.
 *
 * Most of D's field engine is still assembly, so the facts below are read
 * from it rather than from headers (include/field_system.h names a fraction
 * of the 0xB8-byte FieldSystem, sub_02037400 allocates it):
 *
 *   UNK_021C5A08         the FieldSystem pointer field_system.c keeps
 *                        (Platinum's sFieldSystem; written by sub_020372D4 /
 *                        sub_02037304, arm9/asm/unk_020372D4.s)
 *   fs->unk00            the process manager: [0] the field map's
 *                        OverlayManager, [1] a child application, [2] pause
 *   fs+0x10              the field task (sub_02046420 runs it)
 *   fs+0x64              running-field-map flag (sub_020373AC)
 *   fs->unk6C            map load type: 0 normal, 4 the other start-menu
 *                        variant; 1..3 are link rooms (sub_02037594 picks
 *                        the input handler from it, map 0x146 forced to 0)
 *   PlayerAvatar.unk14   the player move state, 0 none, 1 start, 2 moving,
 *                        3 end (sub_02055A38; Platinum's playerMoveState)
 *
 * The start menu (arm9/asm/unk_02035068.s) hides SAVE in the Safari Zone,
 * in Pal Park and where sub_02060144 says so (sub_02035080's 0x90 / 0x94 /
 * 0x195 masks); the link-room handlers use their own menus without SAVE.
 * Save_FileDoesNotBelongToPlayer is Platinum's SaveData_OverwriteCheck: the
 * menu would ask before overwriting another adventure.
 *
 * pc_np_frame runs at the frame boundary on the game's main thread (from
 * the frame publisher in pc_view.c), so the field it reads is between
 * frames rather than mid-task.
 */
#include <stdio.h>
#include <string.h>

#include "global.h"

#include "constants/pokemon.h"

#include "camera.h"
#include "field_system.h"
#include "fx.h"
#include "map_header.h"
#include "map_object.h"
#include "nitro/NNS_g3d.h"
#include "player_avatar.h"
#include "save.h"
#include "save_vars_flags.h"
#include "unk_0205EC84.h"
#include "unk_0205FA2C.h"

#include "pc_np_options.h"

extern FieldSystem *UNK_021C5A08;                /* arm9/asm/unk_020372D4.s */
extern BOOL Field_SaveGame(FieldSystem *fieldSystem); /* overlay 5 */
extern void sub_020222B4(u32 sortMode, u32 bufferMode); /* G3_RequestSwapBuffers, deferred */
extern Camera *pc_np_active_camera(void);        /* pc/patches/arm9/src/camera.c.patch */
extern void pc_dp_rules_frame(void);             /* pc_dp_rules.c: PC_NP_RULES_CHECK */

#define FS_WORD(fs, off) (*(u32 *)((u8 *)(fs) + (off)))

static FieldSystem *field_system(void) {
    return UNK_021C5A08;
}

/* Also the save lab's gate (pc_dp_lab.c): the recipe is applied, and the
 * minted save written, only when the player is free in the field. */
int pc_dp_field_ready(FieldSystem *fs) {
    u32 *proc;
    u32 move;

    if (fs == NULL || fs->unk00 == NULL || fs->location == NULL || fs->playerAvatar == NULL) return 0;
    proc = fs->unk00;
    if (proc[2] != 0) return 0;                         /* paused */
    if (proc[0] == 0 || FS_WORD(fs, 0x64) == 0) return 0; /* sub_020373AC */
    if (proc[1] != 0) return 0;                         /* sub_020373C4: an application */
    if (FS_WORD(fs, 0x10) != 0) return 0;               /* a field task */
    move = PlayerAvatar_GetUnk14(fs->playerAvatar);
    if (move != 0 && move != 3) return 0;
    return MapHeader_GetMapSec(fs->location->mapId) != 0; /* sub_02035068 */
}

static int save_allowed(FieldSystem *fs) {
    SaveVarsFlags *vf = Save_VarsFlags_Get(fs->saveData);

    if (fs->unk6C != 0 && fs->unk6C != 4 && fs->location->mapId != 0x146) return 0;
    if (Save_VarsFlags_CheckSafariSysFlag(vf) || Save_VarsFlags_CheckPalParkSysFlag(vf)) return 0;
    if (sub_02060144((u32 **)fs)) return 0;
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
 * PC_WARP=MAP:X:Z[:DIR] (test harness; tests/link): at the first moment
 * the player is free in the field, a map change to MAP at tile X,Z facing
 * DIR (0 up, 1 down, 2 left, 3 right), through the same field task
 * (sub_02049274: FieldSystem_CreateTask of the map-change task
 * sub_02049304 with a Location) the game's own warps run, so the new map's
 * people are loaded from its events as on any arrival. Platinum's
 * equivalent is pc_lab's `map` line.
 */
extern void sub_02049274(FieldSystem *fieldSystem, u32 mapId, s32 warpId, u32 x, u32 y, u32 dir);

static void warp_frame(FieldSystem *fs, int ready) {
    extern char *getenv(const char *);
    static int sState; /* 0 unread, 1 pending, 2 done */
    static unsigned sMap, sX, sZ, sDir;

    if (sState == 0) {
        const char *v = getenv("PC_WARP");
        sState = 2;
        if (v != NULL && sscanf(v, "%u:%u:%u:%u", &sMap, &sX, &sZ, &sDir) >= 3 && sDir < 4) sState = 1;
    }
    if (sState != 1 || !ready) return;
    fprintf(stderr, "pc-np: PC_WARP to map %u (%u,%u) dir %u\n", sMap, sX, sZ, sDir);
    sub_02049274(fs, sMap, -1, sX, sZ, sDir);
    sState = 2;
}

/*
 * The end-to-end probe's field half (Platinum's pc/src/pc_np_field.c
 * e2e_frame; pc/src/pc_e2e.c, core/include/np_e2e.h): the player's tile
 * and facing, the other map objects, and, while the player is free, the
 * terrain around them through the queries D's movement code makes:
 * GetMetatileBehavior (0xFF off the loaded blocks; arm9/src/field_move.c
 * declares it the same way) and sub_0204A6B4, the collision bit of the
 * same tile attributes (arm9/asm/unk_0204A498.s; Platinum's
 * TerrainCollisionManager_CheckCollision), both through fs+0x58, the
 * terrain provider, which exists once a map is loaded.
 */
extern u8 GetMetatileBehavior(FieldSystem *fieldSystem, s32 x, s32 z);
extern BOOL sub_0204A6B4(FieldSystem *fieldSystem, s32 x, s32 z);

static unsigned e2e_tile(void *ctx, int x, int z) {
    FieldSystem *fs = ctx;
    const u8 behavior = GetMetatileBehavior(fs, x, z);

    if (behavior == 0xFF) return 0;
    return PC_E2E_TILE_KNOWN | behavior | (sub_0204A6B4(fs, x, z) ? PC_E2E_TILE_COLLISION : 0);
}

static void e2e_frame(FieldSystem *fs, int ready) {
    LocalMapObject *player, *obj = NULL;
    s32 i = 0;

    if (!pc_e2e_on()) return;
    if (fs == NULL || fs->unk00 == NULL || fs->location == NULL || fs->playerAvatar == NULL
        || fs->mapObjectManager == NULL || FS_WORD(fs, 0x64) == 0) {
        pc_e2e_field(0, 0, 0, 0, 0, 0, 0);
        pc_e2e_end_frame();
        return;
    }
    player = fs->playerAvatar->mapObject;
    pc_e2e_field(1, (unsigned)fs->location->mapId, (int)PlayerAvatar_GetXCoord(fs->playerAvatar),
                 (int)PlayerAvatar_GetZCoord(fs->playerAvatar), player != NULL ? (int)MapObject_GetYCoord(player) : 0,
                 PlayerAvatar_GetFacingDirection(fs->playerAvatar), PlayerAvatar_GetUnk14(fs->playerAvatar));
    if (ready && FS_WORD(fs, 0x58) != 0) pc_e2e_grid(e2e_tile, fs);
    while (MapObjectManager_GetNextObjectWithFlagFromIndex(fs->mapObjectManager, &obj, &i, MAPOBJECTFLAG_ACTIVE)) {
        if (obj == player) continue;
        pc_e2e_object((int)MapObject_GetXCoord(obj), (int)MapObject_GetZCoord(obj), MapObject_GetID(obj),
                      MapObject_GetSpriteID(obj));
    }
    pc_e2e_end_frame();
}

/* The battle's menu input (overlay 11's ov11_02258E74, Platinum's
 * BattleSystem_MenuInput) and the battle party screen's two input checks
 * (overlay 9's ov09_0221347C, the six slots, and ov09_022134F4, the chosen
 * Pokemon's SHIFT page; Platinum's CheckPartyPokemonScreenButtonPressed /
 * CheckSelectPokemonScreenButtonsPressed, found by the same touch-rect
 * tables) run once per frame while the game waits on them. Their callers
 * reach them through these instead (pc/patches/arm9/overlays/11/asm/
 * ov11_0224D550.s.patch, 09/asm/overlay_09.s.patch), which report to the
 * probe and then run the original. The menu config index is the s8 at
 * +0x69F of D's battle subscreen, the index ov11_02258E74 asserts and
 * scales by the 0x30-byte config table ov11_0225FAAC. */
extern int ov11_02258E74(void *subscreen);
extern BOOL ov09_0221347C(void *battleParty);
extern int ov09_022134F4(void *battleParty);

/* The probe's battle report (pc_np_options.h pc_e2e_battle), D's half of
 * Platinum's pc_pl_e2e_battle, through overlay 11's BattleSystem getters
 * (ov11_0222FF68.s, in Platinum battle_system.c's order: GetBattleType
 * 0x2C, GetBattleContext 0x30, GetMaxBattlers 0x44, GetPartyCount,
 * GetPartyPokemon; GetBattlerType is ov11_02230260) and the BattleContext
 * layout ov11_02242B78 (Platinum's BattleSystem_InitBattleMon) writes:
 * battleMons[] 0xC0 bytes each at 0x2D40, species +0, moves +0xC, types
 * +0x24, the IV word with isEgg (bit 30) +0x14, ppCur +0x2C, level +0x34,
 * curHP +0x4C, maxHP +0x50; partyOrder[battler][6] at 0x312C (as Platinum).
 * D's Disable slot is not located: disabled_move stays 0 and the bot learns
 * a refused move from the menu coming back. The subscreen holds the
 * BattleSystem at +0 and the menu's battler type at +0x69E (the byte before
 * the config index, as Platinum's battlerType / activeMenuConfigIndex). */
extern u32 ov11_0222FF74(void *battleSys);                      /* BattleSystem_GetBattleType */
extern u8 *ov11_0222FF78(void *battleSys);                      /* BattleSystem_GetBattleContext */
extern int ov11_0222FF84(void *battleSys);                      /* BattleSystem_GetMaxBattlers */
extern int ov11_0222FFC8(void *battleSys, int battler);         /* BattleSystem_GetPartyCount */
extern void *ov11_02230014(void *battleSys, int battler, int slot); /* BattleSystem_GetPartyPokemon */
extern u8 ov11_02230260(void *battleSys, int battler);          /* BattleSystem_GetBattlerType */
extern u32 GetMonData(void *mon, int attr, void *ptr);

#define DP_CTX_BATTLE_MONS 0x2D40
#define DP_BATTLE_MON_SIZE 0xC0
#define DP_CTX_PARTY_ORDER 0x312C

static void e2e_battle(void *subscreen) {
    void *bs = *(void **)subscreen;
    const unsigned menuType = *((u8 *)subscreen + 0x69E);
    pc_e2e_mon battlers[4], party[6];
    u8 *ctx;
    int n, i, j, menu = 0, count;

    if (bs == NULL || (ctx = ov11_0222FF78(bs)) == NULL) return;
    memset(battlers, 0, sizeof battlers);
    memset(party, 0, sizeof party);
    n = ov11_0222FF84(bs);
    if (n > 4) n = 4;
    for (i = 0; i < n; i++) {
        const u8 *m = ctx + DP_CTX_BATTLE_MONS + i * DP_BATTLE_MON_SIZE;
        const s32 hp = *(const s32 *)(m + 0x4C);

        if (ov11_02230260(bs, i) == menuType) menu = i;
        battlers[i].species = *(const u16 *)m;
        battlers[i].egg = (unsigned char)(*(const u32 *)(m + 0x14) >> 30 & 1);
        battlers[i].level = m[0x34];
        battlers[i].hp = (unsigned short)(hp > 0 ? hp : 0);
        battlers[i].max_hp = (unsigned short)*(const u32 *)(m + 0x50);
        battlers[i].types[0] = m[0x24];
        battlers[i].types[1] = m[0x25];
        for (j = 0; j < 4; j++) {
            battlers[i].moves[j] = *(const u16 *)(m + 0xC + 2 * j);
            battlers[i].pp[j] = m[0x2C + j];
        }
    }
    count = ov11_0222FFC8(bs, menu);
    if (count > 6) count = 6;
    for (i = 0; i < count; i++) {
        void *mon = ov11_02230014(bs, menu, ctx[DP_CTX_PARTY_ORDER + menu * 6 + i]);

        if (mon == NULL) continue;
        party[i].species = (unsigned short)GetMonData(mon, MON_DATA_SPECIES, NULL);
        party[i].egg = (unsigned char)GetMonData(mon, MON_DATA_IS_EGG, NULL);
        party[i].level = (unsigned char)GetMonData(mon, MON_DATA_LEVEL, NULL);
        party[i].hp = (unsigned short)GetMonData(mon, MON_DATA_HP, NULL);
        party[i].max_hp = (unsigned short)GetMonData(mon, MON_DATA_MAX_HP, NULL);
        for (j = 0; j < 4; j++) {
            party[i].moves[j] = (unsigned short)GetMonData(mon, MON_DATA_MOVE1 + j, NULL);
            party[i].pp[j] = (unsigned char)GetMonData(mon, MON_DATA_MOVE1_PP + j, NULL);
        }
        party[i].types[0] = party[i].types[1] = 0xFF;
    }
    pc_e2e_battle((unsigned)menu, ov11_0222FF74(bs), battlers, (unsigned)n, party, (unsigned)count);
}

int PcDp_E2eMenuInput(void *subscreen) {
    if (pc_e2e_on()) {
        pc_e2e_ui(PC_E2E_UI_BATTLE_MENU, (unsigned)*((s8 *)subscreen + 0x69F));
        e2e_battle(subscreen);
    }
    return ov11_02258E74(subscreen);
}

BOOL PcDp_E2ePartySlots(void *battleParty) {
    if (pc_e2e_on()) pc_e2e_ui(PC_E2E_UI_BATTLE_PARTY, 0);
    return ov09_0221347C(battleParty);
}

int PcDp_E2ePartyShift(void *battleParty) {
    if (pc_e2e_on()) pc_e2e_ui(PC_E2E_UI_BATTLE_PARTY, 1);
    return ov09_022134F4(battleParty);
}

static void np_frame(void) {
    FieldSystem *fs = field_system();
    const int ready = pc_dp_field_ready(fs);

    pc_np_stat.field_ready = (unsigned)ready;
    pc_np_stat.map_id = fs != NULL && fs->location != NULL ? (unsigned)fs->location->mapId : 0;

    e2e_frame(fs, ready);
    pc_dp_rules_frame();
    warp_frame(fs, ready);

    if (pc_np_opt.quicksave_seq == pc_np_stat.quicksave_seq) return;
    if (ready) {
        if (!save_allowed(fs)) {
            quicksave_done(PC_NP_QS_REFUSED);
        } else {
            quicksave_done(Field_SaveGame(fs) ? PC_NP_QS_SAVED : PC_NP_QS_FAILED);
        }
    } else if (++sQsWait > QUICKSAVE_PATIENCE) {
        quicksave_done(PC_NP_QS_REFUSED);
    }
}

/*
 * The frame boundary is OS_Halt, which normally runs on the SDK's idle
 * thread, and the recompiled code called from here (Field_SaveGame, the
 * rules check's battle code) pushes its frames on the running thread's
 * guest stack, armrec_sp: for the idle thread that is OSi_IdleThreadStack,
 * 200 bytes, with the thread structures below it. A quick save overflowed
 * it and the next OS_Halt faulted on a wild pointer. The work gets a guest
 * stack of its own, as pc_agb_slot.c's selftest does; a nested frame
 * boundary (the save waiting on another thread) skips rather than share it.
 * The save lab (pc_dp_lab.c) runs its recipe and save through the same.
 */
int pc_dp_on_guest_stack(void (*fn)(void)) {
    extern u32 armrec_sp;
    static u32 stack[0x2000] __attribute__((aligned(8))); /* 32 KiB */
    static int busy;
    u32 saved;

    if (busy) return 0;
    busy = 1;
    saved = armrec_sp;
    armrec_sp = (u32)(uintptr_t)(stack + 0x2000);
    fn();
    armrec_sp = saved;
    busy = 0;
    return 1;
}

void pc_np_frame(void) {
    pc_dp_on_guest_stack(np_frame);
}

/*
 * The field camera, Platinum's rule (pc/src/pc_np_field.c) on D's Camera.
 * The game's own view matrix is computed first, so its camera state
 * (tracking, history, scripted moves) advances exactly as it always does;
 * then, only while an option is off its default, the view and projection
 * the renderer uses are replaced for this frame by the same camera seen
 * from farther away (zoom) and from a different pitch (tilt), still looking
 * at the game's target. Nothing in the Camera is written, so putting the
 * options back puts back the original picture.
 *
 * The hook points are the field renderer's own calls, ov05_021D7BE0
 * (arm9/overlays/05/asm/ov05_021D74E0.s, Platinum's ov5_021D15F4): its
 * `bl Camera_PushLookAtToNNSGlb` and its closing `bl sub_020222B4` (the
 * swap request) go to PcDp_FieldPushLookAt / PcDp_FieldSwapBuffers below
 * through pc/patches/arm9/overlays/05/asm/ov05_021D74E0.s.patch, a
 * size-neutral replacement of the two `bl` targets.
 */
static Camera *sCamOverride;

#define TILT_UNIT_TO_IDX(t) ((s32)(t) * 65536 / (360 * 16))

void pc_np_camera_begin(struct Camera *fieldCamera) {
    Camera *cam = pc_np_active_camera();
    const unsigned zoom = pc_np_opt.camera_zoom;
    const int tilt = pc_np_opt.camera_tilt;
    fx32 dist, nearClip, farClip;
    s32 pitch;
    u16 px, py;
    VecFx32 pos;

    (void)fieldCamera;
    sCamOverride = NULL;
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
    NNS_G3dGlbLookAt(&pos, &cam->lookAt.camUp, &cam->lookAt.camTarget);

    nearClip = cam->perspective.near;
    farClip = cam->perspective.far;
    if (zoom < 256) nearClip = (fx32)(((s64)nearClip * zoom) >> 8);
    if (zoom > 256) farClip = (fx32)(((s64)farClip * zoom) >> 8);
    if (tilt > 0) farClip += (fx32)(((s64)farClip * tilt) / (15 * 16)); /* +100% per 15 degrees */
    if (farClip > FX32_CONST(30000)) farClip = FX32_CONST(30000);
    /* MTX_PerspectiveW forms 2 * near * far in fx32, which wraps once near
     * x far passes 2^18 square world units: a farther far plane needs a
     * nearer near plane, which a camera this far back can afford. */
    {
        const s64 maxNearFar = 250000;
        const s64 nearUnits = (s64)nearClip >> FX32_INT_SHIFT, farUnits = (s64)farClip >> FX32_INT_SHIFT;

        if (farUnits > 0 && nearUnits * farUnits > maxNearFar) nearClip = (fx32)((maxNearFar / farUnits) << FX32_INT_SHIFT);
    }
    if (cam->perspectiveType == CAMERA_PERSPECTIVE_TYPE_PERSPECTIVE) {
        NNS_G3dGlbPerspective(cam->perspective.fovySin, cam->perspective.fovyCos, cam->perspective.aspect, nearClip,
                              farClip);
    } else {
        /* camera.c's orthographic box is sized by the distance. */
        fx32 top = FX_Mul(FX_Div(cam->perspective.fovySin, cam->perspective.fovyCos), dist);
        fx32 right = FX_Mul(top, cam->perspective.aspect);

        NNS_G3dGlbOrtho(top, -top, -right, right, nearClip, farClip);
    }
    sCamOverride = cam;
}

void pc_np_camera_end(struct Camera *fieldCamera) {
    Camera *cam = sCamOverride;

    (void)fieldCamera;
    if (cam == NULL) return;
    sCamOverride = NULL;
    /* Back to the game's projection for everything drawn outside the field
     * renderer (and for the next frame's renderer to read): the projection
     * half of Camera_ApplyPerspectiveType, without its write of
     * gG3dDepthBufferingMode, which the game did not ask for here. */
    if (cam->perspectiveType == CAMERA_PERSPECTIVE_TYPE_PERSPECTIVE) {
        NNS_G3dGlbPerspective(cam->perspective.fovySin, cam->perspective.fovyCos, cam->perspective.aspect,
                              cam->perspective.near, cam->perspective.far);
    } else {
        fx32 y = FX_Mul(FX_Div(cam->perspective.fovySin, cam->perspective.fovyCos), cam->distance);
        fx32 x = FX_Mul(y, cam->perspective.aspect);

        NNS_G3dGlbOrtho(y, -y, -x, x, cam->perspective.near, cam->perspective.far);
    }
}

/* The field renderer's two calls (see above). */
void PcDp_FieldPushLookAt(void) {
    FieldSystem *fs = field_system();

    Camera_PushLookAtToNNSGlb();
    pc_np_camera_begin(fs != NULL ? fs->camera : NULL);
}

void PcDp_FieldSwapBuffers(u32 sortMode, u32 bufferMode) {
    FieldSystem *fs = field_system();

    sub_020222B4(sortMode, bufferMode);
    pc_np_camera_end(fs != NULL ? fs->camera : NULL);
}

/*
 * NP_STAT_IN_BATTLE (pc_np_stat.in_encounter / in_battle_app; Platinum's
 * src/encounter.c.patch and src/unk_0203D1B8.c.patch). in_encounter is set
 * by pc/patches/arm9/src/encounter.c.patch from Encounter_New /
 * WildEncounter_New to their _Delete, which every field battle (wild,
 * trainer, link, safari, Pal Park) goes through. in_battle_app covers the
 * battle application itself (all a Battle Tower battle has): its template
 * UNK_020F2D94 (arm9/asm/unk_020377F0.s) names sub_020377F0 / sub_02037808,
 * two `return TRUE` bodies, as init and exit; pc/patches/arm9/asm/
 * unk_020377F0.s.patch points those two words here instead.
 */
BOOL PcDp_BattleAppInit(void *overlayManager, u32 *status) {
    (void)overlayManager;
    (void)status;
    pc_np_stat.in_battle_app = 1;
    return TRUE;
}

BOOL PcDp_BattleAppExit(void *overlayManager, u32 *status) {
    (void)overlayManager;
    (void)status;
    pc_np_stat.in_battle_app = 0;
    return TRUE;
}
