/*
 * Platinum's half of the runtime's options (pc/include/pc_np_options.h):
 * what needs the field engine.
 *
 *   field_ready  the player is free in the field: the game would open the
 *                start menu on X right now (field_control.c's gate: no
 *                field task, no application over the map, the avatar not
 *                mid-step, a named location).
 *   map_id       the field's current map header.
 *   quick save   NP_OPT_QUICKSAVE_SEQ moving on: the start menu's SAVE
 *                without the menu, through the same FieldSystem_Save the
 *                save script's TrySaveGame runs, and only where that menu
 *                would offer SAVE at all.
 *   camera       survey zoom and perspective tilt for the field renderer.
 *   rules check  PC_NP_RULES_CHECK in the environment: once the player is
 *                first free in the field, every NP_RULE_FIX_BUGS fix is run
 *                through the game's own code with the bit off and on, and
 *                the verdicts go to the log (see pc_np_rules_check).
 *
 * pc_np_frame runs at the frame boundary on the game's main thread (from
 * the frame publisher), the same place the save lab's PC_LAB_SAVE_AT saves
 * from, so the field it reads is between frames rather than mid-task.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pch/global_pch.h"

#include "constants/battle.h"
#include "constants/battle/condition.h"
#include "constants/battle/moves.h"
#include "constants/field/map_load.h"
#include "constants/heap.h"
#include "constants/species.h"
#include "generated/abilities.h"
#include "generated/item_hold_effects.h"
#include "generated/moves.h"
#include "generated/pokemon_types.h"
#include "generated/trainers.h"
#include "struct_defs/battle_system.h"

#include "battle/battle_context.h"
#include "battle/battle_lib.h"

#include "camera.h"
#include "field/field_system.h"
#include "field_battle_data_transfer.h"
#include "field_system.h"
#include "field_task.h"
#include "location.h"
#include "move_table.h"
#include "party.h"
#include "pokemon.h"
#include "overlay005/save_info_window.h"
#include "player_avatar.h"
#include "savedata.h"
#include "start_menu.h"
#include "system_flags.h"
#include "trainer_data.h"
#include "unk_0206B9D8.h"
#include "vars_flags.h"

#include "pc_np_options.h"

#include "map_object.h"
#include "terrain_collision_manager.h"

#include "constants/field/dynamic_map_features.h"
#include "overlay008/gym_features.h"
#include "persisted_map_features.h"
#include "persisted_map_features_init.h"
#include "savedata_misc.h"

extern FieldSystem *pc_lab_field_system(void); /* pc/patches/src/field_system.c.patch */
extern Camera *pc_np_active_camera(void);      /* pc/patches/src/camera.c.patch */

static int field_ready(FieldSystem *fs)
{
    int move;

    if (fs == NULL || fs->processManager == NULL || fs->location == NULL || fs->playerAvatar == NULL) return 0;
    if (fs->processManager->pause || !FieldSystem_IsRunningFieldMap(fs)) return 0;
    if (fs->task != NULL || FieldSystem_HasChildProcess(fs)) return 0;
    move = PlayerAvatar_GetPlayerMoveState(fs->playerAvatar);
    if (move != PLAYER_MOVE_STATE_END && move != PLAYER_MOVE_STATE_NONE) return 0;
    return FieldSystem_IsInValidLocation(fs);
}

/* Where the start menu hides SAVE (start_menu.c, StartMenu_Get*Hidden
 * Options) or would ask before overwriting another adventure's save. */
static int save_allowed(FieldSystem *fs)
{
    VarsFlags *vf = SaveData_GetVarsFlags(fs->saveData);

    if (SystemFlag_CheckSafariGameActive(vf) || SystemFlag_CheckInPalPark(vf)) return 0;
    if (FieldSystem_IsInBattleTowerSalon(fs)) return 0;
    if (fs->mapLoadType == MAP_LOAD_TYPE_COLOSSEUM || fs->mapLoadType == MAP_LOAD_TYPE_UNION) return 0;
    return !SaveData_OverwriteCheck(fs->saveData);
}

