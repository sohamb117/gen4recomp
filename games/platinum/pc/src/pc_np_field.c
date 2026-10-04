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
 *
 * pc_np_frame runs at the frame boundary on the game's main thread (from
 * the frame publisher), the same place the save lab's PC_LAB_SAVE_AT saves
 * from, so the field it reads is between frames rather than mid-task.
 */
#define _GNU_SOURCE
#include <stdio.h>

#include "pch/global_pch.h"

#include "constants/field/map_load.h"

#include "camera.h"
#include "field/field_system.h"
#include "field_system.h"
#include "field_task.h"
#include "location.h"
#include "overlay005/save_info_window.h"
#include "player_avatar.h"
#include "savedata.h"
#include "start_menu.h"
#include "system_flags.h"
#include "unk_0206B9D8.h"
#include "vars_flags.h"

#include "pc_np_options.h"

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

void pc_np_frame(void)
{
    FieldSystem *fs = pc_lab_field_system();
    const int ready = field_ready(fs);

    pc_np_stat.field_ready = (unsigned)ready;
    pc_np_stat.map_id = fs != NULL && fs->location != NULL ? (unsigned)fs->location->mapHeaderID : 0;

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