/* A request waits this long for the player to be free: walking, the avatar
 * is only between steps for a frame or two (the same moments X opens the
 * menu), and a request made mid-dialogue should still fail promptly. */
#define QUICKSAVE_PATIENCE 60

static unsigned sQsWait;

static void quicksave_done(unsigned result)
{
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
 * PC_NP_RULES_CHECK: the NP_RULE_FIX_BUGS fixes, each run through the
 * patched game code itself with the bit off and then on, so the log shows
 * the cartridge's bug and the fix side by side:
 *
 *   Fire Fang      BattleSystem_CalcEffectiveness, Fire Fang into a Wonder
 *                  Guard Water type: the bug lets it through, the fix
 *                  blocks it; and Shadow Force's charge turn, which the
 *                  bug blocks and the fix lets start.
 *   Rage           the battle controller's pre-move pass on a raging,
 *                  confused battler that picked another move: the bug
 *                  keeps only rage, the fix clears only rage.
 *   form stats     Trainer_Encounter for Beauty Devon, whose second
 *                  Wormadam is the Sandy Cloak: the bug leaves it with the
 *                  Plant Cloak's stats, the fix with its own.
 *
 * The real move table, trainer data and save are used; the battle is a
 * zeroed BattleSystem/BattleContext with only what each path reads filled
 * in. A diagnostic, run once at the first free moment in the field; the
 * option is put back as it was.
 */
extern void PcNp_CheckPreMoveActions(BattleSystem *battleSys, BattleContext *battleCtx); /* battle_controller_player.c.patch */

static int rules_wonder_guard(BattleContext *ctx, int move, int lastTurn)
{
    u32 mask = 0;

    ctx->battleStatusMask = lastTurn ? SYSCTL_LAST_OF_MULTI_TURN : 0;
    BattleSystem_CalcEffectiveness(ctx, move, 0, ABILITY_BLAZE, ABILITY_WONDER_GUARD, HOLD_EFFECT_NONE, TYPE_WATER,
                                   TYPE_WATER, &mask);
    return (mask & MOVE_STATUS_INEFFECTIVE) != 0;
}

static u32 rules_rage(BattleSystem *sys, BattleContext *ctx)
{
    const u32 confused = 1; /* one turn of confusion left */

    memset(ctx->battleMons, 0, sizeof ctx->battleMons);
    ctx->turnStartCheckState = 0;
    ctx->turnStartCheckTemp = 0;
    ctx->battleMons[0].statusVolatile = VOLATILE_CONDITION_RAGE | confused;
    PcNp_CheckPreMoveActions(sys, ctx);
    return ctx->battleMons[0].statusVolatile;
}

static const enum PokemonDataParam kRulesStats[] = {
    MON_DATA_MAX_HP, MON_DATA_ATK, MON_DATA_DEF, MON_DATA_SPEED, MON_DATA_SP_ATK, MON_DATA_SP_DEF
};

/* 1 when Devon's Sandy Cloak Wormadam has its own form's stats, 0 when it
 * has other ones, -1 if the party is not what this check expects. */
static int rules_form_stats(FieldSystem *fs, char *line, size_t cap)
{
    FieldBattleDTO *dto = FieldBattleDTO_New(HEAP_ID_FIELD2, BATTLE_TYPE_TRAINER);
    Pokemon *mon, *ref;
    int result = -1;

    line[0] = 0;
    dto->trainerIDs[1] = TRAINER_BEAUTY_DEVON;
    Trainer_Encounter(dto, fs->saveData, HEAP_ID_FIELD2);
    mon = Party_GetPokemonBySlotIndex(dto->parties[1], 1);
    if (Pokemon_GetValue(mon, MON_DATA_SPECIES, NULL) == SPECIES_WORMADAM && Pokemon_GetValue(mon, MON_DATA_FORM, NULL) == 1) {
        size_t i;

        ref = Pokemon_New(HEAP_ID_FIELD2);
        Pokemon_Copy(mon, ref);
        Pokemon_CalcStats(ref);
        result = 1;
        for (i = 0; i < NELEMS(kRulesStats); i++) {
            const u32 got = Pokemon_GetValue(mon, kRulesStats[i], NULL), want = Pokemon_GetValue(ref, kRulesStats[i], NULL);

            snprintf(line + strlen(line), cap - strlen(line), "%s%lu", i ? "/" : "", (unsigned long)got);
            if (got != want) result = 0;
        }
        Heap_Free(ref);
    }
    FieldBattleDTO_Free(dto);
    return result;
}

static void pc_np_rules_check(FieldSystem *fs)
{
    static int done;
    const unsigned saved = pc_np_opt.rules;
    BattleSystem *sys;
    BattleContext *ctx;
    int ff[2], sf[2], form[2], pass;
    u32 rage[2];
    char stats[2][64];
    unsigned bit;

    if (done || getenv("PC_NP_RULES_CHECK") == NULL) return;
    done = 1;
    sys = calloc(1, sizeof *sys);
    ctx = calloc(1, sizeof *ctx);
    if (sys == NULL || ctx == NULL) {
        fprintf(stderr, "pc-np: rules check: out of memory\n");
        free(sys);
        free(ctx);
        return;
    }
    MoveTable_Load(ctx->aiContext.moveTable);
    sys->maxBattlers = 2;
    for (bit = 0; bit < 2; bit++) {
        pc_np_opt.rules = bit ? (saved | PC_NP_RULE_FIX_BUGS) : (saved & ~PC_NP_RULE_FIX_BUGS);
        ff[bit] = rules_wonder_guard(ctx, MOVE_FIRE_FANG, 0);
        sf[bit] = rules_wonder_guard(ctx, MOVE_SHADOW_FORCE, 0);
        rage[bit] = rules_rage(sys, ctx);
        form[bit] = rules_form_stats(fs, stats[bit], sizeof stats[bit]);
    }
    pc_np_opt.rules = saved;
    free(sys);
    free(ctx);

    fprintf(stderr, "pc-np: rules check: Fire Fang into Wonder Guard blocked: off %d, on %d\n", ff[0], ff[1]);
    fprintf(stderr, "pc-np: rules check: Shadow Force charge turn blocked by Wonder Guard: off %d, on %d\n", sf[0], sf[1]);
    fprintf(stderr, "pc-np: rules check: raging + confused battler after another move, volatile: off %#lx, on %#lx\n",
            (unsigned long)rage[0], (unsigned long)rage[1]);
    fprintf(stderr, "pc-np: rules check: Devon's Sandy Cloak Wormadam HP/Atk/Def/Spe/SpA/SpD: off %s (%s), on %s (%s)\n",
            stats[0], form[0] == 1 ? "its form's" : form[0] == 0 ? "not its form's" : "unexpected party", stats[1],
            form[1] == 1 ? "its form's" : form[1] == 0 ? "not its form's" : "unexpected party");
    pass = ff[0] == 0 && ff[1] == 1 && sf[0] == 1 && sf[1] == 0 && rage[0] == VOLATILE_CONDITION_RAGE && rage[1] == 1
        && form[0] == 0 && form[1] == 1;
    fprintf(stderr, "pc-np: rules check: %s\n", pass ? "PASS (bugs with the bit off, fixed with it on)" : "FAIL");
}

/*
 * The end-to-end probe's field half (pc/src/pc_e2e.c, core/include/
 * np_e2e.h): the player's tile and facing, the other map objects, and,
 * while the player is free, the terrain around them as the movement code
 * sees it (TerrainCollisionManager, the queries PlayerAvatar_CheckCollision
 * makes). Tiles off the loaded map blocks are left unknown.
 */
static unsigned e2e_tile(void *ctx, int x, int z)
{
    FieldSystem *fs = ctx;
    const u8 behavior = TerrainCollisionManager_GetTileBehavior(fs, x, z);

    /* 0xFF: terrain_collision_manager.c's INVALID_TILE_BEHAVIOR, off the loaded blocks */
    if (behavior == 0xFF) return 0;
    return PC_E2E_TILE_KNOWN | behavior | (TerrainCollisionManager_CheckCollision(fs, x, z) ? PC_E2E_TILE_COLLISION : 0);
}

/*
 * The Hearthome Gym rolls its open door per trainer room on entry (MTRNG, gym_features.c:3817-3842) and shows it only
 * as a clue symbol lit near the player; every other door warps back to the entrance. The e2e walk_to_door bot reads
 * the roll from this log line (tests/e2e/README.md). Logged once per roll, only under PC_E2E.
 */
static void e2e_gym_log(FieldSystem *fs)
{
    static int last = -1;
    int door = -1;

    if (PersistedMapFeatures_IsCurrentDynamicMap(fs, DYNAMIC_MAP_FEATURES_HEARTHOME_GYM)) {
        const HearthomeGymPersistedFeatures *f = PersistedMapFeatures_GetBuffer(
            MiscSaveBlock_GetPersistedMapFeatures(FieldSystem_GetSaveData(fs)), DYNAMIC_MAP_FEATURES_HEARTHOME_GYM);
        if (f->initialized) door = f->correctDoorID;
    }
    if (door != last && door >= 0) {
        fprintf(stderr, "pc-e2e: hearthome gym map %u door %d\n", (unsigned)fs->location->mapHeaderID, door);
    }
    last = door;
}

static void e2e_frame(FieldSystem *fs, int ready)
{
    MapObject *player, *obj = NULL;
    int i = 0;

    if (!pc_e2e_on()) return;
    if (fs == NULL || fs->location == NULL || fs->playerAvatar == NULL || fs->mapObjMan == NULL
        || !FieldSystem_IsRunningFieldMap(fs)) {
        pc_e2e_field(0, 0, 0, 0, 0, 0, 0);
        pc_e2e_end_frame();
        return;
    }
    player = PlayerAvatar_GetMapObject(fs->playerAvatar);
    pc_e2e_field(1, (unsigned)fs->location->mapHeaderID, PlayerAvatar_GetXPos(fs->playerAvatar),
                 PlayerAvatar_GetZPos(fs->playerAvatar), player != NULL ? MapObject_GetY(player) : 0,
                 (unsigned)PlayerAvatar_GetFacingDir(fs->playerAvatar),
                 (unsigned)PlayerAvatar_GetPlayerMoveState(fs->playerAvatar));
    if (ready && fs->terrainCollisionMan != NULL) pc_e2e_grid(e2e_tile, fs);
    while (MapObjectMan_FindObjectWithStatus(fs->mapObjMan, &obj, &i, MAP_OBJ_STATUS_0)) {
        if (obj == player) continue;
        pc_e2e_object(MapObject_GetX(obj), MapObject_GetZ(obj), MapObject_GetLocalID(obj), MapObject_GetGraphicsID(obj));
    }
    e2e_gym_log(fs);
    pc_e2e_end_frame();
}

void pc_np_frame(void)
{
    FieldSystem *fs = pc_lab_field_system();
    const int ready = field_ready(fs);

    pc_np_stat.field_ready = (unsigned)ready;
    pc_np_stat.map_id = fs != NULL && fs->location != NULL ? (unsigned)fs->location->mapHeaderID : 0;
    e2e_frame(fs, ready);
    if (ready) pc_np_rules_check(fs);
    if (pc_np_opt.quicksave_seq == pc_np_stat.quicksave_seq) return;
    if (ready) {
        if (!save_allowed(fs)) {
            quicksave_done(PC_NP_QS_REFUSED);
        } else {
            quicksave_done(FieldSystem_Save(fs) ? PC_NP_QS_SAVED : PC_NP_QS_FAILED);
        }
    } else if (++sQsWait > QUICKSAVE_PATIENCE) {
        quicksave_done(PC_NP_QS_REFUSED);
    }
}

/*
 * The field camera. The game's own view matrix is computed first, so its
 * camera state (tracking, history, scripted moves) advances exactly as it
 * always does; then, only while an option is off its default, the view and
 * projection the renderer uses are replaced for this frame by the same
 * camera seen from farther away (zoom) and from a different pitch (tilt),
 * still looking at the game's target. Nothing in the Camera is written, so
 * putting the options back puts back the original picture, and every map
 * load, cutscene or script camera move keeps working on the real values.
 *
 * A farther camera needs a farther far plane or the map clips away before
 * the new edge of the view; a flatter one sees toward the horizon, more so.
 * Beyond the blocks the field has loaded there is nothing to draw: that is
 * the clear colour, the "void" a survey camera shows at the edges.
 */
static Camera *sCamOverride;

#define TILT_UNIT_TO_IDX(t) ((s32)(t) * 65536 / (360 * 16))

void pc_np_camera_begin(struct Camera *fieldCamera)
{
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
    /* Pitch is negative looking down (the default field camera is about
     * -59 degrees); a positive tilt brings it toward the horizon. Kept
     * between 5 and 85 degrees below it. */
    pitch = (s16)cam->angle.x + TILT_UNIT_TO_IDX(tilt);
    if (pitch > -TILT_UNIT_TO_IDX(5 * 16)) pitch = -TILT_UNIT_TO_IDX(5 * 16);
    if (pitch < -TILT_UNIT_TO_IDX(85 * 16)) pitch = -TILT_UNIT_TO_IDX(85 * 16);
    px = (u16)pitch;
    py = cam->angle.y;

    /* camera.c's Camera_AdjustPositionAroundTarget, on a copy. */
    pos.x = FX_Mul(FX_Mul(FX_SinIdx(py), dist), FX_CosIdx(px));
    pos.z = FX_Mul(FX_Mul(FX_CosIdx(py), dist), FX_CosIdx(px));
    pos.y = FX_Mul(FX_SinIdx((u16)-px), dist);
    VEC_Add(&pos, &cam->lookAt.target, &pos);
    NNS_G3dGlbLookAt(&pos, &cam->lookAt.up, &cam->lookAt.target);

    nearClip = cam->perspective.nearClip;
    farClip = cam->perspective.farClip;
    if (zoom < 256) nearClip = (fx32)(((s64)nearClip * zoom) >> 8);
    if (zoom > 256) farClip = (fx32)(((s64)farClip * zoom) >> 8);
    if (tilt > 0) farClip += (fx32)(((s64)farClip * tilt) / (15 * 16)); /* +100% per 15 degrees */
    if (farClip > FX32_CONST(30000)) farClip = FX32_CONST(30000);
    /* MTX_PerspectiveW forms 2 * near * far in fx32, which wraps once near
     * x far passes 2^18 square world units (the game's own 150 x 900 is
     * 135000): a farther far plane needs a nearer near plane, which a
     * camera this far back can afford. */
    {
        const s64 maxNearFar = 250000;
        const s64 nearUnits = (s64)nearClip >> FX32_SHIFT, farUnits = (s64)farClip >> FX32_SHIFT;

        if (nearUnits * farUnits > maxNearFar) nearClip = (fx32)((maxNearFar / farUnits) << FX32_SHIFT);
    }
    if (cam->projection == CAMERA_PROJECTION_PERSPECTIVE) {
        NNS_G3dGlbPerspective(cam->perspective.sinFovY, cam->perspective.cosFovY, cam->perspective.aspectRatio,
                              nearClip, farClip);
    } else {
        /* camera.c's orthographic box is sized by the distance. */
        fx32 top = FX_Mul(FX_Div(cam->perspective.sinFovY, cam->perspective.cosFovY), dist);
        fx32 right = FX_Mul(top, cam->perspective.aspectRatio);

        NNS_G3dGlbOrtho(top, -top, -right, right, nearClip, farClip);
    }
    sCamOverride = cam;
}

void pc_np_camera_end(struct Camera *fieldCamera)
{
    (void)fieldCamera;
    if (sCamOverride == NULL) return;
    /* Back to the game's projection for everything drawn outside the field
     * renderer (and for the next frame's renderer to read). */
    Camera_ComputeProjectionMatrix(sCamOverride->projection, sCamOverride);
    sCamOverride = NULL;
}
